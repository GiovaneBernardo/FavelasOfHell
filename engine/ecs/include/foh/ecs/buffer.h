#pragma once

#include <span>
#include <type_traits>

#include "foh/platform.h"
#include "foh/types.h"

namespace foh {

// Handle to a variable-length array owned by the engine (see BufferPool).
//
// This is how a component holds "a list of things" without owning memory itself:
// the component stays plain data (so it can be memcpy'd, saved and migrated), and
// the elements live in an engine pool that survives game module reloads.
//
//     struct Inventory { u32 gold; BufferHandle<ItemStack> items; };
//     FOH_REFLECT(Inventory, gold, items);
//
// A handle OWNS its buffer: when the component is removed or the entity is
// despawned, the engine frees the buffer. Do not copy a handle into a second
// component; create a second buffer instead.
template <class T>
struct BufferHandle {
    static_assert(std::is_trivially_copyable_v<T>, "buffer elements must be trivially copyable");
    using Element = T;
    u64 raw = 0; // (generation << 32) | (slot + 1); 0 means "no buffer"

    constexpr explicit operator bool() const { return raw != 0; }
    friend constexpr bool operator==(BufferHandle, BufferHandle) = default;
};

// Variable-length arrays addressed by handle. Engine-owned memory.
//
// Thread-safety: create()/release() are thread-safe (a short lock around the slot
// table; the element memory is allocated outside it). Reading or modifying one
// buffer's contents needs exclusive access to THAT buffer: the scheduler provides
// it through the access set of Buffers<T>/BuffersMut<T>, and different buffers
// can be modified from different threads at the same time (e.g. inside parEach).
// Pointers into a buffer are invalidated when that buffer grows.
class FOH_API BufferPool {
public:
    BufferPool();
    ~BufferPool();
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    u64 create(u32 elemSize, const void* data, u32 count);
    void release(u64 handle);
    bool isValid(u64 handle) const;

    u32 size(u64 handle) const;
    void* data(u64 handle, u32 elemSize) const;
    void* push(u64 handle, u32 elemSize, const void* element); // returns the new element
    void reserve(u64 handle, u32 elemSize, u32 capacity);
    void resize(u64 handle, u32 elemSize, u32 count);          // new elements are zeroed
    void erase(u64 handle, u32 elemSize, u32 index);           // keeps order
    void clear(u64 handle);

    // Re-lays out an existing buffer in place (migration): same handle, new element size.
    void* reshape(u64 handle, u32 newElemSize, u32 count);
    u32 elemSize(u64 handle) const;

    size_t liveCount() const;

private:
    struct Impl;
    Impl* m_impl;
};

} // namespace foh
