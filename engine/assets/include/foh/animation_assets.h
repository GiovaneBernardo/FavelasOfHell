#pragma once

// Skeleton and animation clip assets (ADR 0009). CPU forms plus their container (de)serialization.
//
//   Skeleton   META (name, joints: name hash + name, parent, rest TRS, inverse bind)               DEPS
//   Clip       META (name, rate, frames, flags, root motion, sync)  TRAK (per-joint channels)      DEPS
//
// A clip does not depend on a skeleton asset: its tracks are addressed by the hash of the joint name, so a
// clip imported from one file animates any skeleton with the same joint names (the Mixamo workflow, where every
// clip is its own file). Joint names are normalised on import (see NormalizeJointName).
//
// Thread-safety: plain data; (de)serialization functions are pure.

#include <cmath>
#include <string>
#include <string_view>
#include <vector>

#include "foh/asset_container.h"
#include "foh/math.h"
#include "foh/platform.h"
#include "foh/types.h"

namespace foh {

inline constexpr u32 kMaxSkeletonJoints = 255; // vertex joint indices are one byte

// Removes the namespace prefix exporters add ("mixamorig:Hips" -> "Hips", "Armature|Hips" -> "Hips") so that
// clips and skeletons from different files meet on the same names. Case is kept.
FOH_API std::string NormalizeJointName(std::string_view name);
FOH_API u64 JointNameHash(std::string_view normalizedName);

// ---- skeleton ------------------------------------------------------------------------------

struct SkeletonAsset {
    std::string name;
    // Parallel arrays, one entry per joint. Parents always precede their children (one forward pass computes the
    // pose); a joint without a parent has -1.
    std::vector<std::string> jointNames;
    std::vector<u64> jointHashes;
    std::vector<i32> parents;
    std::vector<Vec3> restPosition;
    std::vector<Quat> restRotation;
    std::vector<Vec3> restScale;
    // Model space -> joint space at the bind pose. Skinning matrix = jointModelPose * inverseBind.
    std::vector<Mat4> inverseBind;

    u32 jointCount() const { return static_cast<u32>(parents.size()); }
    i32 findJoint(u64 hash) const {
        for (u32 i = 0; i < jointHashes.size(); ++i)
            if (jointHashes[i] == hash) return static_cast<i32>(i);
        return -1;
    }
    i32 findJoint(std::string_view normalizedName) const { return findJoint(JointNameHash(normalizedName)); }
};

// ---- animation clip ------------------------------------------------------------------------

// A channel is sampled at a fixed rate (ClipAsset::frameRate) and stored as reduced keys: `frames` holds the
// frame index of each key (ascending), values are interpolated linearly (quaternions: shortest-arc nlerp)
// between neighbours. No frames and one value = constant for the whole clip.
struct QuatChannel {
    std::vector<u16> frames;
    std::vector<Quat> values;
    bool constant() const { return frames.empty(); }
};
struct VecChannel {
    std::vector<u16> frames;
    std::vector<Vec3> values;
    bool constant() const { return frames.empty(); }
};

struct ClipTrack {
    u64 jointHash = 0;
    std::string jointName; // diagnostics only
    bool hasRotation = false, hasTranslation = false, hasScale = false; // channels the clip animates; the rest comes from the pose below it
    QuatChannel rotation;
    VecChannel translation;
    VecChannel scale;
};

// How the root (hips) motion was split off the pose. Planar: translation on X/Z and rotation about Y.
//   None    nothing extracted, the pose keeps all motion (idles, additive layers)
//   Linear  the average planar velocity and yaw rate; the pelvis sway stays in the pose (looping locomotion)
//   Full    the exact curve; the pose is fully in place (turns, jumps, one-shots)
enum class RootMotionMode : u8 { None, Linear, Full };

struct ClipAsset {
    std::string name;
    f32 frameRate = 30.0f;
    u32 frameCount = 1; // samples, including the closing sample of a loop
    bool loops = false;
    // The joint whose planar motion became root motion (hash 0: none).
    u64 rootJointHash = 0;
    // Height of the root joint above the ground in the source skeleton's rest pose (meters). The root joint's
    // translation is scaled by (target skeleton root height / this) when the clip plays on another character.
    f32 sourceRootHeight = 0.0f;
    RootMotionMode rootMode = RootMotionMode::None;
    // Cumulative root motion per frame in the clip's start frame: x/z in meters, y = yaw in radians (unwrapped).
    // Empty when rootMode is None. frame 0 is always (0, 0, 0).
    std::vector<Vec3> rootCurve;
    // Foot plant landmarks as a fraction of the loop (0..1), for phase synchronised blending. Valid when hasSync.
    bool hasSync = false;
    f32 leftFootPhase = 0.0f, rightFootPhase = 0.0f;
    std::vector<ClipTrack> tracks;

    // Time of the last sample. A looping clip wraps at this time (the last sample equals the first).
    f32 duration() const { return frameCount > 1 ? static_cast<f32>(frameCount - 1) / frameRate : 0.0f; }
    // Mean planar speed of the root in m/s (0 without root motion).
    f32 rootSpeed() const {
        if (rootCurve.size() < 2 || duration() <= 0.0f) return 0.0f;
        const Vec3 d = rootCurve.back() - rootCurve.front();
        return std::sqrt(d.x * d.x + d.z * d.z) / duration();
    }
    // Total yaw over the clip in radians.
    f32 rootYaw() const { return rootCurve.size() < 2 ? 0.0f : rootCurve.back().y - rootCurve.front().y; }
};

// ---- serialization -------------------------------------------------------------------------

FOH_API std::vector<u8> SerializeSkeleton(const SkeletonAsset& skeleton, AssetId id);
FOH_API bool DeserializeSkeleton(const ContainerReader& reader, SkeletonAsset& out, std::string* error = nullptr);

// Rotations are stored as 16-bit smallest-three quaternions (angle error below 0.003 degrees), translation and
// scale as float32 keys.
FOH_API std::vector<u8> SerializeClip(const ClipAsset& clip, AssetId id);
FOH_API bool DeserializeClip(const ContainerReader& reader, ClipAsset& out, std::string* error = nullptr);

} // namespace foh
