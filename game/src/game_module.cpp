#include <cstdint>

#include "gtabr/ecs.h"
#include "gtabr/game_module.h"
#include "gtabr/log.h"

// Smoke-test content for the hot reload loop: edit the message below while the
// runtime/editor is running, rebuild, and the new text appears within seconds.

namespace {

// Stateless on purpose (no statics): state belongs in the ECS world. The system
// declares what it needs (read access to Time) and the scheduler does the rest.
void HelloSystem(gtabr::Res<gtabr::Time> time) {
    const auto now = static_cast<std::uint64_t>(time->elapsed);
    const auto before = static_cast<std::uint64_t>(time->elapsed - time->delta);
    if (now != before) {
        GTABR_INFO("[game] Ola, Vidigal! V8 t={}s frame={}", now, time->frame);
    }
}

class HelloPlugin final : public gtabr::Plugin {
public:
    void build(gtabr::App& app) override {
        app.addSystem(gtabr::Stage::Update, &HelloSystem).name("HelloSystem");
    }
};

void Build(gtabr::App& app) {
    HelloPlugin plugin;
    app.addPlugin(plugin);
}

void Teardown(gtabr::App&) {}

} // namespace

GTABR_GAME_MODULE("gtabr_game", &Build, &Teardown)
