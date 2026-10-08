#pragma once

// Connects the asset database to the renderer and to worlds:
//   - uploads imported meshes to the GPU (once, on first use) and keeps them current on reimport,
//   - instantiates prefabs / prefab variants into a World,
//   - updates the instances' material colors when a material is reimported.
//
// Thread-safety: main thread only, never while a world is updating (it changes renderer
// buffers and ECS structure).

#include <span>
#include <string>
#include <unordered_map>

#include "foh/asset_db.h"
#include "foh/platform.h"
#include "foh/renderer/renderer.h"
#include "foh/scene/prefab_instance.h"

namespace foh {

class FOH_API AssetSceneBridge {
public:
    // Both must outlive the bridge.
    AssetSceneBridge(AssetDb& db, Renderer& renderer);

    // Spawns a prefab (or variant, with its overrides applied) at `at`. Meshes and materials it
    // needs are loaded and uploaded first. root == Entity{} and `error` set on failure.
    PrefabInstanceResult instantiate(World& world, AssetId prefabOrVariant, const Transform& at, std::string* error = nullptr);

    // Call with ImportResult::changed after a (re)import. Re-uploads meshes that are in use,
    // drops removed ones and refreshes MeshRenderer colors of the given worlds.
    void assetsChanged(std::span<const AssetId> changed, std::span<World* const> worlds);

    // Loads and uploads a mesh if that has not happened yet.
    const MeshInfo* ensureMesh(const AssetId& mesh, std::string* error = nullptr);

private:
    Vec4 materialColor(const AssetId& material);
    bool uploadMesh(const AssetId& mesh, std::string* error);

    AssetDb& m_db;
    Renderer& m_renderer;
    std::unordered_map<AssetId, MeshInfo, AssetIdHash> m_meshes;
    std::unordered_map<AssetId, Vec4, AssetIdHash> m_colors;
};

} // namespace foh
