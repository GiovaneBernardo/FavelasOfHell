#include "city_panel.h"

#include <algorithm>

#include "foh/log.h"
#include "terrain_panel.h"

namespace foh {

void CityPanel::ensureCreated(Renderer& renderer, JobSystem& jobs, World& world, TerrainPanel& terrain, const std::filesystem::path& worldDir) {
    if (m_city) return;
    const std::filesystem::path file = worldDir / "district_leblon.fohcity";
    if (!std::filesystem::exists(file)) {
        FOH_INFO("city: {} not found (python tools/fetch_osm_district.py, then tools/build_district.py)", file.string());
        return;
    }
    auto city = std::make_unique<City>(renderer, world, &jobs);
    if (!city->load(file)) return;
    m_vegetation = terrain.vegetation();
    if (const auto dem = terrain.terrain()->heightmap()) {
        if (const auto patch = dem->patchShared()) {
            AssetId id;
            Vec4 rect;
            if (city->uploadSurfaceMask(*patch, id, rect)) terrain.setSurfaceMask(id, rect);
            if (m_vegetation) m_vegetation->settings.mask = city->vegetationMask(patch);
        }
    }
    if (m_vegetation) {
        m_vegetation->settings.plants = city->plants();
        m_vegetation->rebuildAll();
    }
    m_city = std::move(city);
}

void CityPanel::update(const Terrain& terrain, Vec3 focus) {
    if (m_city) m_city->update(terrain, focus);
}

void CityPanel::shutdown() {
    if (m_vegetation) {
        m_vegetation->settings.plants.reset();
        m_vegetation->settings.mask.reset();
    }
    m_city.reset(); // releases the chunk meshes while the renderer and world still exist
}

void CityPanel::draw(bool* open) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float s = ImGui::GetFontSize() / 15.0f;
    ImGui::SetNextWindowSize(ImVec2(360.0f * s, 380.0f * s), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 380.0f * s, vp->WorkPos.y + 60.0f * s), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("City", open)) {
        ImGui::End();
        return;
    }
    if (!m_city) {
        ImGui::TextWrapped("No district loaded. Run python tools/fetch_osm_district.py and python tools/build_district.py (see ADR 0014).");
        ImGui::End();
        return;
    }
    CitySettings& st = m_city->settings;
    const CityData& d = m_city->data();
    ImGui::Text("%zu streets, %zu buildings, %zu areas, %zu points", d.edges.size(), d.buildings.size(), d.areas.size(), d.points.size());
    bool geometry = false;
    geometry |= ImGui::Checkbox("Streets", &st.roads);
    ImGui::SameLine();
    geometry |= ImGui::Checkbox("Buildings", &st.buildings);
    ImGui::SameLine();
    geometry |= ImGui::Checkbox("Props", &st.props);
    ImGui::SliderFloat("Detail range (m)", &st.range, 150.0f, 1200.0f, "%.0f");
    ImGui::SliderFloat("Far range (m)", &st.farRange, 400.0f, 3500.0f, "%.0f");
    geometry |= ImGui::SliderFloat("Height scale", &st.heightScale, 0.5f, 2.0f, "%.2f");
    int seed = static_cast<int>(st.seed);
    if (ImGui::InputInt("Seed", &seed)) {
        st.seed = static_cast<u32>(std::max(seed, 0));
        geometry = true;
    }
    if (geometry) m_city->rebuildAll();
    if (ImGui::Button("Rebuild everything")) m_city->rebuildAll();
    const CityStats& stats = m_city->stats();
    ImGui::SeparatorText("Built");
    ImGui::Text("%u chunks (%u waiting), %.2f M triangles, %u buildings", stats.chunks, stats.waiting, double(stats.triangles) * 1e-6, stats.buildings);
    ImGui::Text("last chunk: %.1f ms", double(stats.lastChunkMs));
    ImGui::End();
}

} // namespace foh
