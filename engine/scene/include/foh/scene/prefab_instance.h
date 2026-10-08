#pragma once

// Turns a prefab asset into entities. Pure CPU/ECS code (no renderer, no database), so it is
// unit-testable; AssetSceneBridge supplies the asset lookups.
//
// Layout of an instance:
//   <instance root>            Name + Transform (where the prefab was placed)
//     <prefab node>            Name + Transform [+ MeshRenderer for the mesh's first submesh]
//       <name>.sub<k>          one extra entity per further submesh of the node's mesh
//
// Transforms: entities carry a FLAT world Transform for now (see renderer/components.h), so the
// node hierarchy is composed here and baked in; the parent/child relationship is set for the
// hierarchy view. Moving the instance root therefore does not move its children until the
// engine gets transform propagation (known limitation, planned with the Local/Global split).

#include <functional>
#include <vector>

#include "foh/asset_types.h"
#include "foh/ecs.h"
#include "foh/platform.h"
#include "foh/renderer/components.h"

namespace foh {

// What instantiation needs to know about a mesh: the material slot of every submesh.
struct MeshInfo {
    std::vector<u32> submeshMaterialSlots;
};

// Returns null when the mesh is unknown (the node is then created without a MeshRenderer).
using MeshInfoFn = std::function<const MeshInfo*(const AssetId& mesh)>;
// Base color of a material; used for the instance color. Called with an invalid id for slots
// without a material.
using MaterialColorFn = std::function<Vec4(const AssetId& material)>;

struct PrefabInstanceResult {
    Entity root{};
    std::vector<Entity> entities; // every entity created, root first
};

FOH_API PrefabInstanceResult InstantiatePrefab(World& world, const PrefabAsset& prefab, const MeshInfoFn& meshInfo,
                                               const MaterialColorFn& materialColor, const Transform& at);

} // namespace foh
