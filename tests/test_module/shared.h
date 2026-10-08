#pragma once

// Types shared by the test executable and the reloadable test module. They are
// the same C++ types in both DLLs, so they resolve to the same world storage by
// name, which is exactly what hot reload relies on.

namespace reload_test {

struct Ticks { int value = 0; };     // resource
struct Tracked { int seen = 0; };    // component
struct Ping { int tick = 0; };       // event

} // namespace reload_test
