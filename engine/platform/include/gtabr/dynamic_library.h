#pragma once

#include <filesystem>

#include "gtabr/platform.h"

namespace gtabr {

// Thin wrapper over LoadLibrary/dlopen. Move-only; unloads on destruction.
class GTABR_API DynamicLibrary {
public:
    DynamicLibrary() = default;
    ~DynamicLibrary();
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

    bool load(const std::filesystem::path& path);
    void unload();
    bool isLoaded() const { return m_handle != nullptr; }
    void* symbol(const char* name) const;

private:
    void* m_handle = nullptr;
};

} // namespace gtabr
