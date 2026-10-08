#include "editor.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <thread>

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder: the default layout
#include <ImGuizmo.h>

#include "foh/dev_build_defaults.h"
#include "foh/file_system.h"
#include "foh/log.h"
#include "foh/renderer/components.h"
#include "foh/rhi/pipeline_cache.h"
#include "foh/rhi/render_graph.h"
#include "foh/png.h"

namespace foh::editor {

// ---- demo content of the "Animation Debug" scene (a second, independent world) ----

struct DebugBob {
    f32 phase = 0.0f;
    f32 baseY = 1.0f;
    f32 amplitude = 0.5f;
};

} // namespace foh::editor

FOH_REFLECT(foh::editor::DebugBob, phase, baseY, amplitude);

namespace foh {

namespace {

namespace fs = std::filesystem;

using editor::DebugBob;

void animateDebugRig(Res<Time> time, Query<Transform&, const DebugBob&> bobbers) {
    const f32 t = static_cast<f32>(time->elapsed);
    bobbers.parEach([t](Transform& tr, const DebugBob& b) {
        tr.position.y = b.baseY + std::sin(t * 2.0f + b.phase) * b.amplitude;
        tr.rotation = glm::angleAxis(t * 0.7f + b.phase, Vec3(0, 1, 0));
    });
}

ImVec4 col(float r, float g, float b, float a = 1.0f) { return ImVec4(r, g, b, a); }

void applyStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.FrameRounding = 3.0f;
    s.TabRounding = 3.0f;
    s.WindowBorderSize = 1.0f;
    s.FramePadding = ImVec2(6, 3);
    s.ItemSpacing = ImVec2(8, 4);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = col(0.10f, 0.105f, 0.11f);
    c[ImGuiCol_TitleBg] = col(0.08f, 0.085f, 0.09f);
    c[ImGuiCol_TitleBgActive] = col(0.12f, 0.13f, 0.14f);
    c[ImGuiCol_Tab] = col(0.13f, 0.14f, 0.15f);
    c[ImGuiCol_TabSelected] = col(0.22f, 0.30f, 0.38f);
    c[ImGuiCol_TabHovered] = col(0.28f, 0.42f, 0.55f);
    c[ImGuiCol_TabDimmed] = col(0.11f, 0.12f, 0.13f);
    c[ImGuiCol_TabDimmedSelected] = col(0.17f, 0.22f, 0.28f);
    c[ImGuiCol_Header] = col(0.20f, 0.28f, 0.36f, 0.6f);
    c[ImGuiCol_FrameBg] = col(0.16f, 0.17f, 0.18f);
}

} // namespace

Editor::Editor(EditorOptions options) : m_options(std::move(options)), m_vsync(m_options.vsync) {}

Editor::~Editor() { shutdown(); }

// ---- lifetime --------------------------------------------------------------------------

bool Editor::init() {
    WindowDesc wd;
    wd.title = "Favelas of Hell - Editor";
    wd.width = 1600;
    wd.height = 900;
    wd.maximized = m_options.frames == 0; // smoke runs keep a deterministic size
    m_window = Window::create(wd);
    if (!m_window) return false;

    m_jobs = std::make_unique<JobSystem>(JobSystem::defaultWorkerCount());
    FOH_INFO("job system: {} worker threads", m_jobs->workerCount());

    RendererDesc rd;
    rd.jobs = m_jobs.get();
    rd.validation = m_options.validation;
    rd.minPassesPerGroup = m_options.minPassesPerGroup;
    m_renderer = Renderer::create(rd);
    if (!m_renderer) return false;

    u32 w = 0, h = 0;
    m_window->getSize(w, h);
    SwapchainDesc sd;
    sd.windowHandle = m_window->nativeWindowHandle();
    sd.width = w;
    sd.height = h;
    sd.vsync = m_vsync;
    m_swapchain = Swapchain::create(m_renderer->device(), sd);
    if (!m_swapchain) return false;

    // ---- Dear ImGui ----
    IMGUI_CHECKVERSION();
    m_imgui = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    m_iniPath = (GetExecutableDirectory() / "editor_layout_v2.ini").string();
    io.IniFilename = m_iniPath.c_str();
    applyStyle();
    m_platform.init(*m_window);
    if (!m_imguiRenderer.init(*m_renderer)) return false;
    m_dockLayoutBuilt = fs::exists(m_iniPath); // a saved layout wins over the default one

    // ---- scenes: [0] is the main world, which hosts the hot-reloadable game ----
    {
        AppConfig cfg;
        cfg.jobs = m_jobs.get();
        SceneEntry main;
        main.name = "World";
        main.app = std::make_unique<App>(cfg);
        RenderExtractPlugin extract;
        main.app->addPlugin(extract);
        main.renderScene = m_renderer->createScene("World");
        main.app->world().resource<RenderSceneLink>()->scene = m_renderer->scene(main.renderScene);
        m_scenes.push_back(std::move(main));

        const fs::path exeDir = GetExecutableDirectory();
        m_game = std::make_unique<GameModuleHost>(*m_scenes[0].app, exeDir / FOH_GAME_MODULE_FILE, exeDir / ".hotreload");
        if (!m_game->load()) FOH_ERROR("Could not load the game module; the World scene stays empty");

        if (const auto c = DefaultDevBuilderConfig(); c.isValid()) m_gameBuilder.emplace(c);
        if (const auto c = DefaultShaderBuilderConfig(); c.isValid()) m_shaderBuilder.emplace(c);
    }
    createDebugScene();

    // ---- viewport panels ----
    auto addViewport = [&](const char* title, size_t sceneIndex, bool editorCamera, bool grid) {
        ViewportPanel p;
        p.title = title;
        p.sceneIndex = sceneIndex;
        p.useEditorCamera = editorCamera;
        p.grid = grid;
        ViewportDesc vd;
        vd.name = title;
        vd.width = 960;
        vd.height = 540;
        vd.scene = m_scenes[sceneIndex].renderScene;
        vd.flags = ViewportFlags::Sky | (grid ? ViewportFlags::Grid : ViewportFlags::None);
        p.id = m_renderer->createViewport(vd);
        m_viewports.push_back(std::move(p));
    };
    addViewport("Scene", 0, true, true);
    addViewport("Game", 0, false, false);
    addViewport("Animation Debug", 1, true, true);
    m_viewports[2].camera.position = Vec3(0.0f, 3.0f, 9.0f);
    return true;
}

void Editor::createDebugScene() {
    AppConfig cfg;
    cfg.jobs = m_jobs.get();
    SceneEntry dbg;
    dbg.name = "Animation Debug";
    dbg.app = std::make_unique<App>(cfg);
    RenderExtractPlugin extract;
    dbg.app->addPlugin(extract);
    dbg.app->registerComponent<DebugBob>();
    dbg.app->addSystem(Stage::Update, &animateDebugRig).name("animateDebugRig");
    dbg.renderScene = m_renderer->createScene("Animation Debug");
    dbg.app->world().resource<RenderSceneLink>()->scene = m_renderer->scene(dbg.renderScene);

    World& w = dbg.app->world();
    w.spawn(Transform{{0, 0, 0}, Quat(1, 0, 0, 0), {14, 1, 14}},
            MeshRenderer{static_cast<u32>(BuiltinMesh::Plane), {0.22f, 0.24f, 0.30f, 1}});
    // A ring of "joints" bobbing out of phase, standing in for an animated rig.
    constexpr int kJoints = 12;
    for (int i = 0; i < kJoints; ++i) {
        const f32 a = static_cast<f32>(i) / kJoints * 2.0f * math::kPi;
        const f32 hue = static_cast<f32>(i) / kJoints;
        w.spawn(Transform{{std::cos(a) * 4.0f, 1.0f, std::sin(a) * 4.0f}, Quat(1, 0, 0, 0), {0.8f, 0.8f, 0.8f}},
                MeshRenderer{static_cast<u32>(BuiltinMesh::Cube),
                             {0.4f + 0.5f * std::sin(hue * 6.28f), 0.4f + 0.5f * std::sin(hue * 6.28f + 2.1f),
                              0.4f + 0.5f * std::sin(hue * 6.28f + 4.2f), 1}},
                DebugBob{a * 2.0f, 1.0f, 0.5f});
    }
    w.spawn(Transform{{0, 1.6f, 0}, Quat(1, 0, 0, 0), {2.2f, 2.2f, 2.2f}},
            MeshRenderer{static_cast<u32>(BuiltinMesh::Sphere), {0.9f, 0.9f, 0.95f, 1}},
            DebugBob{0.0f, 1.6f, 0.25f});
    // A low, warm sun so this scene is visibly different from the main one.
    w.spawn(Transform{{0, 0, 0}, glm::quatLookAt(glm::normalize(Vec3(0.7f, -0.35f, -0.4f)), Vec3(0, 1, 0)), {1, 1, 1}},
            DirectionalLight{{1.0f, 0.7f, 0.45f}, 3.5f});
    m_scenes.push_back(std::move(dbg));
}

void Editor::shutdown() {
    if (m_renderer) m_renderer->device().waitIdle();
    m_gameBuilder.reset();
    m_shaderBuilder.reset();
    m_game.reset();
    m_scenes.clear();
    m_imguiRenderer.shutdown();
    m_platform.shutdown();
    if (m_imgui) {
        ImGui::DestroyContext(m_imgui);
        m_imgui = nullptr;
    }
    m_swapchain.reset();
    m_renderer.reset();
    m_jobs.reset();
    m_window.reset();
}

// ---- main loop ----------------------------------------------------------------------------

int Editor::run() {
    using Clock = std::chrono::steady_clock;
    if (!init()) {
        FOH_ERROR("Editor initialization failed");
        return 1;
    }
    auto last = Clock::now();
    u32 rendered = 0;
    while (m_window->pollEvents()) {
        const auto now = Clock::now();
        const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
        last = now;

        if (m_window->isMinimized()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (!frame(dt)) break;

        ++rendered;
        if (m_options.frames && rendered >= m_options.frames) break;
    }

    const u32 errors = m_renderer ? m_renderer->device().errorCount() : 0;
    if (errors) FOH_ERROR("{} graphics API error(s) were reported during the run", errors);
    return errors ? 2 : 0;
}

bool Editor::frame(float dt) {
    // Window resize -> swapchain.
    u32 w = 0, h = 0;
    m_window->getSize(w, h);
    if (w != m_swapchain->width() || h != m_swapchain->height()) {
        if (!m_swapchain->resize(w, h)) return false;
    }
    m_swapchain->setVsync(m_vsync);

    m_avgFrameMs = m_avgFrameMs * 0.95f + dt * 1000.0f * 0.05f;

    if (m_options.stress) runStress();

    // 1. UI (also decides viewport sizes, activity and cameras).
    m_platform.newFrame(dt);
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
    m_sceneRefs.clear();
    for (SceneEntry& sc : m_scenes) m_sceneRefs.push_back({sc.name.c_str(), &sc.app->world()});
    if (!m_selection.valid(m_sceneRefs)) m_selection.entity = kNullEntity;
    buildUi(dt);
    ImGui::Render();
    m_platform.endFrame();

    // 2. Simulation: game hot reload, then every world. Extraction systems fill the
    //    renderer's scene snapshots at the end of each update.
    if (m_gameBuilder) m_gameBuilder->poll();
    if (m_shaderBuilder) m_shaderBuilder->poll();
    if (m_game) m_game->pollReload();
    for (SceneEntry& s : m_scenes) s.app->update(dt);

    // 3. Render: viewports first (parallel graphs), then the window graph with the UI.
    m_renderer->beginFrame();
    m_renderer->renderViewports();
    m_imguiRenderer.prepare(ImGui::GetDrawData(), m_externalTextures);
    m_renderer->renderWindow(*m_swapchain, &Editor::buildWindowPasses, this);

    ++m_frameCount;
    if (m_options.frames && !m_options.screenshot.empty() && m_frameCount == m_options.frames) saveScreenshot();

    m_swapchain->present();
    m_renderer->endFrame();
    return !m_quit;
}

// ---- window graph ---------------------------------------------------------------------------

void Editor::buildWindowPasses(RenderGraph& graph, RgTexture backbuffer, void* user) {
    auto* self = static_cast<Editor*>(user);
    struct UiData {
        RgTexture target;
    };
    graph.addPass<UiData>(
        "ImGui",
        [&](RgPassBuilder& b, UiData& d) {
            d.target = b.colorTarget(0, backbuffer);
            // The viewport images the UI samples this frame.
            for (nvrhi::ITexture* tex : self->m_externalTextures) {
                const RgTexture image = graph.importTexture(tex, nvrhi::ResourceStates::ShaderResource,
                                                            nvrhi::ResourceStates::ShaderResource, "ViewportImage");
                b.read(image, RgAccess::SampledRead);
            }
            b.neverCull();
        },
        [self](const UiData& d, RgPassContext& ctx) {
            ctx.cmd()->clearTextureFloat(ctx.texture(d.target), nvrhi::AllSubresources,
                                         nvrhi::Color(0.06f, 0.065f, 0.07f, 1.0f));
            self->m_imguiRenderer.render(ImGui::GetDrawData(), ctx);
        });
}

// ---- UI ----------------------------------------------------------------------------------------

void Editor::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Quit")) m_quit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        for (ViewportPanel& p : m_viewports) ImGui::MenuItem(p.title.c_str(), nullptr, &p.open);
        ImGui::Separator();
        ImGui::MenuItem("Entities", nullptr, &m_showEntities);
        ImGui::MenuItem("Inspector", nullptr, &m_showInspector);
        ImGui::MenuItem("Render Graph", nullptr, &m_showRenderGraph);
        ImGui::MenuItem("Stats", nullptr, &m_showStats);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Settings")) {
        ImGui::MenuItem("VSync", nullptr, &m_vsync);
        if (ImGui::MenuItem("Reset layout")) {
            std::error_code ec;
            fs::remove(m_iniPath, ec);
            m_dockLayoutBuilt = false;
        }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void Editor::buildDockLayout(unsigned int dockspaceId) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, vp->Size);
    ImGuiID top = 0, bottom = 0, left = 0, right = 0, bottomLeft = 0, bottomRight = 0;
    ImGuiID side = 0, main = 0, sideTop = 0, sideBottom = 0;
    side = ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Right, 0.22f, nullptr, &main);
    sideBottom = ImGui::DockBuilderSplitNode(side, ImGuiDir_Down, 0.58f, nullptr, &sideTop);
    ImGui::DockBuilderDockWindow("Entities", sideTop);
    ImGui::DockBuilderDockWindow("Inspector", sideBottom);
    bottom = ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, 0.34f, nullptr, &top);
    right = ImGui::DockBuilderSplitNode(top, ImGuiDir_Right, 0.5f, nullptr, &left);
    bottomRight = ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.55f, nullptr, &bottomLeft);
    ImGui::DockBuilderDockWindow("Scene", left);
    ImGui::DockBuilderDockWindow("Game", right);
    ImGui::DockBuilderDockWindow("Animation Debug", bottomLeft);
    ImGui::DockBuilderDockWindow("Render Graph", bottomRight);
    ImGui::DockBuilderDockWindow("Stats", bottomRight);
    ImGui::DockBuilderFinish(dockspaceId);
}

void Editor::buildUi(float dt) {
    drawMenuBar();
    const ImGuiID dockspace = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
    if (!m_dockLayoutBuilt) {
        buildDockLayout(dockspace);
        m_dockLayoutBuilt = true;
    }

    for (ViewportPanel& p : m_viewports) drawViewport(p, dt);
    if (m_showEntities) m_inspector.drawEntities(m_sceneRefs, m_selection, &m_showEntities);
    if (m_showInspector) m_inspector.drawInspector(m_sceneRefs, m_selection, &m_showInspector);
    if (m_showRenderGraph) drawRenderGraphPanel();
    if (m_showStats) drawStatsPanel(dt);
}

void Editor::drawViewport(ViewportPanel& p, float dt) {
    if (!p.open) {
        m_renderer->setViewportActive(p.id, false);
        if (p.capturing) {
            p.capturing = false;
            m_window->setRelativeMouse(false);
        }
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool visible = ImGui::Begin(p.title.c_str(), &p.open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    m_renderer->setViewportActive(p.id, visible);

    if (visible) {
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const u32 pw = static_cast<u32>(std::max(avail.x * io.DisplayFramebufferScale.x, 8.0f));
        const u32 ph = static_cast<u32>(std::max(avail.y * io.DisplayFramebufferScale.y, 8.0f));

        // Resizing reallocates render targets, so wait until the panel size settles
        // (the old image is just stretched meanwhile). The first size applies at once.
        if (pw != p.wantWidth || ph != p.wantHeight) {
            p.wantWidth = pw;
            p.wantHeight = ph;
            p.stableFrames = 0;
        } else if (p.stableFrames < 1000) {
            ++p.stableFrames;
        }
        if (!p.sizeApplied || p.stableFrames == 4) {
            m_renderer->setViewportSize(p.id, pw, ph);
            p.sizeApplied = true;
        }

        nvrhi::ITexture* image = m_renderer->viewportTexture(p.id);
        ImGui::SetNextItemAllowOverlap();
        ImGui::Image(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(image))), avail);
        const bool hovered = ImGui::IsItemHovered();

        // Camera: RMB captures the mouse for fly navigation (editor cameras only).
        if (p.useEditorCamera) {
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !p.capturing) {
                p.capturing = true;
                m_window->setRelativeMouse(true);
            }
            if (p.capturing && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                p.capturing = false;
                m_window->setRelativeMouse(false);
            }
            if (p.capturing) ImGui::SetNextFrameWantCaptureKeyboard(true);
            p.camera.update(m_window->input(), dt, p.capturing, hovered);
            const f32 aspect = p.wantHeight ? static_cast<f32>(p.wantWidth) / static_cast<f32>(p.wantHeight) : 1.0f;
            m_renderer->setViewportCamera(
                p.id, MakeViewParams(p.camera.world(), p.camera.fovY, aspect, p.camera.nearZ, p.camera.farZ));
        } else {
            m_renderer->clearViewportCamera(p.id);
        }

        const int panelIndex = static_cast<int>(&p - m_viewports.data());
        if (p.useEditorCamera) drawGizmo(p, origin, avail, panelIndex);
        handlePicking(p, origin, avail, hovered);

        // Overlay controls (top-left of the image), on a translucent backdrop so they stay readable on a bright sky.
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 8, origin.y + 8));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.04f, 0.045f, 0.05f, 0.72f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
        ImGui::PushID(p.title.c_str());
        if (ImGui::BeginChild("##overlay", ImVec2(0, 0),
                              ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            ImGui::SetNextItemWidth(150);
            if (ImGui::BeginCombo("##scene", m_scenes[p.sceneIndex].name.c_str())) {
                for (size_t i = 0; i < m_scenes.size(); ++i) {
                    if (ImGui::Selectable(m_scenes[i].name.c_str(), i == p.sceneIndex)) {
                        p.sceneIndex = i;
                        m_renderer->setViewportScene(p.id, m_scenes[i].renderScene);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Checkbox("Editor camera", &p.useEditorCamera) && !p.useEditorCamera && p.capturing) {
                p.capturing = false;
                m_window->setRelativeMouse(false);
            }
            ImGui::SameLine();
            if (p.useEditorCamera) {
                ImGui::RadioButton("Move", &m_gizmoOp, ImGuizmo::TRANSLATE);
                ImGui::SameLine();
                ImGui::RadioButton("Rotate", &m_gizmoOp, ImGuizmo::ROTATE);
                ImGui::SameLine();
                ImGui::RadioButton("Scale", &m_gizmoOp, ImGuizmo::SCALE);
                ImGui::SameLine();
                ImGui::Checkbox("Local", &m_gizmoLocal);
                ImGui::SameLine();
            }
            if (ImGui::Checkbox("Grid", &p.grid)) {
                m_renderer->setViewportFlags(p.id, ViewportFlags::Sky | (p.grid ? ViewportFlags::Grid : ViewportFlags::None));
            }
        }
        ImGui::EndChild();
        ImGui::PopID();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    } else if (p.capturing) {
        p.capturing = false;
        m_window->setRelativeMouse(false);
    }
    ImGui::End();
}

void Editor::drawRenderGraphPanel() {
    if (!ImGui::Begin("Render Graph", &m_showRenderGraph)) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("One graph instance per viewport; passes run in declaration order, culled ones are skipped.");
    for (const ViewportPanel& p : m_viewports) {
        ViewportDebugInfo info;
        if (!m_renderer->viewportDebugInfo(p.id, info)) continue;
        const bool open = ImGui::CollapsingHeader(info.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        if (!open) continue;
        ImGui::Text("%ux%u  |  %u instances  |  %u command list group(s)  |  %u/%u physical/virtual transients  |  %u barriers",
                    info.width, info.height, info.instances, info.groups, info.physicalTextures,
                    info.transientTextures, info.barriers);
        if (ImGui::BeginTable(("passes##" + info.name).c_str(), 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Group");
            ImGui::TableSetupColumn("State");
            ImGui::TableHeadersRow();
            for (const auto& pass : info.passes) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (pass.culled) ImGui::TextDisabled("%s", pass.name); else ImGui::TextUnformatted(pass.name);
                ImGui::TableNextColumn();
                if (pass.culled) ImGui::TextDisabled("-"); else ImGui::Text("%u", pass.group);
                ImGui::TableNextColumn();
                if (pass.culled) ImGui::TextDisabled("culled"); else ImGui::TextUnformatted("recorded");
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void Editor::drawStatsPanel(float) {
    if (!ImGui::Begin("Stats", &m_showStats)) {
        ImGui::End();
        return;
    }
    ImGui::Text("%.2f ms (%.0f fps)", m_avgFrameMs, 1000.0f / std::max(m_avgFrameMs, 0.001f));
    ImGui::Text("GPU: %s", m_renderer->device().adapterName().c_str());
    ImGui::Text("Worker threads: %u", m_jobs->workerCount());
    ImGui::Text("Pipelines cached: %zu", m_renderer->pipelines().size());
    ImGui::Text("RHI errors: %u", m_renderer->device().errorCount());
    if (m_game) ImGui::Text("Game module generation: %u", m_game->generation());
    ImGui::Separator();
    for (const SceneEntry& s : m_scenes) {
        ImGui::Text("Scene '%s': %zu entities, %zu systems", s.name.c_str(), s.app->world().entityCount(),
                    s.app->systemCount());
    }
    ImGui::Separator();
    ImGui::TextDisabled("Scene / Animation Debug: hold RMB over the view, WASD + Q/E to fly, wheel = speed, Shift = fast");
    ImGui::End();
}

namespace {

// ImGuizmo gives back a world matrix; split it into the Transform components.
void applyMatrix(Transform& t, const Mat4& m) {
    t.position = Vec3(m[3]);
    Vec3 axes[3] = {Vec3(m[0]), Vec3(m[1]), Vec3(m[2])};
    for (int i = 0; i < 3; ++i) {
        const f32 len = glm::length(axes[i]);
        t.scale[i] = len;
        if (len > 1e-6f) axes[i] /= len;
    }
    t.rotation = glm::normalize(glm::quat_cast(Mat3(axes[0], axes[1], axes[2])));
}

} // namespace

// Transform gizmo (ImGuizmo) for the selected entity, drawn over an editor-camera viewport.
// ImGuizmo expects an OpenGL-style projection (it unprojects the mouse with it), so it gets
// one with the same fov/planes instead of our reverse-Z matrix.
void Editor::drawGizmo(ViewportPanel& p, const ImVec2& origin, const ImVec2& size, int panelIndex) {
    if (!m_selection.valid(m_sceneRefs) || m_selection.scene != p.sceneIndex) return;
    World& world = *m_sceneRefs[m_selection.scene].world;
    Transform* t = world.get<Transform>(m_selection.entity);
    if (!t || size.x < 8 || size.y < 8) return;

    // Hotkeys, only while the cursor is over this view and the camera is not flying.
    const bool hotkeys = ImGui::IsWindowHovered() && !p.capturing && !ImGui::GetIO().WantTextInput;
    if (hotkeys) {
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) m_gizmoOp = ImGuizmo::TRANSLATE;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) m_gizmoOp = ImGuizmo::ROTATE;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) m_gizmoOp = ImGuizmo::SCALE;
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
            // Frame the selection: back off along the current view direction.
            const f32 dist = std::max(4.0f, std::max({t->scale.x, t->scale.y, t->scale.z}) * 3.0f);
            p.camera.position = t->position - (p.camera.rotation() * Vec3(0, 0, -1)) * dist;
        }
    }

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(origin.x, origin.y, size.x, size.y);
    ImGuizmo::SetID(panelIndex);
    const Mat4 view = glm::inverse(p.camera.world());
    const Mat4 proj = glm::perspectiveRH_NO(p.camera.fovY, size.x / size.y, p.camera.nearZ, p.camera.farZ);
    Mat4 model = t->matrix();
    const bool snap = ImGui::GetIO().KeyCtrl;
    const float step = m_gizmoOp == ImGuizmo::ROTATE ? 15.0f : (m_gizmoOp == ImGuizmo::SCALE ? 0.1f : 0.5f);
    const float snapAll[3] = {step, step, step};
    if (ImGuizmo::Manipulate(&view[0][0], &proj[0][0], static_cast<ImGuizmo::OPERATION>(m_gizmoOp),
                             m_gizmoLocal ? ImGuizmo::LOCAL : ImGuizmo::WORLD, &model[0][0], nullptr,
                             snap ? snapAll : nullptr)) {
        applyMatrix(*t, model);
    }
}

// Click-to-select: ask the renderer for the entity under the cursor (an id pass runs on the
// next frame only), and apply answers as they arrive.
void Editor::handlePicking(ViewportPanel& p, const ImVec2& origin, const ImVec2& size, bool hovered) {
    ImGuiIO& io = ImGui::GetIO();
    const bool gizmoBusy = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
    if (hovered && !gizmoBusy && !p.capturing && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
        if (drag.x * drag.x + drag.y * drag.y < 16.0f && size.x > 0 && size.y > 0) {
            const float fx = (io.MousePos.x - origin.x) / size.x;
            const float fy = (io.MousePos.y - origin.y) / size.y;
            if (fx >= 0 && fx < 1 && fy >= 0 && fy < 1) {
                m_renderer->requestPick(p.id, static_cast<u32>(fx * static_cast<float>(m_renderer->viewportWidth(p.id))),
                                        static_cast<u32>(fy * static_cast<float>(m_renderer->viewportHeight(p.id))));
            }
        }
    }
    u64 picked = 0;
    if (m_renderer->takePickResult(p.id, picked)) {
        const Entity e{picked};
        World& world = *m_sceneRefs[p.sceneIndex].world;
        if (!e.isNull() && world.isAlive(e)) {
            m_selection.scene = p.sceneIndex;
            m_selection.entity = e;
        } else if (m_selection.scene == p.sceneIndex) {
            m_selection.entity = kNullEntity; // clicked empty space
        }
    }
}

// Scripted abuse for QA runs (--stress). Everything here is also reachable by a user.
void Editor::runStress() {
    const u32 f = m_frameCount;
    constexpr u32 kSizes[][2] = {{1100, 700}, {1700, 950}, {900, 600}, {1600, 900}};
    if (f % 25 == 5) {
        const auto& s = kSizes[(f / 25) % 4];
        m_window->setSize(s[0], s[1]); // swapchain + every panel resize
    }
    if (f % 9 == 3 && m_viewports.size() >= 3) {
        // Viewport target recreation behind the UI's back.
        const u32 w = 200 + (f * 37) % 700, h = 150 + (f * 53) % 400;
        m_renderer->setViewportSize(m_viewports[f % 3].id, w, h);
    }
    if (f == 30) {
        m_viewports[1].sceneIndex = 1; // Game panel looks at the debug world
        m_renderer->setViewportScene(m_viewports[1].id, m_scenes[1].renderScene);
    }
    if (f == 60) {
        m_viewports[1].sceneIndex = 0;
        m_renderer->setViewportScene(m_viewports[1].id, m_scenes[0].renderScene);
    }
    if (f == 40) m_viewports[2].open = false;
    if (f == 55) m_viewports[2].open = true;
}

// ---- screenshot (smoke runs) --------------------------------------------------------------------

void Editor::saveScreenshot() {
    std::vector<u8> pixels;
    if (!m_swapchain->readback(pixels)) {
        FOH_ERROR("screenshot: backbuffer readback failed");
        return;
    }
    const bool ok = WritePng(m_options.screenshot, pixels.data(), m_swapchain->width(), m_swapchain->height(),
                             m_swapchain->width() * 4);
    FOH_INFO("screenshot {}: {}", ok ? "saved to" : "FAILED for", m_options.screenshot);
}

} // namespace foh
