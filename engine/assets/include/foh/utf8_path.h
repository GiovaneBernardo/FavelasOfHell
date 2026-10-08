#pragma once

// Paths travel as UTF-8 strings in asset files and the editor, and as fs::path in file APIs.
// (std::filesystem::u8path is deprecated in C++20, and path::string() is lossy on Windows.)

#include <filesystem>
#include <string>
#include <string_view>

namespace foh {

inline std::filesystem::path PathFromUtf8(std::string_view s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

// Generic ('/') separators.
inline std::string PathToUtf8(const std::filesystem::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

} // namespace foh
