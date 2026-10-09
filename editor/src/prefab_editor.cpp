#include "prefab_editor.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <format>

#include <imgui.h>

#include "foh/log.h"
#include "foh/renderer/components.h"
#include "foh/utf8_path.h"

namespace foh {

namespace {

namespace fs = std::filesystem;

std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

// Reads the bounds of mesh files on a worker: a mesh file holds all its vertices, which is far too much to read
// on the main thread just to learn how big the model is.
struct PrefabEditor::BoundsTask {
    JobCounter counter;
    std::vector<std::pair<AssetId, fs::path>> meshes;
    std::vector<std::pair<Vec3, Vec3>> bounds;
    std::vector<char> ok;
    static void run(void* arg) {
        auto* t = static_cast<BoundsTask*>(arg);
        t->bounds.resize(t->meshes.size());
        t->ok.assign(t->meshes.size(), 0);
        for (size_t i = 0; i < t->meshes.size(); ++i) {
            std::vector<u8> bytes;
            if (!ReadFileBytes(t->meshes[i].second, bytes)) continue;
            auto reader = ContainerReader::Open(std::move(bytes));
            if (!reader) continue;
            MeshAsset mesh;
            if (!DeserializeMesh(*reader, mesh)) continue;
            t->bounds[i] = {mesh.boundsMin, mesh.boundsMax};
            t->ok[i] = 1;
        }
    }
};

PrefabEditor::PrefabEditor() = default;
PrefabEditor::~PrefabEditor() { shutdown(); }

void PrefabEditor::init(AssetDb& db, AssetPicker& picker, JobSystem& jobs, PrefabEditorHost host) {
    m_db = &db;
    m_picker = &picker;
    m_jobs = &jobs;
    m_host = std::move(host);
}

void PrefabEditor::shutdown() {
    if (m_jobs && m_bounds) m_jobs->wait(m_bounds->counter);
    m_bounds.reset();
    m_jobs = nullptr;
}

std::string PrefabEditor::VariantPathFor(const std::string& sourcePath) {
    fs::path p = PathFromUtf8(sourcePath);
    p.replace_extension(".fvariant");
    return PathToUtf8(p);
}

// ---- loading and saving ----------------------------------------------------------------------------

bool PrefabEditor::open(const std::string& rel) {
    m_error.clear();
    const std::string ext = lowerAscii(PathToUtf8(PathFromUtf8(rel).extension()));
    AssetId basePrefab;
    std::string sourcePath, variantPath;
    PrefabVariantAsset existing;
    bool hasExisting = false;
    auto fail = [&](std::string why) {
        m_error = std::move(why);
        return false;
    };

    if (ext == ".fvariant") {
        const AssetId vid = m_db->findAuthored(rel);
        if (!vid.isValid() || !m_db->load(vid, existing)) return fail("Cannot read the variant '" + rel + "'.");
        hasExisting = true;
        basePrefab = existing.base;
        variantPath = rel;
        const AssetInfo* bi = m_db->find(basePrefab);
        if (!bi) return fail("The model this variant is based on is not imported any more.");
        const SourceRecord* rec = m_db->findSource(bi->source);
        sourcePath = rec ? rec->path : rel;
    } else {
        const SourceRecord* rec = m_db->findSource(rel);
        if (!rec) return fail("'" + rel + "' is not imported yet. Double-click it in the content browser first.");
        for (const SubAssetRecord& s : rec->subAssets)
            if (!s.tombstone && s.kind == AssetKind::Prefab) basePrefab = s.guid;
        if (!basePrefab.isValid()) return fail("'" + rel + "' has no prefab (it was imported as meshes only).");
        sourcePath = rel;
        variantPath = VariantPathFor(rel);
        const AssetId vid = m_db->findAuthored(variantPath);
        if (vid.isValid() && m_db->load(vid, existing) && existing.base == basePrefab) hasExisting = true;
    }

    PrefabAsset base;
    std::string err;
    if (!m_db->load(basePrefab, base, &err)) return fail("Cannot read the prefab: " + err);
    if (base.nodes.empty()) return fail("The prefab has no nodes.");

    m_base = std::move(base);
    m_work = m_base;
    m_basePrefab = basePrefab;
    m_sourcePath = sourcePath;
    m_variantPath = variantPath;
    m_title = PathToUtf8(PathFromUtf8(sourcePath).stem());
    const size_t n = m_base.nodes.size();
    m_removed.assign(n, 0);
    if (hasExisting) {
        for (const PrefabOverride& o : existing.overrides) {
            const i32 idx = m_base.findNode(o.nodePath);
            if (idx < 0) continue;
            PrefabNode& node = m_work.nodes[static_cast<size_t>(idx)];
            if (o.flags & PrefabOverride::Remove) m_removed[static_cast<size_t>(idx)] = 1;
            if (o.flags & PrefabOverride::SetTransform) {
                node.position = o.position;
                node.rotation = o.rotation;
                node.scale = o.scale;
            }
            if ((o.flags & PrefabOverride::SetMaterial) && o.materialSlot < node.materials.size()) node.materials[o.materialSlot] = o.material;
        }
    }
    m_kids.assign(n, {});
    m_roots.clear();
    for (size_t i = 0; i < n; ++i) {
        const i32 parent = m_base.nodes[i].parent;
        if (parent < 0) m_roots.push_back(static_cast<int>(i));
        else m_kids[static_cast<size_t>(parent)].push_back(static_cast<int>(i));
    }
    m_selected = m_roots.empty() ? -1 : m_roots.front();
    m_eulerActive = false;
    commitRoots();
    m_dirty = false;
    m_loaded = true;
    startBoundsJob();
    m_framePending = true;
    if (m_host.setPreviewVisible) m_host.setPreviewVisible(true);
    rebuildPreview();
    return true;
}

void PrefabEditor::startBoundsJob() {
    if (m_bounds) m_jobs->wait(m_bounds->counter);
    m_bounds.reset();
    auto task = std::make_unique<BoundsTask>();
    for (const PrefabNode& node : m_base.nodes) {
        if (!node.mesh.isValid() || m_meshBounds.count(node.mesh)) continue;
        if (std::any_of(task->meshes.begin(), task->meshes.end(), [&](const auto& m) { return m.first == node.mesh; })) continue;
        if (const AssetInfo* info = m_db->find(node.mesh)) task->meshes.emplace_back(node.mesh, info->file);
    }
    if (task->meshes.empty()) return;
    m_bounds = std::move(task);
    m_jobs->submit(&BoundsTask::run, m_bounds.get(), m_bounds->counter, JobPriority::Low);
}

bool PrefabEditor::runSelfTest(float height) {
    Vec3 lo, hi;
    if (!m_loaded || !computeBounds(lo, hi) || hi.y - lo.y <= 1e-5f) return false;
    m_adjScale *= height / (hi.y - lo.y);
    m_adjRot = glm::normalize(glm::angleAxis(glm::radians(90.0f), Vec3(0, 1, 0)) * m_adjRot);
    applyWholeModel();
    sitOnGround();
    rebuildPreview();
    save();
    if (computeBounds(lo, hi)) FOH_INFO("prefab editor self test: size {:.3f} x {:.3f} x {:.3f} m, lowest point {:.3f}, {} edit(s), saved '{}'",
                                        hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, lo.y, makeVariant().overrides.size(), m_variantPath);
    if (m_host.place) m_host.place(m_variantPath);
    return !m_dirty;
}

void PrefabEditor::update() {
    if (!m_pendingOpen.empty()) {
        const std::string path = std::move(m_pendingOpen);
        m_pendingOpen.clear();
        open(path);
    }
    if (m_bounds && m_bounds->counter.pending.load(std::memory_order_acquire) == 0) {
        for (size_t i = 0; i < m_bounds->meshes.size(); ++i)
            if (m_bounds->ok[i]) m_meshBounds[m_bounds->meshes[i].first] = m_bounds->bounds[i];
        m_bounds.reset();
    }
    if (m_framePending && !m_bounds && m_loaded) {
        m_framePending = false;
        frameView();
    }
}

PrefabVariantAsset PrefabEditor::makeVariant() const {
    PrefabVariantAsset v;
    v.base = m_basePrefab;
    v.name = m_base.name;
    for (size_t i = 0; i < m_base.nodes.size(); ++i) {
        const PrefabNode& b = m_base.nodes[i];
        const PrefabNode& w = m_work.nodes[i];
        PrefabOverride o;
        o.nodePath = m_base.nodePath(static_cast<u32>(i));
        if (m_removed[i]) {
            o.flags = PrefabOverride::Remove;
            v.overrides.push_back(o);
            continue;
        }
        if (b.position != w.position || b.rotation != w.rotation || b.scale != w.scale) {
            PrefabOverride t = o;
            t.flags = PrefabOverride::SetTransform;
            t.position = w.position;
            t.rotation = w.rotation;
            t.scale = w.scale;
            v.overrides.push_back(t);
        }
        for (size_t s = 0; s < b.materials.size() && s < w.materials.size(); ++s) {
            if (b.materials[s] == w.materials[s]) continue;
            PrefabOverride m = o;
            m.flags = PrefabOverride::SetMaterial;
            m.materialSlot = static_cast<u32>(s);
            m.material = w.materials[s];
            v.overrides.push_back(m);
        }
    }
    return v;
}

bool PrefabEditor::edited() const { return !makeVariant().overrides.empty(); }

void PrefabEditor::edit() {
    m_dirty = true;
    m_rebuild = true;
}

void PrefabEditor::rebuildPreview() {
    m_rebuild = false;
    if (!m_host.show) return;
    m_host.show(ApplyVariant(m_base, makeVariant()).prefab);
}

void PrefabEditor::save() {
    const AssetId id = m_db->saveVariant(m_variantPath, makeVariant());
    if (!id.isValid()) {
        m_error = "Could not write '" + m_variantPath + "'.";
        return;
    }
    m_error.clear();
    m_dirty = false;
    if (m_host.refreshBrowser) m_host.refreshBrowser();
}

void PrefabEditor::resetAll() {
    m_work = m_base;
    std::fill(m_removed.begin(), m_removed.end(), 0);
    commitRoots();
    edit();
}

// ---- measuring and whole-model fixes -------------------------------------------------------------------

bool PrefabEditor::isGone(int node) const {
    for (int i = node; i >= 0; i = m_work.nodes[static_cast<size_t>(i)].parent)
        if (m_removed[static_cast<size_t>(i)]) return true;
    return false;
}

bool PrefabEditor::computeBounds(Vec3& lo, Vec3& hi) const {
    const size_t n = m_work.nodes.size();
    std::vector<Mat4> world(n);
    lo = Vec3(FLT_MAX);
    hi = Vec3(-FLT_MAX);
    bool any = false;
    for (size_t i = 0; i < n; ++i) {
        const PrefabNode& node = m_work.nodes[i];
        const Transform t{node.position, node.rotation, node.scale};
        world[i] = node.parent >= 0 ? world[static_cast<size_t>(node.parent)] * t.matrix() : t.matrix();
        if (!node.mesh.isValid() || isGone(static_cast<int>(i))) continue;
        const auto it = m_meshBounds.find(node.mesh);
        if (it == m_meshBounds.end()) continue;
        for (int c = 0; c < 8; ++c) {
            const Vec3 corner((c & 1) ? it->second.second.x : it->second.first.x, (c & 2) ? it->second.second.y : it->second.first.y,
                              (c & 4) ? it->second.second.z : it->second.first.z);
            const Vec3 p = Vec3(world[i] * Vec4(corner, 1.0f));
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
            any = true;
        }
    }
    return any;
}

void PrefabEditor::frameView() {
    Vec3 lo, hi;
    if (!m_host.frame) return;
    if (!computeBounds(lo, hi)) {
        m_host.frame(Vec3(0.0f, 1.0f, 0.0f), 2.0f);
        return;
    }
    m_host.frame((lo + hi) * 0.5f, std::max(0.25f, glm::length(hi - lo) * 0.5f));
}

void PrefabEditor::commitRoots() {
    m_rootBase.assign(m_work.nodes.size(), NodeXf{});
    for (const int r : m_roots) {
        const PrefabNode& n = m_work.nodes[static_cast<size_t>(r)];
        m_rootBase[static_cast<size_t>(r)] = {n.position, n.rotation, n.scale};
    }
    m_adjScale = 1.0f;
    m_adjRot = Quat(1.0f, 0.0f, 0.0f, 0.0f);
    m_adjOffset = Vec3(0.0f);
}

void PrefabEditor::applyWholeModel() {
    for (const int r : m_roots) {
        const NodeXf& b = m_rootBase[static_cast<size_t>(r)];
        PrefabNode& n = m_work.nodes[static_cast<size_t>(r)];
        n.position = m_adjRot * (b.position * m_adjScale) + m_adjOffset;
        n.rotation = glm::normalize(m_adjRot * b.rotation);
        n.scale = b.scale * m_adjScale;
    }
    edit();
}

void PrefabEditor::sitOnGround() {
    Vec3 lo, hi;
    if (!computeBounds(lo, hi)) return;
    m_adjOffset.y -= lo.y;
    applyWholeModel();
}

// ---- ui --------------------------------------------------------------------------------------------------

void PrefabEditor::draw(bool* visible) {
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30.0f, ImGui::GetFontSize() * 46.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Prefab Editor", visible)) {
        ImGui::End();
        return;
    }
    if (!m_loaded) {
        ImGui::TextWrapped("Fix an imported model once, for every future placement: scale, rotation, parts and materials.");
        ImGui::TextDisabled("Right-click a model in the Content Browser > Edit prefab...");
        if (!m_error.empty()) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "%s", m_error.c_str());
        ImGui::End();
        return;
    }
    drawHeader();
    drawWholeModel();
    ImGui::SeparatorText("Parts");
    const float avail = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginChild("##nodes", ImVec2(0, std::max(avail * 0.35f, ImGui::GetFontSize() * 6.0f)), ImGuiChildFlags_Borders)) {
        for (const int r : m_roots) drawTree(r);
    }
    ImGui::EndChild();
    drawNode();
    if (m_rebuild) rebuildPreview();
    ImGui::End();
}

void PrefabEditor::drawHeader() {
    ImGui::TextUnformatted(m_title.c_str());
    ImGui::TextDisabled("%s", m_sourcePath.c_str());
    if (m_dirty) ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.35f, 1), "Unsaved changes");
    else if (edited()) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1), "Saved: placing this model uses '%s'", m_variantPath.c_str());
    else ImGui::TextDisabled("No changes: placing this model uses the import as it is");
    if (!m_error.empty()) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "%s", m_error.c_str());

    if (ImGui::Button("Save")) save();
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_dirty);
    if (ImGui::Button("Revert")) m_pendingOpen = m_variantPath == VariantPathFor(m_sourcePath) ? m_sourcePath : m_variantPath;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Go back to the last saved state");
    ImGui::SameLine();
    if (ImGui::Button("Reset to import")) resetAll();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drop every edit (Save to make it permanent)");
    ImGui::SameLine();
    if (ImGui::Button("Place in scene") && m_host.place) {
        if (m_dirty) save();
        if (!m_dirty) m_host.place(m_variantPath);
    }
}

void PrefabEditor::drawWholeModel() {
    ImGui::SeparatorText("Whole model");
    Vec3 lo, hi;
    const bool haveBounds = computeBounds(lo, hi);
    if (haveBounds) {
        const Vec3 size = hi - lo;
        ImGui::Text("Size  %.3f x %.3f x %.3f m   (width x height x depth)", size.x, size.y, size.z);
        const f32 biggest = std::max({size.x, size.y, size.z});
        if (biggest > 60.0f) ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.35f, 1), "Huge: exported in centimeters? Try a scale of 0.01.");
        else if (biggest < 0.05f) ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.35f, 1), "Tiny: exported in meters as millimeters? Try a scale of 100.");
        ImGui::TextDisabled("Lowest point %.3f m above the ground", lo.y);
    } else {
        ImGui::TextDisabled(m_bounds ? "Measuring the meshes..." : "Size unknown (no mesh data)");
    }
    ImGui::BeginDisabled(m_roots.empty());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    if (ImGui::DragFloat("Scale", &m_adjScale, 0.01f, 0.0001f, 100000.0f, "x %.4f", ImGuiSliderFlags_Logarithmic)) applyWholeModel();
    ImGui::SameLine();
    if (ImGui::SmallButton("x0.01")) { m_adjScale *= 0.01f; applyWholeModel(); }
    ImGui::SameLine();
    if (ImGui::SmallButton("x0.1")) { m_adjScale *= 0.1f; applyWholeModel(); }
    ImGui::SameLine();
    if (ImGui::SmallButton("x10")) { m_adjScale *= 10.0f; applyWholeModel(); }
    ImGui::SameLine();
    if (ImGui::SmallButton("x100")) { m_adjScale *= 100.0f; applyWholeModel(); }

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    ImGui::InputFloat("Height (m)", &m_targetHeight, 0.0f, 0.0f, "%.2f");
    ImGui::SameLine();
    ImGui::BeginDisabled(!haveBounds || hi.y - lo.y <= 1e-5f || m_targetHeight <= 0.0f);
    if (ImGui::Button("Fit height")) {
        m_adjScale *= m_targetHeight / (hi.y - lo.y);
        applyWholeModel();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Scales the whole model so it is this tall");

    auto rotate = [&](const char* label, Vec3 axis, f32 degrees) {
        if (ImGui::SmallButton(label)) {
            m_adjRot = glm::normalize(glm::angleAxis(glm::radians(degrees), axis) * m_adjRot);
            applyWholeModel();
        }
        ImGui::SameLine();
    };
    ImGui::TextUnformatted("Rotate");
    ImGui::SameLine();
    rotate("X -90", Vec3(1, 0, 0), -90.0f);
    rotate("X +90", Vec3(1, 0, 0), 90.0f);
    rotate("Y +90", Vec3(0, 1, 0), 90.0f);
    rotate("Z -90", Vec3(0, 0, 1), -90.0f);
    rotate("Z +90", Vec3(0, 0, 1), 90.0f);
    ImGui::NewLine();

    ImGui::BeginDisabled(!haveBounds);
    if (ImGui::Button("Sit on ground")) sitOnGround();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Frame")) frameView();
    ImGui::SameLine();
    if (ImGui::Button("Undo whole-model changes")) {
        m_adjScale = 1.0f;
        m_adjRot = Quat(1.0f, 0.0f, 0.0f, 0.0f);
        m_adjOffset = Vec3(0.0f);
        applyWholeModel();
    }
    ImGui::EndDisabled();
}

void PrefabEditor::drawTree(int node) {
    const PrefabNode& n = m_work.nodes[static_cast<size_t>(node)];
    const bool gone = m_removed[static_cast<size_t>(node)] != 0;
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (m_kids[static_cast<size_t>(node)].empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    if (node == m_selected) flags |= ImGuiTreeNodeFlags_Selected;
    if (n.parent < 0) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    ImGui::PushID(node);
    if (gone) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const std::string label = n.name + (n.mesh.isValid() ? "" : "  (group)") + (gone ? "  (removed)" : "");
    const bool open = ImGui::TreeNodeEx("##node", flags, "%s", label.c_str());
    if (gone) ImGui::PopStyleColor();
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        m_selected = node;
        m_eulerActive = false;
    }
    if (ImGui::BeginPopupContextItem("##ctx")) {
        if (ImGui::MenuItem(gone ? "Restore" : "Remove (with its children)")) {
            m_removed[static_cast<size_t>(node)] = gone ? 0 : 1;
            edit();
        }
        ImGui::EndPopup();
    }
    if (open) {
        for (const int k : m_kids[static_cast<size_t>(node)]) drawTree(k);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void PrefabEditor::drawNode() {
    if (m_selected < 0) return;
    const size_t sel = static_cast<size_t>(m_selected);
    PrefabNode& w = m_work.nodes[sel];
    const PrefabNode& b = m_base.nodes[sel];
    ImGui::SeparatorText("Selected part");
    ImGui::TextUnformatted(m_base.nodePath(static_cast<u32>(sel)).c_str());
    ImGui::PushID(m_selected);
    ImGui::PushItemWidth(-ImGui::GetFontSize() * 6.0f);

    bool xf = false;
    xf |= ImGui::DragFloat3("Position", &w.position.x, 0.01f);

    Vec3 e = m_eulerActive ? m_euler : glm::degrees(glm::eulerAngles(w.rotation));
    if (ImGui::DragFloat3("Rotation", &e.x, 0.25f, 0.0f, 0.0f, "%.1f")) {
        w.rotation = glm::normalize(Quat(glm::radians(e)));
        m_euler = e;
        m_eulerActive = true;
        xf = true;
    } else if (ImGui::IsItemActive()) {
        m_euler = e;
        m_eulerActive = true;
    } else {
        m_eulerActive = false;
    }

    ImGui::Checkbox("Uniform scale", &m_uniformScale);
    if (m_uniformScale) {
        f32 s = w.scale.x;
        if (ImGui::DragFloat("Scale", &s, 0.005f, 0.0001f, 100000.0f, "%.4f")) {
            w.scale = w.scale.x != 0.0f ? w.scale * (s / w.scale.x) : Vec3(s);
            xf = true;
        }
    } else {
        xf |= ImGui::DragFloat3("Scale", &w.scale.x, 0.005f);
    }
    ImGui::PopItemWidth();
    if (xf) {
        if (w.parent < 0) commitRoots(); // the whole-model controls now start from this
        edit();
    }

    if (ImGui::Button("Reset part")) {
        w.position = b.position;
        w.rotation = b.rotation;
        w.scale = b.scale;
        w.materials = b.materials;
        m_removed[sel] = 0;
        if (w.parent < 0) commitRoots();
        m_eulerActive = false;
        edit();
    }
    ImGui::SameLine();
    if (ImGui::Button(m_removed[sel] ? "Restore part" : "Remove part")) {
        m_removed[sel] = m_removed[sel] ? 0 : 1;
        edit();
    }

    if (w.mesh.isValid()) {
        const AssetInfo* mi = m_db->find(w.mesh);
        ImGui::Text("Mesh: %s", mi ? mi->name.c_str() : "(missing)");
        for (size_t s = 0; s < w.materials.size(); ++s) {
            ImGui::Text("Material slot %zu", s);
            ImGui::PushID(static_cast<int>(s));
            if (m_picker->field("##mat", w.materials[s], AssetKind::Material)) edit();
            ImGui::PopID();
        }
    }
    ImGui::PopID();
}

} // namespace foh
