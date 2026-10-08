#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "foh/dev_builder.h"
#include "foh/ecs.h"
#include "foh/game_module_host.h"
#include "foh/jobs.h"
#include "foh/renderer/renderer.h"
#include "foh/rhi/swapchain.h"
#include "foh/window.h"
#include "fly_camera.h"
#include "imgui_platform.h"
#include "imgui_renderer.h"
#include "inspector.h"

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
        bool open = true;
        bool capturing = false; // RMB look/fly in progress
        FlyCamera camera;
        u32 wantWidth = 0, wantHeight = 0;
        u32 stableFrames = 0;
        bool sizeApplied = false;
    };

    bool init();
    void shutdown();
    bool frame(float dt);
    void buildUi(float dt);
    void buildDockLayout(unsigned int dockspaceId);
    void drawMenuBar();
    void drawViewport(ViewportPanel& panel, float dt);
    void drawRenderGraphPanel();
    void drawStatsPanel(float dt);
    void saveScreenshot();
    void createDebugScene();
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
    ImGuiPlatform m_platform;
    ImGuiRenderer m_imguiRenderer;
    std::vector<nvrhi::ITexture*> m_externalTextures; // viewport images drawn this frame

    std::vector<SceneEntry> m_scenes; // [0] = main world with the hot-reloadable game
    std::unique_ptr<GameModuleHost> m_game;
    std::optional<DevBuilder> m_gameBuilder;
    std::optional<DevBuilder> m_shaderBuilder;
    std::vector<ViewportPanel> m_viewports;

    bool m_dockLayoutBuilt = false;
    bool m_showRenderGraph = true;
    bool m_showStats = true;
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
