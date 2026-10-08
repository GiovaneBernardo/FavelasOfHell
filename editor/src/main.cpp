#include <chrono>
#include <optional>
#include <thread>

#include <imgui.h>

#include "gtabr/dev_build_defaults.h"
#include "gtabr/ecs.h"
#include "gtabr/file_system.h"
#include "gtabr/game_module_host.h"
#include "gtabr/log.h"
#include "gtabr/window.h"

// Editor skeleton: window + ImGui context + hot-reloading game module. The
// ImGui backends/renderer integration arrive with the renderer.
int main() {
    using Clock = std::chrono::steady_clock;

    auto window = gtabr::Window::create({.title = "gtaBR Editor", .width = 1920, .height = 1080});
    if (!window) return 1;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    gtabr::App app;
    const auto exeDir = gtabr::GetExecutableDirectory();
    gtabr::GameModuleHost game(app, exeDir / GTABR_GAME_MODULE_FILE, exeDir / ".hotreload");
    if (!game.load()) {
        GTABR_ERROR("Could not load the game module");
        ImGui::DestroyContext();
        return 1;
    }

    // Dev only: rebuild the game module when game/ sources are saved.
    std::optional<gtabr::DevBuilder> devBuilder;
    if (const auto cfg = gtabr::DefaultDevBuilderConfig(); cfg.isValid()) devBuilder.emplace(cfg);

    auto last = Clock::now();
    while (window->pollEvents() && !window->isKeyDown(gtabr::Key::Escape)) {
        const auto now = Clock::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;

        if (devBuilder) devBuilder->poll();
        game.pollReload();
        app.update(dt);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    ImGui::DestroyContext();
    return 0;
}
