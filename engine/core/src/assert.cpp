#include "foh/assert.h"

#include "foh/log.h"

namespace foh {

void ReportAssertFailure(const char* expr, const char* file, int line, const char* message) {
    FOH_FATAL("Assertion failed: {} ({}) at {}:{}", message, expr, file, line);
}

} // namespace foh
