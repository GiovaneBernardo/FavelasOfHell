#include "foh/scene/prefab_instance.h"

namespace foh {

namespace {

// World transform of a child given its parent's world transform. Exact for uniform parent
// scale; with a non-uniform scale on a rotated parent the shear cannot be represented in TRS
// and is dropped.
Transform compose(const Transform& parent, const Transform& local) {
    Transform t;
    t.position = parent.position + parent.rotation * (parent.scale * local.position);
    t.rotation = parent.rotation * local.rotation;
    t.scale = parent.scale * local.scale;
    return t;
}

} // namespace

PrefabInstanceResult InstantiatePrefab(World& world, const PrefabAsset& prefab, const MeshInfoFn& meshInfo,
                                       const MaterialColorFn& materialColor, const Transform& at) {
    PrefabInstanceResult result;
    result.root = world.spawn(Name::Make(prefab.name.empty() ? "Prefab" : prefab.name), at);
    result.entities.push_back(result.root);

    std::vector<Transform> worldTransforms(prefab.nodes.size());
    std::vector<Entity> nodeEntities(prefab.nodes.size());
    for (size_t i = 0; i < prefab.nodes.size(); ++i) {
        const PrefabNode& node = prefab.nodes[i];
        Transform local;
        local.position = node.position;
        local.rotation = node.rotation;
        local.scale = node.scale;
        worldTransforms[i] = compose(node.parent >= 0 ? worldTransforms[static_cast<size_t>(node.parent)] : at, local);

        const Entity e = world.spawn(Name::Make(node.name), worldTransforms[i]);
        world.setParent(e, node.parent >= 0 ? nodeEntities[static_cast<size_t>(node.parent)] : result.root);
        nodeEntities[i] = e;
        result.entities.push_back(e);

        const MeshInfo* info = node.mesh.isValid() && meshInfo ? meshInfo(node.mesh) : nullptr;
        if (!info) continue;
        for (u32 sub = 0; sub < info->submeshMaterialSlots.size(); ++sub) {
            const u32 slot = info->submeshMaterialSlots[sub];
            MeshRenderer mr;
            mr.asset = node.mesh;
            mr.submesh = sub;
            mr.material = slot < node.materials.size() ? node.materials[slot] : AssetId{};
            if (materialColor) mr.color = materialColor(mr.material);
            if (sub == 0) {
                world.insert(e, mr);
            } else {
                const Entity extra = world.spawn(Name::Make(node.name + ".sub" + std::to_string(sub)), worldTransforms[i], mr);
                world.setParent(extra, e);
                result.entities.push_back(extra);
            }
        }
    }
    return result;
}

} // namespace foh
