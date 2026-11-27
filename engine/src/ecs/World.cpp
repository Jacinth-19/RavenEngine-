#include "raven/ecs/World.h"
#include <algorithm>

namespace raven::ecs {

Entity World::CreateEntity() {
    uint32_t index;
    if (!m_FreeList.empty()) {
        index = m_FreeList.back();
        m_FreeList.pop_back();
    } else {
        index = static_cast<uint32_t>(m_Generations.size());
        m_Generations.push_back(0);
        m_SlotAlive.push_back(0);
    }
    m_SlotAlive[index] = 1;
    Entity e = MakeEntity(index, m_Generations[index]);
    m_AliveEntities.push_back(e);
    ++m_AliveCount;
    return e;
}

void World::DestroyEntity(Entity entity) {
    uint32_t index = EntityIndex(entity);
    if (index >= m_SlotAlive.size() || !m_SlotAlive[index]) return;
    if (m_Generations[index] != EntityGeneration(entity)) return;  // stale handle

    for (auto& [type, pool] : m_Pools) pool->RemoveEntity(entity);

    m_SlotAlive[index] = 0;
    m_Generations[index] = (m_Generations[index] + 1) & kGenerationMask;
    m_FreeList.push_back(index);
    auto it = std::find(m_AliveEntities.begin(), m_AliveEntities.end(), entity);
    if (it != m_AliveEntities.end()) m_AliveEntities.erase(it);
    --m_AliveCount;
}

bool World::IsAlive(Entity entity) const {
    uint32_t index = EntityIndex(entity);
    return index < m_SlotAlive.size() && m_SlotAlive[index] &&
           m_Generations[index] == EntityGeneration(entity);
}

void World::Clear() {
    m_Pools.clear();
    m_Generations.clear();
    m_SlotAlive.clear();
    m_FreeList.clear();
    m_AliveEntities.clear();
    m_AliveCount = 0;
}

} // namespace raven::ecs
