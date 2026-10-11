// The character tab of the Asset Forge: checks a skinned model against the humanoid the animation library speaks, and rigs an
// unskinned one automatically (humanoid_rig.h), the Mixamo way: place the joints, get a skinned character that already walks.
#include <algorithm>
#include <cmath>
#include <format>
#include <unordered_map>

#include <cstring>

#include <imgui.h>
#include <ImGuizmo.h>

#include "asset_forge.h"
#include "foh/log.h"
#include "foh/utf8_path.h"

namespace foh {

namespace fs = std::filesystem;

namespace {

ImU32 jointColor(u32 index) {
    const char* name = HumanoidJointName(index);
    if (std::string_view(name).starts_with("Left")) return IM_COL32(90, 190, 255, 255);
    if (std::string_view(name).starts_with("Right")) return IM_COL32(255, 120, 110, 255);
    return IM_COL32(235, 235, 235, 255);
}

// The joint on the other side of the body (LeftArm <-> RightArm), -1 for the centre line.
int mirrorOf(u32 index) {
    std::string name = HumanoidJointName(index);
    if (name.starts_with("Left")) name = "Right" + name.substr(4);
    else if (name.starts_with("Right")) name = "Left" + name.substr(5);
    else return -1;
    return HumanoidJointIndex(name);
}

} // namespace

// ---- skinned models ----------------------------------------------------------------------------------------------------

void AssetForge::checkSkeleton() {
    m_skinnedModel = m_work.skeleton.isValid();
    m_skeletonReport.clear();
    m_skeletonMatched = 0;
    if (!m_skinnedModel) return;
    SkeletonAsset sk;
    if (!m_db->load(m_work.skeleton, sk)) {
        m_skeletonReport = "The skeleton could not be read.";
        return;
    }
    std::string missing;
    for (u32 i = 0; i < kHumanoidJointCount; ++i) {
        if (sk.findJoint(HumanoidJointName(i)) >= 0) ++m_skeletonMatched;
        else if (i != 6 && i != 19 && i != 24) missing += std::string(missing.empty() ? "" : ", ") + HumanoidJointName(i); // the end bones are optional
    }
    m_skeletonReport = std::format("{} joints; {} of the {} humanoid joints found by name.", sk.jointCount(), m_skeletonMatched, kHumanoidJointCount);
    if (!missing.empty()) m_skeletonReport += " Missing: " + missing + ".";
}

void AssetForge::placeLandmarks() {
    if (!m_haveBounds) return;
    // Feet on the ground, centred: the rig and the animations assume the origin between the feet.
    Vec3 lo, hi;
    boundsOfModel(lo, hi);
    if (std::abs(lo.y) > 0.02f || std::abs(lo.x + hi.x) > 0.1f) centerAndGround();
    boundsOfModel(lo, hi);
    // The vertices tell whether the arms are out or hanging (every third one is plenty).
    std::vector<Vec3> points;
    for (size_t ni = 0; ni < m_work.nodes.size(); ++ni) {
        const PrefabNode& node = m_work.nodes[ni];
        const auto it = node.mesh.isValid() ? m_meshes.find(node.mesh) : m_meshes.end();
        if (it == m_meshes.end()) continue;
        const MeshAsset& mesh = it->second;
        const VertexStream* pos = mesh.findStream(VertexSemantic::Position);
        if (!pos || pos->format != VertexFormat::Float3 || mesh.vertexStride == 0) continue;
        for (u32 v = 0; v < mesh.vertexCount(); v += 3) {
            Vec3 p;
            std::memcpy(&p, mesh.vertexData.data() + size_t(v) * mesh.vertexStride + pos->offset, sizeof(Vec3));
            points.push_back(Vec3(m_nodes[ni].world * Vec4(p, 1.0f)));
        }
    }
    m_character.landmarks = DefaultHumanoidLandmarks(lo, hi, points);
    m_character.hasLandmarks = true;
    m_characterDirty = true;
}

// ---- rigging -------------------------------------------------------------------------------------------------------------

void AssetForge::startRig() {
    if (m_rig || !m_meshesReady) return;
    if (!m_character.hasLandmarks) placeLandmarks();
    auto task = std::make_unique<RigTask>();
    HumanoidRigInput& in = task->input;
    in.landmarks = m_character.landmarks;
    in.normalizeTPose = m_character.normalizeTPose;
    in.smoothness = m_character.skinSmoothness;
    m_rigSources.clear();

    // Materials: one per distinct material of the model, with its factors so the rigged model looks right even before the
    // variant puts the real (textured) materials back.
    std::unordered_map<AssetId, u32, AssetIdHash> materialIndex;
    auto materialFor = [&](AssetId id) -> u32 {
        const auto it = materialIndex.find(id);
        if (it != materialIndex.end()) return it->second;
        RigMaterial rm;
        rm.name = std::format("material{}", in.materials.size());
        MaterialAsset ma;
        if (id.isValid() && m_db->load(id, ma)) {
            rm.name = std::format("{}_{}", rm.name, ma.name.empty() ? "m" : ma.name);
            for (char& c : rm.name)
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
            rm.baseColor = ma.baseColor;
            rm.metallic = ma.metallic;
            rm.roughness = ma.roughness;
        }
        const u32 index = static_cast<u32>(in.materials.size());
        in.materials.push_back(rm);
        materialIndex[id] = index;
        return index;
    };

    std::unordered_map<std::string, int> usedNames;
    for (size_t ni = 0; ni < m_work.nodes.size(); ++ni) {
        const PrefabNode& node = m_work.nodes[ni];
        if (!node.mesh.isValid()) continue;
        const auto it = m_meshes.find(node.mesh);
        if (it == m_meshes.end()) continue;
        const MeshAsset& mesh = it->second;
        const VertexStream* pos = mesh.findStream(VertexSemantic::Position);
        if (!pos || pos->format != VertexFormat::Float3 || mesh.vertexStride == 0) continue;
        const VertexStream* nrm = mesh.findStream(VertexSemantic::Normal);
        const VertexStream* tan = mesh.findStream(VertexSemantic::Tangent);
        const VertexStream* uv = mesh.findStream(VertexSemantic::Uv0);

        RigMesh out;
        out.name = node.name.empty() ? std::format("mesh{}", ni) : node.name;
        if (const int n = usedNames[out.name]++; n > 0) out.name += std::format("_{}", n);
        const Mat4& world = m_nodes[ni].world;
        const Mat3 normalMatrix = glm::transpose(glm::inverse(Mat3(world)));
        const bool mirrored = glm::determinant(Mat3(world)) < 0.0f;
        const u32 count = mesh.vertexCount();
        out.positions.resize(count);
        if (nrm && nrm->format == VertexFormat::Float3) out.normals.resize(count);
        if (tan && tan->format == VertexFormat::Float4) out.tangents.resize(count);
        if (uv && uv->format == VertexFormat::Float2) out.uvs.resize(count);
        for (u32 v = 0; v < count; ++v) {
            const u8* base = mesh.vertexData.data() + size_t(v) * mesh.vertexStride;
            Vec3 p;
            std::memcpy(&p, base + pos->offset, sizeof(Vec3));
            out.positions[v] = Vec3(world * Vec4(p, 1.0f));
            if (!out.normals.empty()) {
                Vec3 n;
                std::memcpy(&n, base + nrm->offset, sizeof(Vec3));
                out.normals[v] = glm::normalize(normalMatrix * n);
            }
            if (!out.tangents.empty()) {
                Vec4 t;
                std::memcpy(&t, base + tan->offset, sizeof(Vec4));
                out.tangents[v] = Vec4(glm::normalize(Mat3(world) * Vec3(t)), mirrored ? -t.w : t.w);
            }
            if (!out.uvs.empty()) std::memcpy(&out.uvs[v], base + uv->offset, sizeof(Vec2));
        }
        out.indices = mesh.indices;
        if (mirrored)
            for (size_t i = 0; i + 2 < out.indices.size(); i += 3) std::swap(out.indices[i + 1], out.indices[i + 2]);
        RigSourceNode source;
        source.name = out.name;
        source.srcNode = static_cast<int>(ni);
        for (const SubMesh& sm : mesh.subMeshes) {
            const AssetId material = sm.materialSlot < node.materials.size() ? node.materials[sm.materialSlot] : AssetId{};
            out.primitives.push_back({sm.firstIndex, sm.indexCount, materialFor(material)});
            source.submeshMaterials.push_back(material);
        }
        if (out.primitives.empty()) continue;
        in.meshes.push_back(std::move(out));
        m_rigSources.push_back(std::move(source));
    }
    if (in.meshes.empty()) {
        m_rigStatus = "Nothing to rig: the meshes are not loaded.";
        return;
    }
    m_rigStatus = "Skinning...";
    m_rig = std::move(task);
    m_jobs->submit(&RigTask::run, m_rig.get(), m_rig->counter, JobPriority::Low);
}

void AssetForge::finishRig() {
    std::unique_ptr<RigTask> task = std::move(m_rig);
    m_rigResult = std::move(task->result);
    if (!m_rigResult.ok || task->glb.empty()) {
        m_rigStatus = "Rigging failed: " + m_rigResult.error;
        return;
    }
    const std::string slugName = slug() + "_rigged";
    m_riggedSource = "imported/" + slugName + "/" + slugName + ".glb";
    const fs::path file = m_db->sourceRoot() / PathFromUtf8(m_riggedSource);
    if (!WriteFileBytes(file, task->glb)) {
        m_rigStatus = "Cannot write " + PathToUtf8(file);
        return;
    }
    m_rigStatus = std::format("Skinned {} mesh(es); importing the rigged model...", task->input.meshes.size());
    m_rigTicket = m_host.importFile ? m_host.importFile(file) : 0;
    if (m_rigTicket == 0) m_rigStatus = "The rigged model was written but could not be imported: " + m_riggedSource;
}

void AssetForge::applyRiggedImport(const ImportResult& result) {
    if (!result.ok) {
        m_rigStatus = "Importing the rigged model failed: " + result.error;
        return;
    }
    PrefabAsset rigged;
    std::string error;
    if (!m_db->load(result.prefab, rigged, &error)) {
        m_rigStatus = "Cannot read the rigged prefab: " + error;
        return;
    }
    // The import made plain materials from the glTF factors; put the model's real ones back with a variant.
    PrefabVariantAsset variant;
    variant.name = slug() + "_rigged";
    variant.base = result.prefab;
    int restored = 0;
    for (const RigSourceNode& src : m_rigSources) {
        i32 index = -1;
        for (size_t i = 0; i < rigged.nodes.size(); ++i)
            if (rigged.nodes[i].name == src.name && rigged.nodes[i].mesh.isValid()) index = static_cast<i32>(i);
        if (index < 0) continue;
        const PrefabNode& node = rigged.nodes[static_cast<size_t>(index)];
        for (size_t i = 0; i < src.submeshMaterials.size() && i < node.submeshSlots.size(); ++i) {
            if (!src.submeshMaterials[i].isValid()) continue;
            PrefabOverride o;
            o.nodePath = rigged.nodePath(static_cast<u32>(index));
            o.flags = PrefabOverride::SetMaterial;
            o.materialSlot = node.submeshSlots[i];
            o.material = src.submeshMaterials[i];
            variant.overrides.push_back(o);
            ++restored;
        }
    }
    if (restored > 0) m_db->saveVariant(SiblingPath(m_riggedSource, ".fvariant"), variant);
    m_character.riggedModel = m_riggedSource;
    m_character.model = m_sourcePath;
    saveCharacter();
    if (m_host.refreshBrowser) m_host.refreshBrowser();

    // Show the result: the character stands in the preview and can walk.
    VariantApplyResult resolved;
    const AssetId shown = m_db->findAuthored(SiblingPath(m_riggedSource, ".fvariant"));
    if (m_db->loadResolvedPrefab(shown.isValid() ? shown : result.prefab, resolved) && m_host.show) {
        m_host.show(resolved.prefab);
        m_rigPreviewShown = true;
        if (m_host.previewWalk) m_host.previewWalk(m_walkSpeed);
    }
    m_rigStatus = std::format("Done: {} (arms lifted {:.0f} / {:.0f} degrees to a T-pose). {} material(s) restored.", m_riggedSource, m_rigResult.armLiftDegrees[0],
                              m_rigResult.armLiftDegrees[1], restored);
    if (!result.warnings.empty()) m_rigStatus += " Import warnings: " + result.warnings.front();
}

bool AssetForge::saveCharacter() {
    m_character.model = m_sourcePath;
    if (!writeText(pathFor(".fchar"), WriteCharacterDef(m_character))) return false;
    m_characterLoaded = true;
    m_characterDirty = false;
    if (m_host.refreshBrowser) m_host.refreshBrowser();
    if (m_host.log) m_host.log(std::format("Saved character '{}'", pathFor(".fchar")));
    return true;
}

// ---- UI --------------------------------------------------------------------------------------------------------------------

void AssetForge::drawCharacterTab() {
    CharacterDef& c = m_character;
    bool changed = false;
    if (m_characterLoaded) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1), "%s", pathFor(".fchar").c_str());
    else ImGui::TextDisabled("Not saved yet.");

    int role = static_cast<int>(c.role);
    changed |= ImGui::RadioButton("Player", &role, 0);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("NPC", &role, 1);
    c.role = role == 0 ? CharacterRole::Player : CharacterRole::Npc;
    // (CharacterRole::Player is 0)
    {
        Vec3 lo, hi;
        if (boundsOfModel(lo, hi)) c.height = hi.y - lo.y;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("height %.2f m", c.height);
    changed |= ImGui::DragFloat("Collision radius", &c.radius, 0.005f, 0.15f, 1.0f, "%.2f m");

    if (m_skinnedModel) {
        ImGui::SeparatorText("Skeleton");
        ImGui::TextWrapped("%s", m_skeletonReport.c_str());
        const size_t clips = m_db->assetsOfKind(AssetKind::AnimationClip).size();
        if (m_skeletonMatched >= 22) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1), "Compatible: the %zu imported animation clip(s) play on this character.", clips);
        else if (m_skeletonMatched >= 14)
            ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.35f, 1), "Partly compatible: clips move the joints found by name; the rest stay in the rest pose.");
        else
            ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1),
                               "The bones are not named like Mixamo's (Hips, Spine, LeftArm...). Rename them in Blender, or use the auto-rig on the model without its skeleton.");
        ImGui::TextDisabled("This model is already skinned, so there is nothing to rig.");
    } else {
        ImGui::SeparatorText("Auto-rig");
        ImGui::TextWrapped("Place the 25 joints on the character (arms out in a T-pose or hanging in an A-pose, facing +Z), then rig: the Forge skins the model to a Mixamo skeleton, so every imported Mixamo animation works on it.");
        if (ImGui::Button("Place joints from the model")) placeLandmarks();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Puts the feet on the ground, centres the model, and scales the template skeleton to its size and arm span.");
        ImGui::SameLine();
        ImGui::Checkbox("Mirror left / right", &m_mirrorJoints);

        if (c.hasLandmarks) {
            ImGui::TextDisabled("Select a joint, then move it with the gizmo in the Prefab Preview view (or type its position).");
            if (ImGui::BeginChild("##joints", ImVec2(0, ImGui::GetFontSize() * 8.0f), ImGuiChildFlags_Borders)) {
                for (u32 i = 0; i < kHumanoidJointCount; ++i) {
                    ImGui::PushStyleColor(ImGuiCol_Text, jointColor(i));
                    if (ImGui::Selectable(HumanoidJointName(i), m_selectedJoint == static_cast<int>(i))) m_selectedJoint = static_cast<int>(i);
                    ImGui::PopStyleColor();
                }
            }
            ImGui::EndChild();
            Vec3& p = c.landmarks[static_cast<size_t>(std::clamp(m_selectedJoint, 0, static_cast<int>(kHumanoidJointCount) - 1))];
            if (ImGui::DragFloat3("Position", &p.x, 0.002f, -10.0f, 10.0f, "%.3f")) {
                const int other = mirrorOf(static_cast<u32>(m_selectedJoint));
                if (m_mirrorJoints && other >= 0) c.landmarks[static_cast<size_t>(other)] = Vec3(-p.x, p.y, p.z);
                changed = true;
            }
            changed |= ImGui::Checkbox("Lift hanging arms to a T-pose", &c.normalizeTPose);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The animations are made for a T-pose skeleton. An A-pose model (arms hanging) is bent into a T-pose while it is skinned.");
            changed |= ImGui::SliderFloat("Skin softness", &c.skinSmoothness, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("0: rigid, sharp joints. 1: very soft, the skin follows several bones.");

            ImGui::BeginDisabled(m_rig != nullptr || m_rigTicket != 0 || !m_meshesReady);
            if (ImGui::Button("Rig character")) startRig();
            ImGui::EndDisabled();
            if (m_rig || m_rigTicket != 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("working...");
            }
        }
        if (!m_rigStatus.empty()) ImGui::TextWrapped("%s", m_rigStatus.c_str());

        if (!c.riggedModel.empty()) {
            ImGui::SeparatorText("Rigged model");
            ImGui::TextUnformatted(c.riggedModel.c_str());
            ImGui::TextUnformatted("Preview:");
            ImGui::SameLine();
            const float speeds[] = {0.0f, 1.6f, 4.2f};
            const char* names[] = {"Stand", "Walk", "Run"};
            for (int i = 0; i < 3; ++i) {
                if (i) ImGui::SameLine();
                if (ImGui::RadioButton(names[i], std::abs(m_walkSpeed - speeds[i]) < 0.01f)) {
                    m_walkSpeed = speeds[i];
                    if (m_host.previewWalk) m_host.previewWalk(m_walkSpeed);
                }
            }
            if (ImGui::Button("Show the rigged model")) {
                VariantApplyResult resolved;
                const AssetId shown = m_db->findAuthored(SiblingPath(c.riggedModel, ".fvariant"));
                const SourceRecord* rec = m_db->findSource(c.riggedModel);
                AssetId prefab = shown;
                if (!prefab.isValid() && rec)
                    for (const SubAssetRecord& s : rec->subAssets)
                        if (!s.tombstone && s.kind == AssetKind::Prefab) prefab = s.guid;
                if (prefab.isValid() && m_db->loadResolvedPrefab(prefab, resolved) && m_host.show) {
                    m_host.show(resolved.prefab);
                    m_rigPreviewShown = true;
                    if (m_host.previewWalk) m_host.previewWalk(m_walkSpeed);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Show the source model")) {
                m_rigPreviewShown = false;
                m_previewDirty = true;
            }
        }
    }
    if (changed) m_characterDirty = true;

    ImGui::Separator();
    if (m_characterDirty) ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.35f, 1), "Unsaved changes");
    if (ImGui::Button("Save character")) {
        if (fixed()) saveVariant();
        saveCharacter();
    }
    const std::string playable = !c.riggedModel.empty() ? c.riggedModel : (m_skinnedModel ? m_sourcePath : std::string());
    ImGui::SameLine();
    ImGui::BeginDisabled(playable.empty() || !m_host.useAsPlayer);
    if (ImGui::Button("Use as the player")) {
        c.role = CharacterRole::Player;
        saveCharacter();
        m_host.useAsPlayer(playable);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(playable.empty() || !m_host.spawn);
    if (ImGui::Button("Spawn in the game")) {
        saveCharacter();
        m_host.spawn(pathFor(".fchar"));
    }
    ImGui::EndDisabled();
}

void AssetForge::drawCharacterOverlay(const View& v, bool hovered) {
    if (m_skinnedModel || !m_character.hasLandmarks) return;
    const auto& lm = m_character.landmarks;
    for (u32 i = 1; i < kHumanoidJointCount; ++i) {
        const i32 parent = HumanoidJointParent(i);
        if (parent < 0) continue;
        line3(v, lm[static_cast<u32>(parent)], lm[i], jointColor(i), 2.0f);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (u32 i = 0; i < kHumanoidJointCount; ++i) {
        ImVec2 s;
        if (!v.project(lm[i], s)) continue;
        const bool selected = static_cast<int>(i) == m_selectedJoint;
        dl->AddCircleFilled(s, selected ? 6.0f : 4.0f, selected ? IM_COL32(255, 230, 90, 255) : jointColor(i));
        dl->AddCircle(s, selected ? 6.5f : 4.5f, IM_COL32(20, 20, 20, 220), 14, 1.5f);
        // A click on a joint selects it.
        const ImGuiIO& io = ImGui::GetIO();
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing()) {
            const float dx = io.MousePos.x - s.x, dy = io.MousePos.y - s.y;
            if (dx * dx + dy * dy < 10.0f * 10.0f) m_selectedJoint = static_cast<int>(i);
        }
    }
    label3(v, lm[static_cast<u32>(std::clamp(m_selectedJoint, 0, static_cast<int>(kHumanoidJointCount) - 1))], HumanoidJointName(static_cast<u32>(m_selectedJoint)),
           IM_COL32(255, 230, 90, 255));
    Vec3& p = m_character.landmarks[static_cast<size_t>(std::clamp(m_selectedJoint, 0, static_cast<int>(kHumanoidJointCount) - 1))];
    if (moveHandle(v, 3, p, hovered)) {
        const int other = mirrorOf(static_cast<u32>(m_selectedJoint));
        if (m_mirrorJoints && other >= 0) m_character.landmarks[static_cast<size_t>(other)] = Vec3(-p.x, p.y, p.z);
        m_characterDirty = true;
    }
}

} // namespace foh
