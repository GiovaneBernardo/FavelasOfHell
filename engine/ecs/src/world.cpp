#include "gtabr/ecs.h"

namespace gtabr {

Entity World::spawn() {
    ++m_aliveCount;
    if (!m_freeList.empty()) {
        const u32 index = m_freeList.back();
        m_freeList.pop_back();
        return {index, m_generations[index]};
    }
    m_generations.push_back(0);
    return {static_cast<u32>(m_generations.size() - 1), 0};
}

bool World::isAlive(Entity entity) const {
    return entity.index < m_generations.size() && m_generations[entity.index] == entity.generation;
}

bool World::despawn(Entity entity) {
    if (!isAlive(entity)) return false;
    ++m_generations[entity.index];
    m_freeList.push_back(entity.index);
    --m_aliveCount;
    return true;
}

} // namespace gtabr
