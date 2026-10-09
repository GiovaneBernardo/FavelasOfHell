#include "foh/scene/prefab_instance.h"

#include "foh/animation/components.h"

namespace foh {

PrefabInstanceResult InstantiatePrefab(World& world, const PrefabAsset& prefab, const Transform& at) {
    PrefabInstanceResult result;
    result.root = world.spawn(Name::Make(prefab.name.empty() ? "Prefab" : prefab.name), at);
    result.entities.push_back(result.root);
    if (prefab.skeleton.isValid()) {
        // A skinned character: the root animates every skinned mesh below it (bones are not entities).
        Animator animator;
        animator.skeleton = prefab.skeleton;
        world.insert(result.root, animator);
        world.insert(result.root, SkinPalette{});
        world.insert(result.root, LocomotionClips{});
        world.insert(result.root, LocomotionControl{});
        world.insert(result.root, LocomotionState{});
    }

    std::vector<Entity> nodeEntities(prefab.nodes.size());
    for (size_t i = 0; i < prefab.nodes.size(); ++i) {
        const PrefabNode& node = prefab.nodes[i];
        Transform local;
        local.position = node.position;
        local.rotation = node.rotation;
        local.scale = node.scale;

        const Entity e = world.spawn(Name::Make(node.name), local);
        world.setParent(e, node.parent >= 0 ? nodeEntities[static_cast<size_t>(node.parent)] : result.root);
        nodeEntities[i] = e;
        result.entities.push_back(e);

        if (!node.mesh.isValid()) continue;
        for (u32 sub = 0; sub < node.submeshSlots.size(); ++sub) {
            const u32 slot = node.submeshSlots[sub];
            MeshRenderer mr;
            mr.asset = node.mesh;
            mr.submesh = sub;
            mr.material = slot < node.materials.size() ? node.materials[slot] : AssetId{};
            mr.color = Vec4(1.0f); // a tint on top of the material, not the color itself
            if (sub == 0) {
                world.insert(e, mr);
                if (node.skinned) world.insert(e, SkinnedMesh{});
            } else {
                const Entity extra = world.spawn(Name::Make(node.name + ".sub" + std::to_string(sub)), Transform{}, mr);
                if (node.skinned) world.insert(extra, SkinnedMesh{});
                // Skinned submeshes hang directly below the root: that is where the SkinPalette is.
                world.setParent(extra, node.skinned ? result.root : e);
                result.entities.push_back(extra);
            }
        }
    }
    return result;
}

} // namespace foh
