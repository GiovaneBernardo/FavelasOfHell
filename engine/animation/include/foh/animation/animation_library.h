#pragma once

// The animation assets that are loaded and ready to play, plus the data derived from them (clip bindings, locomotion
// blend spaces). The asset loader fills it; the animation systems read it. Like MeshLookup in the renderer it is
// engine-owned memory that worlds reach through a resource holding a pointer (AnimationLink), so it is shared by all
// worlds and survives game module reloads.
//
// Thread-safety: set*/remove*/prepareMoveSet on the main thread while no world is updating; every const method is
// safe to call concurrently from systems.

#include <memory>
#include <unordered_map>
#include <utility>

#include "foh/animation/components.h"
#include "foh/animation/move_set.h"
#include "foh/animation/pose.h"
#include "foh/animation_assets.h"
#include "foh/asset_id.h"
#include "foh/platform.h"

namespace foh {

class FOH_API AnimationLibrary {
public:
    void setSkeleton(const AssetId& id, SkeletonAsset skeleton);
    void removeSkeleton(const AssetId& id);
    void setClip(const AssetId& id, ClipAsset clip);
    void removeClip(const AssetId& id);

    const SkeletonAsset* skeleton(const AssetId& id) const;
    const ClipAsset* clip(const AssetId& id) const;
    // The clip resolved against the skeleton; null when either is missing.
    const ClipBinding* binding(const AssetId& skeleton, const AssetId& clip) const;

    // Builds (once per distinct set of clips) and returns the blend space; null when nothing in `clips` is usable yet.
    // Rebuilt after any clip changes. Main thread only.
    // `crouch` selects the crouched stance's clips (crouchIdleClip, crouchWalk*Clip) instead of the standing ones.
    const MoveSet* prepareMoveSet(const LocomotionClips& clips, bool crouch = false);
    // Lookup only (from systems). Null when not prepared.
    const MoveSet* moveSet(const LocomotionClips& clips, bool crouch = false) const;

    // Changes whenever anything above does; characters re-validate their cached state against it.
    u32 generation() const { return m_generation; }
    size_t skeletonCount() const { return m_skeletons.size(); }
    size_t clipCount() const { return m_clips.size(); }

private:
    struct PairHash {
        size_t operator()(const std::pair<AssetId, AssetId>& p) const {
            const AssetIdHash h;
            return h(p.first) * 1000003u ^ h(p.second);
        }
    };
    void rebind(const AssetId& skeleton, const AssetId& clip);

    std::unordered_map<AssetId, std::shared_ptr<const SkeletonAsset>, AssetIdHash> m_skeletons;
    std::unordered_map<AssetId, std::shared_ptr<const ClipAsset>, AssetIdHash> m_clips;
    std::unordered_map<std::pair<AssetId, AssetId>, ClipBinding, PairHash> m_bindings;
    std::unordered_map<u64, MoveSet> m_moveSets;
    u32 m_generation = 1;
};

// Builds the blend space from the clips in `library` (exposed for tests). False when nothing is usable.
FOH_API bool BuildMoveSet(const AnimationLibrary& library, const LocomotionClips& clips, bool crouch, MoveSet& out);
FOH_API u64 HashLocomotionClips(const LocomotionClips& clips, bool crouch);

} // namespace foh
