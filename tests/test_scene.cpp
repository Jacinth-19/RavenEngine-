// Scene: JSON round-trip, hierarchy, world matrices, dirty tracking.
#include "test_framework.h"

#include <cmath>
#include <filesystem>

#include <glm/glm.hpp>

#include "raven/scene/Scene.h"

using namespace raven;

namespace {
constexpr float kEps = 1e-4f;
bool Near(float a, float b, float eps = kEps) { return std::fabs(a - b) < eps; }

struct SceneFixture {
    Scene scene{"Fixture"};
    ecs::Entity root = scene.CreateEntity("Root");
    ecs::Entity child = scene.CreateEntity("Child");
    ecs::Entity light = scene.CreateEntity("Sun");

    SceneFixture() {
        scene.SetParent(child, root);
        auto& rt = scene.World().GetComponent<TransformComponent>(root);
        rt.position = {1.0f, 2.0f, 3.0f};
        rt.rotation = {0.0f, 0.5f, 0.0f};
        rt.scale = {2.0f, 2.0f, 2.0f};
        auto& ct = scene.World().GetComponent<TransformComponent>(child);
        ct.position = {1.0f, 0.0f, 0.0f};
        ct.scale = {0.5f, 0.5f, 0.5f};
        auto& lt = scene.World().GetComponent<TransformComponent>(light);
        lt.position = {0.0f, 5.0f, 0.0f};

        DirectionalLightComponent dl;
        dl.direction = {0.3f, -1.0f, 0.2f};
        dl.intensity = 2.5f;
        scene.World().AddComponent<DirectionalLightComponent>(light, dl);

        MeshRendererComponent mr;
        mr.mesh = UUID::Parse("3f6b9c2a-1a7e-4c1d-9b2f-8d5a6e7f1023").value_or(UUID::Nil());
        mr.material = UUID::Parse("c63e9d2b-7a4f-4b0d-d5f8-4e0b3c2a13d9").value_or(UUID::Nil());
        scene.World().AddComponent<MeshRendererComponent>(root, mr);

        ScriptComponent sc;
        sc.scriptPath = "assets/scripts/spin.lua";
        scene.World().AddComponent<ScriptComponent>(child, sc);
    }
};
} // namespace

RAVEN_TEST("scene: save/load round-trip preserves components and hierarchy") {
    SceneFixture fixture;
    const std::string text = fixture.scene.SerializeToString();

    Scene loaded("Loaded");
    std::string error;
    CHECK(loaded.LoadFromString(text, &error));

    auto rootLoaded = loaded.FindByName("Root");
    auto childLoaded = loaded.FindByName("Child");
    auto lightLoaded = loaded.FindByName("Sun");
    CHECK(rootLoaded.has_value());
    CHECK(childLoaded.has_value());
    CHECK(lightLoaded.has_value());

    const auto& ct = loaded.World().GetComponent<TransformComponent>(*childLoaded);
    CHECK(Near(ct.position.x, 1.0f));
    // child should still be parented to root
    const auto& childChildrenParent = loaded.World().GetComponent<TransformComponent>(*childLoaded);
    CHECK(childChildrenParent.parent == *rootLoaded);

    const auto& mr = loaded.World().GetComponent<MeshRendererComponent>(*rootLoaded);
    CHECK(mr.mesh == UUID::Parse("3f6b9c2a-1a7e-4c1d-9b2f-8d5a6e7f1023").value_or(UUID::Nil()));
    CHECK(mr.material == UUID::Parse("c63e9d2b-7a4f-4b0d-d5f8-4e0b3c2a13d9").value_or(UUID::Nil()));

    const auto& dl = loaded.World().GetComponent<DirectionalLightComponent>(*lightLoaded);
    CHECK(Near(dl.intensity, 2.5f));

    const auto& sc = loaded.World().GetComponent<ScriptComponent>(*childLoaded);
    CHECK(sc.scriptPath == "assets/scripts/spin.lua");
    CHECK(sc.enabled);
}

RAVEN_TEST("scene: world matrix composes parent and child transforms") {
    SceneFixture fixture;
    glm::mat4 world = fixture.scene.GetWorldMatrix(fixture.child);
    // child local translation (1,0,0) scaled by parent scale (2,2,2) rotated 0.5 rad about Y, then + (1,2,3)
    const float c = std::cos(0.5f), s = std::sin(0.5f);
    const float expectedX = c * 2.0f + 1.0f;   // cos(yaw)*2 + parent pos.x
    const float expectedZ = -s * 2.0f + 3.0f;  // -sin(yaw)*2 + parent pos.z
    CHECK(Near(world[3][0], expectedX, 1e-3f));
    CHECK(Near(world[3][1], 2.0f, 1e-3f));
    CHECK(Near(world[3][2], expectedZ, 1e-3f));
}

RAVEN_TEST("scene: file save/load round-trip via disk") {
    SceneFixture fixture;
    const auto tmp = std::filesystem::temp_directory_path() / "raven_test_scene_roundtrip.ravenscene";
    std::string error;
    CHECK(fixture.scene.Save(tmp, &error));

    Scene loaded;
    CHECK(loaded.Load(tmp, &error));
    CHECK(loaded.FindByName("Root").has_value());
    CHECK(!loaded.IsDirty());
    std::filesystem::remove(tmp);
}

RAVEN_TEST("scene: dirty flag is set by mutations and cleared by save") {
    SceneFixture fixture;
    fixture.scene.ClearDirty();
    CHECK(!fixture.scene.IsDirty());
    fixture.scene.CreateEntity("Another");
    CHECK(fixture.scene.IsDirty());
    fixture.scene.ClearDirty();

    const auto tmp = std::filesystem::temp_directory_path() / "raven_test_scene_dirty.ravenscene";
    std::string error;
    CHECK(fixture.scene.Save(tmp, &error));
    CHECK(!fixture.scene.IsDirty());
    std::filesystem::remove(tmp);
}

RAVEN_TEST("scene: destroy entity removes it and its descendants") {
    SceneFixture fixture;
    fixture.scene.DestroyEntity(fixture.root);
    CHECK(!fixture.scene.FindByName("Root").has_value());
    CHECK(!fixture.scene.FindByName("Child").has_value());
    CHECK(fixture.scene.FindByName("Sun").has_value());
}

RAVEN_TEST("scene: duplicate copies components and detaches from parent hierarchy") {
    SceneFixture fixture;
    ecs::Entity dup = fixture.scene.DuplicateEntity(fixture.child);
    CHECK(dup != fixture.child);
    CHECK(fixture.scene.World().HasComponent<ScriptComponent>(dup));
    const auto& t = fixture.scene.World().GetComponent<TransformComponent>(dup);
    CHECK(Near(t.position.x, 1.0f));
}

RAVEN_TEST("scene: is descendant query works through the hierarchy") {
    SceneFixture fixture;
    CHECK(fixture.scene.IsDescendantOf(fixture.child, fixture.root));
    CHECK(!fixture.scene.IsDescendantOf(fixture.root, fixture.child));
    CHECK(!fixture.scene.IsDescendantOf(fixture.light, fixture.root));
}
