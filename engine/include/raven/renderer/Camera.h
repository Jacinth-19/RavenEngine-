// RavenEngine — perspective camera. View/projection matrices + screen→ray for picking.
// Projection is GLM_FORCE_DEPTH_ZERO_TO_ONE with a Vulkan-style Y flip.
#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace raven {

struct Ray {
    glm::vec3 origin{0.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f};
};

class Camera {
public:
    glm::vec3 position{0.0f, 2.0f, 6.0f};
    glm::vec3 rotation{0.0f};            // Euler radians (XYZ); rotation.x = pitch, rotation.y = yaw
    float fovDegrees = 60.0f;
    float nearPlane = 0.1f;
    float farPlane = 500.0f;

    glm::mat4 ViewMatrix() const;
    glm::mat4 ProjectionMatrix(float aspectRatio) const;
    glm::mat4 ViewProjection(float aspectRatio) const;

    // Rotate around `target` orbiting yaw/pitch (editor camera style).
    void LookAt(const glm::vec3& eye, const glm::vec3& target);
    glm::vec3 Forward() const;
    glm::vec3 Right() const;
    glm::vec3 Up() const;

    // Screen-space (0..1, origin top-left) → world-space ray.
    Ray RayFromScreen(float u, float v, float aspectRatio) const;
};

} // namespace raven
