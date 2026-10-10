#include "road_tool.h"

#include <algorithm>
#include <cmath>

#include "foh/log.h"
#include "foh/renderer/components.h"

namespace foh {

namespace {

const AssetId kRoadMaterial{0x7E44A1A3ull, 0xFFFF0001ull};
const AssetId kSidewalkMaterial{0x7E44A1A3ull, 0xFFFF0002ull};

bool Project(const Mat4& viewMatrix, f32 fovY, ImVec2 origin, ImVec2 size, const Vec3& p, ImVec2& out) {
    const Vec4 v = viewMatrix * Vec4(p, 1.0f);
    if (v.z > -0.05f) return false;
    const f32 t = std::tan(fovY * 0.5f), aspect = size.x / size.y;
    out = ImVec2(origin.x + (v.x / (-v.z) / (t * aspect) * 0.5f + 0.5f) * size.x, origin.y + (0.5f - v.y / (-v.z) / t * 0.5f) * size.y);
    return true;
}

} // namespace

AssetId RoadTool::surfaceAsset(const View& v) const { return AssetId{0x7E44A1A3ull, u64(v.serial) * 2}; }
AssetId RoadTool::sidewalkAsset(const View& v) const { return AssetId{0x7E44A1A3ull, u64(v.serial) * 2 + 1}; }

void RoadTool::init(Renderer& renderer, World& world, Terrain& terrain, Vegetation* vegetation, const std::filesystem::path& file) {
    if (m_terrain) return;
    m_renderer = &renderer;
    m_world = &world;
    m_terrain = &terrain;
    m_vegetation = vegetation;
    m_file = file;
    if (std::filesystem::exists(file) && m_network.load(file)) FOH_INFO("roads: loaded {} road(s) from {}", m_network.roads.size(), file.string());
    m_views.resize(m_network.roads.size());
    for (View& v : m_views) v.serial = m_nextSerial++;
    rebuildAll();
    if (m_vegetation) {
        m_vegetation->settings.clearings.clear();
        for (const Road& r : m_network.roads) {
            const auto c = RoadClearings(r);
            m_vegetation->settings.clearings.insert(m_vegetation->settings.clearings.end(), c.begin(), c.end());
        }
    }
    m_active = m_network.roads.empty() ? -1 : 0;
}

void RoadTool::addRoad(Road road, bool carve) {
    if (!m_terrain) return;
    m_network.roads.push_back(std::move(road));
    m_views.emplace_back();
    m_views.back().serial = m_nextSerial++;
    m_active = static_cast<int>(m_network.roads.size()) - 1;
    if (carve) CarveRoad(*m_terrain, m_network.roads.back());
    rebuild(size_t(m_active));
    commit();
}

void RoadTool::removeEntities(View& v) {
    for (Entity* e : {&v.surface, &v.sidewalk})
        if (!e->isNull() && m_world->isAlive(*e)) m_world->despawn(*e);
    v.surface = v.sidewalk = Entity{};
    if (v.surfaceUp) m_renderer->releaseMeshAsset(surfaceAsset(v));
    if (v.sidewalkUp) m_renderer->releaseMeshAsset(sidewalkAsset(v));
    v.surfaceUp = v.sidewalkUp = false;
}

void RoadTool::shutdown() {
    if (!m_terrain) return;
    if (!m_network.roads.empty() || std::filesystem::exists(m_file)) m_network.save(m_file);
    for (View& v : m_views) removeEntities(v);
    m_views.clear();
    m_terrain = nullptr;
}

void RoadTool::rebuild(size_t index) {
    if (index >= m_network.roads.size()) return;
    const Road& road = m_network.roads[index];
    View& v = m_views[index];
    RoadMeshes meshes;
    if (road.points.size() >= 2) meshes = BuildRoadMeshes(*m_terrain, road);
    auto upload = [&](const RoadMesh& m, const AssetId& id, bool& up) {
        if (m.empty()) {
            if (up) m_renderer->releaseMeshAsset(id);
            up = false;
            return;
        }
        const MeshIndexRange whole{0, static_cast<u32>(m.indices.size())};
        up = m_renderer->uploadMeshAsset(id, MeshGeometry{m.positions, m.normals, m.uvs, m.tangents, m.indices}, std::span(&whole, 1));
    };
    FOH_INFO("roads: '{}' {} points -> {} surface and {} sidewalk triangles", road.name, road.points.size(), meshes.surface.indices.size() / 3, meshes.sidewalk.indices.size() / 3);
    upload(meshes.surface, surfaceAsset(v), v.surfaceUp);
    upload(meshes.sidewalk, sidewalkAsset(v), v.sidewalkUp);
    auto ensure = [&](Entity& e, bool up, const AssetId& mesh, const AssetId& material, const char* label) {
        if (!up) {
            if (!e.isNull() && m_world->isAlive(e)) m_world->despawn(e);
            e = Entity{};
            return;
        }
        if (!e.isNull() && m_world->isAlive(e)) return; // same asset id: the renderer picks up the new geometry
        MeshRenderer mr;
        mr.asset = mesh;
        mr.material = material;
        e = m_world->spawn(Transform{}, mr, Name::Make(std::string(label) + " " + road.name));
    };
    ensure(v.surface, v.surfaceUp, surfaceAsset(v), kRoadMaterial, "Road");
    ensure(v.sidewalk, v.sidewalkUp, sidewalkAsset(v), kSidewalkMaterial, "Sidewalk");
}

void RoadTool::rebuildAll() {
    for (size_t i = 0; i < m_network.roads.size(); ++i) rebuild(i);
}

void RoadTool::commit() {
    m_network.touch();
    m_network.save(m_file);
    if (m_vegetation) {
        m_vegetation->settings.clearings.clear();
        for (const Road& r : m_network.roads) {
            const auto c = RoadClearings(r);
            m_vegetation->settings.clearings.insert(m_vegetation->settings.clearings.end(), c.begin(), c.end());
        }
        m_vegetation->rebuildAll();
    }
}

void RoadTool::applyMaterials() {
    MaterialParams road;
    road.baseColorTexture = m_asphaltColor.id;
    road.normalTexture = m_asphaltNormal.id;
    road.roughness = 0.88f;
    road.doubleSided = true; // the strips are wound by the direction the road runs: the lit side is chosen by the view
    if (!m_asphaltColor.id.isValid()) road.baseColor = Vec4(0.16f, 0.16f, 0.17f, 1.0f);
    m_renderer->setMaterial(kRoadMaterial, road);
    MaterialParams pave = road;
    pave.baseColorTexture = m_paveColor.id;
    pave.normalTexture = m_paveNormal.id;
    pave.roughness = 0.8f;
    pave.baseColor = m_paveColor.id.isValid() ? Vec4(1.0f) : Vec4(0.55f, 0.54f, 0.5f, 1.0f);
    m_renderer->setMaterial(kSidewalkMaterial, pave);
    m_materialDirty = false;
}

void RoadTool::updateMaterial(AssetDb& db, AssetLoader& loader) {
    if (!m_terrain) return;
    if (m_materialDirty) applyMaterials();
    if (m_texturesDone) return;
    bool all = true, changed = false;
    for (Texture* t : {&m_asphaltColor, &m_asphaltNormal, &m_paveColor, &m_paveNormal}) {
        if (t->id.isValid()) continue;
        const std::filesystem::path file = db.sourceRoot() / t->path;
        if (!std::filesystem::exists(file)) continue; // not downloaded: a plain colour
        if (const SourceRecord* rec = db.findSource(t->path)) {
            for (const SubAssetRecord& s : rec->subAssets)
                if (!s.tombstone && s.kind == AssetKind::Texture) {
                    t->id = s.guid;
                    loader.request(t->id);
                    changed = true;
                    break;
                }
            continue;
        }
        if (!t->asked) {
            db.beginImport(file);
            t->asked = true;
        }
        all = false;
    }
    if (changed) m_materialDirty = true;
    if (all) m_texturesDone = true;
}

void RoadTool::draw(bool* open) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float s = ImGui::GetFontSize() / 15.0f;
    ImGui::SetNextWindowSize(ImVec2(380.0f * s, 420.0f * s), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 400.0f * s, vp->WorkPos.y + 60.0f * s), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Roads", open)) {
        ImGui::End();
        return;
    }
    if (!m_terrain) {
        ImGui::TextDisabled("Not created yet.");
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("Spline roads on the terrain. Edit points in the Scene view.");
    ImGui::Checkbox("Edit in the viewport", &m_editing);
    ImGui::SameLine();
    ImGui::TextDisabled("click: add point | drag: move | Ctrl+click: delete");

    if (ImGui::Button("New road")) {
        Road r;
        r.name = "Road " + std::to_string(m_network.roads.size() + 1);
        m_network.roads.push_back(r);
        m_views.emplace_back();
        m_views.back().serial = m_nextSerial++;
        m_active = static_cast<int>(m_network.roads.size()) - 1;
        m_editing = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(m_active < 0);
    if (ImGui::Button("Delete road") && m_active >= 0) {
        removeEntities(m_views[size_t(m_active)]);
        m_views.erase(m_views.begin() + m_active);
        m_network.roads.erase(m_network.roads.begin() + m_active);
        m_active = std::min(m_active, static_cast<int>(m_network.roads.size()) - 1);
        commit();
    }
    ImGui::EndDisabled();

    for (int i = 0; i < static_cast<int>(m_network.roads.size()); ++i) {
        const Road& r = m_network.roads[size_t(i)];
        const std::string label = r.name + " (" + std::to_string(r.points.size()) + " points)##road" + std::to_string(i);
        if (ImGui::Selectable(label.c_str(), i == m_active)) m_active = i;
    }

    if (m_active >= 0 && m_active < static_cast<int>(m_network.roads.size())) {
        Road& r = m_network.roads[size_t(m_active)];
        ImGui::SeparatorText("Active road");
        char name[48];
        std::snprintf(name, sizeof(name), "%s", r.name.c_str());
        if (ImGui::InputText("Name", name, sizeof(name))) r.name = name;
        bool changed = false;
        changed |= ImGui::SliderFloat("Width (m)", &r.width, 2.5f, 20.0f, "%.1f");
        changed |= ImGui::SliderFloat("Sidewalk (m each side)", &r.sidewalk, 0.0f, 6.0f, "%.1f");
        changed |= ImGui::SliderFloat("Curb height (m)", &r.curb, 0.0f, 0.3f, "%.2f");
        ImGui::SliderFloat("Max grade", &r.maxGrade, 0.05f, 0.45f, "%.2f");
        if (changed) rebuild(size_t(m_active));
        if (ImGui::IsItemDeactivatedAfterEdit()) commit();

        ImGui::BeginDisabled(r.points.size() < 2);
        if (ImGui::Button("Carve terrain (undoable in Terrain)")) {
            CarveRoad(*m_terrain, r);
            rebuild(size_t(m_active));
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Rebuild mesh")) rebuild(size_t(m_active));
        ImGui::TextDisabled("Carve first (it cuts and fills the ground under the road), then the mesh follows the new ground.");
        const std::vector<Vec2> centre = RoadCenterline(r, 2.0f);
        f32 length = 0.0f;
        for (size_t i = 1; i < centre.size(); ++i) length += glm::length(centre[i] - centre[i - 1]);
        ImGui::Text("Length %.0f m", length);
    }
    ImGui::End();
}

bool RoadTool::handleViewport(const Mat4& cameraWorld, f32 fovY, ImVec2 origin, ImVec2 size, bool hovered, bool cameraCaptured, bool gizmoBusy, float) {
    if (!m_terrain || !m_editing || size.x < 8.0f || size.y < 8.0f) return false;
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const Mat4 view = glm::inverse(cameraWorld);

    // Where the mouse points on the ground.
    m_cursorValid = false;
    if (hovered && !cameraCaptured) {
        const f32 nx = ((io.MousePos.x - origin.x) / size.x) * 2.0f - 1.0f, ny = 1.0f - ((io.MousePos.y - origin.y) / size.y) * 2.0f;
        const f32 t = std::tan(fovY * 0.5f);
        const Vec3 dirView = glm::normalize(Vec3(nx * t * size.x / size.y, ny * t, -1.0f));
        Vec3 hit;
        if (m_terrain->raycast(Vec3(cameraWorld[3]), Vec3(cameraWorld * Vec4(dirView, 0.0f)), 8000.0f, hit)) {
            m_cursorValid = true;
            m_cursor = hit;
        }
    }

    // Draw every road, the active one brighter; find the point under the mouse.
    m_hoverPoint = m_hoverRoad = -1;
    f32 bestPx = 14.0f;
    for (int ri = 0; ri < static_cast<int>(m_network.roads.size()); ++ri) {
        const Road& r = m_network.roads[size_t(ri)];
        const bool active = ri == m_active;
        const std::vector<Vec2> centre = RoadCenterline(r, 3.0f);
        ImVec2 prev;
        bool havePrev = false;
        for (const Vec2& p : centre) {
            ImVec2 sp;
            const bool ok = Project(view, fovY, origin, size, Vec3(p.x, m_terrain->height(p.x, p.y) + 0.5f, p.y), sp);
            if (ok && havePrev) dl->AddLine(prev, sp, active ? IM_COL32(255, 210, 70, 235) : IM_COL32(255, 255, 255, 140), active ? 3.0f : 2.0f);
            prev = sp;
            havePrev = ok;
        }
        for (int pi = 0; pi < static_cast<int>(r.points.size()); ++pi) {
            const RoadPoint& rp = r.points[size_t(pi)];
            ImVec2 sp;
            if (!Project(view, fovY, origin, size, Vec3(rp.x, m_terrain->height(rp.x, rp.z) + 0.5f, rp.z), sp)) continue;
            const f32 d = std::sqrt((sp.x - io.MousePos.x) * (sp.x - io.MousePos.x) + (sp.y - io.MousePos.y) * (sp.y - io.MousePos.y));
            const bool hot = hovered && d < bestPx;
            if (hot) {
                bestPx = d;
                m_hoverPoint = pi;
                m_hoverRoad = ri;
            }
            dl->AddCircleFilled(sp, active ? 6.0f : 4.5f, active ? IM_COL32(255, 210, 70, 255) : IM_COL32(220, 220, 220, 220));
            dl->AddCircle(sp, active ? 6.5f : 5.0f, IM_COL32(20, 20, 20, 220), 16, 1.5f);
        }
    }
    if (m_hoverPoint >= 0) {
        const RoadPoint& rp = m_network.roads[size_t(m_hoverRoad)].points[size_t(m_hoverPoint)];
        ImVec2 sp;
        if (Project(view, fovY, origin, size, Vec3(rp.x, m_terrain->height(rp.x, rp.z) + 0.5f, rp.z), sp)) dl->AddCircle(sp, 11.0f, io.KeyCtrl ? IM_COL32(255, 80, 80, 255) : IM_COL32(120, 220, 255, 255), 24, 2.0f);
    }

    bool used = false;
    const bool click = hovered && !cameraCaptured && !gizmoBusy && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    if (click) {
        if (m_hoverPoint >= 0 && io.KeyCtrl) {
            Road& r = m_network.roads[size_t(m_hoverRoad)];
            r.points.erase(r.points.begin() + m_hoverPoint);
            m_active = m_hoverRoad;
            rebuild(size_t(m_hoverRoad));
            commit();
            used = true;
        } else if (m_hoverPoint >= 0) {
            m_dragPoint = m_hoverPoint;
            m_dragRoad = m_hoverRoad;
            m_active = m_hoverRoad;
            used = true;
        } else if (m_cursorValid && m_active >= 0) {
            Road& r = m_network.roads[size_t(m_active)];
            r.points.push_back({m_cursor.x, m_cursor.z});
            rebuild(size_t(m_active));
            commit();
            used = true;
        }
    }
    if (m_dragPoint >= 0) {
        used = true;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (m_cursorValid) {
                RoadPoint& p = m_network.roads[size_t(m_dragRoad)].points[size_t(m_dragPoint)];
                p.x = m_cursor.x;
                p.z = m_cursor.z;
                if (++m_framesSinceDragBuild >= 3) { // the mesh follows the drag a few times a second, not every frame
                    rebuild(size_t(m_dragRoad));
                    m_framesSinceDragBuild = 0;
                }
            }
        } else {
            rebuild(size_t(m_dragRoad));
            commit();
            m_dragPoint = -1;
        }
    }
    // The release after a click that added or deleted a point must not select what is under the cursor.
    return used || (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left));
}

} // namespace foh
