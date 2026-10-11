#include "asset_forge.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <format>

#include <ImGuizmo.h>

#include "foh/log.h"
#include "foh/renderer/components.h"
#include "foh/utf8_path.h"

namespace foh {

namespace fs = std::filesystem;

namespace {

std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool nearlyEqual(const Vec3& a, const Vec3& b) { return glm::length(a - b) < 1e-5f; }

} // namespace

// Reads mesh files on a worker: a mesh file holds every vertex, far too much to decode on the main thread.
struct AssetForge::MeshLoad {
    JobCounter counter;
    std::vector<std::pair<AssetId, fs::path>> files;
    std::vector<MeshAsset> meshes;
    std::vector<char> ok;
    static void run(void* arg) {
        auto* t = static_cast<MeshLoad*>(arg);
        t->meshes.resize(t->files.size());
        t->ok.assign(t->files.size(), 0);
        for (size_t i = 0; i < t->files.size(); ++i) {
            std::vector<u8> bytes;
            if (!ReadFileBytes(t->files[i].second, bytes)) continue;
            auto reader = ContainerReader::Open(std::move(bytes));
            if (!reader) continue;
            t->ok[i] = DeserializeMesh(*reader, t->meshes[i]) ? 1 : 0;
        }
    }
};

AssetForge::AssetForge() = default;
AssetForge::~AssetForge() { shutdown(); }

void AssetForge::init(AssetDb& db, AssetPicker& picker, JobSystem& jobs, AssetForgeHost host) {
    m_db = &db;
    m_picker = &picker;
    m_jobs = &jobs;
    m_host = std::move(host);
}

std::string AssetForge::SiblingPath(const std::string& sourcePath, const char* extension) {
    fs::path p = PathFromUtf8(sourcePath);
    p.replace_extension(extension);
    return PathToUtf8(p);
}

std::string AssetForge::slug() const {
    std::string s = PathToUtf8(PathFromUtf8(m_sourcePath).stem());
    for (char& c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
    return s.empty() ? "asset" : s;
}

bool AssetForge::writeText(const std::string& relPath, const std::string& text) {
    const fs::path file = m_db->sourceRoot() / PathFromUtf8(relPath);
    const std::span<const u8> bytes(reinterpret_cast<const u8*>(text.data()), text.size());
    if (!WriteFileBytes(file, bytes)) {
        m_error = "Cannot write '" + relPath + "'.";
        return false;
    }
    return true;
}

// ---- opening -------------------------------------------------------------------------------------------------------

bool AssetForge::open(const std::string& relPath) {
    m_error.clear();
    const GameAssetKind kind = GameAssetKindFromPath(relPath);
    std::string model = relPath;
    VehicleDef vehicle;
    PropDef prop;
    CharacterDef character;
    bool haveVehicle = false, haveProp = false, haveCharacter = false;

    auto readText = [&](const std::string& rel, std::string& out) {
        std::vector<u8> bytes;
        if (!ReadFileBytes(m_db->sourceRoot() / PathFromUtf8(rel), bytes)) return false;
        out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
    };

    if (kind != GameAssetKind::None) {
        std::string text, err;
        if (!readText(relPath, text)) {
            m_error = "Cannot read '" + relPath + "'.";
            return false;
        }
        switch (kind) {
        case GameAssetKind::Vehicle: haveVehicle = ParseVehicleDef(text, vehicle, &err); model = vehicle.model; break;
        case GameAssetKind::Prop: haveProp = ParsePropDef(text, prop, &err); model = prop.model; break;
        case GameAssetKind::Character: haveCharacter = ParseCharacterDef(text, character, &err); model = character.model; break;
        default: break;
        }
        if (!err.empty()) {
            m_error = relPath + ": " + err;
            return false;
        }
        if (model.empty()) {
            m_error = relPath + " does not name a model.";
            return false;
        }
    } else if (PathFromUtf8(relPath).extension() == ".fvariant") {
        const AssetId vid = m_db->findAuthored(relPath);
        PrefabVariantAsset v;
        if (!vid.isValid() || !m_db->load(vid, v)) {
            m_error = "Cannot read the variant '" + relPath + "'.";
            return false;
        }
        const AssetInfo* bi = m_db->find(v.base);
        const SourceRecord* rec = bi ? m_db->findSource(bi->source) : nullptr;
        if (!rec) {
            m_error = "The model this variant is based on is not imported any more.";
            return false;
        }
        model = rec->path;
    }

    // The game assets that already sit next to the model.
    std::string text;
    if (!haveVehicle && readText(SiblingPath(model, ".fvehicle"), text)) haveVehicle = ParseVehicleDef(text, vehicle);
    if (!haveProp && readText(SiblingPath(model, ".fprop"), text)) haveProp = ParsePropDef(text, prop);
    if (!haveCharacter && readText(SiblingPath(model, ".fchar"), text)) haveCharacter = ParseCharacterDef(text, character);

    if (!loadModel(model)) return false;

    m_vehicleLoaded = haveVehicle;
    m_propLoaded = haveProp;
    m_characterLoaded = haveCharacter;
    m_vehicleDirty = m_propDirty = m_characterDirty = false;
    m_vehicle = haveVehicle ? vehicle : VehicleDef{};
    m_prop = haveProp ? prop : PropDef{};
    m_character = haveCharacter ? character : CharacterDef{};
    m_vehicle.model = m_prop.model = m_character.model = m_sourcePath;
    if (m_vehicle.name.empty()) m_vehicle.name = m_title;
    if (m_prop.name.empty()) m_prop.name = m_title;
    if (m_character.name.empty()) m_character.name = m_title;
    m_selectedWheel = 0;
    m_vehicleNote.clear();
    m_rigStatus.clear();
    m_rigPreviewShown = false;
    m_riggedSource = m_character.riggedModel;
    m_rigResult = {};

    // Start on the tab that fits what is there: an existing definition, else the nature of the model.
    if (haveVehicle) m_tab = Tab::Vehicle;
    else if (haveCharacter) m_tab = Tab::Character;
    else if (haveProp) m_tab = Tab::Prop;
    else if (m_work.skeleton.isValid()) m_tab = Tab::Character;
    else m_tab = Tab::Vehicle;
    m_tabRequested = true;
    checkSkeleton();
    return true;
}

bool AssetForge::loadModel(const std::string& sourcePath) {
    const SourceRecord* rec = m_db->findSource(sourcePath);
    if (!rec) {
        m_error = "'" + sourcePath + "' is not imported yet. Double-click it in the content browser first.";
        return false;
    }
    AssetId basePrefab;
    for (const SubAssetRecord& s : rec->subAssets)
        if (!s.tombstone && s.kind == AssetKind::Prefab) basePrefab = s.guid;
    if (!basePrefab.isValid()) {
        m_error = "'" + sourcePath + "' has no prefab (it was imported as meshes only).";
        return false;
    }
    PrefabAsset base;
    std::string err;
    if (!m_db->load(basePrefab, base, &err)) {
        m_error = "Cannot read the prefab: " + err;
        return false;
    }
    if (base.nodes.empty()) {
        m_error = "The prefab has no nodes.";
        return false;
    }

    if (m_jobs && m_load) m_jobs->wait(m_load->counter);
    m_load.reset();
    m_meshes.clear();
    m_meshesReady = false;
    m_sourcePath = sourcePath;
    m_title = PathToUtf8(PathFromUtf8(sourcePath).stem());
    m_basePrefab = basePrefab;
    m_base = std::move(base);
    m_hasVariant = false;
    m_existingVariant = {};
    m_existingVariant.name = m_title;
    m_existingVariant.base = basePrefab;
    if (const AssetId vid = m_db->findAuthored(SiblingPath(sourcePath, ".fvariant")); vid.isValid()) {
        PrefabVariantAsset v;
        if (m_db->load(vid, v) && v.base == basePrefab) {
            m_existingVariant = std::move(v);
            m_hasVariant = true;
        }
    }
    m_workBase = m_hasVariant ? ApplyVariant(m_base, m_existingVariant).prefab : m_base;
    m_fixQuarterTurns = 0;
    m_fixScale = 1.0f;
    m_fixOffset = Vec3(0.0f);
    m_variantDirty = false;
    m_loaded = true;
    rebuildWork();
    startMeshLoad();
    m_framePending = true;
    if (m_host.setPreviewVisible) m_host.setPreviewVisible(true);
    m_previewDirty = true;
    return true;
}

void AssetForge::startMeshLoad() {
    auto task = std::make_unique<MeshLoad>();
    for (const PrefabNode& node : m_workBase.nodes) {
        if (!node.mesh.isValid() || m_meshes.count(node.mesh)) continue;
        if (std::any_of(task->files.begin(), task->files.end(), [&](const auto& f) { return f.first == node.mesh; })) continue;
        if (const AssetInfo* info = m_db->find(node.mesh)) task->files.emplace_back(node.mesh, info->file);
    }
    if (task->files.empty()) {
        m_meshesReady = true;
        return;
    }
    m_load = std::move(task);
    m_jobs->submit(&MeshLoad::run, m_load.get(), m_load->counter, JobPriority::Low);
}

void AssetForge::finishMeshLoad() {
    for (size_t i = 0; i < m_load->files.size(); ++i)
        if (m_load->ok[i]) m_meshes[m_load->files[i].first] = std::move(m_load->meshes[i]);
    m_load.reset();
    m_meshesReady = true;
    recomputeNodes();
    if (m_framePending) frameModel();
    // Nothing saved for this model yet: a tall, flat-in-depth model is most likely a character (arms out or hanging, a few
    // centimeters thick front to back); a long one a vehicle.
    if (!m_vehicleLoaded && !m_propLoaded && !m_characterLoaded && !m_work.skeleton.isValid()) {
        const Vec3 size = m_hi - m_lo;
        if (size.y > 0.9f && size.y < 2.6f && size.z < 0.4f * size.y) {
            m_tab = Tab::Character;
            m_tabRequested = true;
        }
    }
    // The first time a model is opened for a type with nothing saved yet, propose a setup.
    if (!m_vehicleLoaded && m_vehicle.wheels.empty() && m_tab == Tab::Vehicle) {
        estimateVehicleFromBounds();
    }
}

void AssetForge::shutdown() {
    if (m_jobs && m_load) m_jobs->wait(m_load->counter);
    if (m_jobs && m_rig) m_jobs->wait(m_rig->counter);
    m_load.reset();
    m_rig.reset();
    m_jobs = nullptr;
}

// ---- the model and its fix -------------------------------------------------------------------------------------------

bool AssetForge::fixed() const { return m_fixQuarterTurns != 0 || m_fixScale != 1.0f || glm::length(m_fixOffset) > 1e-6f; }

void AssetForge::resetFix() {
    m_fixQuarterTurns = 0;
    m_fixScale = 1.0f;
    m_fixOffset = Vec3(0.0f);
    rebuildWork();
    m_previewDirty = true;
}

void AssetForge::rebuildWork() {
    m_work = m_workBase;
    const Quat yaw = glm::angleAxis(glm::radians(90.0f * static_cast<float>(m_fixQuarterTurns & 3)), Vec3(0.0f, 1.0f, 0.0f));
    for (PrefabNode& n : m_work.nodes) {
        if (n.parent >= 0) continue;
        n.position = m_fixScale * (yaw * n.position) + m_fixOffset;
        n.rotation = glm::normalize(yaw * n.rotation);
        n.scale *= m_fixScale;
    }
    recomputeNodes();
}

void AssetForge::recomputeNodes() {
    const size_t n = m_work.nodes.size();
    m_nodes.assign(n, {});
    m_haveBounds = false;
    m_lo = Vec3(FLT_MAX);
    m_hi = Vec3(-FLT_MAX);
    for (size_t i = 0; i < n; ++i) {
        const PrefabNode& node = m_work.nodes[i];
        Node& out = m_nodes[i];
        const Mat4 local = Transform{node.position, node.rotation, node.scale}.matrix();
        out.world = node.parent >= 0 ? m_nodes[static_cast<size_t>(node.parent)].world * local : local;
        out.path = m_work.nodePath(static_cast<u32>(i));
        out.lowerName = lowerAscii(node.name);
        if (!node.mesh.isValid()) continue;
        const auto it = m_meshes.find(node.mesh);
        if (it == m_meshes.end()) continue;
        out.lo = Vec3(FLT_MAX);
        out.hi = Vec3(-FLT_MAX);
        for (int c = 0; c < 8; ++c) {
            const Vec3 corner((c & 1) ? it->second.boundsMax.x : it->second.boundsMin.x, (c & 2) ? it->second.boundsMax.y : it->second.boundsMin.y,
                              (c & 4) ? it->second.boundsMax.z : it->second.boundsMin.z);
            const Vec3 p = Vec3(out.world * Vec4(corner, 1.0f));
            out.lo = glm::min(out.lo, p);
            out.hi = glm::max(out.hi, p);
        }
        out.hasBounds = true;
        m_lo = glm::min(m_lo, out.lo);
        m_hi = glm::max(m_hi, out.hi);
        m_haveBounds = true;
    }
}

bool AssetForge::boundsOfModel(Vec3& lo, Vec3& hi) const {
    lo = m_lo;
    hi = m_hi;
    return m_haveBounds;
}

bool AssetForge::boundsOfNodes(const std::vector<int>& nodes, Vec3& lo, Vec3& hi) const {
    lo = Vec3(FLT_MAX);
    hi = Vec3(-FLT_MAX);
    bool any = false;
    for (const int i : nodes) {
        const Node& n = m_nodes[static_cast<size_t>(i)];
        if (!n.hasBounds) continue;
        lo = glm::min(lo, n.lo);
        hi = glm::max(hi, n.hi);
        any = true;
    }
    return any;
}

void AssetForge::centerAndGround() {
    // Offsets are measured on the model without any offset, so this is idempotent.
    m_fixOffset = Vec3(0.0f);
    rebuildWork();
    Vec3 lo, hi;
    if (!boundsOfModel(lo, hi)) return;
    m_fixOffset = Vec3(-0.5f * (lo.x + hi.x), -lo.y, -0.5f * (lo.z + hi.z));
    rebuildWork();
    m_previewDirty = true;
}

void AssetForge::rebuildPreview() {
    if (m_host.show) m_host.show(m_work);
}

void AssetForge::frameModel() {
    Vec3 lo, hi;
    if (!boundsOfModel(lo, hi)) return;
    m_framePending = false;
    if (m_host.frame) m_host.frame(0.5f * (lo + hi), std::max(0.5f * glm::length(hi - lo), 0.3f));
}

void AssetForge::saveVariant() {
    PrefabVariantAsset v = m_existingVariant;
    v.name = m_title;
    v.base = m_basePrefab;
    for (size_t i = 0; i < m_work.nodes.size(); ++i) {
        const PrefabNode& node = m_work.nodes[i];
        if (node.parent >= 0) continue;
        const std::string path = m_work.nodePath(static_cast<u32>(i));
        const i32 baseIndex = m_base.findNode(path);
        if (baseIndex < 0) continue;
        const PrefabNode& original = m_base.nodes[static_cast<size_t>(baseIndex)];
        const bool same = nearlyEqual(node.position, original.position) && nearlyEqual(node.scale, original.scale) &&
                          std::abs(glm::dot(node.rotation, original.rotation)) > 0.999999f;
        auto it = std::find_if(v.overrides.begin(), v.overrides.end(), [&](const PrefabOverride& o) { return o.nodePath == path; });
        if (same) {
            if (it != v.overrides.end()) it->flags &= ~PrefabOverride::SetTransform;
            continue;
        }
        if (it == v.overrides.end()) {
            v.overrides.push_back({});
            it = v.overrides.end() - 1;
            it->nodePath = path;
        }
        it->flags |= PrefabOverride::SetTransform;
        it->position = node.position;
        it->rotation = node.rotation;
        it->scale = node.scale;
    }
    v.overrides.erase(std::remove_if(v.overrides.begin(), v.overrides.end(), [](const PrefabOverride& o) { return o.flags == 0; }), v.overrides.end());
    m_db->saveVariant(SiblingPath(m_sourcePath, ".fvariant"), v);
    m_existingVariant = v;
    m_hasVariant = true;
    m_workBase = ApplyVariant(m_base, v).prefab;
    m_fixQuarterTurns = 0;
    m_fixScale = 1.0f;
    m_fixOffset = Vec3(0.0f);
    m_variantDirty = false;
    rebuildWork();
    m_previewDirty = true;
    if (m_host.refreshBrowser) m_host.refreshBrowser();
}

void AssetForge::update() {
    if (m_load && m_load->counter.pending.load(std::memory_order_acquire) == 0) finishMeshLoad();
    if (m_rig && m_rig->counter.pending.load(std::memory_order_acquire) == 0) finishRig();
    if (m_previewDirty && m_loaded && !(m_tab == Tab::Character && m_rigPreviewShown)) {
        m_previewDirty = false;
        rebuildPreview();
    }
}

// ---- UI --------------------------------------------------------------------------------------------------------------

void AssetForge::draw(bool* visible) {
    // A floating window by default, over the left half (it needs room; the Prefab Preview view it draws on sits on the right).
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 size(std::min(ImGui::GetFontSize() * 27.0f, vp->WorkSize.x * 0.5f), std::min(ImGui::GetFontSize() * 50.0f, vp->WorkSize.y - 40.0f));
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 12.0f, vp->WorkPos.y + 12.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Asset Forge", visible)) {
        ImGui::End();
        return;
    }
    if (!m_loaded) {
        ImGui::TextWrapped("Turn an imported model into something the game can use: a vehicle with wheels and suspension, a character rigged for the animations, a prop with collision.");
        ImGui::TextDisabled("Right-click a model in the Content Browser > Make game asset...");
        if (!m_error.empty()) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "%s", m_error.c_str());
        ImGui::SeparatorText("Open a model");
        static char filter[64] = {};
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##filter", "Search imported models...", filter, sizeof(filter));
        const std::string needle = lowerAscii(filter);
        if (ImGui::BeginChild("##models", ImVec2(0, ImGui::GetFontSize() * 14.0f), ImGuiChildFlags_Borders)) {
            for (const SourceRecord* rec : m_db->sources()) {
                if (rec->format != "gltf" && rec->format != "fbx") continue;
                bool hasPrefab = false;
                for (const SubAssetRecord& s : rec->subAssets) hasPrefab = hasPrefab || (!s.tombstone && s.kind == AssetKind::Prefab);
                if (!hasPrefab || (!needle.empty() && lowerAscii(rec->path).find(needle) == std::string::npos)) continue;
                if (ImGui::Selectable(rec->path.c_str())) open(rec->path);
            }
        }
        ImGui::EndChild();
        ImGui::End();
        return;
    }
    drawHeader();
    drawModelFix();

    if (ImGui::BeginTabBar("##forgeTabs")) {
        const struct {
            Tab tab;
            const char* label;
            bool exists;
        } tabs[] = {{Tab::Vehicle, "Vehicle", m_vehicleLoaded}, {Tab::Character, "Character", m_characterLoaded}, {Tab::Prop, "Prop", m_propLoaded}};
        for (const auto& t : tabs) {
            ImGuiTabItemFlags flags = 0;
            if (m_tabRequested && m_tab == t.tab) flags |= ImGuiTabItemFlags_SetSelected;
            const std::string label = std::string(t.label) + (t.exists ? " *" : "") + "###tab" + t.label;
            if (ImGui::BeginTabItem(label.c_str(), nullptr, flags)) {
                if (!m_tabRequested) {
                    if (m_tab != t.tab) m_previewDirty = true; // the preview may show something else for another tab
                    m_tab = t.tab;
                }
                if (t.tab == Tab::Vehicle) drawVehicleTab();
                else if (t.tab == Tab::Character) drawCharacterTab();
                else drawPropTab();
                ImGui::EndTabItem();
            }
        }
        m_tabRequested = false;
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void AssetForge::drawHeader() {
    ImGui::TextUnformatted(m_title.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", m_sourcePath.c_str());
    if (!m_meshesReady) ImGui::TextDisabled("Reading the meshes...");
    if (!m_error.empty()) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "%s", m_error.c_str());
}

void AssetForge::drawModelFix() {
    if (!ImGui::CollapsingHeader("Model: size, orientation, origin", ImGuiTreeNodeFlags_DefaultOpen)) return;
    Vec3 lo, hi;
    if (boundsOfModel(lo, hi)) {
        const Vec3 size = hi - lo;
        ImGui::Text("Size  %.2f x %.2f x %.2f m   (width x height x length)", size.x, size.y, size.z);
        ImGui::TextDisabled("Lowest point %.2f m, centre offset %.2f / %.2f m", lo.y, 0.5f * (lo.x + hi.x), 0.5f * (lo.z + hi.z));
        const f32 biggest = std::max({size.x, size.y, size.z});
        if (biggest > 60.0f) ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.35f, 1), "Huge: exported in centimeters? Scale it down.");
        else if (biggest < 0.2f) ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.35f, 1), "Tiny: scale it up.");
        if (m_tab == Tab::Vehicle && size.x > size.z * 1.2f) ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.35f, 1), "The model is longer than it is deep: a vehicle points along +Z. Rotate it 90 degrees.");
    } else {
        ImGui::TextDisabled("The size appears when the meshes are read.");
    }
    bool changed = false;
    const float w = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button("Rotate 90 left")) { m_fixQuarterTurns = (m_fixQuarterTurns + 3) & 3; changed = true; }
    ImGui::SameLine();
    if (ImGui::Button("Rotate 90 right")) { m_fixQuarterTurns = (m_fixQuarterTurns + 1) & 3; changed = true; }
    ImGui::SameLine();
    if (ImGui::Button("Turn 180")) { m_fixQuarterTurns = (m_fixQuarterTurns + 2) & 3; changed = true; }
    (void)w;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    if (m_tab == Tab::Character) ImGui::DragFloat("##th", &m_targetHeight, 0.01f, 0.3f, 3.0f, "height %.2f m");
    else ImGui::DragFloat("##tl", &m_targetLength, 0.01f, 0.3f, 30.0f, "length %.2f m");
    ImGui::SameLine();
    if (ImGui::Button("Scale to it") && boundsOfModel(lo, hi)) {
        const f32 current = m_tab == Tab::Character ? hi.y - lo.y : hi.z - lo.z;
        const f32 target = m_tab == Tab::Character ? m_targetHeight : m_targetLength;
        if (current > 1e-4f) {
            m_fixScale *= target / current;
            m_fixOffset *= target / current;
            changed = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Center + ground")) centerAndGround();
    ImGui::SameLine();
    ImGui::BeginDisabled(!fixed());
    if (ImGui::Button("Reset")) resetFix();
    ImGui::EndDisabled();
    if (changed) {
        rebuildWork();
        m_previewDirty = true;
    }
    if (fixed() || m_variantDirty) {
        m_variantDirty = fixed();
        ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.35f, 1), "Unsaved model fix");
        ImGui::SameLine();
        if (ImGui::Button("Save model fix")) saveVariant();
        ImGui::SameLine();
        ImGui::TextDisabled("(saved as %s, like the prefab editor)", SiblingPath(m_sourcePath, ".fvariant").c_str());
    } else if (m_hasVariant) {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1), "Model fix saved: %s", SiblingPath(m_sourcePath, ".fvariant").c_str());
    }
}

// ---- overlay -------------------------------------------------------------------------------------------------------

bool AssetForge::View::project(const Vec3& p, ImVec2& out) const {
    const Vec4 v = view * Vec4(p, 1.0f);
    if (v.z > -0.05f) return false;
    const f32 t = std::tan(fovY * 0.5f), aspect = size.x / size.y;
    out = ImVec2(origin.x + (v.x / (-v.z) / (t * aspect) * 0.5f + 0.5f) * size.x, origin.y + (0.5f - v.y / (-v.z) / t * 0.5f) * size.y);
    return true;
}

void AssetForge::line3(const View& v, const Vec3& a, const Vec3& b, ImU32 color, float width) const {
    ImVec2 pa, pb;
    if (v.project(a, pa) && v.project(b, pb)) ImGui::GetWindowDrawList()->AddLine(pa, pb, color, width);
}

void AssetForge::circle3(const View& v, const Vec3& center, const Vec3& axisU, const Vec3& axisV, float radius, ImU32 color, float width) const {
    constexpr int kSegments = 28;
    Vec3 prev = center + axisU * radius;
    for (int i = 1; i <= kSegments; ++i) {
        const f32 a = 6.2831853f * static_cast<f32>(i) / kSegments;
        const Vec3 next = center + (axisU * std::cos(a) + axisV * std::sin(a)) * radius;
        line3(v, prev, next, color, width);
        prev = next;
    }
}

void AssetForge::box3(const View& v, const Vec3& c, const Vec3& h, ImU32 color, float width) const {
    Vec3 p[8];
    for (int i = 0; i < 8; ++i) p[i] = c + Vec3((i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z);
    const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : edges) line3(v, p[e[0]], p[e[1]], color, width);
}

void AssetForge::label3(const View& v, const Vec3& p, const char* text, ImU32 color) const {
    ImVec2 s;
    if (!v.project(p, s)) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(ImVec2(s.x + 6.0f, s.y - 6.0f), IM_COL32(0, 0, 0, 200), text);
    dl->AddText(ImVec2(s.x + 5.0f, s.y - 7.0f), color, text);
}

bool AssetForge::moveHandle(const View& v, int id, Vec3& position, bool hovered) {
    (void)hovered;
    Mat4 model = glm::translate(Mat4(1.0f), position);
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(v.origin.x, v.origin.y, v.size.x, v.size.y);
    ImGui::PushID(1000 + id); // ImGuizmo keys its gizmos on the ImGui ID stack
    const bool changed = ImGuizmo::Manipulate(&v.view[0][0], &v.proj[0][0], ImGuizmo::TRANSLATE, ImGuizmo::WORLD, &model[0][0], nullptr, nullptr);
    ImGui::PopID();
    if (changed) position = Vec3(model[3]);
    return changed;
}

void AssetForge::drawOverlay(const Mat4& cameraWorld, float fovY, float nearZ, float farZ, ImVec2 origin, ImVec2 size, bool hovered) {
    if (!m_loaded || size.x < 8.0f || size.y < 8.0f) return;
    View v;
    v.view = glm::inverse(cameraWorld);
    v.proj = glm::perspectiveRH_NO(fovY, size.x / size.y, nearZ, farZ);
    v.fovY = fovY;
    v.origin = origin;
    v.size = size;
    ImGui::PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    if (m_tab == Tab::Vehicle) drawVehicleOverlay(v, hovered);
    else if (m_tab == Tab::Character) drawCharacterOverlay(v, hovered);
    else drawPropOverlay(v, hovered);
    ImGui::PopClipRect();
}

bool AssetForge::runSelfTest(const std::string& what) {
    if (!m_loaded || !m_meshesReady) return false;
    if (what == "vehicle") {
        if (m_qaStep == 0) {
            m_tab = Tab::Vehicle;
            detectWheels();
            m_qaStep = 1;
            m_qaWait = 0;
            return false;
        }
        if (++m_qaWait < 5) return false; // the model fix takes effect
        if (fixed()) saveVariant();
        const bool saved = saveVehicle();
        FOH_INFO("forge self test (vehicle): {} wheel(s), mass {:.0f} kg, collider {:.2f} x {:.2f} x {:.2f}, saved={} '{}'", m_vehicle.wheels.size(), m_vehicle.mass,
                 2 * m_vehicle.colliderHalf.x, 2 * m_vehicle.colliderHalf.y, 2 * m_vehicle.colliderHalf.z, saved, pathFor(".fvehicle"));
        for (const WheelDef& w : m_vehicle.wheels)
            FOH_INFO("  wheel '{}' at ({:.2f}, {:.2f}, {:.2f}) r={:.2f} w={:.2f} node='{}' steer={} drive={}", w.name, w.position.x, w.position.y, w.position.z, w.radius,
                     w.width, w.node, w.steered, w.driven);
        for (const std::string& p : ValidateVehicleDef(m_vehicle)) FOH_WARN("  vehicle problem: {}", p);
        if (m_host.selectForSpawnKey) m_host.selectForSpawnKey(pathFor(".fvehicle"));
        return true;
    }
    if (what == "prop") {
        m_tab = Tab::Prop;
        fitProp();
        if (fixed()) saveVariant();
        const bool saved = saveProp();
        FOH_INFO("forge self test (prop): centre ({:.2f}, {:.2f}, {:.2f}) half ({:.2f}, {:.2f}, {:.2f}) saved={}", m_prop.center.x, m_prop.center.y, m_prop.center.z,
                 m_prop.halfExtents.x, m_prop.halfExtents.y, m_prop.halfExtents.z, saved);
        return true;
    }
    if (what == "character") {
        if (m_qaStep == 0) {
            m_tab = Tab::Character;
            m_tabRequested = true;
            checkSkeleton();
            if (m_skinnedModel) {
                FOH_INFO("forge self test (character): already skinned. {}", m_skeletonReport);
                saveCharacter();
                return true;
            }
            placeLandmarks();
            startRig();
            m_qaStep = 1;
            return false;
        }
        if (m_rig || m_rigTicket != 0) return false;
        FOH_INFO("forge self test (character): {}", m_rigStatus);
        m_walkSpeed = 1.6f; // so a screenshot shows the rig walking
        if (m_host.previewWalk) m_host.previewWalk(m_walkSpeed);
        return true;
    }
    return true;
}

void AssetForge::importFinished(const ImportResult& result) {
    if (m_rigTicket != 0 && result.ticket == m_rigTicket) {
        m_rigTicket = 0;
        applyRiggedImport(result);
    }
}

} // namespace foh
