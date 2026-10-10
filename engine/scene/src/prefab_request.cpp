#include "foh/scene/prefab_request.h"

#include <vector>

#include "foh/log.h"
#include "foh/scene/asset_scene_bridge.h"

namespace foh {

namespace {

// Exclusive: it adds entities and components to the world.
void resolvePrefabRequests(World& world, Res<SceneBridgeLink> link, Query<Entity, const PrefabRequest&> requests) {
    AssetSceneBridge* bridge = link->bridge;
    if (!bridge) return;
    struct Pending {
        Entity entity;
        PrefabRequest request;
    };
    std::vector<Pending> pending;
    requests.each([&](Entity e, const PrefabRequest& r) { pending.push_back({e, r}); });
    for (const Pending& p : pending) {
        const AssetId prefab = p.request.sourcePath[0] ? bridge->findPrefab(p.request.sourcePath) : p.request.prefab;
        if (!prefab.isValid()) continue; // not imported (yet): try again next frame
        std::string error;
        world.remove<PrefabRequest>(p.entity);
        if (!bridge->instantiateInto(world, p.entity, prefab, &error))
            FOH_ERROR("prefab request '{}': {}", p.request.sourcePath, error);
    }
}

} // namespace

void RegisterPrefabRequests(App& app) {
    app.registerComponent<PrefabRequest>();
    app.insertResource(SceneBridgeLink{});
    app.addSystem(Stage::PreUpdate, &resolvePrefabRequests).name("resolvePrefabRequests");
}

} // namespace foh
