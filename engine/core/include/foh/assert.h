#pragma once

#include "foh/platform.h"

namespace foh {
FOH_API void ReportAssertFailure(const char* expr, const char* file, int line, const char* message);
}

// Always-on check for invariants that must hold even in release builds.
#define FOH_VERIFY(cond, msg)                                                   \
    do {                                                                          \
        if (!(cond)) {                                                            \
            ::foh::ReportAssertFailure(#cond, __FILE__, __LINE__, msg);         \
            FOH_DEBUG_BREAK();                                                  \
        }                                                                         \
    } while (false)

// Debug-only check.
#if defined(NDEBUG)
    #define FOH_ASSERT(cond, msg) ((void)0)
#else
    #define FOH_ASSERT(cond, msg) FOH_VERIFY(cond, msg)
#endif
