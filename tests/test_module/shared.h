#pragma once

#include "foh/ecs/reflect.h"

// Types shared by the test executable and the reloadable test module. They are
// the same C++ types in both DLLs, so they resolve to the same world storage by
// name, which is exactly what hot reload relies on.

namespace reload_test {

struct Ticks { int value = 0; };     // resource
struct Tracked { int seen = 0; };    // component
struct Ping { int tick = 0; };       // event
struct Observed { int count = 0; };  // resource written by an observer

} // namespace reload_test

FOH_REFLECT(reload_test::Ticks, value);
FOH_REFLECT(reload_test::Tracked, seen);
FOH_REFLECT(reload_test::Observed, count);
