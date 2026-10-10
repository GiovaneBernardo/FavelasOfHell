#include "foh/animation/animation_library.h"

#include "foh/asset_container.h"

namespace foh {

u64 HashLocomotionClips(const LocomotionClips& clips, bool crouch) {
    // LocomotionClips is only AssetIds (no padding), so its bytes are its value.
    static_assert(sizeof(LocomotionClips) == 24 * sizeof(AssetId));
    return HashBytes(&clips, sizeof(clips), crouch ? 1 : 0);
}

void AnimationLibrary::rebind(const AssetId& skeleton, const AssetId& clip) {
    const auto s = m_skeletons.find(skeleton);
    const auto c = m_clips.find(clip);
    if (s == m_skeletons.end() || c == m_clips.end()) return;
    m_bindings[{skeleton, clip}] = BindClip(*s->second, *c->second);
}

void AnimationLibrary::setSkeleton(const AssetId& id, SkeletonAsset skeleton) {
    m_rigs[id] = FindRig(skeleton);
    m_skeletons[id] = std::make_shared<const SkeletonAsset>(std::move(skeleton));
    for (const auto& [clipId, clip] : m_clips) rebind(id, clipId);
    m_moveSets.clear();
    ++m_generation;
}

void AnimationLibrary::removeSkeleton(const AssetId& id) {
    m_skeletons.erase(id);
    m_rigs.erase(id);
    for (auto it = m_bindings.begin(); it != m_bindings.end();) it = it->first.first == id ? m_bindings.erase(it) : std::next(it);
    ++m_generation;
}

void AnimationLibrary::setClip(const AssetId& id, ClipAsset clip) {
    m_clips[id] = std::make_shared<const ClipAsset>(std::move(clip));
    for (const auto& [skeletonId, skeleton] : m_skeletons) rebind(skeletonId, id);
    m_moveSets.clear(); // the blend spaces are measured from the clips
    ++m_generation;
}

void AnimationLibrary::removeClip(const AssetId& id) {
    m_clips.erase(id);
    for (auto it = m_bindings.begin(); it != m_bindings.end();) it = it->first.second == id ? m_bindings.erase(it) : std::next(it);
    m_moveSets.clear();
    ++m_generation;
}

const SkeletonAsset* AnimationLibrary::skeleton(const AssetId& id) const {
    const auto it = m_skeletons.find(id);
    return it == m_skeletons.end() ? nullptr : it->second.get();
}

const ClipAsset* AnimationLibrary::clip(const AssetId& id) const {
    const auto it = m_clips.find(id);
    return it == m_clips.end() ? nullptr : it->second.get();
}

const RigInfo* AnimationLibrary::rig(const AssetId& id) const {
    const auto it = m_rigs.find(id);
    return it == m_rigs.end() ? nullptr : &it->second;
}

const ClipBinding* AnimationLibrary::binding(const AssetId& skeleton, const AssetId& clip) const {
    const auto it = m_bindings.find({skeleton, clip});
    return it == m_bindings.end() ? nullptr : &it->second;
}

const MoveSet* AnimationLibrary::prepareMoveSet(const LocomotionClips& clips, bool crouch) {
    const u64 key = HashLocomotionClips(clips, crouch);
    if (const auto it = m_moveSets.find(key); it != m_moveSets.end()) return it->second.usable() ? &it->second : nullptr;
    MoveSet set;
    BuildMoveSet(*this, clips, crouch, set);
    const auto [it, inserted] = m_moveSets.emplace(key, std::move(set));
    return it->second.usable() ? &it->second : nullptr;
}

const MoveSet* AnimationLibrary::moveSet(const LocomotionClips& clips, bool crouch) const {
    const auto it = m_moveSets.find(HashLocomotionClips(clips, crouch));
    return it != m_moveSets.end() && it->second.usable() ? &it->second : nullptr;
}

} // namespace foh
