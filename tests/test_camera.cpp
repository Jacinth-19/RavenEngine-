// Camera math: view/projection orientation and screen→ray picking.
#include "test_framework.h"

#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include "raven/renderer/Camera.h"

using namespace raven;

constexpr float kEps = 1e-4f;

RAVEN_TEST("camera: look-at forward direction points at the target") {
    Camera cam;
    cam.position = {5.0f, 3.0f, 5.0f};
    cam.LookAt(cam.position, glm::vec3(0.0f, 0.0f, 0.0f));
    glm::vec3 fwd = cam.Forward();
    glm::vec3 toTarget = glm::normalize(glm::vec3(0.0f) - cam.position);
    CHECK(glm::length(fwd - toTarget) < 1e-3f);
}

RAVEN_TEST("camera: ray through screen center travels along the forward axis") {
    Camera cam;
    cam.position = {0.0f, 2.0f, 8.0f};
    cam.LookAt(cam.position, glm::vec3(0.0f, 0.0f, 0.0f));
    const float aspect = 16.0f / 9.0f;
    Ray ray = cam.RayFromScreen(0.5f, 0.5f, aspect);
    glm::vec3 fwd = cam.Forward();
    CHECK(std::fabs(glm::length(ray.origin - cam.position) - cam.nearPlane) < 1e-2f);
    CHECK(glm::length(ray.direction - fwd) < 1e-2f);
}

RAVEN_TEST("camera: ray directions fan out for off-center pixels") {
    Camera cam;
    cam.position = {0.0f, 0.0f, 0.0f};
    cam.rotation = {0.0f, 0.0f, 0.0f};   // facing -Z by convention
    const float aspect = 1.0f;
    Ray left = cam.RayFromScreen(0.1f, 0.5f, aspect);
    Ray right = cam.RayFromScreen(0.9f, 0.5f, aspect);
    CHECK(left.direction.x < 0.0f);
    CHECK(right.direction.x > 0.0f);
    CHECK(std::fabs(glm::dot(left.direction, glm::vec3(0, 1, 0))) < 1e-2f);
}

RAVEN_TEST("camera: visible point projects to positive NDC depth") {
    Camera cam;
    cam.position = {0.0f, 0.0f, 5.0f};
    cam.LookAt(cam.position, glm::vec3(0.0f, 0.0f, 0.0f));
    const float aspect = 1.0f;
    glm::mat4 vp = cam.ViewProjection(aspect);
    glm::vec4 clip = vp * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    clip /= clip.w;
    CHECK(clip.z >= 0.0f && clip.z <= 1.0f);   // GLM_FORCE_DEPTH_ZERO_TO_ONE
}

RAVEN_TEST("camera: projection flips Y for Vulkan clip space") {
    Camera cam;
    cam.position = {0.0f, 0.0f, 0.0f};
    cam.rotation = {0.0f, 0.0f, 0.0f};
    glm::mat4 proj = cam.ProjectionMatrix(1.0f);
    // A point above the view axis must land at NEGATIVE NDC y (Vulkan y-down).
    glm::vec4 clip = proj * glm::vec4(0.0f, 1.0f, -5.0f, 1.0f);
    CHECK(clip.y * clip.w < 0.0f);
}

RAVEN_TEST("camera: inverse of view*projection unprojects consistently") {
    Camera cam;
    cam.position = {3.0f, 2.0f, 4.0f};
    cam.LookAt(cam.position, glm::vec3(-1.0f, 0.5f, 2.0f));
    const float aspect = 1.33f;
    glm::vec4 world{1.5f, -0.5f, 0.25f, 1.0f};
    glm::mat4 vp = cam.ViewProjection(aspect);
    glm::vec4 ndc = vp * world;
    ndc /= ndc.w;
    glm::mat4 inv = glm::inverse(vp);
    glm::vec4 back = inv * glm::vec4(ndc.x, ndc.y, ndc.z, 1.0f);
    back /= back.w;
    CHECK(glm::length(glm::vec3(back) - glm::vec3(world)) < 1e-3f);
}

RAVEN_TEST("camera: aspect changes horizontal but not vertical FOV") {
    Camera cam;
    cam.fovDegrees = 60.0f;
    glm::mat4 narrow = cam.ProjectionMatrix(1.0f);
    glm::mat4 wide = cam.ProjectionMatrix(2.0f);
    CHECK(std::fabs(narrow[1][1] - wide[1][1]) < 1e-5f);   // vertical scale identical
    CHECK(std::fabs(narrow[0][0]) > std::fabs(wide[0][0])); // narrower viewport → larger x scale
}
