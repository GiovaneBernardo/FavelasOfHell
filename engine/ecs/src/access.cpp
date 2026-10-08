#include "gtabr/ecs/access.h"

namespace gtabr {

namespace {
bool intersects(const std::vector<ComponentId>& a, const std::vector<ComponentId>& b) {
    for (ComponentId x : a) {
        if (std::find(b.begin(), b.end(), x) != b.end()) return true;
    }
    return false;
}
} // namespace

bool AccessSet::conflictsWith(const AccessSet& other) const {
    if (exclusive || other.exclusive) return true;
    // read/read never conflicts; anything involving a write does.
    return intersects(writes, other.writes) || intersects(writes, other.reads) || intersects(reads, other.writes);
}

} // namespace gtabr
