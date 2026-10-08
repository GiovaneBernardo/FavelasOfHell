#pragma once

#include <string_view>

#include "foh/types.h"

namespace foh::detail {

template <class T>
constexpr std::string_view rawTypeName() {
#if defined(_MSC_VER) && !defined(__clang__)
    return __FUNCSIG__;
#else
    return __PRETTY_FUNCTION__;
#endif
}

constexpr bool startsWith(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

// Fully qualified type name, identical across DLLs and runs. This is the stable
// identity of a component/resource/event: the same text in the engine and in a
// reloaded game module resolves to the same storage in the World.
template <class T>
constexpr std::string_view typeName() {
    std::string_view s = rawTypeName<T>();
#if defined(_MSC_VER) && !defined(__clang__)
    // "... __cdecl foh::detail::rawTypeName<struct ns::Foo>(void)"
    constexpr std::string_view kBegin = "rawTypeName<";
    const size_t b = s.find(kBegin);
    s = s.substr(b + kBegin.size());
    s = s.substr(0, s.rfind(">(void)"));
    for (std::string_view prefix : {"struct ", "class ", "enum ", "union "}) {
        if (startsWith(s, prefix)) {
            s.remove_prefix(prefix.size());
            break;
        }
    }
#else
    // clang: "... rawTypeName() [T = ns::Foo]"   gcc: "... rawTypeName() [with T = ns::Foo; ...]"
    constexpr std::string_view kBegin = "T = ";
    const size_t b = s.find(kBegin);
    s = s.substr(b + kBegin.size());
    s = s.substr(0, s.find_first_of(";]"));
#endif
    return s;
}

constexpr u64 fnv1a(std::string_view s) {
    u64 h = 14695981039346656037ull;
    for (char c : s) {
        h ^= static_cast<u8>(c);
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace foh::detail
