// The prop tab of the Asset Forge: a collision shape fitted to the model, a body type and a surface.
#include <algorithm>
#include <cmath>
#include <format>

#include "asset_forge.h"

namespace foh {

namespace {

constexpr const char* kSurfaces[] = {"concrete", "metal", "wood", "glass", "plastic", "dirt", "grass", "rubber", "cloth", "water"};

} // namespace

void AssetForge::fitProp() {
    Vec3 lo, hi;
    if (!boundsOfModel(lo, hi)) return;
    const Vec3 half = 0.5f * (hi - lo);
    m_prop.center = 0.5f * (lo + hi);
    switch (m_prop.shape) {
    case PropShape::Box: m_prop.halfExtents = half; break;
    case PropShape::Sphere: m_prop.halfExtents = Vec3(std::max({half.x, half.y, half.z}), 0.0f, 0.0f); break;
    case PropShape::Capsule: {
        const f32 r = std::max(half.x, half.z);
        m_prop.halfExtents = Vec3(r, std::max(half.y - r, 0.0f), 0.0f);
        break;
    }
    }
    m_propDirty = true;
}

bool AssetForge::saveProp() {
    m_prop.model = m_sourcePath;
    if (!writeText(pathFor(".fprop"), WritePropDef(m_prop))) return false;
    m_propLoaded = true;
    m_propDirty = false;
    if (m_host.refreshBrowser) m_host.refreshBrowser();
    if (m_host.log) m_host.log(std::format("Saved prop '{}'", pathFor(".fprop")));
    return true;
}

void AssetForge::drawPropTab() {
    PropDef& p = m_prop;
    bool changed = false;
    if (m_propLoaded) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1), "%s", pathFor(".fprop").c_str());
    else ImGui::TextDisabled("Not saved yet.");
    if (!m_propLoaded && p.halfExtents == PropDef{}.halfExtents && m_haveBounds) fitProp();

    ImGui::SeparatorText("Body");
    int body = static_cast<int>(p.body);
    changed |= ImGui::RadioButton("Static (solid)", &body, 0);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Dynamic", &body, 1);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Decoration (no collision)", &body, 2);
    p.body = static_cast<PropBody>(body);
    if (p.body == PropBody::Static) ImGui::TextDisabled("Blocks the player and vehicles. Use for walls, poles, benches, parked things that never move.");
    if (p.body == PropBody::Dynamic) ImGui::TextDisabled("Meant to be pushed and knocked over. Until the rigid body simulation lands it blocks like a static prop.");
    if (p.body == PropBody::Decoration) ImGui::TextDisabled("Only drawn: cables, signs high above, grass tufts.");

    ImGui::BeginDisabled(p.body == PropBody::Decoration);
    ImGui::SeparatorText("Collision shape");
    int shape = static_cast<int>(p.shape);
    const char* shapes[] = {"Box", "Sphere", "Capsule (upright)"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    if (ImGui::Combo("Shape", &shape, shapes, 3)) {
        p.shape = static_cast<PropShape>(shape);
        fitProp();
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Fit to model")) fitProp();
    changed |= ImGui::DragFloat3("Centre", &p.center.x, 0.01f, -50.0f, 50.0f, "%.3f");
    if (p.shape == PropShape::Box) changed |= ImGui::DragFloat3("Half size", &p.halfExtents.x, 0.01f, 0.01f, 50.0f, "%.3f");
    if (p.shape == PropShape::Sphere) changed |= ImGui::DragFloat("Radius", &p.halfExtents.x, 0.01f, 0.01f, 50.0f, "%.3f m");
    if (p.shape == PropShape::Capsule) {
        changed |= ImGui::DragFloat("Radius", &p.halfExtents.x, 0.01f, 0.01f, 50.0f, "%.3f m");
        changed |= ImGui::DragFloat("Half length of the axis", &p.halfExtents.y, 0.01f, 0.0f, 50.0f, "%.3f m");
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Physics and surface");
    if (p.body == PropBody::Dynamic) changed |= ImGui::DragFloat("Mass", &p.mass, 0.5f, 0.1f, 20000.0f, "%.1f kg");
    changed |= ImGui::DragFloat("Friction", &p.friction, 0.005f, 0.0f, 2.0f, "%.2f");
    changed |= ImGui::DragFloat("Bounciness", &p.restitution, 0.005f, 0.0f, 1.0f, "%.2f");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    if (ImGui::BeginCombo("Material", p.surface.c_str())) {
        for (const char* s : kSurfaces)
            if (ImGui::Selectable(s, p.surface == s)) {
                p.surface = s;
                changed = true;
            }
        ImGui::EndCombo();
    }
    changed |= ImGui::Checkbox("Breakable", &p.breakable);
    if (p.breakable) changed |= ImGui::DragFloat("Health", &p.health, 1.0f, 1.0f, 10000.0f, "%.0f");
    if (changed) m_propDirty = true;

    ImGui::Separator();
    if (m_propDirty) ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.35f, 1), "Unsaved changes");
    if (ImGui::Button("Save prop")) {
        if (fixed()) saveVariant();
        saveProp();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_host.spawn);
    if (ImGui::Button("Save and place in the game")) {
        if (fixed()) saveVariant();
        if (saveProp()) m_host.spawn(pathFor(".fprop"));
    }
    ImGui::EndDisabled();
    if (!m_error.empty()) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "%s", m_error.c_str());
}

void AssetForge::drawPropOverlay(const View& v, bool hovered) {
    const PropDef& p = m_prop;
    if (p.body == PropBody::Decoration) return;
    const ImU32 color = IM_COL32(255, 200, 60, 255);
    switch (p.shape) {
    case PropShape::Box: box3(v, p.center, p.halfExtents, color, 2.0f); break;
    case PropShape::Sphere:
        circle3(v, p.center, Vec3(1, 0, 0), Vec3(0, 1, 0), p.halfExtents.x, color, 2.0f);
        circle3(v, p.center, Vec3(1, 0, 0), Vec3(0, 0, 1), p.halfExtents.x, color, 2.0f);
        circle3(v, p.center, Vec3(0, 1, 0), Vec3(0, 0, 1), p.halfExtents.x, color, 2.0f);
        break;
    case PropShape::Capsule: {
        const Vec3 top = p.center + Vec3(0, p.halfExtents.y, 0), bottom = p.center - Vec3(0, p.halfExtents.y, 0);
        circle3(v, top, Vec3(1, 0, 0), Vec3(0, 0, 1), p.halfExtents.x, color, 2.0f);
        circle3(v, bottom, Vec3(1, 0, 0), Vec3(0, 0, 1), p.halfExtents.x, color, 2.0f);
        for (const Vec3 d : {Vec3(1, 0, 0), Vec3(-1, 0, 0), Vec3(0, 0, 1), Vec3(0, 0, -1)}) line3(v, top + d * p.halfExtents.x, bottom + d * p.halfExtents.x, color, 2.0f);
        circle3(v, top, Vec3(1, 0, 0), Vec3(0, 1, 0), p.halfExtents.x, color, 1.0f);
        circle3(v, bottom, Vec3(1, 0, 0), Vec3(0, 1, 0), p.halfExtents.x, color, 1.0f);
        break;
    }
    }
    if (moveHandle(v, 2, m_prop.center, hovered)) m_propDirty = true;
}

} // namespace foh
