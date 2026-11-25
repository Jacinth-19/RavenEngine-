// Physics: integration, ground contact, sleep/wake, fixed-timestep ticking, sphere response.
#include "test_framework.h"

#include <cmath>

#include <glm/glm.hpp>

#include "raven/physics/PhysicsWorld.h"
#include "raven/scene/Scene.h"
#include "raven/scene/Components.h"

using namespace raven;

namespace {

constexpr float kDt = 1.0f / 60.0f;

struct BodyScene {
    Scene scene{"Phys"};
    ecs::Entity body;
    BodyScene(glm::vec3 position, RigidbodyComponent rb) {
        body = scene.CreateEntity("Body");
        scene.World().GetComponent<TransformComponent>(body).position = position;
        scene.World().AddComponent<BoxColliderComponent>(body);
        scene.World().AddComponent<RigidbodyComponent>(body, rb);
    }
};

} // namespace

RAVEN_TEST("physics: falling dynamic body rests on the ground and falls asleep") {
    BodyScene fixture({0.0f, 3.0f, 0.0f}, RigidbodyComponent{});
    auto& scene = fixture.scene;
    const ecs::Entity body = fixture.body;
    PhysicsWorld world;
    for (int i = 0; i < 600; ++i) world.Step(scene, kDt);

    auto& rb = scene.World().GetComponent<RigidbodyComponent>(body);
    const auto& t = scene.World().GetComponent<TransformComponent>(body);
    CHECK(std::fabs(t.position.y - 0.5f) < 1e-4f);   // ground plane + half extent (0.5 * scale 1)
    CHECK(!rb.isAwake);                              // came to rest → asleep
    CHECK(std::fabs(rb.linearVelocity.y) < 1e-6f);
}

RAVEN_TEST("physics: box half extents and scale set the rest height") {
    BodyScene fixture({0.0f, 5.0f, 0.0f}, RigidbodyComponent{});
    auto& scene = fixture.scene;
    const ecs::Entity body = fixture.body;
    auto& collider = scene.World().GetComponent<BoxColliderComponent>(body);
    collider.halfExtents = {0.25f, 1.0f, 0.25f};
    auto& t = scene.World().GetComponent<TransformComponent>(body);
    t.scale = {2.0f, 2.0f, 2.0f};

    PhysicsWorld world;
    for (int i = 0; i < 600; ++i) world.Step(scene, kDt);
    CHECK(std::fabs(t.position.y - 2.0f) < 1e-4f);   // 0 + 1.0 * 2.0
}

RAVEN_TEST("physics: sleep state survives subsequent steps (body stays put)") {
    BodyScene fixture({1.0f, 0.5f, -2.0f}, RigidbodyComponent{});
    auto& scene = fixture.scene;
    const ecs::Entity body = fixture.body;
    PhysicsWorld world;
    for (int i = 0; i < 400; ++i) world.Step(scene, kDt);
    auto& rb = scene.World().GetComponent<RigidbodyComponent>(body);
    REQUIRE(!rb.isAwake);
    const glm::vec3 atSleep = scene.World().GetComponent<TransformComponent>(body).position;
    for (int i = 0; i < 120; ++i) world.Step(scene, kDt);
    const glm::vec3 later = scene.World().GetComponent<TransformComponent>(body).position;
    CHECK(glm::distance(atSleep, later) == 0.0f);
}

RAVEN_TEST("physics: sleeping body wakes when externally disturbed") {
    BodyScene fixture({0.0f, 0.5f, 0.0f}, RigidbodyComponent{});
    auto& scene = fixture.scene;
    const ecs::Entity body = fixture.body;
    PhysicsWorld world;
    for (int i = 0; i < 400; ++i) world.Step(scene, kDt);
    REQUIRE(!scene.World().GetComponent<RigidbodyComponent>(body).isAwake);

    auto& rb = scene.World().GetComponent<RigidbodyComponent>(body);
    rb.linearVelocity = {2.5f, 0.0f, 0.0f};   // external impulse source (e.g. a script)
    const float xBefore = scene.World().GetComponent<TransformComponent>(body).position.x;
    world.Step(scene, kDt);
    CHECK(scene.World().GetComponent<RigidbodyComponent>(body).isAwake);
    CHECK(scene.World().GetComponent<TransformComponent>(body).position.x > xBefore);
}

RAVEN_TEST("physics: Advance runs a fixed number of ticks regardless of frame dt") {
    BodyScene fixture({0.0f, 10.0f, 0.0f}, RigidbodyComponent{});
    auto& scene = fixture.scene;
    PhysicsWorld world;
    PhysicsConfig cfg = world.Config();
    cfg.fixedTimestep = 1.0f / 60.0f;
    cfg.maxSubSteps = 5;
    world.SetConfig(cfg);

    world.Advance(scene, 1.0f / 30.0f);
    CHECK(world.FixedTicksLastAdvance() == 2);

    world.Advance(scene, 10.0f);   // huge catch-up request is clamped to maxSubSteps
    CHECK(world.FixedTicksLastAdvance() == 5);
}

RAVEN_TEST("physics: tiny frame delta accumulates across frames") {
    BodyScene fixture({0.0f, 10.0f, 0.0f}, RigidbodyComponent{});
    auto& scene = fixture.scene;
    PhysicsWorld world;
    PhysicsConfig cfg = world.Config();
    cfg.fixedTimestep = 1.0f / 60.0f;
    world.SetConfig(cfg);

    world.Advance(scene, 1.0f / 240.0f);
    CHECK(world.FixedTicksLastAdvance() == 0);   // not enough time yet — accumulates
    world.Advance(scene, 1.0f / 240.0f);
    world.Advance(scene, 1.0f / 240.0f);
    world.Advance(scene, 1.0f / 240.0f);
    CHECK(world.FixedTicksLastAdvance() == 1);   // accumulated 1/60 — one tick fires
}

RAVEN_TEST("physics: static bodies are never integrated") {
    RigidbodyComponent rb;
    rb.type = RigidbodyType::Static;
    BodyScene fixture({0.0f, 7.0f, 0.0f}, rb);
    auto& scene = fixture.scene;
    const ecs::Entity body = fixture.body;
    PhysicsWorld world;
    for (int i = 0; i < 120; ++i) world.Step(scene, kDt);
    CHECK(std::fabs(scene.World().GetComponent<TransformComponent>(body).position.y - 7.0f) < 1e-6f);
}

RAVEN_TEST("physics: overlapping dynamic spheres push apart") {
    Scene scene("Pair");
    const ecs::Entity a = scene.CreateEntity("A");
    const ecs::Entity b = scene.CreateEntity("B");
    auto& world_ = scene.World();
    world_.GetComponent<TransformComponent>(a).position = {0.0f, 5.0f, 0.0f};
    world_.GetComponent<TransformComponent>(b).position = {0.6f, 5.0f, 0.0f};
    for (ecs::Entity e : {a, b}) {
        world_.AddComponent<BoxColliderComponent>(e);   // half extents x = 0.5 → touching at 1.0
        RigidbodyComponent rb;
        rb.useGravity = false;
        world_.AddComponent<RigidbodyComponent>(e, rb);
    }
    PhysicsWorld world;
    world.Step(scene, kDt);
    const float newDist = glm::distance(world_.GetComponent<TransformComponent>(a).position,
                                        world_.GetComponent<TransformComponent>(b).position);
    CHECK(newDist > 0.6f);
}
