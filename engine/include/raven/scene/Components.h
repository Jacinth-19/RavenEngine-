// RavenEngine — scene components. Plain data; serialization lives in Scene.cpp
// so the structs stay free of serializer concerns.
#pragma once

#include <glm/glm.hpp>
#include <string>
#include <vector>

#include "raven/core/UUID.h"
#include "raven/ecs/World.h"

namespace raven {

struct NameComponent {
    std::string name;
};

struct TransformComponent {
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f};   // Euler angles, radians, XYZ order
    glm::vec3 scale{1.0f};
    ecs::Entity parent = ecs::NullEntity;
    std::vector<ecs::Entity> children;
};

// Distance-based LOD chain for the mesh on this entity. levels[0] should equal the
// MeshRendererComponent mesh (LOD0); empty chain means "no LOD, draw LOD0 always".
// Serialized as scene component "lod" (schema v2) and consumed by the renderer
// (projected screen height vs the tier LOD bias/thresholds picks the level; entities
// beyond the tier's lodMaxDistance are not drawn at all).
struct LodComponent {
    std::vector<UUID> levels;         // mesh UUIDs, LOD0..LODn
    std::vector<float> screenHeights; // switch LOD k -> k+1 below screenHeights[k] px
    float boundingRadius = 1.0f;      // bounding sphere radius in meters
    float lodBias = 0.0f;             // additive to preset lodBias at draw time
};

struct MeshRendererComponent {
    UUID mesh;                  // asset UUID (nil = not set)
    UUID material;              // asset UUID (nil = fallback default material)
    bool castShadow = true;
    bool visible = true;
};

struct DirectionalLightComponent {
    glm::vec3 direction{0.4472f, -0.8944f, 0.1789f};  // normalized-ish; -Y dominant
    glm::vec3 color{1.0f, 0.98f, 0.92f};
    float intensity = 3.0f;
    bool castShadow = true;
};

struct PointLightComponent {
    glm::vec3 color{1.0f};
    float intensity = 10.0f;
    float radius = 5.0f;
};

struct CameraComponent {
    float fovDegrees = 60.0f;
    float nearPlane = 0.1f;
    float farPlane = 250.0f;
    bool isPrimary = false;
};

enum class RigidbodyType : uint8_t { Static = 0, Dynamic = 1 };

struct RigidbodyComponent {
    RigidbodyType type = RigidbodyType::Dynamic;
    float mass = 1.0f;
    glm::vec3 linearVelocity{0.0f};
    bool useGravity = true;
    float restitution = 0.35f;      // bounciness on ground hit
    // Runtime state, not serialized: bodies at rest below PhysicsConfig::sleepLinearVelocity
    // for PhysicsConfig::sleepTime seconds are put to sleep (skipped by the solver) until
    // disturbed by an external velocity change above the wake threshold.
    bool isAwake = true;
    float sleepTimer = 0.0f;
};

struct BoxColliderComponent {
    glm::vec3 halfExtents{0.5f, 0.5f, 0.5f};
};

struct AudioSourceComponent {
    UUID clip;                       // audio asset UUID
    float volume = 1.0f;
    bool loop = false;
    bool playOnStart = false;
    bool is3D = true;
};

struct ScriptComponent {
    std::string scriptPath;          // path relative to project assets root, e.g. "assets/scripts/spin.lua"
    bool enabled = true;
};

// Marks the entity as a root-level prefab instance created from an .ravenprefab
// bundle (see docs/ROADMAP.md). Kept for forward compatibility.
struct PrefabInstanceComponent {
    UUID prefab;
};

} // namespace raven
