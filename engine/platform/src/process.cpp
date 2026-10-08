#include "foh/process.h"

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <spawn.h>
    #include <sys/wait.h>
    #include <unistd.h>
extern char** environ;
#endif

namespace foh {

Process::~Process() { closeHandle(); }

void Process::closeHandle() {
#if defined(_WIN32)
    if (m_handle) ::CloseHandle(static_cast<HANDLE>(m_handle));
#endif
    m_handle = nullptr;
}

#if defined(_WIN32)

namespace {
std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
} // namespace

bool Process::start(const std::string& commandLine, const std::filesystem::path& workingDir,
                    const std::filesystem::path& logFile) {
    closeHandle();
    m_started = false;
    m_exitCode = -1;

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE log = ::CreateFileW(logFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) return false;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = log;
    si.hStdError = log;
    si.hStdInput = INVALID_HANDLE_VALUE;

    std::wstring cmd = L"cmd.exe /d /c \"" + ToWide(commandLine) + L"\"";
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                     workingDir.empty() ? nullptr : workingDir.c_str(), &si, &pi);
    ::CloseHandle(log); // the child holds its own copy
    if (!ok) return false;
    ::CloseHandle(pi.hThread);
    m_handle = pi.hProcess;
    m_started = true;
    return true;
}

bool Process::isRunning() {
    if (!m_started || !m_handle) return false;
    if (::WaitForSingleObject(static_cast<HANDLE>(m_handle), 0) == WAIT_TIMEOUT) return true;
    DWORD code = 0;
    ::GetExitCodeProcess(static_cast<HANDLE>(m_handle), &code);
    m_exitCode = static_cast<int>(code);
    m_started = false;
    closeHandle();
    return false;
}

#else

bool Process::start(const std::string& commandLine, const std::filesystem::path& workingDir,
                    const std::filesystem::path& logFile) {
    m_started = false;
    m_exitCode = -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 1, logFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
    const std::string full =
        workingDir.empty() ? commandLine : "cd '" + workingDir.string() + "' && " + commandLine;
    char* argv[] = {const_cast<char*>("sh"), const_cast<char*>("-c"), const_cast<char*>(full.c_str()), nullptr};
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, "/bin/sh", &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) return false;
    m_pid = pid;
    m_started = true;
    return true;
}

bool Process::isRunning() {
    if (!m_started) return false;
    int status = 0;
    const pid_t r = ::waitpid(static_cast<pid_t>(m_pid), &status, WNOHANG);
    if (r == 0) return true;
    m_exitCode = (r > 0 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    m_started = false;
    return false;
}

#endif

} // namespace foh
