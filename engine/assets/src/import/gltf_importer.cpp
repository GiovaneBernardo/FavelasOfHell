#include <algorithm>
#include <cmath>
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
        buildSkeletons();
        readMeshes();
        readNodes();
        readClips();
    }

private:
    size_t nodeIndex(const cgltf_node* n) const { return cgltf_node_index(m_data, n); }

    // Skeletons: skins that share a joint are one skeleton (a character split into several skinned meshes has one
    // skin per mesh, all over the same bones). The joint set also holds every ancestor of a joint, so model space is
    // the glTF scene space (exporters hang the bones under "Armature" nodes with scale and rotation).
    i32 makeSkeleton(const std::set<size_t>& nodes, const std::string& name, bool usedBySkin) {
        ImportedSkeleton sk;
        sk.key = m_skeletonKeys.empty() ? "skeleton" : "skeleton#" + std::to_string(m_skeletonKeys.size() + 1);
        m_skeletonKeys.insert(sk.key);
        sk.name = name;
        sk.usedBySkin = usedBySkin;
        const i32 skeletonIndex = static_cast<i32>(m_out.skeletons.size());
        auto visit = [&](auto&& self, const cgltf_node* n, i32 parent) -> void {
            i32 here = parent;
            const size_t index = nodeIndex(n);
            if (nodes.contains(index)) {
                here = static_cast<i32>(sk.parents.size());
                ImportedNode local;
                if (n->has_matrix) {
                    decomposeMatrix(n->matrix, local);
                } else {
                    if (n->has_translation) local.position = Vec3(n->translation[0], n->translation[1], n->translation[2]);
                    if (n->has_rotation) local.rotation = glm::normalize(Quat(n->rotation[3], n->rotation[0], n->rotation[1], n->rotation[2]));
                    if (n->has_scale) local.scale = Vec3(n->scale[0], n->scale[1], n->scale[2]);
                }
                sk.jointNames.push_back(n->name ? n->name : "node" + std::to_string(index));
                sk.parents.push_back(parent);
                sk.restPosition.push_back(local.position);
                sk.restRotation.push_back(local.rotation);
                sk.restScale.push_back(local.scale);
                sk.inverseBind.push_back(Mat4(1.0f));
                sk.hasInverseBind.push_back(0);
                m_nodeSkeleton[index] = skeletonIndex;
                m_nodeJoint[index] = here;
            }
            for (cgltf_size c = 0; c < n->children_count; ++c) self(self, n->children[c], here);
        };
        for (cgltf_size i = 0; i < m_data->nodes_count; ++i)
            if (!m_data->nodes[i].parent) visit(visit, &m_data->nodes[i], -1);
        m_out.skeletons.push_back(std::move(sk));
        return skeletonIndex;
    }

    void buildSkeletons() {
        m_nodeSkeleton.assign(m_data->nodes_count, -1);
        m_nodeJoint.assign(m_data->nodes_count, -1);
        m_skinSkeleton.assign(m_data->skins_count, -1);
        m_skinJoint.resize(m_data->skins_count);
        m_meshSkin.assign(m_data->meshes_count, -1);
        for (cgltf_size n = 0; n < m_data->nodes_count; ++n) {
            const cgltf_node& node = m_data->nodes[n];
            if (node.mesh && node.skin) {
                const size_t mesh = cgltf_mesh_index(m_data, node.mesh);
                if (m_meshSkin[mesh] < 0) m_meshSkin[mesh] = static_cast<i32>(cgltf_skin_index(m_data, node.skin));
            }
        }
        if (m_data->skins_count == 0) return;

        // Union-find over skins that share a joint node.
        std::vector<size_t> parent(m_data->skins_count);
        for (size_t i = 0; i < parent.size(); ++i) parent[i] = i;
        auto find = [&](size_t x) {
            while (parent[x] != x) x = parent[x] = parent[parent[x]];
            return x;
        };
        std::map<size_t, size_t> firstSkinOfNode;
        for (cgltf_size k = 0; k < m_data->skins_count; ++k)
            for (cgltf_size j = 0; j < m_data->skins[k].joints_count; ++j) {
                const auto [it, inserted] = firstSkinOfNode.try_emplace(nodeIndex(m_data->skins[k].joints[j]), k);
                if (!inserted) parent[find(k)] = find(it->second);
            }
        std::map<size_t, std::vector<size_t>> groups;
        for (size_t k = 0; k < m_data->skins_count; ++k) groups[find(k)].push_back(k);

        for (const auto& [rootSkin, skins] : groups) {
            std::set<size_t> nodes;
            for (size_t k : skins)
                for (cgltf_size j = 0; j < m_data->skins[k].joints_count; ++j)
                    for (const cgltf_node* n = m_data->skins[k].joints[j]; n; n = n->parent) nodes.insert(nodeIndex(n));
            const cgltf_skin& first = m_data->skins[skins.front()];
            const i32 skeleton = makeSkeleton(nodes, first.name ? first.name : m_out.name + " skeleton", true);
            ImportedSkeleton& sk = m_out.skeletons[static_cast<size_t>(skeleton)];
            for (size_t k : skins) {
                const cgltf_skin& skin = m_data->skins[k];
                m_skinSkeleton[k] = skeleton;
                for (cgltf_size j = 0; j < skin.joints_count; ++j) {
                    const i32 joint = m_nodeJoint[nodeIndex(skin.joints[j])];
                    m_skinJoint[k].push_back(joint);
                    if (joint < 0 || !skin.inverse_bind_matrices || j >= skin.inverse_bind_matrices->count) continue;
                    f32 m[16];
                    cgltf_accessor_read_float(skin.inverse_bind_matrices, j, m, 16);
                    const Mat4 ibm = glm::make_mat4(m);
                    if (!sk.hasInverseBind[static_cast<size_t>(joint)]) {
                        sk.inverseBind[static_cast<size_t>(joint)] = ibm;
                        sk.hasInverseBind[static_cast<size_t>(joint)] = 1;
                    } else if (glm::length(Vec3(sk.inverseBind[static_cast<size_t>(joint)][3] - ibm[3])) > 0.01f) {
                        m_out.warnings.push_back("skin '" + std::string(skin.name ? skin.name : "?") + "': joint '" +
                                                 sk.jointNames[static_cast<size_t>(joint)] + "' has a different bind pose than another skin on the same skeleton");
                    }
                }
            }
        }
    }

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
            if (import_util::probeImage(encoded.data(), encoded.size(), error)) {
                image.encoded = std::move(encoded);
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
            m_currentSkin = m_meshSkin[i] >= 0 && m_skinSkeleton[static_cast<size_t>(m_meshSkin[i])] >= 0 ? m_meshSkin[i] : -1;
            if (m_currentSkin >= 0) {
                mesh.skeleton = m_skinSkeleton[static_cast<size_t>(m_currentSkin)];
                m_out.skeletons[static_cast<size_t>(mesh.skeleton)].usedBySkin = true;
            }
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
        const cgltf_accessor *joints0 = nullptr, *weights0 = nullptr;
        for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
            const cgltf_attribute& attr = prim.attributes[a];
            if (attr.type == cgltf_attribute_type_joints && attr.index == 0) joints0 = attr.data;
            else if (attr.type == cgltf_attribute_type_weights && attr.index == 0) weights0 = attr.data;
            else if ((attr.type == cgltf_attribute_type_joints || attr.type == cgltf_attribute_type_weights) && attr.index > 0 && !m_warnedInfluences) {
                m_warnedInfluences = true;
                m_out.warnings.push_back("mesh '" + mesh.name + "': more than four joint influences per vertex, the strongest four are kept");
            }
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

        if (m_currentSkin >= 0) {
            const std::vector<i32>& map = m_skinJoint[static_cast<size_t>(m_currentSkin)];
            const bool usable = joints0 && weights0 && joints0->count == position->count && weights0->count == position->count;
            u32 unweighted = 0;
            for (size_t v = 0; v < local.vertices.size(); ++v) {
                u32 joints[4] = {0, 0, 0, 0};
                f32 weights[4] = {0, 0, 0, 0};
                if (usable) {
                    cgltf_uint j[4] = {0, 0, 0, 0};
                    cgltf_accessor_read_uint(joints0, v, j, 4);
                    cgltf_accessor_read_float(weights0, v, weights, 4);
                    for (int k = 0; k < 4; ++k) {
                        const i32 mapped = j[k] < map.size() ? map[j[k]] : -1;
                        if (mapped < 0) weights[k] = 0.0f;
                        joints[k] = mapped < 0 ? 0u : static_cast<u32>(mapped);
                    }
                }
                ImportedVertex& vert = local.vertices[v];
                if (!import_util::packSkinInfluences(joints, weights, vert.joints, vert.weights)) {
                    vert.joints[0] = 0;
                    vert.weights[0] = 255;
                    ++unweighted;
                }
            }
            if (unweighted > 0)
                m_out.warnings.push_back("mesh '" + mesh.name + "': " + std::to_string(unweighted) + " vertices without skin weights follow the first joint");
        }

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
        for (const cgltf_node* root : roots) addNode(*root, -1, 0, false);
    }

    void addNode(const cgltf_node& src, i32 parent, int depth, bool underJoint) {
        if (depth > 256) return; // malformed (cyclic) files; cgltf_validate should catch it, this is the backstop
        // Bones (and the group nodes above them) are described by the skeleton asset, not by prefab nodes. Skinned
        // meshes below them are placed by the skeleton and re-rooted; anything else cannot follow the animation.
        const bool isJoint = m_nodeJoint[nodeIndex(&src)] >= 0;
        if (isJoint || underJoint) {
            if (isJoint || !(src.mesh && src.skin)) {
                if (src.mesh && !src.skin)
                    m_out.warnings.push_back("mesh node '" + std::string(src.name ? src.name : "?") + "' hangs below a bone; attachments are not imported yet");
                for (cgltf_size c = 0; c < src.children_count; ++c) addNode(*src.children[c], -1, depth + 1, true);
                return;
            }
        }
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
        // A skinned mesh is placed by its skeleton, not by its node (glTF: the node transform is ignored for skinning).
        if (src.skin && node.mesh >= 0 && m_out.meshes[static_cast<size_t>(node.mesh)].skeleton >= 0) {
            node.parent = -1;
            node.position = Vec3(0.0f);
            node.rotation = Quat(1.0f, 0.0f, 0.0f, 0.0f);
            node.scale = Vec3(1.0f);
        }
        const i32 index = static_cast<i32>(m_out.nodes.size());
        m_out.nodes.push_back(std::move(node));
        for (cgltf_size c = 0; c < src.children_count; ++c) addNode(*src.children[c], index, depth + 1, underJoint);
    }

    // ---- animation ----

    struct Curve {
        std::vector<f32> times;
        std::vector<f32> values; // components per key: 3 or 4 (cubic spline: 3 * that, per key: in-tangent, value, out-tangent)
        cgltf_interpolation_type interpolation = cgltf_interpolation_type_linear;
        u32 components = 3;
        bool valid() const { return !times.empty() && values.size() >= times.size() * components; }

        // Value at time t in `out` (components entries).
        void evaluate(f32 t, f32* out) const {
            const size_t n = times.size();
            const bool cubic = interpolation == cgltf_interpolation_type_cubic_spline;
            const size_t stride = cubic ? components * 3 : components;
            const size_t valueOffset = cubic ? components : 0;
            auto key = [&](size_t i, size_t part) { return values.data() + i * stride + part; };
            if (n == 1 || t <= times.front()) {
                std::copy_n(key(0, valueOffset), components, out);
                return;
            }
            if (t >= times.back()) {
                std::copy_n(key(n - 1, valueOffset), components, out);
                return;
            }
            const size_t hi = static_cast<size_t>(std::upper_bound(times.begin(), times.end(), t) - times.begin());
            const size_t lo = hi - 1;
            const f32 span = times[hi] - times[lo];
            const f32 u = span > 0.0f ? (t - times[lo]) / span : 0.0f;
            if (interpolation == cgltf_interpolation_type_step) {
                std::copy_n(key(lo, valueOffset), components, out);
            } else if (cubic) {
                const f32 u2 = u * u, u3 = u2 * u;
                for (u32 c = 0; c < components; ++c) {
                    const f32 p0 = key(lo, components)[c], m0 = key(lo, components * 2)[c] * span;
                    const f32 p1 = key(hi, components)[c], m1 = key(hi, 0)[c] * span;
                    out[c] = (2 * u3 - 3 * u2 + 1) * p0 + (u3 - 2 * u2 + u) * m0 + (-2 * u3 + 3 * u2) * p1 + (u3 - u2) * m1;
                }
            } else if (components == 4) {
                const f32* a = key(lo, 0);
                const f32* b = key(hi, 0);
                const Quat q = glm::slerp(glm::normalize(Quat(a[3], a[0], a[1], a[2])), glm::normalize(Quat(b[3], b[0], b[1], b[2])), u);
                out[0] = q.x, out[1] = q.y, out[2] = q.z, out[3] = q.w;
            } else {
                for (u32 c = 0; c < components; ++c) out[c] = key(lo, 0)[c] + (key(hi, 0)[c] - key(lo, 0)[c]) * u;
            }
        }
    };

    bool readCurve(const cgltf_animation_sampler& sampler, u32 components, Curve& out) {
        out.interpolation = sampler.interpolation;
        out.components = components;
        if (!sampler.input || !sampler.output) return false;
        if (!floatArray(sampler.input, out.times) || !floatArray(sampler.output, out.values)) return false;
        return out.valid();
    }

    void readClips() {
        if (m_data->animations_count == 0) return;

        // A file with animations but no skin (a Mixamo-style "animation only" export): the animated nodes and their
        // ancestors are the skeleton. It becomes an asset only if a mesh uses it, which cannot happen here.
        if (m_out.skeletons.empty()) {
            std::set<size_t> nodes;
            for (cgltf_size a = 0; a < m_data->animations_count; ++a)
                for (cgltf_size c = 0; c < m_data->animations[a].channels_count; ++c)
                    for (const cgltf_node* n = m_data->animations[a].channels[c].target_node; n; n = n->parent) nodes.insert(nodeIndex(n));
            if (nodes.empty()) return;
            makeSkeleton(nodes, m_out.name + " skeleton", false);
        }

        std::set<std::string> keys;
        for (cgltf_size a = 0; a < m_data->animations_count; ++a) {
            const cgltf_animation& anim = m_data->animations[a];
            // The skeleton is the one of the first channel that targets a joint.
            i32 skeleton = -1;
            for (cgltf_size c = 0; c < anim.channels_count && skeleton < 0; ++c)
                if (anim.channels[c].target_node) skeleton = m_nodeSkeleton[nodeIndex(anim.channels[c].target_node)];
            if (skeleton < 0) continue;
            const ImportedSkeleton& sk = m_out.skeletons[static_cast<size_t>(skeleton)];

            f32 duration = 0.0f;
            for (cgltf_size c = 0; c < anim.channels_count; ++c) {
                const cgltf_animation_sampler* sampler = anim.channels[c].sampler;
                if (!sampler || !sampler->input || sampler->input->count == 0) continue;
                f32 last = 0.0f;
                cgltf_accessor_read_float(sampler->input, sampler->input->count - 1, &last, 1);
                duration = std::max(duration, last);
            }
            ImportedClip clip;
            clip.name = anim.name && *anim.name ? anim.name : (m_data->animations_count == 1 ? m_out.name : "animation" + std::to_string(a));
            clip.key = import_util::uniqueKey(keys, clip.name, "clip" + std::to_string(a));
            clip.skeleton = skeleton;
            clip.frameRate = 30.0f;
            clip.frameCount = static_cast<u32>(std::llround(duration * clip.frameRate)) + 1;
            clip.tracks.resize(sk.parents.size());
            u32 skipped = 0;
            for (cgltf_size c = 0; c < anim.channels_count; ++c) {
                const cgltf_animation_channel& channel = anim.channels[c];
                if (!channel.target_node || !channel.sampler) continue;
                const size_t node = nodeIndex(channel.target_node);
                if (m_nodeSkeleton[node] != skeleton) {
                    ++skipped;
                    continue;
                }
                ImportedClipTrack& track = clip.tracks[static_cast<size_t>(m_nodeJoint[node])];
                const bool rotation = channel.target_path == cgltf_animation_path_type_rotation;
                const bool translation = channel.target_path == cgltf_animation_path_type_translation;
                const bool scale = channel.target_path == cgltf_animation_path_type_scale;
                if (!rotation && !translation && !scale) {
                    ++skipped; // morph target weights
                    continue;
                }
                Curve curve;
                if (!readCurve(*channel.sampler, rotation ? 4u : 3u, curve)) {
                    ++skipped;
                    continue;
                }
                for (u32 f = 0; f < clip.frameCount; ++f) {
                    f32 v[4];
                    curve.evaluate(static_cast<f32>(f) / clip.frameRate, v);
                    if (rotation) {
                        Quat q(v[3], v[0], v[1], v[2]);
                        track.rotation.push_back(glm::normalize(q));
                    } else if (translation) {
                        track.translation.push_back(Vec3(v[0], v[1], v[2]));
                    } else {
                        track.scale.push_back(Vec3(v[0], v[1], v[2]));
                    }
                }
            }
            if (skipped > 0)
                m_out.warnings.push_back("animation '" + clip.name + "': " + std::to_string(skipped) + " channels skipped (not on the skeleton, or morph targets)");
            m_out.clips.push_back(std::move(clip));
        }
    }

    fs::path m_dir;
    cgltf_data* m_data;
    cgltf_options m_options;
    ImportedScene& m_out;
    std::map<size_t, i32> m_imageMap;
    std::set<std::string> m_imageKeys;
    size_t m_materialBase = 0;
    size_t m_meshBase = 0;
    std::vector<i32> m_nodeSkeleton, m_nodeJoint;  // glTF node -> imported skeleton / joint index, -1 if none
    std::vector<i32> m_skinSkeleton;               // glTF skin -> imported skeleton
    std::vector<std::vector<i32>> m_skinJoint;     // glTF skin -> (skin joint index -> skeleton joint index)
    std::vector<i32> m_meshSkin;                   // glTF mesh -> the skin of the first node that draws it with one
    std::set<std::string> m_skeletonKeys;
    i32 m_currentSkin = -1;
    bool m_warnedInfluences = false;
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
