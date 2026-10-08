#pragma once

#include <span>
#include <type_traits>
#include <vector>

#include "gtabr/ecs/world.h"

namespace gtabr {

// Engine-owned storage for one event type (raw bytes; survives game reloads).
//
// An event sent during a frame is readable by systems that run after the
// segment that sent it, for the rest of that frame and the whole next frame.
// Every reader keeps its own cursor, so it sees each event exactly once.
//
// Thread-safety: not thread-safe. append()/advance() run on the main thread
// between parallel segments; read() may be called concurrently by readers
// because nothing mutates the channel while systems run.
class GTABR_API EventChannel {
public:
    explicit EventChannel(u32 elemSize) : m_elemSize(elemSize) {}

    u32 elemSize() const { return m_elemSize; }
    void append(const void* data, u32 count);
    // Events with sequence number >= cursor that are still stored; moves the
    // cursor to the end. The pointer is valid until the next append/advance.
    const std::byte* read(u64& cursor, u32& count) const;
    void advance();
    void clear();

private:
    u32 m_elemSize;
    std::vector<std::byte> m_bytes;
    u64 m_baseSeq = 0;  // sequence number of the first stored event
    u32 m_count = 0;    // stored events
    u32 m_curStart = 0; // index where the current frame's events begin
};

template <class T>
EventChannel& eventChannelFor(World& world) {
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>,
                  "events are copied as raw bytes: they must be trivially copyable");
    static_assert(alignof(T) <= 16, "event alignment above 16 is not supported");
    static_assert(!std::is_empty_v<T>, "an event needs data");
    return world.eventChannel(detail::fnv1a(detail::typeName<T>()), static_cast<u32>(sizeof(T)));
}

// System parameter: sends events. Buffered per system and published in system
// registration order after the parallel segment, so sending needs no locking.
template <class T>
class EventWriter {
public:
    explicit EventWriter(std::vector<T>& buffer) : m_buffer(&buffer) {}
    void send(const T& event) { m_buffer->push_back(event); }

private:
    std::vector<T>* m_buffer;
};

// System parameter: reads events not yet seen by this system.
template <class T>
class EventReader {
public:
    EventReader(const EventChannel& channel, u64& cursor) : m_channel(&channel), m_cursor(&cursor) {}
    // Valid until the system returns.
    std::span<const T> read() const {
        u32 count = 0;
        const std::byte* data = m_channel->read(*m_cursor, count);
        return {reinterpret_cast<const T*>(data), count};
    }

private:
    const EventChannel* m_channel;
    u64* m_cursor;
};

} // namespace gtabr
