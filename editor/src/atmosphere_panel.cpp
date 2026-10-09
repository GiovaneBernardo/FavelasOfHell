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

// The sea applies live too. It is drawn by viewports that have the water pass on (the "Water Test" panel).
void DrawWater(Renderer& renderer) {
    WaterDesc d = renderer.water();
    bool changed = ImGui::Checkbox("Enabled##water", &d.enabled);
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    int quality = static_cast<int>(d.quality);
    if (ImGui::Combo("Quality##water", &quality, "Low (Gerstner waves, no SSR)\0Medium (FFT 128, 12-step SSR)\0High (FFT 256, 24-step SSR)\0")) {
        d.quality = static_cast<u32>(quality);
        changed = true;
    }
    changed |= ImGui::Checkbox("Screen-space reflections", &d.reflections);
    changed |= ImGui::SliderFloat("Sea level (m)", &d.seaLevel, -5.0f, 5.0f, "%.2f");

    ImGui::SeparatorText("Waves");
    changed |= ImGui::SliderFloat("Wind direction##water", &d.windDirection, 0.0f, 360.0f, "%.0f deg");
    changed |= ImGui::SliderFloat("Wind sea height (m)", &d.waveHeight, 0.0f, 4.0f, "%.2f");
    changed |= ImGui::SliderFloat("Wind sea period (s)", &d.wavePeriod, 2.0f, 14.0f, "%.1f");
    changed |= ImGui::SliderFloat("Spread", &d.spread, 0.25f, 6.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    changed |= ImGui::SliderFloat("Swell height (m)", &d.swellHeight, 0.0f, 4.0f, "%.2f");
    changed |= ImGui::SliderFloat("Swell period (s)", &d.swellPeriod, 5.0f, 18.0f, "%.1f");
    changed |= ImGui::SliderFloat("Swell direction", &d.swellDirection, 0.0f, 360.0f, "%.0f deg");
    changed |= ImGui::SliderFloat("Choppiness", &d.choppiness, 0.0f, 1.5f, "%.2f");
    changed |= ImGui::SliderFloat("Largest tile (m)", &d.tileSize, 128.0f, 2048.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The largest cascade repeats at this distance; swell longer than it cannot exist.");
    changed |= ImGui::SliderFloat("Shoaling depth (m)", &d.shoalingDepth, 0.5f, 20.0f, "%.1f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Waves lose height in water shallower than this (needs a sea floor height field).");

    ImGui::SeparatorText("Look");
    changed |= ImGui::ColorEdit3("Water color", &d.bodyColor.x, ImGuiColorEditFlags_Float);
    changed |= ImGui::SliderFloat("Turbidity", &d.turbidity, 0.01f, 1.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
    changed |= ImGui::SliderFloat3("Absorption (1/m)", &d.absorption.x, 0.0f, 1.5f, "%.3f");
    changed |= ImGui::SliderFloat("Refraction", &d.refraction, 0.0f, 0.2f, "%.3f");
    changed |= ImGui::SliderFloat("Sand brightness", &d.sandBrightness, 0.0f, 1.2f, "%.2f");

    ImGui::SeparatorText("Foam");
    changed |= ImGui::SliderFloat("Whitecaps", &d.foam, 0.0f, 3.0f, "%.2f");
    changed |= ImGui::SliderFloat("Fold threshold", &d.foamThreshold, -0.2f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Foam forms where the surface folds over itself harder than this (1 = flat). Higher = more foam.");
    changed |= ImGui::SliderFloat("Foam lifetime (s)", &d.foamPersistence, 0.2f, 12.0f, "%.1f");
    changed |= ImGui::SliderFloat("Shore foam", &d.shoreFoam, 0.0f, 3.0f, "%.2f");
    ImGui::PopItemWidth();
    if (changed) renderer.setWater(d);
    if (ImGui::Button("Defaults##water")) renderer.setWater(WaterDesc{});
    ImGui::TextWrapped("FFT wave cascades (wind sea + swell, JONSWAP) simulated once per frame; a camera-centred clipmap "
                       "mesh; reflections from the screen, else from a sky + cloud probe. The shoreline is where the "
                       "terrain crosses the sea level.");
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
    if (ImGui::CollapsingHeader("Water")) DrawWater(renderer);
    ImGui::End();
}

} // namespace foh
