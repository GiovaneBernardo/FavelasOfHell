#include "gtabr/jobs.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "gtabr/assert.h"

namespace gtabr {

namespace {
thread_local u32 t_threadSlot = 0;
} // namespace

struct JobSystem::Impl {
    struct Job {
        JobFn fn;
        void* arg;
        JobCounter* counter;
    };

    std::mutex mutex;
    // One condition variable for workers and waiters: every predicate includes
    // "queue not empty", so whoever wakes can take the new job.
    std::condition_variable cv;
    std::deque<Job> queue;
    std::vector<std::thread> threads;
    bool stopping = false;

    void run(const Job& job) {
        job.fn(job.arg);
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
                cv.wait(lock, [&] { return stopping || !queue.empty(); });
                if (queue.empty()) return; // stopping and drained
                job = queue.front();
                queue.pop_front();
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

void JobSystem::submit(JobFn fn, void* arg, JobCounter& counter) {
    GTABR_ASSERT(fn != nullptr, "null job function");
    // Count before enqueueing so a fast job cannot drive the counter to zero early.
    counter.pending.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->queue.push_back({fn, arg, &counter});
    }
    m_impl->cv.notify_one();
}

void JobSystem::wait(JobCounter& counter) {
    for (;;) {
        Impl::Job job;
        {
            std::unique_lock lock(m_impl->mutex);
            m_impl->cv.wait(lock, [&] {
                return counter.pending.load(std::memory_order_acquire) == 0 || !m_impl->queue.empty();
            });
            if (counter.pending.load(std::memory_order_acquire) == 0) return;
            job = m_impl->queue.front();
            m_impl->queue.pop_front();
        }
        m_impl->run(job);
    }
}

} // namespace gtabr
