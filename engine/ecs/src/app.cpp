#include "gtabr/ecs.h"

#include <algorithm>

namespace gtabr {

void App::addSystem(Stage stage, SystemFn fn, const char* name) {
    m_systems[static_cast<size_t>(stage)].push_back({fn, name, m_activeOwner});
}

void App::removeSystemsOwnedBy(OwnerId owner) {
    for (auto& stage : m_systems) {
        std::erase_if(stage, [owner](const SystemEntry& s) { return s.owner == owner; });
    }
}

void App::update(f32 deltaSeconds) {
    m_time.delta = deltaSeconds;
    m_time.elapsed += deltaSeconds;
    ++m_time.frame;
    SystemContext ctx{m_world, m_time};
    for (auto& stage : m_systems) {
        for (const SystemEntry& system : stage) system.fn(ctx);
    }
}

size_t App::systemCount() const {
    size_t n = 0;
    for (const auto& stage : m_systems) n += stage.size();
    return n;
}

} // namespace gtabr
