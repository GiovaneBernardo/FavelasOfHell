#include "game_assets_panel.h"

#include <algorithm>

#include <imgui.h>

#include "foh/utf8_path.h"

namespace foh {

namespace fs = std::filesystem;

void GameAssetsPanel::scan() {
    m_items.clear();
    std::error_code ec;
    for (fs::recursive_directory_iterator it(m_host.assetRoot, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string rel = PathToUtf8(fs::relative(it->path(), m_host.assetRoot, ec));
        const GameAssetKind kind = GameAssetKindFromPath(rel);
        if (kind == GameAssetKind::None) continue;
        std::string generic = rel;
        std::replace(generic.begin(), generic.end(), '\\', '/');
        m_items.push_back({generic, PathToUtf8(it->path().stem()), kind});
    }
    std::sort(m_items.begin(), m_items.end(), [](const Item& a, const Item& b) { return a.kind != b.kind ? a.kind < b.kind : a.name < b.name; });
    m_lastScan = std::chrono::steady_clock::now();
    m_dirty = false;
}

void GameAssetsPanel::draw(bool* open) {
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 28.0f, ImGui::GetFontSize() * 22.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Game Assets", open)) {
        ImGui::End();
        return;
    }
    if (m_dirty || std::chrono::steady_clock::now() - m_lastScan > std::chrono::seconds(3)) scan();
    ImGui::TextWrapped("Vehicles, props and characters made with the Asset Forge (right-click a model in the Content Browser > Make game asset).");
    if (ImGui::Button("Refresh")) scan();
    ImGui::SameLine();
    if (!m_spawnKey.empty()) ImGui::TextDisabled("B key spawns: %s", m_spawnKey.c_str());
    else ImGui::TextDisabled("B key spawns the placeholder sedan (pick a vehicle below to change it)");

    const struct {
        GameAssetKind kind;
        const char* title;
    } groups[] = {{GameAssetKind::Vehicle, "Vehicles"}, {GameAssetKind::Character, "Characters"}, {GameAssetKind::Prop, "Props"}};
    for (const auto& g : groups) {
        ImGui::SeparatorText(g.title);
        bool any = false;
        for (const Item& item : m_items) {
            if (item.kind != g.kind) continue;
            any = true;
            ImGui::PushID(item.rel.c_str());
            ImGui::TextUnformatted(item.name.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", item.rel.c_str());
            ImGui::SameLine(ImGui::GetFontSize() * 11.0f);
            if (ImGui::SmallButton("Spawn") && m_host.spawn) m_host.spawn(item.rel);
            ImGui::SameLine();
            if (ImGui::SmallButton("Edit") && m_host.edit) m_host.edit(item.rel);
            if (g.kind == GameAssetKind::Vehicle) {
                ImGui::SameLine();
                const bool selected = m_spawnKey == item.rel;
                if (ImGui::SmallButton(selected ? "B key *" : "B key") && m_host.selectForSpawnKey) {
                    m_spawnKey = item.rel;
                    m_host.selectForSpawnKey(item.rel);
                }
            }
            ImGui::PopID();
        }
        if (!any) ImGui::TextDisabled("None yet.");
    }
    ImGui::End();
}

} // namespace foh
