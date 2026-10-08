#include "content_browser.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>

#include <imgui.h>

#include "foh/log.h"
#include "foh/shell.h"
#include "foh/utf8_path.h"
#include "theme.h"
#include "thumb_loaders.h"

namespace foh {

namespace {

namespace fs = std::filesystem;

constexpr size_t kMaxSearchIndex = 50000;

u64 hashBytes(const void* data, size_t size, u64 h = 0xcbf29ce484222325ull) {
    const auto* p = static_cast<const u8*>(data);
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
u64 hashString(const std::string& s, u64 h = 0xcbf29ce484222325ull) { return hashBytes(s.data(), s.size(), h); }
template <class T> u64 hashValue(const T& v, u64 h) { return hashBytes(&v, sizeof v, h); }

std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool containsNoCase(const std::string& hay, const std::string& needleLower) {
    if (needleLower.empty()) return true;
    return lowerAscii(hay).find(needleLower) != std::string::npos;
}

bool lessNoCase(const std::string& a, const std::string& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const int ca = std::tolower(static_cast<unsigned char>(a[i])), cb = std::tolower(static_cast<unsigned char>(b[i]));
        if (ca != cb) return ca < cb;
    }
    return a.size() < b.size();
}

i64 toTicks(const fs::file_time_type& t) { return static_cast<i64>(t.time_since_epoch().count()); }

std::string formatTime(i64 ticks) {
    // file_time_type -> system_clock without relying on clock_cast availability.
    const auto ft = fs::file_time_type(fs::file_time_type::duration(ticks));
    const auto sys = std::chrono::system_clock::now() + (ft - fs::file_time_type::clock::now());
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::time_point_cast<std::chrono::system_clock::duration>(sys));
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tmv);
    return buf;
}

// Shortens `text` with "..." so that it fits `width` at font size `fs`.
std::string ellipsize(ImFont* font, float fs, const std::string& text, float width) {
    if (font->CalcTextSizeA(fs, 1e9f, 0.0f, text.c_str()).x <= width) return text;
    std::string cut = text;
    while (!cut.empty()) {
        // drop whole UTF-8 sequences
        do cut.pop_back(); while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80);
        if (font->CalcTextSizeA(fs, 1e9f, 0.0f, (cut + "...").c_str()).x <= width) return cut + "...";
    }
    return "...";
}

ImU32 withAlpha(ImU32 c, float a) {
    const ImU32 alpha = static_cast<ImU32>(static_cast<float>((c >> IM_COL32_A_SHIFT) & 255) * a);
    return (c & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
}

bool validFileName(const std::string& name) {
    if (name.empty() || name == "." || name == "..") return false;
    if (name.find_first_of("<>:\"/\\|?*") != std::string::npos) return false;
    for (unsigned char c : name)
        if (c < 32) return false;
    return name.back() != ' ' && name.back() != '.';
}

} // namespace

// ---- setup and listing -------------------------------------------------------------------------------

void ContentBrowser::init(AssetDb& db, ThumbnailService& thumbs, EditorPrefs& prefs, const std::vector<std::string>& log,
                          ContentBrowserActions actions) {
    m_db = &db;
    m_thumbs = &thumbs;
    m_prefs = &prefs;
    m_log = &log;
    m_actions = std::move(actions);
    m_root = db.sourceRoot();
    std::error_code ec;
    fs::create_directories(m_root, ec);
    m_tree.rel.clear();
    m_tree.name = "Assets";
    m_thumbSize = prefs.getFloat("browser.thumb", 112.0f);
    m_treeWidth = prefs.getFloat("browser.tree", 190.0f);
    m_listView = prefs.getBool("browser.list", false);
    m_searchEverywhere = prefs.getBool("browser.searchAll", true);
    m_sort = static_cast<SortMode>(std::clamp(static_cast<int>(prefs.getFloat("browser.sort", 0.0f)), 0, 3));
    navigateTo(prefs.get("browser.folder", ""));
}

void ContentBrowser::savePrefs() {
    m_prefs->setFloat("browser.thumb", m_thumbSize);
    m_prefs->setFloat("browser.tree", m_treeWidth);
    m_prefs->setBool("browser.list", m_listView);
    m_prefs->setBool("browser.searchAll", m_searchEverywhere);
    m_prefs->setFloat("browser.sort", static_cast<float>(static_cast<int>(m_sort)));
    m_prefs->set("browser.folder", m_current);
}

fs::path ContentBrowser::absolute(const std::string& rel) const { return rel.empty() ? m_root : m_root / PathFromUtf8(rel); }

void ContentBrowser::refresh() {
    m_dirty = true;
    m_searchIndexTime = -1e9;
    std::function<void(TreeNode&)> invalidate = [&](TreeNode& n) {
        n.scanned = false;
        for (TreeNode& k : n.kids) invalidate(k);
    };
    invalidate(m_tree);
}

void ContentBrowser::navigateTo(const std::string& relFolder) {
    std::error_code ec;
    std::string rel = relFolder;
    if (!rel.empty() && !fs::is_directory(absolute(rel), ec)) rel.clear();
    if (m_historyPos < 0 || m_history[static_cast<size_t>(m_historyPos)] != rel) {
        m_history.resize(static_cast<size_t>(m_historyPos + 1));
        m_history.push_back(rel);
        m_historyPos = static_cast<int>(m_history.size()) - 1;
    }
    m_current = rel;
    m_selected.clear();
    m_anchor = -1;
    m_renaming.clear();
    m_dirty = true;
    savePrefs();
}

bool ContentBrowser::passesFilter(const Entry& e) const {
    if (e.isDir) return true;
    return (m_categoryMask & (1u << static_cast<unsigned>(e.type.category))) != 0;
}

void ContentBrowser::listFolder() {
    m_entries.clear();
    const fs::path dir = absolute(m_current);
    std::error_code ec;
    const std::string needle = lowerAscii(m_search);
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::directory_entry& de = *it;
        Entry e;
        e.name = PathToUtf8(de.path().filename());
        if (e.name.empty()) continue;
        const std::string ext = PathToUtf8(de.path().extension());
        e.ext = lowerAscii(ext.empty() ? ext : ext.substr(1));
        if (!m_showHidden && (e.name[0] == '.' || e.ext == "meta")) continue;
        std::error_code e2;
        e.isDir = de.is_directory(e2);
        e.stem = e.isDir ? e.name : PathToUtf8(de.path().stem());
        e.rel = m_current.empty() ? e.name : m_current + "/" + e.name;
        if (!e.isDir) e.size = de.file_size(e2);
        e.mtime = toTicks(de.last_write_time(e2));
        e.type = e.isDir ? FolderType() : ClassifyExtension(e.ext);
        if (!passesFilter(e) || !containsNoCase(e.name, needle)) continue;
        m_entries.push_back(std::move(e));
    }
    m_dirStamp = fs::last_write_time(dir, ec);
}

void ContentBrowser::listSearch() {
    const double now = ImGui::GetTime();
    if (now - m_searchIndexTime > 5.0) {
        m_searchIndex.clear();
        std::error_code ec;
        for (fs::recursive_directory_iterator it(m_root, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end && m_searchIndex.size() < kMaxSearchIndex; it.increment(ec)) {
            const fs::directory_entry& de = *it;
            Entry e;
            e.name = PathToUtf8(de.path().filename());
            if (e.name.empty() || e.name[0] == '.') {
                if (de.is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
            const std::string ext = PathToUtf8(de.path().extension());
            e.ext = lowerAscii(ext.empty() ? ext : ext.substr(1));
            if (e.ext == "meta" && !m_showHidden) continue;
            std::error_code e2;
            e.isDir = de.is_directory(e2);
            e.stem = e.isDir ? e.name : PathToUtf8(de.path().stem());
            e.rel = PathToUtf8(de.path().lexically_relative(m_root));
            if (!e.isDir) e.size = de.file_size(e2);
            e.mtime = toTicks(de.last_write_time(e2));
            e.type = e.isDir ? FolderType() : ClassifyExtension(e.ext);
            m_searchIndex.push_back(std::move(e));
        }
        m_searchIndexTime = now;
    }
    m_entries.clear();
    const std::string needle = lowerAscii(m_search);
    for (const Entry& e : m_searchIndex) {
        if (passesFilter(e) && containsNoCase(e.name, needle)) m_entries.push_back(e);
        if (m_entries.size() >= 5000) break; // more matches than anyone will scroll through
    }
}

void ContentBrowser::scanTree(TreeNode& node) {
    std::vector<TreeNode> old = std::move(node.kids);
    node.kids.clear();
    std::error_code ec;
    for (fs::directory_iterator it(absolute(node.rel), fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code e2;
        if (!it->is_directory(e2)) continue;
        TreeNode kid;
        kid.name = PathToUtf8(it->path().filename());
        if (kid.name.empty() || kid.name[0] == '.') continue;
        kid.rel = node.rel.empty() ? kid.name : node.rel + "/" + kid.name;
        for (TreeNode& o : old) {
            if (o.rel == kid.rel) {
                kid.kids = std::move(o.kids);
                break;
            }
        }
        node.kids.push_back(std::move(kid));
    }
    std::sort(node.kids.begin(), node.kids.end(), [](const TreeNode& a, const TreeNode& b) { return lessNoCase(a.name, b.name); });
    node.scanned = true;
}

void ContentBrowser::tick() {
    const double now = ImGui::GetTime();
    static std::string lastSearch;
    if (lastSearch != m_search) {
        lastSearch = m_search;
        m_dirty = true;
    }
    if (now - m_lastCheck > 0.75) {
        m_lastCheck = now;
        std::error_code ec;
        const auto stamp = fs::last_write_time(absolute(m_current), ec);
        if (!ec && stamp != m_dirStamp) m_dirty = true;
        if (ec && !m_current.empty()) navigateTo(""); // the folder was deleted behind our back
    }
    if (m_dirty) {
        m_dirty = false;
        if (m_search[0] && m_searchEverywhere) listSearch();
        else listFolder();
        // Keep sorted.
        const SortMode mode = m_sort;
        std::stable_sort(m_entries.begin(), m_entries.end(), [mode](const Entry& a, const Entry& b) {
            if (a.isDir != b.isDir) return a.isDir;
            switch (mode) {
                case SortMode::Type:
                    if (a.ext != b.ext) return a.ext < b.ext;
                    break;
                case SortMode::Date:
                    if (a.mtime != b.mtime) return a.mtime > b.mtime;
                    break;
                case SortMode::Size:
                    if (a.size != b.size) return a.size > b.size;
                    break;
                default: break;
            }
            return lessNoCase(a.name, b.name);
        });
        // Drop selections that no longer exist.
        for (auto it = m_selected.begin(); it != m_selected.end();) {
            it = findEntry(*it) ? std::next(it) : m_selected.erase(it);
        }
    }
}

const ContentBrowser::Entry* ContentBrowser::findEntry(const std::string& rel) const {
    for (const Entry& e : m_entries)
        if (e.rel == rel) return &e;
    return nullptr;
}

bool ContentBrowser::isImported(const Entry& e) const { return e.type.importable && m_db->findSource(e.rel) != nullptr; }

void ContentBrowser::say(const std::string& message) { FOH_INFO("content browser: {}", message); }

// ---- window -----------------------------------------------------------------------------------------

void ContentBrowser::draw(bool* open, float scale) {
    m_scale = scale;
    if (!ImGui::Begin("Content Browser", open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }
    // File dialog answers (the Import button).
    std::vector<std::string> picked;
    if (TakeOpenFilesResult(picked)) {
        m_openedDialog = false;
        for (const std::string& f : picked) {
            if (m_actions.importFile) m_actions.importFile(PathFromUtf8(f));
        }
        refresh();
    }

    tick();
    drawToolbar(scale);
    ImGui::Separator();

    const float footerH = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
    const float logH = m_showLog ? ImGui::GetTextLineHeightWithSpacing() * 7.0f : 0.0f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float bodyH = std::max(avail.y - footerH - logH, 40.0f);

    // Folder tree.
    ImGui::BeginChild("##tree", ImVec2(m_treeWidth * scale, bodyH), ImGuiChildFlags_Borders);
    drawTree(m_tree, true);
    ImGui::EndChild();

    // Splitter.
    ImGui::SameLine(0, 0);
    ImGui::InvisibleButton("##split", ImVec2(6.0f * scale, bodyH));
    if (ImGui::IsItemActive()) {
        m_treeWidth = std::clamp(m_treeWidth + ImGui::GetIO().MouseDelta.x / scale, 90.0f, 500.0f);
        m_prefs->setFloat("browser.tree", m_treeWidth);
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    ImGui::SameLine(0, 0);

    ImGui::BeginChild("##content", ImVec2(0, bodyH), ImGuiChildFlags_Borders, ImGuiWindowFlags_None);
    drawContent(scale);
    ImGui::EndChild();

    if (m_showLog && m_log) {
        ImGui::BeginChild("##log", ImVec2(0, logH), ImGuiChildFlags_Borders);
        for (const std::string& line : *m_log) ImGui::TextUnformatted(line.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }

    drawFooter(scale);
    drawDialogs();
    handleShortcuts();
    ImGui::End();
}

void ContentBrowser::drawToolbar(float scale) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(EditorTheme::accent()));
    if (ImGui::Button("+ Import")) {
        ShowOpenFilesDialog("Models and textures", "gltf;glb;fbx;png;jpg;jpeg;tga;bmp", m_root);
        m_openedDialog = true;
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Import models (glTF, FBX) and textures into the project.\nYou can also drop files onto the editor window.");
    ImGui::SameLine();
    if (ImGui::Button("Workshop") && m_actions.openWorkshop) m_actions.openWorkshop();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Browse and download free assets (ambientCG, Poly Haven, Sketchfab)");
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // History and up.
    ImGui::BeginDisabled(m_historyPos <= 0);
    if (ImGui::ArrowButton("##back", ImGuiDir_Left)) {
        --m_historyPos;
        m_current = m_history[static_cast<size_t>(m_historyPos)];
        m_selected.clear();
        m_dirty = true;
        savePrefs();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_historyPos + 1 >= static_cast<int>(m_history.size()));
    if (ImGui::ArrowButton("##fwd", ImGuiDir_Right)) {
        ++m_historyPos;
        m_current = m_history[static_cast<size_t>(m_historyPos)];
        m_selected.clear();
        m_dirty = true;
        savePrefs();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_current.empty());
    if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
        const size_t slash = m_current.find_last_of('/');
        navigateTo(slash == std::string::npos ? std::string() : m_current.substr(0, slash));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();

    // Right-aligned controls first (so the breadcrumb can use the rest).
    const float searchW = 200.0f * scale;
    const ImGuiStyle& st = ImGui::GetStyle();
    const float rightW = searchW + ImGui::CalcTextSize("Filters").x + ImGui::CalcTextSize("Refresh").x + st.FramePadding.x * 4.0f +
                         st.ItemSpacing.x * 4.0f;
    const float crumbMax = ImGui::GetContentRegionAvail().x - rightW;

    ImGui::BeginChild("##crumbs", ImVec2(std::max(crumbMax, 60.0f), ImGui::GetFrameHeight()), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        std::string path;
        std::string target;
        if (ImGui::SmallButton("Assets")) navigateTo("");
        size_t pos = 0;
        while (pos < m_current.size()) {
            size_t slash = m_current.find('/', pos);
            if (slash == std::string::npos) slash = m_current.size();
            const std::string seg = m_current.substr(pos, slash - pos);
            target = m_current.substr(0, slash);
            ImGui::SameLine(0, 2.0f * scale);
            ImGui::TextDisabled(">");
            ImGui::SameLine(0, 2.0f * scale);
            ImGui::PushID(target.c_str());
            if (ImGui::SmallButton(seg.c_str())) {
                ImGui::PopID();
                navigateTo(target);
                break;
            }
            ImGui::PopID();
            pos = slash + 1;
        }
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(searchW);
    if (ImGui::InputTextWithHint("##search", "Search assets", m_search, sizeof m_search)) m_dirty = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Name search. With \"All folders\" checked in Filters it covers the whole project.");
    ImGui::SameLine();
    if (ImGui::Button("Filters")) ImGui::OpenPopup("##filters");
    if (ImGui::BeginPopup("##filters")) {
        static const struct { FileCategory cat; const char* name; } kCats[] = {
            {FileCategory::Model, "Models"}, {FileCategory::Image, "Textures / images"}, {FileCategory::Material, "Materials"},
            {FileCategory::Prefab, "Prefabs"}, {FileCategory::Scene, "Scenes"}, {FileCategory::Audio, "Audio"},
            {FileCategory::Shader, "Shaders"}, {FileCategory::Script, "Source"}, {FileCategory::Text, "Text"},
            {FileCategory::Archive, "Archives"}, {FileCategory::Other, "Other"}};
        ImGui::SeparatorText("Show");
        for (const auto& c : kCats) {
            bool on = (m_categoryMask & (1u << static_cast<unsigned>(c.cat))) != 0;
            if (ImGui::Checkbox(c.name, &on)) {
                m_categoryMask = on ? (m_categoryMask | (1u << static_cast<unsigned>(c.cat))) : (m_categoryMask & ~(1u << static_cast<unsigned>(c.cat)));
                m_dirty = true;
            }
        }
        if (ImGui::SmallButton("All")) {
            m_categoryMask = ~0u;
            m_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("None")) {
            m_categoryMask = 0;
            m_dirty = true;
        }
        ImGui::SeparatorText("Search");
        if (ImGui::Checkbox("All folders", &m_searchEverywhere)) {
            m_dirty = true;
            savePrefs();
        }
        if (ImGui::Checkbox("Show hidden / .meta", &m_showHidden)) {
            m_dirty = true;
            m_searchIndexTime = -1e9;
        }
        ImGui::SeparatorText("Sort by");
        static const char* kSorts[] = {"Name", "Type", "Date modified", "Size"};
        for (int i = 0; i < 4; ++i) {
            if (ImGui::RadioButton(kSorts[i], static_cast<int>(m_sort) == i)) {
                m_sort = static_cast<SortMode>(i);
                m_dirty = true;
                savePrefs();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) refresh();
}

void ContentBrowser::drawTree(TreeNode& node, bool isRoot) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (isRoot) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    if (node.rel == m_current && !(m_search[0] && m_searchEverywhere)) flags |= ImGuiTreeNodeFlags_Selected;
    if (node.scanned && node.kids.empty()) flags |= ImGuiTreeNodeFlags_Leaf;

    // The label starts with blanks that make room for the folder icon drawn over them.
    const float iconW = ImGui::GetFontSize() * 1.15f;
    const float spaceW = std::max(ImGui::CalcTextSize(" ").x, 1.0f);
    const std::string label = std::string(static_cast<size_t>(std::ceil(iconW / spaceW)) + 1, ' ') + node.name + "###dir:" + node.rel;

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 2.0f * m_scale));
    const bool opened = ImGui::TreeNodeEx(label.c_str(), flags);
    ImGui::PopStyleVar();
    const ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
    const float spacing = ImGui::GetTreeNodeToLabelSpacing();
    DrawFolderIcon(ImGui::GetWindowDrawList(), ImVec2(rmin.x + spacing, rmin.y), ImVec2(rmin.x + spacing + iconW, rmax.y),
                   FolderType().color, ImGui::IsItemHovered());
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) navigateTo(node.rel);
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetDragPayload)) {
            moveInto(static_cast<const char*>(p->Data), node.rel);
        }
        ImGui::EndDragDropTarget();
    }
    if (opened) {
        if (!node.scanned) scanTree(node);
        for (TreeNode& k : node.kids) drawTree(k, false);
        ImGui::TreePop();
    }
}

void ContentBrowser::drawContent(float scale) {
    if (m_entries.empty()) {
        const char* msg = m_search[0] ? "No assets match the search." : "This folder is empty.\nDrop files here, use Import, or open the Workshop.";
        const ImVec2 sz = ImGui::CalcTextSize(msg);
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPos(ImVec2((avail.x - sz.x) * 0.5f, avail.y * 0.35f));
        ImGui::TextDisabled("%s", msg);
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::InvisibleButton("##empty", ImGui::GetContentRegionAvail());
    } else if (m_listView) {
        drawList(scale);
    } else {
        drawGrid(scale);
    }
    backgroundContextMenu();
}

void ContentBrowser::drawGrid(float scale) {
    const float tile = std::round(m_thumbSize * scale);
    const float gap = 8.0f * scale;
    const float lineH = ImGui::GetTextLineHeight();
    const float textH = lineH * 3.0f + 8.0f * scale;
    const float availW = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize;
    const int cols = std::max(1, static_cast<int>((availW + gap) / (tile + gap)));
    const int count = static_cast<int>(m_entries.size());
    const int rows = (count + cols - 1) / cols;
    const float rowH = tile + textH + gap;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float pad = gap * 0.5f;

    // Ctrl+wheel zoom.
    if (ImGui::IsWindowHovered() && ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0.0f) {
        m_thumbSize = std::clamp(m_thumbSize + ImGui::GetIO().MouseWheel * 12.0f, 64.0f, 256.0f);
        m_prefs->setFloat("browser.thumb", m_thumbSize);
    }

    ImGuiListClipper clipper;
    clipper.Begin(rows, rowH);
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            for (int c = 0; c < cols; ++c) {
                const int i = r * cols + c;
                if (i >= count) break;
                const ImVec2 pos(origin.x + pad + static_cast<float>(c) * (tile + gap), origin.y + pad + static_cast<float>(r) * rowH);
                drawTile(m_entries[static_cast<size_t>(i)], static_cast<size_t>(i), pos, tile, textH, scale);
            }
        }
    }
    clipper.End();
    // The clipper reserved the full height; make sure the cursor is past the last row.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + static_cast<float>(rows) * rowH + pad));
    ImGui::Dummy(ImVec2(1, 1));
}

void ContentBrowser::drawTile(const Entry& e, size_t index, ImVec2 pos, float tile, float textH, float scale) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 size(tile, tile + textH);
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushID(e.rel.c_str());
    ImGui::InvisibleButton("##tile", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool selected = m_selected.count(e.rel) != 0;
    handleEntryInteraction(e, index);

    const float round = 6.0f * scale;
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    ImU32 bg = EditorTheme::tileBg();
    if (hovered) bg = IM_COL32(50, 53, 60, 255);
    if (selected) bg = IM_COL32(40, 64, 98, 255);
    dl->AddRectFilled(pos, max, bg, round);

    // Preview area: a square, slightly inset.
    const float inset = 4.0f * scale;
    const ImVec2 pa(pos.x + inset, pos.y + inset), pb(pos.x + tile - inset, pos.y + tile - inset);
    dl->AddRectFilled(pa, pb, IM_COL32(26, 28, 32, 255), round - 2.0f * scale);
    drawPreview(e, dl, pa, pb, hovered);

    // Type bar along the bottom of the preview, like Unreal's colored strip.
    const float barH = 3.0f * scale;
    dl->AddRectFilled(ImVec2(pa.x, pb.y - barH), pb, e.type.color, 0.0f);
    if (selected) dl->AddRect(pos, max, EditorTheme::accent(), round, 0, 2.0f * scale);

    // Badges: imported check (top-left), extension (top-right) when the preview hides the file type.
    if (e.type.importable) {
        const bool imported = isImported(e);
        const float r = 7.0f * scale;
        const ImVec2 c(pa.x + r + 3.0f * scale, pa.y + r + 3.0f * scale);
        dl->AddCircleFilled(c, r, imported ? IM_COL32(52, 168, 96, 235) : IM_COL32(110, 116, 128, 200));
        if (imported) {
            dl->AddLine(ImVec2(c.x - r * 0.45f, c.y), ImVec2(c.x - r * 0.1f, c.y + r * 0.4f), IM_COL32_WHITE, 1.6f * scale);
            dl->AddLine(ImVec2(c.x - r * 0.1f, c.y + r * 0.4f), ImVec2(c.x + r * 0.5f, c.y - r * 0.4f), IM_COL32_WHITE, 1.6f * scale);
        } else {
            dl->AddCircleFilled(c, r * 0.28f, IM_COL32_WHITE);
        }
    }
    if (!e.isDir && !e.ext.empty() && (e.type.imagePreview || e.type.modelPreview)) {
        std::string label = e.ext;
        for (char& ch : label) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        ImFont* font = ImGui::GetFont();
        const float fs = ImGui::GetFontSize() * 0.78f;
        const ImVec2 ts = font->CalcTextSizeA(fs, 1e9f, 0.0f, label.c_str());
        const ImVec2 pillMax(pb.x - 4.0f * scale, pa.y + 4.0f * scale + ts.y + 4.0f * scale);
        const ImVec2 pillMin(pillMax.x - ts.x - 10.0f * scale, pa.y + 4.0f * scale);
        dl->AddRectFilled(pillMin, pillMax, IM_COL32(12, 13, 15, 200), 4.0f * scale);
        dl->AddText(font, fs, ImVec2(pillMin.x + 5.0f * scale, pillMin.y + 2.0f * scale), IM_COL32(220, 224, 230, 255), label.c_str());
    }

    // Name (2 lines) and subtitle.
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const float tw = tile - 10.0f * scale;
    const float tx = pos.x + 5.0f * scale;
    const float ty = pos.y + tile + 3.0f * scale;
    const float lineH = ImGui::GetTextLineHeight();
    const bool renaming = m_renaming == e.rel;
    if (renaming) {
        ImGui::SetCursorScreenPos(ImVec2(tx, ty));
        ImGui::SetNextItemWidth(tw);
        if (m_renameFocus) {
            ImGui::SetKeyboardFocusHere();
            m_renameFocus = false;
        }
        ImGui::InputText("##rename", m_renameBuf, sizeof m_renameBuf, ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemDeactivated()) {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) m_renaming.clear();
            else commitRename(e, m_renameBuf);
        }
    } else {
        const std::string& nm = e.isDir ? e.name : e.stem;
        const char* s = nm.c_str();
        const char* end = s + nm.size();
        const char* l1 = font->CalcWordWrapPosition(fs, s, end, tw);
        if (l1 == s) l1 = s + 1;
        const std::string line1(s, l1);
        std::string line2(l1, end);
        const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
        auto centered = [&](const std::string& t, float y, ImU32 col) {
            const std::string fitted = ellipsize(font, fs, t, tw);
            const float w = font->CalcTextSizeA(fs, 1e9f, 0.0f, fitted.c_str()).x;
            dl->AddText(font, fs, ImVec2(tx + (tw - w) * 0.5f, y), col, fitted.c_str());
        };
        centered(line1, ty, textCol);
        if (!line2.empty()) centered(line2, ty + lineH, textCol);
        // Subtitle: ".ext" and type or size, dim.
        std::string sub;
        if (e.isDir) sub = "Folder";
        else if (e.type.category == FileCategory::Other && !e.ext.empty()) sub = "." + e.ext + " file";
        else sub = e.type.label;
        centered(sub, ty + lineH * 2.0f + 2.0f * scale, IM_COL32(128, 135, 148, 255));
    }

    // Drag source: the asset path.
    if (!e.isDir && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        ImGui::SetDragDropPayload(kAssetDragPayload, e.rel.c_str(), e.rel.size() + 1);
        ImGui::TextUnformatted(e.name.c_str());
        ImGui::TextDisabled("Drop onto a viewport to place it");
        ImGui::EndDragDropSource();
    }
    if (e.isDir && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        ImGui::SetDragDropPayload(kAssetDragPayload, e.rel.c_str(), e.rel.size() + 1);
        ImGui::Text("Folder %s", e.name.c_str());
        ImGui::EndDragDropSource();
    }
    if (e.isDir && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetDragPayload)) moveInto(static_cast<const char*>(p->Data), e.rel);
        ImGui::EndDragDropTarget();
    }

    if (hovered && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && !ImGui::IsMouseDown(0)) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(e.name.c_str());
        ImGui::TextDisabled("%s", e.type.label);
        if (!e.isDir) ImGui::TextDisabled("%s", FormatBytes(e.size).c_str());
        if (m_search[0] && m_searchEverywhere) ImGui::TextDisabled("%s", e.rel.c_str());
        ImGui::TextDisabled("Modified %s", formatTime(e.mtime).c_str());
        if (const SourceRecord* rec = e.type.importable ? m_db->findSource(e.rel) : nullptr) {
            u32 live = 0;
            for (const SubAssetRecord& s : rec->subAssets) live += s.tombstone ? 0 : 1;
            ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.55f, 1), "Imported: %u asset(s)", live);
        } else if (e.type.importable) {
            ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.4f, 1), "Not imported yet (double-click)");
        }
        ImGui::EndTooltip();
    }

    entryContextMenu(e);
    ImGui::PopID();
}

void ContentBrowser::drawPreview(const Entry& e, ImDrawList* dl, ImVec2 a, ImVec2 b, bool hovered) {
    if (e.isDir) {
        DrawFolderIcon(dl, a, b, e.type.color, hovered);
        return;
    }
    ThumbRect rect;
    ThumbnailService::State st = ThumbnailService::State::Unknown;
    u64 key = 0;
    if (e.type.imagePreview) {
        key = hashString(e.rel, 0x1111);
        key = hashValue(e.size, key);
        key = hashValue(e.mtime, key);
        st = m_thumbs->lookup(key, rect);
        if (st == ThumbnailService::State::Unknown) {
            m_thumbs->request(key, [path = absolute(e.rel)](ThumbPixels& out) { return LoadImageThumbnail(path, out); });
            st = ThumbnailService::State::Pending;
        }
    } else if (e.type.modelPreview) {
        if (const SourceRecord* rec = m_db->findSource(e.rel)) {
            AssetId prefab;
            for (const SubAssetRecord& s : rec->subAssets)
                if (!s.tombstone && s.kind == AssetKind::Prefab) prefab = s.guid;
            if (prefab.isValid()) {
                key = hashString(e.rel, 0x2222);
                key = hashValue(rec->contentHash, key);
                key = hashValue(rec->importerVersion, key);
                key = hashValue(prefab, key);
                st = m_thumbs->lookup(key, rect);
                if (st == ThumbnailService::State::Unknown) {
                    ModelThumbSource src;
                    if (const AssetInfo* pi = m_db->find(prefab)) src.prefabFile = pi->file;
                    for (const SubAssetRecord& s : rec->subAssets) {
                        if (s.tombstone || (s.kind != AssetKind::Mesh && s.kind != AssetKind::Material)) continue;
                        if (const AssetInfo* ai = m_db->find(s.guid)) src.files.emplace(s.guid, ai->file);
                    }
                    m_thumbs->request(key, [src = std::move(src)](ThumbPixels& out) { return RenderModelThumbnail(src, out); });
                    st = ThumbnailService::State::Pending;
                }
            }
        }
    }

    if (st == ThumbnailService::State::Ready) {
        const float w = b.x - a.x, h = b.y - a.y;
        float iw = w, ih = h;
        if (rect.aspect >= 1.0f) ih = w / rect.aspect;
        else iw = h * rect.aspect;
        const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        const ImVec2 p0(c.x - iw * 0.5f, c.y - ih * 0.5f), p1(c.x + iw * 0.5f, c.y + ih * 0.5f);
        dl->AddImage(ImTextureRef(rect.texture), p0, p1, rect.uv0, rect.uv1);
        return;
    }
    // No preview (yet): the typed icon, with a spinner while a preview is on its way.
    DrawFileIcon(dl, a, b, e.type.color, e.ext);
    if (st == ThumbnailService::State::Pending) {
        const float t = static_cast<float>(ImGui::GetTime()) * 5.0f;
        const float r = (b.x - a.x) * 0.06f;
        const ImVec2 c(b.x - r * 2.2f, b.y - r * 2.6f);
        dl->PathArcTo(c, r, t, t + 4.4f, 18);
        dl->PathStroke(IM_COL32(200, 206, 216, 220), 0, 2.0f * m_scale);
    }
}

void ContentBrowser::drawList(float scale) {
    const float rowH = ImGui::GetFrameHeight() + 4.0f * scale;
    const float iconS = rowH - 4.0f * scale;
    if (!ImGui::BeginTable("##list", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 150.0f * scale);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f * scale);
    ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 130.0f * scale);
    ImGui::TableHeadersRow();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_entries.size()), rowH);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const Entry& e = m_entries[static_cast<size_t>(i)];
            ImGui::TableNextRow(ImGuiTableRowFlags_None, rowH);
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(e.rel.c_str());
            const bool selected = m_selected.count(e.rel) != 0;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Selectable("##row", selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick,
                              ImVec2(0, rowH));
            const bool hovered = ImGui::IsItemHovered();
            handleEntryInteraction(e, static_cast<size_t>(i));
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                ImGui::SetDragDropPayload(kAssetDragPayload, e.rel.c_str(), e.rel.size() + 1);
                ImGui::TextUnformatted(e.name.c_str());
                ImGui::EndDragDropSource();
            }
            if (e.isDir && ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kAssetDragPayload)) moveInto(static_cast<const char*>(pl->Data), e.rel);
                ImGui::EndDragDropTarget();
            }
            entryContextMenu(e);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 ia(p.x, p.y + 2.0f * scale), ib(p.x + iconS, p.y + 2.0f * scale + iconS);
            if (e.isDir) DrawFolderIcon(dl, ia, ib, e.type.color, hovered);
            else DrawFileIcon(dl, ia, ib, e.type.color, e.ext);
            ImGui::SameLine(0, 0);
            ImGui::SetCursorScreenPos(ImVec2(p.x + iconS + 8.0f * scale, p.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f - 2.0f * scale));
            ImGui::TextUnformatted(e.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", e.type.label);
            ImGui::TableSetColumnIndex(2);
            if (!e.isDir) ImGui::TextDisabled("%s", FormatBytes(e.size).c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextDisabled("%s", formatTime(e.mtime).c_str());
            ImGui::PopID();
        }
    }
    clipper.End();
    ImGui::EndTable();
}

void ContentBrowser::drawFooter(float scale) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%zu item%s", m_entries.size(), m_entries.size() == 1 ? "" : "s");
    if (!m_selected.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%zu selected)", m_selected.size());
        if (m_selected.size() == 1) {
            if (const Entry* e = findEntry(*m_selected.begin())) {
                ImGui::SameLine();
                ImGui::TextDisabled(" |  %s  %s", e->rel.c_str(), e->isDir ? "" : FormatBytes(e->size).c_str());
            }
        }
    }

    const float sliderW = 110.0f * scale;
    const ImGuiStyle& st = ImGui::GetStyle();
    const float box = ImGui::GetFrameHeight() + st.ItemInnerSpacing.x;
    const float rightW = sliderW + box * 2.0f + ImGui::CalcTextSize("List").x + ImGui::CalcTextSize("Log").x + st.ItemSpacing.x * 2.0f;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - rightW);
    if (ImGui::Checkbox("Log", &m_showLog)) {}
    ImGui::SameLine();
    if (ImGui::Checkbox("List", &m_listView)) m_prefs->setBool("browser.list", m_listView);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(sliderW);
    if (ImGui::SliderFloat("##zoom", &m_thumbSize, 64.0f, 256.0f, "")) m_prefs->setFloat("browser.thumb", m_thumbSize);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Thumbnail size (Ctrl + mouse wheel over the grid)");
}

// ---- interaction -------------------------------------------------------------------------------------------

void ContentBrowser::select(const Entry& e, size_t index) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyShift && m_anchor >= 0) {
        const size_t a = static_cast<size_t>(m_anchor), b = index;
        if (!io.KeyCtrl) m_selected.clear();
        for (size_t i = std::min(a, b); i <= std::max(a, b) && i < m_entries.size(); ++i) m_selected.insert(m_entries[i].rel);
    } else if (io.KeyCtrl) {
        if (!m_selected.erase(e.rel)) m_selected.insert(e.rel);
        m_anchor = static_cast<int>(index);
    } else {
        m_selected.clear();
        m_selected.insert(e.rel);
        m_anchor = static_cast<int>(index);
    }
}

void ContentBrowser::handleEntryInteraction(const Entry& e, size_t index) {
    // Press selects (like a file manager); right-click selects too unless the item is already part of the selection.
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        select(e, index);
        if (m_renaming != e.rel) m_renaming.clear();
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !m_selected.count(e.rel)) {
        m_selected.clear();
        m_selected.insert(e.rel);
        m_anchor = static_cast<int>(index);
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && m_renaming.empty()) open(e);
}

void ContentBrowser::open(const Entry& e) {
    if (e.isDir) {
        // Searching across folders: jump to the folder, clear the search.
        if (m_search[0] && m_searchEverywhere) m_search[0] = '\0';
        navigateTo(e.rel);
        return;
    }
    if (e.type.modelPreview || e.type.category == FileCategory::Model) {
        if (m_actions.placeModel) m_actions.placeModel(e.rel);
        refresh();
    } else if (e.type.importable) {
        if (m_actions.importFile) m_actions.importFile(absolute(e.rel));
        refresh();
    } else {
        OpenWithDefaultApp(absolute(e.rel));
    }
}

void ContentBrowser::entryContextMenu(const Entry& e) {
    if (!ImGui::BeginPopupContextItem("##ctx", ImGuiPopupFlags_MouseButtonRight)) return;
    const bool multi = m_selected.size() > 1;
    if (e.isDir) {
        if (ImGui::MenuItem("Open")) open(e);
    } else if (e.type.category == FileCategory::Model) {
        if (ImGui::MenuItem("Place in scene")) {
            if (m_actions.placeModel) m_actions.placeModel(e.rel);
            refresh();
        }
        if (ImGui::MenuItem(isImported(e) ? "Reimport" : "Import")) {
            if (m_actions.importFile) m_actions.importFile(absolute(e.rel));
            refresh();
        }
    } else if (e.type.importable) {
        if (ImGui::MenuItem(isImported(e) ? "Reimport" : "Import")) {
            if (m_actions.importFile) m_actions.importFile(absolute(e.rel));
            refresh();
        }
    } else if (ImGui::MenuItem("Open with default app")) {
        OpenWithDefaultApp(absolute(e.rel));
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Show in Explorer")) RevealInFileManager(absolute(e.rel));
    if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText(PathToUtf8(absolute(e.rel)).c_str());
    ImGui::Separator();
    if (ImGui::MenuItem("Rename", "F2", false, !multi)) beginRename(e);
    if (ImGui::MenuItem("Delete", "Del")) {
        m_deleteList.assign(m_selected.begin(), m_selected.end());
        m_confirmDelete = true;
    }
    ImGui::EndPopup();
}

void ContentBrowser::backgroundContextMenu() {
    if (!ImGui::BeginPopupContextWindow("##bgctx", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) return;
    if (ImGui::MenuItem("New folder")) createFolder();
    if (ImGui::MenuItem("Import...")) {
        ShowOpenFilesDialog("Models and textures", "gltf;glb;fbx;png;jpg;jpeg;tga;bmp", m_root);
        m_openedDialog = true;
    }
    if (ImGui::MenuItem("Open Workshop") && m_actions.openWorkshop) m_actions.openWorkshop();
    ImGui::Separator();
    if (ImGui::MenuItem("Show in Explorer")) RevealInFileManager(absolute(m_current));
    if (ImGui::MenuItem("Refresh")) refresh();
    ImGui::EndPopup();
}

void ContentBrowser::handleShortcuts() {
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || ImGui::GetIO().WantTextInput) return;
    if (ImGui::IsKeyPressed(ImGuiKey_F2) && m_selected.size() == 1) {
        if (const Entry* e = findEntry(*m_selected.begin())) beginRename(*e);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !m_selected.empty()) {
        m_deleteList.assign(m_selected.begin(), m_selected.end());
        m_confirmDelete = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_A) && ImGui::GetIO().KeyCtrl) {
        for (const Entry& e : m_entries) m_selected.insert(e.rel);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) m_selected.clear();
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !m_current.empty()) {
        const size_t slash = m_current.find_last_of('/');
        navigateTo(slash == std::string::npos ? std::string() : m_current.substr(0, slash));
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && m_selected.size() == 1) {
        if (const Entry* e = findEntry(*m_selected.begin())) open(*e);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F5)) refresh();
}

// ---- operations ---------------------------------------------------------------------------------------------

void ContentBrowser::beginRename(const Entry& e) {
    m_renaming = e.rel;
    m_renameFocus = true;
    std::snprintf(m_renameBuf, sizeof m_renameBuf, "%s", (e.isDir ? e.name : e.stem).c_str());
}

void ContentBrowser::afterFilesChanged() {
    m_db->scan(); // source records are keyed by path
    refresh();
}

void ContentBrowser::commitRename(const Entry& e, const std::string& newStem) {
    m_renaming.clear();
    if (!validFileName(newStem)) {
        say("invalid name '" + newStem + "'");
        return;
    }
    const std::string newName = e.isDir || e.ext.empty() ? newStem : newStem + "." + e.name.substr(e.name.size() - e.ext.size());
    if (newName == e.name) return;
    const fs::path from = absolute(e.rel);
    const fs::path to = from.parent_path() / PathFromUtf8(newName);
    std::error_code ec;
    if (fs::exists(to, ec)) {
        say("'" + newName + "' already exists");
        return;
    }
    fs::rename(from, to, ec);
    if (ec) {
        say("rename failed: " + ec.message());
        return;
    }
    const fs::path metaFrom = from.string() + ".meta", metaTo = to.string() + ".meta";
    if (fs::exists(metaFrom, ec)) fs::rename(metaFrom, metaTo, ec);
    m_selected.clear();
    m_selected.insert(m_current.empty() ? newName : m_current + "/" + newName);
    afterFilesChanged();
}

void ContentBrowser::createFolder() {
    std::error_code ec;
    for (int i = 0; i < 1000; ++i) {
        const std::string name = i == 0 ? "New Folder" : "New Folder " + std::to_string(i);
        const fs::path p = absolute(m_current) / name;
        if (fs::exists(p, ec)) continue;
        if (fs::create_directory(p, ec)) {
            afterFilesChanged();
            const std::string rel = m_current.empty() ? name : m_current + "/" + name;
            m_selected.clear();
            m_selected.insert(rel);
            if (const Entry* e = findEntry(rel)) beginRename(*e);
        }
        return;
    }
}

bool ContentBrowser::moveInto(const std::string& srcRel, const std::string& dstFolderRel) {
    const fs::path from = absolute(srcRel);
    const fs::path destDir = absolute(dstFolderRel);
    const fs::path to = destDir / from.filename();
    std::error_code ec;
    if (from.parent_path() == destDir) return false;
    // A folder must not move into itself or its own subtree.
    const std::string srcPrefix = srcRel + "/";
    if (dstFolderRel == srcRel || dstFolderRel.rfind(srcPrefix, 0) == 0) return false;
    if (fs::exists(to, ec)) {
        say("'" + PathToUtf8(from.filename()) + "' already exists in the target folder");
        return false;
    }
    fs::rename(from, to, ec);
    if (ec) {
        say("move failed: " + ec.message());
        return false;
    }
    const fs::path metaFrom = from.string() + ".meta";
    if (fs::exists(metaFrom, ec)) fs::rename(metaFrom, to.string() + ".meta", ec);
    afterFilesChanged();
    return true;
}

void ContentBrowser::deleteSelection() {
    std::error_code ec;
    for (const std::string& rel : m_deleteList) {
        if (rel.empty() || rel.find("..") != std::string::npos) continue; // never leave the asset root
        const fs::path p = absolute(rel);
        fs::remove_all(p, ec);
        fs::remove(p.string() + ".meta", ec);
        if (ec) say("could not delete '" + rel + "': " + ec.message());
    }
    m_deleteList.clear();
    m_selected.clear();
    afterFilesChanged();
}

void ContentBrowser::drawDialogs() {
    if (m_confirmDelete) {
        ImGui::OpenPopup("Delete assets?");
        m_confirmDelete = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Delete assets?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete %zu item(s) from disk? This cannot be undone.", m_deleteList.size());
        ImGui::Separator();
        const size_t shown = std::min<size_t>(m_deleteList.size(), 8);
        for (size_t i = 0; i < shown; ++i) ImGui::BulletText("%s", m_deleteList[i].c_str());
        if (m_deleteList.size() > shown) ImGui::TextDisabled("... and %zu more", m_deleteList.size() - shown);
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.2f, 0.2f, 1));
        if (ImGui::Button("Delete", ImVec2(110 * m_scale, 0))) {
            deleteSelection();
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110 * m_scale, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_deleteList.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace foh
