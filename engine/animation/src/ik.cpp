#include "foh/animation/ik.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace foh {

namespace {

std::string lowerCase(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Model-space rotation and position of every joint (scale is ignored: skeletons here are rigid).
struct Globals {
    std::array<Quat, kMaxSkeletonJoints> rotation;
    std::array<Vec3, kMaxSkeletonJoints> position;
};

void computeGlobals(const SkeletonAsset& skeleton, std::span<const JointPose> pose, Globals& g) {
    const size_t n = std::min<size_t>(skeleton.jointCount(), pose.size());
    for (size_t i = 0; i < n; ++i) {
        const i32 p = skeleton.parents[i];
        if (p < 0) {
            g.rotation[i] = pose[i].rotation;
            g.position[i] = pose[i].position;
        } else {
            g.rotation[i] = g.rotation[static_cast<size_t>(p)] * pose[i].rotation;
            g.position[i] = g.position[static_cast<size_t>(p)] + g.rotation[static_cast<size_t>(p)] * pose[i].position;
        }
    }
}

// Rotation taking unit vector u to unit vector v along the shortest arc.
Quat fromTo(Vec3 u, Vec3 v) {
    const f32 lu = glm::length(u), lv = glm::length(v);
    if (lu < 1e-6f || lv < 1e-6f) return Quat(1.0f, 0.0f, 0.0f, 0.0f);
    u /= lu;
    v /= lv;
    const f32 d = glm::dot(u, v);
    if (d > 0.999999f) return Quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (d < -0.999999f) {
        Vec3 axis = glm::cross(Vec3(1.0f, 0.0f, 0.0f), u);
        if (glm::length(axis) < 1e-3f) axis = glm::cross(Vec3(0.0f, 1.0f, 0.0f), u);
        return glm::angleAxis(math::kPi, glm::normalize(axis));
    }
    const Vec3 c = glm::cross(u, v);
    return glm::normalize(Quat(1.0f + d, c.x, c.y, c.z));
}

Quat nlerpQuat(const Quat& a, Quat b, f32 t) {
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f) b = Quat(-b.w, -b.x, -b.y, -b.z);
    return glm::normalize(Quat(a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t));
}

} // namespace

RigInfo FindRig(const SkeletonAsset& skeleton) {
    RigInfo rig;
    for (u32 j = 0; j < skeleton.jointCount(); ++j) {
        const std::string n = lowerCase(skeleton.jointNames[j]);
        const i32 id = static_cast<i32>(j);
        if (n == "hips" && rig.hips < 0) rig.hips = id;
        else if (n == "leftupleg" || n == "upleg_l" || n == "thigh_l" || n == "leftthigh") rig.upperLeg[0] = id;
        else if (n == "rightupleg" || n == "upleg_r" || n == "thigh_r" || n == "rightthigh") rig.upperLeg[1] = id;
        else if (n == "leftleg" || n == "leg_l" || n == "calf_l" || n == "leftcalf" || n == "leftshin") rig.lowerLeg[0] = id;
        else if (n == "rightleg" || n == "leg_r" || n == "calf_r" || n == "rightcalf" || n == "rightshin") rig.lowerLeg[1] = id;
        else if (n == "leftfoot" || n == "foot_l") rig.foot[0] = id;
        else if (n == "rightfoot" || n == "foot_r") rig.foot[1] = id;
        else if (n == "spine") rig.spine[0] = id;
        else if (n == "spine1") rig.spine[1] = id;
        else if (n == "spine2") rig.spine[2] = id;
    }
    return rig;
}

bool SolveTwoBoneLeg(const SkeletonAsset& skeleton, std::span<JointPose> pose, i32 upper, i32 lower, i32 foot, const Vec3& target,
                     const Vec3& poleHint, f32 weight) {
    const i32 count = static_cast<i32>(std::min<size_t>(skeleton.jointCount(), pose.size()));
    if (upper < 0 || lower < 0 || foot < 0 || upper >= count || lower >= count || foot >= count) return false;
    if (skeleton.parents[static_cast<size_t>(lower)] != upper || skeleton.parents[static_cast<size_t>(foot)] != lower) return false;

    Globals g;
    computeGlobals(skeleton, pose, g);
    const size_t U = static_cast<size_t>(upper), L = static_cast<size_t>(lower), F = static_cast<size_t>(foot);
    const Vec3 A = g.position[U], B = g.position[L], C = g.position[F];
    const f32 a = glm::length(B - A), b = glm::length(C - B);
    if (a < 1e-4f || b < 1e-4f) return false;

    const Vec3 toTarget = target - A;
    const f32 reach = glm::length(toTarget);
    if (reach < 1e-5f) return false;
    const Vec3 dir = toTarget / reach;
    const f32 d = std::clamp(reach, std::abs(a - b) + 1e-3f, a + b - 1e-3f);

    // Knee position: on the circle of points at distance a from the hip and b from the target, on the pole's side.
    const f32 x = (a * a - b * b + d * d) / (2.0f * d);
    const f32 h = std::sqrt(std::max(a * a - x * x, 0.0f));
    Vec3 pole = poleHint - dir * glm::dot(poleHint, dir);
    if (glm::length(pole) < 1e-4f) pole = Vec3(0.0f, 0.0f, 1.0f) - dir * dir.z;
    if (glm::length(pole) < 1e-4f) pole = Vec3(1.0f, 0.0f, 0.0f);
    const Vec3 newB = A + dir * x + glm::normalize(pole) * h;

    // Upper bone: swing its model-space rotation so the knee lands on newB. Lower bone: swing so the ankle lands on the
    // reachable point of the target ray. The foot keeps its model-space orientation.
    const Quat footGlobal = g.rotation[F];
    const Quat q1 = fromTo(B - A, newB - A);
    const Quat upperGlobal = q1 * g.rotation[U];
    const Vec3 ankle1 = newB + q1 * (C - B);
    const Vec3 reachable = A + dir * d;
    const Quat q2 = fromTo(ankle1 - newB, reachable - newB);
    const Quat lowerGlobal = q2 * q1 * g.rotation[L];

    const i32 parentOfUpper = skeleton.parents[U];
    const Quat parentGlobal = parentOfUpper >= 0 ? g.rotation[static_cast<size_t>(parentOfUpper)] : Quat(1.0f, 0.0f, 0.0f, 0.0f);
    const Quat newUpperLocal = glm::normalize(glm::inverse(parentGlobal) * upperGlobal);
    const Quat newLowerLocal = glm::normalize(glm::inverse(upperGlobal) * lowerGlobal);
    const Quat newFootLocal = glm::normalize(glm::inverse(lowerGlobal) * footGlobal);
    const f32 w = std::clamp(weight, 0.0f, 1.0f);
    pose[U].rotation = nlerpQuat(pose[U].rotation, newUpperLocal, w);
    pose[L].rotation = nlerpQuat(pose[L].rotation, newLowerLocal, w);
    pose[F].rotation = nlerpQuat(pose[F].rotation, newFootLocal, w);
    return true;
}

void ApplyCrouch(const SkeletonAsset& skeleton, const RigInfo& rig, std::span<JointPose> pose, f32 amount, const CrouchSettings& settings) {
    amount = std::clamp(amount, 0.0f, 1.0f);
    if (amount < 1e-3f || !rig.hasLegs() || pose.size() < skeleton.jointCount()) return;

    // Where the animation has the feet, and how the knees bend.
    Globals before;
    computeGlobals(skeleton, pose, before);
    Vec3 footTarget[2], kneePole[2];
    for (int s = 0; s < 2; ++s) {
        const size_t U = static_cast<size_t>(rig.upperLeg[s]), L = static_cast<size_t>(rig.lowerLeg[s]), F = static_cast<size_t>(rig.foot[s]);
        footTarget[s] = before.position[F];
        const Vec3 axis = glm::normalize(footTarget[s] - before.position[U]);
        kneePole[s] = before.position[L] - before.position[U];
        kneePole[s] -= axis * glm::dot(kneePole[s], axis);
        if (glm::length(kneePole[s]) < 0.01f) kneePole[s] = Vec3(0.0f, 0.0f, 1.0f); // straight leg: knees forward
    }

    // Spine: lean forward about the model's side axis, bone by bone from the pelvis up.
    int spineJoints = 0;
    for (const i32 j : rig.spine) spineJoints += j >= 0;
    if (spineJoints > 0) {
        const f32 each = settings.spineLean * amount / static_cast<f32>(spineJoints);
        for (const i32 j : rig.spine) {
            if (j < 0) continue;
            Globals g;
            computeGlobals(skeleton, pose, g);
            const size_t J = static_cast<size_t>(j);
            const i32 p = skeleton.parents[J];
            const Quat parentGlobal = p >= 0 ? g.rotation[static_cast<size_t>(p)] : Quat(1.0f, 0.0f, 0.0f, 0.0f);
            // The character faces +Z with +X to its left: leaning forward is a rotation about +X.
            const Quat newGlobal = glm::angleAxis(each, Vec3(1.0f, 0.0f, 0.0f)) * g.rotation[J];
            pose[J].rotation = glm::normalize(glm::inverse(parentGlobal) * newGlobal);
        }
    }

    // Hips down and back (in model space).
    {
        Globals g;
        computeGlobals(skeleton, pose, g);
        const size_t H = static_cast<size_t>(rig.hips);
        const i32 p = skeleton.parents[H];
        const Quat parentGlobal = p >= 0 ? g.rotation[static_cast<size_t>(p)] : Quat(1.0f, 0.0f, 0.0f, 0.0f);
        const Vec3 delta(0.0f, -settings.hipDrop * amount, -settings.hipBack * amount);
        pose[H].position += glm::inverse(parentGlobal) * delta;
    }

    // Legs: plant the feet where they were; the knees fold.
    for (int s = 0; s < 2; ++s)
        SolveTwoBoneLeg(skeleton, pose, rig.upperLeg[s], rig.lowerLeg[s], rig.foot[s], footTarget[s], kneePole[s], 1.0f);
}

} // namespace foh
