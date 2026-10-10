#include "foh/scene/asset_scene_bridge.h"

#include <cctype>
#include <map>

#include "foh/animation/components.h"
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

AssetId AssetSceneBridge::findPrefab(const std::string& sourcePath) const {
    const SourceRecord* record = m_db.findSource(sourcePath);
    if (!record) return {};
    for (const SubAssetRecord& s : record->subAssets)
        if (!s.tombstone && s.kind == AssetKind::Prefab) return s.guid;
    return {};
}

bool AssetSceneBridge::instantiateInto(World& world, Entity root, AssetId prefabOrVariant, std::string* error) {
    VariantApplyResult resolved;
    if (!m_db.loadResolvedPrefab(prefabOrVariant, resolved, error)) return false;
    for (const PrefabNode& node : resolved.prefab.nodes) {
        m_loader.request(node.mesh);
        for (const AssetId& material : node.materials) m_loader.request(material);
    }
    InstantiatePrefabInto(world, root, resolved.prefab);
    assignLocomotionClips(world, root);
    return true;
}

void AssetSceneBridge::assignLocomotionClips(World& world, Entity root) {
    LocomotionClips* clips = world.get<LocomotionClips>(root);
    if (!clips) return;
    std::map<std::string, AssetId> byName;
    for (const AssetId& id : m_db.assetsOfKind(AssetKind::AnimationClip)) {
        if (const AssetInfo* info = m_db.find(id)) {
            std::string name = info->name;
            for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            byName.emplace(std::move(name), id);
        }
    }
    auto pick = [&](AssetId& slot, std::initializer_list<const char*> names) {
        for (const char* n : names) {
            const auto it = byName.find(n);
            if (it == byName.end()) continue;
            slot = it->second;
            m_loader.request(it->second);
            return;
        }
    };
    pick(clips->idleClip, {"idle", "idle (1)"});
    pick(clips->walkClip, {"walking", "walk"});
    pick(clips->runClip, {"running", "run"});
    pick(clips->sprintClip, {"sprint", "sprinting", "fast run"});
    pick(clips->walkBackClip, {"walking backwards", "walking backward", "walk backward"});
    pick(clips->runBackClip, {"running backward", "running backwards", "run backward"});
    pick(clips->walkLeftClip, {"left strafe walking", "strafe walk left"});
    pick(clips->walkRightClip, {"right strafe walking", "strafe walk right"});
    pick(clips->runLeftClip, {"left strafe", "strafe left"});
    pick(clips->runRightClip, {"right strafe", "strafe right"});
    pick(clips->turnLeftClip, {"left turn"});
    pick(clips->turnRightClip, {"right turn"});
    pick(clips->turnLeft90Clip, {"left turn 90"});
    pick(clips->turnRight90Clip, {"right turn 90"});
    pick(clips->jumpClip, {"jump", "jumping"});
    pick(clips->crouchIdleClip, {"crouch idle", "crouching idle", "crouched idle"});
    pick(clips->crouchWalkClip, {"crouched walking", "crouch walking", "crouching walk", "crouch walk"});
    pick(clips->crouchWalkBackClip, {"crouched walking backwards", "crouch walking backward", "crouched walking backward"});
    pick(clips->crouchWalkLeftClip, {"crouched sneaking left", "crouch strafe left", "crouched walking left", "crouch left"});
    pick(clips->crouchWalkRightClip, {"crouched sneaking right", "crouch strafe right", "crouched walking right", "crouch right"});
}

void AssetSceneBridge::assetsChanged(std::span<const AssetId> changed) {
    for (const AssetId& id : changed) m_loader.assetChanged(id);
}

} // namespace foh
