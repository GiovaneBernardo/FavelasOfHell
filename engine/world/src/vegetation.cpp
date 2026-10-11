#include "foh/world/vegetation.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include "foh/log.h"
#include "foh/renderer/render_scene.h"
#include "foh/world/noise.h"
#include "vegetation_assets.h"

namespace foh {

using noise::Rand01;
using noise::Smoothstep;

namespace {

// Every tile is one retained batch per tree kind and per trunk/leaf mesh, drawn by every pass (depth, four cascades, G-buffer,
// probes): big tiles keep the draw count, which is what costs in a Debug build and on the CPU, low. 128 m / 32 m.
constexpr f32 kTreeTile = 128.0f;
constexpr f32 kGrassTile = 32.0f;
constexpr f32 kTreeSpacing = 5.0f;  // m between planting spots at density 1
constexpr f32 kPalmSpacing = 7.0f;
constexpr f32 kGrassSpacing = 0.40f; // m: tufts overlap, so the ground reads as lawn, not as dots

constexpr u64 kAssetHi = 0x7E44A1A1ull;
// Mesh ids: kind 0..kTreeVariants-1 are trees, kTreeVariants the palm; per kind and LOD one trunk and one leaf mesh.
AssetId TrunkMesh(u32 kind, u32 lod) { return AssetId{kAssetHi, 10 + u64(kind) * 4 + u64(lod) * 2}; }
AssetId LeafMesh(u32 kind, u32 lod) { return AssetId{kAssetHi, 11 + u64(kind) * 4 + u64(lod) * 2}; }
const AssetId kGrassMesh{kAssetHi, 200};
const AssetId kLeafTexture{kAssetHi, 300};
const AssetId kGrassTexture{kAssetHi, 301};
const AssetId kBarkTexture{kAssetHi, 302};
const AssetId kLeafMaterial{kAssetHi, 400};
const AssetId kGrassMaterial{kAssetHi, 401};
const AssetId kBarkMaterial{kAssetHi, 402};

u64 TileKey(i32 x, i32 z) { return (u64(static_cast<u32>(x)) << 32) | u64(static_cast<u32>(z)); }

f32 ClearingFactor(const std::vector<VegetationClearing>& clearings, f32 x, f32 z) {
    f32 f = 1.0f;
    for (const VegetationClearing& c : clearings) {
        const f32 d = std::sqrt((x - c.x) * (x - c.x) + (z - c.z) * (z - c.z));
        f *= Smoothstep(c.radius, c.radius + std::max(c.feather, 0.01f), d);
    }
    return f;
}

u32 PackColor(f32 r, f32 g, f32 b) {
    auto q = [](f32 v) { return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return q(r) | (q(g) << 8) | (q(b) << 16) | 0xFF000000u;
}

// Height and surface slope (the normal's y) at a point: three samples.
f32 HeightAndUp(const Terrain& terrain, f32 x, f32 z, f32& up) {
    constexpr f32 d = 0.6f;
    const f32 h = terrain.height(x, z), hx = terrain.height(x + d, z), hz = terrain.height(x, z + d);
    up = d / std::sqrt((h - hx) * (h - hx) + (h - hz) * (h - hz) + d * d);
    return h;
}

struct TreeRec {
    Vec3 pos;
    f32 scale, yaw;
    u32 tint;
    u8 kind;
    f32 radius; // trunk collision radius in metres (already scaled)
};

// What a job hands back for one tile.
struct TileResult {
    u64 key = 0;
    bool grass = false;
    u64 generation = 0;
    std::vector<TreeRec> trees;
    std::vector<StaticInstance> tufts;
    f32 minY = 1e9f, maxY = -1e9f;
};

} // namespace

struct Vegetation::Impl {
    struct Tile {
        i32 x = 0, z = 0;
        std::vector<u32> sets;   // renderer static set ids
        std::vector<u8> setKind; // per set
        std::vector<u8> setLeaf; // per set: 1 = leaf/frond set, 0 = trunk set
        std::vector<TreeRec> trees;
        u32 treeCount = 0, palmCount = 0, tuftCount = 0;
        u8 lod = 1;
        bool stale = true, built = false, inFlight = false;
        f32 dist = 0.0f;
    };
    struct Task {
        Impl* impl;
        const Terrain* terrain;
        VegetationSettings settings;
        i32 x, z;
        bool grass;
        u64 generation;
        f32 trunkRadius[kTreeVariants + 1];
    };

    Impl(Renderer& r, World& w, JobSystem* j) : renderer(r), jobs(j) {
        if (const RenderSceneLink* link = w.resource<RenderSceneLink>()) scene = link->scene;
    }

    Renderer& renderer;
    JobSystem* jobs;
    RenderScene* scene = nullptr;
    std::unordered_map<u64, Tile> treeTiles, grassTiles;
    std::mutex mutex;
    std::vector<TileResult> done;
    JobCounter counter;
    u32 inFlight = 0;
    u64 generation = 0, dirtyCursor = 0;
    u32 builtSeed = 0;
    f32 trunkRadius[kTreeVariants + 1] = {};
    f32 treeHeight[kTreeVariants + 1] = {};

    // ---- content ----

    void uploadContent() {
        auto uploadMesh = [&](const AssetId& id, const VegMesh& m) {
            if (m.positions.empty()) return;
            std::vector<Vec4> tangents = m.tangents;
            if (tangents.size() != m.positions.size()) tangents.clear();
            const MeshIndexRange whole{0, static_cast<u32>(m.indices.size())};
            if (!renderer.uploadMeshAsset(id, MeshGeometry{m.positions, m.normals, m.uvs, tangents, m.indices}, std::span(&whole, 1)))
                FOH_ERROR("vegetation: could not upload a mesh");
        };
        for (u32 kind = 0; kind <= kTreeVariants; ++kind)
            for (u32 lod = 0; lod < kTreeLods; ++lod) {
                const TreeMeshes t = kind < kTreeVariants ? BuildTree(kind, lod) : BuildPalm(lod);
                uploadMesh(TrunkMesh(kind, lod), t.trunk);
                uploadMesh(LeafMesh(kind, lod), t.leaves);
                trunkRadius[kind] = t.trunkRadius;
                treeHeight[kind] = t.height;
            }
        uploadMesh(kGrassMesh, BuildGrassMesh());

        auto uploadImage = [&](const AssetId& id, const VegImage& img, f32 cutoff, u32 levels, TextureFormat format) {
            TextureUpload up;
            up.id = id;
            up.format = format;
            up.width = img.width;
            up.height = img.height;
            up.mips = BuildFoliageMips(img, cutoff, levels);
            renderer.uploadTexture(std::move(up));
        };
        uploadImage(kLeafTexture, BuildLeafAtlas(), 0.5f, 9, TextureFormat::Rgba8Srgb);
        uploadImage(kGrassTexture, BuildGrassImage(), 0.5f, 9, TextureFormat::Rgba8Srgb);
        uploadImage(kBarkTexture, BuildBarkImage(), 0.0f, 8, TextureFormat::Rgba8Srgb);

        MaterialParams leaf;
        leaf.baseColorTexture = kLeafTexture;
        leaf.alphaMask = true;
        leaf.alphaCutoff = 0.5f;
        leaf.doubleSided = true;
        leaf.foliage = true;
        leaf.translucency = 0.55f;
        leaf.roughness = 0.8f;
        renderer.setMaterial(kLeafMaterial, leaf);
        MaterialParams grass = leaf;
        grass.baseColorTexture = kGrassTexture;
        grass.translucency = 0.30f;
        grass.roughness = 0.9f;
        renderer.setMaterial(kGrassMaterial, grass);
        setBark(kBarkTexture, {});
    }

    void setBark(const AssetId& color, const AssetId& normal) {
        MaterialParams bark;
        bark.baseColorTexture = color;
        bark.normalTexture = normal;
        bark.roughness = 0.88f;
        bark.metallic = 0.0f;
        renderer.setMaterial(kBarkMaterial, bark);
    }

    // ---- tile generation (runs on a job) ----

    static void treeTile(const Task& t, TileResult& out) {
        const Terrain& terrain = *t.terrain;
        const VegetationSettings& s = t.settings;
        const f32 x0 = f32(t.x) * kTreeTile, z0 = f32(t.z) * kTreeTile;
        const u32 seed = s.seed * 2654435761u;
        auto add = [&](Vec3 p, f32 yaw, f32 scale, u32 tint, u8 kind) {
            out.trees.push_back({p, scale, yaw, tint, kind, t.trunkRadius[kind] * scale * 1.05f});
            out.minY = std::min(out.minY, p.y);
            out.maxY = std::max(out.maxY, p.y);
        };
        const f32 spacing = kTreeSpacing / std::sqrt(std::max(s.treeDensity, 0.05f));
        const int n = s.trees ? int(kTreeTile / spacing) : 0;
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const i32 gi = t.x * 1000 + i, gj = t.z * 1000 + j;
                const f32 x = x0 + (f32(i) + 0.1f + 0.8f * Rand01(gi, gj, seed + 1)) * spacing;
                const f32 z = z0 + (f32(j) + 0.1f + 0.8f * Rand01(gi, gj, seed + 2)) * spacing;
                f32 up;
                const f32 h = HeightAndUp(terrain, x, z, up);
                if (h < 1.6f || h > 520.0f || up < 0.64f) continue;
                f32 clear = ClearingFactor(s.clearings, x, z);
                if (s.mask) clear *= s.mask->density(x, z);
                if (clear <= 0.0f) continue;
                const f32 forest = (0.30f + 0.70f * Smoothstep(-0.25f, 0.35f, noise::Fbm(x / 160.0f, z / 160.0f, seed + 3, 3))) * Smoothstep(0.64f, 0.80f, up) *
                                   (1.0f - Smoothstep(430.0f, 520.0f, h)) * Smoothstep(3.0f, 16.0f, h) * clear;
                if (Rand01(gi, gj, seed + 4) > forest * 0.92f) continue;
                const f32 r = Rand01(gi, gj, seed + 7), g = Rand01(gi, gj, seed + 8);
                const f32 shade = 0.78f + 0.3f * r;
                add(Vec3(x, h - 0.15f, z), Rand01(gi, gj, seed + 5) * 6.2831853f, 0.72f + 0.7f * std::pow(Rand01(gi, gj, seed + 6), 1.3f),
                    PackColor(shade * (0.92f + 0.12f * g), shade, shade * (0.85f + 0.12f * r)), static_cast<u8>(Rand01(gi, gj, seed + 9) * f32(kTreeVariants)) % kTreeVariants);
            }
        if (s.palms) {
            const f32 pSpacing = kPalmSpacing / std::sqrt(std::max(s.treeDensity, 0.05f));
            const int pn = int(kTreeTile / pSpacing);
            for (int j = 0; j < pn; ++j)
                for (int i = 0; i < pn; ++i) {
                    const i32 gi = t.x * 1000 + i + 500, gj = t.z * 1000 + j + 500;
                    const f32 x = x0 + (f32(i) + 0.1f + 0.8f * Rand01(gi, gj, seed + 21)) * pSpacing;
                    const f32 z = z0 + (f32(j) + 0.1f + 0.8f * Rand01(gi, gj, seed + 22)) * pSpacing;
                    f32 up;
                    const f32 h = HeightAndUp(terrain, x, z, up);
                    if (h < 1.4f || h > 20.0f || up < 0.86f) continue; // the beach front and low ground
                    const f32 chance = 0.16f * Smoothstep(1.4f, 4.0f, h) * (1.0f - Smoothstep(10.0f, 20.0f, h)) * ClearingFactor(s.clearings, x, z) * (s.mask ? s.mask->density(x, z) : 1.0f) *
                                       (0.4f + 0.6f * Smoothstep(-0.2f, 0.4f, noise::Fbm(x / 70.0f, z / 70.0f, seed + 23, 2)));
                    if (Rand01(gi, gj, seed + 24) > chance) continue;
                    add(Vec3(x, h - 0.1f, z), Rand01(gi, gj, seed + 25) * 6.2831853f, 0.85f + 0.4f * Rand01(gi, gj, seed + 26), 0xFFFFFFFFu, static_cast<u8>(kTreeVariants));
                }
        }
    }

    // Plants that were placed on purpose: street trees, park trees, beach palms.
    static void explicitPlants(const Task& t, TileResult& out) {
        if (!t.settings.plants || !t.settings.trees) return;
        const auto it = t.settings.plants->byTile.find(TileKey(t.x, t.z));
        if (it == t.settings.plants->byTile.end()) return;
        for (const VegetationPlant& p : it->second) {
            f32 up;
            const f32 h = HeightAndUp(*t.terrain, p.x, p.z, up);
            if (h < 0.9f) continue; // under the sea (a survey tree on a rock or a wrong point)
            const u8 kind = std::min<u8>(p.kind, static_cast<u8>(kTreeVariants));
            out.trees.push_back({Vec3(p.x, h - (kind == kTreeVariants ? 0.1f : 0.15f), p.z), p.scale, p.yaw, p.tint, kind, t.trunkRadius[kind] * p.scale * 1.05f});
            out.minY = std::min(out.minY, h);
            out.maxY = std::max(out.maxY, h);
        }
    }

    static void grassTile(const Task& t, TileResult& out) {
        const Terrain& terrain = *t.terrain;
        const VegetationSettings& s = t.settings;
        const f32 x0 = f32(t.x) * kGrassTile, z0 = f32(t.z) * kGrassTile;
        const u32 seed = s.seed * 2246822519u;
        const f32 spacing = kGrassSpacing / std::sqrt(std::max(s.grassDensity, 0.05f));
        const int n = int(kGrassTile / spacing);
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const i32 gi = t.x * 1000 + i, gj = t.z * 1000 + j;
                const f32 x = x0 + (f32(i) + 0.05f + 0.9f * Rand01(gi, gj, seed + 1)) * spacing;
                const f32 z = z0 + (f32(j) + 0.05f + 0.9f * Rand01(gi, gj, seed + 2)) * spacing;
                f32 up;
                const f32 h = HeightAndUp(terrain, x, z, up);
                if (h < 2.4f || h > 380.0f || up < 0.78f) continue;
                const f32 patch = 0.55f + 0.45f * Smoothstep(-0.3f, 0.4f, noise::Fbm(x / 13.0f, z / 13.0f, seed + 3, 2));
                const f32 dry = 1.0f - 0.65f * Smoothstep(0.55f, 0.8f, noise::Fbm(x / 41.0f, z / 41.0f, seed + 5, 2) * 0.5f + 0.5f); // bare patches
                const f32 chance = patch * dry * Smoothstep(0.78f, 0.9f, up) * Smoothstep(2.4f, 5.0f, h) * ClearingFactor(s.clearings, x, z) * (s.mask ? s.mask->density(x, z) : 1.0f);
                if (Rand01(gi, gj, seed + 4) > chance) continue;
                const f32 v = Rand01(gi, gj, seed + 8);
                out.tufts.push_back(MakeStaticInstance(Vec3(x, h - 0.04f, z), Rand01(gi, gj, seed + 6) * 6.2831853f, 0.65f + 0.85f * Rand01(gi, gj, seed + 7),
                                                       PackColor(0.62f + 0.26f * v, 0.66f + 0.18f * v, 0.54f + 0.18f * v)));
                out.minY = std::min(out.minY, h);
                out.maxY = std::max(out.maxY, h);
            }
        // Shuffle (deterministically): sets are thinned by drawing a prefix, which must be an even sample of the tile.
        for (size_t k = out.tufts.size(); k > 1; --k) {
            const size_t m = noise::Hash(i32(k), t.x * 7 + t.z, seed) % k;
            std::swap(out.tufts[k - 1], out.tufts[m]);
        }
    }

    static void runTask(void* arg) {
        std::unique_ptr<Task> t(static_cast<Task*>(arg));
        TileResult r;
        r.key = TileKey(t->x, t->z);
        r.grass = t->grass;
        r.generation = t->generation;
        if (t->grass) grassTile(*t, r);
        else {
            treeTile(*t, r);
            explicitPlants(*t, r);
        }
        std::lock_guard lock(t->impl->mutex);
        t->impl->done.push_back(std::move(r));
    }

    // ---- planting ----

    void clearTile(Tile& t) {
        if (scene)
            for (const u32 id : t.sets) scene->removeStaticSet(id);
        t.sets.clear();
        t.setKind.clear();
        t.setLeaf.clear();
        t.trees.clear();
        t.treeCount = t.palmCount = t.tuftCount = 0;
        t.built = false;
    }

    void plant(Tile& t, TileResult& r) {
        clearTile(t);
        if (!scene) {
            t.built = true;
            return;
        }
        if (r.grass) {
            if (!r.tufts.empty()) {
                StaticInstanceSet set;
                set.mesh = kGrassMesh;
                set.material = kGrassMaterial;
                set.castsShadow = false; // the ground is shaded by contact shadows + AO; 70k blades in four cascades is not worth it
                set.inProbes = false;
                set.center = Vec3((f32(t.x) + 0.5f) * kGrassTile, 0.5f * (r.minY + r.maxY) + 0.3f, (f32(t.z) + 0.5f) * kGrassTile);
                set.radius = kGrassTile * 0.71f + 0.5f * (r.maxY - r.minY) + 1.0f;
                set.instances = std::move(r.tufts);
                t.tuftCount = static_cast<u32>(set.instances.size());
                t.sets.push_back(scene->addStaticSet(std::move(set)));
                t.setKind.push_back(0);
                t.setLeaf.push_back(0);
            }
        } else {
            t.trees = std::move(r.trees);
            std::vector<StaticInstance> bark[kTreeVariants + 1], leaf[kTreeVariants + 1];
            for (const TreeRec& tr : t.trees) {
                const f32 bv = 0.85f + 0.3f * ((tr.tint & 0xFFu) / 255.0f); // trunks vary a little with the tree's own tint
                bark[tr.kind].push_back(MakeStaticInstance(tr.pos, tr.yaw, tr.scale, PackColor(0.46f * bv, 0.36f * bv, 0.28f * bv))); // brown: the texture set is a pale grey
                leaf[tr.kind].push_back(MakeStaticInstance(tr.pos, tr.yaw, tr.scale, tr.tint));
                (tr.kind == kTreeVariants ? t.palmCount : t.treeCount) += 1;
            }
            const Vec3 centre((f32(t.x) + 0.5f) * kTreeTile, 0.5f * (r.minY + r.maxY) + 9.0f, (f32(t.z) + 0.5f) * kTreeTile);
            const f32 radius = kTreeTile * 0.71f + 0.5f * (r.maxY - r.minY) + 20.0f;
            for (u32 kind = 0; kind <= kTreeVariants; ++kind) {
                if (bark[kind].empty()) continue;
                for (int leafSet = 0; leafSet < 2; ++leafSet) {
                    StaticInstanceSet set;
                    set.mesh = leafSet ? LeafMesh(kind, t.lod) : TrunkMesh(kind, t.lod);
                    set.material = leafSet ? kLeafMaterial : kBarkMaterial;
                    set.center = centre;
                    set.radius = radius;
                    set.instances = leafSet ? leaf[kind] : bark[kind];
                    t.sets.push_back(scene->addStaticSet(std::move(set)));
                    t.setKind.push_back(static_cast<u8>(kind));
                    t.setLeaf.push_back(static_cast<u8>(leafSet));
                }
            }
        }
        t.built = true;
        t.stale = false;
    }

    void submit(const Terrain& terrain, const VegetationSettings& s, Tile& t, bool grass) {
        t.inFlight = true;
        t.stale = false;
        ++inFlight;
        auto* task = new Task{this, &terrain, s, t.x, t.z, grass, terrain.generation(), {}};
        for (u32 k = 0; k <= kTreeVariants; ++k) task->trunkRadius[k] = trunkRadius[k];
        if (jobs) jobs->submit(&runTask, task, counter, JobPriority::Low);
        else runTask(task);
    }

    // Tiles within range get a job (nearest first, up to a few in flight); tiles that left the range are removed.
    void stream(std::unordered_map<u64, Tile>& tiles, f32 tileSize, f32 range, Vec3 focus, const Terrain& terrain, bool grass, u32 maxInFlight, u32& waiting) {
        const i32 x0 = i32(std::floor((focus.x - range) / tileSize)), x1 = i32(std::floor((focus.x + range) / tileSize));
        const i32 z0 = i32(std::floor((focus.z - range) / tileSize)), z1 = i32(std::floor((focus.z + range) / tileSize));
        std::vector<Tile*> todo;
        for (i32 z = z0; z <= z1; ++z)
            for (i32 x = x0; x <= x1; ++x) {
                const f32 cx = (f32(x) + 0.5f) * tileSize, cz = (f32(z) + 0.5f) * tileSize;
                const f32 d = std::sqrt((cx - focus.x) * (cx - focus.x) + (cz - focus.z) * (cz - focus.z));
                if (d > range + tileSize * 0.71f) continue;
                auto [it, inserted] = tiles.try_emplace(TileKey(x, z));
                Tile& t = it->second;
                if (inserted) {
                    t.x = x;
                    t.z = z;
                }
                t.dist = d;
                if ((!t.built || t.stale) && !t.inFlight) todo.push_back(&t);
                else if (t.inFlight) ++waiting;
            }
        std::sort(todo.begin(), todo.end(), [](const Tile* a, const Tile* b) { return a->dist < b->dist; });
        waiting += static_cast<u32>(todo.size());
        for (Tile* t : todo) {
            if (inFlight >= maxInFlight) break;
            if (!grass) t->lod = t->dist - 0.5f * kTreeTile < settingsRef->treeNearDistance ? 0 : 1; // detailed if any part of the tile is near
            submit(terrain, *settingsRef, *t, grass);
            --waiting;
        }
        for (auto it = tiles.begin(); it != tiles.end();) {
            Tile& t = it->second;
            const f32 cx = (f32(t.x) + 0.5f) * tileSize, cz = (f32(t.z) + 0.5f) * tileSize;
            const f32 d = std::sqrt((cx - focus.x) * (cx - focus.x) + (cz - focus.z) * (cz - focus.z));
            if (d > range + tileSize * 1.6f + 30.0f && !t.inFlight) {
                clearTile(t);
                it = tiles.erase(it);
            } else {
                ++it;
            }
        }
    }

    const VegetationSettings* settingsRef = nullptr;
};

Vegetation::Vegetation(Renderer& renderer, World& world, JobSystem* jobs) : m_impl(std::make_unique<Impl>(renderer, world, jobs)) { m_impl->uploadContent(); }

Vegetation::~Vegetation() {
    Impl& im = *m_impl;
    if (im.jobs) im.jobs->wait(im.counter); // jobs in flight hold pointers into the terrain and this object
    for (auto& [k, t] : im.treeTiles) im.clearTile(t);
    for (auto& [k, t] : im.grassTiles) im.clearTile(t);
    for (u32 kind = 0; kind <= kTreeVariants; ++kind)
        for (u32 lod = 0; lod < kTreeLods; ++lod) {
            im.renderer.releaseMeshAsset(TrunkMesh(kind, lod));
            im.renderer.releaseMeshAsset(LeafMesh(kind, lod));
        }
    im.renderer.releaseMeshAsset(kGrassMesh);
    im.renderer.releaseTexture(kLeafTexture);
    im.renderer.releaseTexture(kGrassTexture);
    im.renderer.releaseTexture(kBarkTexture);
}

void Vegetation::rebuildAll() {
    for (auto& [k, t] : m_impl->treeTiles) t.stale = true;
    for (auto& [k, t] : m_impl->grassTiles) t.stale = true;
}

void Vegetation::setBarkTextures(const AssetId& color, const AssetId& normal) {
    m_impl->setBark(color.isValid() ? color : kBarkTexture, normal);
}

Vec2 Vegetation::pushOutOfTrunks(Vec2 centre, f32 radius) const {
    const Impl& im = *m_impl;
    const i32 tx = i32(std::floor(centre.x / kTreeTile)), tz = i32(std::floor(centre.y / kTreeTile));
    for (int pass = 0; pass < 2; ++pass)
        for (i32 dz = -1; dz <= 1; ++dz)
            for (i32 dx = -1; dx <= 1; ++dx) {
                const auto it = im.treeTiles.find(TileKey(tx + dx, tz + dz));
                if (it == im.treeTiles.end()) continue;
                for (const TreeRec& t : it->second.trees) {
                    const f32 ox = centre.x - t.pos.x, oz = centre.y - t.pos.z;
                    const f32 minDist = t.radius + radius;
                    const f32 d2 = ox * ox + oz * oz;
                    if (d2 >= minDist * minDist) continue;
                    const f32 d = std::sqrt(d2);
                    if (d < 1e-4f) {
                        centre.x += minDist;
                    } else {
                        centre.x = t.pos.x + ox / d * minDist;
                        centre.y = t.pos.z + oz / d * minDist;
                    }
                }
            }
    return centre;
}

void Vegetation::update(const Terrain& terrain, Vec3 focus) {
    Impl& im = *m_impl;
    im.settingsRef = &settings;

    // 1. Finished tiles are planted (a few per frame: adding a set copies its instances).
    {
        std::vector<TileResult> ready;
        {
            std::lock_guard lock(im.mutex);
            const size_t take = std::min<size_t>(im.done.size(), 8);
            ready.assign(std::make_move_iterator(im.done.begin()), std::make_move_iterator(im.done.begin() + static_cast<std::ptrdiff_t>(take)));
            im.done.erase(im.done.begin(), im.done.begin() + static_cast<std::ptrdiff_t>(take));
        }
        for (TileResult& r : ready) {
            --im.inFlight;
            auto& tiles = r.grass ? im.grassTiles : im.treeTiles;
            const auto it = tiles.find(r.key);
            if (it == tiles.end()) continue; // left the range meanwhile
            Impl::Tile& t = it->second;
            t.inFlight = false;
            im.plant(t, r);
            if (r.generation != terrain.generation()) t.stale = true; // planted on an older terrain: again
        }
    }

    // 2. What changed under the plants: the terrain (everything) or sculpting (the rectangles).
    if (terrain.generation() != im.generation) {
        im.generation = terrain.generation();
        im.dirtyCursor = terrain.dirtyCursor();
        rebuildAll();
    } else {
        const std::vector<TerrainDirtyRect> rects = terrain.dirtyRectsSince(im.dirtyCursor);
        if (!rects.empty()) {
            auto mark = [&](std::unordered_map<u64, Impl::Tile>& tiles, f32 size) {
                for (auto& [k, t] : tiles) {
                    const f32 tx = f32(t.x) * size, tz = f32(t.z) * size;
                    for (const TerrainDirtyRect& r : rects)
                        if (tx < r.maxX && r.minX < tx + size && tz < r.maxZ && r.minZ < tz + size) {
                            t.stale = true;
                            break;
                        }
                }
            };
            mark(im.treeTiles, kTreeTile);
            mark(im.grassTiles, kGrassTile);
        }
    }
    if (im.builtSeed != settings.seed) {
        im.builtSeed = settings.seed;
        rebuildAll();
    }

    // 3. Tiles in range get generated, tiles out of range go.
    m_stats = {};
    const u32 maxInFlight = im.jobs ? std::max(4u, im.jobs->workerCount() / 2) : 2u;
    if (settings.trees || settings.palms) {
        im.stream(im.treeTiles, kTreeTile, settings.treeDistance, focus, terrain, false, maxInFlight, m_stats.waiting);
    } else {
        for (auto& [k, t] : im.treeTiles) im.clearTile(t);
        im.treeTiles.clear();
    }
    if (settings.grass) {
        im.stream(im.grassTiles, kGrassTile, settings.grassDistance, focus, terrain, true, maxInFlight, m_stats.waiting);
    } else {
        for (auto& [k, t] : im.grassTiles) im.clearTile(t);
        im.grassTiles.clear();
    }

    // 4. LOD: trees swap meshes with distance (with hysteresis), grass thins out.
    if (im.scene) {
        for (auto& [k, t] : im.treeTiles) {
            if (!t.built) continue;
            const f32 near = t.dist - 0.5f * kTreeTile; // distance to the near edge of the tile
            const u8 want = near < settings.treeNearDistance ? 0 : (near > settings.treeNearDistance + 25.0f ? 1 : t.lod);
            if (want == t.lod) continue;
            t.lod = want;
            for (size_t i = 0; i < t.sets.size(); ++i)
                if (StaticInstanceSet* set = im.scene->staticSet(t.sets[i])) set->mesh = t.setLeaf[i] ? LeafMesh(t.setKind[i], t.lod) : TrunkMesh(t.setKind[i], t.lod);
        }
        const f32 grassRange = std::max(settings.grassDistance, 1.0f);
        for (auto& [k, t] : im.grassTiles) {
            if (!t.built || t.sets.empty()) continue;
            if (StaticInstanceSet* set = im.scene->staticSet(t.sets[0])) {
                const f32 u = t.dist / grassRange; // 0 near .. 1 at the edge
                set->fraction = u < 0.3f ? 1.0f : (1.0f - 0.8f * Smoothstep(0.3f, 1.0f, u)); // the far edge keeps a fifth of its tufts
            }
        }
    }

    for (const auto& [k, t] : im.treeTiles)
        if (t.built) {
            ++m_stats.treeTiles;
            m_stats.trees += t.treeCount;
            m_stats.palms += t.palmCount;
        }
    for (const auto& [k, t] : im.grassTiles)
        if (t.built) {
            ++m_stats.grassTiles;
            m_stats.tufts += t.tuftCount;
        }
}

} // namespace foh
