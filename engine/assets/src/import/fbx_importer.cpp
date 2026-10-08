#include <algorithm>
#include <map>
#include <set>

#include <ufbx.h>

#include "foh/asset_import.h"
#include "foh/utf8_path.h"
#include "import_util.h"

namespace foh {

namespace {

namespace fs = std::filesystem;

Vec3 toVec3(ufbx_vec3 v) { return Vec3(static_cast<f32>(v.x), static_cast<f32>(v.y), static_cast<f32>(v.z)); }
Vec2 toVec2(ufbx_vec2 v) { return Vec2(static_cast<f32>(v.x), static_cast<f32>(v.y)); }

class FbxReader {
public:
    FbxReader(const fs::path& path, const ufbx_scene& scene, ImportedScene& out)
        : m_dir(path.parent_path()), m_scene(scene), m_out(out) {}

    void readAll() {
        readMaterials();
        for (ufbx_node* root_child : m_scene.root_node->children) addNode(*root_child, -1, 0);
    }

private:
    i32 imageFor(const ufbx_texture* texture) {
        if (!texture) return -1;
        if (auto it = m_imageMap.find(texture); it != m_imageMap.end()) return it->second;

        i32 result = -1;
        std::vector<u8> bytes;
        std::string label = texture->relative_filename.length ? std::string(texture->relative_filename.data, texture->relative_filename.length)
                                                              : std::string(texture->name.data, texture->name.length);
        if (texture->content.size > 0) {
            const u8* p = static_cast<const u8*>(texture->content.data);
            bytes.assign(p, p + texture->content.size);
        } else {
            // Exporters write absolute paths of the author's machine; try the relative name and the
            // bare file name next to the model before giving up.
            const fs::path rel = PathFromUtf8(std::string(texture->relative_filename.data, texture->relative_filename.length));
            const fs::path abs = PathFromUtf8(std::string(texture->absolute_filename.data, texture->absolute_filename.length));
            for (const fs::path& candidate : {m_dir / rel, abs, m_dir / abs.filename(), m_dir / rel.filename()}) {
                if (candidate.empty() || candidate == m_dir) continue;
                if (ReadFileBytes(candidate, bytes)) {
                    m_out.externalFiles.push_back(candidate);
                    break;
                }
            }
        }
        if (bytes.empty()) {
            m_out.warnings.push_back("texture not found: " + label);
        } else {
            ImportedImage image;
            std::string error;
            if (import_util::probeImage(bytes.data(), bytes.size(), error)) {
                image.encoded = std::move(bytes);
                image.name = PathFromUtf8(label).stem().string();
                if (image.name.empty()) image.name = "image" + std::to_string(m_out.images.size());
                image.key = import_util::uniqueKey(m_imageKeys, image.name, "image" + std::to_string(m_out.images.size()));
                result = static_cast<i32>(m_out.images.size());
                m_out.images.push_back(std::move(image));
            } else {
                m_out.warnings.push_back("texture '" + label + "': " + error);
            }
        }
        m_imageMap[texture] = result;
        return result;
    }

    ImportedTextureRef textureRef(const ufbx_material_map& map) {
        ImportedTextureRef ref;
        if (map.texture && map.texture_enabled) ref.image = imageFor(map.texture);
        return ref;
    }

    void readMaterials() {
        std::set<std::string> keys;
        for (size_t i = 0; i < m_scene.materials.count; ++i) {
            const ufbx_material& src = *m_scene.materials.data[i];
            ImportedMaterial m;
            m.name = src.name.length ? std::string(src.name.data, src.name.length) : "material" + std::to_string(i);
            m.key = import_util::uniqueKey(keys, m.name, "material" + std::to_string(i));
            if (src.pbr.base_color.has_value) {
                const ufbx_vec4 c = src.pbr.base_color.value_vec4;
                m.baseColor = Vec4(static_cast<f32>(c.x), static_cast<f32>(c.y), static_cast<f32>(c.z), static_cast<f32>(c.w));
            }
            if (src.pbr.metalness.has_value) m.metallic = static_cast<f32>(src.pbr.metalness.value_real);
            if (src.pbr.roughness.has_value) m.roughness = static_cast<f32>(src.pbr.roughness.value_real);
            if (src.pbr.emission_color.has_value) {
                const f32 factor = src.pbr.emission_factor.has_value ? static_cast<f32>(src.pbr.emission_factor.value_real) : 1.0f;
                m.emissive = toVec3(src.pbr.emission_color.value_vec3) * factor;
            }
            m.doubleSided = src.features.double_sided.enabled;
            m.baseColorTexture = textureRef(src.pbr.base_color);
            m.normalTexture = textureRef(src.pbr.normal_map);
            m.metallicRoughnessTexture = textureRef(src.pbr.roughness); // FBX keeps maps separate; first one wins
            m.occlusionTexture = textureRef(src.pbr.ambient_occlusion);
            m.emissiveTexture = textureRef(src.pbr.emission_color);
            m_materialIndex[&src] = static_cast<i32>(m_out.materials.size());
            m_out.materials.push_back(std::move(m));
        }
    }

    i32 meshFor(const ufbx_mesh& mesh) {
        if (auto it = m_meshIndex.find(&mesh); it != m_meshIndex.end()) return it->second;

        ImportedMesh out;
        out.name = mesh.name.length ? std::string(mesh.name.data, mesh.name.length) : "mesh" + std::to_string(m_out.meshes.size());
        out.key = import_util::uniqueKey(m_meshKeys, out.name, "mesh" + std::to_string(m_out.meshes.size()));

        std::vector<u32> tri(mesh.max_face_triangles * 3);
        auto appendFace = [&](const ufbx_face& face) {
            const u32 triangles = ufbx_triangulate_face(tri.data(), tri.size(), &mesh, face);
            for (u32 t = 0; t < triangles * 3; ++t) {
                const u32 corner = tri[t];
                ImportedVertex v;
                v.position = toVec3(ufbx_get_vertex_vec3(&mesh.vertex_position, corner));
                if (mesh.vertex_normal.exists) v.normal = toVec3(ufbx_get_vertex_vec3(&mesh.vertex_normal, corner));
                if (mesh.vertex_uv.exists) v.uv0 = toVec2(ufbx_get_vertex_vec2(&mesh.vertex_uv, corner));
                if (mesh.vertex_tangent.exists && mesh.vertex_bitangent.exists && mesh.vertex_normal.exists) {
                    const Vec3 t3 = toVec3(ufbx_get_vertex_vec3(&mesh.vertex_tangent, corner));
                    const Vec3 b3 = toVec3(ufbx_get_vertex_vec3(&mesh.vertex_bitangent, corner));
                    v.tangent = Vec4(t3, glm::dot(glm::cross(v.normal, t3), b3) < 0.0f ? -1.0f : 1.0f);
                }
                out.indices.push_back(static_cast<u32>(out.vertices.size()));
                out.vertices.push_back(v);
            }
        };

        if (mesh.material_parts.count == 0) {
            ImportedSubMesh sub;
            for (size_t f = 0; f < mesh.faces.count; ++f) appendFace(mesh.faces.data[f]);
            sub.indexCount = static_cast<u32>(out.indices.size());
            out.subMeshes.push_back(sub);
        } else {
            for (size_t p = 0; p < mesh.material_parts.count; ++p) {
                const ufbx_mesh_part& part = mesh.material_parts.data[p];
                if (part.num_triangles == 0) continue;
                ImportedSubMesh sub;
                sub.firstIndex = static_cast<u32>(out.indices.size());
                for (size_t f = 0; f < part.face_indices.count; ++f) appendFace(mesh.faces.data[part.face_indices.data[f]]);
                sub.indexCount = static_cast<u32>(out.indices.size()) - sub.firstIndex;
                if (part.index < mesh.materials.count) {
                    const auto it = m_materialIndex.find(mesh.materials.data[part.index]);
                    if (it != m_materialIndex.end()) sub.material = it->second;
                }
                out.subMeshes.push_back(sub);
            }
        }

        import_util::weldVertices(out);
        if (!mesh.vertex_normal.exists) import_util::generateNormals(out);
        if (mesh.vertex_uv.exists && !(mesh.vertex_tangent.exists && mesh.vertex_bitangent.exists)) import_util::generateTangents(out);

        const i32 index = static_cast<i32>(m_out.meshes.size());
        m_out.meshes.push_back(std::move(out));
        m_meshIndex[&mesh] = index;
        return index;
    }

    void addNode(const ufbx_node& src, i32 parent, int depth) {
        if (depth > 256) return;
        ImportedNode node;
        node.name = src.name.length ? std::string(src.name.data, src.name.length) : "node" + std::to_string(m_out.nodes.size());
        node.parent = parent;
        node.position = toVec3(src.local_transform.translation);
        node.rotation = Quat(static_cast<f32>(src.local_transform.rotation.w), static_cast<f32>(src.local_transform.rotation.x),
                             static_cast<f32>(src.local_transform.rotation.y), static_cast<f32>(src.local_transform.rotation.z));
        node.scale = toVec3(src.local_transform.scale);
        if (src.mesh) node.mesh = meshFor(*src.mesh);
        const i32 index = static_cast<i32>(m_out.nodes.size());
        m_out.nodes.push_back(std::move(node));
        for (ufbx_node* child : src.children) addNode(*child, index, depth + 1);
    }

    fs::path m_dir;
    const ufbx_scene& m_scene;
    ImportedScene& m_out;
    std::map<const ufbx_texture*, i32> m_imageMap;
    std::map<const ufbx_material*, i32> m_materialIndex;
    std::map<const ufbx_mesh*, i32> m_meshIndex;
    std::set<std::string> m_imageKeys, m_meshKeys;
};

} // namespace

bool ImportFbx(const fs::path& path, ImportedScene& out, std::string& error) {
    std::vector<u8> bytes;
    if (!ReadFileBytes(path, bytes)) {
        error = "cannot read " + path.string();
        return false;
    }

    ufbx_load_opts opts = {};
    // Engine conventions: right handed, Y up, meters. Baking the conversion into the geometry
    // keeps the root node an identity transform, so the prefab needs no special root.
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0f;
    opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
    opts.generate_missing_normals = true;

    ufbx_error fbxError;
    ufbx_scene* scene = ufbx_load_memory(bytes.data(), bytes.size(), &opts, &fbxError);
    if (!scene) {
        error = std::string("cannot load FBX: ") + fbxError.description.data;
        return false;
    }
    struct Guard {
        ufbx_scene* scene;
        ~Guard() { ufbx_free_scene(scene); }
    } guard{scene};

    ImportedScene imported;
    imported.name = path.stem().string();
    FbxReader reader(path, *scene, imported);
    reader.readAll();
    std::sort(imported.externalFiles.begin(), imported.externalFiles.end());
    imported.externalFiles.erase(std::unique(imported.externalFiles.begin(), imported.externalFiles.end()), imported.externalFiles.end());
    out = std::move(imported);
    return true;
}

} // namespace foh
