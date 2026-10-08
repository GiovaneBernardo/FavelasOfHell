#include <chrono>
#include <optional>
#include <thread>

#include "gtabr/dev_build_defaults.h"
#include "gtabr/ecs.h"
#include "gtabr/file_system.h"
#include "gtabr/game_module_host.h"
#include "gtabr/log.h"
#include "gtabr/window.h"

int main() {
    using Clock = std::chrono::steady_clock;

    auto window = gtabr::Window::create({.title = "gtaBR"});
    if (!window) return 1;

    gtabr::App app;
    const auto exeDir = gtabr::GetExecutableDirectory();
    gtabr::GameModuleHost game(app, exeDir / GTABR_GAME_MODULE_FILE, exeDir / ".hotreload");
    if (!game.load()) {
        GTABR_ERROR("Could not load the game module");
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

        // No renderer yet: avoid spinning a core.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return 0;
}
