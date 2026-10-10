#include "foh/world/terrain.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include "foh/log.h"
#include "foh/world/noise.h"

namespace foh {

namespace {

using namespace noise;

i32 FloorDiv(i32 a, i32 b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0); }

u64 TileKey(i32 tx, i32 tz) { return (u64(static_cast<u32>(tx)) << 32) | u64(static_cast<u32>(tz)); }
i32 KeyX(u64 key) { return static_cast<i32>(static_cast<u32>(key >> 32)); }
i32 KeyZ(u64 key) { return static_cast<i32>(static_cast<u32>(key & 0xFFFFFFFFu)); }

constexpr u32 kTileSamples = kTerrainTileCells + 1; // 65 samples per side, neighbours share a row/column
constexpr size_t kTileFloats = size_t(kTileSamples) * kTileSamples;

} // namespace

namespace {
f32 ProceduralBaseHeight(const TerrainGenParams& p, f32 x, f32 z) {
    const u32 s = p.seed * 7919u;
    const f32 floorRel = p.floorHeight - p.seaLevel; // the floor relative to the sea level (negative)
    // Domain warp (z only: the coast runs along x) so that no feature lines up with the axes.
    const f32 wz = z + 38.0f * Fbm(x / 170.0f, z / 170.0f, s + 2, 3);

    // The land sinks back to the floor away from the generated area; everything beyond is exactly flat.
    const f32 mask = (1.0f - Smoothstep(p.landWidth, p.landWidth + 700.0f, std::abs(x))) *
                     (1.0f - Smoothstep(p.landDepth, p.landDepth + 650.0f, -z));
    if (mask <= 0.0f) return p.floorHeight; // far inland / sideways: the floor

    // Shoreline: low-frequency wobble (headlands and bays) plus the beach cove.
    f32 coast = p.coastZ + p.coastWobble * Fbm(x / p.coastScale, 0.37f, s + 3, 3);
    const f32 cx = (x - p.coveX) / 95.0f;
    coast -= p.coveDepth * std::exp(-cx * cx);
    const f32 d = coast - wz; // metres inland (negative: in the sea)
    const f32 coveNear = std::exp(-((x - p.coveX) / 170.0f) * ((x - p.coveX) / 170.0f));

    f32 f; // height relative to the sea level, before the land mask
    if (d <= 0.0f) {
        const f32 off = -d;
        // A smooth fall from the shore to the floor; gentler in front of the beach.
        f = floorRel * (1.0f - std::exp(-off / (p.seabedScale * (1.0f + 0.9f * coveNear))));
        // Rocks breaking the surface near the shore.
        f += 7.0f * (1.0f - Smoothstep(0.0f, 55.0f, off)) * Ridged(x / 16.0f, z / 16.0f, s + 4, 3) * p.rockiness *
             (0.8f + 0.6f * Fbm(x / 90.0f, z / 90.0f, s + 5, 2));
    } else {
        // The favela slope: constant grade that eases off far inland, flattened toward the beach in the cove.
        f32 h1 = p.slope * std::min(d, p.slopeSpan) + 0.15f * p.slope * std::max(0.0f, d - p.slopeSpan);
        h1 *= 1.0f - 0.8f * coveNear * (1.0f - Smoothstep(0.0f, 110.0f, d));

        // Massif (Dois Irmaos-like) and the backdrop ridge, both broken up by ridged noise.
        const f32 mx = (x - p.massifX) / p.massifRadius, mz = (z - p.massifZ) / (p.massifRadius * 0.8f);
        const f32 md = std::sqrt(mx * mx + mz * mz);
        const f32 shape = std::pow(std::max(0.0f, 1.0f - md), 1.1f);
        const f32 crest = 0.7f + 0.6f * Ridged(x / 150.0f, z / 150.0f, s + 6, 4);
        const f32 massif = p.massifHeight * shape * crest;
        const f32 ridge = p.ridgeHeight * Smoothstep(-300.0f, -700.0f, z) * (0.6f + 0.8f * Ridged(x / 220.0f, z / 220.0f, s + 7, 3));

        f = h1 + massif + ridge;

        // Rock: high ground breaks into cliffs; the shore gets boulders. The shoreline itself stays at sea level.
        const f32 rockMask = Smoothstep(80.0f, 260.0f, f);
        f += rockMask * p.rockiness * 20.0f * (Ridged(x / 45.0f, z / 45.0f, s + 8, 4) - 0.35f);
        f += rockMask * p.rockiness * 3.0f * (Ridged(x / 7.0f, z / 7.0f, s + 12, 3) - 0.35f);
        f += (1.0f - Smoothstep(0.0f, 45.0f, d)) * 4.0f * p.rockiness * Ridged(x / 14.0f, z / 14.0f, s + 9, 3);

        // Building shelves: pull the slope toward flat steps, only where people would build.
        if (p.terracing > 0.0f && p.terraceHeight > 0.5f) {
            const f32 th = p.terraceHeight;
            const f32 q = f / th, fl = std::floor(q);
            const f32 stepped = th * (fl + Smoothstep(0.55f, 1.0f, q - fl));
            const f32 w = Smoothstep(2.0f, 14.0f, f) * (1.0f - Smoothstep(120.0f, 200.0f, f)) * (1.0f - rockMask);
            f = Mix(f, stepped, p.terracing * w);
        }

        // Detail at every scale a player can see: undulation, bumps, grain.
        const f32 inland = Smoothstep(0.0f, 22.0f, d);
        f += inland * p.roughness *
             (2.8f * Fbm(x / 70.0f, z / 70.0f, s + 10, 3) + 1.3f * Fbm(x / 19.0f, z / 19.0f, s + 11, 3) +
              0.35f * Fbm(x / 4.5f, z / 4.5f, s + 13, 2) + 0.08f * Fbm(x / 1.3f, z / 1.3f, s + 14, 2));
    }
    return p.seaLevel + floorRel + (f - floorRel) * mask;
}
} // namespace

f32 TerrainBaseHeight(const TerrainGenParams& p, f32 x, f32 z) { return ProceduralBaseHeight(p, x, z); }

f32 TerrainBaseHeight(const TerrainGenParams& p, const DemHeightmap* dem, f32 x, f32 z) {
    if (!dem || !p.useHeightmap) return ProceduralBaseHeight(p, x, z);
    const f32 edge = dem->edgeDistance(x, z);
    if (edge <= 0.0f) return p.floorHeight;
    const u32 s = p.seed * 7919u;
    f32 h = dem->sample(x, z); // metres above the sea
    // The source is 15 m cells made from 30 m data: add the detail a player can see, on land only (the beach and the flats
    // get a little, the mountains a lot).
    const f32 amp = p.demDetail * (0.15f + 0.85f * Smoothstep(5.0f, 60.0f, h)) * (1.0f + 1.5f * Smoothstep(80.0f, 400.0f, h)) * Smoothstep(0.6f, 4.0f, h);
    if (amp > 0.0f)
        h += amp * (2.8f * Fbm(x / 70.0f, z / 70.0f, s + 10, 3) + 1.3f * Fbm(x / 19.0f, z / 19.0f, s + 11, 3) + 0.35f * Fbm(x / 4.5f, z / 4.5f, s + 13, 2) +
                    0.08f * Fbm(x / 1.3f, z / 1.3f, s + 14, 2));
    const f32 floorRel = p.floorHeight - p.seaLevel;
    return p.seaLevel + floorRel + (h - floorRel) * Smoothstep(0.0f, std::max(p.demEdgeFade, 1.0f), edge);
}

// ---- Terrain -------------------------------------------------------------------------------------------------------

struct Terrain::Impl {
    struct TileBackup {
        u64 key;
        bool existed;
        std::vector<f32> d;
    };
    struct Step {
        std::vector<TileBackup> tiles;
    };

    mutable std::shared_mutex mutex;
    std::unordered_map<u64, std::vector<f32>> sculpt;    // key -> 65 x 65 deltas
    std::unordered_map<u64, std::vector<f32>> baseCache; // key -> 65 x 65 base heights (brush support only)
    std::vector<Step> undo, redo;
    Step stroke;
    std::unordered_set<u64> touched;
    bool inStroke = false;
    // Every dirty rect ever reported, newest last, with the absolute index of the first kept one: each consumer keeps its
    // own cursor (the terrain streamer and the vegetation both follow sculpting).
    std::shared_ptr<const DemHeightmap> dem;
    std::vector<TerrainDirtyRect> dirtyLog;
    u64 dirtyLogBase = 0;
    void pushDirty(const TerrainDirtyRect& r) {
        dirtyLog.push_back(r);
        if (dirtyLog.size() > 8192) {
            dirtyLog.erase(dirtyLog.begin(), dirtyLog.begin() + 4096);
            dirtyLogBase += 4096;
        }
    }

    // Sculpt delta at global sample (i, j), 0 where no tile exists. Caller holds the lock.
    f32 sculptSample(i32 i, i32 j) const {
        const i32 tx = FloorDiv(i, i32(kTerrainTileCells)), tz = FloorDiv(j, i32(kTerrainTileCells));
        const auto it = sculpt.find(TileKey(tx, tz));
        if (it == sculpt.end()) return 0.0f;
        return it->second[size_t(j - tz * i32(kTerrainTileCells)) * kTileSamples + size_t(i - tx * i32(kTerrainTileCells))];
    }
    f32 sculptBilinear(f32 x, f32 z) const {
        if (sculpt.empty()) return 0.0f;
        const f32 u = x / kTerrainCell, v = z / kTerrainCell;
        const f32 fu = std::floor(u), fv = std::floor(v);
        const i32 i = static_cast<i32>(fu), j = static_cast<i32>(fv);
        const f32 tu = u - fu, tv = v - fv;
        const f32 a = Mix(sculptSample(i, j), sculptSample(i + 1, j), tu);
        const f32 b = Mix(sculptSample(i, j + 1), sculptSample(i + 1, j + 1), tu);
        return Mix(a, b, tv);
    }
    bool anySculptIn(f32 minX, f32 minZ, f32 maxX, f32 maxZ) const {
        for (const auto& [key, d] : sculpt) {
            const f32 x0 = f32(KeyX(key)) * kTerrainTileSize, z0 = f32(KeyZ(key)) * kTerrainTileSize;
            if (x0 <= maxX && x0 + kTerrainTileSize >= minX && z0 <= maxZ && z0 + kTerrainTileSize >= minZ) return true;
        }
        return false;
    }

    void markTileDirty(u64 key) {
        const f32 x0 = f32(KeyX(key)) * kTerrainTileSize, z0 = f32(KeyZ(key)) * kTerrainTileSize;
        pushDirty({x0 - kTerrainCell, z0 - kTerrainCell, x0 + kTerrainTileSize + kTerrainCell, z0 + kTerrainTileSize + kTerrainCell});
    }
    void backupTile(u64 key) {
        if (!touched.insert(key).second) return;
        const auto it = sculpt.find(key);
        stroke.tiles.push_back({key, it != sculpt.end(), it != sculpt.end() ? it->second : std::vector<f32>{}});
    }
};

Terrain::Terrain() : m_impl(std::make_unique<Impl>()) {}
Terrain::~Terrain() = default;

void Terrain::setHeightmap(std::shared_ptr<const DemHeightmap> dem) {
    {
        std::unique_lock lock(m_impl->mutex);
        m_impl->dem = std::move(dem);
        m_impl->baseCache.clear();
    }
    ++m_generation;
    ++m_revision;
}

bool Terrain::loadHeightmap(const std::filesystem::path& file) {
    std::shared_ptr<const DemHeightmap> dem = DemHeightmap::load(file);
    if (!dem) return false;
    setHeightmap(std::move(dem));
    return true;
}

std::shared_ptr<const DemHeightmap> Terrain::heightmap() const {
    std::shared_lock lock(m_impl->mutex);
    return m_impl->dem;
}

void Terrain::setParams(const TerrainGenParams& params) {
    {
        std::unique_lock lock(m_impl->mutex);
        m_params = params;
        m_impl->baseCache.clear();
    }
    ++m_generation;
    ++m_revision;
}

f32 Terrain::baseHeight(f32 x, f32 z) const {
    TerrainGenParams p;
    std::shared_ptr<const DemHeightmap> dem;
    {
        std::shared_lock lock(m_impl->mutex);
        p = m_params;
        dem = m_impl->dem;
    }
    return TerrainBaseHeight(p, dem.get(), x, z);
}

f32 Terrain::height(f32 x, f32 z) const {
    f32 delta;
    TerrainGenParams p;
    std::shared_ptr<const DemHeightmap> dem;
    {
        std::shared_lock lock(m_impl->mutex);
        delta = m_impl->sculptBilinear(x, z);
        p = m_params; // setParams() writes it under the exclusive lock
        dem = m_impl->dem;
    }
    return TerrainBaseHeight(p, dem.get(), x, z) + delta;
}

f32 Terrain::sculptDelta(f32 x, f32 z) const {
    std::shared_lock lock(m_impl->mutex);
    return m_impl->sculptBilinear(x, z);
}

Vec3 Terrain::normal(f32 x, f32 z) const {
    const f32 c = kTerrainCell;
    const f32 dx = height(x - c, z) - height(x + c, z);
    const f32 dz = height(x, z - c) - height(x, z + c);
    return glm::normalize(Vec3(dx, 2.0f * c, dz));
}

bool Terrain::raycast(Vec3 o, Vec3 dir, f32 maxDist, Vec3& hit) const {
    const f32 len = glm::length(dir);
    if (len < 1e-6f) return false;
    dir /= len;
    auto above = [&](f32 t) {
        const Vec3 p = o + dir * t;
        return p.y - height(p.x, p.z);
    };
    // Sphere tracing: the surface is at least `gap / 2.5` of the ray away (slopes stay below ~2.5), so big steps are safe.
    f32 prev = 0.0f, t = 0.0f;
    if (above(0.0f) < 0.0f) return false; // starts underground
    for (int guard = 0; guard < 4096 && t < maxDist; ++guard) {
        const f32 gap = above(t);
        if (gap < 0.0f) {
            f32 a = prev, b = t;
            for (int it = 0; it < 16; ++it) {
                const f32 m = 0.5f * (a + b);
                (above(m) < 0.0f ? b : a) = m;
            }
            hit = o + dir * (0.5f * (a + b));
            return true;
        }
        prev = t;
        t += std::max(0.25f, gap * 0.35f);
    }
    return false;
}

void Terrain::sampleNodeHeights(const TerrainNodeKey& node, std::vector<f32>& out) const {
    out.assign(size_t(kNodeGrid) * kNodeGrid, 0.0f);
    TerrainGenParams p;
    std::shared_ptr<const DemHeightmap> dem;
    const Vec2 o = node.origin();
    const f32 cell = node.cell();
    const f32 span = node.size() + 2.0f * cell;
    {
        std::shared_lock lock(m_impl->mutex);
        p = m_params;
        dem = m_impl->dem;
        if (m_impl->anySculptIn(o.x - cell, o.y - cell, o.x - cell + span, o.y - cell + span)) {
            for (u32 j = 0; j < kNodeGrid; ++j)
                for (u32 i = 0; i < kNodeGrid; ++i)
                    out[size_t(j) * kNodeGrid + i] = m_impl->sculptBilinear(o.x + (f32(i) - 1.0f) * cell, o.y + (f32(j) - 1.0f) * cell);
        }
    }
    for (u32 j = 0; j < kNodeGrid; ++j)
        for (u32 i = 0; i < kNodeGrid; ++i)
            out[size_t(j) * kNodeGrid + i] += TerrainBaseHeight(p, dem.get(), o.x + (f32(i) - 1.0f) * cell, o.y + (f32(j) - 1.0f) * cell);
}

// ---- sculpting -----------------------------------------------------------------------------------------------------

void Terrain::beginStroke() {
    if (m_impl->inStroke) endStroke();
    m_impl->inStroke = true;
    m_impl->stroke = {};
    m_impl->touched.clear();
}

void Terrain::endStroke() {
    Impl& im = *m_impl;
    if (!im.inStroke) return;
    im.inStroke = false;
    if (im.stroke.tiles.empty()) return;
    im.undo.push_back(std::move(im.stroke));
    im.stroke = {};
    if (im.undo.size() > 64) im.undo.erase(im.undo.begin());
    im.redo.clear();
}

bool Terrain::canUndo() const { return !m_impl->undo.empty(); }
bool Terrain::canRedo() const { return !m_impl->redo.empty(); }
size_t Terrain::sculptTileCount() const {
    std::shared_lock lock(m_impl->mutex);
    return m_impl->sculpt.size();
}

void Terrain::applyBrush(const BrushStamp& st) {
    Impl& im = *m_impl;
    const bool lone = !im.inStroke; // a stamp outside beginStroke/endStroke is its own undo step
    if (lone) beginStroke();

    const f32 c = kTerrainCell;
    const f32 fi = st.center.x / c, fj = st.center.y / c;
    const f32 rs = st.radius / c;
    const i32 i0 = i32(std::floor(fi - rs)), i1 = i32(std::ceil(fi + rs));
    const i32 j0 = i32(std::floor(fj - rs)), j1 = i32(std::ceil(fj + rs));
    const i32 rw = i1 - i0 + 1, rh = j1 - j0 + 1;
    constexpr i32 kTile = i32(kTerrainTileCells);

    std::unique_lock lock(im.mutex);

    // Base heights of every tile the stamp (plus a 2 sample margin for smoothing) reads: computed once, cached.
    {
        std::vector<u64> missing;
        for (i32 tz = FloorDiv(j0 - 3, kTile); tz <= FloorDiv(j1 + 2, kTile); ++tz)
            for (i32 tx = FloorDiv(i0 - 3, kTile); tx <= FloorDiv(i1 + 2, kTile); ++tx)
                if (!im.baseCache.contains(TileKey(tx, tz))) missing.push_back(TileKey(tx, tz));
        std::vector<std::vector<f32>> filled(missing.size());
        const TerrainGenParams p = m_params;
        const std::shared_ptr<const DemHeightmap> dem = im.dem;
        ParallelFor(m_jobs, u32(missing.size()), 1, [&](u32 b, u32 e) {
            for (u32 k = b; k < e; ++k) {
                const f32 x0 = f32(KeyX(missing[k])) * kTerrainTileSize, z0 = f32(KeyZ(missing[k])) * kTerrainTileSize;
                filled[k].resize(kTileFloats);
                for (u32 j = 0; j < kTileSamples; ++j)
                    for (u32 i = 0; i < kTileSamples; ++i) filled[k][size_t(j) * kTileSamples + i] = TerrainBaseHeight(p, dem.get(), x0 + f32(i) * c, z0 + f32(j) * c);
            }
        });
        for (size_t k = 0; k < missing.size(); ++k) im.baseCache.emplace(missing[k], std::move(filled[k]));
    }
    auto baseAt = [&](i32 i, i32 j) {
        const i32 tx = FloorDiv(i, kTile), tz = FloorDiv(j, kTile);
        return im.baseCache.find(TileKey(tx, tz))->second[size_t(j - tz * kTile) * kTileSamples + size_t(i - tx * kTile)];
    };

    // Tiles whose samples are written (border samples belong to two or four tiles): back up, allocate.
    const i32 tx0 = FloorDiv(i0 - 1, kTile), tx1 = FloorDiv(i1, kTile), tz0 = FloorDiv(j0 - 1, kTile), tz1 = FloorDiv(j1, kTile);
    for (i32 tz = tz0; tz <= tz1; ++tz)
        for (i32 tx = tx0; tx <= tx1; ++tx) {
            const u64 key = TileKey(tx, tz);
            im.backupTile(key);
            auto it = im.sculpt.find(key);
            if (it == im.sculpt.end()) im.sculpt.emplace(key, std::vector<f32>(kTileFloats, 0.0f));
        }

    std::vector<f32> smoothed;
    if (st.mode == BrushMode::Smooth) {
        // Average of the final height over a 5 x 5 window, read before anything is written.
        smoothed.assign(size_t(rw) * rh, 0.0f);
        for (i32 j = j0; j <= j1; ++j)
            for (i32 i = i0; i <= i1; ++i) {
                f32 sum = 0.0f;
                for (i32 dj = -2; dj <= 2; ++dj)
                    for (i32 di = -2; di <= 2; ++di) sum += baseAt(i + di, j + dj) + im.sculptSample(i + di, j + dj);
                smoothed[size_t(j - j0) * rw + (i - i0)] = sum / 25.0f;
            }
    }

    const f32 hardness = std::clamp(st.hardness, 0.0f, 0.98f);
    std::vector<f32> next(size_t(rw) * rh);
    std::vector<u8> touchedSample(size_t(rw) * rh, 0);
    for (i32 j = j0; j <= j1; ++j)
        for (i32 i = i0; i <= i1; ++i) {
            const f32 dx = (f32(i) - fi) * c, dz = (f32(j) - fj) * c;
            const f32 r = std::sqrt(dx * dx + dz * dz) / st.radius;
            if (r >= 1.0f) continue;
            const f32 t = std::clamp((1.0f - r) / (1.0f - hardness), 0.0f, 1.0f);
            const f32 w = t * t * (3.0f - 2.0f * t);
            const f32 delta = im.sculptSample(i, j);
            const f32 cur = baseAt(i, j) + delta;
            const f32 k = std::min(1.0f, st.strength * 8.0f * st.dt * w);
            f32 nd = delta;
            switch (st.mode) {
            case BrushMode::Raise: nd += st.strength * 10.0f * st.dt * w; break;
            case BrushMode::Lower: nd -= st.strength * 10.0f * st.dt * w; break;
            case BrushMode::Smooth: nd += (smoothed[size_t(j - j0) * rw + (i - i0)] - cur) * k; break;
            case BrushMode::Flatten: nd += (st.flattenHeight - cur) * k; break;
            case BrushMode::Noise:
                nd += Fbm(f32(i) * c / st.noiseScale, f32(j) * c / st.noiseScale, st.seed, 3) * st.strength * 6.0f * st.dt * w;
                break;
            case BrushMode::Erase: nd -= delta * k; break;
            }
            next[size_t(j - j0) * rw + (i - i0)] = nd;
            touchedSample[size_t(j - j0) * rw + (i - i0)] = 1;
        }

    // Write every tile copy of each sample (they must stay identical).
    for (i32 j = j0; j <= j1; ++j)
        for (i32 i = i0; i <= i1; ++i) {
            const size_t k = size_t(j - j0) * rw + (i - i0);
            if (!touchedSample[k]) continue;
            const i32 txa = FloorDiv(i, kTile), tza = FloorDiv(j, kTile);
            const i32 txn = (i - txa * kTile == 0) ? 2 : 1, tzn = (j - tza * kTile == 0) ? 2 : 1;
            for (i32 dz = 0; dz < tzn; ++dz)
                for (i32 dxx = 0; dxx < txn; ++dxx) {
                    const i32 tx = txa - dxx, tz = tza - dz;
                    im.sculpt[TileKey(tx, tz)][size_t(j - tz * kTile) * kTileSamples + size_t(i - tx * kTile)] = next[k];
                }
        }

    im.pushDirty({f32(i0 - 1) * c, f32(j0 - 1) * c, f32(i1 + 1) * c, f32(j1 + 1) * c});
    ++m_revision;
    lock.unlock();
    if (lone) endStroke();
}

void Terrain::editRect(Vec2 minCorner, Vec2 maxCorner, const HeightEdit& newHeight) {
    Impl& im = *m_impl;
    const bool lone = !im.inStroke;
    if (lone) beginStroke();

    const f32 c = kTerrainCell;
    const i32 i0 = i32(std::floor(minCorner.x / c)), i1 = i32(std::ceil(maxCorner.x / c));
    const i32 j0 = i32(std::floor(minCorner.y / c)), j1 = i32(std::ceil(maxCorner.y / c));
    const i32 rw = i1 - i0 + 1, rh = j1 - j0 + 1;
    constexpr i32 kTile = i32(kTerrainTileCells);

    std::unique_lock lock(im.mutex);
    {
        std::vector<u64> missing;
        for (i32 tz = FloorDiv(j0 - 1, kTile); tz <= FloorDiv(j1, kTile); ++tz)
            for (i32 tx = FloorDiv(i0 - 1, kTile); tx <= FloorDiv(i1, kTile); ++tx)
                if (!im.baseCache.contains(TileKey(tx, tz))) missing.push_back(TileKey(tx, tz));
        std::vector<std::vector<f32>> filled(missing.size());
        const TerrainGenParams p = m_params;
        const std::shared_ptr<const DemHeightmap> dem = im.dem;
        ParallelFor(m_jobs, u32(missing.size()), 1, [&](u32 b, u32 e) {
            for (u32 k = b; k < e; ++k) {
                const f32 x0 = f32(KeyX(missing[k])) * kTerrainTileSize, z0 = f32(KeyZ(missing[k])) * kTerrainTileSize;
                filled[k].resize(kTileFloats);
                for (u32 j = 0; j < kTileSamples; ++j)
                    for (u32 i = 0; i < kTileSamples; ++i) filled[k][size_t(j) * kTileSamples + i] = TerrainBaseHeight(p, dem.get(), x0 + f32(i) * c, z0 + f32(j) * c);
            }
        });
        for (size_t k = 0; k < missing.size(); ++k) im.baseCache.emplace(missing[k], std::move(filled[k]));
    }
    auto baseAt = [&](i32 i, i32 j) {
        const i32 tx = FloorDiv(i, kTile), tz = FloorDiv(j, kTile);
        return im.baseCache.find(TileKey(tx, tz))->second[size_t(j - tz * kTile) * kTileSamples + size_t(i - tx * kTile)];
    };
    const i32 tx0 = FloorDiv(i0 - 1, kTile), tx1 = FloorDiv(i1, kTile), tz0 = FloorDiv(j0 - 1, kTile), tz1 = FloorDiv(j1, kTile);
    for (i32 tz = tz0; tz <= tz1; ++tz)
        for (i32 tx = tx0; tx <= tx1; ++tx) {
            const u64 key = TileKey(tx, tz);
            im.backupTile(key);
            if (!im.sculpt.contains(key)) im.sculpt.emplace(key, std::vector<f32>(kTileFloats, 0.0f));
        }
    std::vector<f32> next(size_t(rw) * rh);
    for (i32 j = j0; j <= j1; ++j)
        for (i32 i = i0; i <= i1; ++i) {
            const f32 base = baseAt(i, j), delta = im.sculptSample(i, j);
            const f32 target = newHeight(f32(i) * c, f32(j) * c, base + delta);
            next[size_t(j - j0) * rw + (i - i0)] = target - base;
        }
    for (i32 j = j0; j <= j1; ++j)
        for (i32 i = i0; i <= i1; ++i) {
            const size_t k = size_t(j - j0) * rw + (i - i0);
            const i32 txa = FloorDiv(i, kTile), tza = FloorDiv(j, kTile);
            const i32 txn = (i - txa * kTile == 0) ? 2 : 1, tzn = (j - tza * kTile == 0) ? 2 : 1;
            for (i32 dz = 0; dz < tzn; ++dz)
                for (i32 dxx = 0; dxx < txn; ++dxx) {
                    const i32 tx = txa - dxx, tz = tza - dz;
                    im.sculpt[TileKey(tx, tz)][size_t(j - tz * kTile) * kTileSamples + size_t(i - tx * kTile)] = next[k];
                }
        }
    im.pushDirty({f32(i0 - 1) * c, f32(j0 - 1) * c, f32(i1 + 1) * c, f32(j1 + 1) * c});
    ++m_revision;
    lock.unlock();
    if (lone) endStroke();
}

void Terrain::undo() {
    Impl& im = *m_impl;
    if (im.inStroke) endStroke();
    if (im.undo.empty()) return;
    Impl::Step step = std::move(im.undo.back());
    im.undo.pop_back();
    Impl::Step inverse;
    {
        std::unique_lock lock(im.mutex);
        // Capture every tile before writing any (a tile restore must not leak into another tile's capture).
        for (const Impl::TileBackup& b : step.tiles) {
            const auto it = im.sculpt.find(b.key);
            inverse.tiles.push_back({b.key, it != im.sculpt.end(), it != im.sculpt.end() ? it->second : std::vector<f32>{}});
        }
        for (Impl::TileBackup& b : step.tiles) {
            if (b.existed) im.sculpt[b.key] = std::move(b.d);
            else im.sculpt.erase(b.key);
            im.markTileDirty(b.key);
        }
    }
    im.redo.push_back(std::move(inverse));
    ++m_revision;
}

void Terrain::redo() {
    Impl& im = *m_impl;
    if (im.redo.empty()) return;
    Impl::Step step = std::move(im.redo.back());
    im.redo.pop_back();
    Impl::Step inverse;
    {
        std::unique_lock lock(im.mutex);
        for (const Impl::TileBackup& b : step.tiles) {
            const auto it = im.sculpt.find(b.key);
            inverse.tiles.push_back({b.key, it != im.sculpt.end(), it != im.sculpt.end() ? it->second : std::vector<f32>{}});
        }
        for (Impl::TileBackup& b : step.tiles) {
            if (b.existed) im.sculpt[b.key] = std::move(b.d);
            else im.sculpt.erase(b.key);
            im.markTileDirty(b.key);
        }
    }
    im.undo.push_back(std::move(inverse));
    ++m_revision;
}

void Terrain::clearSculpt() {
    Impl& im = *m_impl;
    beginStroke();
    {
        std::unique_lock lock(im.mutex);
        for (const auto& [key, d] : im.sculpt) {
            im.backupTile(key);
            im.markTileDirty(key);
        }
        im.sculpt.clear();
    }
    endStroke();
    ++m_revision;
}

std::vector<TerrainDirtyRect> Terrain::dirtyRectsSince(u64& cursor) const {
    const Impl& im = *m_impl;
    std::vector<TerrainDirtyRect> out;
    const u64 end = im.dirtyLogBase + im.dirtyLog.size();
    const u64 from = std::max(cursor, im.dirtyLogBase);
    for (u64 i = from; i < end; ++i) out.push_back(im.dirtyLog[size_t(i - im.dirtyLogBase)]);
    cursor = end;
    return out;
}

u64 Terrain::dirtyCursor() const { return m_impl->dirtyLogBase + m_impl->dirtyLog.size(); }

// ---- persistence ---------------------------------------------------------------------------------------------------
// File: "FOHT", version 2, sizeof(TerrainGenParams) + params, tile count, then per tile its key and 65 x 65 floats.
// Trailing params fields missing in an older file keep their defaults. Version 1 (fixed grid) is not read.

namespace {
constexpr u32 kMagic = 0x54484F46u; // "FOHT"
constexpr u32 kVersion = 2;
template <class T>
void Put(std::ofstream& f, const T& v) {
    f.write(reinterpret_cast<const char*>(&v), sizeof(T));
}
template <class T>
bool Get(std::ifstream& f, T& v) {
    f.read(reinterpret_cast<char*>(&v), sizeof(T));
    return bool(f);
}
} // namespace

bool Terrain::save(const std::filesystem::path& file) const {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path tmp = file.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        std::shared_lock lock(m_impl->mutex);
        Put(f, kMagic);
        Put(f, kVersion);
        Put(f, static_cast<u32>(sizeof(TerrainGenParams)));
        Put(f, m_params);
        Put(f, static_cast<u32>(m_impl->sculpt.size()));
        for (const auto& [key, d] : m_impl->sculpt) {
            Put(f, key);
            f.write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size() * sizeof(f32)));
        }
        if (!f) return false;
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        std::filesystem::remove(file, ec);
        std::filesystem::rename(tmp, file, ec);
    }
    return !ec;
}

bool Terrain::load(const std::filesystem::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    u32 magic = 0, version = 0, paramBytes = 0;
    if (!Get(f, magic) || magic != kMagic || !Get(f, version) || !Get(f, paramBytes) || paramBytes > 4096) return false;
    if (version != kVersion) {
        FOH_WARN("terrain: {} is version {} (expected {}), ignored", file.string(), version, kVersion);
        return false;
    }
    TerrainGenParams params;
    std::vector<char> raw(paramBytes);
    f.read(raw.data(), paramBytes);
    std::memcpy(&params, raw.data(), std::min<size_t>(paramBytes, sizeof(params)));
    u32 count = 0;
    if (!f || !Get(f, count) || count > (1u << 22)) return false;
    std::unordered_map<u64, std::vector<f32>> tiles;
    for (u32 n = 0; n < count; ++n) {
        u64 key = 0;
        if (!Get(f, key)) return false;
        std::vector<f32> d(kTileFloats);
        f.read(reinterpret_cast<char*>(d.data()), std::streamsize(kTileFloats * sizeof(f32)));
        if (!f) return false;
        tiles.emplace(key, std::move(d));
    }
    {
        std::unique_lock lock(m_impl->mutex);
        m_impl->sculpt = std::move(tiles);
        m_impl->baseCache.clear();
        m_impl->undo.clear();
        m_impl->redo.clear();
        m_params = params;
    }
    ++m_generation;
    ++m_revision;
    return true;
}

} // namespace foh
