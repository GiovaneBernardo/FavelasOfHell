#pragma once

// The content browser: a folder tree plus a thumbnail grid (or list) over the asset source root,
// in the spirit of Unreal's. Files show their name and extension, a real preview when one can be
// produced (image decode, rasterized model) and a typed icon otherwise.
//
// Performance: the grid is clipped to the visible rows, directory listings are cached and only
// refreshed when the folder's modification time changes, and thumbnails are produced
// asynchronously and drawn from shared atlas textures (thumbnails.h).
//
// Thread-safety: main thread only.

#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "editor_prefs.h"
#include "file_icons.h"
#include "foh/asset_db.h"
#include "thumbnails.h"

namespace foh {

struct ContentBrowserActions {
    // Imports `file` (anywhere on disk; copied into the project when outside it) without placing it.
    std::function<void(const std::filesystem::path& file)> importFile;
    // Imports the source (a path relative to the asset root) when needed and places it into the active scene.
    std::function<void(const std::string& relPath)> placeModel;
    // Puts a material (path relative to the asset root) on the selected scene object.
    std::function<void(const std::string& relPath)> applyMaterial;
    std::function<void()> openWorkshop;
    // Opens the prefab editor on a model or a prefab variant (path relative to the asset root).
    std::function<void(const std::string& relPath)> editPrefab;
    // Makes a model the one shown by the selected entity (PrefabSlot), or by the game's player (player = true).
    std::function<void(const std::string& relPath, bool player)> useAsModel;
};

// Drag-and-drop payload type: a null-terminated source path relative to the asset root.
inline constexpr const char* kAssetDragPayload = "FOH_ASSET_PATH";

class ContentBrowser {
public:
    void init(AssetDb& db, ThumbnailService& thumbs, EditorPrefs& prefs, const std::vector<std::string>& log,
              ContentBrowserActions actions);
    // Draws the window. `scale` is the editor's UI scale.
    void draw(bool* open, float scale);
    // Re-reads the current folder and the tree on the next frame (call after imports and downloads).
    void refresh();
    void navigateTo(const std::string& relFolder);
    // The one selected file or folder (relative to the root); empty when nothing or several are selected.
    std::string singleSelection() const { return m_selected.size() == 1 ? *m_selected.begin() : std::string(); }

private:
    struct Entry {
        std::string name;  // file name with extension
        std::string stem;  // without extension
        std::string ext;   // lowercase, no dot
        std::string rel;   // relative to the root, '/' separators
        bool isDir = false;
        u64 size = 0;
        i64 mtime = 0;
        FileTypeInfo type;
    };
    struct TreeNode {
        std::string rel;
        std::string name;
        std::vector<TreeNode> kids;
        bool scanned = false;
    };
    enum class SortMode : int { Name, Type, Date, Size };

    // listing
    void listFolder();
    void listSearch();
    void scanTree(TreeNode& node);
    void tick();
    bool passesFilter(const Entry& e) const;
    std::filesystem::path absolute(const std::string& rel) const;

    // ui
    void drawToolbar(float scale);
    void drawTree(TreeNode& node, bool isRoot);
    void drawContent(float scale);
    void drawGrid(float scale);
    void drawList(float scale);
    void drawFooter(float scale);
    void drawTile(const Entry& e, size_t index, ImVec2 pos, float tile, float textH, float scale);
    void drawPreview(const Entry& e, ImDrawList* dl, ImVec2 a, ImVec2 b, bool hovered);
    void handleEntryInteraction(const Entry& e, size_t index);
    void entryContextMenu(const Entry& e);
    void backgroundContextMenu();
    void drawDialogs();
    void handleShortcuts();
    void savePrefs();

    // operations
    void open(const Entry& e);
    void select(const Entry& e, size_t index);
    void beginRename(const Entry& e);
    void commitRename(const Entry& e, const std::string& newName);
    void createFolder();
    void deleteSelection();
    bool moveInto(const std::string& srcRel, const std::string& dstFolderRel);
    void afterFilesChanged();
    void say(const std::string& message);
    bool isImported(const Entry& e) const;
    const Entry* findEntry(const std::string& rel) const;

    AssetDb* m_db = nullptr;
    ThumbnailService* m_thumbs = nullptr;
    EditorPrefs* m_prefs = nullptr;
    const std::vector<std::string>* m_log = nullptr;
    ContentBrowserActions m_actions;
    std::filesystem::path m_root;

    std::string m_current; // folder shown, relative to the root ("" = root)
    std::vector<std::string> m_history;
    int m_historyPos = -1;
    std::vector<Entry> m_entries; // what is shown (folder listing or search results)
    std::vector<Entry> m_searchIndex;
    double m_searchIndexTime = -1e9;
    std::filesystem::file_time_type m_dirStamp{};
    double m_lastCheck = 0.0;
    bool m_dirty = true;
    TreeNode m_tree;

    std::set<std::string> m_selected;
    int m_anchor = -1;
    std::string m_renaming;
    bool m_renameFocus = false;
    char m_renameBuf[256] = {};
    bool m_confirmDelete = false;
    std::vector<std::string> m_deleteList;

    char m_search[128] = {};
    bool m_searchEverywhere = true;
    unsigned m_categoryMask = ~0u; // visible FileCategory bits
    SortMode m_sort = SortMode::Name;
    bool m_listView = false;
    float m_thumbSize = 112.0f; // logical px
    float m_treeWidth = 190.0f; // logical px
    bool m_showLog = false;
    bool m_showHidden = false;

    std::string m_hoverInfo;
    bool m_openedDialog = false;
    float m_scale = 1.0f;
};

} // namespace foh
