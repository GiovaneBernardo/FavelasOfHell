#include "json.h"

#include <cstdlib>
#include <cstring>

namespace foh {

namespace {
const Json kNull;
constexpr int kMaxDepth = 48;
} // namespace

const Json& Json::operator[](std::string_view key) const {
    if (m_type != Type::Object) return kNull;
    for (const auto& [k, v] : m_object)
        if (k == key) return v;
    return kNull;
}

const Json& Json::operator[](size_t index) const {
    return m_type == Type::Array && index < m_array.size() ? m_array[index] : kNull;
}

class JsonParser {
public:
    JsonParser(std::string_view text, std::string* error) : m_s(text), m_error(error) {}

    bool run(Json& out) {
        skip();
        if (!value(out, 0)) return false;
        skip();
        if (m_pos != m_s.size()) return fail("trailing characters");
        return true;
    }

private:
    bool fail(const char* msg) {
        if (m_error && m_error->empty()) *m_error = std::string(msg) + " at byte " + std::to_string(m_pos);
        return false;
    }
    void skip() {
        while (m_pos < m_s.size() && (m_s[m_pos] == ' ' || m_s[m_pos] == '\t' || m_s[m_pos] == '\n' || m_s[m_pos] == '\r')) ++m_pos;
    }
    bool literal(std::string_view word) {
        if (m_s.substr(m_pos, word.size()) != word) return false;
        m_pos += word.size();
        return true;
    }

    bool value(Json& out, int depth) {
        if (depth > kMaxDepth) return fail("nesting too deep");
        if (m_pos >= m_s.size()) return fail("unexpected end");
        const char c = m_s[m_pos];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') {
            out.m_type = Json::Type::String;
            return string(out.m_string);
        }
        if (c == 't') {
            out.m_type = Json::Type::Bool;
            out.m_bool = true;
            return literal("true") || fail("bad literal");
        }
        if (c == 'f') {
            out.m_type = Json::Type::Bool;
            out.m_bool = false;
            return literal("false") || fail("bad literal");
        }
        if (c == 'n') {
            out.m_type = Json::Type::Null;
            return literal("null") || fail("bad literal");
        }
        return number(out);
    }

    bool number(Json& out) {
        const size_t start = m_pos;
        while (m_pos < m_s.size() && (std::strchr("+-.eE", m_s[m_pos]) || (m_s[m_pos] >= '0' && m_s[m_pos] <= '9'))) ++m_pos;
        if (start == m_pos) return fail("unexpected character");
        const std::string text(m_s.substr(start, m_pos - start));
        char* end = nullptr;
        out.m_number = std::strtod(text.c_str(), &end);
        out.m_type = Json::Type::Number;
        return end == text.c_str() + text.size() || fail("bad number");
    }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned& v) {
        if (m_pos + 4 > m_s.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = m_s[m_pos++];
            v <<= 4;
            if (h >= '0' && h <= '9') v |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') v |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v |= static_cast<unsigned>(h - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool string(std::string& out) {
        ++m_pos; // opening quote
        out.clear();
        while (m_pos < m_s.size()) {
            const char c = m_s[m_pos++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (m_pos >= m_s.size()) break;
            const char e = m_s[m_pos++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) return fail("bad \\u escape");
                    if (cp >= 0xD800 && cp < 0xDC00 && m_pos + 1 < m_s.size() && m_s[m_pos] == '\\' && m_s[m_pos + 1] == 'u') {
                        m_pos += 2;
                        unsigned lo = 0;
                        if (!hex4(lo)) return fail("bad \\u escape");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    bool array(Json& out, int depth) {
        out.m_type = Json::Type::Array;
        ++m_pos;
        skip();
        if (m_pos < m_s.size() && m_s[m_pos] == ']') {
            ++m_pos;
            return true;
        }
        while (true) {
            out.m_array.emplace_back();
            skip();
            if (!value(out.m_array.back(), depth + 1)) return false;
            skip();
            if (m_pos >= m_s.size()) return fail("unterminated array");
            if (m_s[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            if (m_s[m_pos] == ']') {
                ++m_pos;
                return true;
            }
            return fail("expected , or ]");
        }
    }

    bool object(Json& out, int depth) {
        out.m_type = Json::Type::Object;
        ++m_pos;
        skip();
        if (m_pos < m_s.size() && m_s[m_pos] == '}') {
            ++m_pos;
            return true;
        }
        while (true) {
            skip();
            if (m_pos >= m_s.size() || m_s[m_pos] != '"') return fail("expected key");
            std::string key;
            if (!string(key)) return false;
            skip();
            if (m_pos >= m_s.size() || m_s[m_pos] != ':') return fail("expected :");
            ++m_pos;
            skip();
            out.m_object.emplace_back(std::move(key), Json());
            if (!value(out.m_object.back().second, depth + 1)) return false;
            skip();
            if (m_pos >= m_s.size()) return fail("unterminated object");
            if (m_s[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            if (m_s[m_pos] == '}') {
                ++m_pos;
                return true;
            }
            return fail("expected , or }");
        }
    }

    std::string_view m_s;
    size_t m_pos = 0;
    std::string* m_error;
};

bool Json::Parse(std::string_view text, Json& out, std::string* error) {
    out = Json();
    if (error) error->clear();
    JsonParser parser(text, error);
    return parser.run(out);
}

} // namespace foh
