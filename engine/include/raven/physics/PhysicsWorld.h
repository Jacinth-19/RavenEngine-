// RavenEngine — minimal rigid-body physics: semi-implicit Euler integration,
// gravity, ground-plane collision (y = 0) with restitution, sphere-sphere
// response between dynamic bodies, box half-extent support via BoxColliderComponent.
// Deliberately small and deterministic; Jolt/Bullet integration is on the roadmap.
//
// Two ways to advance the world:
//   Step(scene, dt)    — integrate directly with the given (variable) dt.
//   Advance(scene, dt) — fixed-timestep accumulator: runs as many fixed ticks of
//                        PhysicsConfig::fixedTimestep as fit, up to maxSubSteps per
//                        frame (excess time is dropped, never spirals). This is the
//                        deterministic API engine loops should call per frame.
//
// Bodies at rest below PhysicsConfig::sleepLinearVelocity for sleepTime seconds fall
// asleep and are skipped by the solver until disturbed (an external velocity above the
// wake threshold wakes them). Sleeping dynamic bodies act as immovable obstacles in
// sphere-sphere response.
#pragma once

#include <glm/glm.hpp>

#include "raven/ecs/World.h"

namespace raven {

class Scene;

struct PhysicsConfig {
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    float groundHeight = 0.0f;        // infinite plane at y = groundHeight
    bool enableSphereResponse = true; // pairwise sphere push-out between dynamics
    float linearDamping = 0.0f;
    float fixedTimestep = 1.0f / 60.0f;  // seconds per fixed tick (Advance)
    int maxSubSteps = 5;                 // catch-up cap per frame (Advance)
    float sleepLinearVelocity = 0.05f;   // m/s — at rest below this
    float sleepTime = 0.5f;              // seconds at rest before falling asleep
};

class PhysicsWorld {
public:
    explicit PhysicsWorld(PhysicsConfig config = {}) : m_Config(config) {}

    void Step(Scene& scene, float deltaSeconds);
    void Advance(Scene& scene, float frameDeltaSeconds);

    const PhysicsConfig& Config() const { return m_Config; }
    void SetConfig(const PhysicsConfig& config) { m_Config = config; }

    // Ticks executed by the last Advance() call (0 when the frame was too short for a tick).
    int FixedTicksLastAdvance() const { return m_FixedTicksLastAdvance; }
    void ResetAccumulator() { m_Accumulator = 0.0f; }

    // Wake a sleeping body. A no-op for awake/static/missing entities.
    void Wake(Scene& scene, ecs::Entity entity);

private:
    PhysicsConfig m_Config;
    float m_Accumulator = 0.0f;
    int m_FixedTicksLastAdvance = 0;
};

} // namespace raven
