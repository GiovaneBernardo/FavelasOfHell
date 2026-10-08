#pragma once

// Connects the asset database to the renderer and to worlds:
//   - instantiates prefabs / prefab variants into a World (instantly: it needs only the small prefab file),
//   - streams the meshes, materials and textures they use to the GPU in the background (AssetLoader),
//   - keeps everything current when a source is reimported.
//
// Thread-safety: main thread only, never while a world is updating (it changes ECS structure and renderer buffers).

#include <span>
#include <string>

#include "foh/asset_db.h"
#include "foh/platform.h"
#include "foh/renderer/renderer.h"
#include "foh/scene/asset_loader.h"
#include "foh/scene/prefab_instance.h"

namespace foh {

class FOH_API AssetSceneBridge {
public:
    // All must outlive the bridge. `jobs` may be null (loads then run inline, one per pump()).
    AssetSceneBridge(AssetDb& db, Renderer& renderer, JobSystem* jobs);

    // Spawns a prefab (or variant, with its overrides applied) at `at` and requests what it needs. Returns at
    // once; the meshes appear as their data arrives. root == Entity{} and `error` set on failure.
    PrefabInstanceResult instantiate(World& world, AssetId prefabOrVariant, const Transform& at, std::string* error = nullptr);

    // Once per frame, before the worlds update: hands loaded data to the renderer.
    void pump() { m_loader.pump(); }

    // Call with ImportResult::changed after a (re)import: reloads what is in use, releases what is gone.
    void assetsChanged(std::span<const AssetId> changed);

    AssetLoader& loader() { return m_loader; }
    const AssetLoader& loader() const { return m_loader; }

private:
    AssetDb& m_db;
    AssetLoader m_loader;
};

} // namespace foh
