#pragma once

#include <memory>
#include <utility>

#include <nvrhi/nvrhi.h>

#include "foh/platform.h"
#include "foh/types.h"

namespace foh {

// Graphics pipelines keyed by (caller key, framebuffer formats). The caller key
// must change whenever the pipeline description does, in particular it should
// include ShaderRef::uid of every shader so hot reloaded shaders rebuild.
//
// Thread-safety: fully thread-safe; pass bodies on different recording threads
// share one cache. Pipeline creation happens outside the lock, so two threads
// racing on the same new key may both build it (one result is discarded).
class FOH_API PipelineCache {
public:
    explicit PipelineCache(nvrhi::IDevice* device);
    ~PipelineCache();
    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;

    // `makeDesc` is only called on a miss: () -> nvrhi::GraphicsPipelineDesc.
    template <class MakeDesc>
    nvrhi::IGraphicsPipeline* graphics(u64 key, nvrhi::IFramebuffer* framebuffer, MakeDesc&& makeDesc) {
        const u64 full = combine(key, framebuffer);
        if (auto* hit = find(full)) return hit;
        const nvrhi::GraphicsPipelineDesc desc = makeDesc();
        return insert(full, m_device->createGraphicsPipeline(desc, framebuffer->getFramebufferInfo()));
    }

    // Drops pipelines unused for `maxAgeFrames` frames. Main thread, between frames.
    void collect(u64 frame, u64 maxAgeFrames = 300);
    size_t size() const;

private:
    u64 combine(u64 key, nvrhi::IFramebuffer* framebuffer) const;
    nvrhi::IGraphicsPipeline* find(u64 fullKey);
    nvrhi::IGraphicsPipeline* insert(u64 fullKey, nvrhi::GraphicsPipelineHandle pipeline);

    nvrhi::IDevice* m_device;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Convenience for building cache keys.
constexpr u64 HashCombine(u64 seed, u64 value) {
    return seed ^ (value + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2));
}

} // namespace foh
