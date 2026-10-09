#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "foh/asset_db.h"
#include "foh/dev_builder.h"
#include "foh/ecs.h"
#include "foh/game_module_host.h"
#include "foh/jobs.h"
#include "foh/renderer/renderer.h"
#include "foh/rhi/swapchain.h"
#include "foh/scene/asset_scene_bridge.h"
#include "foh/window.h"
#include "atmosphere_panel.h"
#include "asset_inspector.h"
#include "asset_picker.h"
#include "content_browser.h"
#include "editor_prefs.h"
#include "fly_camera.h"
#include "imgui_platform.h"
#include "imgui_renderer.h"
#include "inspector.h"
#include "prefab_editor.h"
#include "theme.h"
#include "thumbnails.h"
#include "workshop/workshop.h"

struct ImGuiContext;
struct ImVec2;

namespace foh {

struct EditorOptions {
    u32 frames = 0;            // 0: run until closed; N: render N frames and exit (smoke runs)
    std::string screenshot;    // PNG written after the last frame (needs frames > 0)
    bool validation =
#if defined(NDEBUG)
        false;
#else
        true;
#endif
    bool vsync = true;
    // QA mode: resizes the window and viewports, switches scenes and toggles panels on a
    // schedule to exercise every resource-recreation path under the validation layer.
    bool stress = false;
    u32 minPassesPerGroup = 4; // --group-passes N (1 = one command list per pass)
    u32 windowWidth = 1600, windowHeight = 900; // --window WxH (QA: render a 4K-sized layout)
    bool workshopTest = false; // --workshop-test: download + import the first result with the smallest variant (QA)
    bool openWorkshop = false; // --workshop: open the assets workshop at startup (QA)
    std::string importFile;    // --import <file>: imports it at startup and places it like a drop (QA)
    std::string prefabEdit;    // --prefab-edit <model>: opens the prefab editor on an imported model (QA)
    bool prefabQa = false;     // --prefab-qa: with --prefab-edit, fits/rotates/saves/places the model like a user would (QA)
    std::string inspect;       // --inspect <path>: shows a file in the asset inspector (QA)
};

// The editor: one window, one renderer, any number of scenes (ECS worlds) and
// viewport panels. A viewport panel shows ONE scene through ONE camera and is
// backed by its own render graph, so the three default panels are:
//   Scene            the main world, free-fly editor camera, grid
//   Game             the main world, the camera entity the game defines
//   Animation Debug  a second, independent world, its own editor camera
class Editor {
public:
    explicit Editor(EditorOptions options);
    ~Editor();
    Editor(const Editor&) = delete;
    Editor& operator=(const Editor&) = delete;

    // Returns the process exit code.
    int run();

private:
    struct SceneEntry {
        std::string name;
        std::unique_ptr<App> app;
        SceneId renderScene;
    };
    struct ViewportPanel {
        std::string title;
        ViewportId id;
        size_t sceneIndex = 0;
        bool useEditorCamera = true;
        bool grid = true;
        bool sky = true;
        bool open = true;
        bool capturing = false; // RMB look/fly in progress
        FlyCamera camera;
        u32 wantWidth = 0, wantHeight = 0;
        u32 stableFrames = 0;
        bool sizeApplied = false;
        AssetId pendingMaterial; // dropped on the view: applied to whatever the next pick returns
        float initialW = 0.0f, initialH = 0.0f; // logical size when the panel floats the first time (0: dock default)
        // Where the image was drawn last frame (logical window coordinates): drops land by it.
        float imageX = 0.0f, imageY = 0.0f, imageW = 0.0f, imageH = 0.0f;
    };

    bool init();
    void shutdown();
    bool frame(float dt);
    void buildUi(float dt);
    void buildDockLayout(unsigned int dockspaceId);
    void drawMenuBar();
    void drawViewport(ViewportPanel& panel, float dt);
    void drawRenderGraphPanel();
    void drawRenderSettings();
    void loadRenderSettings();
    void drawStatsPanel(float dt);
    void saveScreenshot();
    void initAssets();
    // Starts an import (returns its ticket, 0 when the file is not importable).
    ImportTicket importFile(const std::filesystem::path& file, float dropX, float dropY, bool hasDropPosition, bool place = true);
    // Materials authored from imported texture sets: created when the last of their textures is in the database.
    void queueMaterial(const MaterialRecipe& recipe, const std::vector<ImportTicket>& tickets);
    void createMaterialAsset(const MaterialRecipe& recipe);
    // Puts a material on a mesh entity / on the selected one.
    void applyMaterial(AssetId material, size_t sceneIndex, Entity entity);
    void applyMaterialToSelection(const std::string& relPath);
    // Places a model of the asset folder (a path relative to it), importing it first when needed.
    void placeAssetByPath(const std::string& relPath, float x, float y, bool hasPosition);
    void handleImportResult(const ImportResult& result);
    // Per frame: starts reimports of changed sources, commits finished imports, streams loaded assets to the GPU.
    void updateAssets(float dt);
    void logAsset(std::string message);
    void instantiateAsset(AssetId prefab, size_t sceneIndex, const Transform& at);
    // Where a dropped model goes: the ground point under the cursor in the viewport it was dropped on.
    Transform dropPlacement(float x, float y, bool hasPosition, size_t& sceneIndex) const;
    void createDebugScene();
    void createPreviewScene();
    void openPrefabEditor(const std::string& relPath);
    // Prefab editor: replaces what the preview scene shows with `prefab`.
    void showPrefabPreview(const PrefabAsset& prefab);
    void framePreview(Vec3 center, float radius);
    void runStress();
    void drawGizmo(ViewportPanel& panel, const ImVec2& origin, const ImVec2& size, int panelIndex);
    void handlePicking(ViewportPanel& panel, const ImVec2& origin, const ImVec2& size, bool hovered);

    static void buildWindowPasses(RenderGraph& graph, RgTexture backbuffer, void* user);

    EditorOptions m_options;
    std::unique_ptr<Window> m_window;
    std::unique_ptr<JobSystem> m_jobs;
    std::unique_ptr<Renderer> m_renderer;
    std::unique_ptr<Swapchain> m_swapchain;
    ImGuiContext* m_imgui = nullptr;
    EditorTheme m_theme;
    EditorPrefs m_prefs;
    ThumbnailService m_thumbs;
    ContentBrowser m_browser;
    Workshop m_workshop;
    ImGuiPlatform m_platform;
    ImGuiRenderer m_imguiRenderer;
    std::vector<nvrhi::ITexture*> m_externalTextures; // viewport images drawn this frame

    std::vector<SceneEntry> m_scenes; // [0] = main world with the hot-reloadable game
    std::unique_ptr<GameModuleHost> m_game;
    std::optional<DevBuilder> m_gameBuilder;
    std::optional<DevBuilder> m_shaderBuilder;
    std::vector<ViewportPanel> m_viewports;

    // Assets: the database of imported sources and the glue that puts them into worlds.
    std::unique_ptr<AssetDb> m_assets;
    std::unique_ptr<AssetSceneBridge> m_assetBridge;
    // Where a model goes once its import finishes (the cursor position of the drop).
    struct Placement {
        ImportTicket ticket = 0;
        float x = 0.0f, y = 0.0f;
        bool hasPosition = false;
        bool place = true;
        std::chrono::steady_clock::time_point started;
    };
    std::vector<Placement> m_placements;
    struct PendingMaterial {
        MaterialRecipe recipe;
        std::vector<ImportTicket> waiting;
        bool failed = false;
    };
    std::vector<PendingMaterial> m_pendingMaterials;
    std::vector<std::string> m_assetLog; // newest last, capped
    float m_sinceAssetPoll = 0.0f;
    // Frame times while assets stream in (imports, loads, texture uploads): reported once when streaming ends.
    struct StreamingStats {
        bool active = false;
        u32 frames = 0;
        double sumMs = 0.0, worstMs = 0.0;
        double worstPhaseMs = 0.0;
        const char* worstPhase = "";
    } m_streaming;
    bool m_showAssets = true;
    // Asset reference widgets, the asset inspector and the prefab editor (with its own scene and viewport).
    AssetPicker m_picker;
    AssetInspector m_assetInspector;
    PrefabEditor m_prefabEditor;
    bool m_showAssetInspector = true;
    bool m_showPrefabEditor = false;
    bool m_prefabEditorWasShown = false;
    std::string m_lastBrowserSelection;
    size_t m_previewScene = 0;
    size_t m_previewViewport = 0;
    Entity m_previewRoot{};
    bool m_showWorkshop = false;

    bool m_dockLayoutBuilt = false;
    bool m_showRenderGraph = true;
    bool m_showStats = true;
    bool m_showAtmosphere = false;
    AtmospherePanel m_atmospherePanel;
    bool m_showEntities = true;
    bool m_showInspector = true;
    EntityInspector m_inspector;
    Selection m_selection;
    std::vector<SceneRef> m_sceneRefs;
    int m_gizmoOp = 7;       // ImGuizmo::TRANSLATE
    bool m_gizmoLocal = false;
    bool m_vsync = true;
    float m_avgFrameMs = 16.0f;
    u32 m_frameCount = 0;
    std::string m_iniPath;
    bool m_quit = false;
};

} // namespace foh
