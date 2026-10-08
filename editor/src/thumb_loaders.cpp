#include "thumb_loaders.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

#include <stb_image.h>

#include "foh/asset_container.h"
#include "foh/asset_types.h"
#include "foh/math.h"

namespace foh {

namespace {

constexpr u64 kMaxDecodePixels = 96ull * 1024 * 1024; // refuse to decode absurd images for a thumbnail

// Area-average downscale of RGBA8 into `out`, fitting kThumbSize. Images that already fit are copied.
void fitToThumbnail(const u8* src, u32 w, u32 h, ThumbPixels& out) {
    const float scale = std::min(1.0f, static_cast<float>(kThumbSize) / static_cast<float>(std::max(w, h)));
    const u32 dw = std::max(1u, static_cast<u32>(std::lround(static_cast<float>(w) * scale)));
    const u32 dh = std::max(1u, static_cast<u32>(std::lround(static_cast<float>(h) * scale)));
    out.width = dw;
    out.height = dh;
    out.rgba.resize(static_cast<size_t>(dw) * dh * 4);
    if (dw == w && dh == h) {
        std::copy(src, src + out.rgba.size(), out.rgba.begin());
        return;
    }
    for (u32 y = 0; y < dh; ++y) {
        const u32 y0 = static_cast<u32>(static_cast<u64>(y) * h / dh);
        const u32 y1 = std::max(y0 + 1, static_cast<u32>(static_cast<u64>(y + 1) * h / dh));
        for (u32 x = 0; x < dw; ++x) {
            const u32 x0 = static_cast<u32>(static_cast<u64>(x) * w / dw);
            const u32 x1 = std::max(x0 + 1, static_cast<u32>(static_cast<u64>(x + 1) * w / dw));
            u32 acc[4] = {0, 0, 0, 0};
            for (u32 sy = y0; sy < y1; ++sy) {
                const u8* p = src + (static_cast<size_t>(sy) * w + x0) * 4;
                for (u32 sx = x0; sx < x1; ++sx, p += 4) {
                    acc[0] += p[0];
                    acc[1] += p[1];
                    acc[2] += p[2];
                    acc[3] += p[3];
                }
            }
            const u32 n = (y1 - y0) * (x1 - x0);
            u8* d = out.rgba.data() + (static_cast<size_t>(y) * dw + x) * 4;
            for (int c = 0; c < 4; ++c) d[c] = static_cast<u8>(acc[c] / n);
        }
    }
}

} // namespace

bool DecodeImageThumbnail(const u8* data, size_t size, ThumbPixels& out) {
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(data, static_cast<int>(size), &w, &h, &comp)) return false;
    if (w <= 0 || h <= 0 || static_cast<u64>(w) * static_cast<u64>(h) > kMaxDecodePixels) return false;
    u8* pixels = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &comp, 4);
    if (!pixels) return false;
    fitToThumbnail(pixels, static_cast<u32>(w), static_cast<u32>(h), out);
    stbi_image_free(pixels);
    return true;
}

bool LoadImageThumbnail(const std::filesystem::path& file, ThumbPixels& out) {
    std::vector<u8> bytes;
    if (!ReadFileBytes(file, bytes) || bytes.empty()) return false;
    return DecodeImageThumbnail(bytes.data(), bytes.size(), out);
}

// ---- model rasterizer ----------------------------------------------------------------------------

namespace {

struct Loaded {
    MeshAsset mesh;
    std::vector<Vec3> colors; // per material slot
};

bool readContainer(const std::filesystem::path& file, std::optional<ContainerReader>& reader) {
    std::vector<u8> bytes;
    if (!ReadFileBytes(file, bytes)) return false;
    reader = ContainerReader::Open(std::move(bytes));
    return reader.has_value();
}

struct Raster {
    u32 size;
    std::vector<float> depth;
    std::vector<float> color; // premultiplied rgb + coverage
    explicit Raster(u32 s) : size(s), depth(static_cast<size_t>(s) * s, 1e30f), color(static_cast<size_t>(s) * s * 4, 0.0f) {}

    void triangle(const Vec3 p[3], const Vec3 n[3], const Vec3& base, const Vec3& lightDir) {
        const float minX = std::floor(std::min({p[0].x, p[1].x, p[2].x}));
        const float maxX = std::ceil(std::max({p[0].x, p[1].x, p[2].x}));
        const float minY = std::floor(std::min({p[0].y, p[1].y, p[2].y}));
        const float maxY = std::ceil(std::max({p[0].y, p[1].y, p[2].y}));
        const int x0 = std::max(0, static_cast<int>(minX)), x1 = std::min(static_cast<int>(size) - 1, static_cast<int>(maxX));
        const int y0 = std::max(0, static_cast<int>(minY)), y1 = std::min(static_cast<int>(size) - 1, static_cast<int>(maxY));
        const float area = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[1].y - p[0].y) * (p[2].x - p[0].x);
        if (std::abs(area) < 1e-8f) return;
        const float inv = 1.0f / area;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
                const float w0 = ((p[1].x - px) * (p[2].y - py) - (p[1].y - py) * (p[2].x - px)) * inv;
                const float w1 = ((p[2].x - px) * (p[0].y - py) - (p[2].y - py) * (p[0].x - px)) * inv;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const float z = w0 * p[0].z + w1 * p[1].z + w2 * p[2].z;
                const size_t i = static_cast<size_t>(y) * size + x;
                if (z >= depth[i]) continue;
                depth[i] = z;
                Vec3 nrm = glm::normalize(n[0] * w0 + n[1] * w1 + n[2] * w2 + Vec3(1e-6f));
                // Imported meshes are not always consistently wound: light the side that faces the camera.
                if (nrm.z < 0.0f) nrm = -nrm;
                const float diff = std::max(glm::dot(nrm, lightDir), 0.0f);
                const float hemi = 0.5f + 0.5f * nrm.y;
                const float lit = 0.22f + 0.30f * hemi + 0.62f * diff;
                const Vec3 c = glm::min(base * lit, Vec3(1.0f));
                color[i * 4 + 0] = c.x;
                color[i * 4 + 1] = c.y;
                color[i * 4 + 2] = c.z;
                color[i * 4 + 3] = 1.0f;
            }
        }
    }
};

} // namespace

bool RenderModelThumbnail(const ModelThumbSource& source, ThumbPixels& out) {
    std::optional<ContainerReader> reader;
    PrefabAsset prefab;
    if (!readContainer(source.prefabFile, reader) || !DeserializePrefab(*reader, prefab) || prefab.nodes.empty()) return false;

    // World matrices (parents precede their children).
    std::vector<Mat4> world(prefab.nodes.size());
    for (size_t i = 0; i < prefab.nodes.size(); ++i) {
        const PrefabNode& n = prefab.nodes[i];
        const Mat4 local = glm::translate(Mat4(1.0f), n.position) * glm::mat4_cast(n.rotation) * glm::scale(Mat4(1.0f), n.scale);
        world[i] = n.parent >= 0 ? world[static_cast<size_t>(n.parent)] * local : local;
    }

    std::unordered_map<AssetId, Loaded, AssetIdHash> cache;
    std::unordered_map<AssetId, Vec3, AssetIdHash> materialColors;
    auto materialColor = [&](AssetId id) {
        auto it = materialColors.find(id);
        if (it != materialColors.end()) return it->second;
        Vec3 c(0.72f, 0.72f, 0.74f);
        auto f = source.files.find(id);
        std::optional<ContainerReader> mr;
        MaterialAsset m;
        if (f != source.files.end() && readContainer(f->second, mr) && DeserializeMaterial(*mr, m)) {
            c = Vec3(m.baseColor);
            // A textured material has a neutral factor: show something lively instead of pure white.
            if (m.baseColorTexture.isValid() && glm::dot(c, Vec3(1.0f)) > 2.9f) c = Vec3(0.78f, 0.74f, 0.68f);
        }
        materialColors.emplace(id, c);
        return c;
    };
    auto loadMesh = [&](AssetId id) -> Loaded* {
        auto it = cache.find(id);
        if (it != cache.end()) return it->second.mesh.vertexStride ? &it->second : nullptr;
        Loaded& l = cache[id];
        auto f = source.files.find(id);
        std::optional<ContainerReader> mr;
        if (f == source.files.end() || !readContainer(f->second, mr) || !DeserializeMesh(*mr, l.mesh)) {
            l.mesh = {};
            return nullptr;
        }
        return &l;
    };

    struct Instance {
        const Loaded* loaded;
        const PrefabNode* node;
        Mat4 m;
    };
    std::vector<Instance> instances;
    Vec3 bmin(1e30f), bmax(-1e30f);
    u64 totalTris = 0;
    for (size_t i = 0; i < prefab.nodes.size(); ++i) {
        const PrefabNode& n = prefab.nodes[i];
        if (!n.mesh.isValid()) continue;
        const Loaded* l = loadMesh(n.mesh);
        if (!l || !l->mesh.findStream(VertexSemantic::Position)) continue;
        instances.push_back({l, &n, world[i]});
        totalTris += l->mesh.indices.size() / 3;
        for (int c = 0; c < 8; ++c) {
            const Vec3 corner((c & 1) ? l->mesh.boundsMax.x : l->mesh.boundsMin.x,
                              (c & 2) ? l->mesh.boundsMax.y : l->mesh.boundsMin.y,
                              (c & 4) ? l->mesh.boundsMax.z : l->mesh.boundsMin.z);
            const Vec3 wp = Vec3(world[i] * Vec4(corner, 1.0f));
            bmin = glm::min(bmin, wp);
            bmax = glm::max(bmax, wp);
        }
    }
    if (instances.empty() || bmin.x > bmax.x) return false;

    // Fixed three-quarter view; orthographic so the framing is exact.
    const Vec3 center = (bmin + bmax) * 0.5f;
    const float radius = std::max(glm::length(bmax - bmin) * 0.5f, 1e-4f);
    const Vec3 viewDir = glm::normalize(Vec3(-0.55f, -0.42f, -0.72f)); // camera looks along this
    const Mat4 view = glm::lookAt(center - viewDir * radius * 3.0f, center, Vec3(0, 1, 0));
    const Vec3 lightDir = glm::normalize(Vec3(-0.4f, 0.8f, 0.5f)); // world space
    const Vec3 lightView = glm::normalize(Vec3(view * Vec4(lightDir, 0.0f)));

    constexpr u32 kSS = 2; // supersampling factor
    const u32 res = kThumbSize * kSS;
    Raster raster(res);
    const float half = radius * 1.08f;
    const float pixelsPerUnit = static_cast<float>(res) * 0.5f / half;
    const u64 stride = std::max<u64>(1, totalTris / 250000);

    for (const Instance& inst : instances) {
        const MeshAsset& mesh = inst.loaded->mesh;
        const VertexStream* ps = mesh.findStream(VertexSemantic::Position);
        const VertexStream* ns = mesh.findStream(VertexSemantic::Normal);
        const Mat4 mv = view * inst.m;
        const Mat3 nm = Mat3(mv);
        const u32 vc = mesh.vertexCount();
        std::vector<Vec3> sp(vc), sn(vc);
        for (u32 v = 0; v < vc; ++v) {
            const u8* base = mesh.vertexData.data() + static_cast<size_t>(v) * mesh.vertexStride;
            Vec3 pos, nor(0, 1, 0);
            std::memcpy(&pos, base + ps->offset, sizeof(Vec3));
            if (ns) std::memcpy(&nor, base + ns->offset, sizeof(Vec3));
            const Vec3 vp = Vec3(mv * Vec4(pos, 1.0f));
            sp[v] = Vec3(static_cast<float>(res) * 0.5f + vp.x * pixelsPerUnit,
                         static_cast<float>(res) * 0.5f - vp.y * pixelsPerUnit, -vp.z);
            sn[v] = glm::normalize(nm * nor + Vec3(1e-6f));
        }
        for (const SubMesh& sm : mesh.subMeshes) {
            const AssetId matId = sm.materialSlot < inst.node->materials.size() ? inst.node->materials[sm.materialSlot] : AssetId{};
            const Vec3 base = matId.isValid() ? materialColor(matId) : Vec3(0.72f, 0.72f, 0.74f);
            const u32 end = std::min<u32>(sm.firstIndex + sm.indexCount, static_cast<u32>(mesh.indices.size()));
            u64 tri = 0;
            for (u32 i = sm.firstIndex; i + 2 < end; i += 3, ++tri) {
                if (stride > 1 && tri % stride) continue;
                const u32 a = mesh.indices[i], b = mesh.indices[i + 1], c = mesh.indices[i + 2];
                if (a >= vc || b >= vc || c >= vc) continue;
                const Vec3 p[3] = {sp[a], sp[b], sp[c]};
                const Vec3 n[3] = {sn[a], sn[b], sn[c]};
                raster.triangle(p, n, base, lightView);
            }
        }
    }

    // Downsample the supersampled image; coverage becomes alpha.
    out.width = kThumbSize;
    out.height = kThumbSize;
    out.rgba.assign(static_cast<size_t>(kThumbSize) * kThumbSize * 4, 0);
    for (u32 y = 0; y < kThumbSize; ++y) {
        for (u32 x = 0; x < kThumbSize; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (u32 sy = 0; sy < kSS; ++sy) {
                for (u32 sx = 0; sx < kSS; ++sx) {
                    const size_t i = (static_cast<size_t>(y * kSS + sy) * res + (x * kSS + sx)) * 4;
                    for (int c = 0; c < 4; ++c) acc[c] += raster.color[i + c];
                }
            }
            const float a = acc[3] / static_cast<float>(kSS * kSS);
            u8* d = out.rgba.data() + (static_cast<size_t>(y) * kThumbSize + x) * 4;
            if (a > 0.0f) {
                const float k = 1.0f / acc[3]; // un-premultiply: acc[3] counts the covered samples
                for (int c = 0; c < 3; ++c) d[c] = static_cast<u8>(std::clamp(acc[c] * k, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            d[3] = static_cast<u8>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    return true;
}

bool RenderMaterialThumbnail(const MaterialThumbSource& src, ThumbPixels& out) {
    ThumbPixels albedo;
    const bool textured = !src.albedoImage.empty() && LoadImageThumbnail(src.albedoImage, albedo) && albedo.width && albedo.height;
    constexpr u32 kSS = 2;
    const u32 res = kThumbSize * kSS;
    const Vec3 lightDir = glm::normalize(Vec3(-0.45f, 0.65f, 0.62f));
    const Vec3 viewDir(0, 0, 1);
    const float shininess = 8.0f + (1.0f - src.roughness) * 120.0f;
    out.width = kThumbSize;
    out.height = kThumbSize;
    out.rgba.assign(static_cast<size_t>(kThumbSize) * kThumbSize * 4, 0);
    const float radius = static_cast<float>(res) * 0.46f;
    const float cx = static_cast<float>(res) * 0.5f, cy = static_cast<float>(res) * 0.5f;

    auto shade = [&](u32 x, u32 y, float rgba[4]) {
        const float dx = (static_cast<float>(x) + 0.5f - cx) / radius, dy = -(static_cast<float>(y) + 0.5f - cy) / radius;
        const float r2 = dx * dx + dy * dy;
        if (r2 > 1.0f) {
            rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.0f;
            return;
        }
        const Vec3 n(dx, dy, std::sqrt(1.0f - r2));
        Vec3 albedoColor = src.baseColor;
        if (textured) {
            // Longitude/latitude, tiled twice around so fine textures stay legible on a small sphere.
            const float u = (std::atan2(n.x, n.z) / (2.0f * math::kPi) + 0.5f) * 2.0f;
            const float v = (std::asin(std::clamp(n.y, -1.0f, 1.0f)) / math::kPi + 0.5f) * 2.0f;
            const u32 tx = static_cast<u32>((u - std::floor(u)) * static_cast<float>(albedo.width)) % albedo.width;
            const u32 ty = static_cast<u32>((1.0f - (v - std::floor(v))) * static_cast<float>(albedo.height)) % albedo.height;
            const u8* p = &albedo.rgba[(static_cast<size_t>(ty) * albedo.width + tx) * 4];
            albedoColor *= Vec3(p[0], p[1], p[2]) / 255.0f;
        }
        const float diff = std::max(glm::dot(n, lightDir), 0.0f);
        const Vec3 h = glm::normalize(lightDir + viewDir);
        const float spec = std::pow(std::max(glm::dot(n, h), 0.0f), shininess) * (0.25f + 0.75f * src.metallic) * (1.0f - 0.6f * src.roughness);
        const Vec3 specColor = glm::mix(Vec3(1.0f), albedoColor, src.metallic);
        const float ambient = 0.28f + 0.2f * (0.5f + 0.5f * n.y);
        const Vec3 diffuseColor = albedoColor * (1.0f - 0.85f * src.metallic);
        const Vec3 c = glm::min(diffuseColor * (ambient + 0.85f * diff) + specColor * spec, Vec3(1.0f));
        rgba[0] = c.x;
        rgba[1] = c.y;
        rgba[2] = c.z;
        rgba[3] = 1.0f;
    };

    for (u32 y = 0; y < kThumbSize; ++y) {
        for (u32 x = 0; x < kThumbSize; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (u32 sy = 0; sy < kSS; ++sy) {
                for (u32 sx = 0; sx < kSS; ++sx) {
                    float s[4];
                    shade(x * kSS + sx, y * kSS + sy, s);
                    for (int c = 0; c < 3; ++c) acc[c] += s[c] * s[3]; // coverage weighted
                    acc[3] += s[3];
                }
            }
            u8* d = out.rgba.data() + (static_cast<size_t>(y) * kThumbSize + x) * 4;
            if (acc[3] > 0.0f) {
                for (int c = 0; c < 3; ++c) d[c] = static_cast<u8>(std::clamp(acc[c] / acc[3], 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            d[3] = static_cast<u8>(acc[3] / static_cast<float>(kSS * kSS) * 255.0f + 0.5f);
        }
    }
    return true;
}

} // namespace foh
