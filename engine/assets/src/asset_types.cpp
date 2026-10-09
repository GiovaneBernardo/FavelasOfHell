#include "foh/asset_types.h"

#include <algorithm>
#include <set>

#include "foh/byte_stream.h"

namespace foh {

namespace {

constexpr u32 kTagMeta = FourCC('M', 'E', 'T', 'A');
constexpr u32 kTagVert = FourCC('V', 'E', 'R', 'T');
constexpr u32 kTagIndx = FourCC('I', 'N', 'D', 'X');
constexpr u32 kTagDeps = FourCC('D', 'E', 'P', 'S');
constexpr u32 kMaxMips = 24;

constexpr u32 kMeshVersion = 2; // 2: skeleton reference (skinned meshes)
constexpr u32 kMaterialVersion = 1;
constexpr u32 kTextureVersion = 1;
constexpr u32 kPrefabVersion = 4; // 2: nodes carry submeshSlots, 3: skeleton reference, 4: node.skinned
constexpr u32 kVariantVersion = 1;

u32 mipTag(u32 level) { return FourCC('M', 'I', 'P', static_cast<char>(level)); }

bool setError(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

bool checkHeader(const ContainerReader& reader, AssetKind kind, u32 currentVersion, std::string* error) {
    if (reader.typeId() != static_cast<u32>(kind)) return setError(error, "container holds a different asset type");
    // Older versions get a migration branch in the Deserialize function; newer ones cannot be read.
    if (reader.typeVersion() == 0 || reader.typeVersion() > currentVersion)
        return setError(error, "unsupported asset type version");
    return true;
}

bool readMeta(const ContainerReader& reader, std::vector<u8>& meta, std::string* error) {
    if (!reader.readChunk(kTagMeta, meta)) return setError(error, "missing or corrupt META chunk");
    return true;
}

std::vector<u8> deps(const std::vector<AssetId>& ids) {
    // Sorted and unique, so the chunk (and the container hash) is stable.
    std::set<AssetId> unique;
    for (const AssetId& id : ids)
        if (id.isValid()) unique.insert(id);
    ByteWriter w;
    w.put<u32>(static_cast<u32>(unique.size()));
    for (const AssetId& id : unique) w.putId(id);
    return w.take();
}

u32 texelBytes(TexelFormat f, u32 w, u32 h) {
    auto blocks = [](u32 v) { return (v + 3) / 4; };
    switch (f) {
        case TexelFormat::Rgba8Unorm:
        case TexelFormat::Rgba8Srgb: return w * h * 4;
        case TexelFormat::Bc7Unorm:
        case TexelFormat::Bc7Srgb:
        case TexelFormat::Bc5Unorm: return blocks(w) * blocks(h) * 16;
        case TexelFormat::Bc4Unorm: return blocks(w) * blocks(h) * 8;
    }
    return 0;
}

void writeTextureRef(ByteWriter& w, const TextureRef& r) {
    w.putId(r.texture);
    w.put<u8>(r.uvSet);
}
TextureRef readTextureRef(ByteReader& r) {
    TextureRef t;
    t.texture = r.getId();
    t.uvSet = r.get<u8>();
    return t;
}

} // namespace

const char* AssetKindName(AssetKind kind) {
    switch (kind) {
        case AssetKind::Mesh: return "mesh";
        case AssetKind::Material: return "material";
        case AssetKind::Texture: return "texture";
        case AssetKind::Prefab: return "prefab";
        case AssetKind::PrefabVariant: return "variant";
        case AssetKind::Skeleton: return "skeleton";
        case AssetKind::AnimationClip: return "clip";
        case AssetKind::None: break;
    }
    return "none";
}

AssetKind AssetKindFromName(std::string_view name) {
    for (AssetKind k : {AssetKind::Mesh, AssetKind::Material, AssetKind::Texture, AssetKind::Prefab,
                        AssetKind::PrefabVariant, AssetKind::Skeleton, AssetKind::AnimationClip})
        if (name == AssetKindName(k)) return k;
    return AssetKind::None;
}

const char* AssetKindExtension(AssetKind kind) {
    switch (kind) {
        case AssetKind::Mesh: return "fmesh";
        case AssetKind::Material: return "fmat";
        case AssetKind::Texture: return "ftex";
        case AssetKind::Prefab: return "fprefab";
        case AssetKind::PrefabVariant: return "fvariant";
        case AssetKind::Skeleton: return "fskel";
        case AssetKind::AnimationClip: return "fclip";
        case AssetKind::None: break;
    }
    return "fasset";
}

// ---- mesh ----------------------------------------------------------------------------------

std::vector<u8> SerializeMesh(const MeshAsset& m, AssetId id) {
    ByteWriter w;
    w.putString(m.name);
    w.put<u32>(m.materialSlotCount);
    w.putVec3(m.boundsMin);
    w.putVec3(m.boundsMax);
    w.put<u32>(m.vertexStride);
    w.put<u32>(m.vertexCount());
    w.put<u32>(static_cast<u32>(m.indices.size()));
    w.put<u32>(static_cast<u32>(m.streams.size()));
    for (const VertexStream& s : m.streams) {
        w.put<u8>(static_cast<u8>(s.semantic));
        w.put<u8>(static_cast<u8>(s.format));
        w.put<u32>(s.offset);
    }
    w.put<u32>(static_cast<u32>(m.subMeshes.size()));
    for (const SubMesh& s : m.subMeshes) {
        w.put<u32>(s.firstIndex);
        w.put<u32>(s.indexCount);
        w.put<u32>(s.materialSlot);
        w.putVec3(s.boundsMin);
        w.putVec3(s.boundsMax);
    }
    w.putId(m.skeleton);

    ContainerWriter c(static_cast<u32>(AssetKind::Mesh), kMeshVersion, id);
    c.addChunk(kTagMeta, w.bytes());
    c.addChunk(kTagVert, m.vertexData, ChunkCodec::Lz4);
    c.addChunk(kTagIndx, std::span<const u8>(reinterpret_cast<const u8*>(m.indices.data()), m.indices.size() * 4),
               ChunkCodec::Lz4);
    c.addChunk(kTagDeps, deps({m.skeleton}));
    return c.finish();
}

bool DeserializeMesh(const ContainerReader& reader, MeshAsset& out, std::string* error) {
    if (!checkHeader(reader, AssetKind::Mesh, kMeshVersion, error)) return false;
    std::vector<u8> meta, vert, indx;
    if (!readMeta(reader, meta, error)) return false;
    if (!reader.readChunk(kTagVert, vert) || !reader.readChunk(kTagIndx, indx))
        return setError(error, "missing or corrupt mesh data chunk");

    ByteReader r(meta);
    MeshAsset m;
    m.name = r.getString();
    m.materialSlotCount = r.get<u32>();
    m.boundsMin = r.getVec3();
    m.boundsMax = r.getVec3();
    m.vertexStride = r.get<u32>();
    const u32 vertexCount = r.get<u32>();
    const u32 indexCount = r.get<u32>();
    const u32 streamCount = r.getCount(6);
    for (u32 i = 0; i < streamCount; ++i) {
        VertexStream s;
        s.semantic = static_cast<VertexSemantic>(r.get<u8>());
        s.format = static_cast<VertexFormat>(r.get<u8>());
        s.offset = r.get<u32>();
        if (s.format > VertexFormat::Unorm8x4) return setError(error, "unknown vertex format");
        m.streams.push_back(s);
    }
    const u32 subCount = r.getCount(36);
    for (u32 i = 0; i < subCount; ++i) {
        SubMesh s;
        s.firstIndex = r.get<u32>();
        s.indexCount = r.get<u32>();
        s.materialSlot = r.get<u32>();
        s.boundsMin = r.getVec3();
        s.boundsMax = r.getVec3();
        m.subMeshes.push_back(s);
    }
    if (reader.typeVersion() >= 2) m.skeleton = r.getId();
    if (!r.ok()) return setError(error, "truncated mesh metadata");

    if (m.vertexStride == 0 || m.vertexStride > 256) return setError(error, "bad vertex stride");
    if (vert.size() != static_cast<u64>(vertexCount) * m.vertexStride) return setError(error, "vertex data size mismatch");
    if (indx.size() != static_cast<u64>(indexCount) * 4) return setError(error, "index data size mismatch");
    for (const VertexStream& s : m.streams)
        if (static_cast<u64>(s.offset) + VertexFormatSize(s.format) > m.vertexStride)
            return setError(error, "vertex stream outside the vertex");
    if (m.skinned()) {
        const VertexStream* joints = m.findStream(VertexSemantic::Joints0);
        const VertexStream* weights = m.findStream(VertexSemantic::Weights0);
        if (!joints || joints->format != VertexFormat::U8x4 || !weights || weights->format != VertexFormat::Unorm8x4)
            return setError(error, "skinned mesh without joint and weight streams");
    }
    for (const SubMesh& s : m.subMeshes) {
        if (s.firstIndex > indexCount || s.indexCount > indexCount - s.firstIndex)
            return setError(error, "submesh outside the index buffer");
        if (s.materialSlot >= std::max(m.materialSlotCount, 1u)) return setError(error, "submesh material slot out of range");
    }
    m.vertexData = std::move(vert);
    m.indices.resize(indexCount);
    if (indexCount) std::memcpy(m.indices.data(), indx.data(), indx.size());
    for (u32 index : m.indices)
        if (index >= vertexCount) return setError(error, "index out of range");
    out = std::move(m);
    return true;
}

// ---- material ------------------------------------------------------------------------------

std::vector<u8> SerializeMaterial(const MaterialAsset& m, AssetId id) {
    ByteWriter w;
    w.putString(m.name);
    w.putVec4(m.baseColor);
    w.put(m.metallic);
    w.put(m.roughness);
    w.putVec3(m.emissive);
    w.put<u8>(static_cast<u8>(m.alphaMode));
    w.put(m.alphaCutoff);
    w.put<u8>(m.doubleSided ? 1 : 0);
    w.put(m.normalScale);
    w.put(m.occlusionStrength);
    writeTextureRef(w, m.baseColorTexture);
    writeTextureRef(w, m.normalTexture);
    writeTextureRef(w, m.metallicRoughnessTexture);
    writeTextureRef(w, m.occlusionTexture);
    writeTextureRef(w, m.emissiveTexture);

    ContainerWriter c(static_cast<u32>(AssetKind::Material), kMaterialVersion, id);
    c.addChunk(kTagMeta, w.bytes());
    c.addChunk(kTagDeps, deps({m.baseColorTexture.texture, m.normalTexture.texture, m.metallicRoughnessTexture.texture,
                               m.occlusionTexture.texture, m.emissiveTexture.texture}));
    return c.finish();
}

bool DeserializeMaterial(const ContainerReader& reader, MaterialAsset& out, std::string* error) {
    if (!checkHeader(reader, AssetKind::Material, kMaterialVersion, error)) return false;
    std::vector<u8> meta;
    if (!readMeta(reader, meta, error)) return false;
    ByteReader r(meta);
    MaterialAsset m;
    m.name = r.getString();
    m.baseColor = r.getVec4();
    m.metallic = r.get<f32>();
    m.roughness = r.get<f32>();
    m.emissive = r.getVec3();
    const u8 alpha = r.get<u8>();
    m.alphaCutoff = r.get<f32>();
    m.doubleSided = r.get<u8>() != 0;
    m.normalScale = r.get<f32>();
    m.occlusionStrength = r.get<f32>();
    m.baseColorTexture = readTextureRef(r);
    m.normalTexture = readTextureRef(r);
    m.metallicRoughnessTexture = readTextureRef(r);
    m.occlusionTexture = readTextureRef(r);
    m.emissiveTexture = readTextureRef(r);
    if (!r.ok()) return setError(error, "truncated material metadata");
    if (alpha > static_cast<u8>(AlphaMode::Blend)) return setError(error, "unknown alpha mode");
    m.alphaMode = static_cast<AlphaMode>(alpha);
    out = std::move(m);
    return true;
}

// ---- texture -------------------------------------------------------------------------------

std::vector<u8> SerializeTexture(const TextureAsset& t, AssetId id) {
    ByteWriter w;
    w.putString(t.name);
    w.put<u8>(static_cast<u8>(t.role));
    w.put<u8>(static_cast<u8>(t.format));
    w.put<u32>(t.width);
    w.put<u32>(t.height);
    w.put<u32>(static_cast<u32>(t.mips.size()));

    ContainerWriter c(static_cast<u32>(AssetKind::Texture), kTextureVersion, id);
    c.addChunk(kTagMeta, w.bytes());
    // Block compressed data is already dense (LZ4 gains a few percent at best and costs a full extra copy of
    // every mip); only raw RGBA8 mips are worth compressing.
    const bool block = t.format != TexelFormat::Rgba8Unorm && t.format != TexelFormat::Rgba8Srgb;
    for (size_t i = 0; i < t.mips.size() && i < kMaxMips; ++i)
        c.addChunk(mipTag(static_cast<u32>(i)), t.mips[i].data, block ? ChunkCodec::None : ChunkCodec::Lz4);
    c.addChunk(kTagDeps, deps({}));
    return c.finish();
}

bool DeserializeTexture(const ContainerReader& reader, TextureAsset& out, u32 firstMip, std::string* error) {
    if (!checkHeader(reader, AssetKind::Texture, kTextureVersion, error)) return false;
    std::vector<u8> meta;
    if (!readMeta(reader, meta, error)) return false;
    ByteReader r(meta);
    TextureAsset t;
    t.name = r.getString();
    const u8 role = r.get<u8>();
    const u8 format = r.get<u8>();
    t.width = r.get<u32>();
    t.height = r.get<u32>();
    const u32 mipCount = r.get<u32>();
    if (!r.ok()) return setError(error, "truncated texture metadata");
    if (role > static_cast<u8>(TextureRole::Data) || format > static_cast<u8>(TexelFormat::Bc4Unorm))
        return setError(error, "unknown texture role or format");
    t.role = static_cast<TextureRole>(role);
    t.format = static_cast<TexelFormat>(format);
    if (t.width == 0 || t.height == 0 || t.width > 1u << 15 || t.height > 1u << 15) return setError(error, "bad texture size");
    if (mipCount == 0 || mipCount > kMaxMips) return setError(error, "bad mip count");
    firstMip = std::min(firstMip, mipCount - 1);

    u32 w = t.width, h = t.height;
    for (u32 i = 0; i < mipCount; ++i) {
        if (i >= firstMip) {
            TextureMip mip;
            mip.width = w;
            mip.height = h;
            if (!reader.readChunk(mipTag(i), mip.data)) return setError(error, "missing or corrupt mip chunk");
            if (mip.data.size() != texelBytes(t.format, w, h)) return setError(error, "mip size mismatch");
            t.mips.push_back(std::move(mip));
        }
        w = std::max(w / 2, 1u);
        h = std::max(h / 2, 1u);
    }
    t.width = t.mips.front().width;
    t.height = t.mips.front().height;
    out = std::move(t);
    return true;
}

bool ReadTextureInfo(const ContainerReader& reader, TextureInfo& out) {
    if (!checkHeader(reader, AssetKind::Texture, kTextureVersion, nullptr)) return false;
    std::vector<u8> meta;
    if (!readMeta(reader, meta, nullptr)) return false;
    ByteReader r(meta);
    r.getString();
    const u8 role = r.get<u8>();
    const u8 format = r.get<u8>();
    TextureInfo info;
    info.width = r.get<u32>();
    info.height = r.get<u32>();
    info.mipCount = r.get<u32>();
    if (!r.ok() || role > static_cast<u8>(TextureRole::Data) || format > static_cast<u8>(TexelFormat::Bc4Unorm)) return false;
    info.role = static_cast<TextureRole>(role);
    info.format = static_cast<TexelFormat>(format);
    out = info;
    return true;
}

// ---- prefab --------------------------------------------------------------------------------

std::string PrefabAsset::nodePath(u32 index) const {
    std::vector<const std::string*> parts;
    i32 cursor = static_cast<i32>(index);
    // `parent < index` is enforced at load, so this terminates; the bound guards hand-built data.
    for (size_t guard = 0; cursor >= 0 && static_cast<size_t>(cursor) < nodes.size() && guard <= nodes.size(); ++guard) {
        parts.push_back(&nodes[static_cast<size_t>(cursor)].name);
        cursor = nodes[static_cast<size_t>(cursor)].parent;
    }
    std::string path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!path.empty()) path += '/';
        path += **it;
    }
    return path;
}

i32 PrefabAsset::findNode(std::string_view path) const {
    for (u32 i = 0; i < nodes.size(); ++i)
        if (nodePath(i) == path) return static_cast<i32>(i);
    return -1;
}

std::vector<u8> SerializePrefab(const PrefabAsset& p, AssetId id) {
    ByteWriter w;
    w.putString(p.name);
    w.put<u32>(static_cast<u32>(p.nodes.size()));
    std::vector<AssetId> refs;
    for (const PrefabNode& n : p.nodes) {
        w.putString(n.name);
        w.put<i32>(n.parent);
        w.putVec3(n.position);
        w.putQuat(n.rotation);
        w.putVec3(n.scale);
        w.putId(n.mesh);
        w.put<u32>(static_cast<u32>(n.materials.size()));
        for (const AssetId& m : n.materials) w.putId(m);
        w.put<u32>(static_cast<u32>(n.submeshSlots.size()));
        for (u32 slot : n.submeshSlots) w.put<u32>(slot);
        w.put<u8>(n.skinned ? 1 : 0);
        refs.push_back(n.mesh);
        refs.insert(refs.end(), n.materials.begin(), n.materials.end());
    }
    w.putId(p.skeleton);
    refs.push_back(p.skeleton);
    ContainerWriter c(static_cast<u32>(AssetKind::Prefab), kPrefabVersion, id);
    c.addChunk(kTagMeta, w.bytes(), ChunkCodec::Lz4);
    c.addChunk(kTagDeps, deps(refs));
    return c.finish();
}

bool DeserializePrefab(const ContainerReader& reader, PrefabAsset& out, std::string* error) {
    if (!checkHeader(reader, AssetKind::Prefab, kPrefabVersion, error)) return false;
    std::vector<u8> meta;
    if (!readMeta(reader, meta, error)) return false;
    ByteReader r(meta);
    PrefabAsset p;
    p.name = r.getString();
    const u32 count = r.getCount(4 + 4 + 12 + 16 + 12 + 16 + 4);
    for (u32 i = 0; i < count; ++i) {
        PrefabNode n;
        n.name = r.getString();
        n.parent = r.get<i32>();
        n.position = r.getVec3();
        n.rotation = r.getQuat();
        n.scale = r.getVec3();
        n.mesh = r.getId();
        const u32 slots = r.getCount(16);
        for (u32 s = 0; s < slots; ++s) n.materials.push_back(r.getId());
        if (reader.typeVersion() >= 2) {
            const u32 subs = r.getCount(4);
            for (u32 s = 0; s < subs; ++s) n.submeshSlots.push_back(r.get<u32>());
        }
        if (reader.typeVersion() >= 4) n.skinned = r.get<u8>() != 0;
        if (!r.ok()) break;
        if (n.parent >= static_cast<i32>(i) || n.parent < -1) return setError(error, "prefab node parent out of order");
        p.nodes.push_back(std::move(n));
    }
    if (reader.typeVersion() >= 3) p.skeleton = r.getId();
    if (!r.ok()) return setError(error, "truncated prefab metadata");
    out = std::move(p);
    return true;
}

// ---- prefab variant ------------------------------------------------------------------------

std::vector<u8> SerializePrefabVariant(const PrefabVariantAsset& v, AssetId id) {
    ByteWriter w;
    w.putString(v.name);
    w.putId(v.base);
    w.put<u32>(static_cast<u32>(v.overrides.size()));
    std::vector<AssetId> refs{v.base};
    for (const PrefabOverride& o : v.overrides) {
        w.putString(o.nodePath);
        w.put<u32>(o.flags);
        w.put<u32>(o.materialSlot);
        w.putId(o.material);
        w.putVec3(o.position);
        w.putQuat(o.rotation);
        w.putVec3(o.scale);
        refs.push_back(o.material);
    }
    ContainerWriter c(static_cast<u32>(AssetKind::PrefabVariant), kVariantVersion, id);
    c.addChunk(kTagMeta, w.bytes());
    c.addChunk(kTagDeps, deps(refs));
    return c.finish();
}

bool DeserializePrefabVariant(const ContainerReader& reader, PrefabVariantAsset& out, std::string* error) {
    if (!checkHeader(reader, AssetKind::PrefabVariant, kVariantVersion, error)) return false;
    std::vector<u8> meta;
    if (!readMeta(reader, meta, error)) return false;
    ByteReader r(meta);
    PrefabVariantAsset v;
    v.name = r.getString();
    v.base = r.getId();
    const u32 count = r.getCount(4 + 4 + 4 + 16 + 12 + 16 + 12);
    for (u32 i = 0; i < count; ++i) {
        PrefabOverride o;
        o.nodePath = r.getString();
        o.flags = r.get<u32>();
        o.materialSlot = r.get<u32>();
        o.material = r.getId();
        o.position = r.getVec3();
        o.rotation = r.getQuat();
        o.scale = r.getVec3();
        if (!r.ok()) break;
        v.overrides.push_back(std::move(o));
    }
    if (!r.ok()) return setError(error, "truncated variant metadata");
    out = std::move(v);
    return true;
}

VariantApplyResult ApplyVariant(const PrefabAsset& base, const PrefabVariantAsset& variant) {
    VariantApplyResult result;
    result.prefab = base;
    if (!variant.name.empty()) result.prefab.name = variant.name;

    std::vector<std::string> paths(base.nodes.size());
    for (u32 i = 0; i < base.nodes.size(); ++i) paths[i] = base.nodePath(i);

    std::vector<bool> removed(base.nodes.size(), false);
    for (const PrefabOverride& o : variant.overrides) {
        const auto it = std::find(paths.begin(), paths.end(), o.nodePath);
        if (it == paths.end()) {
            result.orphaned.push_back(o.nodePath);
            continue;
        }
        PrefabNode& node = result.prefab.nodes[static_cast<size_t>(it - paths.begin())];
        if (o.flags & PrefabOverride::Remove) removed[static_cast<size_t>(it - paths.begin())] = true;
        if (o.flags & PrefabOverride::SetTransform) {
            node.position = o.position;
            node.rotation = o.rotation;
            node.scale = o.scale;
        }
        if (o.flags & PrefabOverride::SetMaterial) {
            if (o.materialSlot < node.materials.size()) node.materials[o.materialSlot] = o.material;
            else result.orphaned.push_back(o.nodePath + "[slot " + std::to_string(o.materialSlot) + "]");
        }
    }

    // Removing a node removes its subtree; then compact and fix parent indices.
    std::vector<i32> remap(base.nodes.size(), -1);
    std::vector<PrefabNode> kept;
    for (size_t i = 0; i < result.prefab.nodes.size(); ++i) {
        const i32 parent = result.prefab.nodes[i].parent;
        if (parent >= 0 && removed[static_cast<size_t>(parent)]) removed[i] = true;
        if (removed[i]) continue;
        PrefabNode n = std::move(result.prefab.nodes[i]);
        n.parent = parent >= 0 ? remap[static_cast<size_t>(parent)] : -1;
        remap[i] = static_cast<i32>(kept.size());
        kept.push_back(std::move(n));
    }
    result.prefab.nodes = std::move(kept);
    return result;
}

std::vector<AssetId> ReadDependencies(const ContainerReader& reader) {
    std::vector<AssetId> out;
    std::vector<u8> bytes;
    if (!reader.readChunk(kTagDeps, bytes)) return out;
    ByteReader r(bytes);
    const u32 n = r.getCount(16);
    for (u32 i = 0; i < n; ++i) out.push_back(r.getId());
    if (!r.ok()) out.clear();
    return out;
}

} // namespace foh
