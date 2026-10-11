#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include "city_internal.h"
#include "foh/log.h"

namespace foh {

namespace {
struct Reader {
    std::ifstream f;
    bool ok = true;
    template <class T>
    T get() {
        T v{};
        f.read(reinterpret_cast<char*>(&v), sizeof(T));
        if (!f) ok = false;
        return v;
    }
    Vec2 vec() {
        const f32 x = get<f32>(), z = get<f32>();
        return Vec2(x, z);
    }
};
} // namespace

bool CityData::load(const std::filesystem::path& file) {
    Reader r;
    r.f.open(file, std::ios::binary);
    if (!r.f) return false;
    char magic[4];
    r.f.read(magic, 4);
    if (!r.f || std::memcmp(magic, "FOHC", 4) != 0 || r.get<u32>() != 1) {
        FOH_ERROR("city: {} is not a valid FOHC v1 file", file.string());
        return false;
    }
    r.get<f64>();
    r.get<f64>(); // lat0, lon0 (the world origin is Vidigal; the coordinates are already metres)
    CityData d;
    auto count = [&](u32 limit) {
        const u32 n = r.get<u32>();
        if (n > limit) r.ok = false;
        return r.ok ? n : 0u;
    };
    d.nodes.resize(count(1u << 20));
    for (Vec2& n : d.nodes) n = r.vec();
    d.edges.resize(count(1u << 20));
    for (CityEdge& e : d.edges) {
        e.a = r.get<u32>();
        e.b = r.get<u32>();
        e.cls = static_cast<RoadClass>(r.get<u8>());
        e.lanes = r.get<u8>();
        e.flags = r.get<u8>();
        r.get<u8>();
        e.width = r.get<f32>();
        e.sidewalkL = r.get<f32>();
        e.sidewalkR = r.get<f32>();
        e.name = r.get<u16>();
        e.pts.resize(r.get<u16>());
        for (Vec2& p : e.pts) p = r.vec();
        if (e.a >= d.nodes.size() || e.b >= d.nodes.size() || e.pts.size() < 2) r.ok = false;
        if (!r.ok) break;
    }
    d.names.resize(count(1u << 20));
    for (std::string& s : d.names) {
        s.resize(r.get<u16>());
        r.f.read(s.data(), std::streamsize(s.size()));
    }
    d.buildings.resize(count(1u << 21));
    for (CityBuilding& b : d.buildings) {
        b.kind = static_cast<BuildingKind>(r.get<u8>());
        b.levels = r.get<u8>();
        b.flags = r.get<u8>();
        b.rings.resize(r.get<u8>());
        b.height = r.get<f32>();
        b.baseZ = r.get<f32>();
        b.osmId = r.get<u32>();
        for (auto& ring : b.rings) {
            ring.resize(r.get<u16>());
            for (Vec2& p : ring) p = r.vec();
        }
        if (b.rings.empty() || b.rings[0].size() < 3) r.ok = false;
        if (!r.ok) break;
        b.lo = b.hi = b.rings[0][0];
        for (const Vec2& p : b.rings[0]) {
            b.lo = glm::min(b.lo, p);
            b.hi = glm::max(b.hi, p);
        }
    }
    d.areas.resize(count(1u << 20));
    for (CityArea& a : d.areas) {
        a.kind = static_cast<AreaKind>(r.get<u8>());
        a.rings.resize(r.get<u8>());
        for (auto& ring : a.rings) {
            ring.resize(r.get<u16>());
            for (Vec2& p : ring) p = r.vec();
        }
        if (!r.ok) break;
    }
    d.points.resize(count(1u << 22));
    for (CityPoint& p : d.points) {
        p.kind = static_cast<PointKind>(r.get<u8>());
        p.p = r.vec();
        p.value = r.get<f32>();
        p.extra = r.get<u32>();
    }
    if (!r.ok) {
        FOH_ERROR("city: {} is truncated or inconsistent", file.string());
        return false;
    }
    *this = std::move(d);
    FOH_INFO("city: {} nodes, {} streets, {} buildings, {} areas, {} points", nodes.size(), edges.size(), buildings.size(), areas.size(), points.size());
    return true;
}

namespace city {

void BuildIndex(CityIndex& index) {
    const CityData& data = *index.data;
    index.buildingsByChunk.clear();
    index.buildingGrid.clear();
    index.pointsByChunk.clear();
    index.beachAreas.clear();
    // Buildings: chunk by centre, and the 32 m grid by bounding box.
    for (u32 bi = 0; bi < data.buildings.size(); ++bi) {
        const CityBuilding& b = data.buildings[bi];
        const Vec2 c = (b.lo + b.hi) * 0.5f;
        index.buildingsByChunk[ChunkKey(ChunkOf(c.x), ChunkOf(c.y))].push_back(bi);
        for (i32 gz = CityIndex::GridOf(b.lo.y); gz <= CityIndex::GridOf(b.hi.y); ++gz)
            for (i32 gx = CityIndex::GridOf(b.lo.x); gx <= CityIndex::GridOf(b.hi.x); ++gx) index.buildingGrid[CityIndex::GridKey(gx, gz)].push_back(bi);
    }
    for (u32 ai = 0; ai < data.areas.size(); ++ai)
        if (data.areas[ai].kind == AreaKind::Beach && !data.areas[ai].rings.empty()) index.beachAreas.push_back(ai);
    for (u32 pi = 0; pi < data.points.size(); ++pi) {
        const Vec2 p = data.points[pi].p;
        index.pointsByChunk[ChunkKey(ChunkOf(p.x), ChunkOf(p.y))].push_back(pi);
    }
    BuildRoadIndex(index);
}

f32 SignedArea(const std::vector<Vec2>& ring) {
    f32 a = 0.0f;
    for (size_t i = 0; i < ring.size(); ++i) a += Cross(ring[i], ring[(i + 1) % ring.size()]);
    return 0.5f * a;
}

bool InsideRings(const std::vector<std::vector<Vec2>>& rings, Vec2 p) {
    bool inside = false;
    for (const auto& ring : rings) {
        const size_t n = ring.size();
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            const Vec2 a = ring[i], b = ring[j];
            if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y + 1e-12f) + a.x) inside = !inside;
        }
    }
    return inside; // even-odd: holes toggle back
}

f32 CityIndex::distanceToStreet(Vec2 p, f32 maxDist) const {
    f32 best = maxDist;
    const i32 cx = GridOf(p.x), cz = GridOf(p.y);
    const i32 span = std::max(1, i32(std::ceil(maxDist / kGrid)));
    for (i32 dz = -span; dz <= span; ++dz)
        for (i32 dx = -span; dx <= span; ++dx) {
            const auto it = roadGrid.find(GridKey(cx + dx, cz + dz));
            if (it == roadGrid.end()) continue;
            for (const RoadSample& s : it->second) best = std::min(best, std::max(0.0f, glm::length(p - s.p) - s.reach));
        }
    return best;
}

} // namespace city
} // namespace foh

namespace foh {

Vec2 PushOutOfFootprint(const std::vector<std::vector<Vec2>>& rings, Vec2 centre, f32 radius) {
    if (rings.empty()) return centre;
    f32 best = 1e9f;
    Vec2 bestPoint(0.0f);
    for (const auto& ring : rings)
        for (size_t i = 0; i < ring.size(); ++i) {
            const Vec2 a = ring[i], c = ring[(i + 1) % ring.size()], ab = c - a;
            const f32 t = std::clamp(glm::dot(centre - a, ab) / std::max(glm::dot(ab, ab), 1e-6f), 0.0f, 1.0f);
            const Vec2 q = a + ab * t;
            const f32 d = glm::length(centre - q);
            if (d < best) {
                best = d;
                bestPoint = q;
            }
        }
    const bool inside = city::InsideRings(rings, centre);
    if (!inside && best >= radius) return centre;
    Vec2 away = centre - bestPoint;
    const f32 len = glm::length(away);
    away = len > 1e-4f ? away / len : Vec2(1, 0);
    return inside ? bestPoint - away * radius : bestPoint + away * radius; // inside: out through the nearest wall
}

CityChunkReport BuildCityChunkReport(const CityData& data, const Terrain& terrain, const CitySettings& settings, i32 chunkX, i32 chunkZ, u32 lod) {
    city::CityIndex index;
    index.data = &data;
    city::BuildIndex(index);
    city::ChunkParts parts;
    if (settings.roads) city::BuildRoadChunk(index, terrain, chunkX, chunkZ, lod, parts);
    if (settings.buildings) city::BuildBuildingChunk(index, terrain, settings, chunkX, chunkZ, lod, parts);
    if (settings.props && lod == 0) city::BuildPropChunk(index, terrain, settings, chunkX, chunkZ, parts);
    CityChunkReport r;
    r.parts = u32(parts.parts.size());
    r.buildings = parts.buildings;
    for (const auto& [slot, m] : parts.parts) {
        r.triangles += u32(m.idx.size() / 3);
        for (const Vec3& p : m.p) {
            r.finite = r.finite && std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
            r.minY = std::min(r.minY, p.y);
            r.maxY = std::max(r.maxY, p.y);
        }
        for (const u32 i : m.idx) r.indicesValid = r.indicesValid && i < m.p.size();
        r.indicesValid = r.indicesValid && m.idx.size() % 3 == 0 && m.n.size() == m.p.size() && m.uv.size() == m.p.size() && m.t.size() == m.p.size();
    }
    for (const auto& b : parts.props) r.propInstances += u32(b.instances.size());
    return r;
}

} // namespace foh
