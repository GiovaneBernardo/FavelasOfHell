#include <chrono>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include "foh/animation/animation.h"
#include "foh/asset_db.h"
#include "foh/dev_build_defaults.h"
#include "foh/input.h"
#include "foh/ecs.h"
#include "foh/file_system.h"
#include "foh/game_module_host.h"
#include "foh/jobs.h"
#include "foh/log.h"
#include "foh/png.h"
#include "foh/renderer/renderer.h"
#include "foh/rhi/graphics_device.h"
#include "foh/rhi/render_graph.h"
#include "foh/rhi/swapchain.h"
#include "foh/scene/asset_scene_bridge.h"
#include "foh/scene/prefab_request.h"
#include "foh/window.h"

// Standalone player: one window, the game world, one full-window viewport that
// renders through the game's camera and is blitted to the swapchain.
//
// Usage: foh_runtime [--frames N] [--screenshot out.png] [--no-vsync] [--no-validation]
//   --frames N      render N frames, then exit (smoke test; exit code 2 on graphics API errors)
//   --screenshot f  with --frames: save the last frame as a PNG
int main(int argc, char** argv) {
    using Clock = std::chrono::steady_clock;
    foh::u32 maxFrames = 0;
    bool vsync = true;
    const char* screenshot = nullptr;
    bool validation =
#if defined(NDEBUG)
        false;
#else
        true;
#endif
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) maxFrames = static_cast<foh::u32>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
        else if (!std::strcmp(argv[i], "--no-vsync")) vsync = false;
        else if (!std::strcmp(argv[i], "--no-validation")) validation = false;
    }

    auto window = foh::Window::create({.title = "Favelas of Hell"});
    if (!window) return 1;

    // Declared in dependency order: destroyed in reverse (game before app before renderer before jobs).
    foh::JobSystem jobs(foh::JobSystem::defaultWorkerCount());
    foh::RendererDesc rd;
    rd.jobs = &jobs;
    rd.validation = validation;
    auto renderer = foh::Renderer::create(rd);
    if (!renderer) return 1;

    foh::u32 w = 0, h = 0;
    window->getSize(w, h);
    auto swapchain = foh::Swapchain::create(renderer->device(), {window->nativeWindowHandle(), w, h, vsync});
    if (!swapchain) return 1;

    // Assets: imported models, clips and textures stream in from the project's cache (the same database the editor
    // fills). Declared before the App, so they outlive the worlds that point at them.
    foh::AnimationLibrary animation;
    foh::AssetDbDesc assetDesc;
    assetDesc.sourceRoot = std::filesystem::path(FOH_PROJECT_ROOT) / "assets";
    assetDesc.cacheRoot = std::filesystem::path(FOH_PROJECT_ROOT) / "cache" / "imported";
    assetDesc.jobs = &jobs;
    foh::AssetDb assets(assetDesc);
    assets.scan();
    foh::AssetSceneBridge bridge(assets, *renderer, &jobs);
    bridge.loader().setAnimationLibrary(&animation);

    foh::AppConfig appConfig;
    appConfig.jobs = &jobs;
    foh::App app(appConfig);
    foh::RenderExtractPlugin extract;
    app.addPlugin(extract);
    foh::AnimationPlugin animationPlugin;
    app.addPlugin(animationPlugin);
    app.insertResource(foh::AnimationLink{&animation});
    foh::RegisterPrefabRequests(app);
    app.world().resource<foh::SceneBridgeLink>()->bridge = &bridge;
    app.insertResource(foh::GameInput{});
    const foh::SceneId scene = renderer->createScene("World");
    app.world().resource<foh::RenderSceneLink>()->scene = renderer->scene(scene);

    const auto exeDir = foh::GetExecutableDirectory();
    foh::GameModuleHost game(app, exeDir / FOH_GAME_MODULE_FILE, exeDir / ".hotreload");
    if (!game.load()) {
        FOH_ERROR("Could not load the game module");
        return 1;
    }

    foh::ViewportDesc vd;
    vd.name = "Game";
    vd.width = w;
    vd.height = h;
    vd.scene = scene;
    vd.flags = foh::ViewportFlags::Sky;
    const foh::ViewportId viewport = renderer->createViewport(vd);

    // Dev only: rebuild the game module / shaders when their sources are saved.
    std::optional<foh::DevBuilder> gameBuilder, shaderBuilder;
    if (const auto cfg = foh::DefaultDevBuilderConfig(); cfg.isValid()) gameBuilder.emplace(cfg);
    if (const auto cfg = foh::DefaultShaderBuilderConfig(); cfg.isValid()) shaderBuilder.emplace(cfg);

    struct PresentContext {
        foh::Renderer* renderer;
        foh::ViewportId viewport;
    } present{renderer.get(), viewport};

    auto last = Clock::now();
    foh::u32 rendered = 0;
    window->setRelativeMouse(true); // the game owns the mouse; Escape quits
    while (window->pollEvents() && !window->input().isDown(foh::Key::Escape)) {
        const auto now = Clock::now();
        const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
        last = now;

        if (window->isMinimized()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        window->getSize(w, h);
        if (w != swapchain->width() || h != swapchain->height()) {
            if (!swapchain->resize(w, h)) break;
            renderer->setViewportSize(viewport, w, h);
        }

        if (gameBuilder) gameBuilder->poll();
        if (shaderBuilder) shaderBuilder->poll();
        game.pollReload();
        bridge.pump(); // loaded meshes, textures and clips
        if (auto* in = app.world().resource<foh::GameInput>()) {
            in->state = window->input();
            in->captured = window->hasFocus();
        }
        app.update(dt);

        renderer->beginFrame();
        renderer->renderViewports();
        renderer->renderWindow(
            *swapchain,
            [](foh::RenderGraph& graph, foh::RgTexture backbuffer, void* user) {
                auto* p = static_cast<PresentContext*>(user);
                p->renderer->addPresentPass(graph, backbuffer, p->viewport);
            },
            &present);
        const bool lastFrame = maxFrames && rendered + 1 >= maxFrames;
        if (lastFrame && screenshot) {
            std::vector<foh::u8> pixels;
            if (swapchain->readback(pixels)) {
                const bool ok = foh::WritePng(screenshot, pixels.data(), swapchain->width(), swapchain->height(), swapchain->width() * 4);
                FOH_INFO("screenshot {}: {}", ok ? "saved to" : "FAILED for", screenshot);
            }
        }
        swapchain->present();
        renderer->endFrame();

        if (maxFrames && ++rendered >= maxFrames) break;
    }

    renderer->device().waitIdle();
    const foh::u32 errors = renderer->device().errorCount();
    if (errors) FOH_ERROR("{} graphics API error(s) were reported during the run", errors);
    return errors ? 2 : 0;
}
