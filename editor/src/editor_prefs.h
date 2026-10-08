#pragma once

// Small persistent key/value store for editor preferences (browser zoom, panel widths, API tokens).
// A plain "key=value" text file next to the executable; unknown keys are kept. Not for project data.
//
// Thread-safety: main thread only.

#include <filesystem>
#include <map>
#include <string>

namespace foh {

class EditorPrefs {
public:
    void load(const std::filesystem::path& file);
    void save() const;
    // Saves at most once per second; call every frame after changing values.
    void saveIfDirty(double now);

    std::string get(const std::string& key, const std::string& fallback = {}) const;
    float getFloat(const std::string& key, float fallback) const;
    bool getBool(const std::string& key, bool fallback) const;
    void set(const std::string& key, const std::string& value);
    void setFloat(const std::string& key, float value);
    void setBool(const std::string& key, bool value);

private:
    std::filesystem::path m_file;
    std::map<std::string, std::string> m_values;
    bool m_dirty = false;
    double m_lastSave = 0.0;
};

} // namespace foh
