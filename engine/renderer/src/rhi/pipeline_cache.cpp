#include "foh/rhi/pipeline_cache.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace foh {

struct PipelineCache::Impl {
    struct Entry {
        nvrhi::GraphicsPipelineHandle pipeline;
        std::atomic<u64> lastUsed{0};
    };
    struct ComputeEntry {
        nvrhi::ComputePipelineHandle pipeline;
        std::atomic<u64> lastUsed{0};
    };
    mutable std::mutex mutex;
    std::unordered_map<u64, std::unique_ptr<Entry>> entries;
    std::unordered_map<u64, std::unique_ptr<ComputeEntry>> computeEntries;
    std::atomic<u64> frame{0};
};

PipelineCache::PipelineCache(nvrhi::IDevice* device) : m_device(device), m_impl(std::make_unique<Impl>()) {}
PipelineCache::~PipelineCache() = default;

u64 PipelineCache::combine(u64 key, nvrhi::IFramebuffer* framebuffer) const {
    const std::hash<nvrhi::FramebufferInfo> hasher;
    return HashCombine(key, hasher(framebuffer->getFramebufferInfo()));
}

nvrhi::IGraphicsPipeline* PipelineCache::find(u64 fullKey) {
    std::lock_guard lock(m_impl->mutex);
    auto it = m_impl->entries.find(fullKey);
    if (it == m_impl->entries.end()) return nullptr;
    it->second->lastUsed.store(m_impl->frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return it->second->pipeline.Get();
}

nvrhi::IGraphicsPipeline* PipelineCache::insert(u64 fullKey, nvrhi::GraphicsPipelineHandle pipeline) {
    std::lock_guard lock(m_impl->mutex);
    auto& slot = m_impl->entries[fullKey];
    if (!slot) {
        slot = std::make_unique<Impl::Entry>();
        slot->pipeline = std::move(pipeline); // may be null on failure: callers must check
    }
    slot->lastUsed.store(m_impl->frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return slot->pipeline.Get();
}

nvrhi::IComputePipeline* PipelineCache::findCompute(u64 key) {
    std::lock_guard lock(m_impl->mutex);
    auto it = m_impl->computeEntries.find(key);
    if (it == m_impl->computeEntries.end()) return nullptr;
    it->second->lastUsed.store(m_impl->frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return it->second->pipeline.Get();
}

nvrhi::IComputePipeline* PipelineCache::insertCompute(u64 key, nvrhi::ComputePipelineHandle pipeline) {
    std::lock_guard lock(m_impl->mutex);
    auto& slot = m_impl->computeEntries[key];
    if (!slot) {
        slot = std::make_unique<Impl::ComputeEntry>();
        slot->pipeline = std::move(pipeline); // may be null on failure: callers must check
    }
    slot->lastUsed.store(m_impl->frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return slot->pipeline.Get();
}

void PipelineCache::collect(u64 frame, u64 maxAgeFrames) {
    m_impl->frame.store(frame, std::memory_order_relaxed);
    if (frame % 60 != 0) return;
    std::lock_guard lock(m_impl->mutex);
    std::erase_if(m_impl->entries, [&](const auto& kv) {
        return kv.second->lastUsed.load(std::memory_order_relaxed) + maxAgeFrames < frame;
    });
    std::erase_if(m_impl->computeEntries, [&](const auto& kv) {
        return kv.second->lastUsed.load(std::memory_order_relaxed) + maxAgeFrames < frame;
    });
}

size_t PipelineCache::size() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->entries.size() + m_impl->computeEntries.size();
}

} // namespace foh
