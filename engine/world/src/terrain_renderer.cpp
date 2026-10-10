#include "foh/world/terrain_renderer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <unordered_map>

#include "foh/log.h"
#include "foh/renderer/components.h"

namespace foh {

namespace {

constexpr u32 kN = kTerrainTileCells + 1; // vertices per node side

u64 PackNode(const TerrainNodeKey& k) {
    return (u64(static_cast<u32>(k.level)) << 56) | (u64(static_cast<u32>(k.x) & 0xFFFFFFFu) << 28) | u64(static_cast<u32>(k.z) & 0xFFFFFFFu);
}

AssetId NodeAsset(u64 id) { return AssetId{0x7E44A1A0ull, id + 1}; }

bool Overlaps(const TerrainNodeKey& a, const TerrainNodeKey& b) {
    const Vec2 oa = a.origin(), ob = b.origin();
    const f32 sa = a.size(), sb = b.size();
    return oa.x < ob.x + sb && ob.x < oa.x + sa && oa.y < ob.y + sb && ob.y < oa.y + sa;
}

} // namespace

void BuildTerrainNodeMesh(const Terrain& terrain, const TerrainNodeKey& node, TerrainTileMesh& out) {
    std::vector<f32> H;
    terrain.sampleNodeHeights(node, H);
    constexpr u32 G = Terrain::kNodeGrid;
    const f32 cell = node.cell();
    const Vec2 o = node.origin();

    out.positions.clear();
    out.normals.clear();
    out.uvs.clear();
    out.indices.clear();
    out.positions.reserve(size_t(kN) * kN + 4 * kN);
    out.normals.reserve(size_t(kN) * kN + 4 * kN);
    out.uvs.reserve(size_t(kN) * kN + 4 * kN);
    for (u32 b = 0; b < kN; ++b)
        for (u32 a = 0; a < kN; ++a) {
            const f32 hl = H[size_t(b + 1) * G + a], hr = H[size_t(b + 1) * G + a + 2];
            const f32 hu = H[size_t(b) * G + a + 1], hd = H[size_t(b + 2) * G + a + 1];
            out.positions.push_back({f32(a) * cell, H[size_t(b + 1) * G + a + 1], f32(b) * cell});
            out.normals.push_back(glm::normalize(Vec3(hl - hr, 2.0f * cell, hu - hd)));
            out.uvs.push_back({(o.x + f32(a) * cell) * 0.25f, (o.y + f32(b) * cell) * 0.25f});
        }
    out.indices.reserve(size_t(kN - 1) * (kN - 1) * 6 + 4 * (kN - 1) * 6);
    for (u32 b = 0; b + 1 < kN; ++b)
        for (u32 a = 0; a + 1 < kN; ++a) {
            const u32 v00 = b * kN + a, v10 = v00 + 1, v01 = v00 + kN, v11 = v01 + 1;
            // Counter-clockwise seen from above, like every mesh the renderer draws. The diagonal alternates so
            // slopes do not show a directional bias.
            if (((a + b) & 1u) == 0) {
                for (u32 v : {v00, v01, v10, v10, v01, v11}) out.indices.push_back(v);
            } else {
                for (u32 v : {v00, v01, v11, v00, v11, v10}) out.indices.push_back(v);
            }
        }

    // Skirts: a lowered copy of every border vertex, so the crack between two LOD levels shows ground, not sky.
    const f32 depth = std::max(2.0f, cell * 3.0f);
    auto skirt = [&](auto indexOf, Vec3 outward) {
        const u32 base = static_cast<u32>(out.positions.size());
        for (u32 k = 0; k < kN; ++k) {
            const u32 top = indexOf(k);
            out.positions.push_back(out.positions[top] - Vec3(0.0f, depth, 0.0f));
            out.normals.push_back(out.normals[top]);
            out.uvs.push_back(out.uvs[top]);
        }
        for (u32 k = 0; k + 1 < kN; ++k) {
            const u32 a = indexOf(k), b = indexOf(k + 1), la = base + k, lb = base + k + 1;
            const Vec3 n = glm::cross(out.positions[la] - out.positions[a], out.positions[b] - out.positions[a]);
            if (glm::dot(n, outward) >= 0.0f) {
                for (u32 v : {a, la, b, b, la, lb}) out.indices.push_back(v);
            } else {
                for (u32 v : {a, b, la, b, lb, la}) out.indices.push_back(v);
            }
        }
    };
    skirt([](u32 k) { return k; }, Vec3(0, 0, -1));                       // north (b = 0)
    skirt([](u32 k) { return (kN - 1) * kN + k; }, Vec3(0, 0, 1));         // south
    skirt([](u32 k) { return k * kN; }, Vec3(-1, 0, 0));                  // west (a = 0)
    skirt([](u32 k) { return k * kN + (kN - 1); }, Vec3(1, 0, 0));         // east
}

// ---- streamer ------------------------------------------------------------------------------------------------------

struct TerrainRenderer::Impl {
    struct Node {
        TerrainNodeKey key;
        u64 id = 0;
        bool resident = false; // mesh uploaded
        bool visible = false;  // entity spawned
        bool stale = true;     // wants a (re)build
        bool desired = false;  // a leaf of this frame's selection
        Entity entity{};
        u32 triangles = 0;
    };
    struct Result {
        u64 id;
        u64 generation;
        TerrainTileMesh mesh;
    };
    struct Task {
        Impl* impl;
        const Terrain* terrain;
        TerrainNodeKey key;
        u64 id;
        u64 generation;
    };

    Impl(Renderer& r, World& w, JobSystem* j) : renderer(r), world(w), jobs(j) {}

    Renderer& renderer;
    World& world;
    JobSystem* jobs;
    std::unordered_map<u64, Node> nodes;
    std::mutex mutex;
    std::vector<Result> done;
    JobCounter counter;
    u32 inFlight = 0;
    u64 generation = 0;
    u64 dirtyCursor = 0;
    AssetId material;
    Vec4 tint{0.36f, 0.34f, 0.27f, 1.0f};

    // water bed: a 1024^2 window of the terrain's height (4 m texels) that follows the focus. It is rebuilt on a job (a
    // million height queries must never run on the frame), then patched in place where sculpting happens.
    static constexpr u32 kBedN = 1024;
    static constexpr f32 kBedSize = 4096.0f, kBedSnap = 1024.0f;
    struct BedJob {
        Impl* impl = nullptr;
        const Terrain* terrain = nullptr;
        u64 generation = 0, cursor = 0;
        f32 originX = 0.0f, originZ = 0.0f;
        std::vector<f32> heights;
        std::atomic<bool> done{false};
    };
    std::shared_ptr<BedJob> bedJob;
    std::vector<f32> bed;
    f32 bedX = 0.0f, bedZ = 0.0f;
    u64 bedGeneration = 0, bedCursor = 0;
    bool haveBed = false, bedDirty = false;
    u32 framesSinceBedUpload = 0;

    static void runTask(void* arg) {
        std::unique_ptr<Task> t(static_cast<Task*>(arg));
        Result r;
        r.id = t->id;
        r.generation = t->generation;
        BuildTerrainNodeMesh(*t->terrain, t->key, r.mesh);
        std::lock_guard lock(t->impl->mutex);
        t->impl->done.push_back(std::move(r));
    }

    void collect(const Terrain& terrain, Vec3 focus, const TerrainNodeKey& key, f32 split, f32 maxDist, std::vector<TerrainNodeKey>& out) const {
        const Vec2 o = key.origin();
        const f32 size = key.size();
        const f32 dx = std::max({o.x - focus.x, 0.0f, focus.x - (o.x + size)});
        const f32 dz = std::max({o.y - focus.z, 0.0f, focus.z - (o.y + size)});
        const f32 ground = terrain.baseHeight(o.x + size * 0.5f, o.y + size * 0.5f);
        const f32 dy = std::max(0.0f, focus.y - ground);
        const f32 dist = std::sqrt(dx * dx + dz * dz + dy * dy);
        if (key.level > 0 && dist < size * split) {
            for (i32 cz = 0; cz < 2; ++cz)
                for (i32 cx = 0; cx < 2; ++cx) collect(terrain, focus, {key.level - 1, key.x * 2 + cx, key.z * 2 + cz}, split, maxDist, out);
        } else if (dist <= maxDist) {
            out.push_back(key);
        }
    }

    void upload(Node& n, const TerrainTileMesh& mesh) {
        const MeshIndexRange whole{0, static_cast<u32>(mesh.indices.size())};
        if (!renderer.uploadMeshAsset(NodeAsset(n.id), MeshGeometry{mesh.positions, mesh.normals, mesh.uvs, {}, mesh.indices}, std::span(&whole, 1))) {
            FOH_ERROR("terrain: could not upload node {}", n.id);
            return;
        }
        n.resident = true;
        n.triangles = static_cast<u32>(mesh.indices.size() / 3);
        if (n.visible && world.isAlive(n.entity)) world.markModified<MeshRenderer>(n.entity);
    }

    void activate(Node& n) {
        Transform t;
        const Vec2 o = n.key.origin();
        t.position = Vec3(o.x, 0.0f, o.y);
        MeshRenderer mr;
        mr.color = tint;
        mr.asset = NodeAsset(n.id);
        mr.material = material;
        char label[48];
        std::snprintf(label, sizeof(label), "Terrain L%d %d,%d", n.key.level, n.key.x, n.key.z);
        n.entity = world.spawn(t, mr, Name::Make(label));
        n.visible = true;
    }

    void remove(Node& n) {
        if (n.visible && world.isAlive(n.entity)) world.despawn(n.entity);
        n.visible = false;
        if (n.resident) renderer.releaseMeshAsset(NodeAsset(n.id));
        n.resident = false;
    }

    static void runBedJob(void* arg) {
        std::shared_ptr<BedJob> job = *static_cast<std::shared_ptr<BedJob>*>(arg);
        delete static_cast<std::shared_ptr<BedJob>*>(arg);
        const f32 texel = kBedSize / f32(kBedN);
        job->heights.resize(size_t(kBedN) * kBedN);
        ParallelFor(job->impl->jobs, kBedN, 4, [&](u32 j0, u32 j1) {
            for (u32 j = j0; j < j1; ++j)
                for (u32 i = 0; i < kBedN; ++i)
                    job->heights[size_t(j) * kBedN + i] = job->terrain->height(job->originX + (f32(i) + 0.5f) * texel, job->originZ + (f32(j) + 0.5f) * texel);
        });
        job->done = true;
    }

    void uploadBed() {
        WaterBedDesc desc;
        desc.origin = Vec2(bedX, bedZ);
        desc.size = Vec2(kBedSize);
        desc.width = desc.height = kBedN;
        desc.heights = bed;
        renderer.setWaterBed(desc);
        framesSinceBedUpload = 0;
        bedDirty = false;
    }

    void updateBed(Terrain& terrain, bool enabled, Vec3 focus) {
        if (!enabled) return;
        ++framesSinceBedUpload;
        if (bedJob && bedJob->done) {
            bed = std::move(bedJob->heights);
            bedX = bedJob->originX;
            bedZ = bedJob->originZ;
            bedGeneration = bedJob->generation;
            bedCursor = bedJob->cursor;
            bedJob.reset();
            haveBed = true;
            uploadBed();
        }
        if (bedJob) return;
        // Where the window should be: snapped, so it does not rebuild on every step.
        const f32 wantX = std::floor((focus.x - kBedSize * 0.5f) / kBedSnap) * kBedSnap, wantZ = std::floor((focus.z - kBedSize * 0.5f) / kBedSnap) * kBedSnap;
        if (!haveBed || bedGeneration != terrain.generation() || wantX != bedX || wantZ != bedZ) {
            auto job = std::make_shared<BedJob>();
            job->impl = this;
            job->terrain = &terrain;
            job->generation = terrain.generation();
            job->cursor = terrain.dirtyCursor();
            job->originX = wantX;
            job->originZ = wantZ;
            bedJob = job;
            auto* arg = new std::shared_ptr<BedJob>(job);
            if (jobs) jobs->submit(&runBedJob, arg, counter, JobPriority::Low);
            else runBedJob(arg);
            return;
        }
        // Sculpting: patch the texels under the touched rectangles.
        const f32 texel = kBedSize / f32(kBedN);
        for (const TerrainDirtyRect& r : terrain.dirtyRectsSince(bedCursor)) {
            const i32 i0 = std::max(0, i32(std::floor((r.minX - bedX) / texel))), i1 = std::min(i32(kBedN) - 1, i32(std::ceil((r.maxX - bedX) / texel)));
            const i32 j0 = std::max(0, i32(std::floor((r.minZ - bedZ) / texel))), j1 = std::min(i32(kBedN) - 1, i32(std::ceil((r.maxZ - bedZ) / texel)));
            for (i32 j = j0; j <= j1; ++j)
                for (i32 i = i0; i <= i1; ++i) bed[size_t(j) * kBedN + size_t(i)] = terrain.height(bedX + (f32(i) + 0.5f) * texel, bedZ + (f32(j) + 0.5f) * texel);
            bedDirty = true;
        }
        if (bedDirty && framesSinceBedUpload >= 12) uploadBed(); // a 4 MB upload at most a few times a second
    }
};

TerrainRenderer::TerrainRenderer(Renderer& renderer, World& world, JobSystem* jobs) : m_impl(std::make_unique<Impl>(renderer, world, jobs)) {}

TerrainRenderer::~TerrainRenderer() {
    Impl& im = *m_impl;
    if (im.jobs) im.jobs->wait(im.counter); // builds in flight hold pointers into the terrain and this object
    for (auto& [id, n] : im.nodes) im.remove(n);
    if (feedWater) im.renderer.clearWaterBed();
}

void TerrainRenderer::setMaterial(const AssetId& material, Vec4 tint) {
    Impl& im = *m_impl;
    im.material = material;
    im.tint = tint;
    for (auto& [id, n] : im.nodes) {
        if (!n.visible || !im.world.isAlive(n.entity)) continue;
        if (MeshRenderer* mr = im.world.get<MeshRenderer>(n.entity)) {
            mr->material = material;
            mr->color = tint;
            im.world.markModified<MeshRenderer>(n.entity);
        }
    }
}

void TerrainRenderer::update(Terrain& terrain, Vec3 focus) {
    Impl& im = *m_impl;

    // 1. What changed since last frame.
    const bool everything = terrain.generation() != im.generation;
    im.generation = terrain.generation();
    if (everything) {
        im.dirtyCursor = terrain.dirtyCursor();
        for (auto& [id, n] : im.nodes) n.stale = true;
    } else {
        const std::vector<TerrainDirtyRect> rects = terrain.dirtyRectsSince(im.dirtyCursor);
        if (!rects.empty()) {
            for (auto& [id, n] : im.nodes) {
                const Vec2 o = n.key.origin();
                const f32 s = n.key.size();
                for (const TerrainDirtyRect& r : rects)
                    if (o.x < r.maxX && r.minX < o.x + s && o.y < r.maxZ && r.minZ < o.y + s) {
                        n.stale = true;
                        break;
                    }
            }
        }
    }

    // 2. The wanted leaves.
    std::vector<TerrainNodeKey> leaves;
    {
        const f32 topSize = kTerrainTileSize * f32(1u << maxLevel);
        const i32 x0 = i32(std::floor((focus.x - viewDistance) / topSize)), x1 = i32(std::floor((focus.x + viewDistance) / topSize));
        const i32 z0 = i32(std::floor((focus.z - viewDistance) / topSize)), z1 = i32(std::floor((focus.z + viewDistance) / topSize));
        for (i32 z = z0; z <= z1; ++z)
            for (i32 x = x0; x <= x1; ++x) im.collect(terrain, focus, {i32(maxLevel), x, z}, splitFactor, viewDistance, leaves);
    }
    for (auto& [id, n] : im.nodes) n.desired = false;
    std::vector<Impl::Node*> desired;
    desired.reserve(leaves.size());
    for (const TerrainNodeKey& k : leaves) {
        const u64 id = PackNode(k);
        auto [it, inserted] = im.nodes.try_emplace(id);
        Impl::Node& n = it->second;
        if (inserted) {
            n.key = k;
            n.id = id;
        }
        n.desired = true;
        desired.push_back(&n);
    }

    // 3. Start builds, nearest first.
    auto distanceTo = [&](const Impl::Node* n) {
        const Vec2 o = n->key.origin();
        const f32 s = n->key.size();
        const f32 dx = std::max({o.x - focus.x, 0.0f, focus.x - (o.x + s)}), dz = std::max({o.y - focus.z, 0.0f, focus.z - (o.y + s)});
        return dx * dx + dz * dz;
    };
    {
        std::vector<Impl::Node*> todo;
        for (Impl::Node* n : desired)
            if (n->stale) todo.push_back(n);
        std::sort(todo.begin(), todo.end(), [&](const Impl::Node* a, const Impl::Node* b) { return distanceTo(a) < distanceTo(b); });
        const u32 maxInFlight = im.jobs ? std::max(4u, im.jobs->workerCount() / 2) : 2u;
        for (Impl::Node* n : todo) {
            if (im.inFlight >= maxInFlight) break;
            n->stale = false;
            ++im.inFlight;
            auto* task = new Impl::Task{&im, &terrain, n->key, n->id, terrain.generation()};
            if (im.jobs) {
                im.jobs->submit(&Impl::runTask, task, im.counter, JobPriority::Low);
            } else {
                Impl::runTask(task);
            }
        }
    }

    // 4. Upload finished meshes, a few per frame.
    {
        std::vector<Impl::Result> ready;
        {
            std::lock_guard lock(im.mutex);
            const size_t take = std::min<size_t>(im.done.size(), 6);
            ready.assign(std::make_move_iterator(im.done.begin()), std::make_move_iterator(im.done.begin() + static_cast<std::ptrdiff_t>(take)));
            im.done.erase(im.done.begin(), im.done.begin() + static_cast<std::ptrdiff_t>(take));
        }
        for (Impl::Result& r : ready) {
            --im.inFlight;
            const auto it = im.nodes.find(r.id);
            if (it == im.nodes.end()) continue; // not wanted any more
            Impl::Node& n = it->second;
            im.upload(n, r.mesh);
            if (r.generation != terrain.generation()) n.stale = true; // built from older parameters: again
        }
    }

    // 5. Swap nodes without holes: a visible node that is no longer wanted goes when everything now covering its area
    //    is resident, and its replacements appear in the same frame.
    std::vector<Impl::Node*> activate;
    for (auto& [id, n] : im.nodes) {
        if (!n.visible || n.desired) continue;
        bool ready = true;
        std::vector<Impl::Node*> covering;
        for (Impl::Node* d : desired)
            if (Overlaps(n.key, d->key)) {
                covering.push_back(d);
                if (!d->resident) ready = false;
            }
        if (!ready) continue;
        im.remove(n);
        for (Impl::Node* d : covering) activate.push_back(d);
    }
    for (Impl::Node* d : activate)
        if (d->resident && !d->visible) im.activate(*d);
    for (Impl::Node* d : desired) {
        if (!d->resident || d->visible) continue;
        bool blocked = false;
        for (auto& [id, n] : im.nodes)
            if (n.visible && Overlaps(n.key, d->key)) {
                blocked = true;
                break;
            }
        if (!blocked) im.activate(*d);
    }

    // 6. Forget nodes that are neither wanted nor visible.
    for (auto it = im.nodes.begin(); it != im.nodes.end();) {
        if (!it->second.desired && !it->second.visible) {
            im.remove(it->second);
            it = im.nodes.erase(it);
        } else {
            ++it;
        }
    }

    im.updateBed(terrain, feedWater, focus);

    m_stats = {};
    m_stats.building = im.inFlight;
    for (const auto& [id, n] : im.nodes) {
        ++m_stats.nodes;
        if (n.visible) {
            ++m_stats.visible;
            m_stats.triangles += n.triangles;
            ++m_stats.perLevel[std::min(n.key.level, 15)];
        }
        if (n.desired && !n.resident) ++m_stats.waiting;
    }
}

} // namespace foh
