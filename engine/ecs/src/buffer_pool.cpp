#include "foh/ecs/buffer.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "foh/assert.h"

namespace foh {

namespace {
constexpr u32 kChunkBits = 10;
constexpr u32 kChunkSize = 1u << kChunkBits;
constexpr u32 kMaxChunks = 4096; // 4M buffers
} // namespace

struct BufferPool::Impl {
    struct Slot {
        std::byte* data = nullptr;
        u32 count = 0;
        u32 capacity = 0;
        u32 elemSize = 0;
        u32 generation = 1; // never 0, so a handle is never 0
        bool live = false;
    };

    // Chunks are only ever added, never moved or freed while the pool lives, so
    // readers can look slots up without a lock while another thread creates buffers.
    std::atomic<Slot*> chunks[kMaxChunks];
    std::mutex mutex; // guards slot allocation and release
    std::vector<u32> freeList;
    u32 next = 0;
    std::atomic<size_t> live{0};

    Impl() {
        for (auto& c : chunks) c.store(nullptr, std::memory_order_relaxed);
    }
    ~Impl() {
        for (auto& c : chunks) {
            Slot* chunk = c.load(std::memory_order_relaxed);
            if (!chunk) continue;
            for (u32 i = 0; i < kChunkSize; ++i) std::free(chunk[i].data);
            delete[] chunk;
        }
    }

    Slot* find(u64 handle) const {
        const u32 index = static_cast<u32>(handle) - 1u;
        if (static_cast<u32>(handle) == 0 || (index >> kChunkBits) >= kMaxChunks) return nullptr;
        Slot* chunk = chunks[index >> kChunkBits].load(std::memory_order_acquire);
        if (!chunk) return nullptr;
        Slot& s = chunk[index & (kChunkSize - 1)];
        return s.live && s.generation == static_cast<u32>(handle >> 32) ? &s : nullptr;
    }
};

BufferPool::BufferPool() : m_impl(new Impl) {}
BufferPool::~BufferPool() { delete m_impl; }

u64 BufferPool::create(u32 elemSize, const void* data, u32 count) {
    FOH_ASSERT(elemSize > 0, "buffer element size must be > 0");
    Impl::Slot* slot = nullptr;
    u32 index = 0;
    {
        std::lock_guard lock(m_impl->mutex);
        if (!m_impl->freeList.empty()) {
            index = m_impl->freeList.back();
            m_impl->freeList.pop_back();
        } else {
            index = m_impl->next++;
            FOH_VERIFY((index >> kChunkBits) < kMaxChunks, "buffer pool exhausted");
            if (!m_impl->chunks[index >> kChunkBits].load(std::memory_order_relaxed)) {
                m_impl->chunks[index >> kChunkBits].store(new Impl::Slot[kChunkSize], std::memory_order_release);
            }
        }
        slot = &m_impl->chunks[index >> kChunkBits].load(std::memory_order_relaxed)[index & (kChunkSize - 1)];
        slot->live = true; // reserved; not visible to others until the handle is returned
        slot->elemSize = elemSize;
        slot->count = 0;
        slot->capacity = 0;
        slot->data = nullptr;
    }
    // Memory is allocated outside the lock: only this thread knows the handle yet.
    if (count > 0) {
        slot->capacity = count;
        slot->count = count;
        slot->data = static_cast<std::byte*>(std::malloc(static_cast<size_t>(count) * elemSize));
        if (data) std::memcpy(slot->data, data, static_cast<size_t>(count) * elemSize);
        else std::memset(slot->data, 0, static_cast<size_t>(count) * elemSize);
    }
    m_impl->live.fetch_add(1, std::memory_order_relaxed);
    return (static_cast<u64>(slot->generation) << 32) | (index + 1u);
}

void BufferPool::release(u64 handle) {
    Impl::Slot* slot = m_impl->find(handle);
    if (!slot) return;
    std::free(slot->data);
    slot->data = nullptr;
    slot->count = slot->capacity = 0;
    std::lock_guard lock(m_impl->mutex);
    slot->live = false;
    if (++slot->generation == 0) slot->generation = 1;
    m_impl->freeList.push_back(static_cast<u32>(handle) - 1u);
    m_impl->live.fetch_sub(1, std::memory_order_relaxed);
}

bool BufferPool::isValid(u64 handle) const { return m_impl->find(handle) != nullptr; }

u32 BufferPool::size(u64 handle) const {
    const Impl::Slot* s = m_impl->find(handle);
    return s ? s->count : 0;
}

u32 BufferPool::elemSize(u64 handle) const {
    const Impl::Slot* s = m_impl->find(handle);
    return s ? s->elemSize : 0;
}

void* BufferPool::data(u64 handle, [[maybe_unused]] u32 elemSize) const {
    Impl::Slot* s = m_impl->find(handle);
    if (!s) return nullptr;
    FOH_ASSERT(s->elemSize == elemSize, "buffer accessed with the wrong element type");
    return s->data;
}

void BufferPool::reserve(u64 handle, [[maybe_unused]] u32 elemSize, u32 capacity) {
    Impl::Slot* s = m_impl->find(handle);
    if (!s || capacity <= s->capacity) return;
    FOH_ASSERT(s->elemSize == elemSize, "buffer accessed with the wrong element type");
    void* grown = std::realloc(s->data, static_cast<size_t>(capacity) * elemSize);
    FOH_VERIFY(grown != nullptr, "out of memory growing a buffer");
    s->data = static_cast<std::byte*>(grown);
    s->capacity = capacity;
}

void* BufferPool::push(u64 handle, [[maybe_unused]] u32 elemSize, const void* element) {
    Impl::Slot* s = m_impl->find(handle);
    if (!s) return nullptr;
    FOH_ASSERT(s->elemSize == elemSize, "buffer accessed with the wrong element type");
    if (s->count == s->capacity) reserve(handle, elemSize, s->capacity < 4 ? 4 : s->capacity * 2);
    std::byte* slotPtr = s->data + static_cast<size_t>(s->count) * elemSize;
    if (element) std::memcpy(slotPtr, element, elemSize);
    else std::memset(slotPtr, 0, elemSize);
    ++s->count;
    return slotPtr;
}

void BufferPool::resize(u64 handle, [[maybe_unused]] u32 elemSize, u32 count) {
    Impl::Slot* s = m_impl->find(handle);
    if (!s) return;
    FOH_ASSERT(s->elemSize == elemSize, "buffer accessed with the wrong element type");
    if (count > s->capacity) reserve(handle, elemSize, count);
    if (count > s->count) {
        std::memset(s->data + static_cast<size_t>(s->count) * elemSize, 0, static_cast<size_t>(count - s->count) * elemSize);
    }
    s->count = count;
}

void BufferPool::erase(u64 handle, [[maybe_unused]] u32 elemSize, u32 index) {
    Impl::Slot* s = m_impl->find(handle);
    if (!s || index >= s->count) return;
    FOH_ASSERT(s->elemSize == elemSize, "buffer accessed with the wrong element type");
    std::byte* at = s->data + static_cast<size_t>(index) * elemSize;
    std::memmove(at, at + elemSize, static_cast<size_t>(s->count - index - 1) * elemSize);
    --s->count;
}

void BufferPool::clear(u64 handle) {
    if (Impl::Slot* s = m_impl->find(handle)) s->count = 0;
}

void* BufferPool::reshape(u64 handle, u32 newElemSize, u32 count) {
    Impl::Slot* s = m_impl->find(handle);
    if (!s) return nullptr;
    std::free(s->data);
    s->elemSize = newElemSize;
    s->count = s->capacity = count;
    s->data = count ? static_cast<std::byte*>(std::calloc(count, newElemSize)) : nullptr;
    return s->data;
}

size_t BufferPool::liveCount() const { return m_impl->live.load(std::memory_order_relaxed); }

} // namespace foh
