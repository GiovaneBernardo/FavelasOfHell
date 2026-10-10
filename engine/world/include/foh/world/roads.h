#pragma once

// Hand-placed roads (ADR 0012): a road is a spline through points on the ground plane (x, z); the height always comes
// from the terrain. From it the engine builds
//   - a surface mesh (asphalt) and a sidewalk mesh (curb, paving, a ramp back into the ground) draped on the terrain,
//   - the carving: the terrain is conformed to a smoothed, grade-limited profile along the road (undoable, written into
//     the sculpt layer, so it is part of the terrain file),
//   - clearings, so vegetation keeps off the road.
// Intersections, lane markings and traffic rules are not here yet: roads simply overlap where they cross.
//
// Thread-safety: main thread only.

#include <filesystem>
#include <string>
#include <vector>

#include "foh/math.h"
#include "foh/platform.h"
#include "foh/types.h"
#include "foh/world/terrain.h"
#include "foh/world/vegetation.h"

namespace foh {

struct RoadPoint {
    f32 x = 0.0f, z = 0.0f;
};

struct Road {
    std::string name = "Road";
    std::vector<RoadPoint> points;
    f32 width = 7.0f;     // m, asphalt
    f32 sidewalk = 2.5f;  // m on each side (0 = none)
    f32 curb = 0.14f;     // m, height of the sidewalk above the asphalt
    f32 maxGrade = 0.18f; // rise/run the carved profile may have
};

struct RoadMesh {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec2> uvs;
    std::vector<Vec4> tangents;
    std::vector<u32> indices;
    bool empty() const { return indices.empty(); }
};

struct RoadMeshes {
    RoadMesh surface;
    RoadMesh sidewalk;
};

class FOH_API RoadNetwork {
public:
    std::vector<Road> roads;
    u64 revision = 1; // grows when the caller reports a change (touch())
    void touch() { ++revision; }

    // Text file: "road <name>" lines with width/sidewalk/curb/grade and "p x z" points. Roads are authored content.
    bool save(const std::filesystem::path& file) const;
    bool load(const std::filesystem::path& file);
};

// The spline resampled every `step` metres (uniform Catmull-Rom through the points; the ends are straight).
FOH_API std::vector<Vec2> RoadCenterline(const Road& road, f32 step);

// The mesh of a road, draped on the terrain as it is NOW (carve first, then build, for a flat cross section).
FOH_API RoadMeshes BuildRoadMeshes(const Terrain& terrain, const Road& road);

// Conforms the terrain to the road: heights along the centre are smoothed and limited to `maxGrade`, the ground across
// the road and sidewalk is made level to them and blends back into the land over a few metres. One undo step.
FOH_API void CarveRoad(Terrain& terrain, const Road& road);

// Circles that keep vegetation off the road.
FOH_API std::vector<VegetationClearing> RoadClearings(const Road& road);

} // namespace foh
