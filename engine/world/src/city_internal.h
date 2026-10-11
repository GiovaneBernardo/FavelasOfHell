#pragma once

// Private to engine/world: what the district generators share (ADR 0014). Chunk generation runs on jobs, so everything
// here is plain data, built once at load and then read-only.

#include <array>
#include <cmath>
#include <deque>
#include <unordered_map>
#include <vector>

#include "foh/asset_id.h"
#include "foh/math.h"
#include "foh/renderer/render_scene.h"
#include "foh/world/city.h"
#include "foh/world/terrain.h"

namespace foh::city {

inline constexpr u64 kHi = 0x7E44A1A5ull;
inline constexpr f32 kChunk = 128.0f;

inline i32 ChunkOf(f32 v) { return i32(std::floor(v / kChunk)); }
inline u64 ChunkKey(i32 x, i32 z) { return (u64(static_cast<u32>(x)) << 32) | u64(static_cast<u32>(z)); }

// Indexed triangle mesh with the attributes the renderer wants.
struct MeshData {
    std::vector<Vec3> p, n;
    std::vector<Vec2> uv;
    std::vector<Vec4> t;
    std::vector<u32> idx;
    bool empty() const { return idx.empty(); }
    u32 vert(Vec3 pos, Vec3 nrm, Vec2 uvs, Vec4 tan = Vec4(1, 0, 0, 1)) {
        p.push_back(pos);
        n.push_back(nrm);
        uv.push_back(uvs);
        t.push_back(tan);
        return static_cast<u32>(p.size() - 1);
    }
    // A triangle that faces `facing` (the winding is fixed to match, so callers cannot get it wrong).
    void tri(u32 a, u32 b, u32 c, const Vec3& facing) {
        const Vec3 cr = glm::cross(p[b] - p[a], p[c] - p[a]);
        if (glm::dot(cr, facing) < 0.0f) std::swap(b, c);
        idx.push_back(a);
        idx.push_back(b);
        idx.push_back(c);
    }
    void quad(u32 a, u32 b, u32 c, u32 d, const Vec3& facing) { // a b c d around the quad
        tri(a, b, c, facing);
        tri(a, c, d, facing);
    }
};

// Materials. Slots index a table; the id of slot k is MaterialId(k).
enum Slot : u32 {
    kAsphalt,
    kSidewalk,
    kPromenade,
    kCycle,
    kMarking,
    kMarkingY,
    kFacade0, // kFacadeStyles * kPalettes
    kShop0 = kFacade0 + 3 * 5,
    kBlank0 = kShop0 + 4,
    kRoofTile = kBlank0 + 5,
    kRoofSlab,
    kFoundation,
    kMetal,
    kLampHead,
    kPaintWhite,
    kPaintRed,
    kGlassDark,
    kWood,
    kPlastic,
    kWire,
    kCarPaint,
    kRubber,
    kPaintBlue,
    kGutter,
    kManhole,
    kDrain,
    kSlotCount
};
inline constexpr u32 kFacadeStyles = 3, kPalettes = 5, kShopVariants = 4;
inline AssetId MaterialId(u32 slot) { return AssetId{kHi, 0x1000ull + slot}; }
inline AssetId TextureId(u32 k) { return AssetId{kHi, 0x2000ull + k}; }

// Props are shared meshes drawn as instances: each is a list of (mesh part, material).
enum class Prop : u32 { LampPole, LampHead, TrafficPole, TrafficHead, ShelterFrame, ShelterRoof, Bench, Bin, PostoTower, PostoRoof, UmbrellaPole, UmbrellaCanopy, KioskBody, KioskRoof, NetPost, NetBand, UtilityPole,
                        CarHatchBody, CarHatchGlass, CarHatchWheels, CarSedanBody, CarSedanGlass, CarSedanWheels, CarSuvBody, CarSuvGlass, CarSuvWheels,
                        Hydrant, SignPost, SignPlate, SignDiscRed, SignDiscBlue, Bollard, AdPanel, AdFace, Count };
inline AssetId PropMesh(Prop p) { return AssetId{kHi, 0x3000ull + static_cast<u64>(p)}; }
inline AssetId ChunkMesh(u32 serial) { return AssetId{kHi, 0x10000ull + serial}; }

struct PropBatch {
    Prop prop;
    u32 slot; // material
    std::vector<StaticInstance> instances;
};

struct ChunkParts {
    std::deque<std::pair<u32, MeshData>> parts; // (material slot, mesh); a deque so a reference to one mesh survives adding another
    std::vector<PropBatch> props;
    f32 ms = 0.0f;
    u32 buildings = 0;
    u32 lod = 0;
};

// ---- the street graph geometry, derived once ----

struct EdgeGeo {
    std::vector<f32> cum; // arc length at every vertex
    f32 length = 0.0f;
    // Distance cut off each end: the asphalt (both sides) and the sidewalk of the left / right side. 0 = reaches the node.
    f32 tA = 0.0f, tB = 0.0f, lA = 0.0f, lB = 0.0f, rA = 0.0f, rB = 0.0f;
    bool valid = true;
    Vec2 lo{0.0f}, hi{0.0f};
};

struct Junction {
    u32 node = 0;
    Vec2 center{0.0f};
    std::vector<Vec2> asphalt;                 // the polygon, in order
    std::vector<std::vector<Vec2>> sidewalks;  // corner patches: curb arc, outer corner
    std::vector<std::array<Vec2, 2>> curbs;    // curb segments (a, b), the sidewalk is on the right of a -> b
    f32 curb = 0.15f;
    bool pavedOnly = false;                    // a pedestrian junction: paving instead of asphalt
    f32 sidewalk = 2.5f;
    bool promenade = false;
};

struct Footprint {
    u32 building;
    Vec2 lo, hi;
};

struct CityIndex {
    const CityData* data = nullptr;
    std::vector<EdgeGeo> edgeGeo;
    std::vector<Junction> junctions;
    std::unordered_map<u64, std::vector<u32>> edgesByChunk, buildingsByChunk, junctionsByChunk, pointsByChunk;
    // 32 m grid of building bounding boxes (collision, party walls, street facing) and of street centre samples.
    static constexpr f32 kGrid = 32.0f;
    std::unordered_map<u64, std::vector<u32>> buildingGrid;
    struct RoadSample {
        Vec2 p;
        f32 reach; // half width + sidewalk
        u32 edge;
    };
    std::unordered_map<u64, std::vector<RoadSample>> roadGrid;
    std::vector<u32> beachAreas; // indices of the beach polygons
    std::vector<f32> nodeRoadHw; // per node: the widest half carriageway of the streets meeting there (0 = none): paths stop at its kerb

    static u64 GridKey(i32 x, i32 z) { return ChunkKey(x, z); }
    static i32 GridOf(f32 v) { return i32(std::floor(v / kGrid)); }
    // Distance from `p` to the nearest street's edge of the carriageway + sidewalk (0 = on it); large when none within `maxDist`.
    f32 distanceToStreet(Vec2 p, f32 maxDist) const;
};

// Point in a polygon with holes (ring 0 outer).
bool InsideRings(const std::vector<std::vector<Vec2>>& rings, Vec2 p);
f32 SignedArea(const std::vector<Vec2>& ring);
inline f32 Cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
inline Vec2 RightOf(Vec2 d) { return Vec2(-d.y, d.x); }

// Builders (each in its own file).
void BuildRoadIndex(CityIndex& index);
// lod 0: full detail; lod 1: the far version (asphalt only / walls and roofs only)
void BuildRoadChunk(const CityIndex& index, const Terrain& terrain, i32 cx, i32 cz, u32 lod, ChunkParts& out);
void BuildBuildingChunk(const CityIndex& index, const Terrain& terrain, const CitySettings& settings, i32 cx, i32 cz, u32 lod, ChunkParts& out);
void BuildPropChunk(const CityIndex& index, const Terrain& terrain, const CitySettings& settings, i32 cx, i32 cz, ChunkParts& out);

// Content: procedural textures and prop meshes (city_assets.cpp), uploaded once when the district loads.
void UploadCityContent(Renderer& renderer);
void ReleaseCityContent(Renderer& renderer);

// Triangulate(): see city.h.
void BuildIndex(CityIndex& index);

} // namespace foh::city
