#pragma once

// A district of a real city on the terrain (ADR 0014): streets, buildings, street furniture and the beach, generated from
// the data files tools/build_district.py makes (street graph with widths, building footprints with heights and ground
// levels, parks, points). Nothing here is an entity: every 128 m chunk is a handful of merged meshes drawn as retained
// StaticInstanceSets (culled per viewport and shadow cascade, like the vegetation), built on the job system around the
// camera and removed with hysteresis behind it.
//
// What is generated, and from what:
//   streets     asphalt with lane markings and zebra crossings, curbed sidewalks, the beach promenade, cycle paths, real
//               junction corners (each node's incident streets are cut back to the corner where their kerbs meet)
//   buildings   one mesh per footprint: ground floor, repeated storeys, parapet, roof and its machinery, a foundation down to
//               the lowest ground under it, and party walls where a taller neighbour shares the wall. Facade textures are
//               generated in code (stand-ins an artist can replace), so one repeat of a texture is one bay of one storey
//   props       street lamps, traffic lights, bus shelters, benches, bins, lifeguard posts, beach umbrellas, kiosks
//   plants      the trees of the survey plus street trees and park trees, handed to the Vegetation as explicit plants
//
// Thread-safety: main thread only (it talks to the Renderer); chunk generation runs on jobs and only reads immutable data and
// the Terrain.

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "foh/ecs.h"
#include "foh/jobs.h"
#include "foh/math.h"
#include "foh/platform.h"
#include "foh/renderer/renderer.h"
#include "foh/types.h"
#include "foh/world/ground_patch.h"
#include "foh/world/terrain.h"
#include "foh/world/vegetation.h"

namespace foh {

enum class RoadClass : u8 { Trunk, Primary, Secondary, Tertiary, Residential, Living, Service, Pedestrian, Cycleway, Footway, Steps };

// The edge flags written by build_district.py.
inline constexpr u8 kEdgeOneway = 1, kEdgeLit = 2, kEdgeBridge = 4, kEdgePromenade = 8;

// A street between two junctions. The polyline runs from node `a` to node `b`; "right" of the direction of travel is
// (-dz, dx) in (x, z), "left" is (dz, -dx).
struct CityEdge {
    u32 a = 0, b = 0;
    RoadClass cls = RoadClass::Residential;
    u8 lanes = 2, flags = 0;
    f32 width = 7.0f;                 // m of asphalt
    f32 sidewalkL = 2.5f, sidewalkR = 2.5f; // m
    u16 name = 0;
    std::vector<Vec2> pts;
};

enum class BuildingKind : u8 { Apartments, House, Commercial, Hotel, Civic, Roof, Shed, Construction };

struct CityBuilding {
    BuildingKind kind = BuildingKind::Apartments;
    u8 levels = 3;
    u8 flags = 0; // 1: the height comes from the survey
    f32 height = 10.0f; // m from the ground floor level to the roof
    f32 baseZ = 0.0f;   // m: ground floor level
    u32 osmId = 0;
    std::vector<std::vector<Vec2>> rings; // 0 = outer (area > 0 in x, z), the rest are courtyards
    Vec2 lo{0.0f}, hi{0.0f};
};

enum class AreaKind : u8 { Park, Garden, Pitch, Pool, Beach, Water, Wood, Scrub, Rock, Grass };
struct CityArea {
    AreaKind kind = AreaKind::Park;
    std::vector<std::vector<Vec2>> rings;
};

enum class PointKind : u8 { Tree, Lamp, BusStop, Bench, Waste, Toilets, Signals, Crossing, Lifeguard, Water };
struct CityPoint {
    PointKind kind = PointKind::Tree;
    Vec2 p{0.0f};
    f32 value = 0.0f;
    u32 extra = 0;
};

struct FOH_API CityData {
    std::vector<Vec2> nodes;
    std::vector<CityEdge> edges;
    std::vector<std::string> names;
    std::vector<CityBuilding> buildings;
    std::vector<CityArea> areas;
    std::vector<CityPoint> points;
    bool load(const std::filesystem::path& file); // "FOHC" v1
};

struct CitySettings {
    bool roads = true;
    bool buildings = true;
    bool props = true;
    f32 range = 450.0f;       // m: chunks this close to the focus are built in full detail (facades, sidewalks, markings, props)
    f32 farRange = 1800.0f;   // m: out to here chunks are built simplified (walls and roofs, asphalt), one draw per material, no shadows
    f32 heightScale = 1.0f;   // multiplies every building's height (a quick way to test a skyline)
    u32 seed = 1;             // facades, roofs and props vary with it; the same seed builds the same city
};

struct CityStats {
    u32 chunks = 0, waiting = 0;
    u64 triangles = 0;
    u32 buildings = 0, parts = 0;
    f32 lastChunkMs = 0.0f;
};

// ---- pieces that are useful (and testable) without a renderer ----

// Moves a vertical circle out of a footprint (ring 0 outer, the rest courtyards): the corrected centre, or the input when it
// touches nothing. A centre inside the footprint is pushed out through the nearest wall.
FOH_API Vec2 PushOutOfFootprint(const std::vector<std::vector<Vec2>>& rings, Vec2 centre, f32 radius);

namespace city {
// Ear-clipping triangulation of a polygon with holes (ring 0 outer, then the holes): `verts` is the outline after bridging the
// holes into it, `tris` indexes it (three per triangle).
FOH_API void Triangulate(const std::vector<std::vector<Vec2>>& rings, std::vector<Vec2>& verts, std::vector<u32>& tris);
} // namespace city

// What one chunk builds, checked: the generators run on the CPU only, so this is how tests and tools look at the geometry.
struct CityChunkReport {
    u32 parts = 0, triangles = 0, buildings = 0, propInstances = 0;
    f32 minY = 1e9f, maxY = -1e9f;
    bool finite = true, indicesValid = true;
};
FOH_API CityChunkReport BuildCityChunkReport(const CityData& data, const Terrain& terrain, const CitySettings& settings, i32 chunkX, i32 chunkZ, u32 lod);

class FOH_API City {
public:
    City(Renderer& renderer, World& world, JobSystem* jobs);
    ~City();
    City(const City&) = delete;
    City& operator=(const City&) = delete;

    // Reads the district and uploads its materials. False (nothing changes) when the file is missing or invalid.
    bool load(const std::filesystem::path& cityFile);
    bool loaded() const;

    // Per frame, before the world updates: streams chunks around `focus`.
    void update(const Terrain& terrain, Vec3 focus);
    // Throws away every chunk (after changing settings that affect the geometry); they come back as the camera needs them.
    void rebuildAll();

    // The trees of the district: surveyed trees, street trees and park trees (a mask keeps natural vegetation off paving and sand).
    // Assign both to Vegetation::settings and call Vegetation::rebuildAll().
    std::shared_ptr<const VegetationPlants> plants() const;
    std::shared_ptr<const VegetationMask> vegetationMask(std::shared_ptr<const GroundPatch> ground) const;

    // The surface mask of the ground patch as a renderer texture (for MaterialParams::terrainMask). Returns false without a patch.
    bool uploadSurfaceMask(const GroundPatch& ground, AssetId& id, Vec4& rect);

    // Collision: moves a vertical circle (centre x, z, radius) out of every building footprint near it. Returns the
    // corrected centre (the input when nothing is touched). A building's ground floor is not enterable.
    Vec2 pushOutOfBuildings(Vec2 centre, f32 radius) const;

    const CityData& data() const;
    CitySettings settings;
    const CityStats& stats() const { return m_stats; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    CityStats m_stats;
};

} // namespace foh
