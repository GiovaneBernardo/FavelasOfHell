#include "foh/file_system.h"

#include <string>

#if defined(_WIN32)
    #include <windows.h>
#elif defined(__linux__)
    #include <unistd.h>
#endif

namespace foh {

std::filesystem::path GetExecutableDirectory() {
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const DWORD len = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(len);
    return std::filesystem::path(buffer).parent_path();
#elif defined(__linux__)
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
#else
    return std::filesystem::current_path();
#endif
}

} // namespace foh
