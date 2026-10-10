#pragma once

// Procedural pose layers that sit on top of the animation (ADR 0009): two-bone leg IK and a crouch built from it.
//
// A crouch without crouch clips: the hips drop, the spine leans forward, and both legs are re-solved so the feet stay
// exactly where the animation put them (planted feet stay planted, a walk keeps its steps), the knees folding to take
// up the difference. With real crouch clips the layer is simply not used.
//
// Everything works on a local pose (JointPose per skeleton joint) and finds the bones by name (RigInfo), so it works
// on any skeleton that names its limbs like Mixamo / HumanIK ("LeftUpLeg", "LeftLeg", "LeftFoot", "Hips", "Spine"...).
//
// Thread-safety: pure functions over caller memory.

#include <span>

#include "foh/animation/pose.h"
#include "foh/animation_assets.h"

namespace foh {

struct RigInfo {
    i32 hips = -1;
    i32 upperLeg[2] = {-1, -1}; // [0] left, [1] right
    i32 lowerLeg[2] = {-1, -1};
    i32 foot[2] = {-1, -1};
    i32 spine[3] = {-1, -1, -1};
    bool hasLegs() const { return hips >= 0 && upperLeg[0] >= 0 && lowerLeg[0] >= 0 && foot[0] >= 0 && upperLeg[1] >= 0 && lowerLeg[1] >= 0 && foot[1] >= 0; }
};

// Finds the limbs by (normalised) joint name. Missing parts stay -1.
FOH_API RigInfo FindRig(const SkeletonAsset& skeleton);

// Rotates the two bones of a leg so the foot reaches `target` (model space), the knee bending toward `poleHint` (a
// direction in model space). The foot keeps its model-space orientation. `pose` is a local pose, modified in place.
// `weight` 0..1 blends from the current pose to the solved one. Returns false when the joints are invalid.
FOH_API bool SolveTwoBoneLeg(const SkeletonAsset& skeleton, std::span<JointPose> pose, i32 upper, i32 lower, i32 foot, const Vec3& target,
                             const Vec3& poleHint, f32 weight = 1.0f);

struct CrouchSettings {
    f32 hipDrop = 0.42f;   // meters the pelvis goes down at full crouch (for a ~1.8 m character)
    f32 hipBack = 0.10f;   // meters it moves back (toward -Z, the character's back) so the weight stays over the feet
    f32 spineLean = 0.45f; // radians the spine leans forward in total
};

// amount 0..1. Hips, spine and both legs are modified; the feet stay where the pose had them.
FOH_API void ApplyCrouch(const SkeletonAsset& skeleton, const RigInfo& rig, std::span<JointPose> pose, f32 amount,
                         const CrouchSettings& settings = {});

} // namespace foh
