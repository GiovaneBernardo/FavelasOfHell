#include "foh/scene/prefab_request.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "foh/animation/components.h"
#include "foh/log.h"
#include "foh/renderer/components.h"
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

// Removes the model a slot instantiated: its parts and the components that only exist for an animated model.
void removeModel(World& world, Entity root) {
    std::vector<Entity> parts;
    world.forEachChild(root, [&](Entity child) {
        if (world.has<SlotModelPart>(child)) parts.push_back(child);
    });
    for (const Entity part : parts) world.despawn(part); // children go with them
    if (world.has<Animator>(root)) world.remove<Animator>(root);
    if (world.has<SkinPalette>(root)) world.remove<SkinPalette>(root);
    if (world.has<LocomotionClips>(root)) world.remove<LocomotionClips>(root);
    if (world.has<LocomotionState>(root)) world.remove<LocomotionState>(root);
    if (world.has<Hidden>(root)) world.remove<Hidden>(root);
}

// Exclusive: swaps the model of every entity whose slot names another one than the one shown.
void resolvePrefabSlots(World& world, Res<SceneBridgeLink> link, Query<Entity, const PrefabSlot&> slots) {
    AssetSceneBridge* bridge = link->bridge;
    if (!bridge) return;
    std::vector<Entity> changed;
    slots.each([&](Entity e, const PrefabSlot& s) {
        if (std::strncmp(s.sourcePath, s.applied, sizeof(s.sourcePath)) != 0) changed.push_back(e);
    });
    for (const Entity e : changed) {
        PrefabSlot* slot = world.get<PrefabSlot>(e);
        if (!slot) continue;
        if (slot->applied[0]) {
            removeModel(world, e);
            slot = world.get<PrefabSlot>(e); // the removals above moved the entity
            if (!slot) continue;
            slot->applied[0] = 0;
        }
        if (!slot->sourcePath[0]) continue;
        const AssetId prefab = bridge->findPrefab(slot->sourcePath);
        if (!prefab.isValid()) continue; // not imported (yet): the slot keeps waiting
        const std::string path = slot->sourcePath;
        PrefabInstanceResult instance;
        std::string error;
        if (!bridge->instantiateInto(world, e, prefab, &error, &instance)) {
            FOH_ERROR("model '{}': {}", path, error);
            if (PrefabSlot* again = world.get<PrefabSlot>(e)) again->sourcePath[0] = 0; // do not retry a broken model every frame
            continue;
        }
        for (const Entity part : instance.entities)
            if (part != e) world.insert(part, SlotModelPart{});
        if (PrefabSlot* again = world.get<PrefabSlot>(e)) std::memcpy(again->applied, path.c_str(), std::min(path.size() + 1, sizeof(again->applied)));
    }
}

} // namespace

void RegisterPrefabRequests(App& app) {
    app.registerComponent<PrefabRequest>();
    app.registerComponent<PrefabSlot>();
    app.registerComponent<SlotModelPart>();
    app.insertResource(SceneBridgeLink{});
    app.addSystem(Stage::PreUpdate, &resolvePrefabRequests).name("resolvePrefabRequests");
    app.addSystem(Stage::PreUpdate, &resolvePrefabSlots).name("resolvePrefabSlots");
}

} // namespace foh
