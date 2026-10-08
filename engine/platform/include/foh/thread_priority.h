#pragma once

#include "foh/platform.h"

namespace foh {

// Lowers (true) or restores (false) the OS scheduling priority of the CALLING thread. The job system uses it around
// low priority jobs (imports, asset decoding): busy background threads at normal priority make the OS scheduler delay
// the threads that run frame work by whole time slices (100+ ms hitches), even when plenty of cores are nominally free.
// No-op where the platform has no suitable call.
FOH_API void SetCurrentThreadBackground(bool background);

} // namespace foh
