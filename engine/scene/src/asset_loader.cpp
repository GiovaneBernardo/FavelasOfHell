#include "foh/scene/asset_loader.h"

#include <algorithm>
#include <cstring>

#include "foh/log.h"

namespace foh {

namespace fs = std::filesystem;

static_assert(static_cast<int>(TexelFormat::Rgba8Unorm) == static_cast<int>(TextureFormat::Rgba8Unorm) &&
                  static_cast<int>(TexelFormat::Rgba8Srgb) == static_cast<int>(TextureFormat::Rgba8Srgb) &&
                  static_cast<int>(TexelFormat::Bc7Unorm) == static_cast<int>(TextureFormat::Bc7Unorm) &&
                  static_cast<int>(TexelFormat::Bc7Srgb) == static_cast<int>(TextureFormat::Bc7Srgb) &&
                  static_cast<int>(TexelFormat::Bc5Unorm) == static_cast<int>(TextureFormat::Bc5Unorm) &&
                  static_cast<int>(TexelFormat::Bc4Unorm) == static_cast<int>(TextureFormat::Bc4Unorm),
              "TextureFormat mirrors TexelFormat");

// Everything a worker produces for one asset. The payload member that matches `kind` is filled.
struct AssetLoader::Job {
    AssetId id;
    AssetKind kind = AssetKind::None;
    fs::path file;
    u32 maxTextureSize = 0;
    JobCounter counter;

    bool ok = false;
    std::string error;

    // Mesh
    std::vector<Vec3> positions, normals;
    std::vector<Vec2> uvs;
    std::vector<Vec4> tangents;
    std::vector<u32> indices;
    std::vector<MeshIndexRange> ranges;
    // Texture / material
    TextureUpload texture;
    MaterialParams material;
};

namespace {

// Worker: file -> renderer-ready data. Pure function of the job (reads one file).
void runJob(AssetLoader::Job&);

} // namespace

AssetLoader::AssetLoader(AssetDb& db, Renderer& renderer, JobSystem* jobs) : m_db(db), m_renderer(renderer), m_jobs(jobs) {}

AssetLoader::~AssetLoader() {
    if (m_jobs)
        for (auto& job : m_inflight) m_jobs->wait(job->counter);
}

void AssetLoader::request(AssetId id) {
    if (!id.isValid()) return;
    const AssetInfo* info = m_db.find(id);
    if (!info) return;
    switch (info->kind) {
        case AssetKind::Prefab:
        case AssetKind::PrefabVariant:
            // Everything the prefab references (meshes, materials; for a variant also its base prefab).
            for (const AssetId& dep : m_db.dependencies(id)) request(dep);
            return;
        case AssetKind::Material:
            for (const AssetId& dep : m_db.dependencies(id)) request(dep); // its textures
            [[fallthrough]];
        case AssetKind::Mesh:
        case AssetKind::Texture: {
            const auto it = m_entries.find(id);
            if (it != m_entries.end() && it->second.state != State::Failed) return; // already requested
            m_entries[id] = {State::Queued, info->kind};
            m_queue.push_back(id);
            return;
        }
        case AssetKind::None: return;
    }
}

void AssetLoader::start(const AssetId& id, AssetKind kind) {
    const AssetInfo* info = m_db.find(id);
    if (!info) {
        m_entries.erase(id);
        return;
    }
    auto job = std::make_unique<Job>();
    job->id = id;
    job->kind = kind;
    job->file = info->file;
    job->maxTextureSize = m_maxTextureSize;
    m_entries[id].state = State::Loading;
    Job* raw = job.get();
    m_inflight.push_back(std::move(job));
    if (m_jobs && m_jobs->workerCount() > 0) {
        m_jobs->submit([](void* p) { runJob(*static_cast<Job*>(p)); }, raw, raw->counter, JobPriority::Low);
    } else {
        runJob(*raw); // inline: pump() applies it right away
    }
}

void AssetLoader::apply(Job& job) {
    const auto found = m_entries.find(job.id);
    if (found == m_entries.end()) return; // released while loading
    Entry& entry = found->second;
    if (!job.ok) {
        entry.state = State::Failed;
        FOH_ERROR("cannot load {} {:016x}{:016x}: {}", AssetKindName(job.kind), job.id.hi, job.id.lo, job.error);
        return;
    }
    switch (job.kind) {
        case AssetKind::Mesh:
            if (!m_renderer.uploadMeshAsset(job.id, {job.positions, job.normals, job.uvs, job.tangents, job.indices}, job.ranges)) {
                entry.state = State::Failed;
                FOH_ERROR("the renderer rejected mesh {:016x}{:016x}", job.id.hi, job.id.lo);
                return;
            }
            break;
        case AssetKind::Texture:
            if (!m_renderer.uploadTexture(std::move(job.texture))) {
                entry.state = State::Failed;
                FOH_ERROR("the renderer rejected texture {:016x}{:016x}", job.id.hi, job.id.lo);
                return;
            }
            break;
        case AssetKind::Material: m_renderer.setMaterial(job.id, job.material); break;
        default: break;
    }
    entry.state = State::Loaded;
    if (entry.reloadAfter) {
        entry.reloadAfter = false;
        entry.state = State::Queued;
        m_queue.push_back(job.id);
    }
}

void AssetLoader::pump(f32 budgetMs) {
    const auto begin = std::chrono::steady_clock::now();
    auto overBudget = [&] {
        return std::chrono::duration<f32, std::milli>(std::chrono::steady_clock::now() - begin).count() > budgetMs;
    };

    // Finished loads go to the renderer, oldest first, as long as the frame budget allows.
    for (size_t i = 0; i < m_inflight.size();) {
        Job& job = *m_inflight[i];
        if (job.counter.pending.load(std::memory_order_acquire) != 0) {
            ++i;
            continue;
        }
        apply(job);
        m_inflight.erase(m_inflight.begin() + static_cast<std::ptrdiff_t>(i));
        if (overBudget()) break;
    }

    // Keep the workers busy, but not with more than a handful of big files at once (memory).
    const size_t maxInflight = m_jobs && m_jobs->workerCount() > 0 ? 6 : 1;
    while (!m_queue.empty() && m_inflight.size() < maxInflight) {
        const AssetId id = m_queue.front();
        m_queue.pop_front();
        const auto it = m_entries.find(id);
        if (it == m_entries.end() || it->second.state != State::Queued) continue; // released or reloaded meanwhile
        start(id, it->second.kind);
    }
}

void AssetLoader::release(const AssetId& id, AssetKind kind) {
    switch (kind) {
        case AssetKind::Mesh: m_renderer.releaseMeshAsset(id); break;
        case AssetKind::Texture: m_renderer.releaseTexture(id); break;
        case AssetKind::Material: m_renderer.releaseMaterial(id); break;
        default: break;
    }
}

void AssetLoader::assetChanged(AssetId id) {
    auto it = m_entries.find(id);
    if (it == m_entries.end()) return; // never loaded: nothing to keep current
    const AssetKind kind = it->second.kind;
    const AssetInfo* info = m_db.find(id);
    if (!info) {
        // Removed by the reimport. A load still in flight lands in apply(), finds no entry and is dropped there.
        release(id, kind);
        m_entries.erase(it);
        return;
    }
    if (kind == AssetKind::Material)
        for (const AssetId& dep : m_db.dependencies(id)) request(dep); // a reimport can add textures (may rehash the map)
    Entry& entry = m_entries[id];
    if (entry.state == State::Loading) {
        entry.reloadAfter = true; // the data on its way is already stale
    } else if (entry.state != State::Queued) {
        // Reload under the same id; the renderer swaps in the new data when it arrives (the old stays until then).
        entry.state = State::Queued;
        m_queue.push_back(id);
    }
}

AssetLoaderStats AssetLoader::stats() const {
    AssetLoaderStats s;
    for (const auto& [id, e] : m_entries) {
        switch (e.state) {
            case State::Queued: ++s.queued; break;
            case State::Loading: ++s.loading; break;
            case State::Loaded: ++s.loaded; break;
            case State::Failed: ++s.failed; break;
        }
    }
    return s;
}

namespace {

void runJob(AssetLoader::Job& job) {
    std::vector<u8> bytes;
    if (!ReadFileBytes(job.file, bytes)) {
        job.error = "cannot read " + job.file.string();
        return;
    }
    auto reader = ContainerReader::Open(std::move(bytes), &job.error);
    if (!reader) return;
    if (reader->id() != job.id) {
        job.error = "container id does not match the database (stale cache?)";
        return;
    }

    switch (job.kind) {
        case AssetKind::Mesh: {
            MeshAsset mesh;
            if (!DeserializeMesh(*reader, mesh, &job.error)) return;
            const VertexStream* pos = mesh.findStream(VertexSemantic::Position);
            if (!pos || pos->format != VertexFormat::Float3) {
                job.error = "mesh has no float3 position stream";
                return;
            }
            // Streams are read by semantic: the renderer takes what it understands and ignores the rest.
            const VertexStream* nrm = mesh.findStream(VertexSemantic::Normal);
            const VertexStream* uv = mesh.findStream(VertexSemantic::Uv0);
            const VertexStream* tan = mesh.findStream(VertexSemantic::Tangent);
            const u32 count = mesh.vertexCount();
            job.positions.resize(count);
            if (nrm && nrm->format == VertexFormat::Float3) job.normals.resize(count);
            if (uv && uv->format == VertexFormat::Float2) job.uvs.resize(count);
            if (tan && tan->format == VertexFormat::Float4) job.tangents.resize(count);
            for (u32 i = 0; i < count; ++i) {
                const u8* v = mesh.vertexData.data() + static_cast<size_t>(i) * mesh.vertexStride;
                std::memcpy(&job.positions[i], v + pos->offset, sizeof(Vec3));
                if (!job.normals.empty()) std::memcpy(&job.normals[i], v + nrm->offset, sizeof(Vec3));
                if (!job.uvs.empty()) std::memcpy(&job.uvs[i], v + uv->offset, sizeof(Vec2));
                if (!job.tangents.empty()) std::memcpy(&job.tangents[i], v + tan->offset, sizeof(Vec4));
            }
            job.indices = std::move(mesh.indices);
            for (const SubMesh& s : mesh.subMeshes) job.ranges.push_back({s.firstIndex, s.indexCount});
            job.ok = true;
            return;
        }
        case AssetKind::Texture: {
            TextureInfo info;
            if (!ReadTextureInfo(*reader, info)) {
                job.error = "bad texture metadata";
                return;
            }
            // Skip the largest mips of oversized textures (the file keeps them; this is a residency choice).
            u32 first = 0, w = info.width, h = info.height;
            while (job.maxTextureSize != 0 && std::max(w, h) > job.maxTextureSize && first + 1 < info.mipCount) {
                ++first;
                w = std::max(w / 2, 1u);
                h = std::max(h / 2, 1u);
            }
            TextureAsset tex;
            if (!DeserializeTexture(*reader, tex, first, &job.error)) return;
            job.texture.id = job.id;
            job.texture.format = static_cast<TextureFormat>(tex.format);
            job.texture.width = tex.width;
            job.texture.height = tex.height;
            job.texture.mips.reserve(tex.mips.size());
            for (TextureMip& m : tex.mips) job.texture.mips.push_back(std::move(m.data));
            job.ok = true;
            return;
        }
        case AssetKind::Material: {
            MaterialAsset m;
            if (!DeserializeMaterial(*reader, m, &job.error)) return;
            MaterialParams& p = job.material;
            p.baseColor = m.baseColor;
            p.emissive = m.emissive;
            p.metallic = m.metallic;
            p.roughness = m.roughness;
            p.normalScale = m.normalScale;
            p.occlusionStrength = m.occlusionStrength;
            p.alphaCutoff = m.alphaCutoff;
            // Blended materials are alpha-tested too: there is no transparent pass yet.
            p.alphaMask = m.alphaMode != AlphaMode::Opaque;
            p.doubleSided = m.doubleSided;
            p.baseColorTexture = m.baseColorTexture.texture;
            p.normalTexture = m.normalTexture.texture;
            p.metalRoughTexture = m.metallicRoughnessTexture.texture;
            p.occlusionTexture = m.occlusionTexture.texture;
            p.emissiveTexture = m.emissiveTexture.texture;
            job.ok = true;
            return;
        }
        default: job.error = "not a loadable asset kind"; return;
    }
}

} // namespace

} // namespace foh
