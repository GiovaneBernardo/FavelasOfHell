#include "inspector.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>

#include <imgui.h>

#include "foh/renderer/components.h"

namespace foh {

namespace {

// "foh::Transform" -> "Transform" (full name in the tooltip).
std::string_view shortName(std::string_view full) {
    const size_t p = full.rfind("::");
    return p == std::string_view::npos ? full : full.substr(p + 2);
}

std::string entityLabel(const World& w, Entity e) {
    if (const Name* n = w.get<Name>(e); n && n->text[0]) {
        return std::format("{}  #{}", n->text, e.index());
    }
    return std::format("Entity #{}", e.index());
}

bool containsNoCase(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
    return it != hay.end();
}

ComponentId findComponent(const std::vector<ComponentInfoView>& all, const std::string& name) {
    for (const ComponentInfoView& c : all) {
        if (c.schema->name == name) return c.id;
    }
    return 0;
}

ImGuiDataType dataTypeOf(FieldKind k) {
    switch (k) {
        case FieldKind::I8: return ImGuiDataType_S8;
        case FieldKind::I16: return ImGuiDataType_S16;
        case FieldKind::I32: return ImGuiDataType_S32;
        case FieldKind::I64: return ImGuiDataType_S64;
        case FieldKind::U8: return ImGuiDataType_U8;
        case FieldKind::U16: return ImGuiDataType_U16;
        case FieldKind::U32: return ImGuiDataType_U32;
        case FieldKind::U64: return ImGuiDataType_U64;
        case FieldKind::F64: return ImGuiDataType_Double;
        default: return ImGuiDataType_Float;
    }
}

bool nameLooksLikeColor(const std::string& n) {
    std::string l;
    for (char c : n) l += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return l.find("color") != std::string::npos || l.find("colour") != std::string::npos;
}

} // namespace

// ---- Entities window --------------------------------------------------------------------

void EntityInspector::refresh(const SceneRef& scene) {
    World& w = *scene.world;
    listComponents(w, m_components);

    std::vector<ComponentId> with, without;
    for (const std::string& n : m_with) {
        if (const ComponentId id = findComponent(m_components, n)) with.push_back(id);
    }
    for (const std::string& n : m_without) {
        if (const ComponentId id = findComponent(m_components, n)) without.push_back(id);
    }
    findEntities(w, with, without, m_entities);
    m_matchedByComponents = m_entities.size();
    m_total = w.entityCount();

    if (m_search[0]) {
        std::erase_if(m_entities, [&](Entity e) { return !containsNoCase(entityLabel(w, e), m_search); });
    }
    m_dirty = false;
    m_sinceRefresh = 0.0;
}

// Draws the removable chips of one filter group. Returns true if one was removed.
bool EntityInspector::drawFilterChips(std::vector<std::string>& names, bool with) {
    bool changed = false;
    ImGui::PushStyleColor(ImGuiCol_Button, with ? ImVec4(0.18f, 0.42f, 0.25f, 1) : ImVec4(0.50f, 0.20f, 0.20f, 1));
    for (size_t i = 0; i < names.size(); ++i) {
        ImGui::PushID(static_cast<int>(i) + (with ? 0 : 1000));
        const std::string label = std::string(with ? "has " : "no ") + std::string(shortName(names[i])) + "  x";
        if (ImGui::SmallButton(label.c_str())) {
            names.erase(names.begin() + static_cast<std::ptrdiff_t>(i));
            changed = true;
            ImGui::PopID();
            break;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nclick to remove this filter", names[i].c_str());
        ImGui::PopID();
        ImGui::SameLine();
    }
    ImGui::PopStyleColor();
    return changed;
}

void EntityInspector::drawEntities(std::span<const SceneRef> scenes, Selection& selection, bool* open) {
    if (!ImGui::Begin("Entities", open)) {
        ImGui::End();
        return;
    }
    if (scenes.empty()) {
        ImGui::End();
        return;
    }
    if (m_scene >= scenes.size()) m_scene = 0;

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##scene", scenes[m_scene].name)) {
        for (size_t i = 0; i < scenes.size(); ++i) {
            if (ImGui::Selectable(scenes[i].name, i == m_scene)) {
                m_scene = i;
                m_with.clear();
                m_without.clear();
                m_dirty = true;
            }
        }
        ImGui::EndCombo();
    }

    // ---- component filters: the type-erased twin of Query<With<A>, Without<B>> ----
    bool changed = false;
    auto componentPopup = [&](const char* id, const char* title, std::vector<std::string>& target) {
        static char filter[64];
        if (ImGui::BeginPopup(id)) {
            ImGui::TextDisabled("%s", title);
            ImGui::SetNextItemWidth(220);
            if (ImGui::IsWindowAppearing()) {
                filter[0] = 0;
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::InputTextWithHint("##filter", "search components", filter, sizeof(filter));
            ImGui::BeginChild("##list", ImVec2(260, 200));
            for (const ComponentInfoView& c : m_components) {
                const std::string& n = c.schema->name;
                const bool used = std::find(m_with.begin(), m_with.end(), n) != m_with.end() ||
                                  std::find(m_without.begin(), m_without.end(), n) != m_without.end();
                if (used || !containsNoCase(n, filter)) continue;
                if (ImGui::Selectable(n.c_str())) {
                    target.push_back(n);
                    changed = true;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndChild();
            ImGui::EndPopup();
        }
    };
    if (ImGui::SmallButton("+ has")) ImGui::OpenPopup("has");
    ImGui::SameLine();
    if (ImGui::SmallButton("+ without")) ImGui::OpenPopup("without");
    ImGui::SameLine();
    if (ImGui::SmallButton("clear")) {
        m_with.clear();
        m_without.clear();
        m_search[0] = 0;
        changed = true;
    }
    componentPopup("has", "Entity must have", m_with);
    componentPopup("without", "Entity must NOT have", m_without);
    if (!m_with.empty() || !m_without.empty()) {
        if (drawFilterChips(m_with, true)) changed = true;
        if (drawFilterChips(m_without, false)) changed = true;
        ImGui::NewLine();
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##search", "search name or #id", m_search, sizeof(m_search))) changed = true;

    // The match is recomputed a few times per second (and at once when the filter
    // changes): cheap, and entities come and go while the game runs.
    m_sinceRefresh += static_cast<double>(ImGui::GetIO().DeltaTime);
    if (changed || m_dirty || m_sinceRefresh > 0.25) refresh(scenes[m_scene]);

    ImGui::TextDisabled("%zu shown, %zu match the components, %zu total", m_entities.size(), m_matchedByComponents,
                        m_total);

    World& world = *scenes[m_scene].world;
    if (ImGui::BeginChild("##entities", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_entities.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const Entity e = m_entities[static_cast<size_t>(i)];
                const bool selected = selection.scene == m_scene && selection.entity == e;
                ImGui::PushID(static_cast<int>(e.index()));
                if (ImGui::Selectable(entityLabel(world, e).c_str(), selected)) {
                    selection.scene = m_scene;
                    selection.entity = e;
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---- Inspector window ---------------------------------------------------------------------

namespace {

struct FieldContext {
    World* world;
    std::span<const SceneRef> scenes;
    Selection* selection;
    size_t sceneIndex;
    unsigned* eulerId;
    Vec3* euler;
};

void drawField(FieldContext& c, const SchemaField& f, std::byte* base, const std::string& prefix, int depth);

void drawElement(FieldContext& c, const SchemaField& f, std::byte* p, const std::string& label, int depth) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label.c_str());
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(p);
    switch (f.kind) {
        case FieldKind::Bool: ImGui::Checkbox("##v", reinterpret_cast<bool*>(p)); break;
        case FieldKind::I8: case FieldKind::I16: case FieldKind::I32: case FieldKind::I64:
        case FieldKind::U8: case FieldKind::U16: case FieldKind::U32: case FieldKind::U64:
            ImGui::DragScalar("##v", dataTypeOf(f.kind), p, 0.25f);
            break;
        case FieldKind::F32: ImGui::DragFloat("##v", reinterpret_cast<float*>(p), 0.02f); break;
        case FieldKind::F64: ImGui::DragScalar("##v", ImGuiDataType_Double, p, 0.02f); break;
        case FieldKind::Vec2: ImGui::DragFloat2("##v", reinterpret_cast<float*>(p), 0.02f); break;
        case FieldKind::Vec3:
            if (nameLooksLikeColor(f.name)) ImGui::ColorEdit3("##v", reinterpret_cast<float*>(p));
            else ImGui::DragFloat3("##v", reinterpret_cast<float*>(p), 0.02f);
            break;
        case FieldKind::Vec4:
            if (nameLooksLikeColor(f.name)) ImGui::ColorEdit4("##v", reinterpret_cast<float*>(p));
            else ImGui::DragFloat4("##v", reinterpret_cast<float*>(p), 0.02f);
            break;
        case FieldKind::Quat: {
            auto* q = reinterpret_cast<Quat*>(p);
            const unsigned id = ImGui::GetID("##q");
            Vec3 e = *c.eulerId == id ? *c.euler : glm::degrees(glm::eulerAngles(*q));
            ImGui::BeginGroup();
            const bool changed = ImGui::DragFloat3("##q", &e.x, 0.25f, 0.0f, 0.0f, "%.1f");
            ImGui::EndGroup();
            if (changed) {
                *q = glm::normalize(Quat(glm::radians(e)));
                *c.eulerId = id;
                *c.euler = e;
            } else if (ImGui::IsItemActive()) {
                *c.eulerId = id;
                *c.euler = e;
            } else if (*c.eulerId == id) {
                *c.eulerId = 0;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Euler angles in degrees (edited as a quaternion)");
            break;
        }
        case FieldKind::Entity: {
            Entity target{*reinterpret_cast<u64*>(p)};
            if (target.isNull()) {
                ImGui::TextDisabled("null");
            } else if (!c.world->isAlive(target)) {
                ImGui::TextDisabled("dead #%u", target.index());
            } else if (ImGui::SmallButton(entityLabel(*c.world, target).c_str())) {
                c.selection->scene = c.sceneIndex;
                c.selection->entity = target;
            }
            break;
        }
        case FieldKind::Text: ImGui::InputText("##v", reinterpret_cast<char*>(p), f.count); break;
        case FieldKind::Struct:
            ImGui::TextDisabled("{ %zu fields }", f.children.size());
            break;
        case FieldKind::Buffer: ImGui::TextDisabled("<buffer>"); break;
    }
    ImGui::PopID();
    if (f.kind == FieldKind::Struct) {
        for (const SchemaField& child : f.children) drawField(c, child, p, label + ".", depth + 1);
    }
}

void drawField(FieldContext& c, const SchemaField& f, std::byte* base, const std::string& prefix, int depth) {
    std::byte* p = base + f.offset;
    const u32 n = f.kind == FieldKind::Text ? 1 : f.count;
    for (u32 i = 0; i < n; ++i) {
        std::string label = prefix + f.name;
        if (n > 1) label += "[" + std::to_string(i) + "]";
        drawElement(c, f, p + static_cast<size_t>(i) * f.elemSize, label, depth);
    }
}

} // namespace

void EntityInspector::drawComponent(World& world, Entity entity, ComponentId id, const Schema& schema,
                                    std::span<const SceneRef> scenes, Selection& selection) {
    ImGui::PushID(static_cast<int>(id & 0x7FFFFFFF));
    const bool open = ImGui::CollapsingHeader(std::string(shortName(schema.name)).c_str(), ImGuiTreeNodeFlags_DefaultOpen);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (%u bytes)", schema.name.c_str(), schema.size);
    bool remove = false;
    if (ImGui::BeginPopupContextItem("##ctx")) {
        if (ImGui::MenuItem("Remove component")) remove = true;
        ImGui::EndPopup();
    }
    if (open) {
        if (schema.size == 0) {
            ImGui::TextDisabled("tag (no data)");
        } else if (!schema.reflected) {
            ImGui::TextDisabled("no FOH_REFLECT: %u bytes, not editable", schema.size);
        } else if (auto* data = static_cast<std::byte*>(world.componentPtr(entity, id))) {
            FieldContext ctx{&world, scenes, &selection, selection.scene, &m_eulerId, &m_euler};
            if (ImGui::BeginTable("##fields", 2, ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
                for (const SchemaField& f : schema.fields) drawField(ctx, f, data, "", 0);
                ImGui::EndTable();
            }
        }
    }
    ImGui::PopID();
    if (remove) world.removeComponent(entity, id);
}

void EntityInspector::drawInspector(std::span<const SceneRef> scenes, Selection& selection, bool* open) {
    if (!ImGui::Begin("Inspector", open)) {
        ImGui::End();
        return;
    }
    if (!selection.valid(scenes)) {
        ImGui::TextDisabled("Nothing selected.\nPick an entity in the Entities list or click one in a viewport.");
        ImGui::End();
        return;
    }
    World& world = *scenes[selection.scene].world;
    const Entity e = selection.entity;

    if (Name* name = world.get<Name>(e)) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##name", name->text, sizeof(name->text));
    } else {
        ImGui::TextUnformatted(entityLabel(world, e).c_str());
    }
    ImGui::TextDisabled("%s, entity %u (generation %u)", scenes[selection.scene].name, e.index(), e.generation());
    if (const Entity parent = world.parentOf(e); !parent.isNull()) {
        ImGui::Text("Parent:");
        ImGui::SameLine();
        if (ImGui::SmallButton(entityLabel(world, parent).c_str())) selection.entity = parent;
    }
    if (const size_t children = world.childCount(e)) ImGui::TextDisabled("%zu children", children);
    ImGui::Separator();

    componentsOf(world, e, m_scratchIds);
    std::vector<ComponentId> ids = m_scratchIds; // components may be removed while iterating
    for (const ComponentId id : ids) {
        if (const Schema* schema = world.schema(id)) drawComponent(world, e, id, *schema, scenes, selection);
    }
    ImGui::End();
}

} // namespace foh
