#include "foh/jobs.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "foh/assert.h"
#include "foh/thread_priority.h"

namespace foh {

namespace {
thread_local u32 t_threadSlot = 0;
// True while this thread runs a low priority job: it may then also help with low priority work
// when it waits, and the jobs it submits are low priority too.
thread_local bool t_runningLow = false;
} // namespace

struct JobSystem::Impl {
    struct Job {
        JobFn fn;
        void* arg;
        JobCounter* counter;
        bool low;
        bool counted = false; // popped by an idle worker: occupies one of the low priority slots until it finishes
    };

    std::mutex mutex;
    // One condition variable for workers and waiters: every predicate includes
    // "queue not empty", so whoever wakes can take the new job.
    std::condition_variable cv;
    std::deque<Job> queue;    // normal priority
    std::deque<Job> lowQueue; // low priority: workers only, after the normal queue is empty
    bool runLowWhileWaiting = false; // no workers: whoever waits has to run everything
    // Idle workers start low priority jobs only while fewer than lowLimit of them run: background work (imports) must
    // never take every core, or the game threads get starved by the OS scheduler and frames stutter.
    std::atomic<u32> lowRunning{0};
    std::atomic<u32> lowLimit{1};
    bool lowAllowed() const { return lowRunning.load(std::memory_order_acquire) < lowLimit.load(std::memory_order_relaxed); }
    std::vector<std::thread> threads;
    bool stopping = false;

    bool pop(Job& job, bool takeLow) {
        if (!queue.empty()) {
            job = queue.front();
            queue.pop_front();
            return true;
        }
        if (takeLow && !lowQueue.empty()) {
            job = lowQueue.front();
            lowQueue.pop_front();
            return true;
        }
        return false;
    }
    bool hasWork(bool takeLow) const { return !queue.empty() || (takeLow && !lowQueue.empty()); }

    void run(const Job& job) {
        const bool previous = t_runningLow;
        t_runningLow = job.low;
        // The OS must see background work as background too (see SetCurrentThreadBackground). Only the transition
        // from a normal context into a low job changes the priority; a low job nested in a low job already has it.
        const bool changePriority = job.low && !previous;
        if (changePriority) SetCurrentThreadBackground(true);
        job.fn(job.arg);
        if (changePriority) SetCurrentThreadBackground(false);
        t_runningLow = previous;
        if (job.counted) {
            lowRunning.fetch_sub(1, std::memory_order_acq_rel);
            { std::lock_guard lock(mutex); } // a worker may have been waiting for a free low priority slot
            cv.notify_all();
        }
        if (job.counter->pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            // Taking the lock orders this notify after a waiter's predicate check,
            // so the wakeup cannot be lost.
            { std::lock_guard lock(mutex); }
            cv.notify_all();
        }
    }

    void workerLoop(u32 slot) {
        t_threadSlot = slot;
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return stopping || hasWork(lowAllowed()); });
                if (!pop(job, lowAllowed())) return; // stopping and drained
                if (job.low) {
                    job.counted = true;
                    lowRunning.fetch_add(1, std::memory_order_acq_rel);
                }
            }
            run(job);
        }
    }
};

u32 JobSystem::defaultWorkerCount() {
    const u32 hw = std::thread::hardware_concurrency();
    return hw > 1 ? std::min(hw - 1, kMaxJobThreads - 1) : 0;
}

u32 JobSystem::currentThreadSlot() { return t_threadSlot; }

JobSystem::JobSystem(u32 workerCount)
    : m_impl(std::make_unique<Impl>()), m_workerCount(std::min(workerCount, kMaxJobThreads - 1)) {
    m_impl->runLowWhileWaiting = m_workerCount == 0;
    m_impl->lowLimit = std::max(1u, m_workerCount / 4);
    m_impl->threads.reserve(m_workerCount);
    for (u32 i = 0; i < m_workerCount; ++i) {
        m_impl->threads.emplace_back([impl = m_impl.get(), slot = i + 1] { impl->workerLoop(slot); });
    }
}

JobSystem::~JobSystem() {
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->stopping = true;
    }
    m_impl->cv.notify_all();
    for (std::thread& t : m_impl->threads) t.join();
}

void JobSystem::setLowPriorityLimit(u32 threads) { m_impl->lowLimit = std::max(1u, threads); m_impl->cv.notify_all(); }

void JobSystem::submit(JobFn fn, void* arg, JobCounter& counter) {
    submit(fn, arg, counter, t_runningLow ? JobPriority::Low : JobPriority::Normal);
}

void JobSystem::submit(JobFn fn, void* arg, JobCounter& counter, JobPriority priority) {
    FOH_ASSERT(fn != nullptr, "null job function");
    // Count before enqueueing so a fast job cannot drive the counter to zero early.
    counter.pending.fetch_add(1, std::memory_order_relaxed);
    const bool low = priority == JobPriority::Low;
    {
        std::lock_guard lock(m_impl->mutex);
        (low ? m_impl->lowQueue : m_impl->queue).push_back({fn, arg, &counter, low});
    }
    // Waking one thread is enough for one job, but a waiter that may not run it would swallow the wakeup
    // for low jobs: wake everyone for those (they are rare and coarse).
    if (low) m_impl->cv.notify_all();
    else m_impl->cv.notify_one();
}

void JobSystem::wait(JobCounter& counter) {
    const bool takeLow = t_runningLow || m_impl->runLowWhileWaiting;
    for (;;) {
        Impl::Job job;
        {
            std::unique_lock lock(m_impl->mutex);
            m_impl->cv.wait(lock, [&] {
                return counter.pending.load(std::memory_order_acquire) == 0 || m_impl->hasWork(takeLow);
            });
            if (counter.pending.load(std::memory_order_acquire) == 0) return;
            if (!m_impl->pop(job, takeLow)) continue;
        }
        m_impl->run(job);
    }
}

void JobSystem::parallelFor(u32 count, u32 grain, RangeFn fn, void* arg) {
    if (count == 0) return;
    grain = std::max(grain, 1u);
    const u32 maxChunks = std::max(1u, m_workerCount + 1) * 4; // a few chunks per thread balances uneven items
    const u32 chunks = std::min(maxChunks, (count + grain - 1) / grain);
    if (chunks <= 1 || m_workerCount == 0) {
        fn(0, count, arg);
        return;
    }
    struct Chunk {
        RangeFn fn;
        void* arg;
        u32 begin, end;
    };
    std::vector<Chunk> work(chunks);
    JobCounter counter;
    for (u32 i = 0; i < chunks; ++i) {
        work[i] = {fn, arg, static_cast<u32>(static_cast<u64>(count) * i / chunks), static_cast<u32>(static_cast<u64>(count) * (i + 1) / chunks)};
    }
    // The last chunk runs on this thread; the rest go to the pool.
    for (u32 i = 0; i + 1 < chunks; ++i)
        submit([](void* p) { auto* c = static_cast<Chunk*>(p); c->fn(c->begin, c->end, c->arg); }, &work[i], counter);
    fn(work[chunks - 1].begin, work[chunks - 1].end, arg);
    wait(counter);
}

} // namespace foh
