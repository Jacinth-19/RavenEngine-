#include "raven/physics/PhysicsWorld.h"

#include "raven/scene/Scene.h"
#include "raven/scene/Components.h"
#include "raven/core/Logger.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace raven {

void PhysicsWorld::Step(Scene& scene, float dt) {
    if (dt <= 0.0f) return;
    dt = std::min(dt, 1.0f / 15.0f);  // clamp huge frame hitches

    auto& world = scene.World();
    const float sleepSpeedSq = m_Config.sleepLinearVelocity * m_Config.sleepLinearVelocity;
    // Wake threshold with hysteresis so a body wobbling at the sleep boundary doesn't flap.
    const float wakeSpeedSq = 4.0f * sleepSpeedSq;

    // Integrate gravity + velocity for dynamic, awake bodies.
    world.ForEach<RigidbodyComponent>([&](ecs::Entity e, RigidbodyComponent& rb) {
        if (rb.type != RigidbodyType::Dynamic) return;
        if (!world.HasComponent<TransformComponent>(e)) return;

        const float speedSq = glm::dot(rb.linearVelocity, rb.linearVelocity);
        if (!rb.isAwake) {
            if (speedSq > wakeSpeedSq) {
                rb.isAwake = true;   // externally disturbed — wake it
                rb.sleepTimer = 0.0f;
            } else {
                return;              // asleep: no integration, no gravity pull
            }
        }

        auto& transform = world.GetComponent<TransformComponent>(e);

        if (rb.useGravity) rb.linearVelocity += m_Config.gravity * dt;
        rb.linearVelocity *= (1.0f - m_Config.linearDamping * dt);
        transform.position += rb.linearVelocity * dt;

        // Ground-plane collision with restitution.
        float halfHeight = 0.5f;
        bool hasCollider = world.HasComponent<BoxColliderComponent>(e);
        if (hasCollider) halfHeight = world.GetComponent<BoxColliderComponent>(e).halfExtents.y * std::abs(transform.scale.y);

        float floor = m_Config.groundHeight + halfHeight;
        if (transform.position.y < floor) {
            transform.position.y = floor;
            if (rb.linearVelocity.y < 0.0f) {
                float impact = -rb.linearVelocity.y;
                rb.linearVelocity.y = impact > 0.5f ? impact * rb.restitution : 0.0f;
                // Ground friction bleeds horizontal velocity.
                rb.linearVelocity.x *= 0.9f;
                rb.linearVelocity.z *= 0.9f;
            }
        }
    });

    // Pairwise sphere response between dynamic bodies (cheap O(n^2); fine for small n).
    // Sleeping dynamics are treated as immovable obstacles: only the awake partner moves.
    if (m_Config.enableSphereResponse) {
        std::vector<ecs::Entity> dynamics;
        world.ForEach<RigidbodyComponent>([&](ecs::Entity e, const RigidbodyComponent& rb) {
            if (rb.type == RigidbodyType::Dynamic && rb.isAwake) dynamics.push_back(e);
        });
        std::vector<ecs::Entity> asleepSet;
        world.ForEach<RigidbodyComponent>([&](ecs::Entity e, const RigidbodyComponent& rb) {
            if (rb.type == RigidbodyType::Dynamic && !rb.isAwake) asleepSet.push_back(e);
        });
        // Awake bodies respond against other awake bodies; asleep bodies are immovable.
        dynamics.insert(dynamics.end(), asleepSet.begin(), asleepSet.end());
        for (size_t i = 0; i < dynamics.size(); ++i) {
            for (size_t j = i + 1; j < dynamics.size(); ++j) {
                ecs::Entity a = dynamics[i], b = dynamics[j];
                auto& ta = world.GetComponent<TransformComponent>(a);
                auto& tb = world.GetComponent<TransformComponent>(b);
                auto& ra = world.GetComponent<RigidbodyComponent>(a);
                auto& rb_ = world.GetComponent<RigidbodyComponent>(b);
                const bool aAwake = ra.isAwake, bAwake = rb_.isAwake;
                if (!aAwake && !bAwake) continue;
                float radx = world.HasComponent<BoxColliderComponent>(a) ? world.GetComponent<BoxColliderComponent>(a).halfExtents.x : 0.5f;
                float rbdx = world.HasComponent<BoxColliderComponent>(b) ? world.GetComponent<BoxColliderComponent>(b).halfExtents.x : 0.5f;
                glm::vec3 delta = tb.position - ta.position;
                float dist = glm::length(delta);
                float minDist = radx + rbdx;
                if (dist > 1e-5f && dist < minDist) {
                    glm::vec3 normal = delta / dist;
                    float penetration = minDist - dist;
                    if (aAwake && bAwake) {
                        ta.position -= normal * (penetration * 0.5f);
                        tb.position += normal * (penetration * 0.5f);
                    } else if (aAwake) {
                        ta.position -= normal * penetration;
                    } else {
                        tb.position += normal * penetration;
                    }
                    float relVel = glm::dot(rb_.linearVelocity - ra.linearVelocity, normal);
                    if (relVel < 0.0f) {
                        if (aAwake && bAwake) {
                            glm::vec3 impulse = normal * (-relVel * 0.5f);
                            ra.linearVelocity -= impulse;
                            rb_.linearVelocity += impulse;
                        } else if (aAwake) {
                            ra.linearVelocity += normal * (-relVel);
                        } else {
                            rb_.linearVelocity -= normal * (-relVel);
                        }
                    }
                }
            }
        }
    }

    // Sleep evaluation for awake bodies: accumulate rest time, freeze when rested long enough.
    world.ForEach<RigidbodyComponent>([&](ecs::Entity e, RigidbodyComponent& rb) {
        if (rb.type != RigidbodyType::Dynamic || !rb.isAwake) return;
        if (glm::dot(rb.linearVelocity, rb.linearVelocity) < sleepSpeedSq) {
            rb.sleepTimer += dt;
            if (rb.sleepTimer >= m_Config.sleepTime) {
                rb.isAwake = false;
                rb.linearVelocity = glm::vec3(0.0f);
                rb.sleepTimer = 0.0f;
            }
        } else {
            rb.sleepTimer = 0.0f;
        }
    });
}

void PhysicsWorld::Advance(Scene& scene, float frameDeltaSeconds) {
    m_FixedTicksLastAdvance = 0;
    if (frameDeltaSeconds <= 0.0f) return;
    const float maxFrame = m_Config.fixedTimestep * static_cast<float>(m_Config.maxSubSteps);
    m_Accumulator += std::min(frameDeltaSeconds, maxFrame);
    const float eps = 1e-6f;
    while (m_Accumulator + eps >= m_Config.fixedTimestep && m_FixedTicksLastAdvance < m_Config.maxSubSteps) {
        Step(scene, m_Config.fixedTimestep);
        m_Accumulator -= m_Config.fixedTimestep;
        ++m_FixedTicksLastAdvance;
    }
    if (m_Accumulator >= m_Config.fixedTimestep) {
        // Hit the catch-up cap: drop the residue rather than spiral the simulation.
        RAVEN_LOG_WARN("PhysicsWorld: dropped %.3f s of unsimulated time (maxSubSteps=%d)",
                       m_Accumulator, m_Config.maxSubSteps);
        m_Accumulator = 0.0f;
    }
}

void PhysicsWorld::Wake(Scene& scene, ecs::Entity entity) {
    auto& world = scene.World();
    if (!world.IsAlive(entity) || !world.HasComponent<RigidbodyComponent>(entity)) return;
    auto& rb = world.GetComponent<RigidbodyComponent>(entity);
    rb.isAwake = true;
    rb.sleepTimer = 0.0f;
}

} // namespace raven
