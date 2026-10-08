#include "foh/ecs.h"
#include "foh/game_module.h"
#include "shared.h"

// A minimal game-style module for the automated hot reload test: it uses every
// kind of system parameter, so the test exercises templates instantiated inside
// a DLL that gets unloaded and loaded again.

namespace {

using namespace foh;
using namespace reload_test;

void Tick(ResMut<Ticks> ticks, Query<Tracked&> tracked, EventWriter<Ping> ping, Commands cmd) {
    ++ticks->value;
    tracked.each([](Tracked& t) { ++t.seen; });
    ping.send({ticks->value});
    if (ticks->value == 1) cmd.spawn(Tracked{});
}

void OnTracked(Trigger<Tracked>, ResMut<Observed> observed) { ++observed->count; }

void Build(App& app) {
    app.registerComponent<Tracked>().registerEvent<Ping>();
    app.addSystem(Stage::Update, &Tick).name("Tick");
    app.onSet(&OnTracked);
}

void Teardown(App&) {}

} // namespace

FOH_GAME_MODULE("test_module", &Build, &Teardown)
