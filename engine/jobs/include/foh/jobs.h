#pragma once

#include <atomic>
#include <memory>
#include <type_traits>

#include "foh/platform.h"
#include "foh/types.h"

namespace foh {

// Upper bound for thread slots (caller thread + workers). Fixed so that
// per-thread tables (ECS stages, scratch buffers) can live on the stack.
inline constexpr u32 kMaxJobThreads = 64;

// Outstanding jobs of one fork-join group. Thread-safe; must outlive its jobs.
struct JobCounter {
    std::atomic<u32> pending{0};
};

// Low priority jobs (imports, asset decoding) only run on pool workers that have no normal job
// to take, and the main thread never executes them while it waits: a frame that waits for its own
// jobs can therefore never get stuck behind a multi-second texture compression. Jobs submitted
// from inside a low priority job (nested fork-join) inherit the priority automatically.
enum class JobPriority : u8 { Normal, Low };

// Minimal job system: a worker pool behind two shared queues.
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
class FOH_API JobSystem {
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

    // `priority` defaults to the priority of the job being run by the calling thread (Normal elsewhere).
    // At most this many pool workers run low priority jobs at once (default: a quarter of them, at least one). A job
    // that waits for its own nested low priority jobs helps with them on top of that.
    void setLowPriorityLimit(u32 threads);

    void submit(JobFn fn, void* arg, JobCounter& counter);
    void submit(JobFn fn, void* arg, JobCounter& counter, JobPriority priority);
    void wait(JobCounter& counter);

    // Splits [0, count) into chunks of at least `grain` items and runs fn(begin, end, arg) on the pool;
    // returns when all are done. Runs inline for small counts or without workers. The calling thread
    // takes part, so this is safe from inside a job.
    using RangeFn = void (*)(u32 begin, u32 end, void* arg);
    void parallelFor(u32 count, u32 grain, RangeFn fn, void* arg);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    u32 m_workerCount;
};

// Convenience wrapper: body(begin, end) over [0, count). Not for hot per-item lambdas that capture a lot:
// the callable is invoked once per chunk, not once per item.
template <class F>
void ParallelFor(JobSystem* jobs, u32 count, u32 grain, F&& body) {
    if (!jobs || jobs->workerCount() == 0 || count <= grain) {
        if (count) body(0u, count);
        return;
    }
    jobs->parallelFor(count, grain,
                      [](u32 begin, u32 end, void* arg) { (*static_cast<std::remove_reference_t<F>*>(arg))(begin, end); },
                      &body);
}

} // namespace foh
