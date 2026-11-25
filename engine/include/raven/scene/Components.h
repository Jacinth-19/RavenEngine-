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
