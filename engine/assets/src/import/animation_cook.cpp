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
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>
#include <vector>

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

// ---- segments (mocap takes) -----------------------------------------------------------------

std::vector<ClipSegment> ParseClipSegments(const std::string& text, std::vector<std::string>* errors) {
    std::vector<ClipSegment> out;
    size_t pos = 0;
    auto fail = [&](const std::string& item, const char* why) {
        if (errors) errors->push_back("clip segment '" + item + "': " + why);
    };
    while (pos < text.size()) {
        size_t end = text.find_first_of(";\n", pos);
        if (end == std::string::npos) end = text.size();
        std::string item = text.substr(pos, end - pos);
        pos = end + 1;
        // trim
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front()))) item.erase(item.begin());
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back()))) item.pop_back();
        if (item.empty()) continue;
        const size_t eq = item.find('=');
        const size_t dash = item.find('-', eq == std::string::npos ? 0 : eq);
        if (eq == std::string::npos || dash == std::string::npos || dash < eq) {
            fail(item, "expected name=start-end");
            continue;
        }
        ClipSegment seg;
        seg.name = item.substr(0, eq);
        while (!seg.name.empty() && std::isspace(static_cast<unsigned char>(seg.name.back()))) seg.name.pop_back();
        std::string tail = item.substr(dash + 1);
        if (const size_t colon = tail.find(':'); colon != std::string::npos) {
            seg.loop = tail.substr(colon + 1).find("loop") != std::string::npos;
            tail = tail.substr(0, colon);
        }
        seg.start = std::strtof(item.c_str() + eq + 1, nullptr);
        seg.end = std::strtof(tail.c_str(), nullptr);
        if (seg.name.empty() || !(seg.end > seg.start) || seg.start < 0.0f) {
            fail(item, "needs a name and 0 <= start < end");
            continue;
        }
        out.push_back(std::move(seg));
    }
    return out;
}

namespace {

// How badly the pose at `end` fails to continue into the pose at `start`: rotation differences of every joint, the
// height of the hips, and the difference of their velocities (a loop must not change speed at the seam).
double loopCost(const ImportedClip& clip, u32 start, u32 end) {
    double cost = 0.0;
    for (const ImportedClipTrack& t : clip.tracks) {
        if (t.rotation.size() <= end || end < 1 || start + 1 >= t.rotation.size()) continue;
        cost += angleBetween(t.rotation[start], t.rotation[end]);
        cost += 0.5 * angleBetween(t.rotation[start + 1] * glm::inverse(t.rotation[start]), t.rotation[end] * glm::inverse(t.rotation[end - 1]));
    }
    if (!clip.tracks.empty() && clip.tracks[0].translation.size() > end) {
        // joint 0 is the first root of the skeleton: its vertical position is the hips height in practice
        cost += 4.0 * std::abs(static_cast<double>(clip.tracks[0].translation[start].y - clip.tracks[0].translation[end].y));
    }
    return cost;
}

} // namespace

std::vector<ImportedClip> SliceClips(const std::vector<ImportedClip>& clips, const ImportSettings& settings, std::vector<std::string>* warnings) {
    std::vector<std::string> errors;
    const std::vector<ClipSegment> segments = ParseClipSegments(settings.clipSegments, &errors);
    if (warnings)
        for (const std::string& e : errors) warnings->push_back(e);
    if (segments.empty() || clips.empty()) return clips;

    // The take is the longest clip of the file.
    const ImportedClip* take = &clips[0];
    for (const ImportedClip& c : clips)
        if (c.frameCount > take->frameCount) take = &c;

    std::vector<ImportedClip> out;
    for (const ClipSegment& seg : segments) {
        const f32 rate = take->frameRate;
        const i64 last = static_cast<i64>(take->frameCount) - 1;
        i64 f0 = std::clamp<i64>(std::llround(seg.start * rate), 0, last);
        i64 f1 = std::clamp<i64>(std::llround(seg.end * rate), 0, last);
        if (f1 - f0 < 2) {
            if (warnings) warnings->push_back("clip segment '" + seg.name + "' is outside the take (" + std::to_string(take->frameCount / rate) + " s long)");
            continue;
        }
        if (seg.loop) {
            // Close the loop: the end frame within +-0.4 s whose pose continues best into the first frame.
            const i64 reach = static_cast<i64>(0.4f * rate);
            double best = 1e30;
            i64 bestEnd = f1;
            for (i64 e = std::max(f1 - reach, f0 + 8); e <= std::min(f1 + reach, last); ++e) {
                const double c = loopCost(*take, static_cast<u32>(f0), static_cast<u32>(e)) + 0.002 * std::abs(static_cast<double>(e - f1));
                if (c < best) {
                    best = c;
                    bestEnd = e;
                }
            }
            f1 = bestEnd;
        }
        ImportedClip clip;
        clip.name = seg.name;
        clip.key = "segment:" + seg.name;
        clip.skeleton = take->skeleton;
        clip.frameRate = take->frameRate;
        clip.frameCount = static_cast<u32>(f1 - f0 + 1);
        clip.loop = seg.loop;
        clip.tracks.resize(take->tracks.size());
        for (size_t j = 0; j < take->tracks.size(); ++j) {
            const ImportedClipTrack& in = take->tracks[j];
            ImportedClipTrack& o = clip.tracks[j];
            auto cut = [&](const auto& src, auto& dst) {
                if (src.size() > static_cast<size_t>(f1)) dst.assign(src.begin() + f0, src.begin() + f1 + 1);
            };
            cut(in.translation, o.translation);
            cut(in.rotation, o.rotation);
            cut(in.scale, o.scale);
        }
        out.push_back(std::move(clip));
    }
    return out;
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
    out.loops = src.loop || settings.clipLoop == "loop" ? true : settings.clipLoop == "once" ? false : seamless;
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
    i32 foot[2] = {-1, -1}; // left, right
    for (u32 j = 0; j < jointCount; ++j) {
        if (lower[j].find("foot") == std::string::npos || lower[j].find("toe") != std::string::npos) continue;
        const int side = sideOf(lower[j]);
        if (side != 0 && foot[(side + 1) / 2] < 0) foot[(side + 1) / 2] = static_cast<i32>(j);
    }

    // ---- airborne interval (jumps) ----
    // The longest run of frames in which both feet are clearly above the lowest height they reach in the clip.
    if (!out.loops || out.rootSpeed() < 0.3f) {
        if (foot[0] >= 0 && foot[1] >= 0 && last >= 6) {
            std::vector<f32> low(out.frameCount);
            f32 ground = 1e30f, top = -1e30f;
            for (u32 f = 0; f <= last; ++f) {
                const f32 a = work.model(static_cast<u32>(foot[0]), f)[3].y, b = work.model(static_cast<u32>(foot[1]), f)[3].y;
                low[f] = std::min(a, b);
                ground = std::min(ground, low[f]);
                top = std::max(top, low[f]);
            }
            if (top - ground > 0.12f) { // a real jump lifts the feet by decimeters; steps and idles do not
                const f32 threshold = ground + std::max(0.05f, 0.25f * (top - ground));
                u32 bestStart = 0, bestLength = 0;
                for (u32 f = 0; f <= last;) {
                    if (low[f] <= threshold) {
                        ++f;
                        continue;
                    }
                    const u32 start = f;
                    while (f <= last && low[f] > threshold) ++f;
                    if (f - start > bestLength) {
                        bestLength = f - start;
                        bestStart = start;
                    }
                }
                if (bestLength >= 3 && bestStart > 0 && bestStart + bestLength <= last) {
                    out.hasAir = true;
                    out.airStart = static_cast<f32>(bestStart) / out.frameRate;
                    out.airEnd = static_cast<f32>(bestStart + bestLength) / out.frameRate;
                }
            }
        }
    }

    if (out.loops && last >= 4 && out.rootSpeed() > 0.3f) {
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
