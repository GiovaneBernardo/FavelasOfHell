#include "gtabr/assert.h"

#include "gtabr/log.h"

namespace gtabr {

void ReportAssertFailure(const char* expr, const char* file, int line, const char* message) {
    GTABR_FATAL("Assertion failed: {} ({}) at {}:{}", message, expr, file, line);
}

} // namespace gtabr
