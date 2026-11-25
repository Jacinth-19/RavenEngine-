#include "raven/physics/PhysicsWorld.h"

#include "raven/scene/Scene.h"
#include "raven/scene/Components.h"
#include "raven/core/Logger.h"

#include <algorithm>
#include <cmath>

namespace raven {

void PhysicsWorld::Step(Scene& scene, float dt) {
    if (dt <= 0.0f) return;
    dt = std::min(dt, 1.0f / 15.0f);  // clamp huge frame hitches

    auto& world = scene.World();

    // Integrate gravity + velocity for dynamic bodies.
    world.ForEach<RigidbodyComponent>([&](ecs::Entity e, RigidbodyComponent& rb) {
        if (rb.type != RigidbodyType::Dynamic) return;
        if (!world.HasComponent<TransformComponent>(e)) return;
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
    if (!m_Config.enableSphereResponse) return;
    std::vector<ecs::Entity> dynamics;
    world.ForEach<RigidbodyComponent>([&](ecs::Entity e, const RigidbodyComponent& rb) {
        if (rb.type == RigidbodyType::Dynamic) dynamics.push_back(e);
    });
    for (size_t i = 0; i < dynamics.size(); ++i) {
        for (size_t j = i + 1; j < dynamics.size(); ++j) {
            ecs::Entity a = dynamics[i], b = dynamics[j];
            auto& ta = world.GetComponent<TransformComponent>(a);
            auto& tb = world.GetComponent<TransformComponent>(b);
            float ra = world.HasComponent<BoxColliderComponent>(a) ? world.GetComponent<BoxColliderComponent>(a).halfExtents.x : 0.5f;
            float rb_ = world.HasComponent<BoxColliderComponent>(b) ? world.GetComponent<BoxColliderComponent>(b).halfExtents.x : 0.5f;
            glm::vec3 delta = tb.position - ta.position;
            float dist = glm::length(delta);
            float minDist = ra + rb_;
            if (dist > 1e-5f && dist < minDist) {
                glm::vec3 normal = delta / dist;
                float penetration = minDist - dist;
                ta.position -= normal * (penetration * 0.5f);
                tb.position += normal * (penetration * 0.5f);
                auto& rav = world.GetComponent<RigidbodyComponent>(a).linearVelocity;
                auto& rbv = world.GetComponent<RigidbodyComponent>(b).linearVelocity;
                float relVel = glm::dot(rbv - rav, normal);
                if (relVel < 0.0f) {
                    glm::vec3 impulse = normal * (-relVel * 0.5f);
                    rav -= impulse;
                    rbv += impulse;
                }
            }
        }
    }
}

} // namespace raven
