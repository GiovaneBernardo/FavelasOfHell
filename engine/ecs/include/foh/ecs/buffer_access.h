#pragma once

#include <span>

#include "foh/ecs/buffer.h"

namespace foh {

namespace detail {
// Identity used in access sets: all buffers of element type T are one resource
// as far as scheduling goes.
template <class T>
struct BufferAccessTag {};
} // namespace detail

// System parameter: read access to the contents of buffers of element type T.
//
//     void Total(Query<const Inventory&> q, Buffers<ItemStack> items) {
//         q.each([&](const Inventory& inv) { for (const ItemStack& s : items.get(inv.items)) ... });
//     }
//
// The scheduler will not run a system that modifies Buffers<T> (BuffersMut<T>) at
// the same time as one that reads them.
template <class T>
class Buffers {
public:
    explicit Buffers(const BufferPool& pool) : m_pool(&pool) {}

    // Empty span for a null or stale handle.
    std::span<const T> get(BufferHandle<T> handle) const {
        return {static_cast<const T*>(m_pool->data(handle.raw, sizeof(T))), m_pool->size(handle.raw)};
    }
    u32 size(BufferHandle<T> handle) const { return m_pool->size(handle.raw); }

private:
    const BufferPool* m_pool;
};

// System parameter: modify buffers of element type T. Exclusive among systems that
// touch Buffers<T>, but inside one system (e.g. parEach) different threads may
// modify different buffers at once.
template <class T>
class BuffersMut {
public:
    explicit BuffersMut(BufferPool& pool) : m_pool(&pool) {}

    std::span<T> get(BufferHandle<T> handle) const {
        return {static_cast<T*>(m_pool->data(handle.raw, sizeof(T))), m_pool->size(handle.raw)};
    }
    u32 size(BufferHandle<T> handle) const { return m_pool->size(handle.raw); }

    // New buffer holding a copy of `initial`. Store the handle in a component (it
    // then owns the buffer); a handle that is never stored leaks until shutdown.
    BufferHandle<T> create(std::span<const T> initial = {}) const {
        return {m_pool->create(sizeof(T), initial.data(), static_cast<u32>(initial.size()))};
    }
    T* push(BufferHandle<T> handle, const T& value) const {
        return static_cast<T*>(m_pool->push(handle.raw, sizeof(T), &value));
    }
    void erase(BufferHandle<T> handle, u32 index) const { m_pool->erase(handle.raw, sizeof(T), index); }
    void resize(BufferHandle<T> handle, u32 count) const { m_pool->resize(handle.raw, sizeof(T), count); }
    void clear(BufferHandle<T> handle) const { m_pool->clear(handle.raw); }
    // Frees the buffer now; clear the handle in its component afterwards.
    void release(BufferHandle<T> handle) const { m_pool->release(handle.raw); }

private:
    BufferPool* m_pool;
};

} // namespace foh
