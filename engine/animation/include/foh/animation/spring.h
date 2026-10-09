#pragma once

// Critically damped springs with exact (closed form) integration: stable for any time step, no overshoot, the
// "half-life" is the time to cover half the remaining distance (Holden, "Spring-It-On").
//
// These smooth everything that must not snap: desired velocity, desired heading, camera-like follow values. They
// are plain inline functions; thread-safe on distinct data.

#include <algorithm>
#include <cmath>

#include "foh/math.h"
#include "foh/types.h"

namespace foh {

namespace spring {

inline constexpr f32 kLn2 = 0.69314718056f;

// Damping coefficient y of exp(-y t) for a critically damped spring with this half-life.
inline f32 decayRate(f32 halfLife) { return 2.0f * kLn2 / std::max(halfLife, 1e-5f); }

// Moves x toward `goal` (goal velocity zero) for dt seconds, updating the velocity v.
inline void step(f32& x, f32& v, f32 goal, f32 halfLife, f32 dt) {
    const f32 y = decayRate(halfLife);
    const f32 j0 = x - goal;
    const f32 j1 = v + j0 * y;
    const f32 e = std::exp(-y * dt);
    x = e * (j0 + j1 * dt) + goal;
    v = e * (v - j1 * y * dt);
}

inline void step(Vec3& x, Vec3& v, const Vec3& goal, f32 halfLife, f32 dt) {
    step(x.x, v.x, goal.x, halfLife, dt);
    step(x.y, v.y, goal.y, halfLife, dt);
    step(x.z, v.z, goal.z, halfLife, dt);
}

// Offset that decays to zero: value after `t` seconds of a critically damped spring that starts at (x0, v0) and
// aims at zero. Used by inertialization: x(t) = e^(-y t) (x0 + (v0 + y x0) t).
inline f32 decayValue(f32 x0, f32 v0, f32 halfLife, f32 t) {
    const f32 y = decayRate(halfLife);
    return std::exp(-y * t) * (x0 + (v0 + y * x0) * t);
}

// A start velocity that would make the offset cross zero (and swing to the other side) is replaced by the fastest
// one that does not: the offset then only ever shrinks.
inline f32 clampStartVelocity(f32 x0, f32 v0, f32 halfLife) {
    const f32 j1 = v0 + decayRate(halfLife) * x0;
    return j1 * x0 < 0.0f ? -decayRate(halfLife) * x0 : v0;
}

} // namespace spring

// Signed shortest angle from `from` to `to`, in (-pi, pi].
inline f32 AngleDelta(f32 from, f32 to) {
    f32 d = std::fmod(to - from, 2.0f * math::kPi);
    if (d > math::kPi) d -= 2.0f * math::kPi;
    if (d <= -math::kPi) d += 2.0f * math::kPi;
    return d;
}

} // namespace foh
