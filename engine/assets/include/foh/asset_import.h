#pragma once

// Import pipeline (ADR 0004):  parse (glTF | FBX | image)  ->  ImportedScene  ->  cook  ->  sub-assets.
// Only the parse step differs per source format. ImportedScene is already in engine
// conventions (right handed, Y up, meters) and holds decoded pixels, so the cook step
// never touches a source format.
//
// Thread-safety: all functions are pure (no shared state) and may run on any thread.

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "foh/animation_assets.h"
#include "foh/asset_container.h"
#include "foh/asset_types.h"
#include "foh/jobs.h"
#include "foh/math.h"
#include "foh/platform.h"
#include "foh/types.h"

namespace foh {

// Per-source import settings; persisted in the source's .meta file as `setting <key> <value>`.
struct ImportSettings {
    bool generatePrefab = true; // false: meshes/materials/textures only (kit pieces)
    bool generateMips = true;
    // BC7 (color, data) / BC5 (normals); falls back to RGBA8 when a side is not a multiple of 4.
    bool compressTextures = true;
    u32 maxTextureSize = 0;     // 0 = unlimited; larger images are box-filtered down
    // Standalone textures only. "auto" decides from the file name (_n/_normal, _orm/_rough/_ao/...).
    std::string textureRole = "auto";
    // Animation clips (ADR 0009). "auto" compares the first and last pose of the clip.
    std::string clipLoop = "auto";          // auto | loop | once
    std::string clipRootMotion = "auto";    // auto (linear when it loops, full otherwise) | none | linear | full
    std::string clipRootJoint = "auto";     // auto (hips/pelvis/root, else the first joint) | a joint name
    // Key reduction error bounds: a key is dropped when linear interpolation of its neighbours stays inside them.
    f32 clipRotationToleranceDeg = 0.05f;
    // Motion capture takes are minutes long and hold many actions: cut them into clips. Syntax, separated by ';':
    //   name=start-end            seconds on the source take, e.g.  idle=14.0-22.5:loop; fidget=40-46.2
    //   :loop                     closes the loop: the end is moved (up to 0.4 s) to where the pose matches the start best
    // Empty: the whole take is one clip. With segments only the segments are emitted.
    std::string clipSegments;
    // A file that only carries animation (Mixamo "without skin", mocap packs): emit the clips only, no meshes,
    // materials, textures, skeleton or prefab.
    bool animationOnly = false;
    f32 clipTranslationTolerance = 0.0005f; // meters

    bool operator==(const ImportSettings&) const = default;
};

struct ImportedVertex {
    Vec3 position{0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    Vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
    Vec2 uv0{0.0f};
    // Skinning (ImportedMesh::skeleton >= 0): indices into the skeleton's joints, weights sum to 255.
    u8 joints[4] = {0, 0, 0, 0};
    u8 weights[4] = {0, 0, 0, 0};
};

// Images stay encoded until the cook step: decoding, mip generation and compression of every
// texture then run in parallel instead of one after another while the file is parsed.
struct ImportedImage {
    std::string key;  // stable within the source (file/uri name, else index)
    std::string name;
    std::vector<u8> encoded; // png/jpg/tga/bmp bytes, decoded by the cook step
};

struct ImportedTextureRef {
    i32 image = -1;
    u8 uvSet = 0;
};

struct ImportedMaterial {
    std::string key;
    std::string name;
    Vec4 baseColor{1.0f};
    f32 metallic = 0.0f;
    f32 roughness = 1.0f;
    Vec3 emissive{0.0f};
    AlphaMode alphaMode = AlphaMode::Opaque;
    f32 alphaCutoff = 0.5f;
    bool doubleSided = false;
    f32 normalScale = 1.0f;
    f32 occlusionStrength = 1.0f;
    ImportedTextureRef baseColorTexture, normalTexture, metallicRoughnessTexture, occlusionTexture, emissiveTexture;
};

struct ImportedSubMesh {
    u32 firstIndex = 0;
    u32 indexCount = 0;
    i32 material = -1; // index into ImportedScene::materials, -1 = default material
};

struct ImportedMesh {
    std::string key;
    std::string name;
    std::vector<ImportedVertex> vertices;
    std::vector<u32> indices;
    std::vector<ImportedSubMesh> subMeshes;
    i32 skeleton = -1; // index into ImportedScene::skeletons, -1 = static mesh
};

struct ImportedNode {
    std::string name;
    i32 parent = -1; // earlier node or -1
    Vec3 position{0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f};
    i32 mesh = -1;
};

// A joint hierarchy. Parents precede children. Names are the source's (cook normalises them).
struct ImportedSkeleton {
    std::string key;
    std::string name;
    std::vector<std::string> jointNames;
    std::vector<i32> parents;
    std::vector<Vec3> restPosition;
    std::vector<Quat> restRotation;
    std::vector<Vec3> restScale;
    std::vector<Mat4> inverseBind;      // model space -> joint space; meaningful where hasInverseBind is set
    std::vector<u8> hasInverseBind;     // joints no skin binds get the inverse of their rest pose
    bool usedBySkin = false;            // some mesh is skinned to it: only then is a Skeleton asset emitted
};

// Per-joint local transforms of a clip, sampled at `frameRate` for every frame. An empty vector means the joint
// is not animated by the clip on that channel (it keeps its rest value).
struct ImportedClipTrack {
    std::vector<Vec3> translation;
    std::vector<Quat> rotation;
    std::vector<Vec3> scale;
};

struct ImportedClip {
    std::string key;
    std::string name;
    i32 skeleton = -1; // the joint hierarchy the tracks belong to (tracks[i] is skeleton joint i)
    f32 frameRate = 30.0f;
    u32 frameCount = 0;
    bool loop = false; // the author says it loops (a segment cut with :loop): never decided by the pose comparison
    std::vector<ImportedClipTrack> tracks;
};

struct ImportedScene {
    std::string name;
    std::vector<ImportedImage> images;
    std::vector<ImportedMaterial> materials;
    std::vector<ImportedMesh> meshes;
    std::vector<ImportedNode> nodes;
    std::vector<ImportedSkeleton> skeletons;
    std::vector<ImportedClip> clips;
    // Files read besides the main one (.bin, textures): they take part in change detection and copying.
    std::vector<std::filesystem::path> externalFiles;
    std::vector<std::string> warnings;
};

enum class SourceFormat { Unknown, Gltf, Fbx, Image };
FOH_API SourceFormat DetectSourceFormat(const std::filesystem::path& path);

FOH_API bool ImportGltf(const std::filesystem::path& path, ImportedScene& out, std::string& error);
FOH_API bool ImportFbx(const std::filesystem::path& path, ImportedScene& out, std::string& error);
FOH_API bool ImportImageFile(const std::filesystem::path& path, ImportedScene& out, std::string& error);

// One cooked sub-asset, ready to be written.
struct CookedAsset {
    std::string key; // stable key within the source (see AssetDb)
    std::string name;
    AssetKind kind = AssetKind::None;
    AssetId guid;
    std::vector<u8> bytes;
    std::vector<AssetId> dependencies;
};

// Maps (kind, key) to the sub-asset's GUID. The asset database supplies it so existing
// keys keep their id across reimports.
using SubAssetIdFn = std::function<AssetId(AssetKind kind, const std::string& key)>;

// Turns an ImportedScene into container files. `sourceIsTexture` marks a standalone
// image import (no prefab, role from settings or file name). With a job system the heavy parts
// (image decode, mips, block compression) run in parallel; the result does not depend on it.
// Problems that do not fail the import (an undecodable image) are appended to `warnings`.
FOH_API std::vector<CookedAsset> CookScene(const ImportedScene& scene, const ImportSettings& settings,
                                           const SubAssetIdFn& idFor, bool sourceIsTexture, JobSystem* jobs = nullptr,
                                           std::vector<std::string>* warnings = nullptr);

// ---- animation cooking (exposed for tests) -------------------------------------------------

// Resolves a skeleton for the cook: normalised names, inverse bind matrices for joints without a skin.
FOH_API SkeletonAsset CookSkeleton(const ImportedSkeleton& source);

// Clip -> asset: name normalisation, loop detection, root motion split (hips planar motion and yaw out of the pose),
// foot-plant detection for phase sync, key reduction. `warnings` receives problems that do not fail the cook.
FOH_API ClipAsset CookClip(const ImportedClip& clip, const ImportedSkeleton& skeleton, const ImportSettings& settings,
                           std::vector<std::string>* warnings = nullptr);

struct ClipSegment {
    std::string name;
    f32 start = 0.0f, end = 0.0f; // seconds on the source take
    bool loop = false;
};
// Parses ImportSettings::clipSegments. Malformed items are skipped and described in `errors`.
FOH_API std::vector<ClipSegment> ParseClipSegments(const std::string& text, std::vector<std::string>* errors = nullptr);

// Cuts each take of `scene` into the segments (all of them from the longest take). Returns the clips unchanged when the
// settings define no segments. Exposed for tests.
FOH_API std::vector<ImportedClip> SliceClips(const std::vector<ImportedClip>& clips, const ImportSettings& settings,
                                             std::vector<std::string>* warnings = nullptr);

// Removes keys that linear interpolation of their neighbours reproduces within the tolerance. Exposed for tests.
FOH_API void ReduceQuatChannel(std::vector<Quat>& samples, f32 toleranceRadians, QuatChannel& out);
FOH_API void ReduceVecChannel(std::vector<Vec3>& samples, f32 tolerance, VecChannel& out);

// ---- texture processing (exposed for tests and the editor) ---------------------------------

// Builds the full mip chain from RGBA8 pixels. Color textures are averaged in linear
// light; other roles average the stored values.
FOH_API std::vector<TextureMip> BuildMipChain(const u8* rgba, u32 width, u32 height, bool srgb, JobSystem* jobs = nullptr);

// Cooks one decoded RGBA8 image into a texture asset: size cap, mip chain (normal maps are
// renormalized per level), then BC7 (color: perceptual weights, data: linear) / BC5 (normals).
// Deterministic whatever the job system does. `warning` receives a note when it had to fall back to RGBA8.
FOH_API bool CookTexture(const u8* rgba, u32 width, u32 height, const std::string& name, TextureRole role,
                         const ImportSettings& settings, JobSystem* jobs, TextureAsset& out, std::string* warning = nullptr);
FOH_API TextureRole GuessTextureRole(const std::string& fileName);

} // namespace foh
