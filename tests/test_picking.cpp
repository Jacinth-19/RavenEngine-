// Entity picking math: a ray cast from the camera through the crate's projected
// screen position must pass close to the crate — this is the geometric core of
// the editor's click-to-select.
#include "test_framework.h"

#include <filesystem>
#include <fstream>
#include <glm/glm.hpp>

#include "raven/renderer/Camera.h"
#include "raven/scene/Scene.h"

using namespace raven;

#ifndef RAVEN_SAMPLE_DIR
#define RAVEN_SAMPLE_DIR "sample"
#endif

namespace {

float DistancePointToRay(const glm::vec3& point, const Ray& ray) {
    const glm::vec3 fromOrigin = point - ray.origin;
    const float t = glm::dot(fromOrigin, ray.direction);
    if (t < 0.0f) return glm::length(fromOrigin);
    const glm::vec3 closest = ray.origin + ray.direction * t;
    return glm::length(point - closest);
}

void ProjectToScreen(const Camera& camera, const glm::vec3& worldPos, float aspect, float& u, float& v) {
    // Same VP matrix as RayFromScreen uses → v comes out top-origin (u right, v down).
    glm::mat4 vp = camera.ViewProjection(aspect);
    glm::vec4 clip = vp * glm::vec4(worldPos, 1.0f);
    clip /= clip.w;
    u = 0.5f * (clip.x + 1.0f);
    v = 0.5f * (clip.y + 1.0f);   // ndcY = v*2-1  (see Camera::RayFromScreen)
}

} // namespace

RAVEN_TEST("picking: center-screen ray passes through the crate") {
    const auto scenePath = std::filesystem::path(RAVEN_SAMPLE_DIR) / "scenes" / "sample_scene.ravenscene";
    Scene scene("Sample");
    std::string error;
    REQUIRE(scene.Load(scenePath, &error));

    auto crate = scene.FindByName("Crate");
    REQUIRE(crate.has_value());
    const glm::vec3 cratePos{scene.GetWorldMatrix(*crate)[3]};

    Camera camera;
    camera.position = {4.2f, 3.1f, 6.3f};
    camera.LookAt(camera.position, cratePos);

    const float aspect = 16.0f / 9.0f;
    Ray ray = camera.RayFromScreen(0.5f, 0.5f, aspect);
    CHECK(DistancePointToRay(cratePos, ray) < 1.5f);
}

RAVEN_TEST("picking: reprojecting the crate to screen and clicking there selects it") {
    const auto scenePath = std::filesystem::path(RAVEN_SAMPLE_DIR) / "scenes" / "sample_scene.ravenscene";
    Scene scene("Sample");
    std::string error;
    REQUIRE(scene.Load(scenePath, &error));

    auto crate = scene.FindByName("Crate");
    REQUIRE(crate.has_value());
    const glm::vec3 cratePos{scene.GetWorldMatrix(*crate)[3]};

    Camera camera;
    camera.position = {4.2f, 3.1f, 6.3f};
    camera.LookAt(camera.position, cratePos);

    const float aspect = 16.0f / 9.0f;
    float u = 0.0f, vTop = 0.0f;
    ProjectToScreen(camera, cratePos, aspect, u, vTop);
    CHECK(u > 0.0f && u < 1.0f);
    CHECK(vTop > 0.0f && vTop < 1.0f);

    Ray click = camera.RayFromScreen(u, vTop, aspect);

    // Distance from the click ray to the crate center must be well inside the crate's radius (1.0 * scale).
    CHECK(DistancePointToRay(cratePos, click) < 1.0f);
}

RAVEN_TEST("picking: ray is a unit vector") {
    Camera camera;
    camera.position = {0.0f, 0.0f, 5.0f};
    camera.LookAt(camera.position, glm::vec3(0.0f));
    const float aspect = 1.5f;
    for (float u : {0.1f, 0.3f, 0.5f, 0.7f, 0.9f}) {
        for (float v : {0.1f, 0.5f, 0.9f}) {
            Ray ray = camera.RayFromScreen(u, v, aspect);
            CHECK(std::fabs(glm::length(ray.direction) - 1.0f) < 1e-3f);
        }
    }
}
