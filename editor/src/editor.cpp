#include "editor.h"
#include "foh/world/surface.h"
#include "water_test_scene.h"

#include <cstdio>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <thread>

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder: the default layout
#include <ImGuizmo.h>

#include "foh/dev_build_defaults.h"
#include "foh/file_system.h"
#include "foh/log.h"
#include "foh/ecs/inspect.h"
#include "foh/input.h"
#include "foh/renderer/components.h"
#include "foh/scene/prefab_request.h"
#include "foh/rhi/pipeline_cache.h"
#include "foh/rhi/render_graph.h"
#include "foh/png.h"
#include "foh/shell.h"
#include "foh/utf8_path.h"

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

} // namespace

Editor::Editor(EditorOptions options) : m_options(std::move(options)), m_vsync(m_options.vsync) {}

Editor::~Editor() { shutdown(); }

// ---- lifetime --------------------------------------------------------------------------

bool Editor::init() {
    WindowDesc wd;
    wd.title = "Favelas of Hell - Editor";
    wd.width = m_options.windowWidth;
    wd.height = m_options.windowHeight;
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
    m_iniPath = (GetExecutableDirectory() / "editor_layout_v3.ini").string();
    io.IniFilename = m_iniPath.c_str();
    m_prefs.load(GetExecutableDirectory() / "editor_prefs.txt");
    loadRenderSettings();
    if (m_options.profile) {
        RenderSettings r = m_renderer->renderSettings();
        r.gpuTimers = true;
        m_renderer->setRenderSettings(r);
    }
    m_theme.init(*m_window);
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
        AnimationPlugin animation;
        main.app->addPlugin(animation);
        main.app->insertResource(AnimationLink{&m_animation});
        RegisterPrefabRequests(*main.app); // game entities ask for models by source path (see prefab_request.h)
        main.app->insertResource(GameInput{});
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
    createPreviewScene();
    createWaterScene();

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
    for (size_t i = 0; i < 2; ++i) { // the two views of the world: they get light probes (each costs a few captures per frame)
        m_viewports[i].probes = true;
        m_renderer->setViewportFlags(m_viewports[i].id, FlagsOf(m_viewports[i]));
    }
    m_renderer->setGpuTimerViewport(m_viewports[1].id); // the Game view: the one whose cost matters
    addViewport("Animation Debug", 1, true, true);
    m_viewports[2].camera.position = Vec3(0.0f, 3.0f, 9.0f);
    addViewport("Prefab Preview", m_previewScene, true, true);
    m_previewViewport = m_viewports.size() - 1;
    m_viewports[m_previewViewport].open = false; // shown while the prefab editor is
    m_viewports[m_previewViewport].initialW = 560.0f;
    m_viewports[m_previewViewport].initialH = 460.0f;
    m_viewports[m_previewViewport].camera.farZ = 500.0f;
    addViewport("Water Test", m_waterScene, true, false);
    m_waterViewport = m_viewports.size() - 1;
    {
        ViewportPanel& wp = m_viewports[m_waterViewport];
        wp.water = true;
        wp.open = m_options.waterTest >= 0; // off by default: the sea costs GPU time and the real terrain does not exist yet
        wp.camera.farZ = 6000.0f;
        m_renderer->setViewportFlags(wp.id, ViewportFlags::Sky | ViewportFlags::Water);
        struct View { Vec3 position; f32 yaw, pitch; };
        const View views[] = {{{-40.0f, 55.0f, -300.0f}, 3.1416f, -0.14f},   // from the hill over the cove
                              {{30.0f, 3.0f, -160.0f}, 3.1416f, -0.04f},      // standing on the beach
                              {{0.0f, 380.0f, -500.0f}, 3.1416f, -0.55f},     // high over the bay
                              {{200.0f, 9.0f, 330.0f}, 3.1416f - 0.5f, -0.10f}}; // facing the islet
        const View& v = views[std::clamp(m_options.waterTest, 0, 3)];
        wp.camera.position = v.position;
        wp.camera.yaw = v.yaw;
        wp.camera.pitch = v.pitch;
    }

    if (m_options.terrain || m_options.city) {
        m_showTerrain = !m_options.city;
        m_showCity = m_options.city;
        if (m_options.city) m_options.terrain = true;
        ensureTerrain();
        if (!m_options.autoplay.empty()) m_showTerrain = false; // QA: playing, so the Game view is what the screenshot should show
        FlyCamera& cam = m_viewports[0].camera;
        struct View { f32 x, z, eye, yaw, pitch; }; // eye: metres above the ground (negative: absolute height)
        const View views[] = {{0.0f, 2200.0f, -1100.0f, 0.0f, -0.42f},  // from the sea, looking north at Vidigal, Dois Irmaos behind
                              {-30.0f, -60.0f, 1.8f, 0.0f, -0.05f},     // on the Vidigal slope, looking uphill
                              {-80.0f, -260.0f, 1.8f, 3.1416f, -0.10f}, // up the hill, looking down over the sea
                              {1500.0f, 0.0f, 1.8f, 1.9f, -0.02f}};     // Leblon beach, looking along the shore
        const View& v = views[std::clamp(m_options.terrainView, 0, 3)];
        f32 vz = v.z;
        if (m_options.terrainView == 3) // the shoreline: the first dry spot walking inland from the sea
            for (vz = 1200.0f; vz > -2500.0f && m_terrainPanel.terrain()->height(v.x, vz) < 2.5f; vz -= 0.5f) {}
        cam.position = Vec3(v.x, v.eye < 0.0f ? -v.eye : m_terrainPanel.terrain()->height(v.x, vz) + v.eye, vz);
        cam.yaw = v.yaw;
        cam.pitch = v.pitch;
        if (!m_options.cam.empty()) { // QA: an explicit camera, no panel in the way
            f32 cx = 0, cz = 0, eye = 1.8f, yaw = 0, pitch = 0;
            if (std::sscanf(m_options.cam.c_str(), "%f,%f,%f,%f,%f", &cx, &cz, &eye, &yaw, &pitch) == 5) {
                cam.position = Vec3(cx, m_terrainPanel.terrain()->height(cx, cz) + eye, cz);
                cam.yaw = yaw;
                cam.pitch = pitch;
                m_showTerrain = false;
            }
        }
        if (m_options.roadsTest) {
            // Roads QA: hover above the beach looking north at the avenue.
            cam.yaw = 0.0f;
            cam.pitch = -0.27f;
            cam.position.z += 95.0f;
            cam.position.y = 38.0f;
        }
    }

    m_thumbs.init(*m_renderer, *m_jobs);
    m_showWorkshop = m_options.openWorkshop;
    if (m_options.workshopTest) m_workshop.enableSelfTest();
    initAssets();
    return true;
}

// ---- assets ---------------------------------------------------------------------------------

void Editor::initAssets() {
    const fs::path root = fs::path(FOH_PROJECT_ROOT);
    AssetDbDesc desc;
    desc.sourceRoot = root / "assets";
    desc.cacheRoot = root / "cache" / "imported";
    desc.jobs = m_jobs.get(); // imports and asset loads run on the job system (low priority)
    m_assets = std::make_unique<AssetDb>(desc);
    m_assets->scan();
    m_assetBridge = std::make_unique<AssetSceneBridge>(*m_assets, *m_renderer, m_jobs.get());
    m_assetBridge->loader().setAnimationLibrary(&m_animation);
    m_scenes[0].app->world().resource<SceneBridgeLink>()->bridge = m_assetBridge.get();
    FOH_INFO("asset database: {} source(s) in {}", m_assets->sources().size(), PathToUtf8(desc.sourceRoot));
    logAsset("Drop a .gltf / .glb / .fbx model or a texture onto the window to import it.");

    ContentBrowserActions actions;
    actions.importFile = [this](const fs::path& file) { importFile(file, 0, 0, false, false); };
    actions.importAsAnimation = [this](const fs::path& file) {
        // Clips only: no meshes, materials, skeleton or prefab (animation packs carry a character in every file).
        ImportSettings s;
        s.animationOnly = true;
        importFile(file, 0, 0, false, false, &s);
    };
    actions.applyMaterial = [this](const std::string& rel) { applyMaterialToSelection(rel); };
    actions.placeModel = [this](const std::string& rel) { placeAssetByPath(rel, 0, 0, false); };
    actions.openWorkshop = [this] { m_showWorkshop = true; };
    actions.editPrefab = [this](const std::string& rel) { openPrefabEditor(rel); };
    actions.useAsModel = [this](const std::string& rel, bool player) { useAsModel(rel, player); };
    m_browser.init(*m_assets, m_thumbs, m_prefs, m_assetLog, std::move(actions));

    WorkshopHost host;
    host.assetRoot = m_assets->sourceRoot();
    host.importFile = [this](const fs::path& file) { return static_cast<unsigned long long>(importFile(file, 0, 0, false, false)); };
    host.createMaterial = [this](const MaterialRecipe& r, const std::vector<unsigned long long>& tickets) {
        queueMaterial(r, std::vector<ImportTicket>(tickets.begin(), tickets.end()));
    };
    host.placeModel = [this](const std::string& rel) { placeAssetByPath(rel, 0, 0, false); };
    host.refreshBrowser = [this] { m_browser.refresh(); };
    m_workshop.init(*m_jobs, m_thumbs, m_prefs, std::move(host));

    AssetPickerHost pickerHost;
    pickerHost.assigned = [this](AssetId id) { m_assetBridge->loader().request(id); }; // streams in what a field now points at
    pickerHost.inspect = [this](AssetId id) {
        m_assetInspector.inspectAsset(id);
        m_showAssetInspector = true;
        ImGui::SetWindowFocus("Asset Inspector");
    };
    m_picker.init(*m_assets, m_thumbs, std::move(pickerHost));
    m_inspector.setAssetPicker(&m_picker);

    AssetInspectorHost inspectorHost;
    inspectorHost.changed = [this](AssetId id) {
        const AssetId ids[] = {id};
        m_assetBridge->assetsChanged(ids); // reloads the material where it is in use
    };
    inspectorHost.editPrefab = [this](const std::string& rel) { openPrefabEditor(rel); };
    inspectorHost.applyImportSettings = [this](const std::string& rel, const ImportSettings& s) { handleImportResult(m_assets->setImportSettings(rel, s)); };
    inspectorHost.reimport = [this](const std::string& rel) { m_assets->beginImport(m_assets->sourceRoot() / PathFromUtf8(rel), true); };
    inspectorHost.refreshBrowser = [this] { m_browser.refresh(); };
    m_assetInspector.init(*m_assets, m_picker, std::move(inspectorHost));

    PrefabEditorHost prefabHost;
    prefabHost.show = [this](const PrefabAsset& p) { showPrefabPreview(p); };
    prefabHost.frame = [this](Vec3 c, float r) { framePreview(c, r); };
    prefabHost.setPreviewVisible = [this](bool v) { m_viewports[m_previewViewport].open = v; };
    prefabHost.place = [this](const std::string& rel) { placeAssetByPath(rel, 0, 0, false); };
    prefabHost.refreshBrowser = [this] { m_browser.refresh(); };
    m_prefabEditor.init(*m_assets, m_picker, *m_jobs, std::move(prefabHost));
}

void Editor::openPrefabEditor(const std::string& relPath) {
    m_showPrefabEditor = true;
    if (!m_prefabEditor.open(relPath)) {
        logAsset("Prefab editor: " + m_prefabEditor.lastError());
        return;
    }
    ImGui::SetWindowFocus("Prefab Editor");
}

void Editor::showPrefabPreview(const PrefabAsset& prefab) {
    World& world = m_scenes[m_previewScene].app->world();
    if (!m_previewRoot.isNull() && world.isAlive(m_previewRoot)) world.despawn(m_previewRoot); // children go with it
    const PrefabInstanceResult inst = InstantiatePrefab(world, prefab, Transform{});
    m_previewRoot = inst.root;
    assignLocomotionClips(world, inst.root);
    // The stage is small: a character walks in place there (the blend still follows its velocity).
    if (LocomotionControl* control = world.get<LocomotionControl>(inst.root)) control->kinematic = false;
    for (const PrefabNode& node : prefab.nodes) {
        if (node.mesh.isValid()) m_assetBridge->loader().request(node.mesh);
        for (const AssetId& material : node.materials)
            if (material.isValid()) m_assetBridge->loader().request(material);
    }
}

void Editor::framePreview(Vec3 center, float radius) {
    ViewportPanel& p = m_viewports[m_previewViewport];
    p.camera.yaw = 0.55f;
    p.camera.pitch = -0.3f;
    const f32 dist = std::max(radius / std::tan(p.camera.fovY * 0.5f) * 1.15f, 0.5f);
    p.camera.position = center - p.camera.rotation() * Vec3(0, 0, -1) * dist;
    p.camera.nearZ = std::clamp(dist * 0.01f, 0.005f, 0.2f);
    p.camera.farZ = std::max(500.0f, dist * 20.0f);
    p.camera.speed = std::clamp(radius * 1.5f, 1.0f, 100.0f);
}

void Editor::placeAssetByPath(const std::string& relPath, float x, float y, bool hasPosition) {
    if (PathFromUtf8(relPath).extension() == ".fvariant") {
        const AssetId variant = m_assets->findAuthored(relPath);
        if (!variant.isValid()) {
            logAsset("Cannot place '" + relPath + "': unknown prefab variant.");
            return;
        }
        size_t sceneIndex = 0;
        const Transform at = dropPlacement(x, y, hasPosition, sceneIndex);
        instantiateAsset(variant, sceneIndex, at);
        return;
    }
    const SourceRecord* rec = m_assets->findSource(relPath);
    AssetId prefab;
    if (rec) {
        for (const SubAssetRecord& s : rec->subAssets)
            if (!s.tombstone && s.kind == AssetKind::Prefab) prefab = s.guid;
    }
    if (!prefab.isValid()) {
        importFile(m_assets->sourceRoot() / PathFromUtf8(relPath), x, y, hasPosition, true); // imports and places
        return;
    }
    // A model fixed in the prefab editor is placed with its fixes (the variant stored next to it).
    if (const AssetId variant = m_assets->findAuthored(PrefabEditor::VariantPathFor(relPath)); variant.isValid()) {
        PrefabVariantAsset v;
        if (m_assets->load(variant, v) && v.base == prefab) prefab = variant;
    }
    size_t sceneIndex = 0;
    const Transform at = dropPlacement(x, y, hasPosition, sceneIndex);
    instantiateAsset(prefab, sceneIndex, at);
}

void Editor::logAsset(std::string message) {
    FOH_INFO("assets: {}", message);
    m_assetLog.push_back(std::move(message));
    if (m_assetLog.size() > 64) m_assetLog.erase(m_assetLog.begin(), m_assetLog.begin() + 16);
}

Transform Editor::dropPlacement(float x, float y, bool hasPosition, size_t& sceneIndex) const {
    // The viewport under the cursor, else the first open editor-camera viewport.
    const ViewportPanel* target = nullptr;
    auto inside = [&](const ViewportPanel& p) {
        return x >= p.imageX && x < p.imageX + p.imageW && y >= p.imageY && y < p.imageY + p.imageH;
    };
    for (const ViewportPanel& p : m_viewports) {
        if (!p.open || !p.useEditorCamera || p.imageW <= 0.0f) continue;
        if (!target) target = &p;
        if (hasPosition && inside(p)) {
            target = &p;
            break;
        }
    }
    Transform t;
    sceneIndex = target ? target->sceneIndex : 0;
    if (!target) return t;

    // Ray through the cursor (or the view center) onto the ground plane y = 0.
    f32 ndcX = 0.0f, ndcY = 0.0f;
    if (hasPosition && inside(*target)) {
        ndcX = (x - target->imageX) / target->imageW * 2.0f - 1.0f;
        ndcY = 1.0f - (y - target->imageY) / target->imageH * 2.0f;
    }
    const FlyCamera& cam = target->camera;
    const f32 aspect = target->imageH > 0.0f ? target->imageW / target->imageH : 1.0f;
    const f32 tanHalf = std::tan(cam.fovY * 0.5f);
    const Vec3 dir = glm::normalize(cam.rotation() * Vec3(ndcX * tanHalf * aspect, ndcY * tanHalf, -1.0f));
    if (dir.y < -1e-4f) {
        const f32 dist = -cam.position.y / dir.y;
        if (dist > 0.0f && dist < 500.0f) {
            t.position = cam.position + dir * dist;
            return t;
        }
    }
    t.position = cam.position + dir * 12.0f; // looking at the sky or too far: in front of the camera
    return t;
}

void Editor::instantiateAsset(AssetId prefab, size_t sceneIndex, const Transform& at) {
    if (sceneIndex >= m_scenes.size()) sceneIndex = 0;
    std::string error;
    const PrefabInstanceResult inst = m_assetBridge->instantiate(m_scenes[sceneIndex].app->world(), prefab, at, &error);
    if (inst.root.isNull()) {
        logAsset("Cannot place the model: " + error);
        return;
    }
    m_selection.scene = sceneIndex;
    m_selection.entity = inst.root;
    assignLocomotionClips(m_scenes[sceneIndex].app->world(), inst.root);
    if (m_options.walkSpeed > 0.0f) {
        // QA (--walk): look at the character from the front so a screenshot shows the gait.
        for (ViewportPanel& p : m_viewports) {
            if (!p.useEditorCamera || p.sceneIndex != sceneIndex) continue;
            p.camera.yaw = 0.0f;
            p.camera.pitch = -0.08f;
            p.camera.position = at.position + Vec3(0.0f, 1.3f, 4.0f);
            break;
        }
    }
    logAsset("Placed " + std::to_string(inst.entities.size()) + " entities into '" + m_scenes[sceneIndex].name + "'.");
}

// Gives an entity a model to show (PrefabSlot): the selected one, or the game's player (the entity that has a slot and a
// LocomotionControl). The model is imported first when it is not yet; the slot waits for it.
void Editor::useAsModel(const std::string& relPath, bool player) {
    size_t sceneIndex = m_selection.scene;
    Entity target = kNullEntity;
    if (player) {
        sceneIndex = 0;
        World& world = m_scenes[0].app->world();
        const ComponentId with[] = {world.component<PrefabSlot>(), world.component<LocomotionControl>()};
        std::vector<Entity> found;
        findEntities(world, with, {}, found);
        if (!found.empty()) target = found.front();
        else logAsset("The game has no player to give a model to.");
    } else if (m_selection.valid(m_sceneRefs)) {
        target = m_selection.entity;
    } else {
        logAsset("Select an entity first (Entities panel), or use \"Use as player model\".");
    }
    if (target.isNull()) return;

    if (!m_assetBridge->findPrefab(relPath).isValid() && !m_assets->findSource(relPath)) {
        importFile(m_assets->sourceRoot() / PathFromUtf8(relPath), 0, 0, false, false); // the slot waits for the import
    }
    World& world = m_scenes[sceneIndex].app->world();
    const PrefabSlot slot = [&] {
        PrefabSlot s = world.has<PrefabSlot>(target) ? *world.get<PrefabSlot>(target) : PrefabSlot{};
        const PrefabSlot wanted = MakePrefabSlot(relPath);
        std::memcpy(s.sourcePath, wanted.sourcePath, sizeof(s.sourcePath));
        return s;
    }();
    world.insert(target, slot);
    logAsset("Model of the " + std::string(player ? "player" : "selected entity") + ": " + relPath);
}

// A placed character gets the locomotion clips that were imported next to it (the bridge matches Mixamo's names).
void Editor::assignLocomotionClips(World& world, Entity root) {
    m_assetBridge->assignLocomotionClips(world, root);
    if (m_options.walkSpeed > 0.0f)
        if (LocomotionControl* control = world.get<LocomotionControl>(root)) control->velocity = Vec3(0.0f, 0.0f, m_options.walkSpeed);
}

// QA (--preview "name@t1,t2,..."): the player plays the clip whose name contains `name`, frozen at each time in turn, with a
// screenshot per time (<screenshot stem>_<k>.png). Frame 30 picks the clip; then 4 frames per time (set, settle, shoot, rest).
void Editor::previewStep() {
    const std::string& spec = m_options.preview;
    const size_t at = spec.find('@');
    const std::string name = spec.substr(0, at);
    std::vector<f32> times;
    if (at != std::string::npos) {
        size_t pos = at + 1;
        while (pos < spec.size()) {
            size_t end = spec.find(',', pos);
            if (end == std::string::npos) end = spec.size();
            times.push_back(std::strtof(spec.c_str() + pos, nullptr));
            pos = end + 1;
        }
    }
    if (m_frameCount == 0 && m_options.shots.empty()) {
        for (size_t k = 0; k < times.size(); ++k) m_options.shots += (k ? "," : "") + std::to_string(40 + 4 * k + 3);
    }
    World& world = m_scenes[0].app->world();
    const ComponentId with[] = {world.component<LocomotionControl>(), world.component<PrefabSlot>()};
    std::vector<Entity> found;
    findEntities(world, with, {}, found);
    if (found.empty()) return;
    LocomotionControl* control = world.get<LocomotionControl>(found.front());
    if (!control) return;
    if (m_frameCount == 30) {
        std::string lowerName = name;
        for (char& c : lowerName) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const AssetId& id : m_assets->assetsOfKind(AssetKind::AnimationClip)) {
            const AssetInfo* info = m_assets->find(id);
            std::string n = info ? info->name : std::string();
            for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (n.find(lowerName) == std::string::npos) continue;
            control->previewClip = id;
            control->previewPlay = false;
            m_assetBridge->loader().request(id);
            logAsset("Preview: " + (info ? info->name : std::string("?")));
            break;
        }
        if (!control->previewClip.isValid()) logAsset("Preview: no clip named '" + name + "'");
    }
    if (m_frameCount >= 40 && !times.empty()) {
        const u32 k = (m_frameCount - 40) / 4, phase = (m_frameCount - 40) % 4;
        if (k < times.size()) {
            if (phase == 0) control->previewTime = times[k];
        }
    }
}

// QA: replaces the real input with a script, so the controls can be exercised without a person at the keyboard.
// Format: comma separated "KEY:frame" (a press on that frame) or "KEY:from-to" (held), KEY = W A S D C V SPACE SHIFT, or
// "MOUSEX:from-to=pixelsPerFrame" / "MOUSEY:...". Frames count editor frames.
void Editor::applyAutoplay(GameInput& input) const {
    input.captured = true;
    input.state = InputState{};
    const std::string& s = m_options.autoplay;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        const std::string item = s.substr(pos, end - pos);
        pos = end + 1;
        const size_t colon = item.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = item.substr(0, colon);
        std::string range = item.substr(colon + 1);
        float amount = 0.0f;
        if (const size_t eq = range.find('='); eq != std::string::npos) {
            amount = static_cast<float>(std::atof(range.c_str() + eq + 1));
            range = range.substr(0, eq);
        }
        u64 from = 0, to = 0;
        if (const size_t dash = range.find('-'); dash != std::string::npos) {
            from = std::strtoull(range.c_str(), nullptr, 10);
            to = std::strtoull(range.c_str() + dash + 1, nullptr, 10);
        } else {
            from = to = std::strtoull(range.c_str(), nullptr, 10);
        }
        const u64 frame = m_frameCount;
        if (frame < from || frame > to) continue;
        if (name == "MOUSEX") input.state.mouseDeltaX = amount;
        else if (name == "MOUSEY") input.state.mouseDeltaY = amount;
        else {
            const Key key = name == "W" ? Key::W : name == "A" ? Key::A : name == "S" ? Key::S : name == "D" ? Key::D : name == "C" ? Key::C
                            : name == "V" ? Key::V : name == "SPACE" ? Key::Space : name == "SHIFT" ? Key::LShift : Key::Unknown;
            if (key == Key::Unknown) continue;
            input.state.keys[static_cast<size_t>(key)] = true;
            if (frame == from) input.state.keysPressed[static_cast<size_t>(key)] = true;
        }
    }
}

ImportTicket Editor::importFile(const fs::path& file, float dropX, float dropY, bool hasDropPosition, bool place, const ImportSettings* initial) {
    const std::string name = PathToUtf8(file.filename());
    if (DetectSourceFormat(file) == SourceFormat::Unknown) {
        logAsset("Cannot import '" + name + "': supported are .gltf, .glb, .fbx, .png, .jpg, .tga, .bmp.");
        return 0;
    }
    // Returns at once: parsing, texture decoding, mips and BC compression run on the worker threads.
    Placement p;
    p.ticket = m_assets->beginImport(file, false, initial);
    p.x = dropX;
    p.y = dropY;
    p.hasPosition = hasDropPosition;
    p.place = place;
    p.started = std::chrono::steady_clock::now();
    m_placements.push_back(p);
    logAsset("Importing '" + name + "'...");
    return p.ticket;
}

void Editor::queueMaterial(const MaterialRecipe& recipe, const std::vector<ImportTicket>& tickets) {
    PendingMaterial pm;
    pm.recipe = recipe;
    pm.waiting = tickets;
    if (pm.waiting.empty()) createMaterialAsset(recipe); // nothing to wait for (textures were refused)
    else m_pendingMaterials.push_back(std::move(pm));
}

void Editor::createMaterialAsset(const MaterialRecipe& recipe) {
    auto textureOf = [&](const std::string& rel) -> AssetId {
        if (rel.empty()) return {};
        const SourceRecord* rec = m_assets->findSource(rel);
        if (!rec) return {};
        for (const SubAssetRecord& s : rec->subAssets)
            if (!s.tombstone && s.kind == AssetKind::Texture) return s.guid;
        return {};
    };
    MaterialAsset m;
    m.name = recipe.name;
    m.baseColorTexture.texture = textureOf(recipe.colorMap);
    m.normalTexture.texture = textureOf(recipe.normalMap);
    const AssetId orm = textureOf(recipe.ormMap);
    m.metallicRoughnessTexture.texture = orm; // glTF packing: G = roughness, B = metalness
    m.occlusionTexture.texture = orm;         // R = occlusion
    m.emissiveTexture.texture = textureOf(recipe.emissiveMap);
    // Factors multiply the maps: 1 lets the textures decide. Without a packed map fall back to a plain dielectric.
    m.metallic = orm.isValid() ? 1.0f : 0.0f;
    m.roughness = orm.isValid() ? 1.0f : 0.8f;
    if (m.emissiveTexture.isValid()) m.emissive = Vec3(1.0f);
    if (!m.baseColorTexture.isValid() && !m.normalTexture.isValid() && !orm.isValid()) {
        logAsset("Could not create material '" + recipe.name + "': none of its textures imported.");
        return;
    }
    const AssetId id = m_assets->saveMaterial(recipe.outPath, m);
    if (!id.isValid()) {
        logAsset("Could not write material '" + recipe.outPath + "'.");
        return;
    }
    const AssetId changed[] = {id};
    m_assetBridge->assetsChanged(changed); // a re-download rewrites the same material: reload it where it is in use
    m_browser.refresh();
    if (m_options.workshopTest && !m_viewports.empty()) { // QA: behave like a drop onto the middle of the Scene view
        ViewportPanel& p = m_viewports.front();
        m_showWorkshop = false;
        p.pendingMaterial = id;
        m_renderer->requestPick(p.id, m_renderer->viewportWidth(p.id) / 2, m_renderer->viewportHeight(p.id) * 7 / 10);
    }
    logAsset("Created material '" + recipe.name + "' (" + recipe.outPath + "). Drag it onto an object to use it.");
}

void Editor::applyMaterial(AssetId material, size_t sceneIndex, Entity entity) {
    if (sceneIndex >= m_scenes.size() || !material.isValid()) return;
    World& world = m_scenes[sceneIndex].app->world();
    MeshRenderer* mr = entity.isNull() || !world.isAlive(entity) ? nullptr : world.get<MeshRenderer>(entity);
    if (!mr) {
        logAsset("That object has no mesh to put a material on.");
        return;
    }
    mr->material = material;
    mr->color = Vec4(1.0f); // the color is a tint on top of the material
    world.markModified<MeshRenderer>(entity);
    m_assetBridge->loader().request(material); // streams the material and its textures in the background
    const AssetInfo* info = m_assets->find(material);
    logAsset("Applied material '" + (info ? info->name : std::string("?")) + "'.");
}

void Editor::applyMaterialToSelection(const std::string& relPath) {
    const AssetId id = m_assets->findAuthored(relPath);
    if (!id.isValid()) return;
    if (!m_selection.valid(m_sceneRefs)) {
        logAsset("Select an object first (or drag the material onto one).");
        return;
    }
    applyMaterial(id, m_selection.scene, m_selection.entity);
}

void Editor::handleImportResult(const ImportResult& result) {
    // A material waits for every one of its textures; it is authored when the last one lands.
    for (size_t i = 0; i < m_pendingMaterials.size();) {
        PendingMaterial& pm = m_pendingMaterials[i];
        const auto it = std::find(pm.waiting.begin(), pm.waiting.end(), result.ticket);
        if (it != pm.waiting.end()) {
            pm.waiting.erase(it);
            pm.failed = pm.failed || !result.ok;
        }
        if (pm.waiting.empty()) {
            const MaterialRecipe recipe = pm.recipe;
            const bool failed = pm.failed;
            m_pendingMaterials.erase(m_pendingMaterials.begin() + static_cast<std::ptrdiff_t>(i));
            if (failed) logAsset("Material '" + recipe.name + "' skipped: a texture failed to import.");
            else createMaterialAsset(recipe);
            continue;
        }
        ++i;
    }
    Placement placement;
    bool dropped = false;
    if (const auto it = std::find_if(m_placements.begin(), m_placements.end(), [&](const Placement& p) { return p.ticket == result.ticket; });
        it != m_placements.end()) {
        placement = *it;
        dropped = true;
        m_placements.erase(it);
    }
    const std::string name = result.displayName.empty() ? result.sourcePath : result.displayName;
    if (!result.ok) {
        logAsset("Import of '" + name + "' failed: " + result.error);
        return;
    }
    if (dropped) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - placement.started).count();
        logAsset("Imported '" + name + "': " + std::to_string(result.assets.size()) + " asset(s) in " + std::to_string(static_cast<int>(ms)) +
                 " ms" + (result.upToDate ? " (unchanged)" : ""));
    } else if (!result.upToDate) {
        logAsset("Reimported '" + result.sourcePath + "' (" + std::to_string(result.changed.size()) + " asset(s) changed)");
    }
    for (const std::string& w : result.warnings) logAsset("  warning: " + w);

    m_assetBridge->assetsChanged(result.changed);

    m_browser.refresh();
    if (!dropped || !placement.place) return;
    const SourceFormat format = DetectSourceFormat(PathFromUtf8(result.sourcePath));
    if (format == SourceFormat::Image) return;
    if (!result.prefab.isValid()) {
        logAsset("  no prefab was generated (meshes only).");
        return;
    }
    size_t sceneIndex = 0;
    const Transform at = dropPlacement(placement.x, placement.y, placement.hasPosition, sceneIndex);
    instantiateAsset(result.prefab, sceneIndex, at);
}

void Editor::updateAssets(float dt) {
    // Once a second: look for sources that changed on disk (stat calls only) and start their reimports.
    m_sinceAssetPoll += dt;
    if (m_sinceAssetPoll >= 1.0f) {
        m_sinceAssetPoll = 0.0f;
        m_assets->beginReimportChanged();
    }
    for (const ImportResult& r : m_assets->pollImports()) handleImportResult(r);
    m_assetBridge->pump(); // loaded meshes/materials/textures -> renderer, a few ms per frame at most
    if (m_terrainPanel.created()) m_terrainPanel.updateMaterial(*m_assets, m_assetBridge->loader());
    if (m_roadTool.created()) m_roadTool.updateMaterial(*m_assets, m_assetBridge->loader());

    // Frame-time bookkeeping while anything is still streaming: the point of all this is that it stays smooth.
    const TextureStats ts = m_renderer->textureStats();
    const bool busy = m_assets->importsPending() || !m_assetBridge->loader().idle() || ts.queued > 0;
    if (busy) {
        const double ms = static_cast<double>(dt) * 1000.0;
        m_streaming.active = true;
        ++m_streaming.frames;
        m_streaming.sumMs += ms;
        m_streaming.worstMs = std::max(m_streaming.worstMs, ms);
    } else if (m_streaming.active) {
        logAsset("Streaming finished: " + std::to_string(m_streaming.frames) + " frames, average " +
                 std::to_string(static_cast<int>(m_streaming.sumMs / std::max(1u, m_streaming.frames) * 10.0) / 10.0).substr(0, 5) + " ms, worst " +
                 std::to_string(static_cast<int>(m_streaming.worstMs * 10.0) / 10.0).substr(0, 5) + " ms; " + std::to_string(ts.resident) +
                 " textures resident (" + std::to_string(ts.residentBytes >> 20) + " MB). Slowest phase: " +
                 m_streaming.worstPhase + " " + std::to_string(static_cast<int>(m_streaming.worstPhaseMs)) + " ms.");
        m_streaming = {};
    }
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

// The prefab editor's stage: an empty world with a sun and a 1.8 m reference figure to judge scale against.
void Editor::createPreviewScene() {
    AppConfig cfg;
    cfg.jobs = m_jobs.get();
    SceneEntry scene;
    scene.name = "Prefab Preview";
    scene.app = std::make_unique<App>(cfg);
    RenderExtractPlugin extract;
    scene.app->addPlugin(extract);
    AnimationPlugin animation;
    scene.app->addPlugin(animation);
    scene.app->insertResource(AnimationLink{&m_animation});
    scene.renderScene = m_renderer->createScene("Prefab Preview");
    scene.app->world().resource<RenderSceneLink>()->scene = m_renderer->scene(scene.renderScene);

    World& w = scene.app->world();
    w.spawn(Transform{{-1.5f, 0.9f, 0}, Quat(1, 0, 0, 0), {0.45f, 1.8f, 0.3f}},
            MeshRenderer{static_cast<u32>(BuiltinMesh::Cube), {0.35f, 0.4f, 0.55f, 1}}, Name::Make("Reference 1.8 m"));
    w.spawn(Transform{{0, 0, 0}, glm::quatLookAt(glm::normalize(Vec3(0.5f, -0.8f, -0.35f)), Vec3(0, 1, 0)), {1, 1, 1}},
            DirectionalLight{{1.0f, 0.96f, 0.9f}, 3.2f});
    m_previewScene = m_scenes.size();
    m_scenes.push_back(std::move(scene));
}

ViewportFlags Editor::FlagsOf(const ViewportPanel& p) {
    return (p.sky ? ViewportFlags::Sky : ViewportFlags::None) | (p.grid ? ViewportFlags::Grid : ViewportFlags::None) |
           (p.water ? ViewportFlags::Water : ViewportFlags::None) | (p.probes ? ViewportFlags::Probes : ViewportFlags::None);
}

// The editor's coast: the terrain mesh and the water's sea-floor texture come from the same height function.
void Editor::createWaterScene() {
    AppConfig cfg;
    cfg.jobs = m_jobs.get();
    SceneEntry scene;
    scene.name = "Water Test";
    scene.app = std::make_unique<App>(cfg);
    RenderExtractPlugin extract;
    scene.app->addPlugin(extract);
    scene.renderScene = m_renderer->createScene("Water Test");
    scene.app->world().resource<RenderSceneLink>()->scene = m_renderer->scene(scene.renderScene);

    const WaterTestMesh mesh = BuildWaterTestMesh(4.0f);
    const AssetId terrainId{0x57A7E5C0A57ull, 1};
    const MeshIndexRange whole{0, static_cast<u32>(mesh.indices.size())};
    if (!m_renderer->uploadMeshAsset(terrainId, MeshGeometry{mesh.positions, mesh.normals, mesh.uvs, {}, mesh.indices}, std::span(&whole, 1))) {
        FOH_ERROR("water test: could not upload the terrain mesh");
    }
    const u32 bedSize = 512;
    const std::vector<f32> bed = BuildWaterTestBed(bedSize);
    WaterBedDesc bd;
    bd.origin = Vec2(-WaterTestTerrain::kHalfSize);
    bd.size = Vec2(2.0f * WaterTestTerrain::kHalfSize);
    bd.width = bd.height = bedSize;
    bd.heights = bed;
    m_renderer->setWaterBed(bd);

    World& w = scene.app->world();
    MeshRenderer ground;
    ground.color = Vec4(0.55f, 0.50f, 0.38f, 1.0f);
    ground.asset = terrainId;
    w.spawn(Transform{}, ground, Name::Make("Coast"));
    // Things to reflect and to break the waves around: houses on the shore, a pier, a boat.
    auto box = [&](Vec3 at, Vec3 size, Vec4 color, const char* name) {
        w.spawn(Transform{at, Quat(1, 0, 0, 0), size}, MeshRenderer{static_cast<u32>(BuiltinMesh::Cube), color}, Name::Make(name));
    };
    for (int i = 0; i < 7; ++i) {
        const f32 x = -150.0f + 55.0f * static_cast<f32>(i), z = -215.0f - 14.0f * static_cast<f32>((i * 5) % 4);
        const f32 h = 6.0f + 3.0f * static_cast<f32>(i % 3);
        box({x, WaterTestTerrain::height(x, z) + h * 0.5f, z}, {11.0f, h, 9.0f},
            {0.75f - 0.07f * static_cast<f32>(i % 4), 0.62f, 0.50f + 0.06f * static_cast<f32>(i % 3), 1.0f}, "House");
    }
    box({60.0f, 0.9f, -60.0f}, {4.0f, 0.35f, 150.0f}, {0.38f, 0.30f, 0.22f, 1.0f}, "Pier");
    box({-120.0f, 0.5f, 120.0f}, {10.0f, 2.2f, 4.0f}, {0.85f, 0.85f, 0.88f, 1.0f}, "Boat");
    w.spawn(Transform{{0, 0, 0}, glm::quatLookAt(glm::normalize(Vec3(0.30f, -0.50f, -0.55f)), Vec3(0, 1, 0)), {1, 1, 1}},
            DirectionalLight{{1.0f, 0.96f, 0.9f}, 3.2f}, Name::Make("Sun"));
    m_waterScene = m_scenes.size();
    m_scenes.push_back(std::move(scene));
}

// Creates the terrain in the main world the first time it is needed (menu View > Terrain, or --terrain). The sea is
// switched on for the viewports of that world, so the coast reads right.
void Editor::ensureTerrain() {
    if (m_terrainPanel.created()) return;
    m_terrainPanel.ensureCreated(*m_renderer, *m_jobs, m_scenes[0].app->world(), fs::path(FOH_PROJECT_ROOT) / "world" / "terrain.fohterrain");
    // Game code walks on it: the player stands on the terrain and bumps into trunks (see foh/world/surface.h).
    if (!m_scenes[0].app->world().resource<WorldSurface>()) m_scenes[0].app->insertResource(WorldSurface{});
    if (WorldSurface* ws = m_scenes[0].app->world().resource<WorldSurface>()) {
        ws->terrain = m_terrainPanel.terrain();
        ws->vegetation = m_terrainPanel.vegetation();
    }
    m_roadTool.init(*m_renderer, m_scenes[0].app->world(), *m_terrainPanel.terrain(), m_terrainPanel.vegetation(),
                    fs::path(FOH_PROJECT_ROOT) / "world" / "roads.fohroads");
    m_cityPanel.ensureCreated(*m_renderer, *m_jobs, m_scenes[0].app->world(), m_terrainPanel, fs::path(FOH_PROJECT_ROOT) / "world");
    if (WorldSurface* ws = m_scenes[0].app->world().resource<WorldSurface>()) ws->city = m_cityPanel.city();
    if (m_options.roadsTest) {
        // QA: an avenue along Leblon beach a little inland of the shore, and a street climbing away from it, both carved.
        Terrain& t = *m_terrainPanel.terrain();
        auto shore = [&](f32 x) {
            f32 z = 1200.0f;
            while (z > -2500.0f && t.height(x, z) < 2.5f) z -= 1.0f;
            return z;
        };
        Road avenue;
        avenue.name = "Beach avenue";
        for (f32 x = 1200.0f; x <= 2400.0f; x += 300.0f) avenue.points.push_back({x, shore(x) - 35.0f});
        m_roadTool.addRoad(std::move(avenue), true);
        Road street;
        street.name = "Side street";
        street.width = 6.0f;
        const f32 z0 = shore(1500.0f) - 35.0f;
        for (int i = 0; i < 5; ++i) street.points.push_back({1500.0f + 12.0f * std::sin(f32(i)), z0 - 70.0f * f32(i)});
        m_roadTool.addRoad(std::move(street), true);
    }
    for (ViewportPanel& p : m_viewports) {
        if (p.sceneIndex != 0) continue;
        p.water = true;
        p.grid = false; // the editor grid lies at sea level, right through the water
        p.camera.farZ = std::max(p.camera.farZ, 14000.0f);
        m_renderer->setViewportFlags(p.id, FlagsOf(p));
    }
}

void Editor::shutdown() {
    if (m_renderer) m_renderer->device().waitIdle();
    m_roadTool.shutdown(); // saves the roads; before the terrain they stand on goes
    if (WorldSurface* ws = m_scenes.empty() ? nullptr : m_scenes[0].app->world().resource<WorldSurface>()) ws->city = nullptr;
    m_cityPanel.shutdown();
    if (WorldSurface* ws = m_scenes.empty() ? nullptr : m_scenes[0].app->world().resource<WorldSurface>()) *ws = WorldSurface{};
    m_terrainPanel.shutdown(); // saves the sculpting, then releases the tile meshes while the renderer and world still exist
    m_prefs.save();
    m_workshop.shutdown();
    m_assetInspector.flush();
    m_prefabEditor.shutdown();
    m_thumbs.shutdown();
    m_gameBuilder.reset();
    m_shaderBuilder.reset();
    m_game.reset();
    m_assetBridge.reset();
    m_assets.reset();
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
    bool startupImportDone = m_options.importFile.empty();
    while (m_window->pollEvents()) {
        for (const DroppedFile& drop : m_window->droppedFiles()) importFile(PathFromUtf8(drop.path), drop.x, drop.y, true);
        // --import (QA): behaves like a drop onto the middle of the Scene panel, once the panels have a size.
        if (!startupImportDone && rendered >= 3) {
            startupImportDone = true;
            const ViewportPanel& p = m_viewports.front();
            ImportSettings initial;
            bool haveInitial = false;
            size_t pos = 0;
            while (pos < m_options.importSettings.size()) {
                size_t end = m_options.importSettings.find('|', pos);
                if (end == std::string::npos) end = m_options.importSettings.size();
                const std::string item = m_options.importSettings.substr(pos, end - pos);
                pos = end + 1;
                const size_t eq = item.find('=');
                if (eq == std::string::npos) continue;
                const std::string key = item.substr(0, eq), value = item.substr(eq + 1);
                haveInitial = true;
                if (key == "animationOnly") initial.animationOnly = value != "0";
                else if (key == "clipSegments") initial.clipSegments = value;
                else if (key == "clipLoop") initial.clipLoop = value;
                else if (key == "clipRootMotion") initial.clipRootMotion = value;
                else haveInitial = haveInitial && true;
            }
            importFile(PathFromUtf8(m_options.importFile), p.imageX + p.imageW * 0.5f, p.imageY + p.imageH * 0.62f, true, true,
                       haveInitial ? &initial : nullptr);
        }
        if (rendered == 5 && !m_options.prefabEdit.empty()) openPrefabEditor(m_options.prefabEdit);
        if (rendered >= 30 && rendered < 200 && m_options.prefabQa && m_prefabEditor.isOpen() && m_prefabEditor.runSelfTest(1.8f)) m_options.prefabQa = false;
        if (rendered == 5 && !m_options.inspect.empty()) m_assetInspector.inspectPath(m_options.inspect);
        if (rendered == 8 && !m_options.inspect.empty()) ImGui::SetWindowFocus("Asset Inspector"); // QA: show it
        const auto now = Clock::now();
        if (m_options.profile) m_profileWorstMs = std::max(m_profileWorstMs, std::chrono::duration<double, std::milli>(now - last).count()); // unclamped: a freeze shows
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

void Editor::profileLap(const char* name, double ms) {
    if (!m_options.profile) return;
    for (auto& [n, sum] : m_profile)
        if (n == name || !std::strcmp(n, name)) {
            sum += ms;
            return;
        }
    m_profile.emplace_back(name, ms);
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
    {
        bool gameOwnsMouse = false;
        for (const ViewportPanel& p : m_viewports) gameOwnsMouse = gameOwnsMouse || (p.open && !p.useEditorCamera && p.sceneIndex == 0 && p.capturing);
        m_platform.setMouseCaptured(gameOwnsMouse);
    }
    m_theme.update(*m_window);
    m_workshop.update();
    m_prefabEditor.update();
    m_thumbs.beginFrame();
    m_platform.newFrame(dt);
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
    m_sceneRefs.clear();
    for (SceneEntry& sc : m_scenes) m_sceneRefs.push_back({sc.name.c_str(), &sc.app->world()});
    if (!m_selection.valid(m_sceneRefs)) m_selection.entity = kNullEntity;
    const auto uiStart = std::chrono::steady_clock::now();
    buildUi(dt);
    profileLap("build ui", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - uiStart).count());
    ImGui::Render();
    m_platform.endFrame();
    m_thumbs.endFrame(); // finished thumbnails reach the GPU before the UI is drawn
    m_prefs.saveIfDirty(ImGui::GetTime());
    {
        const auto t0 = std::chrono::steady_clock::now();
        if (m_options.terrainSculpt && m_terrainPanel.created() && m_frameCount >= 60 && m_frameCount < 150) {
        // QA: a stroke where the camera looks, through the same Terrain calls the brush tool makes (raise, then flatten).
        FlyCamera& cam = m_viewports[0].camera;
        const Vec3 dir = cam.rotation() * Vec3(0, 0, -1);
        Vec3 hit;
        if (m_terrainPanel.terrain()->raycast(cam.position, dir, 4000.0f, hit)) {
            if (m_frameCount == 60) m_terrainPanel.terrain()->beginStroke();
            BrushStamp s;
            s.mode = m_frameCount < 110 ? BrushMode::Raise : BrushMode::Flatten;
            s.center = Vec2(hit.x + std::sin(static_cast<float>(m_frameCount) * 0.1f) * 6.0f, hit.z);
            s.radius = 14.0f;
            s.dt = 1.0f / 60.0f;
            s.strength = 2.0f;
            s.flattenHeight = hit.y + 3.0f;
            m_terrainPanel.terrain()->applyBrush(s);
            if (m_frameCount == 149) m_terrainPanel.terrain()->endStroke();
        }
    }
    if (m_options.terrainSculpt && m_terrainPanel.created() && m_frameCount >= 150 && m_frameCount < 450) {
        // QA: fly 600 m along the coast and back, so nodes and vegetation tiles stream in and out (and mesh space is reused).
        static Vec3 start;
        if (m_frameCount == 150) start = m_viewports[0].camera.position;
        const float t = static_cast<float>(m_frameCount - 150) / 300.0f; // 0..1
        const float along = (t < 0.5f ? t * 2.0f : (1.0f - t) * 2.0f) * 600.0f;
        Vec3 p = start + Vec3(along, 0.0f, 0.0f);
        const f32 ground = m_terrainPanel.terrain()->height(p.x, p.z);
        p.y = std::max(p.y, ground + 1.8f);
        m_viewports[0].camera.position = p;
    }
    m_terrainPanel.update(m_viewports[0].camera.position); // before the worlds update: it spawns and re-meshes node entities
    if (m_cityPanel.created()) m_cityPanel.update(*m_terrainPanel.terrain(), m_viewports[0].camera.position);
        profileLap("terrain+vegetation update", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }

    // 2. Simulation: game hot reload, then every world. Extraction systems fill the
    //    renderer's scene snapshots at the end of each update.
    // Phase timer: while assets stream in, the slowest single phase is reported with the streaming summary.
    auto lapStart = std::chrono::steady_clock::now();
    auto lap = [&](const char* name) {
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - lapStart).count();
        lapStart = now;
        profileLap(name, ms);
        if (m_streaming.active && ms > m_streaming.worstPhaseMs) {
            m_streaming.worstPhaseMs = ms;
            m_streaming.worstPhase = name;
        }
    };
    if (m_gameBuilder) m_gameBuilder->poll();
    if (m_shaderBuilder) m_shaderBuilder->poll();
    if (m_game) m_game->pollReload();
    lap("hot reload polls");
    updateAssets(dt);
    lap("asset import/load pump");
    // The game gets the input only while a Game viewport owns it (clicked, not released with Escape).
    {
        bool playing = false;
        for (const ViewportPanel& p : m_viewports) playing = playing || (p.open && !p.useEditorCamera && p.sceneIndex == 0 && p.capturing);
        if (GameInput* in = m_scenes[0].app->world().resource<GameInput>()) {
            in->captured = playing;
            if (playing) in->state = m_window->input();
            else in->state = InputState{};
            if (!m_options.autoplay.empty()) applyAutoplay(*in);
        }
    }
    for (SceneEntry& s : m_scenes) s.app->update(dt);
    lap("world update");

    // 3. Render: viewports first (parallel graphs), then the window graph with the UI.
    m_renderer->beginFrame();
    lap("renderer beginFrame (texture/mesh uploads)");
    m_renderer->renderViewports();
    lap("render viewports");
    m_imguiRenderer.prepare(ImGui::GetDrawData(), m_externalTextures);
    m_renderer->renderWindow(*m_swapchain, &Editor::buildWindowPasses, this);
    lap("render window");

    ++m_frameCount;
    if (m_options.frames && !m_options.screenshot.empty() && m_frameCount == m_options.frames) saveScreenshot();
    if (!m_options.shots.empty() && !m_options.screenshot.empty()) {
        // QA: numbered screenshots at the listed frames (a contact sheet of a motion).
        size_t pos = 0;
        while (pos < m_options.shots.size()) {
            size_t end = m_options.shots.find(',', pos);
            if (end == std::string::npos) end = m_options.shots.size();
            if (std::strtoul(m_options.shots.c_str() + pos, nullptr, 10) == m_frameCount) {
                const std::string saved = m_options.screenshot;
                const fs::path stem = PathFromUtf8(saved).replace_extension("");
                m_options.screenshot = PathToUtf8(stem) + "_" + std::to_string(m_frameCount) + ".png";
                saveScreenshot();
                m_options.screenshot = saved;
            }
            pos = end + 1;
        }
    }

    m_swapchain->present();
    m_renderer->endFrame();
    if (m_options.profile && ++m_profileFrames == 120) {
        std::string line = "profile (avg ms over 120 frames):";
        for (auto& [n, sum] : m_profile) {
            line += std::format(" | {} {:.2f}", n, sum / 120.0);
            sum = 0.0;
        }
        line += std::format(" | WORST FRAME {:.0f} ms", m_profileWorstMs);
        m_profileWorstMs = 0.0;
        FOH_INFO("{}", line);
        std::string systems = std::format("World scene: {} entities; slowest systems (ms):", m_scenes[0].app->world().entityCount());
        std::vector<App::SystemTiming> t = m_scenes[0].app->systemTimings();
        std::sort(t.begin(), t.end(), [](const auto& a, const auto& b) { return a.ms > b.ms; });
        for (size_t i = 0; i < t.size() && i < 6; ++i) systems += std::format(" | {} {:.2f}", t[i].name, t[i].ms);
        FOH_INFO("{}", systems);
        ViewportDebugInfo info;
        if (m_renderer->viewportDebugInfo(m_viewports[1].id, info) && !info.gpu.empty()) {
            std::vector<ViewportDebugInfo::Timing> gpu = info.gpu;
            float total = 0.0f;
            for (const auto& g : gpu) total += g.ms;
            std::sort(gpu.begin(), gpu.end(), [](const auto& a, const auto& b) { return a.ms > b.ms; });
            std::string line2 = std::format("GPU Game view {:.2f} ms; heaviest passes (ms):", total);
            for (size_t i = 0; i < gpu.size() && i < 14; ++i) line2 += std::format(" | {} {:.2f}", gpu[i].name, gpu[i].ms);
            FOH_INFO("{}", line2);
        }
        m_profileFrames = 0;
    }
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
        if (ImGui::MenuItem("Quit", "Alt+F4")) m_quit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        for (ViewportPanel& p : m_viewports) ImGui::MenuItem(p.title.c_str(), nullptr, &p.open);
        ImGui::Separator();
        ImGui::MenuItem("Entities", nullptr, &m_showEntities);
        ImGui::MenuItem("Inspector", nullptr, &m_showInspector);
        ImGui::MenuItem("Content Browser", nullptr, &m_showAssets);
        ImGui::MenuItem("Asset Inspector", nullptr, &m_showAssetInspector);
        ImGui::MenuItem("Prefab Editor", nullptr, &m_showPrefabEditor);
        ImGui::MenuItem("Assets Workshop", nullptr, &m_showWorkshop);
        ImGui::MenuItem("Terrain", nullptr, &m_showTerrain);
        ImGui::MenuItem("Roads", nullptr, &m_showRoads);
        ImGui::MenuItem("City", nullptr, &m_showCity);
        ImGui::MenuItem("Atmosphere", nullptr, &m_showAtmosphere);
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
    ImGui::DockBuilderDockWindow("Asset Inspector", sideBottom);
    ImGui::DockBuilderDockWindow("Prefab Editor", sideBottom);
    bottom = ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, 0.44f, nullptr, &top);
    right = ImGui::DockBuilderSplitNode(top, ImGuiDir_Right, 0.34f, nullptr, &left);
    bottomRight = ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.24f, nullptr, &bottomLeft);
    ImGui::DockBuilderDockWindow("Scene", left);
    ImGui::DockBuilderDockWindow("Water Test", left);
    ImGui::DockBuilderDockWindow("Game", right);
    ImGui::DockBuilderDockWindow("Animation Debug", right);
    ImGui::DockBuilderDockWindow("Prefab Preview", right);
    ImGui::DockBuilderDockWindow("Content Browser", bottomLeft);
    ImGui::DockBuilderDockWindow("Render Graph", bottomRight);
    ImGui::DockBuilderDockWindow("Stats", bottomRight);
    ImGui::DockBuilderDockWindow("Atmosphere", sideBottom);
    ImGui::DockBuilderFinish(dockspaceId);
}

void Editor::buildUi(float dt) {
    drawMenuBar();
    const ImGuiID dockspace = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
    if (!m_dockLayoutBuilt) {
        buildDockLayout(dockspace);
        m_dockLayoutBuilt = true;
    }

    if (!m_options.playerModel.empty() && m_frameCount == 10) useAsModel(m_options.playerModel, true);
    if (!m_options.preview.empty()) previewStep();
    if (!m_options.autoplay.empty() && m_frameCount == 5) ImGui::SetWindowFocus("Game"); // QA: show the game view
    if (m_options.waterTest >= 0 && m_frameCount == 2) ImGui::SetWindowFocus("Water Test");
    if (!m_options.prefabEdit.empty() && m_options.walkSpeed > 0.0f && m_frameCount == 40) ImGui::SetWindowFocus(m_viewports[m_previewViewport].title.c_str()); // QA: show the stage
    for (ViewportPanel& p : m_viewports) drawViewport(p, dt);
    if (m_showEntities) m_inspector.drawEntities(m_sceneRefs, m_selection, &m_showEntities);
    if (m_showInspector) m_inspector.drawInspector(m_sceneRefs, m_selection, &m_showInspector);
    if (m_showAssets) m_browser.draw(&m_showAssets, m_theme.scale());
    {
        // The asset inspector follows the content browser's selection.
        const std::string selected = m_browser.singleSelection();
        if (selected != m_lastBrowserSelection) {
            m_lastBrowserSelection = selected;
            if (!selected.empty()) m_assetInspector.inspectPath(selected);
        }
        if (m_showAssetInspector) m_assetInspector.draw(&m_showAssetInspector);
        if (m_showPrefabEditor) m_prefabEditor.draw(&m_showPrefabEditor);
        if (m_prefabEditorWasShown && !m_showPrefabEditor) m_viewports[m_previewViewport].open = false;
        m_prefabEditorWasShown = m_showPrefabEditor;
    }
    if (m_showWorkshop) m_workshop.draw(&m_showWorkshop, m_theme.scale());
    if (m_showTerrain) {
        ensureTerrain();
        m_terrainPanel.draw(&m_showTerrain);
    }
    if (m_showCity) {
        ensureTerrain(); // the city stands on the terrain
        m_cityPanel.draw(&m_showCity);
    }
    if (m_showRoads) {
        ensureTerrain(); // roads stand on the terrain
        m_roadTool.draw(&m_showRoads);
    }
    if (m_showAtmosphere) m_atmospherePanel.draw(*m_renderer, &m_showAtmosphere);
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

    if (p.initialW > 0.0f) ImGui::SetNextWindowSize(ImVec2(p.initialW * m_theme.scale(), p.initialH * m_theme.scale()), ImGuiCond_FirstUseEver);
    if (m_options.waterTest >= 0 && &p == &m_viewports[m_waterViewport] && m_frameCount < 8) {
        // QA (--water-test): the water panel takes the whole window so screenshots show it big.
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
        ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(vp->WorkSize, ImGuiCond_Always);
    }
    if (m_options.fullGame && &p == &m_viewports[1] && m_frameCount < 8) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
        ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(vp->WorkSize, ImGuiCond_Always);
    }
    if (m_options.terrain && m_options.autoplay.empty() && &p == &m_viewports[0] && m_frameCount < 8) {
        // QA (--terrain): the Scene panel takes the whole window so screenshots show the terrain big.
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
        ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(vp->WorkSize, ImGuiCond_Always);
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
        p.imageX = origin.x;
        p.imageY = origin.y;
        p.imageW = avail.x;
        p.imageH = avail.y;
        // Assets dragged out of the content browser land where they are dropped.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetDragPayload)) {
                const std::string rel(static_cast<const char*>(payload->Data));
                const ImVec2 mouse = ImGui::GetMousePos();
                const FileTypeInfo type = ClassifyExtension([&] {
                    std::string ext = PathToUtf8(PathFromUtf8(rel).extension());
                    for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    return ext.empty() ? ext : ext.substr(1);
                }());
                if (type.category == FileCategory::Material) {
                    // Which object is under the cursor is only known after the id pass: apply on the answer.
                    const float fx = (mouse.x - origin.x) / std::max(avail.x, 1.0f), fy = (mouse.y - origin.y) / std::max(avail.y, 1.0f);
                    p.pendingMaterial = m_assets->findAuthored(rel);
                    m_renderer->requestPick(p.id, static_cast<u32>(std::clamp(fx, 0.0f, 0.999f) * static_cast<float>(m_renderer->viewportWidth(p.id))),
                                            static_cast<u32>(std::clamp(fy, 0.0f, 0.999f) * static_cast<float>(m_renderer->viewportHeight(p.id))));
                } else if (type.category == FileCategory::Model || type.category == FileCategory::Prefab) placeAssetByPath(rel, mouse.x, mouse.y, true);
                else logAsset("'" + rel + "' cannot be placed in a scene (only models can).");
            }
            ImGui::EndDragDropTarget();
        }

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
            // Playing: a click in the Game viewport gives the game the mouse and keyboard, Escape gives them back.
            if (p.sceneIndex == 0) {
                if (hovered && !p.capturing && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    p.capturing = true;
                    m_window->setRelativeMouse(true);
                }
                if (p.capturing && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                    p.capturing = false;
                    m_window->setRelativeMouse(false);
                }
                if (p.capturing) ImGui::SetNextFrameWantCaptureKeyboard(true);
                ImGui::SetCursorScreenPos(ImVec2(origin.x + 8, origin.y + avail.y - 24.0f * m_theme.scale()));
                ImGui::TextColored(ImVec4(1, 1, 1, 0.75f), p.capturing ? "Playing: WASD move, Shift run, Space jump, C crouch, V camera, Esc release"
                                                                       : "Click to play");
            }
        }

        const int panelIndex = static_cast<int>(&p - m_viewports.data());
        if (p.useEditorCamera) drawGizmo(p, origin, avail, panelIndex);
        // The terrain brush owns the left mouse while it is active over the main world; otherwise a click selects.
        bool terrainTool = false;
        if (p.useEditorCamera && p.sceneIndex == 0 && m_showRoads && m_roadTool.editing() && m_roadTool.created()) {
            terrainTool = m_roadTool.handleViewport(p.camera.world(), p.camera.fovY, origin, avail, hovered, p.capturing,
                                                    ImGuizmo::IsOver() || ImGuizmo::IsUsing(), dt);
        } else if (p.useEditorCamera && p.sceneIndex == 0 && m_showTerrain && m_terrainPanel.created()) {
            terrainTool = m_terrainPanel.handleViewport(p.camera.world(), p.camera.fovY, origin, avail, hovered, p.capturing,
                                                        ImGuizmo::IsOver() || ImGuizmo::IsUsing(), dt);
        }
        if (!terrainTool) handlePicking(p, origin, avail, hovered);

        // Overlay controls (top-left of the image), on a translucent backdrop so they stay readable on a bright sky.
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 8, origin.y + 8));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.04f, 0.045f, 0.05f, 0.72f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
        ImGui::PushID(p.title.c_str());
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5.0f * m_theme.scale(), 2.0f * m_theme.scale()));
        if (ImGui::BeginChild("##overlay", ImVec2(0, 0),
                              ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            ImGui::SetNextItemWidth(150.0f * m_theme.scale());
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
            if (ImGui::Checkbox("Grid", &p.grid)) m_renderer->setViewportFlags(p.id, FlagsOf(p));
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopID();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    } else if (p.capturing) {
        p.capturing = false;
        m_window->setRelativeMouse(false);
    }
    ImGui::End();
}

// Quality and pass settings shared by every viewport (Renderer::setRenderSettings), kept in the editor prefs.
void Editor::loadRenderSettings() {
    RenderSettings r = m_renderer->renderSettings();
    r.shadows = m_prefs.getBool("render.shadows", r.shadows);
    r.shadowCascadeSize = static_cast<u32>(m_prefs.getFloat("render.shadowSize", static_cast<float>(r.shadowCascadeSize)));
    r.shadowDistance = m_prefs.getFloat("render.shadowDistance", r.shadowDistance);
    r.atmosphere = m_prefs.getBool("render.atmosphere", r.atmosphere);
    r.aerialPerspective = m_prefs.getBool("render.aerial", r.aerialPerspective);
    r.exposure = m_prefs.getFloat("render.exposure", r.exposure);
    r.dither = m_prefs.getBool("render.dither", r.dither);
    r.tonemapper = static_cast<Tonemapper>(static_cast<int>(m_prefs.getFloat("render.tonemapper", static_cast<float>(r.tonemapper))));
    r.whitePoint = m_prefs.getFloat("render.whitePoint", r.whitePoint);
    r.autoExposure = m_prefs.getBool("render.autoExposure", r.autoExposure);
    r.exposureCompensation = m_prefs.getFloat("render.exposureComp", r.exposureCompensation);
    r.exposureMinLuminance = m_prefs.getFloat("render.exposureMinLum", r.exposureMinLuminance);
    r.exposureMaxLuminance = m_prefs.getFloat("render.exposureMaxLum", r.exposureMaxLuminance);
    r.adaptBrighten = m_prefs.getFloat("render.adaptBrighten", r.adaptBrighten);
    r.adaptDarken = m_prefs.getFloat("render.adaptDarken", r.adaptDarken);
    r.meterCenterWeight = m_prefs.getFloat("render.meterCenter", r.meterCenterWeight);
    r.bloom = m_prefs.getBool("render.bloom", r.bloom);
    r.bloomStrength = m_prefs.getFloat("render.bloomStrength", r.bloomStrength);
    r.bloomLevels = static_cast<u32>(m_prefs.getFloat("render.bloomLevels", static_cast<float>(r.bloomLevels)));
    r.gtao = m_prefs.getBool("render.gtao", r.gtao);
    r.gtaoRadius = m_prefs.getFloat("render.gtaoRadius", r.gtaoRadius);
    r.gtaoStrength = m_prefs.getFloat("render.gtaoStrength", r.gtaoStrength);
    r.contactShadows = m_prefs.getBool("render.contactShadows", r.contactShadows);
    r.contactShadowLength = m_prefs.getFloat("render.contactShadowLength", r.contactShadowLength);
    r.canopyTransmission = m_prefs.getFloat("render.canopyTransmission", r.canopyTransmission);
    r.ibl = m_prefs.getBool("render.ibl", r.ibl);
    r.iblIntensity = m_prefs.getFloat("render.iblIntensity", r.iblIntensity);
    r.probes = m_prefs.getBool("render.probes", r.probes);
    r.probeSpacing = m_prefs.getFloat("render.probeSpacing", r.probeSpacing);
    r.probesPerFrame = static_cast<u32>(m_prefs.getFloat("render.probesPerFrame", static_cast<float>(r.probesPerFrame)));
    r.probeIntensity = m_prefs.getFloat("render.probeIntensity", r.probeIntensity);
    r.probeInterval = static_cast<u32>(m_prefs.getFloat("render.probeInterval", static_cast<float>(r.probeInterval)));
    m_renderer->setRenderSettings(r);
}

void Editor::drawRenderSettings() {
    RenderSettings r = m_renderer->renderSettings();
    const RenderSettings before = r;
    if (ImGui::CollapsingHeader("Pass settings", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        if (ImGui::TreeNodeEx("ShadowCascades", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enabled##shadows", &r.shadows);
            ImGui::BeginDisabled(!r.shadows);
            static const u32 kSizes[] = {512, 1024, 2048, 4096};
            int idx = 0;
            for (int i = 0; i < 4; ++i) if (kSizes[i] <= r.shadowCascadeSize) idx = i;
            const char* labels[] = {"512", "1024", "2048", "4096"};
            if (ImGui::Combo("Cascade resolution", &idx, labels, 4)) r.shadowCascadeSize = kSizes[idx];
            ImGui::SliderFloat("Distance (m)", &r.shadowDistance, 20.0f, 2000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
            ImGui::EndDisabled();
            ImGui::TreePop();
        }
        if (ImGui::TreeNodeEx("Sky + Deferred (atmosphere)", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Atmosphere lookups", &r.atmosphere);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: gradient sky and constant ambient (the tables themselves are edited in the Atmosphere panel).");
            ImGui::BeginDisabled(!r.atmosphere);
            ImGui::Checkbox("Aerial perspective", &r.aerialPerspective);
            ImGui::EndDisabled();
            ImGui::TreePop();
        }
        if (ImGui::TreeNodeEx("Exposure", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Auto exposure", &r.autoExposure);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Luminance histogram of the HDR image; adapts toward middle grey, per viewport.");
            ImGui::BeginDisabled(r.autoExposure);
            ImGui::SliderFloat("Exposure", &r.exposure, 0.01f, 64.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!r.autoExposure);
            ImGui::SliderFloat("Compensation (EV)", &r.exposureCompensation, -4.0f, 4.0f, "%.2f");
            ImGui::SliderFloat("Adapt to brighter (1/s)", &r.adaptBrighten, 0.1f, 20.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Adapt to darker (1/s)", &r.adaptDarken, 0.1f, 20.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Center weight", &r.meterCenterWeight, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("0: every pixel counts the same. 1: the weight falls to zero at the corners.");
            ImGui::SliderFloat("Histogram min (log2 lum)", &r.exposureMinLuminance, -16.0f, 8.0f, "%.1f");
            ImGui::SliderFloat("Histogram max (log2 lum)", &r.exposureMaxLuminance, -8.0f, 16.0f, "%.1f");
            ImGui::EndDisabled();
            ImGui::TreePop();
        }
        if (ImGui::TreeNodeEx("Bloom", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enabled##bloom", &r.bloom);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Call of Duty: Advanced Warfare: 13-tap downsample chain, tent-filtered upsample with accumulation.");
            ImGui::BeginDisabled(!r.bloom);
            ImGui::SliderFloat("Strength", &r.bloomStrength, 0.0f, 0.5f, "%.3f", ImGuiSliderFlags_Logarithmic);
            int levels = static_cast<int>(r.bloomLevels);
            if (ImGui::SliderInt("Levels", &levels, 1, 8)) r.bloomLevels = static_cast<u32>(levels);
            ImGui::EndDisabled();
            ImGui::TreePop();
        }
        if (ImGui::TreeNodeEx("Ambient occlusion (GTAO)", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enabled##gtao", &r.gtao);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ground-truth AO from the depth buffer: darkens ambient and sky reflection in creases and under overhangs.");
            ImGui::BeginDisabled(!r.gtao);
            ImGui::SliderFloat("Radius (m)", &r.gtaoRadius, 0.2f, 4.0f, "%.2f");
            ImGui::SliderFloat("Strength", &r.gtaoStrength, 0.2f, 3.0f, "%.2f");
            ImGui::EndDisabled();
            ImGui::TreePop();
        }
        if (ImGui::TreeNodeEx("Sun shadows: contact + canopy", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Contact shadows", &r.contactShadows);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Screen-space ray march toward the sun: grass, bark ridges, the crease under a ledge.");
            ImGui::BeginDisabled(!r.contactShadows);
            ImGui::SliderFloat("Contact length (m)", &r.contactShadowLength, 0.1f, 2.0f, "%.2f");
            ImGui::EndDisabled();
            ImGui::SliderFloat("Leaf transmission", &r.canopyTransmission, 0.0f, 2.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sun that leaks through the first layer of leaves above a surface (green, dappled shade). 0 = off.");
            ImGui::TreePop();
        }
        if (ImGui::TreeNodeEx("Indirect light (IBL + probes)", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Sky reflections (IBL)", &r.ibl);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The sky and clouds as an image every surface reflects, blurrier for rough ones. Needs the atmosphere.");
            ImGui::BeginDisabled(!r.ibl);
            ImGui::SliderFloat("Reflection intensity", &r.iblIntensity, 0.0f, 4.0f, "%.2f");
            ImGui::EndDisabled();
            ImGui::Checkbox("Light probes", &r.probes);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Ambient light measured from the scene around the camera, a few probes per frame. Needs terrain (its height field) and the atmosphere.");
            ImGui::BeginDisabled(!r.probes);
            ImGui::SliderFloat("Probe spacing (m)", &r.probeSpacing, 1.0f, 12.0f, "%.1f");
            int perFrame = static_cast<int>(r.probesPerFrame);
            if (ImGui::SliderInt("Probes per frame", &perFrame, 1, 8)) r.probesPerFrame = static_cast<u32>(perFrame);
            int interval = static_cast<int>(r.probeInterval);
            if (ImGui::SliderInt("Refresh every N frames", &interval, 1, 8)) r.probeInterval = static_cast<u32>(interval);
            ImGui::SliderFloat("Probe intensity", &r.probeIntensity, 0.0f, 2.0f, "%.2f");
            ImGui::EndDisabled();
            ImGui::TreePop();
        }
        if (ImGui::TreeNodeEx("Tonemap", ImGuiTreeNodeFlags_DefaultOpen)) {
            static const char* kTonemappers[] = {"Linear (clamp)", "Reinhard", "Reinhard extended", "Uncharted 2", "ACES (fit)", "PBR Neutral"};
            static_assert(sizeof(kTonemappers) / sizeof(kTonemappers[0]) == kTonemapperCount, "tonemapper names out of sync");
            int op = static_cast<int>(r.tonemapper);
            if (ImGui::Combo("Operator", &op, kTonemappers, static_cast<int>(kTonemapperCount))) r.tonemapper = static_cast<Tonemapper>(op);
            const bool usesWhite = r.tonemapper == Tonemapper::ReinhardExtended || r.tonemapper == Tonemapper::Uncharted2;
            ImGui::BeginDisabled(!usesWhite);
            ImGui::SliderFloat("White point", &r.whitePoint, 1.0f, 32.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            ImGui::EndDisabled();
            ImGui::Checkbox("Dither", &r.dither);
            ImGui::TreePop();
        }
        ImGui::PopItemWidth();
        ImGui::Checkbox("GPU timers (per-pass ms in the table below)", &r.gpuTimers);
        if (ImGui::Button("Reset")) r = RenderSettings{};
        ImGui::SameLine();
        ImGui::TextDisabled("Applies to every viewport, saved with the editor prefs.");
    }
    if (std::memcmp(&before, &r, sizeof(r)) != 0) {
        m_renderer->setRenderSettings(r);
        r = m_renderer->renderSettings(); // clamped
        m_prefs.setBool("render.shadows", r.shadows);
        m_prefs.setFloat("render.shadowSize", static_cast<float>(r.shadowCascadeSize));
        m_prefs.setFloat("render.shadowDistance", r.shadowDistance);
        m_prefs.setBool("render.atmosphere", r.atmosphere);
        m_prefs.setBool("render.aerial", r.aerialPerspective);
        m_prefs.setFloat("render.exposure", r.exposure);
        m_prefs.setBool("render.dither", r.dither);
        m_prefs.setFloat("render.tonemapper", static_cast<float>(r.tonemapper));
        m_prefs.setFloat("render.whitePoint", r.whitePoint);
        m_prefs.setBool("render.autoExposure", r.autoExposure);
        m_prefs.setFloat("render.exposureComp", r.exposureCompensation);
        m_prefs.setFloat("render.exposureMinLum", r.exposureMinLuminance);
        m_prefs.setFloat("render.exposureMaxLum", r.exposureMaxLuminance);
        m_prefs.setFloat("render.adaptBrighten", r.adaptBrighten);
        m_prefs.setFloat("render.adaptDarken", r.adaptDarken);
        m_prefs.setFloat("render.meterCenter", r.meterCenterWeight);
        m_prefs.setBool("render.bloom", r.bloom);
        m_prefs.setFloat("render.bloomStrength", r.bloomStrength);
        m_prefs.setFloat("render.bloomLevels", static_cast<float>(r.bloomLevels));
        m_prefs.setBool("render.gtao", r.gtao);
        m_prefs.setFloat("render.gtaoRadius", r.gtaoRadius);
        m_prefs.setFloat("render.gtaoStrength", r.gtaoStrength);
        m_prefs.setBool("render.contactShadows", r.contactShadows);
        m_prefs.setFloat("render.contactShadowLength", r.contactShadowLength);
        m_prefs.setFloat("render.canopyTransmission", r.canopyTransmission);
        m_prefs.setBool("render.ibl", r.ibl);
        m_prefs.setFloat("render.iblIntensity", r.iblIntensity);
        m_prefs.setBool("render.probes", r.probes);
        m_prefs.setFloat("render.probeSpacing", r.probeSpacing);
        m_prefs.setFloat("render.probesPerFrame", static_cast<float>(r.probesPerFrame));
        m_prefs.setFloat("render.probeIntensity", r.probeIntensity);
        m_prefs.setFloat("render.probeInterval", static_cast<float>(r.probeInterval));
    }
}

void Editor::drawRenderGraphPanel() {
    if (!ImGui::Begin("Render Graph", &m_showRenderGraph)) {
        ImGui::End();
        return;
    }
    drawRenderSettings();
    ImGui::TextDisabled("One graph instance per viewport; passes run in declaration order, culled ones are skipped.");
    for (ViewportPanel& p : m_viewports) {
        ViewportDebugInfo info;
        if (!m_renderer->viewportDebugInfo(p.id, info)) continue;
        const bool open = ImGui::CollapsingHeader(info.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        if (!open) continue;
        ImGui::PushID(info.name.c_str());
        // Per-viewport pass switches: the sky pass (off = flat clear) and the editor grid pass.
        bool changed = ImGui::Checkbox("Sky pass", &p.sky);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Grid pass", &p.grid);
        if (ImGui::Checkbox("Water pass", &p.water) || changed) m_renderer->setViewportFlags(p.id, FlagsOf(p));
        ImGui::PopID();
        ImGui::Text("%ux%u  |  %u instances  |  %u command list group(s)  |  %u/%u physical/virtual transients  |  %u barriers",
                    info.width, info.height, info.instances, info.groups, info.physicalTextures,
                    info.transientTextures, info.barriers);
        float gpuTotal = 0.0f;
        for (const auto& t : info.gpu) gpuTotal += t.ms;
        if (!info.gpu.empty()) ImGui::Text("GPU %.2f ms (sum of the passes, a few frames old)", gpuTotal);
        if (ImGui::BeginTable(("passes##" + info.name).c_str(), 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("GPU ms");
            ImGui::TableSetupColumn("Group");
            ImGui::TableSetupColumn("State");
            ImGui::TableHeadersRow();
            size_t liveIndex = 0;
            for (const auto& pass : info.passes) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (pass.culled) ImGui::TextDisabled("%s", pass.name); else ImGui::TextUnformatted(pass.name);
                ImGui::TableNextColumn();
                // The timings come in the order of the live (not culled) passes.
                if (!pass.culled && liveIndex < info.gpu.size()) ImGui::Text("%.3f", info.gpu[liveIndex].ms); else ImGui::TextDisabled("-");
                if (!pass.culled) ++liveIndex;
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

// ImGuizmo works on world matrices; Transform is local to the parent. Converts the manipulated world
// matrix back through the parent chain (the parent's GlobalTransform is current: it is computed every frame).
Mat4 worldMatrixOf(const World& world, Entity e) {
    if (const GlobalTransform* g = world.get<GlobalTransform>(e)) return g->matrix();
    const Transform* t = world.get<Transform>(e);
    return t ? t->matrix() : Mat4(1.0f);
}

void applyWorldMatrix(World& world, Entity e, const Mat4& newWorld) {
    Transform* t = world.get<Transform>(e);
    if (!t) return;
    Mat4 local = newWorld;
    const Entity parent = world.parentOf(e);
    if (!parent.isNull() && world.has<GlobalTransform>(parent))
        local = glm::inverse(world.get<GlobalTransform>(parent)->matrix()) * newWorld;
    *t = Transform::FromMatrix(local);
    world.markModified<Transform>(e); // in-place write: tell the propagation system
}

} // namespace

// Transform gizmo (ImGuizmo) for the selected entity, drawn over an editor-camera viewport.
// ImGuizmo expects an OpenGL-style projection (it unprojects the mouse with it), so it gets
// one with the same fov/planes instead of our reverse-Z matrix.
void Editor::drawGizmo(ViewportPanel& p, const ImVec2& origin, const ImVec2& size, int panelIndex) {
    if (!m_selection.valid(m_sceneRefs) || m_selection.scene != p.sceneIndex) return;
    World& world = *m_sceneRefs[m_selection.scene].world;
    if (!world.has<Transform>(m_selection.entity) || size.x < 8 || size.y < 8) return;
    Mat4 model = worldMatrixOf(world, m_selection.entity);
    const Vec3 worldPosition = Vec3(model[3]);
    const f32 worldExtent = std::max({glm::length(Vec3(model[0])), glm::length(Vec3(model[1])), glm::length(Vec3(model[2]))});

    // Hotkeys, only while the cursor is over this view and the camera is not flying.
    const bool hotkeys = ImGui::IsWindowHovered() && !p.capturing && !ImGui::GetIO().WantTextInput;
    if (hotkeys) {
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) m_gizmoOp = ImGuizmo::TRANSLATE;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) m_gizmoOp = ImGuizmo::ROTATE;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) m_gizmoOp = ImGuizmo::SCALE;
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
            // Frame the selection: back off along the current view direction.
            const f32 dist = std::max(4.0f, worldExtent * 3.0f);
            p.camera.position = worldPosition - (p.camera.rotation() * Vec3(0, 0, -1)) * dist;
        }
    }

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(origin.x, origin.y, size.x, size.y);
    ImGuizmo::SetID(panelIndex);
    const Mat4 view = glm::inverse(p.camera.world());
    const Mat4 proj = glm::perspectiveRH_NO(p.camera.fovY, size.x / size.y, p.camera.nearZ, p.camera.farZ);
    const bool snap = ImGui::GetIO().KeyCtrl;
    const float step = m_gizmoOp == ImGuizmo::ROTATE ? 15.0f : (m_gizmoOp == ImGuizmo::SCALE ? 0.1f : 0.5f);
    const float snapAll[3] = {step, step, step};
    if (ImGuizmo::Manipulate(&view[0][0], &proj[0][0], static_cast<ImGuizmo::OPERATION>(m_gizmoOp),
                             m_gizmoLocal ? ImGuizmo::LOCAL : ImGuizmo::WORLD, &model[0][0], nullptr,
                             snap ? snapAll : nullptr)) {
        applyWorldMatrix(world, m_selection.entity, model);
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
        if (p.pendingMaterial.isValid()) {
            applyMaterial(p.pendingMaterial, p.sceneIndex, e);
            p.pendingMaterial = {};
            if (!e.isNull() && world.isAlive(e)) {
                m_selection.scene = p.sceneIndex;
                m_selection.entity = e;
            }
        } else if (!e.isNull() && world.isAlive(e)) {
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
    // The atmosphere panel and a regeneration (compute passes + cache dump) while frames are in flight.
    if (f == 20) m_showAtmosphere = true;
    // Pass settings changing under load: shadow atlas resize, shadows off, atmosphere off, back to defaults.
    if (f == 120 || f == 180 || f == 220 || f == 260) {
        RenderSettings r = m_renderer->renderSettings();
        if (f == 120) r.shadowCascadeSize = 1024;
        if (f == 180) r.shadows = false;
        if (f == 220) { r.shadows = true; r.atmosphere = false; }
        if (f == 260) r = RenderSettings{};
        m_renderer->setRenderSettings(r);
    }
    if (f == 70) {
        AtmosphereDesc d = m_renderer->atmosphereDesc();
        d.mieScale = 2.0f;
        m_renderer->generateAtmosphere(d);
    }
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
