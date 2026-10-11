#include <doctest/doctest.h>

#include <cmath>
#include <fstream>

#include "asset_test_util.h"
#include "foh/animation_assets.h"
#include "foh/asset_db.h"
#include "foh/game_asset.h"
#include "foh/humanoid_rig.h"

using namespace foh;
using namespace foh::testutil;

namespace {

VehicleDef sampleCar() {
    VehicleDef d;
    d.name = "Test car";
    d.model = "imported/Car/Car.gltf";
    d.mass = 1234.5f;
    d.gears = {3.1f, 2.0f, 1.3f};
    const f32 x = 0.8f, zf = 1.3f, zr = -1.2f;
    for (int i = 0; i < 4; ++i) {
        WheelDef w;
        w.name = std::string(i < 2 ? "Front " : "Rear ") + (i % 2 ? "Right" : "Left");
        w.node = i == 0 ? "Car/Body/Wheel FL" : "";
        w.position = Vec3(i % 2 ? -x : x, 0.33f, i < 2 ? zf : zr);
        w.steered = i < 2;
        w.driven = i >= 2;
        w.handbrake = i >= 2;
        d.wheels.push_back(w);
    }
    return d;
}

} // namespace

TEST_CASE("vehicle definition survives a text round trip") {
    const VehicleDef car = sampleCar();
    const std::string text = WriteVehicleDef(car);
    VehicleDef back;
    std::string error;
    REQUIRE_MESSAGE(ParseVehicleDef(text, back, &error), error);
    CHECK(back.name == car.name);
    CHECK(back.model == car.model);
    CHECK(back.mass == doctest::Approx(1234.5f));
    CHECK(back.gears == car.gears);
    REQUIRE(back.wheels.size() == 4);
    CHECK(back.wheels[0].node == "Car/Body/Wheel FL");
    CHECK(back.wheels[0].steered);
    CHECK_FALSE(back.wheels[0].driven);
    CHECK(back.wheels[3].handbrake);
    CHECK(back.wheels[3].position.x == doctest::Approx(-0.8f));
    CHECK(back.frontAxleCount() == 2);
    CHECK(WriteVehicleDef(back) == text); // stable: no churn in git
}

TEST_CASE("definition parsing is forgiving about unknown and missing keys, strict about the kind") {
    VehicleDef d;
    CHECK(ParseVehicleDef("foh-game-asset 1\ntype vehicle\nmass 900\nfuture_key 3\n[wheel]\nradius 0.3\n[something_else]\nx 1\n", d));
    CHECK(d.mass == doctest::Approx(900.0f));
    CHECK(d.maxTorque == doctest::Approx(VehicleDef{}.maxTorque)); // a missing key keeps the default
    CHECK(d.wheels.size() == 1);
    std::string error;
    CHECK_FALSE(ParseVehicleDef("foh-game-asset 1\ntype prop\n", d, &error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(ParseVehicleDef("hello", d));
    CHECK_FALSE(ParseVehicleDef("", d));
}

TEST_CASE("vehicle validation names the problems") {
    VehicleDef d = sampleCar();
    CHECK(ValidateVehicleDef(d).empty());
    d.wheels.resize(2);
    CHECK_FALSE(ValidateVehicleDef(d).empty());
    d = sampleCar();
    for (WheelDef& w : d.wheels) w.driven = false;
    bool mentionsDriven = false;
    for (const std::string& p : ValidateVehicleDef(d)) mentionsDriven = mentionsDriven || p.find("driven") != std::string::npos;
    CHECK(mentionsDriven);
}

TEST_CASE("prop and character definitions round trip") {
    PropDef p;
    p.name = "Barrel";
    p.body = PropBody::Dynamic;
    p.shape = PropShape::Capsule;
    p.halfExtents = Vec3(0.3f, 0.4f, 0.0f);
    p.surface = "metal";
    p.breakable = true;
    PropDef pb;
    REQUIRE(ParsePropDef(WritePropDef(p), pb));
    CHECK(pb.body == PropBody::Dynamic);
    CHECK(pb.shape == PropShape::Capsule);
    CHECK(pb.halfExtents.y == doctest::Approx(0.4f));
    CHECK(pb.surface == "metal");
    CHECK(pb.breakable);

    CharacterDef c;
    c.name = "Hero";
    c.role = CharacterRole::Player;
    c.hasLandmarks = true;
    for (u32 i = 0; i < kHumanoidJointCount; ++i) c.landmarks[i] = Vec3(float(i), 1.0f, -float(i));
    CharacterDef cb;
    REQUIRE(ParseCharacterDef(WriteCharacterDef(c), cb));
    CHECK(cb.role == CharacterRole::Player);
    CHECK(cb.hasLandmarks);
    CHECK(cb.landmarks[7] == c.landmarks[7]);
    CHECK(cb.landmarks[24] == c.landmarks[24]);

    CHECK(GameAssetKindFromPath("assets/Car.FVEHICLE") == GameAssetKind::Vehicle);
    CHECK(GameAssetKindFromPath("a/b.fprop") == GameAssetKind::Prop);
    CHECK(GameAssetKindFromPath("a/b.gltf") == GameAssetKind::None);
}

// ---- auto rig ---------------------------------------------------------------------------------------------------

namespace {

// A tube from a to b: enough of a limb to test weights without a model file.
void addTube(RigMesh& mesh, Vec3 a, Vec3 b, f32 radius) {
    const int rings = 7, sides = 8;
    const Vec3 axis = glm::normalize(b - a);
    const Vec3 helper = std::abs(axis.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    const Vec3 u = glm::normalize(glm::cross(axis, helper)), v = glm::cross(axis, u);
    const u32 base = static_cast<u32>(mesh.positions.size());
    for (int r = 0; r < rings; ++r) {
        const Vec3 c = a + (b - a) * (float(r) / float(rings - 1));
        for (int s = 0; s < sides; ++s) {
            const f32 ang = 6.2831853f * float(s) / float(sides);
            const Vec3 n = u * std::cos(ang) + v * std::sin(ang);
            mesh.positions.push_back(c + n * radius);
            mesh.normals.push_back(n);
        }
    }
    for (int r = 0; r + 1 < rings; ++r)
        for (int s = 0; s < sides; ++s) {
            const u32 i0 = base + r * sides + s, i1 = base + r * sides + (s + 1) % sides, i2 = i0 + sides, i3 = i1 + sides;
            for (u32 idx : {i0, i2, i1, i1, i2, i3}) mesh.indices.push_back(idx);
        }
}

RigMesh humanoidFromLandmarks(const std::array<Vec3, kHumanoidJointCount>& p) {
    RigMesh m;
    m.name = "Body";
    // Skip the zero-length end bones; each joint to its child is one tube.
    for (u32 i = 1; i < kHumanoidJointCount; ++i) {
        const i32 parent = HumanoidJointParent(i);
        if (parent < 0 || glm::length(p[i] - p[static_cast<u32>(parent)]) < 0.02f) continue;
        const bool torso = i <= 6;
        addTube(m, p[static_cast<u32>(parent)], p[i], torso ? 0.12f : 0.045f);
    }
    m.primitives.push_back({0, static_cast<u32>(m.indices.size()), 0});
    return m;
}

u32 dominantJoint(const RigMesh& m, size_t vertex) {
    u32 best = 0, w = 0;
    for (int c = 0; c < 4; ++c)
        if (m.weights[vertex][c] > w) {
            w = m.weights[vertex][c];
            best = m.joints[vertex][c];
        }
    return best;
}

size_t nearestVertex(const RigMesh& m, Vec3 p) {
    size_t best = 0;
    f32 bd = 1e9f;
    for (size_t i = 0; i < m.positions.size(); ++i) {
        const f32 d = glm::length(m.positions[i] - p);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

} // namespace

TEST_CASE("template landmarks fit the bounds") {
    const auto lm = DefaultHumanoidLandmarks(Vec3(-0.8f, 0.2f, -0.2f), Vec3(0.8f, 2.0f, 0.3f)); // 1.8 m tall, arms out
    CHECK(lm[HumanoidJointIndex("Hips")].y > 0.2f + 0.45f * 1.8f);
    CHECK(lm[HumanoidJointIndex("HeadTop_End")].y == doctest::Approx(0.2f + 1.7855f * 1.0f).epsilon(0.03));
    CHECK(lm[HumanoidJointIndex("LeftHand")].x > 0.6f);
    CHECK(lm[HumanoidJointIndex("RightHand")].x < -0.6f);
    CHECK(lm[HumanoidJointIndex("LeftToeBase")].y < 0.4f);
}

TEST_CASE("template landmarks follow the arms of the model: out, hanging, or in between") {
    for (const f32 droopDegrees : {0.0f, 25.0f, 45.0f}) {
        const auto truth = [&] {
            auto lm = DefaultHumanoidLandmarks(Vec3(-0.75f, 0.0f, -0.1f), Vec3(0.75f, 1.8f, 0.25f));
            const f32 droop = glm::radians(droopDegrees);
            for (const bool left : {true, false}) {
                const f32 side = left ? 1.0f : -1.0f;
                const u32 arm = left ? 8u : 12u;
                const f32 foreLen = glm::length(lm[arm + 1] - lm[arm]), handLen = glm::length(lm[arm + 2] - lm[arm + 1]);
                const Vec3 d(side * std::cos(droop), -std::sin(droop), 0.0f);
                lm[arm + 1] = lm[arm] + d * foreLen;
                lm[arm + 2] = lm[arm + 1] + d * handLen;
            }
            return lm;
        }();
        // A model made of the tubes of that skeleton: its vertices are all the Forge sees.
        const RigMesh mesh = humanoidFromLandmarks(truth);
        Vec3 lo(1e9f), hi(-1e9f);
        for (const Vec3& p : mesh.positions) {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
        const auto found = DefaultHumanoidLandmarks(lo, hi, mesh.positions);
        const Vec3 handTruth = truth[10], handFound = found[10];
        INFO("droop " << droopDegrees);
        CHECK(glm::length(handFound - handTruth) < 0.12f);               // the wrist is where it really is, within a hand
        CHECK(found[10].y == doctest::Approx(handTruth.y).epsilon(0.05)); // and at the right height: out, or hanging by the right amount
    }
}

TEST_CASE("auto rig weights follow the bones and sum to 255") {
    HumanoidRigInput in;
    in.landmarks = DefaultHumanoidLandmarks(Vec3(-0.75f, 0.0f, -0.1f), Vec3(0.75f, 1.8f, 0.25f));
    in.meshes.push_back(humanoidFromLandmarks(in.landmarks));
    in.materials.push_back({"skin", Vec4(0.8f, 0.6f, 0.5f, 1.0f), 0.0f, 0.7f});
    HumanoidRigResult rig;
    REQUIRE_MESSAGE(BuildHumanoidRig(in, rig), rig.error);
    const RigMesh& m = in.meshes[0];
    REQUIRE(m.weights.size() == m.positions.size());
    for (size_t i = 0; i < m.weights.size(); ++i) {
        int sum = 0;
        for (int c = 0; c < 4; ++c) sum += m.weights[i][c];
        REQUIRE(sum == 255);
    }
    auto mid = [&](const char* a, const char* b) {
        return 0.5f * (in.landmarks[HumanoidJointIndex(a)] + in.landmarks[HumanoidJointIndex(b)]);
    };
    // Surface point on the side of the tube: offset sideways so the nearest vertex is on the limb, not on a ring end.
    CHECK(dominantJoint(m, nearestVertex(m, mid("LeftUpLeg", "LeftLeg") + Vec3(0.045f, 0, 0))) == u32(HumanoidJointIndex("LeftUpLeg")));
    CHECK(dominantJoint(m, nearestVertex(m, mid("RightLeg", "RightFoot") + Vec3(-0.045f, 0, 0))) == u32(HumanoidJointIndex("RightLeg")));
    CHECK(dominantJoint(m, nearestVertex(m, mid("LeftForeArm", "LeftHand") + Vec3(0, 0.045f, 0))) == u32(HumanoidJointIndex("LeftForeArm")));
    CHECK(dominantJoint(m, nearestVertex(m, mid("Head", "HeadTop_End") + Vec3(0.045f, 0, 0))) == u32(HumanoidJointIndex("Head")));

    // The skeleton puts every joint where its landmark is.
    const SkeletonAsset& sk = rig.skeleton;
    REQUIRE(sk.jointCount() == kHumanoidJointCount);
    std::vector<Mat4> world(sk.jointCount());
    for (u32 i = 0; i < sk.jointCount(); ++i) {
        const Mat4 local = glm::translate(Mat4(1.0f), sk.restPosition[i]) * glm::mat4_cast(sk.restRotation[i]);
        world[i] = sk.parents[i] >= 0 ? world[static_cast<u32>(sk.parents[i])] * local : local;
        CHECK(glm::length(Vec3(world[i][3]) - in.landmarks[i]) < 1e-3f);
        // Bind pose: inverse bind undoes the joint's own world transform.
        const Mat4 id = world[i] * sk.inverseBind[i];
        CHECK(glm::length(Vec3(id[3])) < 1e-3f);
    }
}

TEST_CASE("an A-pose is lifted to a T-pose") {
    HumanoidRigInput in;
    in.landmarks = DefaultHumanoidLandmarks(Vec3(-0.45f, 0.0f, -0.1f), Vec3(0.45f, 1.8f, 0.25f)); // narrow: arms hang
    const Vec3 armL = in.landmarks[HumanoidJointIndex("LeftArm")], handL = in.landmarks[HumanoidJointIndex("LeftHand")];
    REQUIRE(handL.y < armL.y - 0.2f); // really hanging
    in.meshes.push_back(humanoidFromLandmarks(in.landmarks));
    HumanoidRigResult rig;
    REQUIRE_MESSAGE(BuildHumanoidRig(in, rig), rig.error);
    CHECK(rig.armLiftDegrees[0] > 15.0f);
    CHECK(rig.armLiftDegrees[1] > 15.0f);
    const Vec3 handAfter = rig.jointPositions[HumanoidJointIndex("LeftHand")];
    CHECK(handAfter.y == doctest::Approx(armL.y).epsilon(0.02));
    CHECK(handAfter.x > handL.x); // swung outward
    // The same length: only rotated.
    CHECK(glm::length(handAfter - armL) == doctest::Approx(glm::length(handL - armL)).epsilon(0.01));
}

TEST_CASE("rigged glb imports as a skinned prefab with Mixamo joint names") {
    HumanoidRigInput in;
    in.landmarks = DefaultHumanoidLandmarks(Vec3(-0.75f, 0.0f, -0.1f), Vec3(0.75f, 1.8f, 0.25f));
    in.meshes.push_back(humanoidFromLandmarks(in.landmarks));
    in.materials.push_back({"skin", Vec4(0.8f, 0.6f, 0.5f, 1.0f), 0.0f, 0.7f});
    HumanoidRigResult rig;
    REQUIRE_MESSAGE(BuildHumanoidRig(in, rig), rig.error);
    const std::vector<u8> glb = WriteRiggedGlb(in, rig);
    REQUIRE(glb.size() > 1000);

    TempDir t;
    fs::create_directories(t.path / "assets" / "imported" / "hero");
    const fs::path file = t.path / "assets" / "imported" / "hero" / "hero.glb";
    {
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
    }
    AssetDb db({t.path / "assets", t.path / "cache"});
    const ImportResult r = db.importFile(file);
    INFO(r.error);
    REQUIRE(r.ok);
    REQUIRE(db.assetsOfKind(AssetKind::Skeleton).size() == 1);
    SkeletonAsset sk;
    REQUIRE(db.load(db.assetsOfKind(AssetKind::Skeleton)[0], sk));
    CHECK(sk.jointCount() == kHumanoidJointCount);
    for (u32 i = 0; i < kHumanoidJointCount; ++i) CHECK(sk.findJoint(HumanoidJointName(i)) >= 0);
    CHECK(glm::length(sk.restRotation[sk.findJoint("LeftShoulder")] - rig.skeleton.restRotation[HumanoidJointIndex("LeftShoulder")]) < 1e-3f);

    PrefabAsset prefab;
    REQUIRE(db.load(r.prefab, prefab));
    CHECK(prefab.skeleton.isValid());
    bool skinned = false;
    for (const PrefabNode& n : prefab.nodes) skinned = skinned || (n.skinned && n.mesh.isValid());
    CHECK(skinned);
    MeshAsset mesh;
    REQUIRE(db.load(db.assetsOfKind(AssetKind::Mesh)[0], mesh));
    CHECK(mesh.skinned());
    CHECK(mesh.findStream(VertexSemantic::Joints0) != nullptr);
}

// QA tool, not an assertion: writes an UNSKINNED copy of the Y Bot mesh (T-pose, meters) as a glb, a humanoid to try the
// Asset Forge's auto-rig on. FOH_TEST_STATIC_OUT = the output file; the imported Y Bot meshes must be in the cache.
TEST_CASE("QA tool: write a static copy of the Y Bot mesh" * doctest::skip(std::getenv("FOH_TEST_STATIC_OUT") == nullptr)) {
    const fs::path cache = fs::path("C:/Users/souza/Desktop/Workspace/gtaBR/cache/imported");
    const char* ids[] = {"256d21b1be4617ea1a0c1a40689f58e8", "e20782e9dc6a74b55f0c6d835bbf6af1"};
    std::vector<u8> bin;
    std::string accessors, views, meshes, nodes;
    int accessorCount = 0, viewCount = 0;
    auto addView = [&](const void* data, size_t size, int target) {
        while (bin.size() % 4) bin.push_back(0);
        if (viewCount) views += ',';
        views += "{\"buffer\":0,\"byteOffset\":" + std::to_string(bin.size()) + ",\"byteLength\":" + std::to_string(size) + ",\"target\":" + std::to_string(target) + "}";
        bin.insert(bin.end(), static_cast<const u8*>(data), static_cast<const u8*>(data) + size);
        return viewCount++;
    };
    auto addAccessor = [&](int view, int comp, size_t count, const char* type, const std::string& extra = "") {
        if (accessorCount) accessors += ',';
        accessors += "{\"bufferView\":" + std::to_string(view) + ",\"componentType\":" + std::to_string(comp) + ",\"count\":" + std::to_string(count) + ",\"type\":\"" + type + "\"" + extra + "}";
        return accessorCount++;
    };
    std::array<Vec3, kHumanoidJointCount> sharedLandmarks{};
    bool haveLandmarks = false;
    for (int m = 0; m < 2; ++m) {
        std::vector<u8> bytes;
        REQUIRE(ReadFileBytes(cache / (std::string(ids[m]) + ".fmesh"), bytes));
        auto reader = ContainerReader::Open(std::move(bytes));
        REQUIRE(reader);
        MeshAsset mesh;
        REQUIRE(DeserializeMesh(*reader, mesh));
        const u32 n = mesh.vertexCount();
        std::vector<Vec3> pos(n), nor(n);
        std::vector<Vec2> uv(n);
        const VertexStream* sp = mesh.findStream(VertexSemantic::Position);
        const VertexStream* sn = mesh.findStream(VertexSemantic::Normal);
        const VertexStream* su = mesh.findStream(VertexSemantic::Uv0);
        for (u32 v = 0; v < n; ++v) {
            const u8* base = mesh.vertexData.data() + size_t(v) * mesh.vertexStride;
            std::memcpy(&pos[v], base + sp->offset, 12);
            std::memcpy(&nor[v], base + sn->offset, 12);
            if (su) std::memcpy(&uv[v], base + su->offset, 8);
        }
        // FOH_TEST_APOSE=<degrees>: lower the arms (skinned with the rig's own weights) to make an A-pose model.
        if (const char* apose = std::getenv("FOH_TEST_APOSE")) {
            const f32 angle = glm::radians(static_cast<f32>(std::atof(apose)));
            HumanoidRigInput in;
            Vec3 plo(1e9f), phi(-1e9f);
            for (const Vec3& p : pos) {
                plo = glm::min(plo, p);
                phi = glm::max(phi, p);
            }
            if (!haveLandmarks) { // the first part is the body: every part uses ITS skeleton
                sharedLandmarks = DefaultHumanoidLandmarks(plo, phi);
                haveLandmarks = true;
            }
            in.landmarks = sharedLandmarks;
            in.normalizeTPose = false;
            RigMesh rm;
            rm.name = "m";
            rm.positions = pos;
            rm.indices = mesh.indices;
            rm.primitives.push_back({0, static_cast<u32>(mesh.indices.size()), 0});
            in.meshes.push_back(rm);
            HumanoidRigResult rig;
            REQUIRE(BuildHumanoidRig(in, rig));
            const RigMesh& out = in.meshes[0];
            for (u32 v = 0; v < n; ++v) {
                for (int side = 0; side < 2; ++side) {
                    const u32 first = side == 0 ? 8u : 12u; // LeftArm / RightArm, then the forearm and the hand
                    f32 belongs = 0.0f;
                    for (int c = 0; c < 4; ++c)
                        if (out.joints[v][c] >= first && out.joints[v][c] <= first + 2) belongs += out.weights[v][c] / 255.0f;
                    if (belongs <= 0.0f) continue;
                    const Quat rot = glm::angleAxis((side == 0 ? -1.0f : 1.0f) * angle * belongs, Vec3(0, 0, 1));
                    const Vec3 pivot = in.landmarks[first];
                    pos[v] = pivot + rot * (pos[v] - pivot);
                    nor[v] = glm::normalize(rot * nor[v]);
                }
            }
        }
        Vec3 lo(1e9f), hi(-1e9f);
        for (const Vec3& p : pos) {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
        char minmax[200];
        std::snprintf(minmax, sizeof(minmax), ",\"min\":[%f,%f,%f],\"max\":[%f,%f,%f]", lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
        const int a0 = addAccessor(addView(pos.data(), n * 12, 34962), 5126, n, "VEC3", minmax);
        const int a1 = addAccessor(addView(nor.data(), n * 12, 34962), 5126, n, "VEC3");
        const int a2 = addAccessor(addView(uv.data(), n * 8, 34962), 5126, n, "VEC2");
        const int a3 = addAccessor(addView(mesh.indices.data(), mesh.indices.size() * 4, 34963), 5125, mesh.indices.size(), "SCALAR");
        if (m) { meshes += ','; nodes += ','; }
        meshes += "{\"name\":\"part" + std::to_string(m) + "\",\"primitives\":[{\"attributes\":{\"POSITION\":" + std::to_string(a0) + ",\"NORMAL\":" + std::to_string(a1) + ",\"TEXCOORD_0\":" + std::to_string(a2) + "},\"indices\":" + std::to_string(a3) + ",\"material\":" + std::to_string(m) + "}]}";
        nodes += "{\"name\":\"part" + std::to_string(m) + "\",\"mesh\":" + std::to_string(m) + "}";
        MESSAGE("part " << m << ": " << n << " vertices, bounds " << lo.y << " .. " << hi.y);
    }
    while (bin.size() % 4) bin.push_back(0);
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0,1]}],\"nodes\":[" + nodes + "],\"meshes\":[" + meshes +
                             "],\"materials\":[{\"name\":\"body\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.35,0.65,0.75,1],\"metallicFactor\":0.2,\"roughnessFactor\":0.5}},{\"name\":\"joints\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.1,0.1,0.12,1],\"metallicFactor\":0.5,\"roughnessFactor\":0.5}}],\"accessors\":[" +
                             accessors + "],\"bufferViews\":[" + views + "],\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + ",\"uri\":\"data:application/octet-stream;base64," + base64(bin) + "\"}]}";
    std::ofstream out(std::getenv("FOH_TEST_STATIC_OUT"), std::ios::binary);
    out << json;
    CHECK(out.good());
}
