#pragma once

// What game code can ask of the world's ground: the terrain height and the trees' trunks. A resource of the main world,
// filled by whoever owns the terrain (the editor, the runtime); both pointers are null where there is no terrain, and
// game code then falls back to a flat ground at y = 0. The objects live in the engine, so calling them from a game
// module is fine (no STL crosses the boundary).
//
// Thread-safety: main thread (game systems run there while the terrain streams on jobs).

#include "foh/world/terrain.h"
#include "foh/world/vegetation.h"

namespace foh {

struct WorldSurface {
    const Terrain* terrain = nullptr;
    const Vegetation* vegetation = nullptr;

    // Height of the walkable ground at (x, z): the terrain, or 0 without one.
    f32 groundHeight(f32 x, f32 z) const { return terrain ? terrain->height(x, z) : 0.0f; }
};

} // namespace foh
