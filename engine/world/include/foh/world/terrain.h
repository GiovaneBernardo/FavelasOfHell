#pragma once

// Infinite heightmap terrain (ADR 0012). CPU-side data only; TerrainRenderer (terrain_renderer.h) streams meshes.
//
// Final height = base + sculpt:
//   base    a pure function of (x, z): the procedural placeholder (TerrainGenParams) now, a real DTM later. It is flat at
//           `floorHeight` (-200 m) everywhere except around the generated land (near the origin), so the world has no
//           edge and can grow by changing the generator; the sea bed falls smoothly to that floor.
//   sculpt  hand-made deltas on a 0.5 m grid, stored sparsely in 32 m tiles (a tile exists only where you sculpted).
//           It survives changing the generator or replacing the base.
//
// Coordinates: x east, z south (the sea is toward +z), y up. The finest sample spacing is kTerrainCell.
//
// Thread-safety: the const queries (height, normal, raycast, sampleNodeHeights) may run on any thread while the main
// thread sculpts (a reader/writer lock guards the sculpt tiles). Everything else is main thread only.

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

#include "foh/jobs.h"
#include "foh/world/heightmap.h"
#include "foh/math.h"
#include "foh/platform.h"
#include "foh/types.h"

namespace foh {

// Placeholder terrain that resembles Vidigal's setting: sea along +z with a wavy coast and a cove, a favela slope,
// rock faces and a Dois Irmaos-like massif inland. Plain data so the editor can edit it and it can be saved.
struct TerrainGenParams {
    u32 seed = 1;
    f32 seaLevel = 0.0f;       // m, the water plane
    f32 floorHeight = -200.0f; // m, the flat floor everywhere there is no generated land
    f32 seabedScale = 320.0f;  // m of distance from the shore over which the sea bed falls toward the floor
    f32 coastZ = 120.0f;       // m, mean shoreline position
    f32 coastWobble = 38.0f;   // m, how far headlands/bays stray from coastZ
    f32 coastScale = 260.0f;   // m, wavelength of the coastline
    f32 coveDepth = 70.0f;     // m, extra inland bite of the beach cove
    f32 coveX = 520.0f;        // m, x of the cove (Praia do Vidigal side)
    f32 slope = 0.36f;         // rise/run of the favela slope
    f32 slopeSpan = 520.0f;    // m of inland distance covered by the slope before the massif takes over
    f32 massifHeight = 340.0f; // m of extra height of the massif above the slope
    f32 massifX = -250.0f;
    f32 massifZ = -520.0f;
    f32 massifRadius = 520.0f;
    f32 ridgeHeight = 160.0f;  // m, the backdrop ridge behind the massif
    f32 landWidth = 1300.0f;   // m, half width of the land along x before it sinks back to the floor
    f32 landDepth = 1100.0f;   // m, how far inland the land reaches before it sinks back to the floor
    f32 roughness = 1.0f;      // overall detail amplitude
    f32 rockiness = 1.0f;      // how much high ground is broken into cliffs
    f32 terracing = 0.25f;     // 0..1, flattening into building shelves on the slope
    f32 terraceHeight = 4.5f;  // m between shelves
    // Real-world base (DemHeightmap): used when a heightmap is loaded and this is set; the fields above then only
    // matter when it is off. Appended at the end so older saved files keep loading.
    u32 useHeightmap = 1;
    f32 demEdgeFade = 1500.0f; // m, the land sinks to the floor over this distance inside the heightmap's border
    f32 demDetail = 1.0f;      // procedural detail between the heightmap's samples (0 = none)
};

enum class BrushMode : u8 { Raise, Lower, Smooth, Flatten, Noise, Erase }; // Erase: pulls the sculpt back toward zero

struct BrushStamp {
    BrushMode mode = BrushMode::Raise;
    Vec2 center{0.0f}; // world x, z
    f32 radius = 20.0f;   // m
    f32 strength = 1.0f;  // 0..1+ scale of the effect
    f32 hardness = 0.35f; // 0 = smooth falloff everywhere, 1 = flat top with a sharp rim
    f32 dt = 1.0f / 60.0f;
    f32 flattenHeight = 0.0f; // Flatten: the height to level toward
    u32 seed = 1;             // Noise
    f32 noiseScale = 6.0f;    // Noise: feature size in m
};

inline constexpr f32 kTerrainCell = 0.5f;         // finest sample spacing (m)
inline constexpr u32 kTerrainTileCells = 64;      // cells per mesh tile side, at every LOD
inline constexpr f32 kTerrainTileSize = kTerrainCell * f32(kTerrainTileCells); // 32 m: a LOD 0 tile, also the sculpt tile

// One node of the LOD quadtree: level L covers 32 * 2^L metres per side with the same 64 x 64 cells (so the cell
// size doubles with every level). x, z are the node's integer coordinates at that level.
struct TerrainNodeKey {
    i32 level = 0, x = 0, z = 0;
    f32 size() const { return kTerrainTileSize * f32(1u << level); }
    f32 cell() const { return size() / f32(kTerrainTileCells); }
    Vec2 origin() const { return Vec2(f32(x) * size(), f32(z) * size()); } // world x, z of the corner
    friend bool operator==(const TerrainNodeKey&, const TerrainNodeKey&) = default;
};

struct TerrainDirtyRect {
    f32 minX, minZ, maxX, maxZ;
};

class FOH_API Terrain {
public:
    Terrain();
    ~Terrain();
    Terrain(const Terrain&) = delete;
    Terrain& operator=(const Terrain&) = delete;

    // Optional: lets the terrain use the job system for big one-off computations (filling base caches for a brush).
    void setJobs(JobSystem* jobs) { m_jobs = jobs; }

    // ---- base layer ----
    // The real-world heightmap (null = procedural only). Marks everything changed.
    void setHeightmap(std::shared_ptr<const DemHeightmap> dem);
    bool loadHeightmap(const std::filesystem::path& file); // false (nothing changes) when the file is missing or invalid
    std::shared_ptr<const DemHeightmap> heightmap() const;
    // Lays a finer ground patch (ground_patch.h) over the heightmap. False when the file is missing or there is no heightmap.
    bool loadGroundPatch(const std::filesystem::path& file);
    const TerrainGenParams& params() const { return m_params; }
    // Replaces the generator parameters (the sculpt layer is kept). Everything is marked changed.
    void setParams(const TerrainGenParams& params);
    u64 generation() const { return m_generation; } // grows when the base changes (parameters)
    u64 revision() const { return m_revision; }     // grows on every change of any kind

    // ---- reading ----
    f32 baseHeight(f32 x, f32 z) const;
    f32 height(f32 x, f32 z) const; // base + sculpt, bilinear on the sculpt grid
    f32 sculptDelta(f32 x, f32 z) const; // the hand-made part alone
    Vec3 normal(f32 x, f32 z) const;
    // First hit of a ray with the surface within maxDist (sphere tracing + bisection). False if it misses.
    bool raycast(Vec3 origin, Vec3 direction, f32 maxDist, Vec3& hit) const;

    // Heights of a node on its (65 + 2)^2 grid: one extra row/column of margin on every side for normals.
    // Index (j * 67 + i) is at world (origin + (i - 1) * cell, origin + (j - 1) * cell). Thread-safe.
    static constexpr u32 kNodeGrid = kTerrainTileCells + 3;
    void sampleNodeHeights(const TerrainNodeKey& node, std::vector<f32>& out) const;

    // ---- sculpting (main thread) ----
    // One undo step groups everything between beginStroke() and endStroke(); a stamp outside a stroke is its own step.
    void beginStroke();
    void applyBrush(const BrushStamp& stamp);
    void endStroke();
    bool canUndo() const;
    bool canRedo() const;
    // Edits the final height over a rectangle (world metres) through the sculpt layer: for every 0.5 m sample `newHeight`
    // gets (x, z, the current final height) and returns the height it should have. Undoable like a brush stroke (one
    // step unless called inside beginStroke/endStroke). This is how roads conform the ground.
    using HeightEdit = std::function<f32(f32 x, f32 z, f32 current)>;
    void editRect(Vec2 minCorner, Vec2 maxCorner, const HeightEdit& newHeight);
    void undo();
    void redo();
    void clearSculpt(); // drops every hand edit (undoable)
    size_t sculptTileCount() const;

    // World rectangles whose height changed (sculpting, undo) since `cursor`, which is advanced. Every consumer keeps its
    // own cursor, starting at dirtyCursor() when it begins to follow. A change of generation() means everything changed
    // and is not reported here.
    std::vector<TerrainDirtyRect> dirtyRectsSince(u64& cursor) const;
    u64 dirtyCursor() const;

    // ---- persistence: generator parameters + sparse sculpt tiles (the base is a function, not stored) ----
    bool save(const std::filesystem::path& file) const;
    bool load(const std::filesystem::path& file);

private:
    struct Impl; // sculpt tiles, base caches, undo stacks, the lock
    std::unique_ptr<Impl> m_impl;
    TerrainGenParams m_params;
    JobSystem* m_jobs = nullptr;
    u64 m_generation = 1;
    u64 m_revision = 1;
};

// The generator's height function at one point (also useful for tests and previews).
FOH_API f32 TerrainBaseHeight(const TerrainGenParams& p, f32 x, f32 z);
// With the heightmap (null = procedural): what the terrain really uses.
FOH_API f32 TerrainBaseHeight(const TerrainGenParams& p, const DemHeightmap* dem, f32 x, f32 z);

} // namespace foh
