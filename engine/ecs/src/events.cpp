#include "foh/ecs/events.h"

#include <algorithm>
#include <cstring>

namespace foh {

void EventChannel::append(const void* data, u32 count) {
    if (count == 0) return;
    const size_t at = m_bytes.size();
    m_bytes.resize(at + static_cast<size_t>(count) * m_elemSize);
    std::memcpy(m_bytes.data() + at, data, static_cast<size_t>(count) * m_elemSize);
    m_count += count;
}

const std::byte* EventChannel::read(u64& cursor, u32& count) const {
    // A cursor behind the stored range means those events were already dropped.
    const u64 from = std::max(cursor, m_baseSeq);
    const u64 end = m_baseSeq + m_count;
    count = from < end ? static_cast<u32>(end - from) : 0;
    cursor = end;
    return m_bytes.data() + static_cast<size_t>(from - m_baseSeq) * m_elemSize;
}

void EventChannel::advance() {
    // Two generations are kept: the one that just ended and the one before is
    // dropped, so every event is readable for the rest of its frame plus one more.
    const size_t dropBytes = static_cast<size_t>(m_curStart) * m_elemSize;
    m_bytes.erase(m_bytes.begin(), m_bytes.begin() + static_cast<std::ptrdiff_t>(dropBytes));
    m_baseSeq += m_curStart;
    m_count -= m_curStart;
    m_curStart = m_count;
}

void EventChannel::clear() {
    m_bytes.clear();
    m_baseSeq += m_count;
    m_count = 0;
    m_curStart = 0;
}

} // namespace foh
