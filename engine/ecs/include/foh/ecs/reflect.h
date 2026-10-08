#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <type_traits>
#include <utility>

#include <glm/detail/qualifier.hpp> // forward declarations only: keeps this header light

#include "foh/ecs/buffer.h"
#include "foh/ecs/schema.h"
#include "foh/ecs/type_name.h"
#include "foh/ecs/types.h"

namespace foh {

// Customization point: specialize (via FOH_REFLECT) to describe a struct's fields.
//
//     struct Transform { Vec3 position; Quat rotation; f32 scale; };
//     FOH_REFLECT(Transform, position, rotation, scale);   // at global scope
//
// The description is plain data (names, kinds, offsets) read once at registration
// and then kept by the engine, never a function pointer, so it survives a game
// module reload. Reflected types can be saved, loaded and migrated when their
// layout changes; unreflected types still work but cannot.
//
// Supported field types: bool, all integer and float types, enums, Entity, glm
// vec2/vec3/vec4/quat, char[N] (text), BufferHandle<T>, fixed arrays and
// std::array of those, and other reflected structs. To rename a field without
// losing data, write describe() by hand and pass the old name:
//     b.field("speed", &Mover::speed, "velocity");
template <class T>
struct Reflect {};

template <class T>
class SchemaBuilder;

namespace detail {

template <class T>
concept Reflected = requires(SchemaBuilder<T>& b) { Reflect<T>::describe(b); };

template <class T>
inline constexpr bool kDependentFalse = false;

template <class T>
struct IsStdArray : std::false_type {};
template <class T, size_t N>
struct IsStdArray<std::array<T, N>> : std::true_type {};

template <class T>
struct IsBufferHandle : std::false_type {};
template <class T>
struct IsBufferHandle<BufferHandle<T>> : std::true_type {};

template <class T, class F>
u32 memberOffset(F T::*member) {
    // Probe a zeroed object-sized block instead of a real T: no constructor needed.
    alignas(T) static const unsigned char storage[sizeof(T)] = {};
    const T* base = reinterpret_cast<const T*>(storage);
    return static_cast<u32>(reinterpret_cast<const unsigned char*>(&(base->*member)) - storage);
}

template <class F>
SchemaField classify();

template <class E>
void describeBufferElement(SchemaField& buffer) {
    SchemaField element = classify<E>();
    if (element.kind == FieldKind::Struct) {
        buffer.children = std::move(element.children);
    } else {
        element.name.clear();
        element.offset = 0;
        buffer.children.clear();
        buffer.children.push_back(std::move(element));
    }
    buffer.childSize = static_cast<u32>(sizeof(E));
}

template <class F>
SchemaField classify() {
    SchemaField f;
    if constexpr (std::is_array_v<F>) {
        using E = std::remove_cv_t<std::remove_extent_t<F>>;
        static_assert(!std::is_array_v<E>, "multi-dimensional arrays are not reflectable: wrap the inner one in a struct");
        if constexpr (std::is_same_v<E, char>) {
            f.kind = FieldKind::Text;
            f.count = static_cast<u32>(std::extent_v<F>);
            f.elemSize = 1;
        } else {
            f = classify<E>();
            f.count = static_cast<u32>(std::extent_v<F>);
        }
    } else if constexpr (IsStdArray<F>::value) {
        f = classify<typename F::value_type>();
        f.count = static_cast<u32>(std::tuple_size_v<F>);
    } else if constexpr (std::is_same_v<F, bool>) {
        f.kind = FieldKind::Bool;
        f.elemSize = 1;
    } else if constexpr (std::is_enum_v<F>) {
        f = classify<std::underlying_type_t<F>>();
    } else if constexpr (std::is_integral_v<F>) {
        static_assert(sizeof(F) <= 8);
        constexpr bool s = std::is_signed_v<F>;
        f.kind = sizeof(F) == 1 ? (s ? FieldKind::I8 : FieldKind::U8)
               : sizeof(F) == 2 ? (s ? FieldKind::I16 : FieldKind::U16)
               : sizeof(F) == 4 ? (s ? FieldKind::I32 : FieldKind::U32)
                                : (s ? FieldKind::I64 : FieldKind::U64);
        f.elemSize = static_cast<u32>(sizeof(F));
    } else if constexpr (std::is_floating_point_v<F>) {
        static_assert(sizeof(F) == 4 || sizeof(F) == 8, "long double is not reflectable");
        f.kind = sizeof(F) == 4 ? FieldKind::F32 : FieldKind::F64;
        f.elemSize = static_cast<u32>(sizeof(F));
    } else if constexpr (std::is_same_v<F, Entity>) {
        f.kind = FieldKind::Entity;
        f.elemSize = 8;
    } else if constexpr (std::is_same_v<F, glm::vec<2, float, glm::defaultp>>) {
        f.kind = FieldKind::Vec2;
        f.elemSize = 8;
    } else if constexpr (std::is_same_v<F, glm::vec<3, float, glm::defaultp>>) {
        f.kind = FieldKind::Vec3;
        f.elemSize = 12;
    } else if constexpr (std::is_same_v<F, glm::vec<4, float, glm::defaultp>>) {
        f.kind = FieldKind::Vec4;
        f.elemSize = 16;
    } else if constexpr (std::is_same_v<F, glm::qua<float, glm::defaultp>>) {
        f.kind = FieldKind::Quat;
        f.elemSize = 16;
    } else if constexpr (IsBufferHandle<F>::value) {
        f.kind = FieldKind::Buffer;
        f.elemSize = 8;
        describeBufferElement<typename F::Element>(f);
    } else if constexpr (Reflected<F>) {
        static_assert(std::is_standard_layout_v<F>, "reflected structs must be standard layout");
        Schema nested;
        SchemaBuilder<F> builder(nested);
        Reflect<F>::describe(builder);
        f.kind = FieldKind::Struct;
        f.elemSize = static_cast<u32>(sizeof(F));
        f.children = std::move(nested.fields);
    } else {
        static_assert(kDependentFalse<F>,
                      "field type is not reflectable: use a supported type or add FOH_REFLECT for the struct");
    }
    return f;
}

} // namespace detail

template <class T>
class SchemaBuilder {
public:
    explicit SchemaBuilder(Schema& schema) : m_schema(schema) {}

    template <class F>
    SchemaBuilder& field(const char* name, F T::*member, const char* formerName = nullptr) {
        SchemaField f = detail::classify<std::remove_cv_t<F>>();
        f.name = name;
        if (formerName) f.formerName = formerName;
        f.offset = detail::memberOffset(member);
        m_schema.fields.push_back(std::move(f));
        return *this;
    }

private:
    Schema& m_schema;
};

namespace detail {

// True if the type declares `static constexpr bool kSparse = true;`: its storage is a
// sparse set, so adding and removing it never changes the entity's archetype. Good
// for markers and state flags that toggle often. The price: it is read per entity
// instead of per table column, and a query that mentions it is a little slower.
template <class T>
constexpr bool isSparse() {
    if constexpr (requires { T::kSparse; }) {
        return static_cast<bool>(T::kSparse);
    } else {
        return false;
    }
}

// Identity of a component/resource: kName if the type declares one (stable even if
// the type is renamed or moved to another namespace), else its qualified type name.
template <class T>
constexpr std::string_view componentName() {
    if constexpr (requires { T::kName; }) {
        return std::string_view(T::kName);
    } else {
        return typeName<T>();
    }
}

template <class T>
void buildSchema(Schema& out) {
    out.name = std::string(componentName<T>());
    out.size = std::is_empty_v<T> ? 0u : static_cast<u32>(sizeof(T));
    out.alignment = std::is_empty_v<T> ? 0u : static_cast<u32>(alignof(T));
    if constexpr (std::is_empty_v<T>) {
        out.reflected = true; // a tag has nothing to describe
    } else if constexpr (Reflected<T>) {
        static_assert(std::is_standard_layout_v<T>, "reflected types must be standard layout");
        SchemaBuilder<T> builder(out);
        Reflect<T>::describe(builder);
        out.reflected = true;
    } else {
        out.reflected = false;
    }
}

} // namespace detail

} // namespace foh

// ---- FOH_REFLECT ---------------------------------------------------------------
#define FOH_PP_CAT_(a, b) a##b
#define FOH_PP_CAT(a, b) FOH_PP_CAT_(a, b)
#define FOH_PP_NARG_( _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, _18, _19, _20, _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, _31, _32, N, ...) N
#define FOH_PP_NARG(...) FOH_PP_NARG_(__VA_ARGS__, 32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1)
#define FOH_REFLECT_FIELD(Type, f) b.field(#f, &Type::f);
#define FOH_FE_1(T, a) FOH_REFLECT_FIELD(T, a)
#define FOH_FE_2(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_1(T, __VA_ARGS__)
#define FOH_FE_3(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_2(T, __VA_ARGS__)
#define FOH_FE_4(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_3(T, __VA_ARGS__)
#define FOH_FE_5(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_4(T, __VA_ARGS__)
#define FOH_FE_6(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_5(T, __VA_ARGS__)
#define FOH_FE_7(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_6(T, __VA_ARGS__)
#define FOH_FE_8(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_7(T, __VA_ARGS__)
#define FOH_FE_9(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_8(T, __VA_ARGS__)
#define FOH_FE_10(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_9(T, __VA_ARGS__)
#define FOH_FE_11(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_10(T, __VA_ARGS__)
#define FOH_FE_12(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_11(T, __VA_ARGS__)
#define FOH_FE_13(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_12(T, __VA_ARGS__)
#define FOH_FE_14(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_13(T, __VA_ARGS__)
#define FOH_FE_15(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_14(T, __VA_ARGS__)
#define FOH_FE_16(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_15(T, __VA_ARGS__)
#define FOH_FE_17(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_16(T, __VA_ARGS__)
#define FOH_FE_18(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_17(T, __VA_ARGS__)
#define FOH_FE_19(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_18(T, __VA_ARGS__)
#define FOH_FE_20(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_19(T, __VA_ARGS__)
#define FOH_FE_21(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_20(T, __VA_ARGS__)
#define FOH_FE_22(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_21(T, __VA_ARGS__)
#define FOH_FE_23(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_22(T, __VA_ARGS__)
#define FOH_FE_24(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_23(T, __VA_ARGS__)
#define FOH_FE_25(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_24(T, __VA_ARGS__)
#define FOH_FE_26(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_25(T, __VA_ARGS__)
#define FOH_FE_27(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_26(T, __VA_ARGS__)
#define FOH_FE_28(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_27(T, __VA_ARGS__)
#define FOH_FE_29(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_28(T, __VA_ARGS__)
#define FOH_FE_30(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_29(T, __VA_ARGS__)
#define FOH_FE_31(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_30(T, __VA_ARGS__)
#define FOH_FE_32(T, a, ...) FOH_REFLECT_FIELD(T, a) FOH_FE_31(T, __VA_ARGS__)
#define FOH_FOR_EACH_FIELD(T, ...) FOH_PP_CAT(FOH_FE_, FOH_PP_NARG(__VA_ARGS__))(T, __VA_ARGS__)

// Describes `Type` from a list of its data members. Use at global scope, right
// after the struct:   FOH_REFLECT(game::Health, current, maximum);
#define FOH_REFLECT(Type, ...)                                                      \
    template <>                                                                     \
    struct ::foh::Reflect<Type> {                                                   \
        static void describe(::foh::SchemaBuilder<Type>& b) {                       \
            FOH_FOR_EACH_FIELD(Type, __VA_ARGS__)                                   \
        }                                                                           \
    }

FOH_REFLECT(foh::Time, elapsed, delta, frame);
