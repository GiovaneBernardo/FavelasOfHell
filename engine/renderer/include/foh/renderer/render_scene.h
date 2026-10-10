#pragma once

#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

#include "foh/asset_id.h"
#include "foh/ecs.h"
#include "foh/jobs.h"
#include "foh/math.h"
#include "foh/platform.h"
#include "foh/renderer/components.h"
#include "foh/types.h"

namespace foh {

// ---- extracted data: what the renderer knows about a scene ---------------------------

struct ExtractedMesh {
    Mat4 world;
    Vec4 color;
    u32 mesh;
    u32 material; // renderer material index (0 = default material)
    u64 entity; // Entity::id, for picking
};

// A skinned mesh instance: its first SkinMatrix is skinMatrices[palette] (one per skeleton joint).
struct ExtractedSkinned {
    ExtractedMesh mesh;
    u32 palette;
};

struct ExtractedCamera {
    u64 entity; // Entity::id of the owner
    Mat4 world;
    f32 fovY, nearZ, farZ;
    bool active;
};

struct ExtractedSun {
    bool present = false;
    Vec3 direction{0.4f, -1.0f, 0.3f}; // direction the light travels
    Vec3 color{1.0f};                  // already multiplied by intensity
};

struct ExtractedScene {
    std::vector<ExtractedMesh> meshes;
    std::vector<ExtractedSkinned> skinned;
    std::vector<SkinMatrix> skinMatrices; // the palettes of all characters, back to back
    std::vector<ExtractedCamera> cameras;
    ExtractedSun sun;
    u64 frame = 0;
};

// Imported mesh -> renderer mesh ids (one per submesh). Filled by Renderer::uploadMeshAsset,
// read by the extraction system to turn MeshRenderer asset references into draw ranges.
//
// Thread-safety: set/erase on the main thread while no world is updating; find() may run
// concurrently from extraction jobs.
class FOH_API MeshLookup {
public:
    static constexpr u32 kInvalid = ~0u;

    u32 find(const AssetId& asset, u32 submesh) const {
        const auto it = m_meshes.find(asset);
        return it != m_meshes.end() && submesh < it->second.size() ? it->second[submesh] : kInvalid;
    }
    const std::vector<u32>* ids(const AssetId& asset) const {
        const auto it = m_meshes.find(asset);
        return it == m_meshes.end() ? nullptr : &it->second;
    }
    void set(const AssetId& asset, std::vector<u32> ids) {
        m_meshes[asset] = std::move(ids);
        ++m_version;
    }
    void erase(const AssetId& asset) {
        m_meshes.erase(asset);
        ++m_version;
    }
    // Grows on every change: lets a reader cache lookups for as long as it is unchanged.
    u64 version() const { return m_version; }

private:
    std::unordered_map<AssetId, std::vector<u32>, AssetIdHash> m_meshes;
    u64 m_version = 1;
};

// Imported material -> renderer material index. Filled by Renderer::setMaterial, read by the extraction
// system. An unknown material (not uploaded yet) maps to the default material 0.
//
// Thread-safety: same as MeshLookup.
class FOH_API MaterialLookup {
public:
    u32 find(const AssetId& asset) const {
        const auto it = m_materials.find(asset);
        return it != m_materials.end() ? it->second : 0u;
    }
    bool contains(const AssetId& asset) const { return m_materials.contains(asset); }
    void set(const AssetId& asset, u32 index) {
        m_materials[asset] = index;
        ++m_version;
    }
    void erase(const AssetId& asset) {
        m_materials.erase(asset);
        ++m_version;
    }
    u64 version() const { return m_version; }

private:
    std::unordered_map<AssetId, u32, AssetIdHash> m_materials;
    u64 m_version = 1;
};

// One instance of a static set: upright (yaw only), uniformly scaled, tinted. 32 bytes, so tens of thousands are cheap.
// The rotation is stored as cos/sin already multiplied by the scale (see MakeStaticInstance): packing a frame is a copy.
struct StaticInstance {
    Vec3 position{0.0f};
    f32 scale = 1.0f;
    f32 c = 1.0f, s = 0.0f; // scale * cos(yaw), scale * sin(yaw)
    u32 color = 0xFFFFFFFFu; // rgba8 tint multiplying the material (white = none)
    u32 pad = 0;
};
inline StaticInstance MakeStaticInstance(Vec3 position, f32 yaw, f32 scale, u32 color = 0xFFFFFFFFu) {
    return {position, scale, std::cos(yaw) * scale, std::sin(yaw) * scale, color, 0};
}

// Many copies of one mesh that are not entities: trees, grass, props of a tile. The owner (a streaming system) keeps its
// own gameplay data (collision, ...) and hands the renderer only what is needed to draw. Sets are culled as a whole
// against every viewport's frustum and every shadow cascade (a bounding sphere), and drawn instanced next to the
// entities' batches in the same passes, so they are lit, shadowed and masked exactly like any mesh.
struct StaticInstanceSet {
    AssetId mesh;
    u32 submesh = 0;
    AssetId material; // invalid = the default material
    std::vector<StaticInstance> instances;
    Vec3 center{0.0f}; // bounding sphere of ALL instances including the mesh's extent
    f32 radius = 0.0f; // <= 0: never culled
    // Fraction of the instances drawn (the first ones: store them in random order). A cheap distance LOD for dense sets.
    f32 fraction = 1.0f;
    bool enabled = true;
};

// The renderer's copy of one ECS World's render data. The ECS never exposes its
// storage to the renderer: a plugin (RenderExtractPlugin) copies what is needed
// into this object once per frame, and the renderer only ever reads the
// published snapshot. That keeps the two sides decoupled and makes a dedicated
// render thread possible later (the double buffer below is the seam).
//
// Thread-safety: write() is for the extraction system (inside App::update), read()
// for the renderer after publish(). publish() runs on the main thread between
// the two, never concurrently with either.
class FOH_API RenderScene {
public:
    ExtractedScene& write() {
        m_dirty = true;
        return m_buffers[m_writeIndex];
    }
    const ExtractedScene& read() const { return m_buffers[m_writeIndex ^ 1]; }
    // Makes the last write() visible to read(). Without a new write() since the
    // previous publish() it does nothing, so a frame without a world update keeps
    // showing the last extracted state instead of an older one.
    void publish() {
        if (!m_dirty) return;
        m_writeIndex ^= 1;
        m_dirty = false;
    }

    // Set by the renderer that owns this scene; null for scenes without one (tests).
    const MeshLookup* meshLookup = nullptr;
    const MaterialLookup* materialLookup = nullptr;

    // Scratch of the extraction: character entity -> offset of its palette in ExtractedScene::skinMatrices.
    std::unordered_map<u64, u32>& paletteOffsets() { return m_paletteOffsets; }

    // Static instance sets (see StaticInstanceSet). Main thread only, between frames: the renderer reads them while it
    // prepares the frame, on the same thread. Ids are never 0.
    u32 addStaticSet(StaticInstanceSet set) {
        const u32 id = ++m_nextStaticSet;
        m_staticSets.emplace(id, std::move(set));
        return id;
    }
    void removeStaticSet(u32 id) { m_staticSets.erase(id); }
    StaticInstanceSet* staticSet(u32 id) {
        const auto it = m_staticSets.find(id);
        return it == m_staticSets.end() ? nullptr : &it->second;
    }
    const std::unordered_map<u32, StaticInstanceSet>& staticSets() const { return m_staticSets; }

    // Per-job-thread staging used by the parallel extraction.
    std::vector<ExtractedMesh>& staging(u32 threadSlot) { return m_staging[threadSlot]; }

private:
    ExtractedScene m_buffers[2];
    u32 m_writeIndex = 0;
    bool m_dirty = false;
    std::vector<ExtractedMesh> m_staging[kMaxJobThreads];
    std::unordered_map<u64, u32> m_paletteOffsets;
    std::unordered_map<u32, StaticInstanceSet> m_staticSets;
    u32 m_nextStaticSet = 0;
};

// Resource that ties an ECS world to its RenderScene. Holds a pointer into
// engine-owned memory (the Renderer owns the scene), which is allowed.
struct RenderSceneLink {
    RenderScene* scene = nullptr;
};

// Registers the render components, the link resource (empty until a scene is
// attached) and the PostUpdate extraction system. Call it (or add the plugin) for
// every App whose world should be renderable; it belongs to the engine, not to a
// game module.
FOH_API void RegisterRenderExtract(App& app);

// Registers Transform/GlobalTransform and the systems that keep GlobalTransform current
// (PostUpdate, before extraction): adds it to entities that have a Transform, then recomputes
// it for entities whose Transform changed and for everything below them in the hierarchy.
// RegisterRenderExtract calls this; it is separate so tools and tests can use it alone.
//
// Change detection: static entities cost nothing. Writes through Query references and insert()
// are tracked per archetype table; code that writes in place through World::get must call
// World::markModified<Transform>(entity) (the editor does).
FOH_API void RegisterTransformPropagation(App& app);

class RenderExtractPlugin final : public Plugin {
public:
    void build(App& app) override { RegisterRenderExtract(app); }
};

} // namespace foh
