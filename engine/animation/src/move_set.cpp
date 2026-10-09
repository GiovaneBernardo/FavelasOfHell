#include "foh/animation/move_set.h"

#include <algorithm>
#include <cmath>

#include "foh/animation/animation_library.h"

namespace foh {

namespace {

constexpr f32 kTwoPi = 2.0f * math::kPi;
constexpr f32 kMaxBlendGap = 2.6f;   // radians; a wider hole between two clips of a ring is not interpolated across
constexpr f32 kGapBlendHalfWidth = 0.3f;
constexpr f32 kRingSpeedRatio = 1.4f; // clips whose speeds are within this factor of a ring's belong to it

f32 smoothstep(f32 t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void addItem(MoveWeights& out, u32 entry, f32 weight) {
    if (weight <= 1e-4f) return;
    for (u32 i = 0; i < out.count; ++i)
        if (out.items[i].entry == entry) {
            out.items[i].weight += weight;
            return;
        }
    if (out.count < MoveWeights::kMaxItems) out.items[out.count++] = {entry, weight};
}

void evaluateRing(const MoveSet& set, const MoveRing& ring, f32 theta, f32 weight, MoveWeights& out) {
    const size_t m = ring.entries.size();
    if (m == 0 || weight <= 0.0f) return;
    if (m == 1) {
        addItem(out, ring.entries[0], weight);
        return;
    }
    // First clip whose angle is beyond theta; its predecessor (cyclically) is the other side.
    size_t hi = 0;
    while (hi < m && set.entries[ring.entries[hi]].angle <= theta) ++hi;
    const size_t lo = (hi + m - 1) % m;
    hi %= m;
    const f32 aLo = set.entries[ring.entries[lo]].angle, aHi = set.entries[ring.entries[hi]].angle;
    f32 gap = aHi - aLo;
    if (gap <= 0.0f) gap += kTwoPi;
    f32 d = theta - aLo;
    if (d < 0.0f) d += kTwoPi;
    f32 t;
    if (gap <= kMaxBlendGap) {
        t = std::clamp(d / gap, 0.0f, 1.0f);
    } else {
        // A hole (no clip for these directions): stay on the nearer clip, crossing over quickly around the middle.
        t = smoothstep((d - (gap * 0.5f - kGapBlendHalfWidth)) / (2.0f * kGapBlendHalfWidth));
    }
    addItem(out, ring.entries[lo], weight * (1.0f - t));
    addItem(out, ring.entries[hi], weight * t);
}

} // namespace

MoveWeights EvaluateMoveSet(const MoveSet& set, const Vec2& localVelocity) {
    MoveWeights out;
    const f32 speed = glm::length(localVelocity);
    const size_t rings = set.rings.size();
    if (rings == 0) {
        out.idleWeight = 1.0f;
        return out;
    }
    const f32 theta = std::atan2(localVelocity.x, localVelocity.y);

    // Across speed: below the slowest ring blend with idle, above the fastest ring clamp (the playback rate catches up).
    f32 ringWeight[8] = {};
    const size_t used = std::min<size_t>(rings, 8);
    if (speed <= set.rings[0].speed) {
        const f32 t = set.rings[0].speed > 1e-4f ? speed / set.rings[0].speed : 1.0f;
        ringWeight[0] = t;
        out.idleWeight = set.idle.isValid() ? 1.0f - t : 0.0f;
        if (!set.idle.isValid()) ringWeight[0] = 1.0f;
    } else if (speed >= set.rings[used - 1].speed) {
        ringWeight[used - 1] = 1.0f;
    } else {
        size_t k = 0;
        while (k + 1 < used && set.rings[k + 1].speed < speed) ++k;
        const f32 span = set.rings[k + 1].speed - set.rings[k].speed;
        const f32 t = span > 1e-4f ? (speed - set.rings[k].speed) / span : 1.0f;
        ringWeight[k] = 1.0f - t;
        ringWeight[k + 1] = t;
    }
    for (size_t r = 0; r < used; ++r) evaluateRing(set, set.rings[r], theta, ringWeight[r], out);

    f32 speedSum = 0.0f, durationSum = 0.0f;
    for (u32 i = 0; i < out.count; ++i) {
        const MoveEntry& e = set.entries[out.items[i].entry];
        out.moveWeight += out.items[i].weight;
        speedSum += out.items[i].weight * e.speed;
        durationSum += out.items[i].weight * e.duration;
    }
    if (out.moveWeight > 1e-4f) {
        out.blendedSpeed = speedSum / out.moveWeight;
        out.blendedDuration = durationSum / out.moveWeight;
    }
    return out;
}

const TurnEntry* PickTurn(const MoveSet& set, f32 angle) {
    const TurnEntry* best = nullptr;
    f32 bestError = 1e9f;
    for (const TurnEntry& t : set.turns) {
        if (t.yaw * angle <= 0.0f) continue; // turns the other way
        const f32 error = std::abs(t.yaw - angle);
        if (error < bestError) {
            bestError = error;
            best = &t;
        }
    }
    return best;
}

// ---- building ------------------------------------------------------------------------------------

bool BuildMoveSet(const AnimationLibrary& library, const LocomotionClips& clips, MoveSet& out) {
    out = MoveSet{};
    auto clipOf = [&](const AssetId& id) { return id.isValid() ? library.clip(id) : nullptr; };

    if (const ClipAsset* idle = clipOf(clips.idleClip)) {
        out.idle = clips.idleClip;
        out.idleDuration = std::max(idle->duration(), 0.1f);
    }
    if (const ClipAsset* jump = clipOf(clips.jumpClip)) {
        out.jump = clips.jumpClip;
        out.jumpDuration = std::max(jump->duration(), 0.1f);
    }

    for (const AssetId& id : {clips.walkClip, clips.runClip, clips.sprintClip, clips.walkBackClip, clips.runBackClip, clips.walkLeftClip,
                              clips.walkRightClip, clips.runLeftClip, clips.runRightClip}) {
        const ClipAsset* clip = clipOf(id);
        if (!clip || clip->duration() <= 0.0f) continue;
        MoveEntry e;
        e.clip = id;
        const Vec3 d = clip->rootCurve.size() >= 2 ? clip->rootCurve.back() - clip->rootCurve.front() : Vec3(0.0f);
        e.velocity = Vec2(d.x, d.z) / clip->duration();
        e.speed = glm::length(e.velocity);
        if (e.speed < 0.2f) continue; // no (usable) root motion: the clip cannot be placed in the blend space
        e.angle = std::atan2(e.velocity.x, e.velocity.y);
        e.duration = clip->duration();
        e.syncOffset = clip->hasSync ? clip->leftFootPhase : 0.0f;
        out.entries.push_back(e);
    }

    // Rings: sort by speed, start a new ring when a clip is clearly faster than the ring so far.
    std::vector<u32> bySpeed(out.entries.size());
    for (u32 i = 0; i < bySpeed.size(); ++i) bySpeed[i] = i;
    std::sort(bySpeed.begin(), bySpeed.end(), [&](u32 a, u32 b) { return out.entries[a].speed < out.entries[b].speed; });
    for (u32 index : bySpeed) {
        const f32 speed = out.entries[index].speed;
        if (out.rings.empty() || speed > out.rings.back().speed * kRingSpeedRatio) out.rings.emplace_back();
        MoveRing& ring = out.rings.back();
        const f32 count = static_cast<f32>(ring.entries.size());
        ring.speed = (ring.speed * count + speed) / (count + 1.0f);
        ring.entries.push_back(index);
    }
    for (MoveRing& ring : out.rings)
        std::sort(ring.entries.begin(), ring.entries.end(), [&](u32 a, u32 b) { return out.entries[a].angle < out.entries[b].angle; });

    for (const AssetId& id : {clips.turnLeftClip, clips.turnRightClip, clips.turnLeft90Clip, clips.turnRight90Clip}) {
        const ClipAsset* clip = clipOf(id);
        if (!clip || clip->duration() <= 0.0f || std::abs(clip->rootYaw()) < 0.3f) continue;
        out.turns.push_back({id, clip->rootYaw(), clip->duration()});
    }
    return out.usable();
}

} // namespace foh
