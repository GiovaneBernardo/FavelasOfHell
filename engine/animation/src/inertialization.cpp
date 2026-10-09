#include "foh/animation/inertialization.h"

#include <algorithm>
#include <cmath>

#include "foh/animation/spring.h"

namespace foh {

Vec3 QuatLog(Quat q) {
    if (q.w < 0.0f) q = Quat(-q.w, -q.x, -q.y, -q.z);
    const Vec3 v(q.x, q.y, q.z);
    const f32 s = glm::length(v);
    if (s < 1e-6f) return v * 2.0f;
    return v / s * (2.0f * std::atan2(s, q.w));
}

Quat QuatExp(const Vec3& r) {
    const f32 angle = glm::length(r);
    if (angle < 1e-6f) return glm::normalize(Quat(1.0f, r.x * 0.5f, r.y * 0.5f, r.z * 0.5f));
    return glm::angleAxis(angle, r / angle);
}

namespace {

Vec3 clampVelocity(const Vec3& x0, const Vec3& v0, f32 halfLife) {
    return Vec3(spring::clampStartVelocity(x0.x, v0.x, halfLife), spring::clampStartVelocity(x0.y, v0.y, halfLife),
                spring::clampStartVelocity(x0.z, v0.z, halfLife));
}

Vec3 decayed(const Vec3& x0, const Vec3& v0, f32 halfLife, f32 t) {
    return Vec3(spring::decayValue(x0.x, v0.x, halfLife, t), spring::decayValue(x0.y, v0.y, halfLife, t),
                spring::decayValue(x0.z, v0.z, halfLife, t));
}

} // namespace

void InertializeStart(std::span<JointHistory> history, std::span<const JointPose> now, std::span<const JointPose> previous,
                      f32 dt, f32 halfLife) {
    const size_t n = std::min({history.size(), now.size(), previous.size()});
    const f32 invDt = dt > 1e-5f ? 1.0f / dt : 0.0f;
    for (size_t i = 0; i < n; ++i) {
        JointHistory& h = history[i];
        // Where the character is (its last output) against where the new animation was one frame ago: the gap that has
        // to disappear. Velocities are measured the same way on both sides.
        const Vec3 x0 = h.prevPos - previous[i].position;
        const Vec3 targetVelocity = (now[i].position - previous[i].position) * invDt;
        h.offPos = x0;
        h.offPosVel = clampVelocity(x0, h.velPos - targetVelocity, halfLife);

        const Vec3 r0 = QuatLog(h.prevRot * glm::inverse(previous[i].rotation));
        const Vec3 targetSpin = QuatLog(now[i].rotation * glm::inverse(previous[i].rotation)) * invDt;
        h.offRot = r0;
        h.offRotVel = clampVelocity(r0, h.velRot - targetSpin, halfLife);
    }
}

void InertializeApply(std::span<const JointHistory> history, f32 elapsed, f32 halfLife, std::span<JointPose> pose) {
    const size_t n = std::min(history.size(), pose.size());
    for (size_t i = 0; i < n; ++i) {
        const JointHistory& h = history[i];
        pose[i].position += decayed(h.offPos, h.offPosVel, halfLife, elapsed);
        pose[i].rotation = glm::normalize(QuatExp(decayed(h.offRot, h.offRotVel, halfLife, elapsed)) * pose[i].rotation);
    }
}

void RecordHistory(std::span<JointHistory> history, std::span<const JointPose> pose, f32 dt, bool valid) {
    const size_t n = std::min(history.size(), pose.size());
    const f32 invDt = valid && dt > 1e-5f ? 1.0f / dt : 0.0f;
    for (size_t i = 0; i < n; ++i) {
        JointHistory& h = history[i];
        h.velPos = (pose[i].position - h.prevPos) * invDt;
        h.velRot = QuatLog(pose[i].rotation * glm::inverse(h.prevRot)) * invDt;
        h.prevPos = pose[i].position;
        h.prevRot = pose[i].rotation;
    }
}

} // namespace foh
