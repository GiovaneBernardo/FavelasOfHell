#pragma once

// A procedural coast to look at the water before the real terrain exists: a wavy beach with a cove, hills behind it,
// a rocky islet and a sandbar offshore (shoals, breakers and a swash line to look at). Sea level is 0, the land is
// toward -Z and the sea toward +Z. The same height function feeds the terrain mesh and the water's sea-floor texture,
// which is how the real terrain will feed it too (Renderer::setWaterBed).
//
// Editor-only QA content; nothing in the engine depends on it.

#include <vector>

#include "foh/math.h"
#include "foh/types.h"

namespace foh {

struct WaterTestTerrain {
    static constexpr f32 kHalfSize = 800.0f; // the terrain covers [-800, 800] in x and z

    // Height of the ground (m above the sea level 0; negative = sea floor).
    static f32 height(f32 x, f32 z);
};

struct WaterTestMesh {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec2> uvs;
    std::vector<u32> indices;
};

// Regular grid of `cell` metre squares over the terrain's extent.
WaterTestMesh BuildWaterTestMesh(f32 cell);

// Row-major heights of a size x size grid over the same extent (texel centres), for WaterBedDesc.
std::vector<f32> BuildWaterTestBed(u32 size);

} // namespace foh
