#include "foh/scene/asset_scene_bridge.h"

#include <cstring>

#include "foh/ecs/inspect.h"
#include "foh/log.h"

namespace foh {

AssetSceneBridge::AssetSceneBridge(AssetDb& db, Renderer& renderer) : m_db(db), m_renderer(renderer) {}

bool AssetSceneBridge::uploadMesh(const AssetId& id, std::string* error) {
    MeshAsset mesh;
    if (!m_db.load(id, mesh, error)) return false;

    // The renderer's vertex layout is described by the mesh's own stream table: read the
    // streams it understands by semantic, ignore the rest.
    const VertexStream* pos = mesh.findStream(VertexSemantic::Position);
    if (!pos || pos->format != VertexFormat::Float3) {
        if (error) *error = "mesh has no float3 position stream";
        return false;
    }
    const VertexStream* nrm = mesh.findStream(VertexSemantic::Normal);
    const VertexStream* uv = mesh.findStream(VertexSemantic::Uv0);
    const u32 count = mesh.vertexCount();
    std::vector<Vec3> positions(count), normals;
    std::vector<Vec2> uvs;
    if (nrm && nrm->format == VertexFormat::Float3) normals.resize(count);
    if (uv && uv->format == VertexFormat::Float2) uvs.resize(count);
    for (u32 i = 0; i < count; ++i) {
        const u8* v = mesh.vertexData.data() + static_cast<size_t>(i) * mesh.vertexStride;
        std::memcpy(&positions[i], v + pos->offset, sizeof(Vec3));
        if (!normals.empty()) std::memcpy(&normals[i], v + nrm->offset, sizeof(Vec3));
        if (!uvs.empty()) std::memcpy(&uvs[i], v + uv->offset, sizeof(Vec2));
    }

    std::vector<MeshIndexRange> ranges;
    MeshInfo info;
    for (const SubMesh& s : mesh.subMeshes) {
        ranges.push_back({s.firstIndex, s.indexCount});
        info.submeshMaterialSlots.push_back(s.materialSlot);
    }
    if (!m_renderer.uploadMeshAsset(id, {positions, normals, uvs, mesh.indices}, ranges)) {
        if (error) *error = "the renderer rejected the mesh geometry";
        return false;
    }
    m_meshes[id] = std::move(info);
    return true;
}

const MeshInfo* AssetSceneBridge::ensureMesh(const AssetId& mesh, std::string* error) {
    if (const auto it = m_meshes.find(mesh); it != m_meshes.end()) return &it->second;
    if (!uploadMesh(mesh, error)) return nullptr;
    return &m_meshes.at(mesh);
}

Vec4 AssetSceneBridge::materialColor(const AssetId& material) {
    if (!material.isValid()) return Vec4(0.8f, 0.8f, 0.8f, 1.0f);
    if (const auto it = m_colors.find(material); it != m_colors.end()) return it->second;
    MaterialAsset m;
    const Vec4 color = m_db.load(material, m) ? m.baseColor : Vec4(0.8f, 0.8f, 0.8f, 1.0f);
    m_colors[material] = color;
    return color;
}

PrefabInstanceResult AssetSceneBridge::instantiate(World& world, AssetId prefabOrVariant, const Transform& at, std::string* error) {
    VariantApplyResult resolved;
    if (!m_db.loadResolvedPrefab(prefabOrVariant, resolved, error)) return {};
    for (const std::string& orphan : resolved.orphaned) FOH_WARN("prefab variant: override target no longer exists: {}", orphan);

    auto meshInfo = [&](const AssetId& id) -> const MeshInfo* {
        std::string meshError;
        const MeshInfo* info = ensureMesh(id, &meshError);
        if (!info) FOH_WARN("prefab '{}': mesh {} not drawn: {}", resolved.prefab.name, ToString(id), meshError);
        return info;
    };
    auto color = [&](const AssetId& id) { return materialColor(id); };
    return InstantiatePrefab(world, resolved.prefab, meshInfo, color, at);
}

void AssetSceneBridge::assetsChanged(std::span<const AssetId> changed, std::span<World* const> worlds) {
    bool colorsChanged = false;
    for (const AssetId& id : changed) {
        const AssetInfo* info = m_db.find(id);
        if (!info) {
            // Removed by the reimport.
            if (m_meshes.erase(id)) m_renderer.releaseMeshAsset(id);
            m_colors.erase(id);
            continue;
        }
        if (info->kind == AssetKind::Mesh && m_meshes.contains(id)) {
            std::string error;
            if (!uploadMesh(id, &error)) FOH_ERROR("cannot reload mesh '{}': {}", info->name, error);
        } else if (info->kind == AssetKind::Material) {
            m_colors.erase(id);
            colorsChanged = true;
        }
    }
    if (!colorsChanged) return;

    for (World* world : worlds) {
        const ComponentId renderer = world->component<MeshRenderer>();
        std::vector<Entity> entities;
        findEntities(*world, std::span(&renderer, 1), {}, entities);
        for (const Entity e : entities) {
            MeshRenderer* mr = world->get<MeshRenderer>(e);
            if (mr && mr->material.isValid()) mr->color = materialColor(mr->material);
        }
    }
}

} // namespace foh
