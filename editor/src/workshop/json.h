#pragma once

// A small JSON reader for the web APIs the workshop talks to. Read-only, tolerant lookups
// (a missing key yields a null value instead of an error) and bounded nesting, because the
// input comes from the internet and must never be trusted to be well formed.
//
// Thread-safety: plain data; parsing is pure.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace foh {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Type::Null; }
    bool isString() const { return m_type == Type::String; }
    bool isNumber() const { return m_type == Type::Number; }
    bool isArray() const { return m_type == Type::Array; }
    bool isObject() const { return m_type == Type::Object; }

    // Missing key / wrong type: the null value. Chained lookups are therefore always safe.
    const Json& operator[](std::string_view key) const;
    const Json& operator[](const char* key) const { return (*this)[std::string_view(key)]; }
    const Json& operator[](size_t index) const;
    size_t size() const { return m_type == Type::Array ? m_array.size() : m_type == Type::Object ? m_object.size() : 0; }

    std::string str(const std::string& fallback = {}) const { return m_type == Type::String ? m_string : fallback; }
    double num(double fallback = 0.0) const { return m_type == Type::Number ? m_number : fallback; }
    bool boolean(bool fallback = false) const { return m_type == Type::Bool ? m_bool : fallback; }

    // Object members in document order / array elements.
    const std::vector<std::pair<std::string, Json>>& members() const { return m_object; }
    const std::vector<Json>& items() const { return m_array; }

    static bool Parse(std::string_view text, Json& out, std::string* error = nullptr);

private:
    friend class JsonParser;
    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0.0;
    std::string m_string;
    std::vector<Json> m_array;
    std::vector<std::pair<std::string, Json>> m_object;
};

} // namespace foh
