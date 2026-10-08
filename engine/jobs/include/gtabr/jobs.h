#pragma once

#include <atomic>
#include <memory>

#include "gtabr/platform.h"
#include "gtabr/types.h"

namespace gtabr {

// Upper bound for thread slots (caller thread + workers). Fixed so that
// per-thread tables (ECS stages, scratch buffers) can live on the stack.
inline constexpr u32 kMaxJobThreads = 64;

// Outstanding jobs of one fork-join group. Thread-safe; must outlive its jobs.
struct JobCounter {
    std::atomic<u32> pending{0};
};

// Minimal job system: a worker pool behind one shared queue.
//
// Thread-safety: submit() and wait() may be called from any thread, including
// from inside a running job (nested fork-join). wait() never blocks idle: the
// waiting thread executes queued jobs until its counter reaches zero, so nested
// waits cannot deadlock the pool.
//
// Thread slots: every thread that runs jobs has a stable slot, 0 for threads the
// pool does not own (the main thread) and 1..workerCount for pool workers. Use
// it to index per-thread data, e.g. one ECS stage per slot. Assumes ONE JobSystem
// is active per process at a time.
//
// The queue is a single mutex-protected deque: simple and correct, and fine for
// the tens-to-hundreds of coarse jobs per frame the ECS scheduler submits. Swap
// it for per-worker deques with stealing if profiling ever shows contention.
class GTABR_API JobSystem {
public:
    using JobFn = void (*)(void* arg);

    // workerCount == 0 runs every job inline on the thread that calls wait().
    explicit JobSystem(u32 workerCount);
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    // hardware threads - 1 (the caller is the extra one), clamped.
    static u32 defaultWorkerCount();
    static u32 currentThreadSlot();

    u32 workerCount() const { return m_workerCount; }
    // Slots in use: workers + the calling thread.
    u32 threadCount() const { return m_workerCount + 1; }

    void submit(JobFn fn, void* arg, JobCounter& counter);
    void wait(JobCounter& counter);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    u32 m_workerCount;
};

} // namespace gtabr
