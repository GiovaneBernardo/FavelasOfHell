#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#include <glm/gtc/type_ptr.hpp>

#include "foh/asset_import.h"
#include "foh/utf8_path.h"
#include "foh/log.h"
#include "import_util.h"

namespace foh {

namespace {

namespace fs = std::filesystem;

// Reads files through our own I/O (unicode paths on Windows) and remembers which ones were read.
struct FileCollector {
    std::vector<fs::path> files;
};

cgltf_result readFile(const cgltf_memory_options* memory, const cgltf_file_options* fileOptions, const char* path,
                      cgltf_size* size, void** data) {
    std::vector<u8> bytes;
    const fs::path p = PathFromUtf8(path);
    if (!ReadFileBytes(p, bytes)) return cgltf_result_file_not_found;
    void* copy = memory->alloc_func ? memory->alloc_func(memory->user_data, bytes.size()) : std::malloc(bytes.size());
    if (!copy && !bytes.empty()) return cgltf_result_out_of_memory;
    if (!bytes.empty()) std::memcpy(copy, bytes.data(), bytes.size());
    *size = bytes.size();
    *data = copy;
    static_cast<FileCollector*>(fileOptions->user_data)->files.push_back(p);
    return cgltf_result_success;
}

void releaseFile(const cgltf_memory_options* memory, const cgltf_file_options*, void* data) {
    if (memory->free_func) memory->free_func(memory->user_data, data);
    else std::free(data);
}

Quat quatFromMatrix(const Mat3& m) { return glm::normalize(glm::quat_cast(m)); }

void decomposeMatrix(const f32* m, ImportedNode& node) {
    const Mat4 mat = glm::make_mat4(m);
    node.position = Vec3(mat[3]);
    Vec3 cols[3] = {Vec3(mat[0]), Vec3(mat[1]), Vec3(mat[2])};
    Vec3 scale(glm::length(cols[0]), glm::length(cols[1]), glm::length(cols[2]));
    if (glm::determinant(Mat3(cols[0], cols[1], cols[2])) < 0.0f) scale.x = -scale.x; // mirrored
    for (int i = 0; i < 3; ++i)
        if (scale[i] != 0.0f) cols[i] /= scale[i];
    node.scale = scale;
    node.rotation = quatFromMatrix(Mat3(cols[0], cols[1], cols[2]));
}

std::string imageName(const cgltf_image& image, size_t index) {
    if (image.name && *image.name) return image.name;
    if (image.uri && std::strncmp(image.uri, "data:", 5) != 0) {
        std::string uri = image.uri;
        cgltf_decode_uri(uri.data());
        return PathFromUtf8(uri.c_str()).stem().string();
    }
    return "image" + std::to_string(index);
}

class GltfReader {
public:
    GltfReader(const fs::path& path, cgltf_data* data, const cgltf_options& options, ImportedScene& out)
        : m_dir(path.parent_path()), m_data(data), m_options(options), m_out(out) {}

    void readAll() {
        readMaterials();
        readMeshes();
        readNodes();
    }

private:
    static bool floatArray(const cgltf_accessor* accessor, std::vector<f32>& out) {
        const cgltf_size comps = cgltf_num_components(accessor->type);
        out.resize(accessor->count * comps);
        return cgltf_accessor_unpack_floats(accessor, out.data(), out.size()) == out.size() || out.empty();
    }

    // Decoded image index in the ImportedScene, decoding on first use. -1 on failure.
    i32 imageFor(const cgltf_texture* texture) {
        if (!texture || !texture->image) return -1;
        const size_t gltfIndex = cgltf_image_index(m_data, texture->image);
        if (auto it = m_imageMap.find(gltfIndex); it != m_imageMap.end()) return it->second;

        i32 result = -1;
        std::vector<u8> encoded;
        if (loadImageBytes(*texture->image, encoded)) {
            ImportedImage image;
            std::string error;
            if (import_util::decodeImage(encoded.data(), encoded.size(), image, error)) {
                image.name = imageName(*texture->image, gltfIndex);
                image.key = import_util::uniqueKey(m_imageKeys, image.name, "image" + std::to_string(gltfIndex));
                result = static_cast<i32>(m_out.images.size());
                m_out.images.push_back(std::move(image));
            } else {
                m_out.warnings.push_back("image '" + imageName(*texture->image, gltfIndex) + "': " + error);
            }
        }
        m_imageMap[gltfIndex] = result;
        return result;
    }

    bool loadImageBytes(const cgltf_image& image, std::vector<u8>& out) {
        if (image.buffer_view) {
            const u8* base = cgltf_buffer_view_data(image.buffer_view);
            if (!base) return false;
            out.assign(base, base + image.buffer_view->size);
            return true;
        }
        if (!image.uri) return false;
        if (std::strncmp(image.uri, "data:", 5) == 0) {
            const char* comma = std::strchr(image.uri, ',');
            if (!comma) return false;
            const char* b64 = comma + 1;
            const size_t len = std::strlen(b64);
            size_t padding = 0;
            while (padding < len && padding < 2 && b64[len - 1 - padding] == '=') ++padding;
            const size_t size = len / 4 * 3 - padding;
            void* decoded = nullptr;
            if (cgltf_load_buffer_base64(&m_options, size, b64, &decoded) != cgltf_result_success) return false;
            out.assign(static_cast<u8*>(decoded), static_cast<u8*>(decoded) + size);
            m_options.memory.free_func ? m_options.memory.free_func(m_options.memory.user_data, decoded) : std::free(decoded);
            return true;
        }
        std::string uri = image.uri;
        cgltf_decode_uri(uri.data());
        const fs::path file = m_dir / PathFromUtf8(uri.c_str());
        if (!ReadFileBytes(file, out)) {
            m_out.warnings.push_back("missing texture file: " + file.string());
            return false;
        }
        m_out.externalFiles.push_back(file);
        return true;
    }

    ImportedTextureRef textureRef(const cgltf_texture_view& view) {
        ImportedTextureRef ref;
        if (!view.texture) return ref;
        ref.image = imageFor(view.texture);
        ref.uvSet = static_cast<u8>(std::clamp(view.texcoord, 0, 1));
        return ref;
    }

    void readMaterials() {
        std::set<std::string> keys;
        m_materialBase = m_out.materials.size();
        for (cgltf_size i = 0; i < m_data->materials_count; ++i) {
            const cgltf_material& src = m_data->materials[i];
            ImportedMaterial m;
            m.name = src.name ? src.name : "material" + std::to_string(i);
            m.key = import_util::uniqueKey(keys, m.name, "material" + std::to_string(i));
            // glTF: a material without a pbrMetallicRoughness block still behaves like factors (1, 1, 1).
            if (src.has_pbr_metallic_roughness) {
                const cgltf_pbr_metallic_roughness& p = src.pbr_metallic_roughness;
                m.baseColor = Vec4(p.base_color_factor[0], p.base_color_factor[1], p.base_color_factor[2], p.base_color_factor[3]);
                m.metallic = p.metallic_factor;
                m.roughness = p.roughness_factor;
                m.baseColorTexture = textureRef(p.base_color_texture);
                m.metallicRoughnessTexture = textureRef(p.metallic_roughness_texture);
            } else {
                m.metallic = 1.0f;
                m.roughness = 1.0f;
            }
            if (src.has_pbr_specular_glossiness)
                m_out.warnings.push_back("material '" + m.name + "': KHR_materials_pbrSpecularGlossiness is not supported");
            const f32 strength = src.has_emissive_strength ? src.emissive_strength.emissive_strength : 1.0f;
            m.emissive = Vec3(src.emissive_factor[0], src.emissive_factor[1], src.emissive_factor[2]) * strength;
            m.normalTexture = textureRef(src.normal_texture);
            m.normalScale = src.normal_texture.texture ? src.normal_texture.scale : 1.0f;
            m.occlusionTexture = textureRef(src.occlusion_texture);
            m.occlusionStrength = src.occlusion_texture.texture ? src.occlusion_texture.scale : 1.0f;
            m.emissiveTexture = textureRef(src.emissive_texture);
            m.alphaMode = src.alpha_mode == cgltf_alpha_mode_mask ? AlphaMode::Mask
                          : src.alpha_mode == cgltf_alpha_mode_blend ? AlphaMode::Blend
                                                                      : AlphaMode::Opaque;
            m.alphaCutoff = src.alpha_cutoff;
            m.doubleSided = src.double_sided;
            m_out.materials.push_back(std::move(m));
        }
    }

    void readMeshes() {
        std::set<std::string> keys;
        m_meshBase = m_out.meshes.size();
        for (cgltf_size i = 0; i < m_data->meshes_count; ++i) {
            const cgltf_mesh& src = m_data->meshes[i];
            ImportedMesh mesh;
            mesh.name = src.name ? src.name : "mesh" + std::to_string(i);
            mesh.key = import_util::uniqueKey(keys, mesh.name, "mesh" + std::to_string(i));
            for (cgltf_size p = 0; p < src.primitives_count; ++p) readPrimitive(src.primitives[p], mesh);
            m_out.meshes.push_back(std::move(mesh));
        }
    }

    void readPrimitive(const cgltf_primitive& prim, ImportedMesh& mesh) {
        if (prim.type != cgltf_primitive_type_triangles) {
            m_out.warnings.push_back("mesh '" + mesh.name + "': a non-triangle primitive was skipped");
            return;
        }
        if (prim.has_draco_mesh_compression) {
            m_out.warnings.push_back("mesh '" + mesh.name + "': Draco compressed primitive skipped");
            return;
        }
        const cgltf_accessor *position = nullptr, *normal = nullptr, *tangent = nullptr, *uv0 = nullptr;
        for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
            const cgltf_attribute& attr = prim.attributes[a];
            if (attr.type == cgltf_attribute_type_position) position = attr.data;
            else if (attr.type == cgltf_attribute_type_normal) normal = attr.data;
            else if (attr.type == cgltf_attribute_type_tangent) tangent = attr.data;
            else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) uv0 = attr.data;
        }
        if (!position || position->count == 0) {
            m_out.warnings.push_back("mesh '" + mesh.name + "': primitive without positions skipped");
            return;
        }

        // Build the primitive on its own so missing normals/tangents are generated per primitive.
        ImportedMesh local;
        local.vertices.resize(position->count);
        std::vector<f32> values;
        floatArray(position, values);
        for (size_t v = 0; v < local.vertices.size(); ++v)
            local.vertices[v].position = Vec3(values[v * 3], values[v * 3 + 1], values[v * 3 + 2]);
        if (uv0 && uv0->count == position->count && floatArray(uv0, values))
            for (size_t v = 0; v < local.vertices.size(); ++v) local.vertices[v].uv0 = Vec2(values[v * 2], values[v * 2 + 1]);

        if (prim.indices) {
            local.indices.resize(prim.indices->count);
            for (cgltf_size k = 0; k < prim.indices->count; ++k) {
                const cgltf_size index = cgltf_accessor_read_index(prim.indices, k);
                if (index >= position->count) {
                    m_out.warnings.push_back("mesh '" + mesh.name + "': primitive with an out-of-range index skipped");
                    return;
                }
                local.indices[k] = static_cast<u32>(index);
            }
        } else {
            local.indices.resize(position->count);
            for (u32 k = 0; k < position->count; ++k) local.indices[k] = k;
        }
        local.indices.resize(local.indices.size() / 3 * 3);
        if (local.indices.empty()) return;

        if (normal && normal->count == position->count && floatArray(normal, values)) {
            for (size_t v = 0; v < local.vertices.size(); ++v)
                local.vertices[v].normal = Vec3(values[v * 3], values[v * 3 + 1], values[v * 3 + 2]);
        } else {
            import_util::generateNormals(local);
        }
        if (tangent && tangent->count == position->count && floatArray(tangent, values) && cgltf_num_components(tangent->type) == 4) {
            for (size_t v = 0; v < local.vertices.size(); ++v)
                local.vertices[v].tangent = Vec4(values[v * 4], values[v * 4 + 1], values[v * 4 + 2], values[v * 4 + 3]);
        } else if (uv0) {
            import_util::generateTangents(local);
        }

        ImportedSubMesh sub;
        sub.firstIndex = static_cast<u32>(mesh.indices.size());
        sub.indexCount = static_cast<u32>(local.indices.size());
        sub.material = prim.material ? static_cast<i32>(m_materialBase + cgltf_material_index(m_data, prim.material)) : -1;
        const u32 vertexOffset = static_cast<u32>(mesh.vertices.size());
        for (u32 index : local.indices) mesh.indices.push_back(index + vertexOffset);
        mesh.vertices.insert(mesh.vertices.end(), local.vertices.begin(), local.vertices.end());
        mesh.subMeshes.push_back(sub);
    }

    void readNodes() {
        if (m_data->scenes_count == 0 && m_data->nodes_count == 0) return;
        const cgltf_scene* scene = m_data->scene ? m_data->scene : (m_data->scenes_count ? &m_data->scenes[0] : nullptr);
        std::vector<const cgltf_node*> roots;
        if (scene) {
            roots.assign(scene->nodes, scene->nodes + scene->nodes_count);
        } else {
            for (cgltf_size i = 0; i < m_data->nodes_count; ++i)
                if (!m_data->nodes[i].parent) roots.push_back(&m_data->nodes[i]);
        }
        for (const cgltf_node* root : roots) addNode(*root, -1, 0);
    }

    void addNode(const cgltf_node& src, i32 parent, int depth) {
        if (depth > 256) return; // malformed (cyclic) files; cgltf_validate should catch it, this is the backstop
        ImportedNode node;
        node.name = src.name ? src.name : "node" + std::to_string(cgltf_node_index(m_data, &src));
        node.parent = parent;
        if (src.has_matrix) {
            decomposeMatrix(src.matrix, node);
        } else {
            if (src.has_translation) node.position = Vec3(src.translation[0], src.translation[1], src.translation[2]);
            if (src.has_rotation) node.rotation = glm::normalize(Quat(src.rotation[3], src.rotation[0], src.rotation[1], src.rotation[2]));
            if (src.has_scale) node.scale = Vec3(src.scale[0], src.scale[1], src.scale[2]);
        }
        if (src.mesh) node.mesh = static_cast<i32>(m_meshBase + cgltf_mesh_index(m_data, src.mesh));
        if (src.skin) m_out.warnings.push_back("node '" + node.name + "': skinning is not imported yet (static mesh only)");
        const i32 index = static_cast<i32>(m_out.nodes.size());
        m_out.nodes.push_back(std::move(node));
        for (cgltf_size c = 0; c < src.children_count; ++c) addNode(*src.children[c], index, depth + 1);
    }

    fs::path m_dir;
    cgltf_data* m_data;
    cgltf_options m_options;
    ImportedScene& m_out;
    std::map<size_t, i32> m_imageMap;
    std::set<std::string> m_imageKeys;
    size_t m_materialBase = 0;
    size_t m_meshBase = 0;
};

} // namespace

bool ImportGltf(const fs::path& path, ImportedScene& out, std::string& error) {
    std::vector<u8> bytes;
    if (!ReadFileBytes(path, bytes)) {
        error = "cannot read " + path.string();
        return false;
    }

    FileCollector collector;
    cgltf_options options{};
    options.file.read = &readFile;
    options.file.release = &releaseFile;
    options.file.user_data = &collector;

    cgltf_data* data = nullptr;
    if (cgltf_parse(&options, bytes.data(), bytes.size(), &data) != cgltf_result_success) {
        error = "not a valid glTF/GLB file";
        return false;
    }
    struct Guard {
        cgltf_data* data;
        ~Guard() { cgltf_free(data); }
    } guard{data};

    for (cgltf_size i = 0; i < data->extensions_required_count; ++i) {
        const std::string_view ext = data->extensions_required[i];
        if (ext == "KHR_draco_mesh_compression" || ext == "EXT_meshopt_compression" || ext == "KHR_texture_basisu") {
            error = "required glTF extension not supported yet: " + std::string(ext);
            return false;
        }
    }
    const std::string pathUtf8 = PathToUtf8(path);
    if (cgltf_load_buffers(&options, data, pathUtf8.c_str()) != cgltf_result_success) {
        error = "cannot load the glTF buffers (missing .bin?)";
        return false;
    }
    if (cgltf_validate(data) != cgltf_result_success) {
        error = "glTF validation failed (corrupt accessors or indices)";
        return false;
    }

    ImportedScene scene;
    scene.name = path.stem().string();
    GltfReader reader(path, data, options, scene);
    reader.readAll();
    scene.externalFiles.insert(scene.externalFiles.end(), collector.files.begin(), collector.files.end());
    std::sort(scene.externalFiles.begin(), scene.externalFiles.end());
    scene.externalFiles.erase(std::unique(scene.externalFiles.begin(), scene.externalFiles.end()), scene.externalFiles.end());
    out = std::move(scene);
    return true;
}

} // namespace foh
