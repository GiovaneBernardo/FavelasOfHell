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
Quat toQuat(ufbx_quat q) { return Quat(static_cast<f32>(q.w), static_cast<f32>(q.x), static_cast<f32>(q.y), static_cast<f32>(q.z)); }
Mat4 toMat4(const ufbx_matrix& m) {
    return Mat4(Vec4(static_cast<f32>(m.m00), static_cast<f32>(m.m10), static_cast<f32>(m.m20), 0.0f),
                Vec4(static_cast<f32>(m.m01), static_cast<f32>(m.m11), static_cast<f32>(m.m21), 0.0f),
                Vec4(static_cast<f32>(m.m02), static_cast<f32>(m.m12), static_cast<f32>(m.m22), 0.0f),
                Vec4(static_cast<f32>(m.m03), static_cast<f32>(m.m13), static_cast<f32>(m.m23), 1.0f));
}
std::string str(ufbx_string s) { return std::string(s.data, s.length); }

constexpr f32 kClipFrameRate = 30.0f;

// Value of a baked key list at time t (linear between keys, clamped at the ends).
template <class Key>
size_t keyAfter(const Key* keys, size_t count, double t) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (keys[mid].time <= t) lo = mid + 1;
        else hi = mid;
    }
    return lo; // first key with time > t
}
Vec3 sampleBakedVec(const ufbx_baked_vec3_list& list, double t) {
    if (list.count == 0) return Vec3(0.0f);
    const size_t i = keyAfter(list.data, list.count, t);
    if (i == 0) return toVec3(list.data[0].value);
    if (i == list.count) return toVec3(list.data[list.count - 1].value);
    const ufbx_baked_vec3 &a = list.data[i - 1], &b = list.data[i];
    const f32 f = b.time > a.time ? static_cast<f32>((t - a.time) / (b.time - a.time)) : 0.0f;
    return glm::mix(toVec3(a.value), toVec3(b.value), f);
}
Quat sampleBakedQuat(const ufbx_baked_quat_list& list, double t) {
    if (list.count == 0) return Quat(1.0f, 0.0f, 0.0f, 0.0f);
    const size_t i = keyAfter(list.data, list.count, t);
    if (i == 0) return glm::normalize(toQuat(list.data[0].value));
    if (i == list.count) return glm::normalize(toQuat(list.data[list.count - 1].value));
    const ufbx_baked_quat &a = list.data[i - 1], &b = list.data[i];
    const f32 f = b.time > a.time ? static_cast<f32>((t - a.time) / (b.time - a.time)) : 0.0f;
    return glm::normalize(glm::slerp(glm::normalize(toQuat(a.value)), glm::normalize(toQuat(b.value)), f));
}

class FbxReader {
public:
    FbxReader(const fs::path& path, const ufbx_scene& scene, ImportedScene& out)
        : m_dir(path.parent_path()), m_scene(scene), m_out(out) {}

    void readAll(const fs::path& path) {
        readMaterials();
        buildSkeleton();
        for (ufbx_node* root_child : m_scene.root_node->children) addNode(*root_child, -1, 0);
        readClips(path);
    }

private:
    // One skeleton per file: every bone, every bone a skin refers to, and all their ancestors (a non-bone "Armature"
    // parent carries a transform too, and model space is the scene world space). Joints are in parents-first order.
    void buildSkeleton() {
        const size_t nodeCount = m_scene.nodes.count;
        std::vector<char> isJoint(nodeCount, 0);
        auto markWithAncestors = [&](const ufbx_node* n) {
            for (; n && !n->is_root; n = n->parent) isJoint[n->typed_id] = 1;
        };
        for (size_t i = 0; i < nodeCount; ++i)
            if (m_scene.nodes.data[i]->bone) markWithAncestors(m_scene.nodes.data[i]);
        for (size_t i = 0; i < m_scene.skin_deformers.count; ++i)
            for (const ufbx_skin_cluster* c : m_scene.skin_deformers.data[i]->clusters) markWithAncestors(c->bone_node);

        m_nodeJoint.assign(nodeCount, -1);
        ImportedSkeleton sk;
        sk.key = "skeleton";
        sk.name = m_out.name + " skeleton";
        auto visit = [&](auto&& self, const ufbx_node* n, i32 parent) -> void {
            i32 here = parent;
            if (isJoint[n->typed_id]) {
                here = static_cast<i32>(sk.parents.size());
                m_nodeJoint[n->typed_id] = here;
                sk.jointNames.push_back(str(n->name));
                sk.parents.push_back(parent);
                sk.restPosition.push_back(toVec3(n->local_transform.translation));
                sk.restRotation.push_back(glm::normalize(toQuat(n->local_transform.rotation)));
                sk.restScale.push_back(toVec3(n->local_transform.scale));
                sk.inverseBind.push_back(Mat4(1.0f));
                sk.hasInverseBind.push_back(0);
            }
            for (const ufbx_node* child : n->children) self(self, child, here);
        };
        for (const ufbx_node* child : m_scene.root_node->children) visit(visit, child, -1);
        if (!sk.parents.empty()) {
            m_skeleton = 0;
            m_out.skeletons.push_back(std::move(sk));
        }
    }

    // Weights and joints of one mesh vertex from its skin.
    void skinVertex(const ufbx_skin_deformer& skin, const std::vector<i32>& clusterJoint, u32 vertex, ImportedVertex& out) {
        u32 joints[4] = {0, 0, 0, 0};
        f32 weights[4] = {0, 0, 0, 0};
        if (vertex < skin.vertices.count) {
            const ufbx_skin_vertex sv = skin.vertices.data[vertex];
            u32 n = 0;
            for (u32 k = 0; k < sv.num_weights && n < 4; ++k) {
                const ufbx_skin_weight w = skin.weights.data[sv.weight_begin + k];
                const i32 joint = w.cluster_index < clusterJoint.size() ? clusterJoint[w.cluster_index] : -1;
                if (joint < 0 || w.weight <= 0.0) continue;
                joints[n] = static_cast<u32>(joint);
                weights[n++] = static_cast<f32>(w.weight);
            }
        }
        if (!import_util::packSkinInfluences(joints, weights, out.joints, out.weights)) {
            out.joints[0] = 0; // unweighted vertex: follow the first joint rather than staying behind
            out.weights[0] = 255;
            ++m_unweighted;
        }
    }

    void readClips(const fs::path& path) {
        if (m_skeleton < 0) return;
        const ImportedSkeleton& sk = m_out.skeletons[static_cast<size_t>(m_skeleton)];
        // Stacks that animate nothing, or only a single pose (Mixamo keeps the T-pose as a one-frame take), are not clips.
        struct Baked {
            std::string name;
            ufbx_baked_anim* anim;
        };
        std::vector<Baked> baked;
        for (const ufbx_anim_stack* stack : m_scene.anim_stacks) {
            ufbx_bake_opts opts = {};
            opts.trim_start_time = true;
            opts.resample_rate = kClipFrameRate;
            ufbx_error error;
            ufbx_baked_anim* anim = ufbx_bake_anim(&m_scene, stack->anim, &opts, &error);
            if (!anim) continue;
            size_t joints = 0;
            for (const ufbx_baked_node& n : anim->nodes) joints += m_nodeJoint[n.typed_id] >= 0;
            if (joints == 0 || anim->playback_duration < 1.5 / kClipFrameRate) {
                ufbx_free_baked_anim(anim);
                continue;
            }
            baked.push_back({str(stack->name), anim});
        }
        std::set<std::string> keys;
        for (const Baked& b : baked) {
            ImportedClip clip;
            // "mixamo.com" in every file says nothing; a file with one take is named after the file.
            clip.name = baked.size() == 1 || b.name.empty() ? path.stem().string() : b.name;
            clip.key = import_util::uniqueKey(keys, clip.name, "clip");
            clip.skeleton = m_skeleton;
            clip.frameRate = kClipFrameRate;
            clip.frameCount = static_cast<u32>(std::llround(b.anim->playback_duration * kClipFrameRate)) + 1;
            clip.tracks.resize(sk.parents.size());
            for (const ufbx_baked_node& bn : b.anim->nodes) {
                const i32 joint = m_nodeJoint[bn.typed_id];
                if (joint < 0) continue;
                ImportedClipTrack& track = clip.tracks[static_cast<size_t>(joint)];
                track.translation.resize(clip.frameCount);
                track.rotation.resize(clip.frameCount);
                track.scale.resize(clip.frameCount);
                for (u32 f = 0; f < clip.frameCount; ++f) {
                    const double t = static_cast<double>(f) / kClipFrameRate;
                    track.translation[f] = sampleBakedVec(bn.translation_keys, t);
                    track.rotation[f] = sampleBakedQuat(bn.rotation_keys, t);
                    track.scale[f] = sampleBakedVec(bn.scale_keys, t);
                }
            }
            m_out.clips.push_back(std::move(clip));
        }
        for (Baked& b : baked) ufbx_free_baked_anim(b.anim);
    }

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

        // Skin: map the deformer clusters to skeleton joints and record the inverse bind matrices.
        const ufbx_skin_deformer* skin = m_skeleton >= 0 && mesh.skin_deformers.count > 0 ? mesh.skin_deformers.data[0] : nullptr;
        std::vector<i32> clusterJoint;
        if (skin) {
            ImportedSkeleton& sk = m_out.skeletons[static_cast<size_t>(m_skeleton)];
            clusterJoint.assign(skin->clusters.count, -1);
            for (size_t c = 0; c < skin->clusters.count; ++c) {
                const ufbx_skin_cluster& cluster = *skin->clusters.data[c];
                if (!cluster.bone_node) continue;
                const i32 joint = m_nodeJoint[cluster.bone_node->typed_id];
                if (joint < 0) continue;
                clusterJoint[c] = joint;
                const Mat4 ibm = toMat4(cluster.geometry_to_bone);
                if (!sk.hasInverseBind[static_cast<size_t>(joint)]) {
                    sk.inverseBind[static_cast<size_t>(joint)] = ibm;
                    sk.hasInverseBind[static_cast<size_t>(joint)] = 1;
                } else if (glm::length(Vec3(sk.inverseBind[static_cast<size_t>(joint)][3] - ibm[3])) > 0.01f) {
                    m_out.warnings.push_back("mesh '" + out.name + "': joint '" + sk.jointNames[static_cast<size_t>(joint)] +
                                             "' has a different bind pose than another skin on the same skeleton");
                }
            }
            sk.usedBySkin = true;
            out.skeleton = m_skeleton;
        }

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
                if (skin) skinVertex(*skin, clusterJoint, mesh.vertex_indices.data[corner], v);
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

        if (m_unweighted > 0) {
            m_out.warnings.push_back("mesh '" + out.name + "': " + std::to_string(m_unweighted) +
                                     " vertices without skin weights follow the first joint");
            m_unweighted = 0;
        }
        import_util::weldVertices(out);
        if (!mesh.vertex_normal.exists) import_util::generateNormals(out);
        if (mesh.vertex_uv.exists && !(mesh.vertex_tangent.exists && mesh.vertex_bitangent.exists)) import_util::generateTangents(out);

        const i32 index = static_cast<i32>(m_out.meshes.size());
        m_out.meshes.push_back(std::move(out));
        m_meshIndex[&mesh] = index;
        return index;
    }

    void addNode(const ufbx_node& src, i32 parent, int depth, bool underJoint = false) {
        if (depth > 256) return;
        // Bones are not nodes of the prefab (the skeleton asset describes them), and neither are the group nodes
        // above them. Skinned meshes below are placed by the skeleton and re-rooted; anything else cannot follow it.
        if (!m_nodeJoint.empty() && m_nodeJoint[src.typed_id] >= 0) {
            for (ufbx_node* child : src.children) addNode(*child, -1, depth + 1, true);
            return;
        }
        if (underJoint && !(src.mesh && src.mesh->skin_deformers.count > 0)) {
            if (src.mesh) m_out.warnings.push_back("mesh '" + str(src.name) + "' hangs below a bone; attachments are not imported yet");
            for (ufbx_node* child : src.children) addNode(*child, -1, depth + 1, true);
            return;
        }
        ImportedNode node;
        node.name = src.name.length ? std::string(src.name.data, src.name.length) : "node" + std::to_string(m_out.nodes.size());
        node.parent = parent;
        node.position = toVec3(src.local_transform.translation);
        node.rotation = Quat(static_cast<f32>(src.local_transform.rotation.w), static_cast<f32>(src.local_transform.rotation.x),
                             static_cast<f32>(src.local_transform.rotation.y), static_cast<f32>(src.local_transform.rotation.z));
        node.scale = toVec3(src.local_transform.scale);
        if (src.mesh) node.mesh = meshFor(*src.mesh);
        // A skinned mesh is placed by its skeleton, not by its node: the bind pose already holds the transform.
        if (node.mesh >= 0 && m_out.meshes[static_cast<size_t>(node.mesh)].skeleton >= 0) {
            node.parent = -1;
            node.position = Vec3(0.0f);
            node.rotation = Quat(1.0f, 0.0f, 0.0f, 0.0f);
            node.scale = Vec3(1.0f);
        }
        const i32 index = static_cast<i32>(m_out.nodes.size());
        m_out.nodes.push_back(std::move(node));
        for (ufbx_node* child : src.children) addNode(*child, index, depth + 1, underJoint);
    }

    fs::path m_dir;
    const ufbx_scene& m_scene;
    ImportedScene& m_out;
    std::map<const ufbx_texture*, i32> m_imageMap;
    std::map<const ufbx_material*, i32> m_materialIndex;
    std::map<const ufbx_mesh*, i32> m_meshIndex;
    std::set<std::string> m_imageKeys, m_meshKeys;
    std::vector<i32> m_nodeJoint; // scene node (typed id) -> skeleton joint, -1 if none
    i32 m_skeleton = -1;
    u32 m_unweighted = 0;
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
    reader.readAll(path);
    std::sort(imported.externalFiles.begin(), imported.externalFiles.end());
    imported.externalFiles.erase(std::unique(imported.externalFiles.begin(), imported.externalFiles.end()), imported.externalFiles.end());
    out = std::move(imported);
    return true;
}

} // namespace foh
