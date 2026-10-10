#pragma once

// Streams an infinite Terrain to the renderer as a quadtree of LOD nodes around a focus point (the camera).
//
// Every node is a 64 x 64 cell mesh (see TerrainNodeKey): level 0 nodes are 32 m wide with 0.5 m cells, every level
// doubles both. Nodes near the focus are split into finer ones. Meshes are built on the job system (low priority),
// uploaded on the main thread under a per-frame budget through Renderer::uploadMeshAsset, and shown as one entity per
// node. A node is only swapped for its replacement once the replacement is resident, so the ground never shows holes
// or overlapping levels; cracks between levels are hidden by skirts. Sculpting rebuilds the nodes it touches in place.
// The water's sea-floor texture is fed from the terrain.
//
// Terrain shading is a renderer material (see setMaterial); vegetation is a separate system.
//
// Thread-safety: main thread only (it talks to the Renderer and the World). Node builds run on jobs and only read the
// Terrain (thread-safe).

#include <memory>
#include <vector>

#include "foh/asset_id.h"
#include "foh/ecs.h"
#include "foh/jobs.h"
#include "foh/platform.h"
#include "foh/renderer/renderer.h"
#include "foh/world/terrain.h"

namespace foh {

struct TerrainTileMesh {
    std::vector<Vec3> positions; // relative to the node's corner (x0, 0, z0) so nodes keep float precision far from the origin
    std::vector<Vec3> normals;
    std::vector<Vec2> uvs; // world x/z in metres * 0.25
    std::vector<u32> indices;
};

// Builds the mesh of one node: the 65 x 65 grid plus a skirt around it. Thread-safe.
FOH_API void BuildTerrainNodeMesh(const Terrain& terrain, const TerrainNodeKey& node, TerrainTileMesh& out);

struct TerrainStreamStats {
    u32 nodes = 0;       // resident or being built
    u32 visible = 0;
    u32 building = 0;    // jobs in flight
    u32 waiting = 0;     // desired nodes without a mesh yet
    u64 triangles = 0;   // of the visible nodes
    u32 perLevel[16] = {};
};

class FOH_API TerrainRenderer {
public:
    // `jobs` may be null (meshes are then built on the calling thread, a few per frame).
    TerrainRenderer(Renderer& renderer, World& world, JobSystem* jobs);
    ~TerrainRenderer();
    TerrainRenderer(const TerrainRenderer&) = delete;
    TerrainRenderer& operator=(const TerrainRenderer&) = delete;

    // Per frame, before the world updates (it spawns and despawns entities). `focus` is where detail is wanted.
    void update(Terrain& terrain, Vec3 focus);

    // The material and tint of every node (a material id registered with Renderer::setMaterial; invalid = default).
    void setMaterial(const AssetId& material, Vec4 tint);

    f32 splitFactor = 2.0f;      // a node splits while the focus is closer than size * splitFactor
    u32 maxLevel = 7;            // coarsest level (4096 m nodes, 64 m cells)
    f32 viewDistance = 9000.0f;  // nodes farther than this are not built
    bool feedWater = true;       // keep Renderer::setWaterBed in sync with the terrain

    const TerrainStreamStats& stats() const { return m_stats; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    TerrainStreamStats m_stats;
};

} // namespace foh
