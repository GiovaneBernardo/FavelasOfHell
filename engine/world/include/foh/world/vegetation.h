#pragma once

// Trees, palms and grass on the Terrain (ADR 0012). Nothing here is an entity: each tile of plants is a handful of
// retained, GPU-instanced StaticInstanceSets in the renderer (culled against every viewport and shadow cascade), plus a
// compact list of trunks for collision.
//
// Placement is a deterministic function of the seed and the terrain (a jittered grid per tile, accepted by height, slope
// and noise masks), so a seed always plants the same forest. Tiles around the camera are generated on the job system and
// planted when they are ready; they are removed with hysteresis when the camera leaves. Trees swap to a cheaper mesh with
// distance, grass thins out. The meshes are procedural stand-ins (see vegetation_assets.h); the trunks take a PBR bark
// texture set (setBarkTextures).
//
// Places that must stay clear (the favela's lots, roads, the beach) are described with `clearings`.
//
// Thread-safety: main thread only (it talks to the Renderer); tile generation runs on jobs and only reads the Terrain.

#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

#include "foh/asset_id.h"
#include "foh/ecs.h"
#include "foh/jobs.h"
#include "foh/platform.h"
#include "foh/renderer/renderer.h"
#include "foh/world/terrain.h"

namespace foh {

struct VegetationClearing {
    f32 x = 0.0f, z = 0.0f;
    f32 radius = 20.0f;  // fully clear inside
    f32 feather = 10.0f; // fades back to full density over this much more
};

// A plant placed by hand or by a generator (street trees, park trees, beach palms): drawn and collided like the natural ones.
struct VegetationPlant {
    f32 x = 0.0f, z = 0.0f;
    f32 scale = 1.0f; // 1 = the mesh's natural size (a tree ~11-18 m, a palm ~10 m)
    f32 yaw = 0.0f;
    u8 kind = 0;      // 0..3 broadleaf variants, 4 palm
    u32 tint = 0xFFFFFFFFu;
};

// Explicit plants bucketed by tree tile (128 m), so a tile finds its own without scanning them all.
struct VegetationPlants {
    std::unordered_map<u64, std::vector<VegetationPlant>> byTile;
    static u64 Key(f32 x, f32 z) {
        return (u64(static_cast<u32>(i32(std::floor(x / 128.0f)))) << 32) | u64(static_cast<u32>(i32(std::floor(z / 128.0f))));
    }
    void add(const VegetationPlant& p) { byTile[Key(p.x, p.z)].push_back(p); }
};

// Where natural vegetation may grow, 0..1 (the city's paving and sand say 0). Called from job threads: must be thread-safe.
class VegetationMask {
public:
    virtual ~VegetationMask() = default;
    virtual f32 density(f32 x, f32 z) const = 0;
};

struct VegetationSettings {
    bool trees = true;
    bool palms = true;
    bool grass = true;
    f32 treeDistance = 520.0f;     // m, trees are planted within this range of the focus
    f32 treeNearDistance = 130.0f; // m, closer than this trees use the detailed mesh
    f32 grassDistance = 75.0f;     // m
    f32 treeDensity = 1.0f;        // multiplier
    f32 grassDensity = 1.0f;       // multiplier
    u32 seed = 1;
    std::vector<VegetationClearing> clearings;
    std::shared_ptr<const VegetationMask> mask;     // null = everywhere
    std::shared_ptr<const VegetationPlants> plants; // explicit plants, in addition to the natural ones
};

struct VegetationStats {
    u32 treeTiles = 0, grassTiles = 0;
    u32 trees = 0, palms = 0, tufts = 0;
    u32 waiting = 0; // tiles in range that are not planted yet
};

class FOH_API Vegetation {
public:
    // `world` supplies the renderer scene (its RenderSceneLink); `jobs` may be null (tiles are then built inline).
    Vegetation(Renderer& renderer, World& world, JobSystem* jobs);
    ~Vegetation();
    Vegetation(const Vegetation&) = delete;
    Vegetation& operator=(const Vegetation&) = delete;

    // Per frame, before the world updates.
    void update(const Terrain& terrain, Vec3 focus);

    // Change settings, then call this to replant everything (cheap changes like distances do not need it).
    void rebuildAll();

    // The trunk material's textures (already uploaded or streaming: texture asset ids). Invalid ids keep the stand-in.
    void setBarkTextures(const AssetId& color, const AssetId& normal);

    // Collision: moves a vertical circle (centre x, z in metres, radius) out of the trunks of trees and palms around it.
    // Returns the corrected centre (the input when nothing is touched).
    Vec2 pushOutOfTrunks(Vec2 centre, f32 radius) const;

    VegetationSettings settings;
    const VegetationStats& stats() const { return m_stats; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    VegetationStats m_stats;
};

} // namespace foh
