#pragma once

// Game code asks for a model by naming it; the host instantiates it.
//
// A game module cannot reach the asset database, and asset GUIDs differ between machines (imports are not committed),
// so a game entity says WHAT it wants as a source path:
//
//     world.spawn(Transform{...}, PlayerTag{}, MakePrefabRequest("imported/Y Bot/Y Bot.fbx"));
//
// Every frame (PreUpdate) the scene plugin looks for requests whose model is imported, instantiates the prefab INTO
// that entity (its nodes become children; a skinned prefab adds Animator, SkinPalette and the Locomotion components,
// with the locomotion clips filled in from the clips imported next to it) and removes the request. Until the model
// exists the request just waits, so the game can start before the import finishes.
//
// Thread-safety: the system is exclusive (main thread).

#include <string_view>

#include "foh/asset_id.h"
#include "foh/ecs.h"
#include "foh/ecs/reflect.h"
#include "foh/platform.h"

namespace foh {

class AssetSceneBridge;

struct PrefabRequest {
    char sourcePath[128] = {}; // project-relative source path, e.g. "imported/Y Bot/Y Bot.fbx"
    AssetId prefab;            // used when sourcePath is empty
};

inline PrefabRequest MakePrefabRequest(std::string_view sourcePath) {
    PrefabRequest r;
    const size_t n = sourcePath.size() < sizeof(r.sourcePath) - 1 ? sourcePath.size() : sizeof(r.sourcePath) - 1;
    for (size_t i = 0; i < n; ++i) r.sourcePath[i] = sourcePath[i];
    return r;
}

// A model an entity SHOWS, changeable at any time: set sourcePath (in the inspector, from the content browser's "Use as
// model", or from game code) and the entity swaps to that model. The previous model's nodes and its Animator /
// SkinPalette / Locomotion* components are removed first. `applied` is what is currently instantiated (managed by the
// system; leave it alone). An empty path removes the model. Prefab edits saved next to the source (height, rotation
// from the prefab editor) are applied, so "fix the scale once" carries over to every use.
struct PrefabSlot {
    char sourcePath[128] = {};
    char applied[128] = {};
};

inline PrefabSlot MakePrefabSlot(std::string_view sourcePath) {
    PrefabSlot s;
    const size_t n = sourcePath.size() < sizeof(s.sourcePath) - 1 ? sourcePath.size() : sizeof(s.sourcePath) - 1;
    for (size_t i = 0; i < n; ++i) s.sourcePath[i] = sourcePath[i];
    return s;
}

// Tag on every entity a PrefabSlot created (nodes, skinned meshes): what a swap removes, and what first person hides.
struct SlotModelPart {
    u32 flags = 0;
};

// Resource: where requests are resolved. Holds a pointer into host-owned memory (like RenderSceneLink).
struct SceneBridgeLink {
    AssetSceneBridge* bridge = nullptr;
};

// Registers PrefabRequest, PrefabSlot, SceneBridgeLink and the resolving systems. The host calls it for every world that should
// support requests, then sets the link to its bridge.
FOH_API void RegisterPrefabRequests(App& app);

} // namespace foh

FOH_REFLECT(foh::PrefabRequest, sourcePath, prefab);
FOH_REFLECT(foh::PrefabSlot, sourcePath, applied);
FOH_REFLECT(foh::SlotModelPart, flags);
