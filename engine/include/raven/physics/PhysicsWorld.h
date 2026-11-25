// RavenEngine — minimal rigid-body physics: semi-implicit Euler integration,
// gravity, ground-plane collision (y = 0) with restitution, sphere-sphere
// response between dynamic bodies, box half-extent support via BoxColliderComponent.
// Deliberately small and deterministic; Jolt/Bullet integration is on the roadmap.
#pragma once

#include <glm/glm.hpp>

namespace raven {

class Scene;

struct PhysicsConfig {
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    float groundHeight = 0.0f;        // infinite plane at y = groundHeight
    bool enableSphereResponse = true; // pairwise sphere push-out between dynamics
    float linearDamping = 0.0f;
};

class PhysicsWorld {
public:
    explicit PhysicsWorld(PhysicsConfig config = {}) : m_Config(config) {}

    void Step(Scene& scene, float deltaSeconds);
    const PhysicsConfig& Config() const { return m_Config; }
    void SetConfig(const PhysicsConfig& config) { m_Config = config; }

private:
    PhysicsConfig m_Config;
};

} // namespace raven
