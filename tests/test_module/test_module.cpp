#include "gtabr/ecs.h"
#include "gtabr/game_module.h"
#include "shared.h"

// A minimal game-style module for the automated hot reload test: it uses every
// kind of system parameter, so the test exercises templates instantiated inside
// a DLL that gets unloaded and loaded again.

namespace {

using namespace gtabr;
using namespace reload_test;

void Tick(ResMut<Ticks> ticks, Query<Tracked&> tracked, EventWriter<Ping> ping, Commands cmd) {
    ++ticks->value;
    tracked.each([](Tracked& t) { ++t.seen; });
    ping.send({ticks->value});
    if (ticks->value == 1) cmd.spawn(Tracked{});
}

void Build(App& app) {
    app.registerComponent<Tracked>().registerEvent<Ping>();
    app.addSystem(Stage::Update, &Tick).name("Tick");
}

void Teardown(App&) {}

} // namespace

GTABR_GAME_MODULE("test_module", &Build, &Teardown)
