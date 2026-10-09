#pragma once

// CPU-side forms of the engine asset types and their container (de)serialization
// (ADR 0004). Every asset is one container file with its own GUID; assets refer to
// each other only by AssetId. Runtime code never sees source files.
//
// Chunk layout per type (tags are FourCC):
//   Mesh      META (name, bounds, submeshes, stream descriptor, skeleton)  VERT (interleaved)  INDX (u32)  DEPS
//   Material  META (factors, flags, texture references)                                          DEPS
//   Texture   META (size, role, format, mip count)  MIP0..MIPn (one chunk per mip, streamable)  DEPS
//   Prefab    META (name, nodes with mesh + per-slot material references, skeleton)              DEPS
//   Variant   META (base prefab, overrides)                                                      DEPS
//   Skeleton, AnimationClip: see animation_assets.h
//
// Thread-safety: plain data; (de)serialization functions are pure.

#include <string>
#include <vector>

#include "foh/asset_container.h"
#include "foh/math.h"
#include "foh/platform.h"
#include "foh/types.h"

namespace foh {

enum class AssetKind : u32 {
    None = 0,
    Mesh = FourCC('M', 'E', 'S', 'H'),
    Material = FourCC('M', 'A', 'T', 'L'),
    Texture = FourCC('T', 'E', 'X', 'T'),
    Prefab = FourCC('P', 'R', 'F', 'B'),
    PrefabVariant = FourCC('P', 'R', 'V', 'R'),
    Skeleton = FourCC('S', 'K', 'E', 'L'),
    AnimationClip = FourCC('A', 'C', 'L', 'P'),
};

FOH_API const char* AssetKindName(AssetKind kind);        // "mesh", "material", ...
FOH_API AssetKind AssetKindFromName(std::string_view name); // None when unknown
FOH_API const char* AssetKindExtension(AssetKind kind);   // cooked file extension, no dot

// ---- mesh ----------------------------------------------------------------------------------

// Vertex data is described, not hard-coded: readers look streams up by semantic, so
// new attributes and reordering never break old files and unknown streams are skipped.
enum class VertexSemantic : u8 { Position, Normal, Tangent, Uv0, Uv1, Color0, Joints0, Weights0 };
// U8x4 / Unorm8x4 are the skinning streams: four joint indices and four weights (sum 255) per vertex.
enum class VertexFormat : u8 { Float2, Float3, Float4, U8x4, Unorm8x4 };

inline constexpr u32 VertexFormatSize(VertexFormat f) {
    return f == VertexFormat::Float2 ? 8u : f == VertexFormat::Float3 ? 12u : f == VertexFormat::Float4 ? 16u : 4u;
}

struct VertexStream {
    VertexSemantic semantic = VertexSemantic::Position;
    VertexFormat format = VertexFormat::Float3;
    u32 offset = 0; // bytes from the start of a vertex
};

// A draw range inside the mesh's index buffer with one material slot. All primitives
// of a source mesh share one vertex/index buffer pair (indices already include the
// vertex offset).
struct SubMesh {
    u32 firstIndex = 0;
    u32 indexCount = 0;
    u32 materialSlot = 0;
    Vec3 boundsMin{0.0f};
    Vec3 boundsMax{0.0f};
};

struct MeshAsset {
    std::string name;
    std::vector<VertexStream> streams;
    u32 vertexStride = 0;
    std::vector<u8> vertexData; // interleaved, vertexCount() * vertexStride bytes
    std::vector<u32> indices;
    std::vector<SubMesh> subMeshes;
    u32 materialSlotCount = 0;
    Vec3 boundsMin{0.0f};
    Vec3 boundsMax{0.0f};
    // Valid for a skinned mesh (Joints0/Weights0 streams): the joints index THIS skeleton's joint list, and the
    // vertices are in the skeleton's bind pose space (skinning matrix = jointModelPose * skeleton.inverseBind).
    AssetId skeleton;

    bool skinned() const { return skeleton.isValid(); }
    u32 vertexCount() const { return vertexStride ? static_cast<u32>(vertexData.size() / vertexStride) : 0; }
    const VertexStream* findStream(VertexSemantic s) const {
        for (const VertexStream& st : streams)
            if (st.semantic == s) return &st;
        return nullptr;
    }
};

// The layout the importer cooks (and the one the renderer currently consumes).
struct CookedVertex {
    Vec3 position;
    Vec3 normal;
    Vec4 tangent; // xyz + handedness in w
    Vec2 uv0;
};
static_assert(sizeof(CookedVertex) == 48);

// Skinned meshes append the influences: four joint indices and four weights that sum to 255.
struct CookedSkinnedVertex {
    CookedVertex base;
    u8 joints[4];
    u8 weights[4];
};
static_assert(sizeof(CookedSkinnedVertex) == 56);

// ---- material ------------------------------------------------------------------------------

struct TextureRef {
    AssetId texture;
    u8 uvSet = 0;
    bool isValid() const { return texture.isValid(); }
};

enum class AlphaMode : u8 { Opaque, Mask, Blend };

struct MaterialAsset {
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
    TextureRef baseColorTexture;
    TextureRef normalTexture;
    TextureRef metallicRoughnessTexture; // glTF packing: G = roughness, B = metallic
    TextureRef occlusionTexture;         // often the same texture as the one above (R)
    TextureRef emissiveTexture;
};

// ---- texture -------------------------------------------------------------------------------

// How the texels are meant to be read; decides the GPU format and mip filtering.
enum class TextureRole : u8 { Color, Normal, Data };

// Bc* entries are reserved for the compressed cook (ADR 0004 follow-up): the
// container and descriptor already support them, the importer does not produce them yet.
enum class TexelFormat : u8 { Rgba8Unorm, Rgba8Srgb, Bc7Unorm, Bc7Srgb, Bc5Unorm, Bc4Unorm };

struct TextureMip {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> data;
};

struct TextureAsset {
    std::string name;
    TextureRole role = TextureRole::Color;
    TexelFormat format = TexelFormat::Rgba8Srgb;
    u32 width = 0;
    u32 height = 0;
    std::vector<TextureMip> mips; // mips[0] is the full resolution
};

// ---- prefab --------------------------------------------------------------------------------

struct PrefabNode {
    std::string name; // unique among siblings (the importer guarantees it)
    i32 parent = -1;  // index of an EARLIER node, or -1
    Vec3 position{0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f};
    AssetId mesh;                  // invalid: transform-only node
    std::vector<AssetId> materials; // one per material slot of the mesh
    // Material slot of each submesh of the mesh. Copied here so that placing a prefab needs no mesh file read:
    // the instance can be created at once while the mesh data streams in.
    std::vector<u32> submeshSlots;
    bool skinned = false; // the mesh is skinned to the prefab's skeleton: it gets a SkinnedMesh and is posed by the root
};

struct PrefabAsset {
    std::string name;
    std::vector<PrefabNode> nodes; // parents always precede their children
    // A skinned character: bones are not nodes, the skeleton is referenced once and the instance root gets the
    // animator (the skinned mesh nodes below it read its pose).
    AssetId skeleton;

    // "Root/Child/Leaf": how variants address nodes.
    FOH_API std::string nodePath(u32 index) const;
    FOH_API i32 findNode(std::string_view path) const; // -1 when missing
};

// A variant stores only differences from a base prefab, addressed by node path.
struct PrefabOverride {
    enum Flags : u32 { SetMaterial = 1, SetTransform = 2, Remove = 4 };
    std::string nodePath;
    u32 flags = 0;
    u32 materialSlot = 0; // SetMaterial
    AssetId material;     // SetMaterial
    Vec3 position{0.0f};  // SetTransform
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f};
};

struct PrefabVariantAsset {
    std::string name;
    AssetId base;
    std::vector<PrefabOverride> overrides;
};

struct VariantApplyResult {
    PrefabAsset prefab;
    // Overrides whose target node (or material slot) no longer exists in the base.
    // They are reported, never dropped silently.
    std::vector<std::string> orphaned;
};

FOH_API VariantApplyResult ApplyVariant(const PrefabAsset& base, const PrefabVariantAsset& variant);

// ---- serialization -------------------------------------------------------------------------
// Serialize* returns a complete container file. Deserialize* validates everything it
// reads and returns false (with a reason in `error`) instead of trusting the file.

FOH_API std::vector<u8> SerializeMesh(const MeshAsset& mesh, AssetId id);
FOH_API bool DeserializeMesh(const ContainerReader& reader, MeshAsset& out, std::string* error = nullptr);

FOH_API std::vector<u8> SerializeMaterial(const MaterialAsset& material, AssetId id);
FOH_API bool DeserializeMaterial(const ContainerReader& reader, MaterialAsset& out, std::string* error = nullptr);

FOH_API std::vector<u8> SerializeTexture(const TextureAsset& texture, AssetId id);
// `firstMip` skips the largest mips (texture streaming / quality settings): the result
// then starts at that mip and `width`/`height` describe it.
FOH_API bool DeserializeTexture(const ContainerReader& reader, TextureAsset& out, u32 firstMip = 0,
                                std::string* error = nullptr);

FOH_API std::vector<u8> SerializePrefab(const PrefabAsset& prefab, AssetId id);
FOH_API bool DeserializePrefab(const ContainerReader& reader, PrefabAsset& out, std::string* error = nullptr);

FOH_API std::vector<u8> SerializePrefabVariant(const PrefabVariantAsset& variant, AssetId id);
FOH_API bool DeserializePrefabVariant(const ContainerReader& reader, PrefabVariantAsset& out,
                                      std::string* error = nullptr);

// Size and format of a texture without reading any mip data (to decide how much of it to load).
struct TextureInfo {
    u32 width = 0;
    u32 height = 0;
    u32 mipCount = 0;
    TexelFormat format = TexelFormat::Rgba8Unorm;
    TextureRole role = TextureRole::Color;
};
FOH_API bool ReadTextureInfo(const ContainerReader& reader, TextureInfo& out);

// The assets a container references (read from its DEPS chunk; empty when absent).
FOH_API std::vector<AssetId> ReadDependencies(const ContainerReader& reader);

} // namespace foh
