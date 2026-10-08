#include <algorithm>
#include <cstring>
#include <limits>

#include "foh/ecs/schema.h"
#include "schema_ops.h"

namespace foh {

const char* fieldKindName(FieldKind kind) {
    switch (kind) {
        case FieldKind::Bool: return "bool";
        case FieldKind::I8: return "i8";
        case FieldKind::I16: return "i16";
        case FieldKind::I32: return "i32";
        case FieldKind::I64: return "i64";
        case FieldKind::U8: return "u8";
        case FieldKind::U16: return "u16";
        case FieldKind::U32: return "u32";
        case FieldKind::U64: return "u64";
        case FieldKind::F32: return "f32";
        case FieldKind::F64: return "f64";
        case FieldKind::Entity: return "entity";
        case FieldKind::Vec2: return "vec2";
        case FieldKind::Vec3: return "vec3";
        case FieldKind::Vec4: return "vec4";
        case FieldKind::Quat: return "quat";
        case FieldKind::Text: return "text";
        case FieldKind::Struct: return "struct";
        case FieldKind::Buffer: return "buffer";
    }
    return "?";
}

bool sameLayout(const Schema& a, const Schema& b) {
    if (a.size != b.size || a.alignment != b.alignment || a.reflected != b.reflected) return false;
    return schema::sameFields(a.fields, b.fields);
}

namespace schema {

bool sameFields(const Fields& a, const Fields& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const SchemaField& x = a[i];
        const SchemaField& y = b[i];
        if (x.name != y.name || x.kind != y.kind || x.offset != y.offset || x.count != y.count ||
            x.elemSize != y.elemSize || x.childSize != y.childSize || !sameFields(x.children, y.children)) {
            return false;
        }
    }
    return true;
}

bool hasBuffers(const Fields& fields) {
    for (const SchemaField& f : fields) {
        if (f.kind == FieldKind::Buffer) return true;
        if (f.kind == FieldKind::Struct && hasBuffers(f.children)) return true;
    }
    return false;
}

void releaseBuffers(BufferPool& pool, const Fields& fields, std::byte* base) {
    visitLeaves(fields, base, [&](const SchemaField& f, std::byte* p) {
        if (f.kind != FieldKind::Buffer) return;
        u64 handle;
        std::memcpy(&handle, p, sizeof(handle));
        if (!handle || !pool.isValid(handle)) return;
        if (hasBuffers(f.children)) {
            // Buffers owned by the elements go first.
            std::byte* data = static_cast<std::byte*>(pool.data(handle, f.childSize));
            for (u32 i = 0, n = pool.size(handle); i < n; ++i) {
                releaseBuffers(pool, f.children, data + static_cast<size_t>(i) * f.childSize);
            }
        }
        pool.release(handle);
    });
}

void clearBufferHandles(const Fields& fields, std::byte* base) {
    visitLeaves(fields, base, [](const SchemaField& f, std::byte* p) {
        if (f.kind == FieldKind::Buffer) std::memset(p, 0, sizeof(u64));
    });
}

namespace {

// A number read from storage, kept in the widest type of its family.
struct Number {
    enum Family { Signed, Unsigned, Float } family = Signed;
    i64 s = 0;
    u64 u = 0;
    double f = 0;
};

template <class T>
T load(const std::byte* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

Number readNumber(FieldKind k, const std::byte* p) {
    Number n;
    switch (k) {
        case FieldKind::Bool: n.family = Number::Unsigned; n.u = load<u8>(p) != 0; break;
        case FieldKind::I8: n.s = load<i8>(p); break;
        case FieldKind::I16: n.s = load<i16>(p); break;
        case FieldKind::I32: n.s = load<i32>(p); break;
        case FieldKind::I64: n.s = load<i64>(p); break;
        case FieldKind::U8: n.family = Number::Unsigned; n.u = load<u8>(p); break;
        case FieldKind::U16: n.family = Number::Unsigned; n.u = load<u16>(p); break;
        case FieldKind::U32: n.family = Number::Unsigned; n.u = load<u32>(p); break;
        case FieldKind::U64: n.family = Number::Unsigned; n.u = load<u64>(p); break;
        case FieldKind::F32: n.family = Number::Float; n.f = load<f32>(p); break;
        case FieldKind::F64: n.family = Number::Float; n.f = load<f64>(p); break;
        default: break;
    }
    return n;
}

template <class T>
void store(std::byte* p, T v) {
    std::memcpy(p, &v, sizeof(T));
}

// Converts with saturation so a narrowing change never wraps silently.
template <class T>
T toInteger(const Number& n) {
    constexpr bool kSigned = std::numeric_limits<T>::is_signed;
    switch (n.family) {
        case Number::Signed:
            if (n.s < 0 && !kSigned) return 0;
            if (n.s > 0 && static_cast<u64>(n.s) > static_cast<u64>(std::numeric_limits<T>::max())) return std::numeric_limits<T>::max();
            if (kSigned && n.s < static_cast<i64>(std::numeric_limits<T>::min())) return std::numeric_limits<T>::min();
            return static_cast<T>(n.s);
        case Number::Unsigned:
            return n.u > static_cast<u64>(std::numeric_limits<T>::max()) ? std::numeric_limits<T>::max() : static_cast<T>(n.u);
        case Number::Float: {
            if (!(n.f == n.f)) return 0;
            if (n.f <= static_cast<double>(std::numeric_limits<T>::lowest())) return std::numeric_limits<T>::lowest();
            if (n.f >= static_cast<double>(std::numeric_limits<T>::max())) return std::numeric_limits<T>::max();
            return static_cast<T>(n.f);
        }
    }
    return 0;
}

double toDouble(const Number& n) {
    switch (n.family) {
        case Number::Signed: return static_cast<double>(n.s);
        case Number::Unsigned: return static_cast<double>(n.u);
        case Number::Float: return n.f;
    }
    return 0;
}

void convertNumber(FieldKind from, const std::byte* src, FieldKind to, std::byte* dst) {
    const Number n = readNumber(from, src);
    switch (to) {
        case FieldKind::Bool: store<u8>(dst, toDouble(n) != 0.0 ? 1 : 0); break;
        case FieldKind::I8: store(dst, toInteger<i8>(n)); break;
        case FieldKind::I16: store(dst, toInteger<i16>(n)); break;
        case FieldKind::I32: store(dst, toInteger<i32>(n)); break;
        case FieldKind::I64: store(dst, toInteger<i64>(n)); break;
        case FieldKind::U8: store(dst, toInteger<u8>(n)); break;
        case FieldKind::U16: store(dst, toInteger<u16>(n)); break;
        case FieldKind::U32: store(dst, toInteger<u32>(n)); break;
        case FieldKind::U64: store(dst, toInteger<u64>(n)); break;
        case FieldKind::F32: store(dst, static_cast<f32>(toDouble(n))); break;
        case FieldKind::F64: store(dst, toDouble(n)); break;
        default: break;
    }
}

void convertBuffer(BufferPool& pool, const SchemaField& from, const std::byte* src, const SchemaField& to,
                   std::byte* dst) {
    u64 handle;
    std::memcpy(&handle, src, sizeof(handle));
    if (!handle || !pool.isValid(handle)) return; // none, or dangling: stays empty
    if (from.childSize != to.childSize || !sameFields(from.children, to.children)) {
        const u32 count = pool.size(handle);
        std::vector<std::byte> old(static_cast<size_t>(count) * from.childSize);
        if (!old.empty()) std::memcpy(old.data(), pool.data(handle, from.childSize), old.size());
        std::byte* fresh = static_cast<std::byte*>(pool.reshape(handle, to.childSize, count));
        for (u32 i = 0; i < count; ++i) {
            convertRecord(pool, from.children, old.data() + static_cast<size_t>(i) * from.childSize, to.children,
                          fresh + static_cast<size_t>(i) * to.childSize);
        }
    }
    std::memcpy(dst, &handle, sizeof(handle)); // the handle (ownership) moves to dst
}

void convertField(BufferPool& pool, const SchemaField& from, const std::byte* src, const SchemaField& to,
                  std::byte* dst) {
    if (from.kind == FieldKind::Text && to.kind == FieldKind::Text) {
        const u32 n = std::min(from.count, to.count);
        std::memcpy(dst, src, n);
        if (to.count > 0) dst[to.count - 1] = std::byte{0};
        return;
    }
    const u32 n = std::min(elementCount(from), elementCount(to));
    for (u32 i = 0; i < n; ++i) {
        const std::byte* s = src + static_cast<size_t>(i) * from.elemSize;
        std::byte* d = dst + static_cast<size_t>(i) * to.elemSize;
        if (from.kind == FieldKind::Struct && to.kind == FieldKind::Struct) {
            convertRecord(pool, from.children, s, to.children, d);
        } else if (from.kind == FieldKind::Buffer && to.kind == FieldKind::Buffer) {
            convertBuffer(pool, from, s, to, d);
        } else if (isNumeric(from.kind) && isNumeric(to.kind)) {
            convertNumber(from.kind, s, to.kind, d);
        } else if (from.kind == to.kind) {
            std::memcpy(d, s, std::min(from.elemSize, to.elemSize)); // entity, vectors
        }
        // Different non-numeric kinds: incompatible, the field keeps its default.
    }
}

} // namespace

void convertRecord(BufferPool& pool, const Fields& from, const std::byte* src, const Fields& to, std::byte* dst) {
    for (const SchemaField& target : to) {
        const SchemaField* source = nullptr;
        for (const SchemaField& candidate : from) {
            if (candidate.name == target.name) {
                source = &candidate;
                break;
            }
        }
        if (!source && !target.formerName.empty()) {
            for (const SchemaField& candidate : from) {
                if (candidate.name == target.formerName) {
                    source = &candidate;
                    break;
                }
            }
        }
        if (source) convertField(pool, *source, src + source->offset, target, dst + target.offset);
    }
}

} // namespace schema

} // namespace foh

namespace foh::schema {

void remapEntities(BufferPool& pool, const Fields& fields, std::byte* base,
                   const std::unordered_map<u64, u64>& map) {
    visitLeaves(fields, base, [&](const SchemaField& f, std::byte* p) {
        if (f.kind == FieldKind::Entity) {
            u64 id;
            std::memcpy(&id, p, sizeof(id));
            const auto it = map.find(id);
            id = it == map.end() ? 0 : it->second;
            std::memcpy(p, &id, sizeof(id));
        } else if (f.kind == FieldKind::Buffer) {
            u64 handle;
            std::memcpy(&handle, p, sizeof(handle));
            if (!handle || !pool.isValid(handle)) return;
            auto* data = static_cast<std::byte*>(pool.data(handle, f.childSize));
            for (u32 i = 0, n = pool.size(handle); i < n; ++i) {
                remapEntities(pool, f.children, data + static_cast<size_t>(i) * f.childSize, map);
            }
        }
    });
}

} // namespace foh::schema
