#include "atmosphere_panel.h"

#include <cstring>

#include <imgui.h>

namespace foh {

namespace {

bool SameDesc(const AtmosphereDesc& a, const AtmosphereDesc& b) { return std::memcmp(&a, &b, sizeof(AtmosphereDesc)) == 0; }

const char* PhaseName(AtmosphereStatus::Phase p) {
    switch (p) {
        case AtmosphereStatus::Phase::Missing: return "no tables (fallback sky)";
        case AtmosphereStatus::Phase::Generating: return "generating...";
        case AtmosphereStatus::Phase::Saving: return "in use, writing the cache file...";
        case AtmosphereStatus::Phase::Ready: return "ready";
    }
    return "";
}

// Volumetric clouds apply live (no tables to rebuild): every change goes straight to the renderer.
void DrawClouds(Renderer& renderer) {
    static const char* kPresetNames[kCloudPresetCount] = {"Clear (few fair-weather cumulus)", "Fair (scattered cumulus)",
                                                          "Broken (big gaps)", "Stratocumulus (low lumpy deck)",
                                                          "Overcast (flat stratus)", "Storm (cumulonimbus)"};
    CloudDesc d = renderer.clouds();
    bool changed = ImGui::Checkbox("Enabled", &d.enabled);
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);

    ImGui::SeparatorText("Weather");
    int preset = static_cast<int>(d.preset);
    if (ImGui::Combo("Preset", &preset, kPresetNames, static_cast<int>(kCloudPresetCount))) {
        d.preset = static_cast<CloudPreset>(preset);
        changed = true;
    }
    changed |= ImGui::Checkbox("Change by itself", &d.autoCycle);
    if (d.autoCycle) {
        changed |= ImGui::SliderFloat("Every (s)", &d.cycleSeconds, 10.0f, 1200.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    }
    changed |= ImGui::SliderFloat("Transition (s)", &d.transitionSeconds, 0.0f, 300.0f, "%.0f");
    const CloudWeather w = renderer.cloudWeather();
    ImGui::TextDisabled("Now: coverage %.2f, type %.2f (0 stratus, 1 cumulus), storm %.2f -> %s", w.coverage, w.type,
                        w.storm, kPresetNames[static_cast<u32>(w.target)]);
    changed |= ImGui::SliderFloat("Coverage bias", &d.coverageBias, -0.5f, 0.5f, "%+.2f");
    changed |= ImGui::SliderFloat("Density", &d.density, 0.1f, 3.0f, "%.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Higher = darker, more opaque clouds.");

    ImGui::SeparatorText("Layer");
    changed |= ImGui::DragFloat("Base altitude (m)", &d.baseAltitude, 10.0f, 200.0f, 8000.0f, "%.0f");
    changed |= ImGui::DragFloat("Thickness (m)", &d.thickness, 10.0f, 200.0f, 6000.0f, "%.0f");
    changed |= ImGui::SliderFloat("Feature size", &d.cloudScale, 0.25f, 4.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    changed |= ImGui::SliderFloat("Detail", &d.detail, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Sky light", &d.ambient, 0.0f, 3.0f, "%.2f");
    changed |= ImGui::SliderFloat("Wind speed (m/s)", &d.windSpeed, 0.0f, 40.0f, "%.1f");
    changed |= ImGui::SliderFloat("Wind direction", &d.windDirection, 0.0f, 360.0f, "%.0f deg");
    int quality = static_cast<int>(d.quality);
    if (ImGui::Combo("Quality", &quality, "Low (20-36 steps)\0Medium (32-56 steps)\0High (48-96 steps)\0")) {
        d.quality = static_cast<u32>(quality);
        changed = true;
    }
    ImGui::PopItemWidth();
    if (changed) renderer.setClouds(d);
    if (ImGui::Button("Defaults##clouds")) renderer.setClouds(CloudDesc{});
    ImGui::TextWrapped("Half-resolution ray march, accumulated over time; only visible sky is shaded. Lit by the sun "
                       "through the atmosphere tables, so they follow the time of day.");
}

} // namespace

void AtmospherePanel::draw(Renderer& renderer, bool* open) {
    if (!ImGui::Begin("Atmosphere", open)) {
        ImGui::End();
        return;
    }
    const AtmosphereStatus st = renderer.atmosphereStatus();
    if (!m_inited && st.ready) {
        m_draft = renderer.atmosphereDesc(); // start from what is actually in use (it may come from the cache)
        m_inited = true;
    }

    ImGui::Text("State: %s", PhaseName(st.phase));
    if (!st.message.empty()) ImGui::TextDisabled("%s", st.message.c_str());
    ImGui::Text("Tables: %.1f MiB on the GPU", static_cast<double>(st.lutBytes) / (1024.0 * 1024.0));
    if (st.cacheBytes) {
        ImGui::Text("Cache file: %.1f MiB", static_cast<double>(st.cacheBytes) / (1024.0 * 1024.0));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", st.cachePath.c_str());
    } else {
        ImGui::TextDisabled("Cache file: none yet");
    }
    if (st.lastGenerateMs > 0.0f) ImGui::Text("Last generation: %.0f ms", st.lastGenerateMs);
    ImGui::Separator();

    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    ImGui::SliderFloat("Air density (Rayleigh)", &m_draft.rayleighScale, 0.0f, 3.0f, "%.2f");
    ImGui::SliderFloat("Haze (Mie)", &m_draft.mieScale, 0.0f, 8.0f, "%.2f");
    ImGui::SliderFloat("Sun glow (Mie g)", &m_draft.mieAnisotropy, 0.0f, 0.99f, "%.2f");
    ImGui::SliderFloat("Ozone", &m_draft.ozoneScale, 0.0f, 4.0f, "%.2f");
    ImGui::ColorEdit3("Ground albedo", &m_draft.groundAlbedo.x, ImGuiColorEditFlags_Float);
    int orders = static_cast<int>(m_draft.scatteringOrders);
    if (ImGui::SliderInt("Scattering orders", &orders, 1, 6)) m_draft.scatteringOrders = static_cast<u32>(orders);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("1 = single scattering only. 4 is the reference default; more costs generation time only.");
    if (ImGui::TreeNode("Planet")) {
        ImGui::DragFloat("Radius (km)", &m_draft.bottomRadiusKm, 10.0f, 1000.0f, 20000.0f, "%.0f");
        ImGui::DragFloat("Top of atmosphere (km)", &m_draft.topRadiusKm, 1.0f, m_draft.bottomRadiusKm + 10.0f,
                         m_draft.bottomRadiusKm + 200.0f, "%.0f");
        ImGui::TreePop();
    }
    ImGui::PopItemWidth();
    ImGui::Separator();

    const bool busy = st.phase == AtmosphereStatus::Phase::Generating;
    const bool dirty = !SameDesc(m_draft, renderer.atmosphereDesc());
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(dirty ? "Generate and save to cache *" : "Generate and save to cache")) renderer.generateAtmosphere(m_draft);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Defaults")) m_draft = AtmosphereDesc{};
    if (dirty) ImGui::TextDisabled("* parameters changed, tables not rebuilt yet");
    ImGui::TextWrapped("The tables are loaded from the cache file when the editor or game starts; generating them takes "
                       "a moment on the GPU and only has to be repeated after changing a parameter here "
                       "(or the precompute shaders).");
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Clouds", ImGuiTreeNodeFlags_DefaultOpen)) DrawClouds(renderer);
    ImGui::End();
}

} // namespace foh
