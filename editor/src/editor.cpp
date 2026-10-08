#include "editor.h"

#include <algorithm>
#include <cctype>
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
    FOH_INFO("asset database: {} source(s) in {}", m_assets->sources().size(), PathToUtf8(desc.sourceRoot));
    logAsset("Drop a .gltf / .glb / .fbx model or a texture onto the window to import it.");

    ContentBrowserActions actions;
    actions.importFile = [this](const fs::path& file) { importFile(file, 0, 0, false, false); };
    actions.applyMaterial = [this](const std::string& rel) { applyMaterialToSelection(rel); };
    actions.placeModel = [this](const std::string& rel) { placeAssetByPath(rel, 0, 0, false); };
    actions.openWorkshop = [this] { m_showWorkshop = true; };
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
}

void Editor::placeAssetByPath(const std::string& relPath, float x, float y, bool hasPosition) {
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
    logAsset("Placed " + std::to_string(inst.entities.size()) + " entities into '" + m_scenes[sceneIndex].name + "'.");
}

ImportTicket Editor::importFile(const fs::path& file, float dropX, float dropY, bool hasDropPosition, bool place) {
    const std::string name = PathToUtf8(file.filename());
    if (DetectSourceFormat(file) == SourceFormat::Unknown) {
        logAsset("Cannot import '" + name + "': supported are .gltf, .glb, .fbx, .png, .jpg, .tga, .bmp.");
        return 0;
    }
    // Returns at once: parsing, texture decoding, mips and BC compression run on the worker threads.
    Placement p;
    p.ticket = m_assets->beginImport(file);
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

void Editor::shutdown() {
    if (m_renderer) m_renderer->device().waitIdle();
    m_prefs.save();
    m_workshop.shutdown();
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
            importFile(PathFromUtf8(m_options.importFile), p.imageX + p.imageW * 0.5f, p.imageY + p.imageH * 0.62f, true);
        }
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
    m_theme.update(*m_window);
    m_workshop.update();
    m_thumbs.beginFrame();
    m_platform.newFrame(dt);
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
    m_sceneRefs.clear();
    for (SceneEntry& sc : m_scenes) m_sceneRefs.push_back({sc.name.c_str(), &sc.app->world()});
    if (!m_selection.valid(m_sceneRefs)) m_selection.entity = kNullEntity;
    buildUi(dt);
    ImGui::Render();
    m_platform.endFrame();
    m_thumbs.endFrame(); // finished thumbnails reach the GPU before the UI is drawn
    m_prefs.saveIfDirty(ImGui::GetTime());

    // 2. Simulation: game hot reload, then every world. Extraction systems fill the
    //    renderer's scene snapshots at the end of each update.
    // Phase timer: while assets stream in, the slowest single phase is reported with the streaming summary.
    auto lapStart = std::chrono::steady_clock::now();
    auto lap = [&](const char* name) {
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - lapStart).count();
        lapStart = now;
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
        if (ImGui::MenuItem("Quit", "Alt+F4")) m_quit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        for (ViewportPanel& p : m_viewports) ImGui::MenuItem(p.title.c_str(), nullptr, &p.open);
        ImGui::Separator();
        ImGui::MenuItem("Entities", nullptr, &m_showEntities);
        ImGui::MenuItem("Inspector", nullptr, &m_showInspector);
        ImGui::MenuItem("Content Browser", nullptr, &m_showAssets);
        ImGui::MenuItem("Assets Workshop", nullptr, &m_showWorkshop);
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
    bottom = ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, 0.44f, nullptr, &top);
    right = ImGui::DockBuilderSplitNode(top, ImGuiDir_Right, 0.34f, nullptr, &left);
    bottomRight = ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.24f, nullptr, &bottomLeft);
    ImGui::DockBuilderDockWindow("Scene", left);
    ImGui::DockBuilderDockWindow("Game", right);
    ImGui::DockBuilderDockWindow("Animation Debug", right);
    ImGui::DockBuilderDockWindow("Content Browser", bottomLeft);
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
    if (m_showAssets) m_browser.draw(&m_showAssets, m_theme.scale());
    if (m_showWorkshop) m_workshop.draw(&m_showWorkshop, m_theme.scale());
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
                } else if (type.category == FileCategory::Model) placeAssetByPath(rel, mouse.x, mouse.y, true);
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
        }

        const int panelIndex = static_cast<int>(&p - m_viewports.data());
        if (p.useEditorCamera) drawGizmo(p, origin, avail, panelIndex);
        handlePicking(p, origin, avail, hovered);

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
            if (ImGui::Checkbox("Grid", &p.grid)) {
                m_renderer->setViewportFlags(p.id, ViewportFlags::Sky | (p.grid ? ViewportFlags::Grid : ViewportFlags::None));
            }
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
