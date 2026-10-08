#pragma once

// Engine-internal operations over schemas: walking values, owning buffers, and
// converting a value from one layout to another (migration, snapshot loading).

#include <unordered_map>
#include <vector>

#include "foh/ecs/buffer.h"
#include "foh/ecs/schema.h"

namespace foh::schema {

using Fields = std::vector<SchemaField>;

inline u32 elementCount(const SchemaField& f) { return f.kind == FieldKind::Text ? 1u : f.count; }

inline bool isNumeric(FieldKind k) { return k >= FieldKind::Bool && k <= FieldKind::F64; }

// Calls fn(field, pointer) for every leaf value (arrays expanded element by
// element, nested structs recursed into). Buffer fields are leaves.
template <class Fn>
void visitLeaves(const Fields& fields, std::byte* base, Fn&& fn) {
    for (const SchemaField& f : fields) {
        const u32 n = elementCount(f);
        for (u32 i = 0; i < n; ++i) {
            std::byte* p = base + f.offset + static_cast<size_t>(i) * f.elemSize;
            if (f.kind == FieldKind::Struct) visitLeaves(f.children, p, fn);
            else fn(f, p);
        }
    }
}

bool sameFields(const Fields& a, const Fields& b);
bool hasBuffers(const Fields& fields);

// Frees every buffer owned by the value (including buffers nested in buffer elements).
void releaseBuffers(BufferPool& pool, const Fields& fields, std::byte* base);
// Zeroes the buffer handles (ownership moved elsewhere).
void clearBufferHandles(const Fields& fields, std::byte* base);

// Converts `src` (laid out per `from`) into `dst` (per `to`), matching fields by
// name (or former name). `dst` must be zeroed. Missing fields stay zero, removed
// ones are dropped, numbers are converted, buffers are re-laid out IN PLACE in the
// pool (the handle moves to `dst`).
void convertRecord(BufferPool& pool, const Fields& from, const std::byte* src, const Fields& to, std::byte* dst);

} // namespace foh::schema

namespace foh::schema {

// Replaces every Entity value in the record (including inside nested structs and
// buffer elements) through `map`; ids not in the map become null.
void remapEntities(BufferPool& pool, const Fields& fields, std::byte* base,
                   const std::unordered_map<u64, u64>& map);

} // namespace foh::schema
