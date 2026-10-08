#include <cstdint>

#include "gtabr/ecs.h"
#include "gtabr/game_module.h"
#include "gtabr/log.h"

// Smoke-test content for the hot reload loop: edit the message below while the
// runtime/editor is running, rebuild, and the new text appears within seconds.

namespace {

// Stateless on purpose (no statics): state belongs in the ECS world.
void HelloSystem(gtabr::SystemContext& ctx) {
    const auto now = static_cast<std::uint64_t>(ctx.time.elapsed);
    const auto before = static_cast<std::uint64_t>(ctx.time.elapsed - ctx.time.delta);
    if (now != before) {
        GTABR_INFO("[game] Ola, Vidigal! V8 t={}s entities={}", now, ctx.world.entityCount());
    }
}

class HelloPlugin final : public gtabr::Plugin {
public:
    void build(gtabr::App& app) override {
        app.addSystem(gtabr::Stage::Update, &HelloSystem, "HelloSystem");
    }
};

void Build(gtabr::App& app) {
    HelloPlugin plugin;
    app.addPlugin(plugin);
}

void Teardown(gtabr::App&) {}

} // namespace

GTABR_GAME_MODULE("gtabr_game", &Build, &Teardown)
