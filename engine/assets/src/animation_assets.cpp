#include "foh/animation_assets.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "foh/byte_stream.h"

namespace foh {

namespace {

constexpr u32 kTagMeta = FourCC('M', 'E', 'T', 'A');
constexpr u32 kTagTrak = FourCC('T', 'R', 'A', 'K');
constexpr u32 kTagDeps = FourCC('D', 'E', 'P', 'S');

constexpr u32 kSkeletonVersion = 1;
constexpr u32 kClipVersion = 2; // 2: airborne interval
constexpr u32 kMaxClipFrames = 1u << 16; // frame indices are 16 bit

bool setError(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

bool checkHeader(const ContainerReader& reader, u32 kind, u32 currentVersion, std::string* error) {
    if (reader.typeId() != kind) return setError(error, "container holds a different asset type");
    if (reader.typeVersion() == 0 || reader.typeVersion() > currentVersion) return setError(error, "unsupported asset type version");
    return true;
}

// Smallest-three: drop the largest component (it is recoverable from the unit length), keep the other three as
// 16-bit values in [-1/sqrt2, 1/sqrt2]. The sign is chosen so the dropped component is positive.
constexpr f32 kSmallestRange = 0.70710678118f;

struct PackedQuat {
    i16 a, b, c;
    u8 largest;
};

PackedQuat packQuat(Quat q) {
    q = glm::normalize(q);
    const f32 v[4] = {q.x, q.y, q.z, q.w};
    u32 largest = 0;
    for (u32 i = 1; i < 4; ++i)
        if (std::abs(v[i]) > std::abs(v[largest])) largest = i;
    const f32 sign = v[largest] < 0.0f ? -1.0f : 1.0f;
    i16 out[3];
    u32 n = 0;
    for (u32 i = 0; i < 4; ++i) {
        if (i == largest) continue;
        const f32 scaled = std::clamp(v[i] * sign / kSmallestRange, -1.0f, 1.0f) * 32767.0f;
        out[n++] = static_cast<i16>(std::lround(scaled));
    }
    return {out[0], out[1], out[2], static_cast<u8>(largest)};
}

Quat unpackQuat(const PackedQuat& p) {
    const f32 small[3] = {static_cast<f32>(p.a) / 32767.0f * kSmallestRange, static_cast<f32>(p.b) / 32767.0f * kSmallestRange,
                          static_cast<f32>(p.c) / 32767.0f * kSmallestRange};
    f32 v[4];
    u32 n = 0;
    f32 sum = 0.0f;
    for (u32 i = 0; i < 4; ++i) {
        if (i == p.largest) continue;
        v[i] = small[n];
        sum += small[n] * small[n];
        ++n;
    }
    v[p.largest] = std::sqrt(std::max(0.0f, 1.0f - sum));
    return glm::normalize(Quat(v[3], v[0], v[1], v[2]));
}

void putMat4(ByteWriter& w, const Mat4& m) {
    for (int c = 0; c < 4; ++c) w.putVec4(m[c]);
}
Mat4 getMat4(ByteReader& r) {
    Mat4 m;
    for (int c = 0; c < 4; ++c) m[c] = r.getVec4();
    return m;
}

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool finite(const Quat& q) { return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w); }

// ---- channels --------------------------------------------------------------------------------

void putQuatChannel(ByteWriter& w, const QuatChannel& ch) {
    w.put<u32>(static_cast<u32>(ch.values.size()));
    for (u16 f : ch.frames) w.put<u16>(f);
    for (const Quat& q : ch.values) {
        const PackedQuat p = packQuat(q);
        w.put<i16>(p.a);
        w.put<i16>(p.b);
        w.put<i16>(p.c);
        w.put<u8>(p.largest);
    }
}

bool getQuatChannel(ByteReader& r, QuatChannel& ch, u32 frameCount) {
    const u32 n = r.getCount(7);
    if (n == 0 || (n > 1 && n > frameCount)) return false;
    if (n > 1) {
        ch.frames.resize(n);
        for (u16& f : ch.frames) f = r.get<u16>();
        for (u32 i = 0; i < n; ++i)
            if (ch.frames[i] >= frameCount || (i > 0 && ch.frames[i] <= ch.frames[i - 1])) return false;
    }
    ch.values.resize(n);
    for (Quat& q : ch.values) {
        PackedQuat p;
        p.a = r.get<i16>();
        p.b = r.get<i16>();
        p.c = r.get<i16>();
        p.largest = r.get<u8>();
        if (p.largest > 3) return false;
        q = unpackQuat(p);
    }
    return r.ok();
}

void putVecChannel(ByteWriter& w, const VecChannel& ch) {
    w.put<u32>(static_cast<u32>(ch.values.size()));
    for (u16 f : ch.frames) w.put<u16>(f);
    for (const Vec3& v : ch.values) w.putVec3(v);
}

bool getVecChannel(ByteReader& r, VecChannel& ch, u32 frameCount) {
    const u32 n = r.getCount(12);
    if (n == 0 || (n > 1 && n > frameCount)) return false;
    if (n > 1) {
        ch.frames.resize(n);
        for (u16& f : ch.frames) f = r.get<u16>();
        for (u32 i = 0; i < n; ++i)
            if (ch.frames[i] >= frameCount || (i > 0 && ch.frames[i] <= ch.frames[i - 1])) return false;
    }
    ch.values.resize(n);
    for (Vec3& v : ch.values) {
        v = r.getVec3();
        if (!finite(v)) return false;
    }
    return r.ok();
}

} // namespace

std::string NormalizeJointName(std::string_view name) {
    // The last ':' or '|' separates the namespace from the joint ("mixamorig:Hips", "Armature|Hips").
    const size_t cut = name.find_last_of(":|");
    if (cut != std::string_view::npos && cut + 1 < name.size()) name.remove_prefix(cut + 1);
    return std::string(name);
}

u64 JointNameHash(std::string_view normalizedName) { return HashString(normalizedName, 0x4a6f696e74ull); }

// ---- skeleton ------------------------------------------------------------------------------

std::vector<u8> SerializeSkeleton(const SkeletonAsset& s, AssetId id) {
    ByteWriter w;
    w.putString(s.name);
    w.put<u32>(s.jointCount());
    for (u32 i = 0; i < s.jointCount(); ++i) {
        w.putString(s.jointNames[i]);
        w.put<u64>(s.jointHashes[i]);
        w.put<i32>(s.parents[i]);
        w.putVec3(s.restPosition[i]);
        w.putQuat(s.restRotation[i]);
        w.putVec3(s.restScale[i]);
        putMat4(w, s.inverseBind[i]);
    }
    ContainerWriter c(FourCC('S', 'K', 'E', 'L'), kSkeletonVersion, id);
    c.addChunk(kTagMeta, w.bytes(), ChunkCodec::Lz4);
    c.addChunk(kTagDeps, [] {
        ByteWriter d;
        d.put<u32>(0);
        return d.take();
    }());
    return c.finish();
}

bool DeserializeSkeleton(const ContainerReader& reader, SkeletonAsset& out, std::string* error) {
    if (!checkHeader(reader, FourCC('S', 'K', 'E', 'L'), kSkeletonVersion, error)) return false;
    std::vector<u8> meta;
    if (!reader.readChunk(kTagMeta, meta)) return setError(error, "missing or corrupt META chunk");
    ByteReader r(meta);
    SkeletonAsset s;
    s.name = r.getString();
    const u32 count = r.getCount(4 + 8 + 4 + 12 + 16 + 12 + 64);
    if (count > kMaxSkeletonJoints) return setError(error, "too many joints");
    for (u32 i = 0; i < count; ++i) {
        s.jointNames.push_back(r.getString());
        s.jointHashes.push_back(r.get<u64>());
        s.parents.push_back(r.get<i32>());
        s.restPosition.push_back(r.getVec3());
        s.restRotation.push_back(r.getQuat());
        s.restScale.push_back(r.getVec3());
        s.inverseBind.push_back(getMat4(r));
        if (!r.ok()) break;
        if (s.parents[i] >= static_cast<i32>(i) || s.parents[i] < -1) return setError(error, "joint parent out of order");
        if (!finite(s.restPosition[i]) || !finite(s.restRotation[i]) || !finite(s.restScale[i]))
            return setError(error, "non-finite rest pose");
    }
    if (!r.ok()) return setError(error, "truncated skeleton metadata");
    out = std::move(s);
    return true;
}

// ---- clip ----------------------------------------------------------------------------------

std::vector<u8> SerializeClip(const ClipAsset& clip, AssetId id) {
    ByteWriter w;
    w.putString(clip.name);
    w.put<f32>(clip.frameRate);
    w.put<u32>(clip.frameCount);
    w.put<u8>(clip.loops ? 1 : 0);
    w.put<u64>(clip.rootJointHash);
    w.put<f32>(clip.sourceRootHeight);
    w.put<u8>(static_cast<u8>(clip.rootMode));
    w.put<u8>(clip.hasSync ? 1 : 0);
    w.put<f32>(clip.leftFootPhase);
    w.put<f32>(clip.rightFootPhase);
    w.put<u32>(static_cast<u32>(clip.rootCurve.size()));
    for (const Vec3& v : clip.rootCurve) w.putVec3(v);
    w.put<u32>(static_cast<u32>(clip.tracks.size()));
    w.put<u8>(clip.hasAir ? 1 : 0);
    w.put<f32>(clip.airStart);
    w.put<f32>(clip.airEnd);

    ByteWriter t;
    for (const ClipTrack& track : clip.tracks) {
        t.put<u64>(track.jointHash);
        t.putString(track.jointName);
        t.put<u8>(static_cast<u8>((track.hasRotation ? 1 : 0) | (track.hasTranslation ? 2 : 0) | (track.hasScale ? 4 : 0)));
        if (track.hasRotation) putQuatChannel(t, track.rotation);
        if (track.hasTranslation) putVecChannel(t, track.translation);
        if (track.hasScale) putVecChannel(t, track.scale);
    }

    ContainerWriter c(FourCC('A', 'C', 'L', 'P'), kClipVersion, id);
    c.addChunk(kTagMeta, w.bytes());
    c.addChunk(kTagTrak, t.bytes(), ChunkCodec::Lz4);
    c.addChunk(kTagDeps, [] {
        ByteWriter d;
        d.put<u32>(0);
        return d.take();
    }());
    return c.finish();
}

bool DeserializeClip(const ContainerReader& reader, ClipAsset& out, std::string* error) {
    if (!checkHeader(reader, FourCC('A', 'C', 'L', 'P'), kClipVersion, error)) return false;
    std::vector<u8> meta, trak;
    if (!reader.readChunk(kTagMeta, meta)) return setError(error, "missing or corrupt META chunk");
    if (!reader.readChunk(kTagTrak, trak)) return setError(error, "missing or corrupt TRAK chunk");

    ByteReader r(meta);
    ClipAsset c;
    c.name = r.getString();
    c.frameRate = r.get<f32>();
    c.frameCount = r.get<u32>();
    c.loops = r.get<u8>() != 0;
    c.rootJointHash = r.get<u64>();
    c.sourceRootHeight = r.get<f32>();
    const u8 mode = r.get<u8>();
    c.hasSync = r.get<u8>() != 0;
    c.leftFootPhase = r.get<f32>();
    c.rightFootPhase = r.get<f32>();
    const u32 curveCount = r.getCount(12);
    c.rootCurve.reserve(curveCount);
    for (u32 i = 0; i < curveCount; ++i) c.rootCurve.push_back(r.getVec3());
    const u32 trackCount = r.get<u32>();
    if (reader.typeVersion() >= 2) {
        c.hasAir = r.get<u8>() != 0;
        c.airStart = r.get<f32>();
        c.airEnd = r.get<f32>();
    }
    if (!r.ok()) return setError(error, "truncated clip metadata");
    if (mode > static_cast<u8>(RootMotionMode::Full)) return setError(error, "unknown root motion mode");
    c.rootMode = static_cast<RootMotionMode>(mode);
    if (!(c.frameRate > 0.0f && c.frameRate <= 1000.0f)) return setError(error, "bad frame rate");
    if (!std::isfinite(c.sourceRootHeight)) return setError(error, "bad root height");
    if (c.frameCount == 0 || c.frameCount > kMaxClipFrames) return setError(error, "bad frame count");
    if (!c.rootCurve.empty() && c.rootCurve.size() != c.frameCount) return setError(error, "root curve length mismatch");
    if (!(c.leftFootPhase >= 0.0f && c.leftFootPhase <= 1.0f && c.rightFootPhase >= 0.0f && c.rightFootPhase <= 1.0f))
        return setError(error, "bad foot phase");
    for (const Vec3& v : c.rootCurve)
        if (!finite(v)) return setError(error, "non-finite root curve");
    if (trackCount > 4096 || trackCount > trak.size() / 10) return setError(error, "bad track count");

    ByteReader t(trak);
    c.tracks.resize(trackCount);
    for (ClipTrack& track : c.tracks) {
        track.jointHash = t.get<u64>();
        track.jointName = t.getString();
        const u8 flags = t.get<u8>();
        if (!t.ok() || flags > 7) return setError(error, "bad clip track");
        track.hasRotation = flags & 1;
        track.hasTranslation = flags & 2;
        track.hasScale = flags & 4;
        if (track.hasRotation && !getQuatChannel(t, track.rotation, c.frameCount)) return setError(error, "bad rotation channel");
        if (track.hasTranslation && !getVecChannel(t, track.translation, c.frameCount)) return setError(error, "bad translation channel");
        if (track.hasScale && !getVecChannel(t, track.scale, c.frameCount)) return setError(error, "bad scale channel");
    }
    if (!t.ok()) return setError(error, "truncated clip tracks");
    out = std::move(c);
    return true;
}

} // namespace foh
