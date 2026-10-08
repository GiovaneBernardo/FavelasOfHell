#include "editor_prefs.h"

#include <cstdlib>
#include <fstream>

namespace foh {

void EditorPrefs::load(const std::filesystem::path& file) {
    m_file = file;
    m_values.clear();
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        m_values[line.substr(0, eq)] = line.substr(eq + 1);
    }
    m_dirty = false;
}

void EditorPrefs::save() const {
    if (m_file.empty()) return;
    std::ofstream out(m_file, std::ios::trunc);
    for (const auto& [k, v] : m_values) out << k << '=' << v << '\n';
}

void EditorPrefs::saveIfDirty(double now) {
    if (!m_dirty || now - m_lastSave < 1.0) return;
    save();
    m_dirty = false;
    m_lastSave = now;
}

std::string EditorPrefs::get(const std::string& key, const std::string& fallback) const {
    auto it = m_values.find(key);
    return it == m_values.end() ? fallback : it->second;
}

float EditorPrefs::getFloat(const std::string& key, float fallback) const {
    auto it = m_values.find(key);
    if (it == m_values.end()) return fallback;
    char* end = nullptr;
    const float v = std::strtof(it->second.c_str(), &end);
    return end == it->second.c_str() ? fallback : v;
}

bool EditorPrefs::getBool(const std::string& key, bool fallback) const {
    auto it = m_values.find(key);
    return it == m_values.end() ? fallback : it->second == "1";
}

void EditorPrefs::set(const std::string& key, const std::string& value) {
    auto it = m_values.find(key);
    if (it != m_values.end() && it->second == value) return;
    m_values[key] = value;
    m_dirty = true;
}

void EditorPrefs::setFloat(const std::string& key, float value) { set(key, std::to_string(value)); }
void EditorPrefs::setBool(const std::string& key, bool value) { set(key, value ? "1" : "0"); }

} // namespace foh
