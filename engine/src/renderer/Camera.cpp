#include "raven/renderer/Camera.h"

#include <glm/gtc/matrix_transform.hpp>

namespace raven {

// Explicit yaw(pitch) rotation — avoids quaternion/euler-order ambiguity.
// R = yaw(Y) * pitch(X); view = Rᵀ * T(-p).
static glm::mat4 YawPitchMatrix(const glm::vec3& rotation) {
    glm::mat4 yaw = glm::rotate(glm::mat4(1.0f), rotation.y, glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 pitch = glm::rotate(glm::mat4(1.0f), rotation.x, glm::vec3(1.0f, 0.0f, 0.0f));
    return yaw * pitch;
}

glm::mat4 Camera::ViewMatrix() const {
    glm::mat4 inverseRotation = glm::transpose(YawPitchMatrix(rotation));  // orthonormal: transpose == inverse
    glm::mat4 translation = glm::translate(glm::mat4(1.0f), -position);
    return inverseRotation * translation;
}

glm::mat4 Camera::ProjectionMatrix(float aspectRatio) const {
    float fov = glm::radians(fovDegrees);
    glm::mat4 proj = glm::perspective(fov, aspectRatio, nearPlane, farPlane);
    proj[1][1] *= -1.0f;  // GLM perspective is OpenGL-style; flip Y for Vulkan
    return proj;
}

glm::mat4 Camera::ViewProjection(float aspectRatio) const {
    return ProjectionMatrix(aspectRatio) * ViewMatrix();
}

void Camera::LookAt(const glm::vec3& eye, const glm::vec3& target) {
    position = eye;
    glm::vec3 direction = glm::normalize(target - eye);
    rotation.y = std::atan2(-direction.x, -direction.z);
    rotation.x = std::asin(glm::clamp(direction.y, -1.0f, 1.0f));
    rotation.z = 0.0f;
}

glm::vec3 Camera::Forward() const {
    return glm::normalize(glm::vec3(YawPitchMatrix(rotation) * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
}

glm::vec3 Camera::Right() const {
    return glm::normalize(glm::vec3(YawPitchMatrix(rotation) * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)));
}

glm::vec3 Camera::Up() const {
    return glm::normalize(glm::vec3(YawPitchMatrix(rotation) * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f)));
}

Ray Camera::RayFromScreen(float u, float v, float aspectRatio) const {
    glm::mat4 invVP = glm::inverse(ViewProjection(aspectRatio));
    float ndcX = u * 2.0f - 1.0f;
    float ndcY = v * 2.0f - 1.0f;
    glm::vec4 nearPoint = invVP * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    glm::vec4 farPoint = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    glm::vec3 origin = glm::vec3(nearPoint) / nearPoint.w;
    glm::vec3 far = glm::vec3(farPoint) / farPoint.w;
    return Ray{origin, glm::normalize(far - origin)};
}

} // namespace raven
