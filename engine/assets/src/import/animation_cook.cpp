// Cooking of skeletons and animation clips (ADR 0009). Everything here is independent of the source format: the
// parsers deliver densely sampled local transforms and this file turns them into the compact runtime clip.
//
// Pipeline for a clip:
//   1. normalise joint names (clips bind to skeletons by name hash)
//   2. loop detection (first pose == last pose)
//   3. root motion: the root joint's (hips) planar translation and yaw leave the pose and become a curve, so the
//      remaining pose is "in place" and can be driven by gameplay speed or by the curve
//   4. foot plant landmarks (looping locomotion), used to phase-align blends
//   5. key reduction under per-channel error bounds
#include <algorithm>
#include <cmath>
#include <set>

#include "foh/asset_import.h"

namespace foh {

namespace {

constexpr f32 kPi = 3.14159265358979f;

Mat4 composeTrs(const Vec3& t, const Quat& q, const Vec3& s) {
    return glm::translate(Mat4(1.0f), t) * glm::mat4_cast(q) * glm::scale(Mat4(1.0f), s);
}

std::string lowerCase(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Angle between two rotations in radians, from the relative rotation a * conj(b) in double precision. (Taking acos of
// the dot product instead has a noise floor of ~0.03 degrees for float quaternions: acos is flat near 1, and the
// reduction tolerance is of that order.)
double angleBetween(const Quat& a, const Quat& b) {
    const double ax = a.x, ay = a.y, az = a.z, aw = a.w, bx = -static_cast<double>(b.x), by = -static_cast<double>(b.y),
                 bz = -static_cast<double>(b.z), bw = b.w;
    const double w = aw * bw - ax * bx - ay * by - az * bz;
    const double x = aw * bx + ax * bw + ay * bz - az * by;
    const double y = aw * by - ax * bz + ay * bw + az * bx;
    const double z = aw * bz + ax * by - ay * bx + az * bw;
    return 2.0 * std::atan2(std::sqrt(x * x + y * y + z * z), std::abs(w));
}

Quat nlerpShortest(const Quat& a, Quat b, f32 t) {
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f) b = Quat(-b.w, -b.x, -b.y, -b.z);
    return glm::normalize(Quat(a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t));
}

// Recursive (iterative) Ramer-Douglas-Peucker style reduction: keep the endpoints, then repeatedly keep the sample
// that the interpolation between the current neighbouring keys reproduces worst, until all are within tolerance.
template <class T, class ErrorFn, class LerpFn>
void reduceSamples(const std::vector<T>& s, f32 tolerance, ErrorFn error, LerpFn lerp, std::vector<u16>& frames, std::vector<T>& values) {
    frames.clear();
    values.clear();
    if (s.empty()) return;
    bool flat = true;
    for (size_t i = 1; i < s.size() && flat; ++i) flat = error(s[0], s[i]) <= tolerance;
    if (flat || s.size() == 1) {
        values.push_back(s[0]);
        return;
    }
    std::vector<bool> keep(s.size(), false);
    keep.front() = keep.back() = true;
    struct Span {
        size_t a, b;
    };
    std::vector<Span> stack{{0, s.size() - 1}};
    while (!stack.empty()) {
        const Span span = stack.back();
        stack.pop_back();
        if (span.b - span.a < 2) continue;
        double worst = -1.0;
        size_t worstIndex = 0;
        for (size_t i = span.a + 1; i < span.b; ++i) {
            const f32 t = static_cast<f32>(i - span.a) / static_cast<f32>(span.b - span.a);
            const double e = error(s[i], lerp(s[span.a], s[span.b], t));
            if (e > worst) {
                worst = e;
                worstIndex = i;
            }
        }
        if (worst <= tolerance) continue;
        keep[worstIndex] = true;
        stack.push_back({span.a, worstIndex});
        stack.push_back({worstIndex, span.b});
    }
    for (size_t i = 0; i < s.size(); ++i) {
        if (!keep[i]) continue;
        frames.push_back(static_cast<u16>(i));
        values.push_back(s[i]);
    }
}

// Joint side from the name: -1 left, +1 right, 0 unknown.
int sideOf(const std::string& lower) {
    if (lower.find("left") != std::string::npos) return -1;
    if (lower.find("right") != std::string::npos) return 1;
    auto hasToken = [&](char side) {
        for (size_t i = 0; i < lower.size(); ++i) {
            if (lower[i] != side) continue;
            const bool before = i == 0 || lower[i - 1] == '_' || lower[i - 1] == '.' || lower[i - 1] == '-';
            const bool after = i + 1 == lower.size() || lower[i + 1] == '_' || lower[i + 1] == '.' || lower[i + 1] == '-';
            if (before && after) return true;
        }
        return false;
    };
    if (hasToken('l')) return -1;
    if (hasToken('r')) return 1;
    return 0;
}

struct ClipWork {
    const ImportedSkeleton& skeleton;
    std::vector<ImportedClipTrack> tracks;
    u32 frames;

    Vec3 translation(u32 j, u32 f) const {
        const auto& t = tracks[j].translation;
        return t.empty() ? skeleton.restPosition[j] : t[std::min<size_t>(f, t.size() - 1)];
    }
    Quat rotation(u32 j, u32 f) const {
        const auto& r = tracks[j].rotation;
        return r.empty() ? skeleton.restRotation[j] : r[std::min<size_t>(f, r.size() - 1)];
    }
    Vec3 scale(u32 j, u32 f) const {
        const auto& s = tracks[j].scale;
        return s.empty() ? skeleton.restScale[j] : s[std::min<size_t>(f, s.size() - 1)];
    }
    Mat4 local(u32 j, u32 f) const { return composeTrs(translation(j, f), rotation(j, f), scale(j, f)); }
    Mat4 model(u32 j, u32 f) const {
        Mat4 m = local(j, f);
        for (i32 p = skeleton.parents[j]; p >= 0; p = skeleton.parents[static_cast<size_t>(p)]) m = local(static_cast<u32>(p), f) * m;
        return m;
    }
    Mat4 parentModel(u32 j, u32 f) const { return skeleton.parents[j] < 0 ? Mat4(1.0f) : model(static_cast<u32>(skeleton.parents[j]), f); }
};

Quat yawQuat(f32 a) { return glm::angleAxis(a, Vec3(0.0f, 1.0f, 0.0f)); }

// Rotation about the vertical axis contained in `q` (a rotation expressed in model space, Y up), in (-pi, pi].
f32 twistAboutY(Quat q) {
    if (q.w < 0.0f) q = Quat(-q.w, -q.x, -q.y, -q.z);
    return 2.0f * std::atan2(q.y, q.w);
}

i32 pickRootJoint(const ImportedSkeleton& sk, const std::vector<std::string>& lower, const std::string& wanted) {
    if (wanted != "auto") {
        const std::string w = lowerCase(NormalizeJointName(wanted));
        for (size_t j = 0; j < lower.size(); ++j)
            if (lower[j] == w) return static_cast<i32>(j);
    }
    i32 best = -1;
    int bestDepth = 1 << 20;
    for (size_t j = 0; j < lower.size(); ++j) {
        const std::string& n = lower[j];
        if (n != "hips" && n != "pelvis" && n != "root" && n != "hip" && n != "cog" && n != "center") continue;
        int depth = 0;
        for (i32 p = sk.parents[j]; p >= 0; p = sk.parents[static_cast<size_t>(p)]) ++depth;
        if (depth < bestDepth) {
            bestDepth = depth;
            best = static_cast<i32>(j);
        }
    }
    if (best >= 0) return best;
    for (size_t j = 0; j < sk.parents.size(); ++j)
        if (sk.parents[j] < 0) return static_cast<i32>(j);
    return -1;
}

} // namespace

// ---- skeleton --------------------------------------------------------------------------------

SkeletonAsset CookSkeleton(const ImportedSkeleton& source) {
    SkeletonAsset s;
    s.name = source.name;
    const u32 count = static_cast<u32>(source.parents.size());
    std::set<std::string> used;
    std::vector<Mat4> restModel(count);
    for (u32 j = 0; j < count; ++j) {
        std::string name = NormalizeJointName(source.jointNames[j]);
        if (name.empty()) name = "joint" + std::to_string(j);
        if (!used.insert(name).second) {
            name += "#" + std::to_string(j); // duplicate after normalisation: keep both addressable
            used.insert(name);
        }
        s.jointNames.push_back(name);
        s.jointHashes.push_back(JointNameHash(name));
        s.parents.push_back(source.parents[j]);
        s.restPosition.push_back(source.restPosition[j]);
        s.restRotation.push_back(source.restRotation[j]);
        s.restScale.push_back(source.restScale[j]);
        const Mat4 local = composeTrs(source.restPosition[j], source.restRotation[j], source.restScale[j]);
        restModel[j] = source.parents[j] >= 0 ? restModel[static_cast<size_t>(source.parents[j])] * local : local;
        const bool bound = j < source.hasInverseBind.size() && source.hasInverseBind[j];
        s.inverseBind.push_back(bound ? source.inverseBind[j] : glm::inverse(restModel[j]));
    }
    return s;
}

// ---- key reduction ---------------------------------------------------------------------------

void ReduceQuatChannel(std::vector<Quat>& samples, f32 toleranceRadians, QuatChannel& out) {
    reduceSamples<Quat>(
        samples, toleranceRadians, [](const Quat& a, const Quat& b) { return angleBetween(a, b); },
        [](const Quat& a, const Quat& b, f32 t) { return nlerpShortest(a, b, t); }, out.frames, out.values);
}

void ReduceVecChannel(std::vector<Vec3>& samples, f32 tolerance, VecChannel& out) {
    reduceSamples<Vec3>(
        samples, tolerance, [](const Vec3& a, const Vec3& b) { return static_cast<double>(glm::length(a - b)); },
        [](const Vec3& a, const Vec3& b, f32 t) { return a + (b - a) * t; }, out.frames, out.values);
}

// ---- clip ------------------------------------------------------------------------------------

ClipAsset CookClip(const ImportedClip& src, const ImportedSkeleton& sk, const ImportSettings& settings,
                   std::vector<std::string>* warnings) {
    ClipAsset out;
    out.name = src.name;
    out.frameRate = src.frameRate > 0.0f ? src.frameRate : 30.0f;
    out.frameCount = std::max(1u, std::min(src.frameCount, 65536u));
    const u32 jointCount = static_cast<u32>(sk.parents.size());
    auto warn = [&](const std::string& message) {
        if (warnings) warnings->push_back("clip '" + src.name + "': " + message);
    };
    if (src.frameCount > 65536u) warn("longer than 65536 frames, truncated");

    ClipWork work{sk, src.tracks, out.frameCount};
    work.tracks.resize(jointCount);
    for (ImportedClipTrack& t : work.tracks) {
        // A channel shorter than the clip would read past its end; treat it as not animated.
        if (t.translation.size() < out.frameCount) t.translation.clear();
        if (t.rotation.size() < out.frameCount) t.rotation.clear();
        if (t.scale.size() < out.frameCount) t.scale.clear();
    }

    std::vector<std::string> names(jointCount), lower(jointCount);
    for (u32 j = 0; j < jointCount; ++j) {
        names[j] = NormalizeJointName(sk.jointNames[j]);
        lower[j] = lowerCase(names[j]);
    }

    const u32 last = out.frameCount - 1;
    const f32 duration = out.duration();
    const i32 root = pickRootJoint(sk, lower, settings.clipRootJoint);

    // Rest model matrices for the source root height.
    if (root >= 0) {
        Mat4 m = composeTrs(sk.restPosition[static_cast<size_t>(root)], sk.restRotation[static_cast<size_t>(root)], sk.restScale[static_cast<size_t>(root)]);
        for (i32 p = sk.parents[static_cast<size_t>(root)]; p >= 0; p = sk.parents[static_cast<size_t>(p)])
            m = composeTrs(sk.restPosition[static_cast<size_t>(p)], sk.restRotation[static_cast<size_t>(p)], sk.restScale[static_cast<size_t>(p)]) * m;
        out.sourceRootHeight = m[3].y;
        out.rootJointHash = JointNameHash(names[static_cast<size_t>(root)]);
    }

    // ---- loop detection ----
    // Same pose at both ends. The root's planar motion and yaw are ignored (a walk ends somewhere else, facing
    // the same way); its own tilt and height must match like every other joint.
    bool seamless = last > 0;
    if (seamless) {
        for (u32 j = 0; j < jointCount && seamless; ++j) {
            Quat q0 = work.rotation(j, 0), q1 = work.rotation(j, last);
            if (static_cast<i32>(j) == root) {
                const Quat rel = q1 * glm::inverse(q0);
                const f32 turned = twistAboutY(rel);
                // A clip that ends facing elsewhere (a 90 degree turn) is not a cycle even when the pose matches.
                if (std::abs(turned) > 0.35f) seamless = false;
                q1 = yawQuat(-turned) * q1;
            }
            if (angleBetween(q0, q1) > 2.5 * kPi / 180.0) seamless = false;
            if (!work.tracks[j].translation.empty() && static_cast<i32>(j) != root &&
                glm::length(work.translation(j, 0) - work.translation(j, last)) > 0.01f)
                seamless = false;
        }
        if (seamless && root >= 0 && std::abs(work.translation(static_cast<u32>(root), 0).y - work.translation(static_cast<u32>(root), last).y) > 0.02f)
            seamless = false;
    }
    out.loops = settings.clipLoop == "loop" ? true : settings.clipLoop == "once" ? false : seamless;
    if (settings.clipLoop == "loop" && !seamless) warn("forced to loop but the first and last pose differ");

    // ---- root motion ----
    RootMotionMode mode = RootMotionMode::None;
    if (root >= 0 && last > 0) {
        mode = settings.clipRootMotion == "none"     ? RootMotionMode::None
               : settings.clipRootMotion == "linear" ? RootMotionMode::Linear
               : settings.clipRootMotion == "full"   ? RootMotionMode::Full
                                                     : (out.loops ? RootMotionMode::Linear : RootMotionMode::Full);
    }
    if (mode != RootMotionMode::None) {
        const u32 r = static_cast<u32>(root);
        const u32 n = out.frameCount;
        std::vector<Vec3> P(n);
        std::vector<Quat> Q(n);
        std::vector<Mat4> parent(n);
        for (u32 f = 0; f < n; ++f) {
            parent[f] = work.parentModel(r, f);
            const Mat4 m = parent[f] * work.local(r, f);
            P[f] = Vec3(m[3]);
            Mat3 basis(m);
            for (int c = 0; c < 3; ++c) basis[c] = glm::normalize(basis[c]);
            Q[f] = glm::normalize(glm::quat_cast(basis));
        }
        std::vector<f32> yaw(n);
        f32 previous = 0.0f;
        for (u32 f = 0; f < n; ++f) {
            f32 a = twistAboutY(Q[f] * glm::inverse(Q[0]));
            while (a - previous > kPi) a -= 2.0f * kPi; // unwrap: the yaw is a continuous angle
            while (a - previous < -kPi) a += 2.0f * kPi;
            yaw[f] = previous = a;
        }
        const Vec2 total(P[last].x - P[0].x, P[last].z - P[0].z);
        const f32 velocity = glm::length(total) / duration;
        const f32 yawRate = yaw[last] / duration;
        // A clip that barely moves keeps its pose untouched (idles drift by sub-millimeters otherwise).
        const bool negligible = glm::length(total) < 0.02f && std::abs(yaw[last]) < 0.02f;
        if (negligible && settings.clipRootMotion == "auto") {
            mode = RootMotionMode::None;
        } else if (velocity > 100.0f) {
            warn("implausible root speed, root motion disabled");
            mode = RootMotionMode::None;
        }
        if (mode != RootMotionMode::None) {
            out.rootCurve.resize(n);
            auto& tr = work.tracks[r];
            tr.translation.resize(n);
            tr.rotation.resize(n);
            for (u32 f = 0; f < n; ++f) {
                const f32 t = static_cast<f32>(f) / out.frameRate;
                Vec2 rootPos;
                f32 rootYaw;
                if (mode == RootMotionMode::Full) {
                    rootPos = Vec2(P[f].x - P[0].x, P[f].z - P[0].z);
                    rootYaw = yaw[f];
                } else {
                    rootPos = total * (t / duration);
                    rootYaw = yawRate * t;
                }
                out.rootCurve[f] = Vec3(rootPos.x, rootYaw, rootPos.y);

                // What stays in the pose: the vertical motion, and (Linear) the sway around the average path.
                const Vec3 residual = yawQuat(-rootYaw) * Vec3(P[f].x - P[0].x - rootPos.x, 0.0f, P[f].z - P[0].z - rootPos.y);
                const Vec3 posModel(P[0].x + residual.x, P[f].y, P[0].z + residual.z);
                const Quat rotModel = yawQuat(-rootYaw) * Q[f];

                const Mat4 inverseParent = glm::inverse(parent[f]);
                tr.translation[f] = Vec3(inverseParent * Vec4(posModel, 1.0f));
                Mat3 pb(parent[f]);
                for (int c = 0; c < 3; ++c) pb[c] = glm::normalize(pb[c]);
                tr.rotation[f] = glm::normalize(glm::inverse(glm::normalize(glm::quat_cast(pb))) * rotModel);
            }
            out.rootCurve[0] = Vec3(0.0f);
        }
    }
    out.rootMode = mode;
    if (mode == RootMotionMode::None) out.rootCurve.clear();

    // ---- foot plant landmarks ----
    if (out.loops && last >= 4 && out.rootSpeed() > 0.3f) {
        i32 foot[2] = {-1, -1}; // left, right
        for (u32 j = 0; j < jointCount; ++j) {
            if (lower[j].find("foot") == std::string::npos || lower[j].find("toe") != std::string::npos) continue;
            const int side = sideOf(lower[j]);
            if (side != 0 && foot[(side + 1) / 2] < 0) foot[(side + 1) / 2] = static_cast<i32>(j);
        }
        if (foot[0] >= 0 && foot[1] >= 0) {
            const u32 n = last; // the closing sample duplicates frame 0
            f32 phases[2] = {0.0f, 0.0f};
            bool ok = true;
            for (int side = 0; side < 2 && ok; ++side) {
                std::vector<f32> height(n);
                f32 lo = 1e30f, hi = -1e30f;
                for (u32 f = 0; f < n; ++f) {
                    height[f] = work.model(static_cast<u32>(foot[side]), f)[3].y;
                    lo = std::min(lo, height[f]);
                    hi = std::max(hi, height[f]);
                }
                if (hi - lo < 0.02f) {
                    ok = false;
                    break;
                }
                const f32 threshold = lo + 0.25f * (hi - lo);
                std::vector<bool> stance(n);
                u32 stanceCount = 0;
                for (u32 f = 0; f < n; ++f) stanceCount += (stance[f] = height[f] <= threshold);
                if (stanceCount == 0 || stanceCount == n) {
                    ok = false;
                    break;
                }
                // The longest cyclic run of stance frames; the landmark is where it begins (the foot comes down).
                u32 bestStart = 0, bestLength = 0;
                for (u32 f = 0; f < n; ++f) {
                    if (!stance[f] || stance[(f + n - 1) % n]) continue;
                    u32 length = 0;
                    while (length < n && stance[(f + length) % n]) ++length;
                    if (length > bestLength) {
                        bestLength = length;
                        bestStart = f;
                    }
                }
                phases[side] = static_cast<f32>(bestStart) / static_cast<f32>(n);
            }
            if (ok) {
                out.hasSync = true;
                out.leftFootPhase = phases[0];
                out.rightFootPhase = phases[1];
            }
        } else {
            warn("no left/right foot joints found, locomotion sync disabled for this clip");
        }
    }

    // ---- tracks ----
    const f32 rotTolerance = std::max(settings.clipRotationToleranceDeg, 0.0f) * kPi / 180.0f;
    const f32 posTolerance = std::max(settings.clipTranslationTolerance, 0.0f);
    for (u32 j = 0; j < jointCount; ++j) {
        const ImportedClipTrack& in = work.tracks[j];
        if (in.translation.empty() && in.rotation.empty() && in.scale.empty()) continue;
        ClipTrack track;
        track.jointHash = JointNameHash(names[j]);
        track.jointName = names[j];
        if (!in.rotation.empty()) {
            std::vector<Quat> samples(in.rotation.begin(), in.rotation.begin() + out.frameCount);
            ReduceQuatChannel(samples, rotTolerance, track.rotation);
            track.hasRotation = true;
        }
        if (!in.translation.empty()) {
            std::vector<Vec3> samples(in.translation.begin(), in.translation.begin() + out.frameCount);
            VecChannel reduced;
            ReduceVecChannel(samples, posTolerance, reduced);
            // Bone offsets that never change belong to the skeleton (the clip's character may have other
            // proportions); only the root (height) and really animated joints carry translation.
            if (static_cast<i32>(j) == root || !reduced.constant()) {
                track.translation = std::move(reduced);
                track.hasTranslation = true;
            }
        }
        if (!in.scale.empty()) {
            std::vector<Vec3> samples(in.scale.begin(), in.scale.begin() + out.frameCount);
            VecChannel reduced;
            ReduceVecChannel(samples, 1e-4f, reduced);
            if (!reduced.constant() || glm::length(reduced.values[0] - Vec3(1.0f)) > 1e-3f) {
                track.scale = std::move(reduced);
                track.hasScale = true;
            }
        }
        if (track.hasRotation || track.hasTranslation || track.hasScale) out.tracks.push_back(std::move(track));
    }
    return out;
}

} // namespace foh
