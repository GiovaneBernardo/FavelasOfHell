#pragma once

// Pose math (ADR 0009): local-space poses, clip sampling, weighted blending, root motion, skinning matrices.
// Everything here is a pure function over caller-provided memory: no allocation, no hidden state, safe to call from
// any job thread on distinct data.
//
// Conventions: a pose is one JointPose per skeleton joint, in the skeleton's joint order, in the parent's space.
// Rotations are unit quaternions; blending uses shortest-arc normalised lerp (the hemisphere of the first pose wins).

#include <span>
#include <vector>

#include "foh/animation_assets.h"
#include "foh/math.h"
#include "foh/platform.h"
#include "foh/renderer/components.h"
#include "foh/types.h"

namespace foh {

struct JointPose {
    Vec3 position{0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f};
};

// A clip resolved against one skeleton: which joint each track drives.
struct ClipBinding {
    std::vector<i32> trackJoint; // per clip track; -1 when the skeleton has no such joint (the track is ignored)
    i32 rootJoint = -1;          // the joint that carried the clip's root motion
    f32 rootTranslationScale = 1.0f; // target root height / source root height
};

FOH_API void SetRestPose(const SkeletonAsset& skeleton, std::span<JointPose> out);

// Resolves track names against the skeleton. `rootHeight` is the model-space height of the skeleton's root joint at rest.
FOH_API ClipBinding BindClip(const SkeletonAsset& skeleton, const ClipAsset& clip);

// Height of a joint above the model origin in the skeleton's rest pose.
FOH_API f32 RestHeight(const SkeletonAsset& skeleton, i32 joint);

// Maps a time to a time inside the clip: looping clips wrap, others clamp to [0, duration].
FOH_API f32 WrapClipTime(const ClipAsset& clip, f32 time);

// Overwrites the channels the clip animates; joints and channels it does not touch keep what `pose` holds (start
// from SetRestPose). `time` is wrapped/clamped like WrapClipTime.
FOH_API void SampleClip(const ClipAsset& clip, const ClipBinding& binding, f32 time, std::span<JointPose> pose);

// ---- blending ------------------------------------------------------------------------------------
// Weighted sum of poses: BlendInit with the first, BlendAdd for the others, BlendFinish normalises. The accumulator
// holds un-normalised rotations until BlendFinish, so it must not be used as a pose in between.
FOH_API void BlendInit(std::span<JointPose> accumulator, std::span<const JointPose> first, f32 weight);
FOH_API void BlendAdd(std::span<JointPose> accumulator, std::span<const JointPose> pose, f32 weight);
FOH_API void BlendFinish(std::span<JointPose> accumulator, f32 totalWeight);
// result = lerp(a, b, t) joint by joint (shortest arc).
FOH_API void BlendTwo(std::span<const JointPose> a, std::span<const JointPose> b, f32 t, std::span<JointPose> out);

// ---- root motion ---------------------------------------------------------------------------------

// Planar movement relative to the heading at the start of the interval: translation in the character's local frame
// (x = its +X, y = its +Z), yaw in radians about +Y (positive turns +Z toward +X).
struct RootDelta {
    Vec2 translation{0.0f};
    f32 yaw = 0.0f;
};

// Cumulative root motion at `time` in the clip's start frame (x, yaw, z), interpolated. A clip without root motion
// gives zero.
FOH_API Vec3 SampleRootCurve(const ClipAsset& clip, f32 time);

// Root motion between two times of a clip (t1 may pass the end of a looping clip: the movement continues from the start).
FOH_API RootDelta RootMotionBetween(const ClipAsset& clip, f32 t0, f32 t1);

// ---- skinning ------------------------------------------------------------------------------------

// palette[i] = jointModelPose[i] * skeleton.inverseBind[i], the matrix the skinning pass applies to bind-pose vertices.
FOH_API void ComputeSkinPalette(const SkeletonAsset& skeleton, std::span<const JointPose> pose, std::span<SkinMatrix> out);
// Model-space transforms only (for IK, attachments, tests): 3x4 rows as in SkinMatrix.
FOH_API void ComputeModelPose(const SkeletonAsset& skeleton, std::span<const JointPose> pose, std::span<SkinMatrix> out);

} // namespace foh
