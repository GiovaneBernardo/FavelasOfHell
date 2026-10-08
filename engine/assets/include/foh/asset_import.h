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

#include "foh/asset_container.h"
#include "foh/asset_types.h"
#include "foh/math.h"
#include "foh/platform.h"
#include "foh/types.h"

namespace foh {

// Per-source import settings; persisted in the source's .meta file as `setting <key> <value>`.
struct ImportSettings {
    bool generatePrefab = true; // false: meshes/materials/textures only (kit pieces)
    bool generateMips = true;
    u32 maxTextureSize = 0;     // 0 = unlimited; larger images are box-filtered down
    // Standalone textures only. "auto" decides from the file name (_n/_normal, _orm/_rough/_ao/...).
    std::string textureRole = "auto";

    bool operator==(const ImportSettings&) const = default;
};

struct ImportedVertex {
    Vec3 position{0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    Vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
    Vec2 uv0{0.0f};
};

struct ImportedImage {
    std::string key;  // stable within the source (file/uri name, else index)
    std::string name;
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> pixels; // RGBA8, as stored in the file (no color space conversion yet)
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
};

struct ImportedNode {
    std::string name;
    i32 parent = -1; // earlier node or -1
    Vec3 position{0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f};
    i32 mesh = -1;
};

struct ImportedScene {
    std::string name;
    std::vector<ImportedImage> images;
    std::vector<ImportedMaterial> materials;
    std::vector<ImportedMesh> meshes;
    std::vector<ImportedNode> nodes;
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
// image import (no prefab, role from settings or file name).
FOH_API std::vector<CookedAsset> CookScene(const ImportedScene& scene, const ImportSettings& settings,
                                           const SubAssetIdFn& idFor, bool sourceIsTexture);

// ---- texture processing (exposed for tests and the editor) ---------------------------------

// Builds the full mip chain from RGBA8 pixels. Color textures are averaged in linear
// light; other roles average the stored values.
FOH_API std::vector<TextureMip> BuildMipChain(const u8* rgba, u32 width, u32 height, bool srgb);
FOH_API TextureRole GuessTextureRole(const std::string& fileName);

} // namespace foh
