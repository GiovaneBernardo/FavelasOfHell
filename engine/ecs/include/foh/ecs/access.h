#pragma once

#include <algorithm>
#include <vector>

#include "foh/ecs/types.h"
#include "foh/platform.h"

namespace foh {

// What a system touches. The scheduler derives all ordering and parallelism from
// these sets: two systems may run at the same time only if neither writes
// something the other reads or writes. Components and resources share one
// namespace (a resource is a component on a singleton).
struct AccessSet {
    std::vector<ComponentId> reads;
    std::vector<ComponentId> writes;
    // Needs `World&`: runs alone, on the main thread, outside the parallel phase.
    bool exclusive = false;

    void read(ComponentId id) { addUnique(reads, id); }
    void write(ComponentId id) { addUnique(writes, id); }

    FOH_API bool conflictsWith(const AccessSet& other) const;

private:
    static void addUnique(std::vector<ComponentId>& v, ComponentId id) {
        if (std::find(v.begin(), v.end(), id) == v.end()) v.push_back(id);
    }
};

} // namespace foh
