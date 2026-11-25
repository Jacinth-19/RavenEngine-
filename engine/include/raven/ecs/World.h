// RavenEngine — entity + sparse-set component pool + world registry.
#pragma once

#include <cstdint>
#include <typeindex>
#include <unordered_map>
#include <vector>
#include <memory>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace raven::ecs {

// Entity is a 32-bit handle: low 20 bits = index into the slot table,
// high 12 bits = generation (bumped on destroy so stale handles are detected).
using Entity = uint32_t;
inline constexpr Entity NullEntity = std::numeric_limits<Entity>::max();
inline constexpr uint32_t kIndexBits = 20;
inline constexpr uint32_t kGenerationBits = 12;
inline constexpr uint32_t kIndexMask = (1u << kIndexBits) - 1u;
inline constexpr uint32_t kGenerationMask = (1u << kGenerationBits) - 1u;

inline constexpr Entity MakeEntity(uint32_t index, uint32_t generation) {
    return ((generation & kGenerationMask) << kIndexBits) | (index & kIndexMask);
}
inline constexpr uint32_t EntityIndex(Entity e) { return e & kIndexMask; }
inline constexpr uint32_t EntityGeneration(Entity e) { return (e >> kIndexBits) & kGenerationMask; }

class IComponentPool {
public:
    virtual ~IComponentPool() = default;
    virtual void RemoveEntity(Entity entity) = 0;
    virtual size_t Size() const = 0;
};

template <typename T>
class ComponentPool final : public IComponentPool {
public:
    template <typename... Args>
    T& Emplace(Entity entity, Args&&... args) {
        uint32_t index = EntityIndex(entity);
        EnsureSparse(index);
        if (m_Sparse[index] != kInvalid) {
            T& existing = m_Dense[m_Sparse[index]];
            existing = T(std::forward<Args>(args)...);
            return existing;
        }
        m_Dense.emplace_back(std::forward<Args>(args)...);
        m_DenseToEntity.push_back(entity);
        m_Sparse[index] = static_cast<uint32_t>(m_Dense.size() - 1);
        return m_Dense.back();
    }

    T& Get(Entity entity) { return m_Dense[SparseIndex(entity)]; }
    const T& Get(Entity entity) const { return m_Dense[SparseIndex(entity)]; }

    bool Has(Entity entity) const {
        uint32_t index = EntityIndex(entity);
        return index < m_Sparse.size() && m_Sparse[index] != kInvalid;
    }

    void RemoveEntity(Entity entity) override {
        uint32_t index = EntityIndex(entity);
        if (index >= m_Sparse.size() || m_Sparse[index] == kInvalid) return;
        uint32_t denseIndex = m_Sparse[index];
        uint32_t last = static_cast<uint32_t>(m_Dense.size() - 1);
        if (denseIndex != last) {
            m_Dense[denseIndex] = std::move(m_Dense[last]);
            m_DenseToEntity[denseIndex] = m_DenseToEntity[last];
            m_Sparse[EntityIndex(m_DenseToEntity[denseIndex])] = denseIndex;
        }
        m_Dense.pop_back();
        m_DenseToEntity.pop_back();
        m_Sparse[index] = kInvalid;
    }

    size_t Size() const override { return m_Dense.size(); }

    std::vector<T>& Dense() { return m_Dense; }
    const std::vector<T>& Dense() const { return m_Dense; }
    const std::vector<Entity>& DenseToEntity() const { return m_DenseToEntity; }

private:
    static constexpr uint32_t kInvalid = std::numeric_limits<uint32_t>::max();

    uint32_t SparseIndex(Entity entity) const {
        uint32_t index = EntityIndex(entity);
        if (index >= m_Sparse.size() || m_Sparse[index] == kInvalid)
            throw std::out_of_range("component pool: entity has no such component");
        return m_Sparse[index];
    }
    void EnsureSparse(uint32_t index) {
        if (m_Sparse.size() <= index) m_Sparse.resize(index + 1, kInvalid);  // grow preserving existing entries
    }

    std::vector<T> m_Dense;
    std::vector<Entity> m_DenseToEntity;
    std::vector<uint32_t> m_Sparse;
};

class World {
public:
    World() = default;
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    Entity CreateEntity();
    void DestroyEntity(Entity entity);          // removes all components too
    bool IsAlive(Entity entity) const;
    size_t EntityCount() const { return m_AliveCount; }

    const std::vector<Entity>& Entities() const { return m_AliveEntities; }

    template <typename T, typename... Args>
    T& AddComponent(Entity entity, Args&&... args) {
        if (!IsAlive(entity)) throw std::out_of_range("AddComponent on dead entity");
        return Pool<T>().Emplace(entity, std::forward<Args>(args)...);
    }
    template <typename T> T& GetComponent(Entity entity) { return Pool<T>().Get(entity); }
    template <typename T> const T& GetComponent(Entity entity) const {
        auto it = m_Pools.find(pool_key<T>());
        if (it == m_Pools.end()) throw std::out_of_range("component pool: entity has no such component");
        return static_cast<const ComponentPool<T>*>(it->second.get())->Get(entity);
    }
    template <typename T> bool HasComponent(Entity entity) const {
        auto it = m_Pools.find(pool_key<T>());
        return it != m_Pools.end() &&
               static_cast<const ComponentPool<T>*>(it->second.get())->Has(entity);
    }
    template <typename T> void RemoveComponent(Entity entity) {
        auto it = m_Pools.find(pool_key<T>());
        if (it != m_Pools.end()) it->second->RemoveEntity(entity);
    }
    template <typename T> size_t ComponentCount() const {
        auto it = m_Pools.find(pool_key<T>());
        return it == m_Pools.end() ? 0 : it->second->Size();
    }

    // Single-component iteration: ForEach(fn(entity, component)). Mutable.
    template <typename T, typename F>
    void ForEach(F&& fn) {
        auto it = m_Pools.find(pool_key<T>());
        if (it == m_Pools.end()) return;
        auto* pool = static_cast<ComponentPool<T>*>(it->second.get());
        const auto& entities = pool->DenseToEntity();
        auto& dense = pool->Dense();
        for (size_t i = 0; i < entities.size(); ++i) fn(entities[i], dense[i]);
    }

    void Clear();

private:
    template <typename T> ComponentPool<T>& Pool() {
        auto key = pool_key<T>();
        auto it = m_Pools.find(key);
        if (it == m_Pools.end()) {
            auto pool = std::make_unique<ComponentPool<T>>();
            it = m_Pools.emplace(key, std::move(pool)).first;
        }
        return *static_cast<ComponentPool<T>*>(it->second.get());
    }
    template <typename T> static std::type_index pool_key() { return std::type_index(typeid(T)); }

    std::vector<uint32_t> m_Generations;
    std::vector<char> m_SlotAlive;
    std::vector<uint32_t> m_FreeList;
    std::vector<Entity> m_AliveEntities;
    std::unordered_map<std::type_index, std::unique_ptr<IComponentPool>> m_Pools;
    size_t m_AliveCount = 0;
};

} // namespace raven::ecs
