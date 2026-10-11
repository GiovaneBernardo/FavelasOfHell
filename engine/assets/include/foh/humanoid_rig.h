#pragma once

// Automatic humanoid rigging (ADR 0015): an unrigged character model in, a skinned Mixamo-compatible character out.
//
// The result is a glTF binary (.glb) that goes through the normal importer, so the rigged character is an ordinary
// skinned prefab with a skeleton asset, nothing in the engine knows it was made by a tool. The skeleton uses the 25 main
// joints of Mixamo's rig with Mixamo's own rest rotations, so every Mixamo clip already imported (walk, run, idle...)
// animates it without retargeting. Fingers are not rigged: the hand moves as one piece.
//
// How it works:
//   1. LANDMARKS: the 25 joint positions in model space. DefaultHumanoidLandmarks() proposes them from the bounds (the
//      Mixamo template scaled to the character), the artist moves them onto the real joints in the Forge.
//   2. T-POSE: arms that hang below horizontal (an A-pose) are lifted to a T-pose, because the animation clips are
//      authored against a T-pose skeleton. Only the arm chain moves, weighted by the skin weights.
//   3. SKIN WEIGHTS: every vertex is weighted to the bones by its distance to the bone segments, scaled by a per-bone
//      thickness, then diffused along the mesh edges so joints bend smoothly. Vertices at the same position (UV seams,
//      hard edges) are one point: the weights never split along a seam.
//   4. SKELETON: joint positions come from the landmarks, rest rotations from the Mixamo template.
//
// Thread-safety: pure functions over caller memory; BuildHumanoidRig is meant to run on a worker (it takes up to a second
// on a 100k vertex character).

#include <array>
#include <span>
#include <string>
#include <vector>

#include "foh/animation_assets.h"
#include "foh/game_asset.h"
#include "foh/math.h"
#include "foh/platform.h"
#include "foh/types.h"

namespace foh {

struct RigPrimitive {
    u32 firstIndex = 0;
    u32 indexCount = 0;
    u32 material = 0; // index into HumanoidRigInput::materials
};

struct RigMesh {
    std::string name;
    std::vector<Vec3> positions; // model space (node transforms baked in)
    std::vector<Vec3> normals;   // may be empty
    std::vector<Vec4> tangents;  // may be empty
    std::vector<Vec2> uvs;       // may be empty
    std::vector<u32> indices;
    std::vector<RigPrimitive> primitives;
    // Filled by BuildHumanoidRig: four joints and four weights (sum 255) per vertex.
    std::vector<std::array<u8, 4>> joints;
    std::vector<std::array<u8, 4>> weights;
};

struct RigMaterial {
    std::string name;
    Vec4 baseColor{1.0f};
    f32 metallic = 0.0f;
    f32 roughness = 0.8f;
};

struct HumanoidRigInput {
    std::vector<RigMesh> meshes;
    std::vector<RigMaterial> materials;
    std::array<Vec3, kHumanoidJointCount> landmarks{};
    bool normalizeTPose = true;
    f32 smoothness = 0.5f; // 0 rigid .. 1 very soft
};

struct HumanoidRigResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
    // The skeleton in Mixamo's joint order: names, parents, rest TRS, inverse bind. Joint positions are the landmarks
    // after the T-pose lift.
    SkeletonAsset skeleton;
    std::array<Vec3, kHumanoidJointCount> jointPositions{}; // model space, after the lift
    std::array<f32, 2> armLiftDegrees{};                    // left, right: how far the arms were raised
};

// The Mixamo template (Y Bot, model space, meters, T-pose, 1.785 m tall): joint positions and local rest rotations.
FOH_API const std::array<Vec3, kHumanoidJointCount>& HumanoidTemplatePositions();
FOH_API i32 HumanoidJointParent(u32 index); // -1 for the Hips

// Template scaled and placed to fit a character whose bounds are given (feet on boundsMin.y, centred on x/z). With the
// vertices of the model (any subset, model space) the arms follow the geometry: the outermost points on each side show
// whether the arms are out (T-pose), hanging (A-pose) or anywhere in between. Without them the bounding box decides.
FOH_API std::array<Vec3, kHumanoidJointCount> DefaultHumanoidLandmarks(Vec3 boundsMin, Vec3 boundsMax, std::span<const Vec3> points = {});

// Skins the meshes (fills joints/weights, moves vertices when the arms are lifted) and builds the skeleton.
FOH_API bool BuildHumanoidRig(HumanoidRigInput& input, HumanoidRigResult& result);

// A glTF binary with the skinned meshes, the materials' factors and the skeleton. Needs a successful BuildHumanoidRig.
FOH_API std::vector<u8> WriteRiggedGlb(const HumanoidRigInput& input, const HumanoidRigResult& rig);

} // namespace foh
