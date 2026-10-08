#pragma once

#include <string>
#include <vector>

#include "foh/platform.h"
#include "foh/types.h"

namespace foh {

// What a reflected field holds. Closed set on purpose: every kind has a defined
// binary layout, a conversion rule for migration, and a text form.
enum class FieldKind : u8 {
    Bool,
    I8, I16, I32, I64,
    U8, U16, U32, U64,
    F32, F64,
    Entity, // 64-bit id; remapped when a snapshot is loaded
    Vec2, Vec3, Vec4, Quat, // float vectors (glm layout)
    Text,   // char[N], NUL-terminated
    Struct, // nested reflected struct (children)
    Buffer, // BufferHandle<T>: engine-owned variable-length array (children = element)
};

struct SchemaField {
    std::string name;
    // Name this field had in an older version of the type; lets a rename migrate
    // its data instead of resetting it.
    std::string formerName;
    FieldKind kind = FieldKind::I32;
    u32 offset = 0;
    u32 count = 1;    // array length; for Text, the capacity in bytes
    u32 elemSize = 0; // bytes of one element
    // Struct: the nested fields. Buffer: the fields of ONE buffer element (a single
    // unnamed field for primitive elements).
    std::vector<SchemaField> children;
    u32 childSize = 0; // Buffer: sizeof one element
};

// Pure data description of a type's memory layout. Contains no function pointers
// and no shared_ptr control blocks, so it can safely outlive the module (DLL)
// that built it. This is what makes hot-reload migration and save files possible.
struct Schema {
    std::string name;
    u32 size = 0;
    u32 alignment = 0;
    // false for types without FOH_REFLECT: they still work, but cannot be saved,
    // inspected field by field, or migrated.
    bool reflected = false;
    std::vector<SchemaField> fields;
};

// True if both schemas describe exactly the same layout (names, kinds, offsets,
// counts, sizes, recursively). Former names are ignored.
FOH_API bool sameLayout(const Schema& a, const Schema& b);
FOH_API const char* fieldKindName(FieldKind kind);

} // namespace foh
