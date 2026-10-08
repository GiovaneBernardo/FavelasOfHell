#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "foh/asset_import.h"
#include "import_util.h"

namespace foh {

namespace {

// ---- color space ---------------------------------------------------------------------------

const f32* srgbToLinearTable() {
    static const std::vector<f32> table = [] {
        std::vector<f32> t(256);
        for (int i = 0; i < 256; ++i) {
            const f32 c = static_cast<f32>(i) / 255.0f;
            t[static_cast<size_t>(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    return table.data();
}

u8 linearToSrgb8(f32 v) {
    v = std::clamp(v, 0.0f, 1.0f);
    const f32 c = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    return static_cast<u8>(c * 255.0f + 0.5f);
}

TextureMip downsample(const TextureMip& src, bool srgb) {
    TextureMip dst;
    dst.width = std::max(src.width / 2, 1u);
    dst.height = std::max(src.height / 2, 1u);
    dst.data.resize(static_cast<size_t>(dst.width) * dst.height * 4);
    const f32* lut = srgbToLinearTable();
    for (u32 y = 0; y < dst.height; ++y) {
        for (u32 x = 0; x < dst.width; ++x) {
            const u32 x0 = std::min(2 * x, src.width - 1), x1 = std::min(2 * x + 1, src.width - 1);
            const u32 y0 = std::min(2 * y, src.height - 1), y1 = std::min(2 * y + 1, src.height - 1);
            const u8* taps[4] = {&src.data[(static_cast<size_t>(y0) * src.width + x0) * 4],
                                 &src.data[(static_cast<size_t>(y0) * src.width + x1) * 4],
                                 &src.data[(static_cast<size_t>(y1) * src.width + x0) * 4],
                                 &src.data[(static_cast<size_t>(y1) * src.width + x1) * 4]};
            u8* out = &dst.data[(static_cast<size_t>(y) * dst.width + x) * 4];
            for (int c = 0; c < 3; ++c) {
                if (srgb) {
                    const f32 sum = lut[taps[0][c]] + lut[taps[1][c]] + lut[taps[2][c]] + lut[taps[3][c]];
                    out[c] = linearToSrgb8(sum * 0.25f);
                } else {
                    out[c] = static_cast<u8>((taps[0][c] + taps[1][c] + taps[2][c] + taps[3][c] + 2) / 4);
                }
            }
            out[3] = static_cast<u8>((taps[0][3] + taps[1][3] + taps[2][3] + taps[3][3] + 2) / 4); // alpha is always linear
        }
    }
    return dst;
}

// ---- cooking helpers -----------------------------------------------------------------------

TextureRole roleFromSetting(const std::string& setting, const std::string& fileName) {
    if (setting == "color") return TextureRole::Color;
    if (setting == "normal") return TextureRole::Normal;
    if (setting == "data") return TextureRole::Data;
    return GuessTextureRole(fileName);
}

const char* roleSuffix(TextureRole r) { return r == TextureRole::Color ? "color" : r == TextureRole::Normal ? "normal" : "data"; }

CookedAsset cookTexture(const ImportedImage& image, TextureRole role, const std::string& key, const ImportSettings& settings,
                        AssetId guid) {
    const bool srgb = role == TextureRole::Color;
    // The full chain is always built: the size cap is "drop the largest mips", and the
    // no-mips option keeps only the first remaining level.
    std::vector<TextureMip> chain = BuildMipChain(image.pixels.data(), image.width, image.height, srgb);
    size_t drop = 0;
    if (settings.maxTextureSize != 0)
        while (drop + 1 < chain.size() && std::max(chain[drop].width, chain[drop].height) > settings.maxTextureSize) ++drop;
    const size_t last = settings.generateMips ? chain.size() : drop + 1;

    TextureAsset tex;
    tex.name = image.name;
    tex.role = role;
    tex.format = srgb ? TexelFormat::Rgba8Srgb : TexelFormat::Rgba8Unorm;
    tex.mips.assign(std::make_move_iterator(chain.begin() + static_cast<std::ptrdiff_t>(drop)),
                    std::make_move_iterator(chain.begin() + static_cast<std::ptrdiff_t>(last)));
    tex.width = tex.mips.front().width;
    tex.height = tex.mips.front().height;

    CookedAsset out;
    out.key = key;
    out.name = image.name;
    out.kind = AssetKind::Texture;
    out.guid = guid;
    out.bytes = SerializeTexture(tex, guid);
    return out;
}

struct MeshCook {
    MeshAsset asset;
    std::vector<i32> slotMaterials; // slot -> ImportedScene material index (-1 = none)
};

MeshCook cookMesh(const ImportedMesh& src) {
    MeshCook out;
    MeshAsset& m = out.asset;
    m.name = src.name;
    m.streams = {{VertexSemantic::Position, VertexFormat::Float3, offsetof(CookedVertex, position)},
                 {VertexSemantic::Normal, VertexFormat::Float3, offsetof(CookedVertex, normal)},
                 {VertexSemantic::Tangent, VertexFormat::Float4, offsetof(CookedVertex, tangent)},
                 {VertexSemantic::Uv0, VertexFormat::Float2, offsetof(CookedVertex, uv0)}};
    m.vertexStride = sizeof(CookedVertex);
    m.vertexData.resize(src.vertices.size() * sizeof(CookedVertex));
    auto* dst = reinterpret_cast<CookedVertex*>(m.vertexData.data());
    for (size_t i = 0; i < src.vertices.size(); ++i)
        dst[i] = {src.vertices[i].position, src.vertices[i].normal, src.vertices[i].tangent, src.vertices[i].uv0};
    m.indices = src.indices;

    for (const ImportedSubMesh& s : src.subMeshes) {
        auto it = std::find(out.slotMaterials.begin(), out.slotMaterials.end(), s.material);
        if (it == out.slotMaterials.end()) it = out.slotMaterials.insert(out.slotMaterials.end(), s.material);
        SubMesh sm;
        sm.firstIndex = s.firstIndex;
        sm.indexCount = s.indexCount;
        sm.materialSlot = static_cast<u32>(it - out.slotMaterials.begin());
        sm.boundsMin = Vec3(std::numeric_limits<f32>::max());
        sm.boundsMax = Vec3(std::numeric_limits<f32>::lowest());
        for (u32 i = s.firstIndex; i < s.firstIndex + s.indexCount; ++i) {
            const Vec3& p = src.vertices[src.indices[i]].position;
            sm.boundsMin = glm::min(sm.boundsMin, p);
            sm.boundsMax = glm::max(sm.boundsMax, p);
        }
        if (s.indexCount == 0) sm.boundsMin = sm.boundsMax = Vec3(0.0f);
        m.subMeshes.push_back(sm);
    }
    m.materialSlotCount = static_cast<u32>(std::max<size_t>(out.slotMaterials.size(), 1));
    m.boundsMin = Vec3(std::numeric_limits<f32>::max());
    m.boundsMax = Vec3(std::numeric_limits<f32>::lowest());
    for (const ImportedVertex& v : src.vertices) {
        m.boundsMin = glm::min(m.boundsMin, v.position);
        m.boundsMax = glm::max(m.boundsMax, v.position);
    }
    if (src.vertices.empty()) m.boundsMin = m.boundsMax = Vec3(0.0f);
    return out;
}

} // namespace

std::vector<TextureMip> BuildMipChain(const u8* rgba, u32 width, u32 height, bool srgb) {
    std::vector<TextureMip> chain;
    TextureMip base;
    base.width = width;
    base.height = height;
    base.data.assign(rgba, rgba + static_cast<size_t>(width) * height * 4);
    chain.push_back(std::move(base));
    while (chain.back().width > 1 || chain.back().height > 1) chain.push_back(downsample(chain.back(), srgb));
    return chain;
}

TextureRole GuessTextureRole(const std::string& fileName) {
    std::string s = std::filesystem::path(fileName).stem().string();
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto endsWith = [&](std::string_view suffix) { return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0; };
    auto contains = [&](std::string_view part) { return s.find(part) != std::string::npos; };
    if (endsWith("_n") || endsWith("-n") || contains("normal") || endsWith("_nrm") || endsWith("_nor")) return TextureRole::Normal;
    for (std::string_view part : {"_orm", "_rma", "_arm", "rough", "metal", "_ao", "occlusion", "_mask", "height", "displacement", "_disp"})
        if (contains(part)) return TextureRole::Data;
    return TextureRole::Color;
}

std::vector<CookedAsset> CookScene(const ImportedScene& scene, const ImportSettings& settings, const SubAssetIdFn& idFor,
                                   bool sourceIsTexture) {
    std::vector<CookedAsset> out;

    // ---- textures ----
    // One cooked texture per (image, role): the same image can serve as color in one slot and
    // as data in another, and the two need different formats and mip filtering.
    std::map<std::pair<i32, TextureRole>, AssetId> textureIds;
    auto textureFor = [&](const ImportedTextureRef& ref, TextureRole role) -> TextureRef {
        TextureRef r;
        if (ref.image < 0 || static_cast<size_t>(ref.image) >= scene.images.size()) return r;
        r.uvSet = ref.uvSet;
        const auto key = std::make_pair(ref.image, role);
        auto it = textureIds.find(key);
        if (it == textureIds.end()) {
            const ImportedImage& image = scene.images[static_cast<size_t>(ref.image)];
            const std::string subKey = image.key + ":" + roleSuffix(role);
            const AssetId guid = idFor(AssetKind::Texture, subKey);
            out.push_back(cookTexture(image, role, subKey, settings, guid));
            it = textureIds.emplace(key, guid).first;
        }
        r.texture = it->second;
        return r;
    };

    if (sourceIsTexture) {
        for (const ImportedImage& image : scene.images) {
            const TextureRole role = roleFromSetting(settings.textureRole, image.name);
            const AssetId guid = idFor(AssetKind::Texture, image.key);
            out.push_back(cookTexture(image, role, image.key, settings, guid));
        }
        return out;
    }

    // ---- materials ----
    std::vector<AssetId> materialIds(scene.materials.size());
    for (size_t i = 0; i < scene.materials.size(); ++i) {
        const ImportedMaterial& src = scene.materials[i];
        MaterialAsset m;
        m.name = src.name;
        m.baseColor = src.baseColor;
        m.metallic = src.metallic;
        m.roughness = src.roughness;
        m.emissive = src.emissive;
        m.alphaMode = src.alphaMode;
        m.alphaCutoff = src.alphaCutoff;
        m.doubleSided = src.doubleSided;
        m.normalScale = src.normalScale;
        m.occlusionStrength = src.occlusionStrength;
        m.baseColorTexture = textureFor(src.baseColorTexture, TextureRole::Color);
        m.normalTexture = textureFor(src.normalTexture, TextureRole::Normal);
        m.metallicRoughnessTexture = textureFor(src.metallicRoughnessTexture, TextureRole::Data);
        m.occlusionTexture = textureFor(src.occlusionTexture, TextureRole::Data);
        m.emissiveTexture = textureFor(src.emissiveTexture, TextureRole::Color);

        CookedAsset c;
        c.key = src.key;
        c.name = src.name;
        c.kind = AssetKind::Material;
        c.guid = idFor(AssetKind::Material, src.key);
        c.bytes = SerializeMaterial(m, c.guid);
        for (const TextureRef* t : {&m.baseColorTexture, &m.normalTexture, &m.metallicRoughnessTexture, &m.occlusionTexture, &m.emissiveTexture})
            if (t->isValid()) c.dependencies.push_back(t->texture);
        materialIds[i] = c.guid;
        out.push_back(std::move(c));
    }

    // ---- meshes ----
    std::vector<AssetId> meshIds(scene.meshes.size());
    std::vector<std::vector<AssetId>> meshSlotMaterials(scene.meshes.size());
    for (size_t i = 0; i < scene.meshes.size(); ++i) {
        MeshCook cook = cookMesh(scene.meshes[i]);
        CookedAsset c;
        c.key = scene.meshes[i].key;
        c.name = scene.meshes[i].name;
        c.kind = AssetKind::Mesh;
        c.guid = idFor(AssetKind::Mesh, c.key);
        c.bytes = SerializeMesh(cook.asset, c.guid);
        meshIds[i] = c.guid;
        for (i32 mat : cook.slotMaterials)
            meshSlotMaterials[i].push_back(mat >= 0 && static_cast<size_t>(mat) < materialIds.size() ? materialIds[static_cast<size_t>(mat)] : AssetId{});
        out.push_back(std::move(c));
    }

    // ---- prefab ----
    if (settings.generatePrefab && !scene.nodes.empty()) {
        PrefabAsset prefab;
        prefab.name = scene.name;
        std::vector<std::set<std::string>> siblingNames(scene.nodes.size() + 1); // index parent+1
        CookedAsset c;
        for (const ImportedNode& n : scene.nodes) {
            PrefabNode node;
            std::set<std::string>& used = siblingNames[static_cast<size_t>(n.parent + 1)];
            node.name = import_util::uniqueKey(used, import_util::sanitizeName(n.name), "node");
            node.parent = n.parent;
            node.position = n.position;
            node.rotation = n.rotation;
            node.scale = n.scale;
            if (n.mesh >= 0 && static_cast<size_t>(n.mesh) < scene.meshes.size()) {
                node.mesh = meshIds[static_cast<size_t>(n.mesh)];
                node.materials = meshSlotMaterials[static_cast<size_t>(n.mesh)];
                c.dependencies.push_back(node.mesh);
                for (const AssetId& m : node.materials)
                    if (m.isValid()) c.dependencies.push_back(m);
            }
            prefab.nodes.push_back(std::move(node));
        }
        c.key = "prefab";
        c.name = scene.name;
        c.kind = AssetKind::Prefab;
        c.guid = idFor(AssetKind::Prefab, c.key);
        c.bytes = SerializePrefab(prefab, c.guid);
        out.push_back(std::move(c));
    }
    return out;
}

} // namespace foh
