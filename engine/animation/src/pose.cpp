#include "foh/animation/pose.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace foh {

namespace {

// First key index whose frame is greater than `frame` (frames ascending).
size_t upperBound(const std::vector<u16>& frames, f32 frame) {
    size_t lo = 0, hi = frames.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (static_cast<f32>(frames[mid]) <= frame) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

Quat nlerp(const Quat& a, Quat b, f32 t) {
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f) b = Quat(-b.w, -b.x, -b.y, -b.z);
    const Quat q(a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
    const f32 len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return len > 1e-8f ? Quat(q.w / len, q.x / len, q.y / len, q.z / len) : a;
}

Quat sampleRotation(const QuatChannel& ch, f32 frame) {
    if (ch.frames.empty() || frame <= static_cast<f32>(ch.frames.front())) return ch.values.front();
    if (frame >= static_cast<f32>(ch.frames.back())) return ch.values.back();
    const size_t hi = upperBound(ch.frames, frame), lo = hi - 1;
    const f32 span = static_cast<f32>(ch.frames[hi]) - static_cast<f32>(ch.frames[lo]);
    return nlerp(ch.values[lo], ch.values[hi], (frame - static_cast<f32>(ch.frames[lo])) / span);
}

Vec3 sampleVector(const VecChannel& ch, f32 frame) {
    if (ch.frames.empty() || frame <= static_cast<f32>(ch.frames.front())) return ch.values.front();
    if (frame >= static_cast<f32>(ch.frames.back())) return ch.values.back();
    const size_t hi = upperBound(ch.frames, frame), lo = hi - 1;
    const f32 span = static_cast<f32>(ch.frames[hi]) - static_cast<f32>(ch.frames[lo]);
    const f32 t = (frame - static_cast<f32>(ch.frames[lo])) / span;
    return ch.values[lo] + (ch.values[hi] - ch.values[lo]) * t;
}

// Planar rotation about +Y by `yaw`: local -> world (x' = x cos + z sin, z' = -x sin + z cos).
Vec2 rotatePlanar(const Vec2& v, f32 yaw) {
    const f32 c = std::cos(yaw), s = std::sin(yaw);
    return Vec2(v.x * c + v.y * s, -v.x * s + v.y * c);
}

struct Affine {
    Mat3 basis{1.0f};
    Vec3 translation{0.0f};
};

Affine toAffine(const JointPose& p) {
    const Mat3 r = glm::mat3_cast(p.rotation);
    Affine a;
    a.basis = Mat3(r[0] * p.scale.x, r[1] * p.scale.y, r[2] * p.scale.z);
    a.translation = p.position;
    return a;
}

Affine multiply(const Affine& a, const Affine& b) {
    Affine r;
    r.basis = a.basis * b.basis;
    r.translation = a.basis * b.translation + a.translation;
    return r;
}

SkinMatrix toRows(const Affine& a) {
    SkinMatrix m;
    m.row0 = Vec4(a.basis[0][0], a.basis[1][0], a.basis[2][0], a.translation.x);
    m.row1 = Vec4(a.basis[0][1], a.basis[1][1], a.basis[2][1], a.translation.y);
    m.row2 = Vec4(a.basis[0][2], a.basis[1][2], a.basis[2][2], a.translation.z);
    return m;
}

} // namespace

void SetRestPose(const SkeletonAsset& skeleton, std::span<JointPose> out) {
    const size_t n = std::min<size_t>(out.size(), skeleton.jointCount());
    for (size_t i = 0; i < n; ++i) out[i] = {skeleton.restPosition[i], skeleton.restRotation[i], skeleton.restScale[i]};
}

f32 RestHeight(const SkeletonAsset& skeleton, i32 joint) {
    if (joint < 0 || joint >= static_cast<i32>(skeleton.jointCount())) return 0.0f;
    // Walk up the chain: only the translation matters for a height, but rotations and scales of the parents move it.
    std::array<i32, kMaxSkeletonJoints> chain;
    u32 depth = 0;
    for (i32 j = joint; j >= 0 && depth < chain.size(); j = skeleton.parents[static_cast<size_t>(j)]) chain[depth++] = j;
    Affine model;
    for (u32 k = depth; k-- > 0;) {
        const size_t j = static_cast<size_t>(chain[k]);
        model = multiply(model, toAffine({skeleton.restPosition[j], skeleton.restRotation[j], skeleton.restScale[j]}));
    }
    return model.translation.y;
}

ClipBinding BindClip(const SkeletonAsset& skeleton, const ClipAsset& clip) {
    ClipBinding b;
    b.trackJoint.reserve(clip.tracks.size());
    for (const ClipTrack& t : clip.tracks) b.trackJoint.push_back(skeleton.findJoint(t.jointHash));
    b.rootJoint = clip.rootJointHash != 0 ? skeleton.findJoint(clip.rootJointHash) : -1;
    if (b.rootJoint >= 0 && clip.sourceRootHeight > 1e-3f) {
        const f32 target = RestHeight(skeleton, b.rootJoint);
        if (target > 1e-3f) b.rootTranslationScale = target / clip.sourceRootHeight;
    }
    return b;
}

f32 WrapClipTime(const ClipAsset& clip, f32 time) {
    const f32 d = clip.duration();
    if (d <= 0.0f) return 0.0f;
    if (clip.loops) {
        time = std::fmod(time, d);
        return time < 0.0f ? time + d : time;
    }
    return std::clamp(time, 0.0f, d);
}

void SampleClip(const ClipAsset& clip, const ClipBinding& binding, f32 time, std::span<JointPose> pose) {
    const f32 frame = WrapClipTime(clip, time) * clip.frameRate;
    const size_t tracks = std::min(clip.tracks.size(), binding.trackJoint.size());
    for (size_t i = 0; i < tracks; ++i) {
        const i32 joint = binding.trackJoint[i];
        if (joint < 0 || static_cast<size_t>(joint) >= pose.size()) continue;
        const ClipTrack& track = clip.tracks[i];
        JointPose& p = pose[static_cast<size_t>(joint)];
        if (track.hasRotation) p.rotation = sampleRotation(track.rotation, frame);
        if (track.hasTranslation) {
            p.position = sampleVector(track.translation, frame);
            // The root carries the character's height: other proportions scale it.
            if (joint == binding.rootJoint) p.position *= binding.rootTranslationScale;
        }
        if (track.hasScale) p.scale = sampleVector(track.scale, frame);
    }
}

// ---- blending ------------------------------------------------------------------------------------

void BlendInit(std::span<JointPose> acc, std::span<const JointPose> first, f32 w) {
    const size_t n = std::min(acc.size(), first.size());
    for (size_t i = 0; i < n; ++i) {
        acc[i].position = first[i].position * w;
        acc[i].rotation = Quat(first[i].rotation.w * w, first[i].rotation.x * w, first[i].rotation.y * w, first[i].rotation.z * w);
        acc[i].scale = first[i].scale * w;
    }
}

void BlendAdd(std::span<JointPose> acc, std::span<const JointPose> pose, f32 w) {
    const size_t n = std::min(acc.size(), pose.size());
    for (size_t i = 0; i < n; ++i) {
        const Quat& q = pose[i].rotation;
        const Quat& a = acc[i].rotation;
        const f32 sign = a.x * q.x + a.y * q.y + a.z * q.z + a.w * q.w < 0.0f ? -w : w;
        acc[i].position += pose[i].position * w;
        acc[i].rotation = Quat(a.w + q.w * sign, a.x + q.x * sign, a.y + q.y * sign, a.z + q.z * sign);
        acc[i].scale += pose[i].scale * w;
    }
}

void BlendFinish(std::span<JointPose> acc, f32 total) {
    if (!(total > 1e-6f)) return;
    const f32 inv = 1.0f / total;
    for (JointPose& j : acc) {
        j.position *= inv;
        j.scale *= inv;
        const Quat& q = j.rotation;
        const f32 len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        j.rotation = len > 1e-8f ? Quat(q.w / len, q.x / len, q.y / len, q.z / len) : Quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
}

void BlendTwo(std::span<const JointPose> a, std::span<const JointPose> b, f32 t, std::span<JointPose> out) {
    const size_t n = std::min({a.size(), b.size(), out.size()});
    for (size_t i = 0; i < n; ++i) {
        out[i].position = a[i].position + (b[i].position - a[i].position) * t;
        out[i].rotation = nlerp(a[i].rotation, b[i].rotation, t);
        out[i].scale = a[i].scale + (b[i].scale - a[i].scale) * t;
    }
}

// ---- root motion ---------------------------------------------------------------------------------

Vec3 SampleRootCurve(const ClipAsset& clip, f32 time) {
    if (clip.rootCurve.empty()) return Vec3(0.0f);
    const f32 frame = std::clamp(time, 0.0f, clip.duration()) * clip.frameRate;
    const size_t lo = std::min(static_cast<size_t>(frame), clip.rootCurve.size() - 1);
    const size_t hi = std::min(lo + 1, clip.rootCurve.size() - 1);
    return clip.rootCurve[lo] + (clip.rootCurve[hi] - clip.rootCurve[lo]) * (frame - static_cast<f32>(lo));
}

namespace {

RootDelta rootBetweenInside(const ClipAsset& clip, f32 t0, f32 t1) {
    const Vec3 a = SampleRootCurve(clip, t0), b = SampleRootCurve(clip, t1);
    RootDelta d;
    // The curve is in the clip's start frame; express the step in the heading the character had at t0.
    d.translation = rotatePlanar(Vec2(b.x - a.x, b.z - a.z), -a.y);
    d.yaw = b.y - a.y;
    return d;
}

} // namespace

RootDelta RootMotionBetween(const ClipAsset& clip, f32 t0, f32 t1) {
    if (clip.rootCurve.empty() || !(t1 > t0)) return {};
    const f32 duration = clip.duration();
    if (!clip.loops || t1 <= duration) return rootBetweenInside(clip, t0, t1);
    // Past the end of a loop: finish the cycle, then continue from its start in the heading the character now has.
    const RootDelta first = rootBetweenInside(clip, t0, duration);
    const RootDelta second = rootBetweenInside(clip, 0.0f, std::min(t1 - duration, duration));
    RootDelta d;
    d.translation = first.translation + rotatePlanar(second.translation, first.yaw);
    d.yaw = first.yaw + second.yaw;
    return d;
}

// ---- skinning ------------------------------------------------------------------------------------

void ComputeModelPose(const SkeletonAsset& skeleton, std::span<const JointPose> pose, std::span<SkinMatrix> out) {
    const size_t n = std::min({static_cast<size_t>(skeleton.jointCount()), pose.size(), out.size()});
    std::array<Affine, kMaxSkeletonJoints> model;
    for (size_t i = 0; i < n; ++i) {
        const Affine local = toAffine(pose[i]);
        const i32 parent = skeleton.parents[i];
        model[i] = parent >= 0 ? multiply(model[static_cast<size_t>(parent)], local) : local;
        out[i] = toRows(model[i]);
    }
}

void ComputeSkinPalette(const SkeletonAsset& skeleton, std::span<const JointPose> pose, std::span<SkinMatrix> out) {
    const size_t n = std::min({static_cast<size_t>(skeleton.jointCount()), pose.size(), out.size()});
    std::array<Affine, kMaxSkeletonJoints> model;
    for (size_t i = 0; i < n; ++i) {
        const Affine local = toAffine(pose[i]);
        const i32 parent = skeleton.parents[i];
        model[i] = parent >= 0 ? multiply(model[static_cast<size_t>(parent)], local) : local;
        const Mat4& ib = skeleton.inverseBind[i];
        Affine bind;
        bind.basis = Mat3(ib);
        bind.translation = Vec3(ib[3]);
        out[i] = toRows(multiply(model[i], bind));
    }
}

} // namespace foh
