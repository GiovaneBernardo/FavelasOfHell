#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <thread>

#include "gtabr/ecs.h"
#include "gtabr/game_module_host.h"
#include "test_module/shared.h"

using namespace gtabr;
using namespace reload_test;
namespace fs = std::filesystem;

// Drives the real host with a real DLL: load, run, "rebuild" (touch the files),
// reload, run, unload. The ECS contract under test: the module's systems are
// removed before its code is unloaded, and the data in the world is untouched.
TEST_CASE("Game module hot reload keeps world state and swaps systems") {
    const fs::path built = GTABR_TEST_MODULE_PATH;
    const fs::path dir = fs::temp_directory_path() / "gtabr_hot_reload_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const fs::path dll = dir / built.filename();
    fs::copy_file(built, dll);
    fs::path pdb = built;
    pdb.replace_extension(".pdb");
    if (fs::exists(pdb)) fs::copy_file(pdb, dir / pdb.filename());

    App app({.workerThreads = 2});
    app.insertResource(Ticks{});

    {
        GameModuleHost host(app, dll, dir / "shadow");
        REQUIRE(host.load());
        CHECK(host.generation() == 1);
        CHECK(app.systemCount() == 1);

        app.update(0.016f);
        app.update(0.016f);
        CHECK(app.world().resource<Ticks>()->value == 2);

        // Simulate a rebuild: the host reloads when the files change and settle.
        const auto later = fs::file_time_type::clock::now() + std::chrono::seconds(2);
        fs::last_write_time(dll, later);
        if (fs::exists(dir / pdb.filename())) fs::last_write_time(dir / pdb.filename(), later);

        bool reloaded = false;
        for (int i = 0; i < 200 && !reloaded; ++i) {
            reloaded = host.pollReload();
            if (!reloaded) std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        REQUIRE(reloaded);
        CHECK(host.generation() == 2);
        CHECK(app.systemCount() == 1); // old module's system gone, new one registered

        app.update(0.016f);
        app.update(0.016f);
        CHECK(app.world().resource<Ticks>()->value == 4);

        // The entity spawned by generation 1 was updated by both generations.
        int seen = -1;
        OwnedQuery<const Tracked&> q(app.world());
        q.each([&](const Tracked& t) { seen = t.seen; });
        CHECK(seen == 3);
    } // host destroyed: unload() removes the module's systems before the DLL goes away

    CHECK(app.systemCount() == 0);
    app.update(0.016f);
    CHECK(app.world().resource<Ticks>()->value == 4);
    fs::remove_all(dir, ec);
}
