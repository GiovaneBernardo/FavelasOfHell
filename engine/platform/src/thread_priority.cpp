#include "foh/thread_priority.h"

#if defined(FOH_PLATFORM_WINDOWS)
    #include <windows.h>
#elif defined(FOH_PLATFORM_LINUX)
    #include <sys/resource.h>
    #include <sys/syscall.h>
    #include <unistd.h>
#endif

namespace foh {

void SetCurrentThreadBackground(bool background) {
#if defined(FOH_PLATFORM_WINDOWS)
    // BELOW_NORMAL rather than THREAD_MODE_BACKGROUND_BEGIN: the latter also throttles I/O and memory priority so hard that
    // an import would crawl.
    SetThreadPriority(GetCurrentThread(), background ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL);
#elif defined(FOH_PLATFORM_LINUX)
    // On Linux nice values are per thread.
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), background ? 10 : 0);
#else
    (void)background;
#endif
}

} // namespace foh
