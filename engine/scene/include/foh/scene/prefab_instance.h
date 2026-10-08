#pragma once

// Turns a prefab asset into entities. Pure CPU/ECS code (no renderer, no database), so it is
// unit-testable; AssetSceneBridge supplies the asset lookups.
//
// Layout of an instance:
//   <instance root>            Name + Transform (where the prefab was placed)
//     <prefab node>            Name + Transform [+ MeshRenderer for the mesh's first submesh]
//       <name>.sub<k>          one extra entity per further submesh of the node's mesh
//
// Transforms: every node keeps its LOCAL transform and is parented to its prefab parent, so moving the
// instance root (or any node) moves everything below it; the transform propagation system computes the
// world transforms (GlobalTransform). The instance root is placed at `at`, which is a world transform
// (the root has no parent).

#include <vector>

#include "foh/asset_types.h"
#include "foh/ecs.h"
#include "foh/platform.h"
#include "foh/renderer/components.h"

namespace foh {

struct PrefabInstanceResult {
    Entity root{};
    std::vector<Entity> entities; // every entity created, root first
};

// Needs nothing but the prefab itself: meshes and materials are referenced by asset id and show up in the
// renderer when their data has been loaded (until then the instance simply is not drawn / uses the default
// material). Components must be registered in the world (RegisterRenderExtract does it).
FOH_API PrefabInstanceResult InstantiatePrefab(World& world, const PrefabAsset& prefab, const Transform& at);

} // namespace foh
