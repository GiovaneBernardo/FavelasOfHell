#include "gtabr/dynamic_library.h"

#include <utility>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

namespace gtabr {

DynamicLibrary::~DynamicLibrary() { unload(); }

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
    : m_handle(std::exchange(other.m_handle, nullptr)) {}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
    if (this != &other) {
        unload();
        m_handle = std::exchange(other.m_handle, nullptr);
    }
    return *this;
}

bool DynamicLibrary::load(const std::filesystem::path& path) {
    unload();
#if defined(_WIN32)
    m_handle = ::LoadLibraryW(path.c_str());
#else
    m_handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    return m_handle != nullptr;
}

void DynamicLibrary::unload() {
    if (!m_handle) return;
#if defined(_WIN32)
    ::FreeLibrary(static_cast<HMODULE>(m_handle));
#else
    ::dlclose(m_handle);
#endif
    m_handle = nullptr;
}

void* DynamicLibrary::symbol(const char* name) const {
    if (!m_handle) return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(m_handle), name));
#else
    return ::dlsym(m_handle, name);
#endif
}

} // namespace gtabr
