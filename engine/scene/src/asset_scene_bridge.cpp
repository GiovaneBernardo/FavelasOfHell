#include "foh/scene/asset_scene_bridge.h"

#include "foh/log.h"

namespace foh {

AssetSceneBridge::AssetSceneBridge(AssetDb& db, Renderer& renderer, JobSystem* jobs) : m_db(db), m_loader(db, renderer, jobs) {}

PrefabInstanceResult AssetSceneBridge::instantiate(World& world, AssetId prefabOrVariant, const Transform& at, std::string* error) {
    VariantApplyResult resolved;
    if (!m_db.loadResolvedPrefab(prefabOrVariant, resolved, error)) return {};
    for (const std::string& orphan : resolved.orphaned) FOH_WARN("prefab variant: override target no longer exists: {}", orphan);

    // A variant may swap materials: request what the RESOLVED prefab references, not only what the files list.
    for (const PrefabNode& node : resolved.prefab.nodes) {
        m_loader.request(node.mesh);
        for (const AssetId& material : node.materials) m_loader.request(material);
    }
    return InstantiatePrefab(world, resolved.prefab, at);
}

void AssetSceneBridge::assetsChanged(std::span<const AssetId> changed) {
    for (const AssetId& id : changed) m_loader.assetChanged(id);
}

} // namespace foh
