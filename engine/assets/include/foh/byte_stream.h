#pragma once

// Tiny bounds-checked binary streams for asset metadata chunks. Every value is
// little endian and fixed width. Reading past the end (or a length that does not
// fit) sets the failure flag and returns zeros, so a corrupt chunk never reads
// out of bounds; callers check ok() once at the end.

#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "foh/asset_container.h"
#include "foh/math.h"
#include "foh/types.h"

namespace foh {

class ByteWriter {
public:
    template <class T>
    void put(T v) {
        static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
        const size_t at = m_bytes.size();
        m_bytes.resize(at + sizeof(T));
        std::memcpy(m_bytes.data() + at, &v, sizeof(T));
    }
    void putString(const std::string& s) {
        put<u32>(static_cast<u32>(s.size()));
        m_bytes.insert(m_bytes.end(), s.begin(), s.end());
    }
    void putId(const AssetId& id) {
        put<u64>(id.hi);
        put<u64>(id.lo);
    }
    void putVec2(const Vec2& v) { put(v.x), put(v.y); }
    void putVec3(const Vec3& v) { put(v.x), put(v.y), put(v.z); }
    void putVec4(const Vec4& v) { put(v.x), put(v.y), put(v.z), put(v.w); }
    void putQuat(const Quat& q) { put(q.w), put(q.x), put(q.y), put(q.z); }

    const std::vector<u8>& bytes() const { return m_bytes; }
    std::vector<u8> take() { return std::move(m_bytes); }

private:
    std::vector<u8> m_bytes;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const u8> bytes) : m_bytes(bytes) {}

    template <class T>
    T get() {
        static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
        T v{};
        if (!take(sizeof(T))) return v;
        std::memcpy(&v, m_bytes.data() + m_pos - sizeof(T), sizeof(T));
        return v;
    }
    std::string getString() {
        const u32 n = get<u32>();
        if (!take(n)) return {};
        return std::string(reinterpret_cast<const char*>(m_bytes.data() + m_pos - n), n);
    }
    AssetId getId() {
        AssetId id;
        id.hi = get<u64>();
        id.lo = get<u64>();
        return id;
    }
    Vec2 getVec2() { return {get<f32>(), get<f32>()}; }
    Vec3 getVec3() { return {get<f32>(), get<f32>(), get<f32>()}; }
    Vec4 getVec4() { return {get<f32>(), get<f32>(), get<f32>(), get<f32>()}; }
    Quat getQuat() {
        const f32 w = get<f32>(), x = get<f32>(), y = get<f32>(), z = get<f32>();
        return Quat(w, x, y, z);
    }
    // A count that is about to drive an allocation: each element needs at least
    // `minElementBytes` more bytes, so a forged count cannot exceed what is left.
    u32 getCount(size_t minElementBytes) {
        const u32 n = get<u32>();
        if (minElementBytes != 0 && n > (m_bytes.size() - m_pos) / minElementBytes) {
            m_ok = false;
            return 0;
        }
        return n;
    }

    bool ok() const { return m_ok; }
    size_t remaining() const { return m_bytes.size() - m_pos; }

private:
    bool take(size_t n) {
        if (!m_ok || n > m_bytes.size() - m_pos) {
            m_ok = false;
            return false;
        }
        m_pos += n;
        return true;
    }

    std::span<const u8> m_bytes;
    size_t m_pos = 0;
    bool m_ok = true;
};

} // namespace foh
