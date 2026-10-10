#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <random>

#include "foh/animation_assets.h"
#include "foh/asset_db.h"
#include "foh/asset_import.h"
#include "foh/asset_types.h"
#include "asset_test_util.h"

using namespace foh;
using namespace foh::testutil;
namespace fs = std::filesystem;

namespace {

constexpr f32 kPi = 3.14159265358979f;

AssetDbDesc dbFor(const TempDir& t) { return {t.path / "assets", t.path / "cache"}; }

// Linear sample of a reduced channel at a frame (what the runtime sampler does).
Vec3 sampleVec(const VecChannel& ch, f32 frame) {
    if (ch.constant()) return ch.values[0];
    size_t hi = 1;
    while (hi + 1 < ch.frames.size() && ch.frames[hi] < frame) ++hi;
    const f32 a = ch.frames[hi - 1], b = ch.frames[hi];
    const f32 t = std::clamp((frame - a) / (b - a), 0.0f, 1.0f);
    return ch.values[hi - 1] + (ch.values[hi] - ch.values[hi - 1]) * t;
}

f32 angleBetween(const Quat& a, const Quat& b) {
    const double ax = a.x, ay = a.y, az = a.z, aw = a.w, bx = -static_cast<double>(b.x), by = -static_cast<double>(b.y),
                 bz = -static_cast<double>(b.z), bw = b.w;
    const double w = aw * bw - ax * bx - ay * by - az * bz;
    const double x = aw * bx + ax * bw + ay * bz - az * by;
    const double y = aw * by - ax * bz + ay * bw + az * bx;
    const double z = aw * bz + ax * by - ay * bx + az * bw;
    return static_cast<f32>(2.0 * std::atan2(std::sqrt(x * x + y * y + z * z), std::abs(w)));
}

const ClipTrack* findTrack(const ClipAsset& clip, std::string_view name) {
    const u64 hash = JointNameHash(name);
    for (const ClipTrack& t : clip.tracks)
        if (t.jointHash == hash) return &t;
    return nullptr;
}

// A skinned quad over two joints (Hips > Spine) below an "Armature" node, with a clip that moves the hips by 1.5 m in
// one second and bends the spine by 90 degrees. Names carry the Mixamo prefix.
std::string makeSkinnedGltf() {
    std::vector<u8> buf;
    auto putFloats = [&](std::initializer_list<f32> v) {
        for (f32 f : v) {
            u8 b[4];
            std::memcpy(b, &f, 4);
            buf.insert(buf.end(), b, b + 4);
        }
    };
    auto putU16 = [&](std::initializer_list<u16> v) {
        for (u16 s : v) {
            buf.push_back(static_cast<u8>(s & 255));
            buf.push_back(static_cast<u8>(s >> 8));
        }
    };
    putFloats({0, 1, 0, 1, 1, 0, 0, 1.5f, 0, 1, 1.5f, 0});                 // positions   [0, 48)
    for (int b : {0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0}) buf.push_back(static_cast<u8>(b)); // joints [48, 64)
    putFloats({1, 0, 0, 0, 1, 0, 0, 0, 0.5f, 0.5f, 0, 0, 0.75f, 0.25f, 0, 0}); // weights [64, 128)
    putU16({0, 1, 2, 1, 3, 2});                                            // indices     [128, 140)
    putFloats({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1});          // IBM hips    [140, 204)
    putFloats({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1.5f, 0, 1});       // IBM spine   [204, 268)
    putFloats({0, 1});                                                     // times       [268, 276)
    putFloats({0, 1, 0, 0, 1, 1.5f});                                      // hips translation [276, 300)
    putFloats({0, 0, 0, 1, 0.70710678f, 0, 0, 0.70710678f});               // spine rotation   [300, 332)

    std::string json = R"({
 "asset": {"version": "2.0"},
 "scene": 0,
 "scenes": [{"nodes": [0, 3]}],
 "nodes": [
   {"name": "Armature", "children": [1]},
   {"name": "mixamorig:Hips", "translation": [0, 1, 0], "children": [2]},
   {"name": "mixamorig:Spine", "translation": [0, 0.5, 0]},
   {"name": "Body", "mesh": 0, "skin": 0}
 ],
 "meshes": [{"name": "Body", "primitives": [{"attributes": {"POSITION": 0, "JOINTS_0": 1, "WEIGHTS_0": 2}, "indices": 3}]}],
 "skins": [{"name": "Rig", "joints": [1, 2], "inverseBindMatrices": 4, "skeleton": 1}],
 "animations": [{"name": "Walk",
   "samplers": [{"input": 5, "output": 6, "interpolation": "LINEAR"}, {"input": 5, "output": 7, "interpolation": "LINEAR"}],
   "channels": [{"sampler": 0, "target": {"node": 1, "path": "translation"}}, {"sampler": 1, "target": {"node": 2, "path": "rotation"}}]}],
 "accessors": [
   {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [0, 1, 0], "max": [1, 1.5, 0]},
   {"bufferView": 1, "componentType": 5121, "count": 4, "type": "VEC4"},
   {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC4"},
   {"bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR"},
   {"bufferView": 4, "componentType": 5126, "count": 2, "type": "MAT4"},
   {"bufferView": 5, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0], "max": [1]},
   {"bufferView": 6, "componentType": 5126, "count": 2, "type": "VEC3"},
   {"bufferView": 7, "componentType": 5126, "count": 2, "type": "VEC4"}
 ],
 "bufferViews": [
   {"buffer": 0, "byteOffset": 0, "byteLength": 48},
   {"buffer": 0, "byteOffset": 48, "byteLength": 16},
   {"buffer": 0, "byteOffset": 64, "byteLength": 64},
   {"buffer": 0, "byteOffset": 128, "byteLength": 12},
   {"buffer": 0, "byteOffset": 140, "byteLength": 128},
   {"buffer": 0, "byteOffset": 268, "byteLength": 8},
   {"buffer": 0, "byteOffset": 276, "byteLength": 24},
   {"buffer": 0, "byteOffset": 300, "byteLength": 32}
 ],
 "buffers": [{"byteLength": 332, "uri": "data:application/octet-stream;base64,%BUFFER%"}]
})";
    json.replace(json.find("%BUFFER%"), 8, base64(buf));
    return json;
}

// Hips > {LeftFoot, RightFoot}: a one second walk cycle (31 samples, the last one closes the loop) going 1.6 m/s along +Z
// with a small sideways sway and bob. Feet lift in alternation: the left one is up during the first half.
ImportedSkeleton walkSkeleton() {
    ImportedSkeleton sk;
    sk.key = "skeleton";
    sk.name = "walker";
    sk.jointNames = {"mixamorig:Hips", "mixamorig:LeftFoot", "mixamorig:RightFoot"};
    sk.parents = {-1, 0, 0};
    sk.restPosition = {Vec3(0, 1, 0), Vec3(0.1f, -0.9f, 0), Vec3(-0.1f, -0.9f, 0)};
    sk.restRotation = {Quat(1, 0, 0, 0), Quat(1, 0, 0, 0), Quat(1, 0, 0, 0)};
    sk.restScale = {Vec3(1), Vec3(1), Vec3(1)};
    sk.inverseBind.assign(3, Mat4(1.0f));
    sk.hasInverseBind.assign(3, 0);
    return sk;
}

ImportedClip walkClip(f32 speed, f32 yawRate = 0.0f) {
    ImportedClip clip;
    clip.name = "walk";
    clip.key = "walk";
    clip.skeleton = 0;
    clip.frameRate = 30.0f;
    clip.frameCount = 31;
    clip.tracks.resize(3);
    for (u32 f = 0; f < clip.frameCount; ++f) {
        const f32 t = static_cast<f32>(f) / 30.0f;
        const f32 yaw = yawRate * t;
        const Quat yawQ = glm::angleAxis(yaw, Vec3(0, 1, 0));
        // the hips are expressed in model space (the skeleton root has no parent)
        const Vec3 forward = yawQ * Vec3(0, 0, 1);
        const Vec3 side = yawQ * Vec3(1, 0, 0);
        clip.tracks[0].translation.push_back(Vec3(0, 1 + 0.005f * std::cos(4 * kPi * t), 0) + forward * (speed * t) + side * (0.02f * std::sin(2 * kPi * t)));
        clip.tracks[0].rotation.push_back(yawQ);
        const f32 leftLift = 0.1f * std::max(0.0f, std::sin(2 * kPi * t));
        const f32 rightLift = 0.1f * std::max(0.0f, -std::sin(2 * kPi * t));
        clip.tracks[1].translation.push_back(Vec3(0.1f, -0.9f + leftLift, 0));
        clip.tracks[2].translation.push_back(Vec3(-0.1f, -0.9f + rightLift, 0));
        clip.tracks[1].rotation.push_back(Quat(1, 0, 0, 0));
        clip.tracks[2].rotation.push_back(Quat(1, 0, 0, 0));
    }
    return clip;
}

f32 cyclicDistance(f32 a, f32 b) {
    const f32 d = std::abs(a - b);
    return std::min(d, 1.0f - d);
}

} // namespace

TEST_CASE("joint names are normalised so clips and skeletons meet") {
    CHECK(NormalizeJointName("mixamorig:Hips") == "Hips");
    CHECK(NormalizeJointName("Armature|Hips") == "Hips");
    CHECK(NormalizeJointName("Hips") == "Hips");
    CHECK(NormalizeJointName("mixamorig1:LeftHand") == "LeftHand");
    CHECK(JointNameHash("Hips") != JointNameHash("hips"));
}

TEST_CASE("skeleton and clip assets round trip") {
    const AssetId id = GenerateAssetId();
    const ImportedSkeleton src = walkSkeleton();
    const SkeletonAsset skeleton = CookSkeleton(src);
    REQUIRE(skeleton.jointCount() == 3);
    CHECK(skeleton.jointNames[0] == "Hips");
    CHECK(skeleton.findJoint("LeftFoot") == 1);
    // joints no skin binds get the inverse of their rest pose: hips at y = 1
    CHECK(skeleton.inverseBind[0][3].y == doctest::Approx(-1.0f));
    CHECK(skeleton.inverseBind[1][3].y == doctest::Approx(-0.1f)); // foot at y = 1 - 0.9

    SkeletonAsset back;
    std::string error;
    auto reader = ContainerReader::Open(SerializeSkeleton(skeleton, id), &error);
    REQUIRE(reader);
    REQUIRE(DeserializeSkeleton(*reader, back, &error));
    CHECK(back.jointNames == skeleton.jointNames);
    CHECK(back.jointHashes == skeleton.jointHashes);
    CHECK(back.parents == skeleton.parents);
    CHECK(back.inverseBind[2] == skeleton.inverseBind[2]);

    ImportSettings settings;
    ClipAsset clip = CookClip(walkClip(1.6f), src, settings);
    ClipAsset clipBack;
    reader = ContainerReader::Open(SerializeClip(clip, id), &error);
    REQUIRE(reader);
    REQUIRE(DeserializeClip(*reader, clipBack, &error));
    CHECK(clipBack.frameCount == clip.frameCount);
    CHECK(clipBack.loops == clip.loops);
    CHECK(clipBack.rootMode == clip.rootMode);
    CHECK(clipBack.rootCurve.size() == clip.rootCurve.size());
    CHECK(clipBack.hasSync == clip.hasSync);
    CHECK(clipBack.tracks.size() == clip.tracks.size());
    for (size_t i = 0; i < clip.tracks.size(); ++i) {
        CHECK(clipBack.tracks[i].jointHash == clip.tracks[i].jointHash);
        CHECK(clipBack.tracks[i].translation.frames == clip.tracks[i].translation.frames);
    }
}

TEST_CASE("quaternion keys survive 16-bit smallest-three quantisation within a few thousandths of a degree") {
    std::mt19937 rng(7);
    std::normal_distribution<f32> n(0.0f, 1.0f);
    ClipAsset clip;
    clip.frameCount = 64;
    ClipTrack track;
    track.jointHash = 1;
    track.hasRotation = true;
    for (u16 f = 0; f < 64; f += 1) {
        track.rotation.frames.push_back(f);
        track.rotation.values.push_back(glm::normalize(Quat(n(rng), n(rng), n(rng), n(rng))));
    }
    clip.tracks.push_back(track);
    ClipAsset back;
    std::string error;
    auto reader = ContainerReader::Open(SerializeClip(clip, GenerateAssetId()), &error);
    REQUIRE(reader);
    REQUIRE(DeserializeClip(*reader, back, &error));
    REQUIRE(back.tracks[0].rotation.values.size() == 64);
    f32 worst = 0.0f;
    for (size_t i = 0; i < 64; ++i) worst = std::max(worst, angleBetween(clip.tracks[0].rotation.values[i], back.tracks[0].rotation.values[i]));
    CHECK(worst * 180.0f / kPi < 0.005f);
}

TEST_CASE("key reduction respects its error bound and collapses constants and straight lines") {
    std::vector<Vec3> line;
    for (int i = 0; i < 40; ++i) line.push_back(Vec3(0.01f * static_cast<f32>(i), 0, 1));
    VecChannel reduced;
    ReduceVecChannel(line, 0.0005f, reduced);
    CHECK(reduced.frames.size() == 2); // a straight line is its two endpoints

    std::vector<Vec3> constant(40, Vec3(1, 2, 3));
    ReduceVecChannel(constant, 0.0005f, reduced);
    CHECK(reduced.constant());
    CHECK(reduced.values.size() == 1);

    std::vector<Vec3> wave;
    for (int i = 0; i < 90; ++i) wave.push_back(Vec3(0.05f * std::sin(static_cast<f32>(i) * 0.2f), 0, 0));
    const f32 tolerance = 0.0005f;
    ReduceVecChannel(wave, tolerance, reduced);
    CHECK(reduced.frames.size() < wave.size());
    CHECK(reduced.frames.front() == 0);
    CHECK(reduced.frames.back() == 89);
    for (size_t i = 0; i < wave.size(); ++i)
        CHECK(glm::length(sampleVec(reduced, static_cast<f32>(i)) - wave[i]) <= tolerance * 1.01f);

    // rotations: a smooth swing, then check the bound in angle
    std::vector<Quat> swing;
    for (int i = 0; i < 90; ++i) swing.push_back(glm::angleAxis(0.8f * std::sin(static_cast<f32>(i) * 0.1f), glm::normalize(Vec3(1, 2, 3))));
    QuatChannel q;
    const f32 angleTolerance = 0.5f * kPi / 180.0f;
    ReduceQuatChannel(swing, angleTolerance, q);
    CHECK(q.frames.size() < swing.size() / 2);
    for (size_t i = 0; i < swing.size(); ++i) {
        size_t hi = 1;
        while (hi + 1 < q.frames.size() && q.frames[hi] < i) ++hi;
        const f32 a = q.frames[hi - 1], b = q.frames[hi];
        const f32 t = (static_cast<f32>(i) - a) / (b - a);
        Quat qb = q.values[hi];
        if (glm::dot(q.values[hi - 1], qb) < 0) qb = -qb;
        const Quat mixed = glm::normalize(q.values[hi - 1] * (1 - t) + qb * t);
        CHECK(angleBetween(mixed, swing[i]) <= angleTolerance * 1.05f);
    }
}

TEST_CASE("a looping walk keeps its sway in the pose and its average velocity as root motion") {
    ImportSettings settings;
    std::vector<std::string> warnings;
    const ClipAsset clip = CookClip(walkClip(1.6f), walkSkeleton(), settings, &warnings);
    INFO((warnings.empty() ? std::string() : warnings[0]));
    CHECK(clip.loops);
    CHECK(clip.rootMode == RootMotionMode::Linear);
    CHECK(clip.rootSpeed() == doctest::Approx(1.6f).epsilon(0.01));
    CHECK(std::abs(clip.rootYaw()) < 0.01f);
    REQUIRE(clip.rootCurve.size() == 31);
    CHECK(clip.rootCurve[0] == Vec3(0.0f));
    CHECK(clip.rootCurve[30].z == doctest::Approx(1.6f).epsilon(0.01));
    CHECK(clip.sourceRootHeight == doctest::Approx(1.0f));

    const ClipTrack* hips = findTrack(clip, "Hips");
    REQUIRE(hips);
    REQUIRE(hips->hasTranslation);
    f32 maxSway = 0.0f, maxForward = 0.0f;
    for (u32 f = 0; f < 31; ++f) {
        const Vec3 p = sampleVec(hips->translation, static_cast<f32>(f));
        maxSway = std::max(maxSway, std::abs(p.x));
        maxForward = std::max(maxForward, std::abs(p.z));
    }
    CHECK(maxSway == doctest::Approx(0.02f).epsilon(0.1)); // the pelvis sway is still in the pose
    CHECK(maxForward < 0.002f);                            // the forward travel is not

    // foot landmarks: the left foot comes down at the middle of the cycle, the right one at the start
    REQUIRE(clip.hasSync);
    CHECK(cyclicDistance(clip.leftFootPhase, 0.5f) < 0.07f);
    CHECK(cyclicDistance(clip.rightFootPhase, 0.0f) < 0.07f);

    // the feet are not root-motion material: their translation is constant offsets only if it never changes
    const ClipTrack* left = findTrack(clip, "LeftFoot");
    REQUIRE(left);
    CHECK(left->hasTranslation); // it moves (lift), so it is kept
}

TEST_CASE("a one-shot turn gives the exact root curve and an in-place pose") {
    ImportSettings settings;
    // a walk that is cut short: the pose differs at the ends, so it does not loop; it also turns by 45 degrees per second
    ImportedClip src = walkClip(1.0f, 0.785398f);
    for (u32 j = 1; j < 3; ++j) src.tracks[j].translation[30].y += 0.2f; // end pose differs from the start
    const ClipAsset clip = CookClip(src, walkSkeleton(), settings);
    CHECK_FALSE(clip.loops);
    CHECK(clip.rootMode == RootMotionMode::Full);
    CHECK(clip.rootYaw() == doctest::Approx(0.785398f).epsilon(0.02));
    const Vec3 end = clip.rootCurve.back();
    CHECK(std::sqrt(end.x * end.x + end.z * end.z) == doctest::Approx(1.0f).epsilon(0.02));

    // after the split the hips neither travel nor turn
    const ClipTrack* hips = findTrack(clip, "Hips");
    REQUIRE(hips);
    for (u32 f = 0; f < 31; ++f) {
        const Vec3 p = sampleVec(hips->translation, static_cast<f32>(f));
        CHECK(std::abs(p.x) < 0.002f);
        CHECK(std::abs(p.z) < 0.002f);
    }
    CHECK(angleBetween(hips->rotation.constant() ? hips->rotation.values[0] : hips->rotation.values.back(), Quat(1, 0, 0, 0)) < 0.01f);
}

TEST_CASE("settings override loop and root motion detection") {
    ImportSettings settings;
    settings.clipRootMotion = "none";
    ClipAsset clip = CookClip(walkClip(1.6f), walkSkeleton(), settings);
    CHECK(clip.rootMode == RootMotionMode::None);
    CHECK(clip.rootCurve.empty());
    const ClipTrack* hips = findTrack(clip, "Hips");
    REQUIRE(hips);
    CHECK(sampleVec(hips->translation, 30.0f).z == doctest::Approx(1.6f).epsilon(0.01)); // all motion stays in the pose

    settings = {};
    settings.clipLoop = "once";
    clip = CookClip(walkClip(1.6f), walkSkeleton(), settings);
    CHECK_FALSE(clip.loops);
    CHECK(clip.rootMode == RootMotionMode::Full);
}

TEST_CASE("an idle that barely moves keeps its pose untouched") {
    ImportSettings settings;
    ImportedClip idle = walkClip(0.0f);
    for (auto& p : idle.tracks[0].translation) p.x = 0.0f; // no sway: a pure bob
    const ClipAsset clip = CookClip(idle, walkSkeleton(), settings);
    CHECK(clip.loops);
    CHECK(clip.rootMode == RootMotionMode::None);
    CHECK_FALSE(clip.hasSync);
}

TEST_CASE("glTF import: skeleton, skinned mesh, clip and a prefab without bone nodes") {
    TempDir t;
    writeText(t.path / "assets/rig.gltf", makeSkinnedGltf());
    AssetDb db(dbFor(t));
    const ImportResult r = db.importFile("rig.gltf");
    INFO(r.error);
    REQUIRE(r.ok);

    REQUIRE(db.assetsOfKind(AssetKind::Skeleton).size() == 1);
    REQUIRE(db.assetsOfKind(AssetKind::AnimationClip).size() == 1);
    REQUIRE(db.assetsOfKind(AssetKind::Mesh).size() == 1);

    SkeletonAsset skeleton;
    std::string error;
    const AssetId skeletonId = db.assetsOfKind(AssetKind::Skeleton)[0];
    REQUIRE(db.load(skeletonId, skeleton, &error));
    // the Armature node above the bones is part of the skeleton: model space is the scene space
    REQUIRE(skeleton.jointCount() == 3);
    CHECK(skeleton.jointNames[0] == "Armature");
    CHECK(skeleton.jointNames[1] == "Hips");
    CHECK(skeleton.jointNames[2] == "Spine");
    CHECK(skeleton.parents[1] == 0);
    CHECK(skeleton.parents[2] == 1);
    CHECK(skeleton.inverseBind[1][3].y == doctest::Approx(-1.0f));
    CHECK(skeleton.inverseBind[2][3].y == doctest::Approx(-1.5f));

    MeshAsset mesh;
    REQUIRE(db.load(db.assetsOfKind(AssetKind::Mesh)[0], mesh, &error));
    CHECK(mesh.skeleton == skeletonId);
    REQUIRE(mesh.skinned());
    REQUIRE(mesh.vertexStride == sizeof(CookedSkinnedVertex));
    const auto* v = reinterpret_cast<const CookedSkinnedVertex*>(mesh.vertexData.data());
    for (u32 i = 0; i < mesh.vertexCount(); ++i) {
        CHECK(v[i].weights[0] + v[i].weights[1] + v[i].weights[2] + v[i].weights[3] == 255);
        for (int k = 0; k < 4; ++k)
            if (v[i].weights[k] != 0) CHECK(v[i].joints[k] < skeleton.jointCount());
    }
    // vertex 0 is bound entirely to Hips (skeleton joint 1), vertex 2 half to Hips and half to Spine
    CHECK(v[0].joints[0] == 1);
    CHECK(v[0].weights[0] == 255);
    CHECK(v[2].weights[0] + v[2].weights[1] == 255);
    CHECK(std::abs(static_cast<int>(v[2].weights[0]) - static_cast<int>(v[2].weights[1])) <= 1);

    ClipAsset clip;
    REQUIRE(db.load(db.assetsOfKind(AssetKind::AnimationClip)[0], clip, &error));
    CHECK(clip.name == "Walk");
    CHECK(clip.frameCount == 31);
    CHECK(clip.duration() == doctest::Approx(1.0f));
    CHECK_FALSE(clip.loops);
    CHECK(clip.rootMode == RootMotionMode::Full);
    CHECK(clip.rootCurve.back().z == doctest::Approx(1.5f).epsilon(0.01));
    const ClipTrack* spine = findTrack(clip, "Spine");
    REQUIRE(spine);
    CHECK(spine->hasRotation);
    CHECK(angleBetween(spine->rotation.values.back(), Quat(0.70710678f, 0.70710678f, 0, 0)) < 0.01f);

    // the prefab has only the skinned mesh node and references the skeleton for the animator on the instance root
    PrefabAsset prefab;
    REQUIRE(db.load(r.prefab, prefab, &error));
    CHECK(prefab.skeleton == skeletonId);
    REQUIRE(prefab.nodes.size() == 1);
    CHECK(prefab.nodes[0].name == "Body");
    CHECK(prefab.nodes[0].parent == -1);
}

TEST_CASE("reimporting a skinned model keeps the skeleton and clip identities") {
    TempDir t;
    writeText(t.path / "assets/rig.gltf", makeSkinnedGltf());
    AssetDb db(dbFor(t));
    REQUIRE(db.importFile("rig.gltf").ok);
    const AssetId skeleton = db.assetsOfKind(AssetKind::Skeleton)[0];
    const AssetId clip = db.assetsOfKind(AssetKind::AnimationClip)[0];
    const ImportResult again = db.importFile("rig.gltf", true);
    REQUIRE(again.ok);
    CHECK(db.assetsOfKind(AssetKind::Skeleton)[0] == skeleton);
    CHECK(db.assetsOfKind(AssetKind::AnimationClip)[0] == clip);
}

// Optional: the Mixamo files the lead downloads. Set FOH_TEST_MIXAMO to the folder holding "Y Bot.fbx", "walking.fbx"...
TEST_CASE("Mixamo FBX: Y Bot and locomotion clips import as one skeleton and clean clips" * doctest::skip(std::getenv("FOH_TEST_MIXAMO") == nullptr)) {
    const fs::path dir = std::getenv("FOH_TEST_MIXAMO") ? fs::path(std::getenv("FOH_TEST_MIXAMO")) : fs::path();
    TempDir t;
    AssetDb db(dbFor(t));
    ImportResult bot = db.importFile((dir / "Y Bot.fbx").string());
    INFO(bot.error);
    REQUIRE(bot.ok);
    REQUIRE(db.assetsOfKind(AssetKind::Skeleton).size() == 1);
    SkeletonAsset skeleton;
    REQUIRE(db.load(db.assetsOfKind(AssetKind::Skeleton)[0], skeleton));
    CHECK(skeleton.findJoint("Hips") >= 0);
    CHECK(skeleton.findJoint("LeftFoot") >= 0);
    CHECK(skeleton.restPosition[static_cast<size_t>(skeleton.findJoint("Hips"))].y == doctest::Approx(1.0f).epsilon(0.05)); // meters, not centimeters

    PrefabAsset prefab;
    REQUIRE(db.load(bot.prefab, prefab));
    CHECK(prefab.skeleton.isValid());
    for (const PrefabNode& n : prefab.nodes) CHECK(n.name.rfind("mixamorig", 0) != 0); // no bone nodes

    struct Expect {
        const char* file;
        bool loops;
        RootMotionMode mode;
        f32 speed;
    };
    for (const Expect& e : {Expect{"walking.fbx", true, RootMotionMode::Linear, 1.6f}, Expect{"running.fbx", true, RootMotionMode::Linear, 4.2f},
                            Expect{"left strafe walking.fbx", true, RootMotionMode::Linear, 1.66f}}) {
        const ImportResult r = db.importFile((dir / e.file).string());
        INFO(e.file << ": " << r.error);
        REQUIRE(r.ok);
        ClipAsset clip;
        bool found = false;
        for (const AssetId& id : db.assetsOfKind(AssetKind::AnimationClip)) {
            REQUIRE(db.load(id, clip));
            if (clip.name == fs::path(e.file).stem().string()) {
                found = true;
                break;
            }
        }
        REQUIRE(found);
        INFO(e.file << " loops=" << clip.loops << " speed=" << clip.rootSpeed() << " duration=" << clip.duration());
        CHECK(clip.loops == e.loops);
        CHECK(clip.rootMode == e.mode);
        CHECK(clip.rootSpeed() == doctest::Approx(e.speed).epsilon(0.05));
        CHECK(clip.hasSync);
        CHECK(clip.tracks.size() >= 60);
    }
}

// Prints what the importer made of every clip in the folder (run with -s). Not an assertion: a way to look at real data.
TEST_CASE("Mixamo FBX: report on every clip" * doctest::skip(std::getenv("FOH_TEST_MIXAMO") == nullptr)) {
    const fs::path dir = std::getenv("FOH_TEST_MIXAMO") ? fs::path(std::getenv("FOH_TEST_MIXAMO")) : fs::path();
    TempDir t;
    AssetDb db(dbFor(t));
    REQUIRE(db.importFile(dir / "Y Bot.fbx").ok);
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".fbx" || entry.path().stem() == "Y Bot") continue;
        const ImportResult r = db.importFile(entry.path());
        REQUIRE(r.ok);
        for (const AssetId& id : r.assets) {
            ClipAsset clip;
            if (!db.load(id, clip)) continue;
            size_t keys = 0;
            for (const ClipTrack& tr : clip.tracks)
                keys += tr.rotation.values.size() + tr.translation.values.size() + tr.scale.values.size();
            MESSAGE(clip.name << ": " << clip.frameCount << " frames " << clip.duration() << "s loops=" << clip.loops << " root="
                              << static_cast<int>(clip.rootMode) << " speed=" << clip.rootSpeed() << " yaw=" << clip.rootYaw() * 57.2958f
                              << "deg sync=" << clip.hasSync << " L=" << clip.leftFootPhase << " R=" << clip.rightFootPhase
                              << " tracks=" << clip.tracks.size() << " keys=" << keys << " air=" << clip.hasAir << ":" << clip.airStart << "-" << clip.airEnd << " warnings=" << r.warnings.size());
        }
    }
}

// Prints the clips a take yields for given segments (run with -s): FOH_TEST_TAKE = the fbx, FOH_TEST_SEGMENTS = the
// clipSegments setting. A way to check a cut before using it.
TEST_CASE("Take segments: report" * doctest::skip(std::getenv("FOH_TEST_TAKE") == nullptr)) {
    TempDir t;
    AssetDb db(dbFor(t));
    ImportSettings settings;
    settings.animationOnly = true;
    if (const char* seg = std::getenv("FOH_TEST_SEGMENTS")) settings.clipSegments = seg;
    const ImportResult r = db.importFile(fs::path(std::getenv("FOH_TEST_TAKE")), false, &settings);
    INFO(r.error);
    REQUIRE(r.ok);
    for (const std::string& w : r.warnings) MESSAGE("warning: " << w);
    for (const AssetId& id : r.assets) {
        ClipAsset clip;
        if (!db.load(id, clip)) continue;
        MESSAGE(clip.name << ": " << clip.duration() << "s loops=" << clip.loops << " root=" << static_cast<int>(clip.rootMode)
                          << " speed=" << clip.rootSpeed() << " yaw=" << clip.rootYaw() * 57.2958f << "deg tracks=" << clip.tracks.size());
    }
}
