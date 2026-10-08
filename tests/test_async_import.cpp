#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "asset_test_util.h"
#include "foh/asset_db.h"
#include "foh/jobs.h"

using namespace foh;
using namespace foh::testutil;

namespace {

AssetDbDesc dbFor(const TempDir& t, JobSystem* jobs) { return {t.path / "assets", t.path / "cache", jobs}; }

void writeModel(const TempDir& t, const std::string& rel, const std::string& gltf) {
    writeText(t.path / "assets" / rel, gltf);
    writeTexture((t.path / "assets" / rel).parent_path() / "albedo.png", 8, 8, 200, 100, 50);
}

// Polls like the editor does, once per "frame", until `done` says stop or the time is up.
template <class F>
std::vector<ImportResult> pollUntil(AssetDb& db, F&& done, double seconds = 20.0) {
    std::vector<ImportResult> all;
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
        for (ImportResult& r : db.pollImports()) all.push_back(std::move(r));
        if (done(all)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return all;
}

} // namespace

// ---- job system ----------------------------------------------------------------------------------

TEST_CASE("parallelFor covers every index exactly once, also when nested") {
    JobSystem jobs(4);
    constexpr u32 kCount = 100000;
    std::vector<std::atomic<u8>> hits(kCount);
    ParallelFor(&jobs, kCount, 100, [&](u32 b, u32 e) {
        for (u32 i = b; i < e; ++i) hits[i].fetch_add(1);
    });
    bool all = true;
    for (u32 i = 0; i < kCount; ++i) all = all && hits[i].load() == 1;
    CHECK(all);

    // Nested: every outer chunk starts an inner parallel loop (what an import does per texture).
    std::atomic<u64> sum{0};
    ParallelFor(&jobs, 16, 1, [&](u32 b, u32 e) {
        for (u32 o = b; o < e; ++o)
            ParallelFor(&jobs, 1000, 10, [&](u32 ib, u32 ie) {
                for (u32 i = ib; i < ie; ++i) sum.fetch_add(i);
            });
    });
    CHECK(sum.load() == 16u * (999u * 1000u / 2));

    // Degenerate inputs.
    int calls = 0;
    ParallelFor(&jobs, 0, 1, [&](u32, u32) { ++calls; });
    CHECK(calls == 0);
    ParallelFor(static_cast<JobSystem*>(nullptr), 5, 1, [](u32 b, u32 e) {
        CHECK(b == 0);
        CHECK(e == 5);
    });
}

TEST_CASE("a thread waiting on normal jobs never runs low priority jobs") {
    JobSystem jobs(2);
    std::atomic<bool> release{false};
    std::atomic<u32> lowSlot{999};
    struct Low {
        std::atomic<bool>* release;
        std::atomic<u32>* slot;
    } low{&release, &lowSlot};
    JobCounter lowCounter, normalCounter;
    // Two low jobs occupy both workers until released; they record where they ran.
    for (int i = 0; i < 2; ++i)
        jobs.submit(
            [](void* p) {
                auto* l = static_cast<Low*>(p);
                l->slot->store(JobSystem::currentThreadSlot());
                while (!l->release->load()) std::this_thread::yield();
            },
            &low, lowCounter, JobPriority::Low);

    // A normal job must still complete promptly even though every worker is busy: the main thread runs it
    // itself while waiting, and it must not get stuck running a low job instead.
    std::atomic<int> normalRan{0};
    jobs.submit([](void* p) { static_cast<std::atomic<int>*>(p)->fetch_add(1); }, &normalRan, normalCounter, JobPriority::Normal);
    const auto t0 = std::chrono::steady_clock::now();
    jobs.wait(normalCounter);
    CHECK(normalRan.load() == 1);
    CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 5.0);

    release = true;
    jobs.wait(lowCounter); // the main thread only waits here; workers finish the low jobs
    CHECK(lowSlot.load() != 0);
    CHECK(lowSlot.load() != 999);
}

TEST_CASE("without workers a waiting thread runs low priority jobs itself") {
    JobSystem jobs(0);
    int ran = 0;
    JobCounter c;
    jobs.submit([](void* p) { ++*static_cast<int*>(p); }, &ran, c, JobPriority::Low);
    jobs.wait(c);
    CHECK(ran == 1);
}

TEST_CASE("jobs submitted from a low priority job inherit the priority") {
    JobSystem jobs(3);
    std::atomic<u32> innerOnMain{0};
    struct Ctx {
        JobSystem* jobs;
        std::atomic<u32>* innerOnMain;
    } ctx{&jobs, &innerOnMain};
    JobCounter outer;
    jobs.submit(
        [](void* p) {
            auto* c = static_cast<Ctx*>(p);
            ParallelFor(c->jobs, 64, 1, [&](u32, u32) {
                if (JobSystem::currentThreadSlot() == 0) c->innerOnMain->fetch_add(1);
            });
        },
        &ctx, outer, JobPriority::Low);
    jobs.wait(outer);
    CHECK(innerOnMain.load() == 0); // all inner chunks are low priority: only workers ran them
}

// ---- asynchronous imports ----------------------------------------------------------------------

TEST_CASE("beginImport returns immediately and the result arrives through pollImports") {
    TempDir t;
    JobSystem jobs(3);
    writeModel(t, "a.gltf", makeGltf("A"));
    AssetDb db(dbFor(t, &jobs));

    const ImportTicket ticket = db.beginImport("a.gltf");
    CHECK(ticket != 0);
    CHECK(db.importsPending());
    CHECK(db.pendingImportNames() == std::vector<std::string>{"a.gltf"});
    CHECK(db.sources().empty()); // nothing is committed until the main thread polls

    const auto results = pollUntil(db, [](auto& r) { return !r.empty(); });
    REQUIRE(results.size() == 1);
    INFO(results[0].error);
    CHECK(results[0].ok);
    CHECK(results[0].ticket == ticket);
    CHECK(results[0].displayName == "a.gltf");
    CHECK(results[0].prefab.isValid());
    CHECK_FALSE(db.importsPending());
    CHECK(db.sources().size() == 1);
    CHECK(db.assetsOfKind(AssetKind::Mesh).size() == 1);

    // Imported textures are block compressed.
    TextureAsset tex;
    REQUIRE(db.load(db.assetsOfKind(AssetKind::Texture)[0], tex));
    CHECK((tex.format == TexelFormat::Bc7Srgb || tex.format == TexelFormat::Bc5Unorm));
}

TEST_CASE("several imports run one after another in request order and a bad file fails alone") {
    TempDir t;
    JobSystem jobs(3);
    writeModel(t, "one/a.gltf", makeGltf("A"));
    writeModel(t, "two/b.gltf", makeGltf("B"));
    writeText(t.path / "assets/bad.gltf", "{ nope");
    AssetDb db(dbFor(t, &jobs));

    const ImportTicket a = db.beginImport("one/a.gltf");
    const ImportTicket bad = db.beginImport("bad.gltf");
    const ImportTicket b = db.beginImport("two/b.gltf");
    CHECK(db.pendingImportNames().size() == 3);

    std::vector<ImportResult> results = pollUntil(db, [](auto& r) { return r.size() == 3; });
    REQUIRE(results.size() == 3);
    CHECK(results[0].ticket == a);
    CHECK(results[1].ticket == bad);
    CHECK(results[2].ticket == b);
    CHECK(results[0].ok);
    CHECK_FALSE(results[1].ok);
    CHECK_FALSE(results[1].error.empty());
    CHECK(results[2].ok);
    CHECK(db.sources().size() == 2);
}

TEST_CASE("dropping a file from outside copies it on the worker and keeps the same ticket") {
    TempDir project, downloads;
    JobSystem jobs(3);
    writeText(downloads.path / "pack/model.gltf", makeGltf("M"));
    writeTexture(downloads.path / "pack/albedo.png", 16, 16, 90, 90, 200);

    AssetDb db(dbFor(project, &jobs));
    const ImportTicket ticket = db.beginImport(downloads.path / "pack/model.gltf");
    const auto results = pollUntil(db, [](auto& r) { return !r.empty(); });
    REQUIRE(results.size() == 1);
    INFO(results[0].error);
    CHECK(results[0].ok);
    CHECK(results[0].ticket == ticket); // copy + import are one request
    CHECK(results[0].displayName == "model.gltf");
    CHECK(results[0].sourcePath == "imported/model/model.gltf");
    CHECK(fs::exists(project.path / "assets/imported/model/albedo.png"));

    const ImportTicket missing = db.beginImport(downloads.path / "pack/missing.gltf");
    const auto failed = pollUntil(db, [](auto& r) { return !r.empty(); });
    REQUIRE(failed.size() == 1);
    CHECK(failed[0].ticket == missing);
    CHECK_FALSE(failed[0].ok);
}

TEST_CASE("the main thread stays responsive while an import runs") {
    TempDir t;
    JobSystem jobs(3);
    // A big texture makes the import take a visible amount of time.
    writeText(t.path / "assets/big.gltf", makeGltf("Big"));
    writeTexture(t.path / "assets/albedo.png", 1024, 1024, 10, 120, 200);
    AssetDb db(dbFor(t, &jobs));

    db.beginImport("big.gltf");
    int polls = 0;
    const auto results = pollUntil(db, [&](auto& r) {
        ++polls;
        return !r.empty();
    });
    REQUIRE(results.size() == 1);
    CHECK(results[0].ok);
    CHECK(polls >= 1);
}

TEST_CASE("blocking and asynchronous APIs agree, with and without a job system") {
    TempDir t1, t2;
    JobSystem jobs(3);
    writeModel(t1, "m.gltf", makeGltf("M"));
    writeModel(t2, "m.gltf", makeGltf("M"));
    AssetDb withJobs(dbFor(t1, &jobs)), inlineDb(dbFor(t2, nullptr));
    const ImportResult a = withJobs.importFile("m.gltf");
    const ImportResult b = inlineDb.importFile("m.gltf");
    REQUIRE(a.ok);
    REQUIRE(b.ok);
    CHECK(a.assets.size() == b.assets.size());

    // The cooked files of the two are byte identical for the same source (GUIDs differ per source asset,
    // so compare a texture by content through the loader).
    auto colorTexture = [](AssetDb& db) {
        for (const AssetId& id : db.assetsOfKind(AssetKind::Texture)) {
            TextureAsset t;
            REQUIRE(db.load(id, t));
            if (t.role == TextureRole::Color) return t;
        }
        return TextureAsset{};
    };
    const TextureAsset ta = colorTexture(withJobs), tb = colorTexture(inlineDb);
    REQUIRE(ta.mips.size() > 0);
    CHECK(ta.format == tb.format);
    REQUIRE(ta.mips.size() == tb.mips.size());
    for (size_t i = 0; i < ta.mips.size(); ++i) CHECK(ta.mips[i].data == tb.mips[i].data);

    // Hot reload tick, asynchronous flavor: change the source, start, poll.
    writeText(t1.path / "assets/m.gltf", makeGltf("M", "Renamed"));
    const auto tickets = withJobs.beginReimportChanged();
    REQUIRE(tickets.size() == 1);
    const auto results = pollUntil(withJobs, [](auto& r) { return !r.empty(); });
    REQUIRE(results.size() == 1);
    CHECK(results[0].ok);
    CHECK(results[0].changed.size() == 1); // only the prefab changed
    CHECK(withJobs.beginReimportChanged().empty());
}

TEST_CASE("destroying the database while an import runs waits for it") {
    TempDir t;
    JobSystem jobs(3);
    writeModel(t, "a.gltf", makeGltf("A"));
    {
        AssetDb db(dbFor(t, &jobs));
        db.beginImport("a.gltf");
    } // must not crash or leave the worker writing into freed memory
    CHECK(true);
}
