#include <doctest/doctest.h>

#include <atomic>
#include <set>
#include <thread>
#include <vector>

#include "foh/jobs.h"

using namespace foh;

namespace {
void addOne(void* arg) { static_cast<std::atomic<int>*>(arg)->fetch_add(1); }
} // namespace

TEST_CASE("JobSystem runs every submitted job exactly once") {
    for (u32 workers : {0u, 1u, 4u}) {
        CAPTURE(workers);
        JobSystem jobs(workers);
        CHECK(jobs.threadCount() == workers + 1);
        std::atomic<int> sum{0};
        JobCounter counter;
        for (int i = 0; i < 1000; ++i) jobs.submit(&addOne, &sum, counter);
        jobs.wait(counter);
        CHECK(sum.load() == 1000);
        CHECK(counter.pending.load() == 0);
    }
}

namespace {
struct Nested {
    JobSystem* jobs;
    std::atomic<int>* sum;
};
void spawnChildren(void* arg) {
    auto* n = static_cast<Nested*>(arg);
    JobCounter inner;
    for (int i = 0; i < 8; ++i) n->jobs->submit(&addOne, n->sum, inner);
    n->jobs->wait(inner); // nested wait: must help instead of blocking a worker
}
} // namespace

TEST_CASE("JobSystem nested fork-join does not deadlock, even with one worker") {
    for (u32 workers : {0u, 1u, 2u}) {
        CAPTURE(workers);
        JobSystem jobs(workers);
        std::atomic<int> sum{0};
        Nested nested{&jobs, &sum};
        JobCounter outer;
        for (int i = 0; i < 16; ++i) jobs.submit(&spawnChildren, &nested, outer);
        jobs.wait(outer);
        CHECK(sum.load() == 16 * 8);
    }
}

namespace {
struct SlotLog {
    std::atomic<u32> mask{0};
};
void recordSlot(void* arg) {
    static_cast<SlotLog*>(arg)->mask.fetch_or(1u << JobSystem::currentThreadSlot());
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
}
} // namespace

TEST_CASE("JobSystem thread slots: main is 0, workers are 1..N") {
    CHECK(JobSystem::currentThreadSlot() == 0);
    JobSystem jobs(3);
    SlotLog log;
    JobCounter counter;
    for (int i = 0; i < 24; ++i) jobs.submit(&recordSlot, &log, counter);
    jobs.wait(counter);
    CHECK((log.mask.load() & ~0xFu) == 0); // only slots 0..3 exist
    CHECK(log.mask.load() != 0);
}
