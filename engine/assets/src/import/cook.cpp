#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>

#include "foh/asset_import.h"
#include "import_util.h"

namespace foh {

namespace {

// ---- cooking helpers -----------------------------------------------------------------------

TextureRole roleFromSetting(const std::string& setting, const std::string& fileName) {
    if (setting == "color") return TextureRole::Color;
    if (setting == "normal") return TextureRole::Normal;
    if (setting == "data") return TextureRole::Data;
    return GuessTextureRole(fileName);
}

const char* roleSuffix(TextureRole r) { return r == TextureRole::Color ? "color" : r == TextureRole::Normal ? "normal" : "data"; }

struct MeshCook {
    MeshAsset asset;
    std::vector<i32> slotMaterials; // slot -> ImportedScene material index (-1 = none)
};

// `skeleton` is valid for a skinned mesh: the vertices then carry joint indices and weights.
MeshCook cookMesh(const ImportedMesh& src, const AssetId& skeleton) {
    MeshCook out;
    MeshAsset& m = out.asset;
    m.name = src.name;
    m.streams = {{VertexSemantic::Position, VertexFormat::Float3, offsetof(CookedVertex, position)},
                 {VertexSemantic::Normal, VertexFormat::Float3, offsetof(CookedVertex, normal)},
                 {VertexSemantic::Tangent, VertexFormat::Float4, offsetof(CookedVertex, tangent)},
                 {VertexSemantic::Uv0, VertexFormat::Float2, offsetof(CookedVertex, uv0)}};
    if (skeleton.isValid()) {
        m.skeleton = skeleton;
        m.streams.push_back({VertexSemantic::Joints0, VertexFormat::U8x4, offsetof(CookedSkinnedVertex, joints)});
        m.streams.push_back({VertexSemantic::Weights0, VertexFormat::Unorm8x4, offsetof(CookedSkinnedVertex, weights)});
        m.vertexStride = sizeof(CookedSkinnedVertex);
        m.vertexData.resize(src.vertices.size() * sizeof(CookedSkinnedVertex));
        auto* skinned = reinterpret_cast<CookedSkinnedVertex*>(m.vertexData.data());
        for (size_t i = 0; i < src.vertices.size(); ++i) {
            const ImportedVertex& v = src.vertices[i];
            skinned[i].base = {v.position, v.normal, v.tangent, v.uv0};
            std::memcpy(skinned[i].joints, v.joints, 4);
            std::memcpy(skinned[i].weights, v.weights, 4);
        }
    } else {
        m.vertexStride = sizeof(CookedVertex);
        m.vertexData.resize(src.vertices.size() * sizeof(CookedVertex));
        auto* dst = reinterpret_cast<CookedVertex*>(m.vertexData.data());
        for (size_t i = 0; i < src.vertices.size(); ++i)
            dst[i] = {src.vertices[i].position, src.vertices[i].normal, src.vertices[i].tangent, src.vertices[i].uv0};
    }
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
                                   bool sourceIsTexture, JobSystem* jobs, std::vector<std::string>* warnings) {
    std::vector<CookedAsset> out;

    // ---- textures ----
    // One cooked texture per (image, role): the same image can serve as color in one slot and as data
    // in another, and the two need different formats and mip filtering. References are resolved first
    // (cheap, sequential, deterministic); the expensive cooking happens afterwards in one parallel batch.
    struct TextureJob {
        i32 image;
        TextureRole role;
        std::string key;
        AssetId guid;
        CookedAsset result;
        std::string warning;
        bool ok = false;
    };
    std::vector<TextureJob> textureJobs;
    std::map<std::pair<i32, TextureRole>, AssetId> textureIds;
    auto textureFor = [&](const ImportedTextureRef& ref, TextureRole role) -> TextureRef {
        TextureRef r;
        if (ref.image < 0 || static_cast<size_t>(ref.image) >= scene.images.size()) return r;
        r.uvSet = ref.uvSet;
        const auto key = std::make_pair(ref.image, role);
        auto it = textureIds.find(key);
        if (it == textureIds.end()) {
            const std::string subKey = scene.images[static_cast<size_t>(ref.image)].key + ":" + roleSuffix(role);
            const AssetId guid = idFor(AssetKind::Texture, subKey);
            textureJobs.push_back({ref.image, role, subKey, guid, {}, {}, false});
            it = textureIds.emplace(key, guid).first;
        }
        r.texture = it->second;
        return r;
    };
    if (sourceIsTexture) {
        for (size_t i = 0; i < scene.images.size(); ++i) {
            const ImportedImage& image = scene.images[i];
            textureJobs.push_back({static_cast<i32>(i), roleFromSetting(settings.textureRole, image.name), image.key,
                                   idFor(AssetKind::Texture, image.key), {}, {}, false});
        }
    }

    // Cooked after the materials below have registered their references.
    auto cookTextures = [&] {
        auto cookOne = [&](TextureJob& job) {
            const ImportedImage& image = scene.images[static_cast<size_t>(job.image)];
            import_util::DecodedImage decoded;
            std::string error;
            if (!import_util::decodeImage(image.encoded.data(), image.encoded.size(), decoded, error)) {
                job.warning = "image '" + image.name + "': " + error;
                return;
            }
            TextureAsset tex;
            if (!CookTexture(decoded.pixels.get(), decoded.width, decoded.height, image.name, job.role, settings, jobs, tex,
                             &job.warning))
                return;
            job.result.key = job.key;
            job.result.name = image.name;
            job.result.kind = AssetKind::Texture;
            job.result.guid = job.guid;
            job.result.bytes = SerializeTexture(tex, job.guid);
            job.ok = true;
        };
        // At most two textures are decoded/compressed at the same time (each one is parallel inside): a 4K image
        // is 64 MB decoded plus its mips, and a scene with fifty of them must neither hold them all in memory nor
        // fault in gigabytes of fresh pages at once (which stalls every other thread of the process, the game's too).
        const u32 count = static_cast<u32>(textureJobs.size());
        ParallelFor(jobs, count, std::max(1u, (count + 1) / 2), [&](u32 begin, u32 end) {
            for (u32 i = begin; i < end; ++i) cookOne(textureJobs[i]);
        });
        for (TextureJob& job : textureJobs) {
            if (!job.warning.empty() && warnings) warnings->push_back(job.warning);
            if (job.ok) out.push_back(std::move(job.result));
        }
    };

    if (sourceIsTexture) {
        cookTextures();
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

    cookTextures();

    // ---- skeletons and clips ----
    // A skeleton becomes an asset only when a mesh is skinned to it. A file of animations alone (Mixamo "without
    // skin") yields clips only: they find their joints by name on whatever skeleton plays them.
    std::vector<AssetId> skeletonIds(scene.skeletons.size());
    for (size_t i = 0; i < scene.skeletons.size(); ++i) {
        const ImportedSkeleton& src = scene.skeletons[i];
        if (!src.usedBySkin) continue;
        if (src.parents.size() > kMaxSkeletonJoints) {
            if (warnings) warnings->push_back("skeleton '" + src.name + "' has more than 255 joints; skinned meshes bound to it are static");
            continue;
        }
        CookedAsset c;
        c.key = src.key;
        c.name = src.name;
        c.kind = AssetKind::Skeleton;
        c.guid = idFor(AssetKind::Skeleton, c.key);
        c.bytes = SerializeSkeleton(CookSkeleton(src), c.guid);
        skeletonIds[i] = c.guid;
        out.push_back(std::move(c));
    }
    for (const ImportedClip& clip : scene.clips) {
        if (clip.skeleton < 0 || static_cast<size_t>(clip.skeleton) >= scene.skeletons.size() || clip.frameCount == 0) continue;
        CookedAsset c;
        c.key = clip.key;
        c.name = clip.name;
        c.kind = AssetKind::AnimationClip;
        c.guid = idFor(AssetKind::AnimationClip, c.key);
        c.bytes = SerializeClip(CookClip(clip, scene.skeletons[static_cast<size_t>(clip.skeleton)], settings, warnings), c.guid);
        out.push_back(std::move(c));
    }

    // ---- meshes ----
    std::vector<AssetId> meshIds(scene.meshes.size());
    std::vector<std::vector<AssetId>> meshSlotMaterials(scene.meshes.size());
    std::vector<std::vector<u32>> meshSubmeshSlots(scene.meshes.size());
    for (size_t i = 0; i < scene.meshes.size(); ++i) {
        const i32 skeletonIndex = scene.meshes[i].skeleton;
        const AssetId skeleton = skeletonIndex >= 0 && static_cast<size_t>(skeletonIndex) < skeletonIds.size() ? skeletonIds[static_cast<size_t>(skeletonIndex)] : AssetId{};
        MeshCook cook = cookMesh(scene.meshes[i], skeleton);
        CookedAsset c;
        c.key = scene.meshes[i].key;
        c.name = scene.meshes[i].name;
        c.kind = AssetKind::Mesh;
        c.guid = idFor(AssetKind::Mesh, c.key);
        c.bytes = SerializeMesh(cook.asset, c.guid);
        meshIds[i] = c.guid;
        if (skeleton.isValid()) c.dependencies.push_back(skeleton);
        for (const SubMesh& sm : cook.asset.subMeshes) meshSubmeshSlots[i].push_back(sm.materialSlot);
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
                node.submeshSlots = meshSubmeshSlots[static_cast<size_t>(n.mesh)];
                c.dependencies.push_back(node.mesh);
                // The instance root animates the skinned meshes below it; one skeleton per prefab.
                const i32 skeletonIndex = scene.meshes[static_cast<size_t>(n.mesh)].skeleton;
                if (skeletonIndex >= 0 && static_cast<size_t>(skeletonIndex) < skeletonIds.size() && skeletonIds[static_cast<size_t>(skeletonIndex)].isValid()) {
                    const AssetId& id = skeletonIds[static_cast<size_t>(skeletonIndex)];
                    if (!prefab.skeleton.isValid()) prefab.skeleton = id;
                    else if (prefab.skeleton != id && warnings) warnings->push_back("prefab '" + scene.name + "': meshes are skinned to different skeletons, only the first one is animated");
                }
                for (const AssetId& m : node.materials)
                    if (m.isValid()) c.dependencies.push_back(m);
            }
            prefab.nodes.push_back(std::move(node));
        }
        if (prefab.skeleton.isValid()) c.dependencies.push_back(prefab.skeleton);
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
