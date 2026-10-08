#include "foh/shell.h"

#include <mutex>

#include <SDL3/SDL_dialog.h>

#if defined(_WIN32)
    #include <windows.h>
    #include <shellapi.h>
#else
    #include <cstdlib>
#endif

namespace foh {

namespace {

struct DialogState {
    std::mutex mutex;
    bool ready = false;
    std::vector<std::string> files;
    // SDL keeps the filter strings alive only for the duration of the call; the dialog copies them.
};

DialogState& dialogState() {
    static DialogState state;
    return state;
}

void SDLCALL onDialogDone(void*, const char* const* list, int) {
    DialogState& s = dialogState();
    std::lock_guard lock(s.mutex);
    s.files.clear();
    if (list) {
        for (; *list; ++list) s.files.emplace_back(*list);
    }
    s.ready = true;
}

#if defined(_WIN32)
std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
#endif

} // namespace

bool RevealInFileManager(const std::filesystem::path& path) {
#if defined(_WIN32)
    std::error_code ec;
    const bool dir = std::filesystem::is_directory(path, ec);
    const std::wstring native = std::filesystem::absolute(path, ec).make_preferred().wstring();
    const std::wstring args = dir ? L"\"" + native + L"\"" : L"/select,\"" + native + L"\"";
    return reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL)) > 32;
#else
    std::error_code ec;
    const auto folder = std::filesystem::is_directory(path, ec) ? path : path.parent_path();
    #if defined(__APPLE__)
    const std::string cmd = "open \"" + folder.string() + "\" &";
    #else
    const std::string cmd = "xdg-open \"" + folder.string() + "\" >/dev/null 2>&1 &";
    #endif
    return std::system(cmd.c_str()) == 0;
#endif
}

bool OpenWithDefaultApp(const std::filesystem::path& path) {
#if defined(_WIN32)
    return reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#else
    #if defined(__APPLE__)
    const std::string cmd = "open \"" + path.string() + "\" &";
    #else
    const std::string cmd = "xdg-open \"" + path.string() + "\" >/dev/null 2>&1 &";
    #endif
    return std::system(cmd.c_str()) == 0;
#endif
}

bool OpenUrl(const std::string& url) {
    // Only web links: this must never become a way to launch arbitrary files from data fetched online.
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) return false;
    if (url.find_first_of("\"\r\n ") != std::string::npos) return false;
#if defined(_WIN32)
    return reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, L"open", toWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#else
    #if defined(__APPLE__)
    const std::string cmd = "open \"" + url + "\" &";
    #else
    const std::string cmd = "xdg-open \"" + url + "\" >/dev/null 2>&1 &";
    #endif
    return std::system(cmd.c_str()) == 0;
#endif
}

void ShowOpenFilesDialog(const std::string& filterName, const std::string& patterns, const std::filesystem::path& startFolder) {
    SDL_DialogFileFilter filter{filterName.c_str(), patterns.c_str()};
    const std::string start = startFolder.empty() ? std::string() : startFolder.string();
    SDL_ShowOpenFileDialog(&onDialogDone, nullptr, nullptr, &filter, 1, start.empty() ? nullptr : start.c_str(), true);
}

bool TakeOpenFilesResult(std::vector<std::string>& files) {
    DialogState& s = dialogState();
    std::lock_guard lock(s.mutex);
    if (!s.ready) return false;
    files = std::move(s.files);
    s.files.clear();
    s.ready = false;
    return true;
}

} // namespace foh
