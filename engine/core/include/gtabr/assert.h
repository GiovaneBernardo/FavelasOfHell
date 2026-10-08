#pragma once

#include "gtabr/platform.h"

namespace gtabr {
GTABR_API void ReportAssertFailure(const char* expr, const char* file, int line, const char* message);
}

// Always-on check for invariants that must hold even in release builds.
#define GTABR_VERIFY(cond, msg)                                                   \
    do {                                                                          \
        if (!(cond)) {                                                            \
            ::gtabr::ReportAssertFailure(#cond, __FILE__, __LINE__, msg);         \
            GTABR_DEBUG_BREAK();                                                  \
        }                                                                         \
    } while (false)

// Debug-only check.
#if defined(NDEBUG)
    #define GTABR_ASSERT(cond, msg) ((void)0)
#else
    #define GTABR_ASSERT(cond, msg) GTABR_VERIFY(cond, msg)
#endif
