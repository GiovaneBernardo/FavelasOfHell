#include "foh/humanoid_rig.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_map>

namespace foh {

namespace {

constexpr u32 kJoints = kHumanoidJointCount;

// Joint indices of the humanoid (the order of HumanoidJointName).
enum Joint : u32 {
    Hips, Spine, Spine1, Spine2, Neck, Head, HeadTopEnd,
    LShoulder, LArm, LForeArm, LHand,
    RShoulder, RArm, RForeArm, RHand,
    LUpLeg, LLeg, LFoot, LToeBase, LToeEnd,
    RUpLeg, RLeg, RFoot, RToeBase, RToeEnd,
};

// Mixamo's rig as exported for Y Bot: parent, local rest rotation (w x y z) and model-space position of the T-pose.
struct TemplateJoint {
    i32 parent;
    Quat rotation;
    Vec3 position;
};

const std::array<TemplateJoint, kJoints>& templateJoints() {
    static const std::array<TemplateJoint, kJoints> t = {{
        {-1, {1.0000000f, 0.0000000f, -0.0000017f, 0.0000436f}, {0.00000f, 0.99792f, 0.00000f}},
        {0, {0.9981542f, -0.0607302f, 0.0000000f, 0.0000061f}, {0.00000f, 1.09715f, -0.01227f}},
        {1, {1.0000000f, 0.0001961f, 0.0000000f, -0.0000093f}, {0.00000f, 1.21361f, -0.02650f}},
        {2, {0.9983333f, 0.0577117f, 0.0000022f, 0.0000033f}, {0.00000f, 1.34721f, -0.04276f}},
        {3, {0.9999960f, 0.0028277f, 0.0000000f, 0.0000000f}, {0.00000f, 1.49754f, -0.03483f}},
        {4, {1.0000000f, 0.0000000f, 0.0000000f, 0.0000000f}, {0.00000f, 1.60075f, -0.00341f}},
        {5, {1.0000000f, 0.0000000f, 0.0000000f, 0.0000000f}, {0.00000f, 1.78550f, 0.06296f}},
        {3, {-0.4397942f, -0.4538351f, -0.5448494f, 0.5511389f}, {0.06103f, 1.43832f, -0.03571f}},
        {7, {0.9948156f, -0.0104518f, -0.0010627f, 0.1011505f}, {0.18758f, 1.43567f, -0.06171f}},
        {8, {1.0000000f, 0.0000000f, 0.0000000f, 0.0000000f}, {0.46163f, 1.43570f, -0.06171f}},
        {9, {1.0000000f, 0.0000000f, 0.0000000f, 0.0000000f}, {0.73777f, 1.43572f, -0.06171f}},
        {3, {-0.4397927f, -0.4538376f, 0.5448472f, -0.5511400f}, {-0.06109f, 1.43831f, -0.03571f}},
        {11, {0.9948158f, -0.0104554f, 0.0010631f, -0.1011494f}, {-0.18764f, 1.43564f, -0.06171f}},
        {12, {1.0000000f, 0.0000000f, 0.0000000f, 0.0000000f}, {-0.46168f, 1.43562f, -0.06171f}},
        {13, {1.0000000f, 0.0000000f, 0.0000000f, 0.0000000f}, {-0.73783f, 1.43559f, -0.06171f}},
        {0, {0.0029696f, -0.0000203f, 0.0063502f, -0.9999754f}, {0.09124f, 0.93136f, -0.00055f}},
        {15, {0.9998184f, -0.0181143f, 0.0001825f, -0.0059201f}, {0.09369f, 0.52540f, -0.00571f}},
        {16, {0.8408193f, 0.5405499f, 0.0143665f, 0.0249510f}, {0.09124f, 0.10492f, -0.02630f}},
        {17, {0.9730687f, 0.2276921f, -0.0325215f, -0.0153656f}, {0.09498f, 0.03284f, 0.11336f}},
        {18, {1.0000000f, 0.0000000f, 0.0000000f, 0.0000000f}, {0.09356f, 0.03108f, 0.21334f}},
        {0, {0.0030575f, -0.0000180f, -0.0063338f, 0.9999753f}, {-0.09124f, 0.93136f, -0.00055f}},
        {20, {0.9998178f, -0.0181471f, -0.0001825f, 0.0059201f}, {-0.09369f, 0.52540f, -0.00570f}},
        {21, {0.8408105f, 0.5405636f, -0.0143660f, -0.0249512f}, {-0.09125f, 0.10492f, -0.02630f}},
        {22, {0.9730798f, 0.2277087f, 0.0321196f, 0.0152579f}, {-0.09498f, 0.03284f, 0.11336f}},
        {23, {1.0000000f, 0.0000000f, 0.0000000f, 0.0000000f}, {-0.09356f, 0.03109f, 0.21334f}},
    }};
    return t;
}

constexpr f32 kTemplateHeight = 1.80f; // mesh height of the template: HeadTop_End plus the skull above it

// ---- bones -----------------------------------------------------------------------------------------------------

struct Bone {
    bool active = false;
    Vec3 a{0.0f}, b{0.0f};
    f32 radius = 0.05f; // template meters, scaled with the character
    f32 minY = -1e9f, maxY = 1e9f; // legs never reach above the hips / below the ankles (feet), in character units
};

f32 distanceToSegment(const Vec3& p, const Vec3& a, const Vec3& b) {
    const Vec3 ab = b - a;
    const f32 len2 = glm::dot(ab, ab);
    const f32 t = len2 > 1e-12f ? std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    return glm::length(p - (a + ab * t));
}

std::array<Bone, kJoints> makeBones(const std::array<Vec3, kJoints>& p, f32 scale) {
    std::array<Bone, kJoints> bones{};
    auto set = [&](Joint j, Vec3 a, Vec3 b, f32 radius) {
        bones[j].active = true;
        bones[j].a = a;
        bones[j].b = b;
        bones[j].radius = radius * scale;
    };
    set(Hips, p[Hips], p[Spine], 0.16f);
    set(Spine, p[Spine], p[Spine1], 0.15f);
    set(Spine1, p[Spine1], p[Spine2], 0.16f);
    set(Spine2, p[Spine2], p[Neck], 0.17f);
    set(Neck, p[Neck], p[Head], 0.065f);
    set(Head, p[Head], p[HeadTopEnd], 0.11f);
    for (const bool left : {true, false}) {
        const Joint sh = left ? LShoulder : RShoulder, arm = Joint(sh + 1), fore = Joint(sh + 2), hand = Joint(sh + 3);
        set(sh, p[sh], p[arm], 0.06f);
        set(arm, p[arm], p[fore], 0.05f);
        set(fore, p[fore], p[hand], 0.042f);
        // The hand bone reaches past the wrist to the fingertips.
        const Vec3 dir = glm::normalize(p[hand] - p[fore]);
        set(hand, p[hand], p[hand] + dir * 0.19f * scale, 0.045f);
        const Joint up = left ? LUpLeg : RUpLeg, leg = Joint(up + 1), foot = Joint(up + 2), toe = Joint(up + 3);
        set(up, p[up], p[leg], 0.09f);
        set(leg, p[leg], p[foot], 0.06f);
        set(foot, p[foot], p[toe], 0.05f);
        set(toe, p[toe], p[Joint(toe + 1)], 0.035f);
        // Legs stay above the ankle, feet and toes below the shin: the heel is the foot's, never the leg's.
        bones[up].minY = p[foot].y - 0.02f * scale;
        bones[leg].minY = p[foot].y - 0.02f * scale;
    }
    return bones;
}

// ---- welded vertex graph -----------------------------------------------------------------------------------------

struct Welded {
    std::vector<u32> uniqueOf;    // vertex -> unique point
    std::vector<Vec3> points;     // unique point -> position
    std::vector<u32> adjStart;    // CSR neighbour lists over the unique points
    std::vector<u32> adj;
    f32 meanEdge = 0.0f;
};

struct KeyHash {
    size_t operator()(const std::array<i32, 3>& k) const {
        return static_cast<size_t>(static_cast<u32>(k[0]) * 73856093u ^ static_cast<u32>(k[1]) * 19349663u ^ static_cast<u32>(k[2]) * 83492791u);
    }
};

Welded weld(const RigMesh& mesh) {
    Welded w;
    const size_t n = mesh.positions.size();
    w.uniqueOf.resize(n);
    std::unordered_map<std::array<i32, 3>, u32, KeyHash> seen;
    seen.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const Vec3& p = mesh.positions[i];
        const std::array<i32, 3> key = {static_cast<i32>(std::lround(p.x * 20000.0f)), static_cast<i32>(std::lround(p.y * 20000.0f)),
                                        static_cast<i32>(std::lround(p.z * 20000.0f))};
        const auto [it, inserted] = seen.try_emplace(key, static_cast<u32>(w.points.size()));
        if (inserted) w.points.push_back(p);
        w.uniqueOf[i] = it->second;
    }
    // Neighbours from triangle edges, deduplicated.
    std::vector<std::pair<u32, u32>> edges;
    edges.reserve(mesh.indices.size());
    for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const u32 a = w.uniqueOf[mesh.indices[t]], b = w.uniqueOf[mesh.indices[t + 1]], c = w.uniqueOf[mesh.indices[t + 2]];
        if (a != b) { edges.emplace_back(a, b); edges.emplace_back(b, a); }
        if (b != c) { edges.emplace_back(b, c); edges.emplace_back(c, b); }
        if (c != a) { edges.emplace_back(c, a); edges.emplace_back(a, c); }
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    w.adjStart.assign(w.points.size() + 1, 0);
    for (const auto& e : edges) ++w.adjStart[e.first + 1];
    for (size_t i = 0; i < w.points.size(); ++i) w.adjStart[i + 1] += w.adjStart[i];
    w.adj.resize(edges.size());
    std::vector<u32> fill(w.adjStart.begin(), w.adjStart.end() - 1);
    f64 lengthSum = 0.0;
    for (const auto& e : edges) {
        w.adj[fill[e.first]++] = e.second;
        lengthSum += glm::length(w.points[e.first] - w.points[e.second]);
    }
    w.meanEdge = edges.empty() ? 0.0f : static_cast<f32>(lengthSum / static_cast<f64>(edges.size()));
    return w;
}

} // namespace

// ---- public: template and landmarks -------------------------------------------------------------------------------

const std::array<Vec3, kHumanoidJointCount>& HumanoidTemplatePositions() {
    static const std::array<Vec3, kJoints> positions = [] {
        std::array<Vec3, kJoints> p;
        for (u32 i = 0; i < kJoints; ++i) p[i] = templateJoints()[i].position;
        return p;
    }();
    return positions;
}

i32 HumanoidJointParent(u32 index) { return index < kJoints ? templateJoints()[index].parent : -1; }

std::array<Vec3, kHumanoidJointCount> DefaultHumanoidLandmarks(Vec3 boundsMin, Vec3 boundsMax, std::span<const Vec3> points) {
    const auto& tpl = HumanoidTemplatePositions();
    const f32 height = std::max(boundsMax.y - boundsMin.y, 0.1f);
    const f32 s = height / kTemplateHeight;
    const f32 cx = 0.5f * (boundsMin.x + boundsMax.x), cz = 0.5f * (boundsMin.z + boundsMax.z) - 0.05f * s;
    std::array<Vec3, kJoints> out;
    for (u32 i = 0; i < kJoints; ++i) out[i] = Vec3(cx + tpl[i].x * s, boundsMin.y + tpl[i].y * s, cz + tpl[i].z * s);

    const f32 width = boundsMax.x - boundsMin.x;
    const f32 reach = 0.5f * width - 0.02f * height; // from the centre line to the fingertips
    for (const bool left : {true, false}) {
        const f32 side = left ? 1.0f : -1.0f;
        const Joint sh = left ? LShoulder : RShoulder, arm = Joint(sh + 1), fore = Joint(sh + 2), hand = Joint(sh + 3);
        const f32 foreLen = glm::length(tpl[fore] - tpl[arm]) * s, handLen = glm::length(tpl[hand] - tpl[fore]) * s;
        const f32 armLen = foreLen + handLen + 0.19f * s; // elbow + wrist + the fingers beyond it
        const f32 armRoot = std::abs(tpl[arm].x) * s;

        // The fingertips: the average of the outermost vertices on this side. The arms are what sticks out sideways, so this
        // is the hand whether the arms are out or hanging.
        Vec3 tip(0.0f);
        int tips = 0;
        if (!points.empty() && width > 0.2f * height) {
            const f32 xMin = side > 0 ? cx + 0.80f * 0.5f * width : boundsMin.x, xMax = side > 0 ? boundsMax.x : cx - 0.80f * 0.5f * width;
            // Only the arm region: the shoulders down to the hips (a raised hand or a hat brim must not count).
            const f32 yLow = boundsMin.y + 0.40f * height, yHigh = boundsMin.y + 0.90f * height;
            for (const Vec3& p : points)
                if (p.x >= xMin && p.x <= xMax && p.y >= yLow && p.y <= yHigh) {
                    tip += p;
                    ++tips;
                }
            if (tips >= 8) tip /= static_cast<f32>(tips);
            else tips = 0;
        }
        if (tips > 0) {
            // Point the arm at the fingertips, scaled to reach them.
            const Vec3 root = out[arm];
            const Vec3 toTip = tip - root;
            const f32 length = glm::length(toTip);
            if (length > 0.05f * height) {
                const Vec3 d = toTip / length;
                const f32 k = std::clamp(length / (armLen - 0.5f * 0.19f * s), 0.6f, 1.6f); // the average tip is a bit short of the very end
                out[fore] = root + d * (foreLen * k);
                out[hand] = out[fore] + d * (handLen * k);
                continue;
            }
        }
        if (width > 0.6f * height) {
            // Arms out (T-pose): stretch the arm chain to the width of the model.
            const f32 k = std::max((reach - armRoot) / std::max(armLen, 1e-3f), 0.3f);
            for (const Joint j : {fore, hand}) out[j].x = cx + side * (armRoot + (std::abs(tpl[j].x) * s - armRoot) * k);
        } else {
            // Arms down: the elbow and wrist hang from the shoulder at the angle that makes the hands reach the sides.
            const f32 sinPhi = std::clamp((reach - armRoot) / std::max(armLen, 1e-3f), 0.0f, 0.7f);
            const f32 cosPhi = std::sqrt(1.0f - sinPhi * sinPhi);
            out[fore] = out[arm] + Vec3(side * sinPhi, -cosPhi, 0.0f) * foreLen;
            out[hand] = out[fore] + Vec3(side * sinPhi, -cosPhi, 0.0f) * handLen;
        }
    }
    return out;
}

// ---- public: the rig -----------------------------------------------------------------------------------------------

bool BuildHumanoidRig(HumanoidRigInput& input, HumanoidRigResult& result) {
    result = {};
    auto fail = [&](std::string why) {
        result.error = std::move(why);
        return false;
    };
    if (input.meshes.empty()) return fail("There is no mesh to rig.");
    for (const RigMesh& m : input.meshes)
        if (m.positions.empty() || m.indices.size() < 3) return fail("The mesh '" + m.name + "' has no triangles.");

    std::array<Vec3, kJoints> p = input.landmarks;
    for (const Vec3& v : p)
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) return fail("A landmark is not a number.");
    // A size reference: the template's hips-to-head length, so thicknesses follow the character.
    const f32 trunk = glm::length(p[HeadTopEnd] - p[Hips]);
    const f32 trunkTemplate = glm::length(HumanoidTemplatePositions()[HeadTopEnd] - HumanoidTemplatePositions()[Hips]);
    if (trunk < 0.05f) return fail("The head and hips landmarks are on top of each other.");
    const f32 scale = trunk / trunkTemplate;
    for (u32 i = 1; i < kJoints; ++i) {
        const i32 parent = HumanoidJointParent(i);
        if (parent >= 0 && glm::length(p[i] - p[static_cast<u32>(parent)]) < 1e-4f && i != LToeEnd && i != RToeEnd && i != HeadTopEnd)
            return fail(std::format("The landmarks of '{}' and '{}' are at the same place.", HumanoidJointName(i), HumanoidJointName(static_cast<u32>(parent))));
    }

    const std::array<Bone, kJoints> bones = makeBones(p, scale);
    const f32 k = 4.0f + (1.0f - std::clamp(input.smoothness, 0.0f, 1.0f)) * 10.0f; // sharpness of the nearest-bone preference
    const f32 sigma = (0.012f + 0.07f * std::clamp(input.smoothness, 0.0f, 1.0f)) * scale; // meters of diffusion along the surface

    // Arm lift angles (A-pose -> T-pose).
    struct ArmLift {
        Joint first; // arm joint: the pivot
        f32 angle = 0.0f;
        Vec3 pivot{0.0f};
    };
    ArmLift lifts[2] = {{LArm}, {RArm}};
    if (input.normalizeTPose) {
        for (ArmLift& lift : lifts) {
            const Joint hand = Joint(lift.first + 2);
            const Vec3 d = p[hand] - p[lift.first];
            const f32 horizontal = std::sqrt(d.x * d.x + d.z * d.z);
            const f32 droop = std::atan2(-d.y, horizontal); // > 0: below horizontal
            if (droop > 0.05f && droop < 1.6f) lift.angle = droop;
            lift.pivot = p[lift.first];
        }
    }
    result.armLiftDegrees = {glm::degrees(lifts[0].angle), glm::degrees(lifts[1].angle)};

    for (RigMesh& mesh : input.meshes) {
        const Welded w = weld(mesh);
        const size_t n = w.points.size();
        std::vector<f32> weights(n * kJoints, 0.0f);

        // 1. nearest bones
        for (size_t v = 0; v < n; ++v) {
            f32 ratio[kJoints];
            f32 best = 1e9f;
            for (u32 j = 0; j < kJoints; ++j) {
                ratio[j] = 1e9f;
                const Bone& bone = bones[j];
                if (!bone.active) continue;
                f32 d = distanceToSegment(w.points[v], bone.a, bone.b) / bone.radius;
                if (w.points[v].y < bone.minY) d += (bone.minY - w.points[v].y) / bone.radius * 4.0f; // the gate: below the ankle a leg bone is far away
                ratio[j] = d;
                best = std::min(best, d);
            }
            best = std::max(best, 0.05f);
            f32 sum = 0.0f;
            for (u32 j = 0; j < kJoints; ++j) {
                if (ratio[j] >= 1e8f) continue;
                const f32 wt = std::exp(-k * (ratio[j] / best - 1.0f));
                weights[v * kJoints + j] = wt;
                sum += wt;
            }
            for (u32 j = 0; j < kJoints; ++j) weights[v * kJoints + j] /= sum;
        }

        // 2. diffusion along the surface: joints bend smoothly
        if (w.meanEdge > 1e-6f && sigma > 0.0f) {
            const f32 lambda = 0.5f;
            const int iterations = std::clamp(static_cast<int>(std::ceil(2.0f * sigma * sigma / (lambda * w.meanEdge * w.meanEdge))), 0, 160);
            std::vector<f32> next(weights.size());
            for (int it = 0; it < iterations; ++it) {
                for (size_t v = 0; v < n; ++v) {
                    const u32 begin = w.adjStart[v], end = w.adjStart[v + 1];
                    f32* out = &next[v * kJoints];
                    const f32* self = &weights[v * kJoints];
                    if (begin == end) {
                        std::memcpy(out, self, sizeof(f32) * kJoints);
                        continue;
                    }
                    f32 avg[kJoints] = {};
                    for (u32 e = begin; e < end; ++e) {
                        const f32* nb = &weights[size_t(w.adj[e]) * kJoints];
                        for (u32 j = 0; j < kJoints; ++j) avg[j] += nb[j];
                    }
                    const f32 inv = 1.0f / static_cast<f32>(end - begin);
                    for (u32 j = 0; j < kJoints; ++j) out[j] = self[j] * (1.0f - lambda) + avg[j] * inv * lambda;
                }
                weights.swap(next);
            }
        }

        // 3. the A-pose lift, weighted by how much of the arm chain a point belongs to
        if (input.normalizeTPose) {
            for (size_t i = 0; i < mesh.positions.size(); ++i) {
                const size_t v = w.uniqueOf[i];
                for (int side = 0; side < 2; ++side) {
                    const ArmLift& lift = lifts[side];
                    if (lift.angle == 0.0f) continue;
                    const f32 belongs = weights[v * kJoints + lift.first] + weights[v * kJoints + lift.first + 1] + weights[v * kJoints + lift.first + 2];
                    if (belongs < 1e-4f) continue;
                    const f32 angle = (side == 0 ? 1.0f : -1.0f) * lift.angle * belongs;
                    const Quat rot = glm::angleAxis(angle, Vec3(0.0f, 0.0f, 1.0f));
                    mesh.positions[i] = lift.pivot + rot * (mesh.positions[i] - lift.pivot);
                    if (!mesh.normals.empty()) mesh.normals[i] = glm::normalize(rot * mesh.normals[i]);
                    if (!mesh.tangents.empty()) mesh.tangents[i] = Vec4(rot * Vec3(mesh.tangents[i]), mesh.tangents[i].w);
                }
            }
        }

        // 4. the strongest four influences per vertex, as bytes that add up to 255
        mesh.joints.assign(mesh.positions.size(), {0, 0, 0, 0});
        mesh.weights.assign(mesh.positions.size(), {255, 0, 0, 0});
        for (size_t i = 0; i < mesh.positions.size(); ++i) {
            const f32* wt = &weights[size_t(w.uniqueOf[i]) * kJoints];
            u32 order[kJoints];
            for (u32 j = 0; j < kJoints; ++j) order[j] = j;
            std::partial_sort(order, order + 4, order + kJoints, [&](u32 a, u32 b) { return wt[a] > wt[b]; });
            f32 total = 0.0f;
            for (int c = 0; c < 4; ++c) total += wt[order[c]] > 0.01f ? wt[order[c]] : 0.0f;
            if (total <= 0.0f) { // no bone claims it: follow the strongest
                mesh.joints[i] = {static_cast<u8>(order[0]), 0, 0, 0};
                mesh.weights[i] = {255, 0, 0, 0};
                continue;
            }
            int assigned = 0, strongest = 0;
            for (int c = 0; c < 4; ++c) {
                const f32 v = wt[order[c]] > 0.01f ? wt[order[c]] / total : 0.0f;
                const int q = static_cast<int>(std::lround(v * 255.0f));
                mesh.joints[i][static_cast<size_t>(c)] = static_cast<u8>(order[c]);
                mesh.weights[i][static_cast<size_t>(c)] = static_cast<u8>(q);
                assigned += q;
                if (q > mesh.weights[i][static_cast<size_t>(strongest)]) strongest = c;
            }
            mesh.weights[i][static_cast<size_t>(strongest)] = static_cast<u8>(std::clamp(int(mesh.weights[i][static_cast<size_t>(strongest)]) + 255 - assigned, 0, 255));
        }
    }

    // The joint positions after the lift: the arm chain swings up around the arm joint.
    for (int side = 0; side < 2; ++side) {
        const ArmLift& lift = lifts[side];
        if (lift.angle == 0.0f) continue;
        const Quat rot = glm::angleAxis((side == 0 ? 1.0f : -1.0f) * lift.angle, Vec3(0.0f, 0.0f, 1.0f));
        for (u32 c = 1; c <= 2; ++c) p[lift.first + c] = lift.pivot + rot * (p[lift.first + c] - lift.pivot);
    }
    result.jointPositions = p;

    // The skeleton: Mixamo's rest rotations, joint positions from the landmarks.
    SkeletonAsset& sk = result.skeleton;
    sk.name = "Humanoid";
    std::array<Quat, kJoints> worldRot;
    for (u32 i = 0; i < kJoints; ++i) {
        const TemplateJoint& t = templateJoints()[i];
        sk.jointNames.push_back(HumanoidJointName(i));
        sk.jointHashes.push_back(JointNameHash(sk.jointNames.back()));
        sk.parents.push_back(t.parent);
        worldRot[i] = t.parent >= 0 ? worldRot[static_cast<u32>(t.parent)] * t.rotation : t.rotation;
        const Vec3 local = t.parent >= 0 ? glm::conjugate(worldRot[static_cast<u32>(t.parent)]) * (p[i] - p[static_cast<u32>(t.parent)]) : p[i];
        sk.restPosition.push_back(local);
        sk.restRotation.push_back(t.rotation);
        sk.restScale.push_back(Vec3(1.0f));
        sk.inverseBind.push_back(glm::inverse(glm::translate(Mat4(1.0f), p[i]) * glm::mat4_cast(worldRot[i])));
    }
    result.ok = true;
    return true;
}

// ---- glb -----------------------------------------------------------------------------------------------------------

namespace {

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
        else out += c;
    }
    return out;
}

struct BinBuilder {
    std::vector<u8> data;
    std::string views, accessors;
    int viewCount = 0, accessorCount = 0;

    int addView(const void* bytes, size_t size, int target) {
        while (data.size() % 4) data.push_back(0);
        const size_t offset = data.size();
        data.insert(data.end(), static_cast<const u8*>(bytes), static_cast<const u8*>(bytes) + size);
        if (viewCount) views += ',';
        views += std::format("{{\"buffer\":0,\"byteOffset\":{},\"byteLength\":{}{}}}", offset, size, target ? std::format(",\"target\":{}", target) : "");
        return viewCount++;
    }
    int addAccessor(int view, int componentType, size_t count, const char* type, bool normalized = false, const std::string& bounds = {}) {
        if (accessorCount) accessors += ',';
        accessors += std::format("{{\"bufferView\":{},\"componentType\":{},\"count\":{},\"type\":\"{}\"{}{}}}", view, componentType, count, type,
                                 normalized ? ",\"normalized\":true" : "", bounds);
        return accessorCount++;
    }
};

} // namespace

std::vector<u8> WriteRiggedGlb(const HumanoidRigInput& input, const HumanoidRigResult& rig) {
    if (!rig.ok) return {};
    BinBuilder bin;
    std::string nodes, meshes, materials;
    const SkeletonAsset& sk = rig.skeleton;

    // Joint nodes 0..24 in humanoid order.
    std::array<std::vector<u32>, kJoints> children;
    for (u32 i = 1; i < kJoints; ++i) children[static_cast<u32>(sk.parents[i])].push_back(i);
    for (u32 i = 0; i < kJoints; ++i) {
        const Vec3& t = sk.restPosition[i];
        const Quat& q = sk.restRotation[i];
        if (i) nodes += ',';
        nodes += std::format("{{\"name\":\"{}\",\"translation\":[{},{},{}],\"rotation\":[{},{},{},{}]", sk.jointNames[i], t.x, t.y, t.z, q.x, q.y, q.z, q.w);
        if (!children[i].empty()) {
            nodes += ",\"children\":[";
            for (size_t c = 0; c < children[i].size(); ++c) nodes += std::format("{}{}", c ? "," : "", children[i][c]);
            nodes += ']';
        }
        nodes += '}';
    }

    // Inverse bind matrices.
    std::vector<f32> ibm;
    for (u32 i = 0; i < kJoints; ++i) {
        const Mat4& m = sk.inverseBind[i];
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) ibm.push_back(m[c][r]);
    }
    const int ibmAccessor = bin.addAccessor(bin.addView(ibm.data(), ibm.size() * sizeof(f32), 0), 5126, kJoints, "MAT4");

    std::string sceneNodes = "0";
    u32 nodeIndex = kJoints;
    for (size_t mi = 0; mi < input.meshes.size(); ++mi) {
        const RigMesh& mesh = input.meshes[mi];
        Vec3 lo(1e30f), hi(-1e30f);
        for (const Vec3& v : mesh.positions) {
            lo = glm::min(lo, v);
            hi = glm::max(hi, v);
        }
        const int posAcc = bin.addAccessor(bin.addView(mesh.positions.data(), mesh.positions.size() * sizeof(Vec3), 34962), 5126, mesh.positions.size(), "VEC3", false,
                                           std::format(",\"min\":[{},{},{}],\"max\":[{},{},{}]", lo.x, lo.y, lo.z, hi.x, hi.y, hi.z));
        std::string attrs = std::format("\"POSITION\":{}", posAcc);
        if (mesh.normals.size() == mesh.positions.size())
            attrs += std::format(",\"NORMAL\":{}", bin.addAccessor(bin.addView(mesh.normals.data(), mesh.normals.size() * sizeof(Vec3), 34962), 5126, mesh.normals.size(), "VEC3"));
        if (mesh.tangents.size() == mesh.positions.size())
            attrs += std::format(",\"TANGENT\":{}", bin.addAccessor(bin.addView(mesh.tangents.data(), mesh.tangents.size() * sizeof(Vec4), 34962), 5126, mesh.tangents.size(), "VEC4"));
        if (mesh.uvs.size() == mesh.positions.size())
            attrs += std::format(",\"TEXCOORD_0\":{}", bin.addAccessor(bin.addView(mesh.uvs.data(), mesh.uvs.size() * sizeof(Vec2), 34962), 5126, mesh.uvs.size(), "VEC2"));
        attrs += std::format(",\"JOINTS_0\":{}", bin.addAccessor(bin.addView(mesh.joints.data(), mesh.joints.size() * 4, 34962), 5121, mesh.joints.size(), "VEC4"));
        attrs += std::format(",\"WEIGHTS_0\":{}", bin.addAccessor(bin.addView(mesh.weights.data(), mesh.weights.size() * 4, 34962), 5121, mesh.weights.size(), "VEC4", true));

        std::string prims;
        for (size_t pi = 0; pi < mesh.primitives.size(); ++pi) {
            const RigPrimitive& prim = mesh.primitives[pi];
            const int idxAcc = bin.addAccessor(bin.addView(mesh.indices.data() + prim.firstIndex, prim.indexCount * sizeof(u32), 34963), 5125, prim.indexCount, "SCALAR");
            if (pi) prims += ',';
            prims += std::format("{{\"attributes\":{{{}}},\"indices\":{},\"material\":{}}}", attrs, idxAcc, prim.material);
        }
        if (mi) meshes += ',';
        meshes += std::format("{{\"name\":\"{}\",\"primitives\":[{}]}}", jsonEscape(mesh.name), prims);
        nodes += std::format(",{{\"name\":\"{}\",\"mesh\":{},\"skin\":0}}", jsonEscape(mesh.name), mi);
        sceneNodes += std::format(",{}", nodeIndex++);
    }
    for (size_t i = 0; i < input.materials.size(); ++i) {
        const RigMaterial& m = input.materials[i];
        if (i) materials += ',';
        materials += std::format("{{\"name\":\"{}\",\"pbrMetallicRoughness\":{{\"baseColorFactor\":[{},{},{},{}],\"metallicFactor\":{},\"roughnessFactor\":{}}}}}", jsonEscape(m.name),
                                 m.baseColor.r, m.baseColor.g, m.baseColor.b, m.baseColor.a, m.metallic, m.roughness);
    }
    std::string jointList;
    for (u32 i = 0; i < kJoints; ++i) jointList += std::format("{}{}", i ? "," : "", i);

    const std::string json = std::format(
        "{{\"asset\":{{\"version\":\"2.0\",\"generator\":\"FOH Asset Forge\"}},\"scene\":0,\"scenes\":[{{\"nodes\":[{}]}}],\"nodes\":[{}],"
        "\"skins\":[{{\"inverseBindMatrices\":{},\"joints\":[{}],\"skeleton\":0}}],\"meshes\":[{}],\"materials\":[{}],\"accessors\":[{}],\"bufferViews\":[{}],"
        "\"buffers\":[{{\"byteLength\":{}}}]}}",
        sceneNodes, nodes, ibmAccessor, jointList, meshes, materials, bin.accessors, bin.views, (bin.data.size() + 3) & ~size_t(3));

    std::string jsonPadded = json;
    while (jsonPadded.size() % 4) jsonPadded += ' ';
    while (bin.data.size() % 4) bin.data.push_back(0);

    std::vector<u8> out;
    auto put32 = [&](u32 v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>((v >> (8 * i)) & 0xFF));
    };
    put32(0x46546C67); // "glTF"
    put32(2);
    put32(static_cast<u32>(12 + 8 + jsonPadded.size() + 8 + bin.data.size()));
    put32(static_cast<u32>(jsonPadded.size()));
    put32(0x4E4F534A); // JSON
    out.insert(out.end(), jsonPadded.begin(), jsonPadded.end());
    put32(static_cast<u32>(bin.data.size()));
    put32(0x004E4942); // BIN
    out.insert(out.end(), bin.data.begin(), bin.data.end());
    return out;
}

} // namespace foh
