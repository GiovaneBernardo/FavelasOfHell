#pragma once

// The locomotion blend space (ADR 0009), built from the clips themselves.
//
// Every move clip is measured: its root motion gives the velocity it was authored for (direction and speed). Clips of
// similar speed form a "ring" (walk strafes and walk forward are one ring, run ones another); inside a ring clips
// are ordered by direction. For a desired velocity the blend is
//   - across speed:     idle <-> slowest ring <-> ... <-> fastest ring   (two adjacent rings at most)
//   - across direction: the two clips of a ring on either side of the desired direction
// so at most four move clips plus idle are evaluated. Where a ring has a large hole (no backward clip) the nearest
// clip is used with a short blend instead of mixing its two distant neighbours into a nonsense pose.
//
// Thread-safety: plain data; EvaluateMoveSet is pure.

#include <vector>

#include "foh/asset_id.h"
#include "foh/math.h"
#include "foh/platform.h"
#include "foh/types.h"

namespace foh {

struct MoveEntry {
    AssetId clip;
    Vec2 velocity{0.0f}; // (x, z) in the character's local frame, m/s, measured from the clip's root motion
    f32 speed = 0.0f;
    f32 angle = 0.0f;    // atan2(x, z): 0 forward, positive toward the character's +X (its left)
    f32 duration = 1.0f; // one gait cycle, seconds
    f32 syncOffset = 0.0f; // phase of the left foot coming down; clips are sampled at (phase + offset)
};

struct MoveRing {
    f32 speed = 0.0f;           // mean speed of its clips
    std::vector<u32> entries;   // indices into MoveSet::entries, ascending angle
};

struct TurnEntry {
    AssetId clip;
    f32 yaw = 0.0f; // total heading change, radians (positive: toward +X)
    f32 duration = 1.0f;
};

struct MoveSet {
    AssetId idle;
    f32 idleDuration = 1.0f;
    AssetId jump;
    f32 jumpDuration = 1.0f;
    std::vector<MoveEntry> entries;
    std::vector<MoveRing> rings; // ascending speed
    std::vector<TurnEntry> turns;
    bool usable() const { return idle.isValid() || jump.isValid() || !entries.empty() || !turns.empty(); }
};

struct MoveWeights {
    static constexpr u32 kMaxItems = 8;
    struct Item {
        u32 entry = 0;
        f32 weight = 0.0f;
    } items[kMaxItems];
    u32 count = 0;
    f32 idleWeight = 0.0f;
    f32 moveWeight = 0.0f;        // sum of the item weights (idleWeight + moveWeight = 1)
    f32 blendedSpeed = 0.0f;      // weight-averaged authored speed of the move clips
    f32 blendedDuration = 1.0f;   // weight-averaged cycle time of the move clips
};

// `localVelocity` is (x, z) in the character's frame.
FOH_API MoveWeights EvaluateMoveSet(const MoveSet& set, const Vec2& localVelocity);

// The turn clip whose heading change is closest to `angle` among those turning the same way; null when none.
FOH_API const TurnEntry* PickTurn(const MoveSet& set, f32 angle);

} // namespace foh
