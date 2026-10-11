#include "terrain_panel.h"

#include <algorithm>
#include <cmath>

#include "foh/log.h"

namespace foh {

namespace {

// The brush ring needs the view matrix of the camera that drew the image; this projects a world point to the screen.
bool Project(const Mat4& viewMatrix, f32 fovY, ImVec2 origin, ImVec2 size, const Vec3& p, ImVec2& out) {
    const Vec4 v = viewMatrix * Vec4(p, 1.0f);
    if (v.z > -0.05f) return false;
    const f32 t = std::tan(fovY * 0.5f), aspect = size.x / size.y;
    out = ImVec2(origin.x + (v.x / (-v.z) / (t * aspect) * 0.5f + 0.5f) * size.x, origin.y + (0.5f - v.y / (-v.z) / t * 0.5f) * size.y);
    return true;
}

} // namespace

void TerrainPanel::ensureCreated(Renderer& renderer, JobSystem& jobs, World& world, const std::filesystem::path& file) {
    if (m_terrain) return;
    m_renderer = &renderer;
    m_jobs = &jobs;
    m_file = file;
    m_terrain = std::make_unique<Terrain>();
    m_terrain->setJobs(&jobs);
    m_tiles = std::make_unique<TerrainRenderer>(renderer, world, &jobs);
    m_vegetation = std::make_unique<Vegetation>(renderer, world, &jobs);
    // The real-world base (the placeholder DEM from tools/build_rio_heightmap.py); without the file the terrain is procedural.
    m_terrain->loadHeightmap(file.parent_path() / "rio_dem.fohdem");
    m_terrain->loadGroundPatch(file.parent_path() / "leblon_ground.fohp"); // the district's beach, flat city and platforms (ADR 0014)
    if (std::filesystem::exists(file) && m_terrain->load(file)) {
        FOH_INFO("terrain: loaded {}", file.string());
    } else {
        m_terrain->setParams(TerrainGenParams{});
    }
    m_draft = m_terrain->params();
    m_savedRevision = m_terrain->revision();
}

namespace {
const AssetId kTerrainMaterial{0x7E44A1A0ull, 0xFFFFFFFFFFFFFF00ull};
}

void TerrainPanel::setSurfaceMask(const AssetId& id, const Vec4& rect) {
    m_maskId = id;
    m_maskRect = rect;
    m_materialDirty = true;
}

void TerrainPanel::applyMaterial() {
    MaterialParams p;
    p.terrain = true;
    p.terrainTileSize = m_tileSize;
    for (u32 l = 0; l < 4; ++l) {
        p.terrainAlbedo[l] = m_layers[l].albedo;
        p.terrainNormal[l] = m_layers[l].normal;
    }
    p.terrainMask = m_maskId;
    p.terrainMaskRect = m_maskRect;
    m_renderer->setMaterial(kTerrainMaterial, p);
    m_tiles->setMaterial(kTerrainMaterial, m_color);
    m_materialDirty = false;
}

void TerrainPanel::updateMaterial(AssetDb& db, AssetLoader& loader) {
    if (!m_terrain) return;
    if (m_materialDirty) applyMaterial(); // the procedural look at once; textures join as they arrive
    if (m_texturesDone) return;
    bool all = true, changed = false;
    auto resolve = [&](const std::string& rel, AssetId& id, bool& asked) {
        if (id.isValid()) return;
        const std::filesystem::path file = db.sourceRoot() / rel;
        if (!std::filesystem::exists(file)) return; // not downloaded: that layer stays a plain colour
        if (const SourceRecord* rec = db.findSource(rel)) {
            for (const SubAssetRecord& s : rec->subAssets)
                if (!s.tombstone && s.kind == AssetKind::Texture) {
                    id = s.guid;
                    loader.request(id); // streams to the renderer; the material picks it up when resident
                    changed = true;
                    return;
                }
            return;
        }
        if (!asked) {
            db.beginImport(file);
            asked = true;
        }
        all = false;
    };
    for (Layer& l : m_layers) {
        resolve(l.albedoPath, l.albedo, l.askedAlbedo);
        resolve(l.normalPath, l.normal, l.askedNormal);
    }
    const AssetId barkBefore = m_barkColor, barkNormalBefore = m_barkNormal;
    resolve(m_barkColorPath, m_barkColor, m_askedBarkColor);
    resolve(m_barkNormalPath, m_barkNormal, m_askedBarkNormal);
    if (m_barkColor != barkBefore || m_barkNormal != barkNormalBefore) m_vegetation->setBarkTextures(m_barkColor, m_barkNormal);
    if (changed) m_materialDirty = true;
    if (all) m_texturesDone = true;
}

void TerrainPanel::update(Vec3 focus) {
    if (!m_terrain) return;
    m_tiles->update(*m_terrain, focus);
    m_vegetation->update(*m_terrain, focus);
}

void TerrainPanel::shutdown() {
    save();
    m_vegetation.reset();
    m_tiles.reset();
    m_terrain.reset();
}

void TerrainPanel::save() {
    if (!m_terrain || m_terrain->revision() == m_savedRevision) return;
    if (m_terrain->save(m_file)) {
        m_savedRevision = m_terrain->revision();
        FOH_INFO("terrain: saved {}", m_file.string());
    } else {
        FOH_ERROR("terrain: could not save {}", m_file.string());
    }
}

void TerrainPanel::draw(bool* open) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float s = ImGui::GetFontSize() / 15.0f; // roughly the UI scale
    ImGui::SetNextWindowSize(ImVec2(400.0f * s, vp->WorkSize.y * 0.7f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 420.0f * s, vp->WorkPos.y + 40.0f * s), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Terrain", open)) {
        ImGui::End();
        return;
    }
    if (!m_terrain) {
        ImGui::TextDisabled("Not created yet.");
        ImGui::End();
        return;
    }
    Terrain& t = *m_terrain;
    ImGui::TextDisabled("Infinite placeholder terrain (flat at the floor, land near the origin) + your sculpting.");

    // ---- sculpt ----
    if (ImGui::CollapsingHeader("Sculpt", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Sculpt in the viewport (LMB)", &m_sculptEnabled);
        ImGui::SameLine();
        ImGui::TextDisabled("Shift: invert raise/lower, Ctrl+Z / Ctrl+Y");
        int mode = static_cast<int>(m_brush.mode);
        const char* names[] = {"Raise", "Lower", "Smooth", "Flatten", "Noise", "Erase sculpt"};
        for (int i = 0; i < 6; ++i) {
            if (i) ImGui::SameLine();
            if (ImGui::RadioButton(names[i], &mode, i)) m_brush.mode = static_cast<BrushMode>(mode);
            if (i == 2) ImGui::NewLine();
        }
        ImGui::SliderFloat("Radius (m)", &m_brush.radius, 1.0f, 120.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Strength", &m_brush.strength, 0.02f, 3.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Hardness", &m_brush.hardness, 0.0f, 0.95f, "%.2f");
        if (m_brush.mode == BrushMode::Flatten) {
            ImGui::Checkbox("Use height under the cursor at the start of the stroke", &m_sampleFlattenOnClick);
            if (!m_sampleFlattenOnClick) ImGui::SliderFloat("Height (m)", &m_brush.flattenHeight, -200.0f, 600.0f, "%.1f");
        }
        if (m_brush.mode == BrushMode::Noise) ImGui::SliderFloat("Feature size (m)", &m_brush.noiseScale, 0.5f, 40.0f, "%.1f");
        ImGui::BeginDisabled(!t.canUndo());
        if (ImGui::Button("Undo")) t.undo();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!t.canRedo());
        if (ImGui::Button("Redo")) t.redo();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Clear all sculpting")) t.clearSculpt();
        if (m_cursorValid) ImGui::Text("Cursor: x %.1f  z %.1f  height %.1f m", m_cursor.x, m_cursor.z, m_cursor.y);
    }

    // ---- generator ----
    if (ImGui::CollapsingHeader("Generator (base layer)", ImGuiTreeNodeFlags_DefaultOpen)) {
        TerrainGenParams& p = m_draft;
        bool applied = false;
        auto slider = [&](const char* label, f32& v, f32 lo, f32 hi, const char* fmt = "%.1f") {
            ImGui::SliderFloat(label, &v, lo, hi, fmt);
            if (ImGui::IsItemDeactivatedAfterEdit() || (m_liveRegenerate && ImGui::IsItemEdited())) applied = true;
        };
        int seed = static_cast<int>(p.seed);
        if (ImGui::InputInt("Seed", &seed)) {
            p.seed = static_cast<u32>(std::max(seed, 0));
            applied = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Random")) {
            p.seed = (p.seed * 1664525u + 1013904223u) % 100000u;
            applied = true;
        }
        const bool hasDem = static_cast<bool>(t.heightmap());
        ImGui::BeginDisabled(!hasDem);
        bool useDem = p.useHeightmap != 0;
        if (ImGui::Checkbox("Use the real heightmap (Rio, 15 m)", &useDem)) {
            p.useHeightmap = useDem ? 1u : 0u;
            applied = true;
        }
        ImGui::EndDisabled();
        if (!hasDem) ImGui::TextDisabled("world/rio_dem.fohdem not found: python tools/fetch_rio_dem.py, then build_rio_heightmap.py");
        if (p.useHeightmap && hasDem) {
            slider("Detail between samples", p.demDetail, 0.0f, 3.0f, "%.2f");
            slider("Edge fade (m)", p.demEdgeFade, 100.0f, 4000.0f, "%.0f");
            ImGui::TextDisabled("The sliders below only drive the placeholder generator (heightmap off).");
        }
        ImGui::SeparatorText("Sea and floor");
        slider("Floor height (m)", p.floorHeight, -600.0f, -50.0f, "%.0f");
        slider("Sea bed falloff (m)", p.seabedScale, 60.0f, 900.0f, "%.0f");
        ImGui::SeparatorText("Coast");
        slider("Shore z (m)", p.coastZ, -100.0f, 240.0f);
        slider("Coast wobble (m)", p.coastWobble, 0.0f, 120.0f);
        slider("Coast wavelength (m)", p.coastScale, 60.0f, 700.0f, "%.0f");
        slider("Cove depth (m)", p.coveDepth, 0.0f, 160.0f);
        slider("Cove x (m)", p.coveX, -1200.0f, 1200.0f, "%.0f");
        ImGui::SeparatorText("Land extent");
        slider("Land half width (m)", p.landWidth, 300.0f, 3000.0f, "%.0f");
        slider("Land depth inland (m)", p.landDepth, 300.0f, 3000.0f, "%.0f");
        ImGui::SeparatorText("Slope and massif");
        slider("Slope (rise/run)", p.slope, 0.1f, 0.9f, "%.2f");
        slider("Slope span (m)", p.slopeSpan, 100.0f, 900.0f, "%.0f");
        slider("Massif height (m)", p.massifHeight, 0.0f, 800.0f, "%.0f");
        slider("Massif x (m)", p.massifX, -1500.0f, 1500.0f, "%.0f");
        slider("Massif z (m)", p.massifZ, -1500.0f, 100.0f, "%.0f");
        slider("Massif radius (m)", p.massifRadius, 80.0f, 1000.0f, "%.0f");
        slider("Back ridge (m)", p.ridgeHeight, 0.0f, 500.0f, "%.0f");
        ImGui::SeparatorText("Detail");
        slider("Roughness", p.roughness, 0.0f, 3.0f, "%.2f");
        slider("Rockiness", p.rockiness, 0.0f, 3.0f, "%.2f");
        slider("Terracing", p.terracing, 0.0f, 1.0f, "%.2f");
        slider("Shelf height (m)", p.terraceHeight, 1.0f, 8.0f);
        ImGui::Checkbox("Regenerate while dragging", &m_liveRegenerate);
        ImGui::SameLine();
        if (ImGui::Button("Reset to defaults")) {
            m_draft = TerrainGenParams{};
            applied = true;
        }
        if (applied) t.setParams(m_draft);
        ImGui::TextDisabled("Changing the generator keeps your sculpting; the visible nodes rebuild nearest first.");
    }

    // ---- vegetation ----
    if (ImGui::CollapsingHeader("Vegetation")) {
        VegetationSettings& v = m_vegetation->settings;
        bool replant = false;
        replant |= ImGui::Checkbox("Trees", &v.trees);
        ImGui::SameLine();
        replant |= ImGui::Checkbox("Palms", &v.palms);
        ImGui::SameLine();
        ImGui::Checkbox("Grass", &v.grass);
        ImGui::SliderFloat("Tree distance (m)", &v.treeDistance, 100.0f, 1200.0f, "%.0f");
        ImGui::SliderFloat("Detailed trees within (m)", &v.treeNearDistance, 20.0f, 300.0f, "%.0f");
        ImGui::SliderFloat("Grass distance (m)", &v.grassDistance, 10.0f, 120.0f, "%.0f");
        replant |= ImGui::SliderFloat("Tree density", &v.treeDensity, 0.2f, 3.0f, "%.2f");
        replant |= ImGui::SliderFloat("Grass density", &v.grassDensity, 0.2f, 3.0f, "%.2f");
        int seed = static_cast<int>(v.seed);
        if (ImGui::InputInt("Planting seed", &seed)) v.seed = static_cast<u32>(std::max(seed, 0));
        if (replant || ImGui::Button("Replant")) m_vegetation->rebuildAll();
        const VegetationStats& vs = m_vegetation->stats();
        ImGui::TextDisabled("%u trees, %u palms in %u tiles, %u grass tufts in %u tiles (%u tiles waiting)", vs.trees, vs.palms, vs.treeTiles, vs.tufts,
                            vs.grassTiles, vs.waiting);
    }

    // ---- streaming / file ----
    if (ImGui::CollapsingHeader("Streaming and file")) {
        TerrainRenderer& r = *m_tiles;
        ImGui::SliderFloat("LOD distance factor", &r.splitFactor, 1.0f, 4.0f, "%.1f");
        ImGui::SliderFloat("View distance (m)", &r.viewDistance, 1000.0f, 16000.0f, "%.0f");
        ImGui::Checkbox("Feed the water's sea floor", &r.feedWater);
        if (ImGui::SliderFloat("Ground texture size (m)", &m_tileSize, 1.5f, 16.0f, "%.1f")) m_materialDirty = true;
        const TerrainStreamStats& st = r.stats();
        ImGui::Text("%u nodes (%u visible, %u building, %u waiting), %.2f M triangles", st.nodes, st.visible, st.building, st.waiting,
                    double(st.triangles) * 1e-6);
        ImGui::TextDisabled("Nodes per level (0 = 0.5 m cells): %u %u %u %u %u %u %u %u", st.perLevel[0], st.perLevel[1], st.perLevel[2],
                            st.perLevel[3], st.perLevel[4], st.perLevel[5], st.perLevel[6], st.perLevel[7]);
        if (ImGui::Button("Save")) {
            m_savedRevision = 0;
            save();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reload from disk")) {
            if (t.load(m_file)) m_draft = t.params();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s (%zu sculpted tiles)", m_file.filename().string().c_str(), t.revision() != m_savedRevision ? " (unsaved)" : "",
                            t.sculptTileCount());
    }
    ImGui::End();
}

bool TerrainPanel::handleViewport(const Mat4& cameraWorld, f32 fovY, ImVec2 origin, ImVec2 size, bool hovered, bool cameraCaptured,
                                  bool gizmoBusy, float dt) {
    if (!m_terrain || size.x < 8.0f || size.y < 8.0f) return false;
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (hovered && !cameraCaptured) {
        const f32 nx = ((io.MousePos.x - origin.x) / size.x) * 2.0f - 1.0f, ny = 1.0f - ((io.MousePos.y - origin.y) / size.y) * 2.0f;
        const f32 t = std::tan(fovY * 0.5f);
        const Vec3 dirView = glm::normalize(Vec3(nx * t * size.x / size.y, ny * t, -1.0f));
        const Vec3 dir = Vec3(cameraWorld * Vec4(dirView, 0.0f));
        Vec3 hit;
        m_cursorValid = m_terrain->raycast(Vec3(cameraWorld[3]), dir, 8000.0f, hit);
        if (m_cursorValid) m_cursor = hit;
    } else if (!m_stroking) {
        m_cursorValid = false;
    }

    if (!m_sculptEnabled) {
        if (m_stroking) {
            m_terrain->endStroke();
            m_stroking = false;
        }
        return false;
    }

    // Brush ring draped on the terrain.
    if (m_cursorValid) {
        const Mat4 view = glm::inverse(cameraWorld);
        ImVec2 pts[65];
        int n = 0;
        bool ok = true;
        for (int i = 0; i < 64 && ok; ++i) {
            const f32 a = f32(i) / 64.0f * glm::two_pi<f32>();
            const f32 x = m_cursor.x + std::cos(a) * m_brush.radius, z = m_cursor.z + std::sin(a) * m_brush.radius;
            ok = Project(view, fovY, origin, size, Vec3(x, m_terrain->height(x, z) + 0.4f, z), pts[n++]);
        }
        const ImU32 col = m_stroking ? IM_COL32(255, 200, 80, 230) : IM_COL32(255, 255, 255, 200);
        if (ok) {
            pts[n++] = pts[0];
            dl->AddPolyline(pts, n, col, ImDrawFlags_None, 2.0f);
        }
        ImVec2 c;
        if (Project(view, fovY, origin, size, m_cursor + Vec3(0, 0.4f, 0), c)) dl->AddCircleFilled(c, 3.0f, col);
    }

    bool ended = false;
    const bool canStart = hovered && !cameraCaptured && !gizmoBusy && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_cursorValid;
    if (canStart) {
        m_terrain->beginStroke();
        m_stroking = true;
        ++m_strokeCount;
        if (m_brush.mode == BrushMode::Flatten && m_sampleFlattenOnClick) m_brush.flattenHeight = m_cursor.y;
        m_brush.seed = m_strokeCount * 7919u;
    }
    if (m_stroking) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            BrushStamp s = m_brush;
            s.center = Vec2(m_cursor.x, m_cursor.z);
            s.dt = std::min(dt, 1.0f / 30.0f);
            if (io.KeyShift && s.mode == BrushMode::Raise) s.mode = BrushMode::Lower;
            else if (io.KeyShift && s.mode == BrushMode::Lower) s.mode = BrushMode::Raise;
            m_terrain->applyBrush(s);
        } else {
            m_terrain->endStroke();
            m_stroking = false;
            ended = true; // the release must not select what is under the cursor
        }
    }
    if (hovered && io.KeyCtrl && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) io.KeyShift ? m_terrain->redo() : m_terrain->undo();
        if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) m_terrain->redo();
    }
    return m_stroking || canStart || ended;
}

} // namespace foh
