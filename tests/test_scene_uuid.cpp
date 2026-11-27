// Entity UUID component + scene schema v3 (emit v3, read v1/v2 with UUID minting).
#include "test_framework.h"

#include <string>

#include <nlohmann/json.hpp>

#include "raven/scene/Scene.h"

using namespace raven;

namespace {
// Minimal scene documents. v1: initial format (no lod, no uuid).
// v2: adds lod.
const char* kV1Scene = R"({
  "schemaVersion": 1,
  "name": "Legacy V1",
  "entities": [
    { "id": 0, "components": {
        "name": { "name": "OldRoot" },
        "transform": { "position": [1, 2, 3], "rotation": [0, 0, 0], "scale": [1, 1, 1], "parent": -1 }
    }},
    { "id": 1, "components": {
        "name": { "name": "OldChild" },
        "transform": { "position": [0, 0, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1], "parent": 0 }
    }}
  ]
})";

const char* kV2Scene = R"({
  "schemaVersion": 2,
  "name": "Legacy V2",
  "entities": [
    { "id": 0, "components": {
        "name": { "name": "LodEntity" },
        "transform": { "position": [0, 0, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1], "parent": -1 },
        "lod": { "levels": ["3f6b9c2a-1a7e-4c1d-9b2f-8d5a6e7f1023", ""],
                 "screenHeights": [0.5, 0.15],
                 "boundingRadius": 2.5,
                 "lodBias": 1.25 }
    }}
  ]
})";
} // namespace

RAVEN_TEST("scene v3: newly created entities carry a non-nil UUID") {
    Scene scene("UuidFixture");
    ecs::Entity e = scene.CreateEntity("Thing");
    const auto& world = scene.World();
    CHECK(world.HasComponent<UuidComponent>(e));
    CHECK(!world.GetComponent<UuidComponent>(e).id.IsNil());
}

RAVEN_TEST("scene v3: save emits schemaVersion 3 and a uuid component per entity") {
    Scene scene("UuidWrite");
    scene.CreateEntity("A");
    scene.CreateEntity("B");
    const std::string text = scene.SerializeToString();

    nlohmann::json root = nlohmann::json::parse(text);
    CHECK_EQ(root["schemaVersion"].get<int>(), 3);
    for (const auto& entry : root["entities"]) {
        CHECK(entry["components"].contains("uuid"));
        CHECK(entry["components"]["uuid"].contains("id"));
        CHECK(!entry["components"]["uuid"]["id"].get<std::string>().empty());
    }
}

RAVEN_TEST("scene v1: loads with UUIDs minted; save then round-trips v3 stable") {
    std::string error;
    Scene scene;
    CHECK(scene.LoadFromString(kV1Scene, &error));
    auto root = scene.FindByName("OldRoot");
    auto child = scene.FindByName("OldChild");
    CHECK(root.has_value());
    CHECK(child.has_value());
    CHECK(scene.World().HasComponent<UuidComponent>(*root));
    CHECK(scene.World().HasComponent<UuidComponent>(*child));
    const UUID rootId = scene.World().GetComponent<UuidComponent>(*root).id;
    const UUID childId = scene.World().GetComponent<UuidComponent>(*child).id;
    CHECK(!rootId.IsNil());
    CHECK(!childId.IsNil());
    CHECK(rootId != childId);

    // Re-saving must emit v3 with the minted ids baked in; reloading preserves them.
    const std::string text = scene.SerializeToString();
    nlohmann::json saved = nlohmann::json::parse(text);
    CHECK_EQ(saved["schemaVersion"].get<int>(), 3);

    Scene reloaded;
    CHECK(reloaded.LoadFromString(text, &error));
    auto root2 = reloaded.FindByName("OldRoot");
    CHECK(root2.has_value());
    CHECK(reloaded.World().GetComponent<UuidComponent>(*root2).id == rootId);
}

RAVEN_TEST("scene v2: lod component survives and UUID is minted") {
    std::string error;
    Scene scene;
    CHECK(scene.LoadFromString(kV2Scene, &error));
    auto lod = scene.FindByName("LodEntity");
    CHECK(lod.has_value());
    CHECK(scene.World().HasComponent<UuidComponent>(*lod));
    CHECK(scene.World().HasComponent<LodComponent>(*lod));
    const auto& chain = scene.World().GetComponent<LodComponent>(*lod);
    CHECK_EQ(chain.levels.size(), size_t(2));
    CHECK_EQ(chain.screenHeights.size(), size_t(2));
}

RAVEN_TEST("scene v3: explicit UUIDs load and FindByUUID resolves them") {
    Scene scene("UuidLookup");
    ecs::Entity e = scene.CreateEntity("Named");
    const UUID id = scene.World().GetComponent<UuidComponent>(e).id;

    const std::string text = scene.SerializeToString();
    Scene reloaded;
    std::string error;
    CHECK(reloaded.LoadFromString(text, &error));
    auto found = reloaded.FindByUUID(id);
    CHECK(found.has_value());
    CHECK(reloaded.World().GetComponent<NameComponent>(*found).name == "Named");

    CHECK(!reloaded.FindByUUID(UUID::Nil()).has_value());
    CHECK(!reloaded.FindByUUID(UUID::Generate()).has_value());
}

RAVEN_TEST("scene v3: malformed uuid string falls back to minting (lenient reader)") {
    const char* doc = R"({
      "schemaVersion": 3,
      "name": "Broken",
      "entities": [
        { "id": 0, "components": {
            "name": { "name": "BrokenUuid" },
            "uuid": { "id": "not-a-uuid" },
            "transform": { "position": [0, 0, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1], "parent": -1 }
        }}
      ]
    })";
    std::string error;
    Scene scene;
    CHECK(scene.LoadFromString(doc, &error));  // must not fail the load
    auto e = scene.FindByName("BrokenUuid");
    CHECK(e.has_value());
    CHECK(scene.World().HasComponent<UuidComponent>(*e));
    CHECK(!scene.World().GetComponent<UuidComponent>(*e).id.IsNil());
}

RAVEN_TEST("scene v3: duplicated entity gets a fresh UUID") {
    Scene scene("UuidDuplicate");
    ecs::Entity original = scene.CreateEntity("Original");
    const UUID originalId = scene.World().GetComponent<UuidComponent>(original).id;
    ecs::Entity copy = scene.DuplicateEntity(original);
    CHECK(copy != ecs::NullEntity);
    CHECK(scene.World().HasComponent<UuidComponent>(copy));
    CHECK(scene.World().GetComponent<UuidComponent>(copy).id != originalId);
}

RAVEN_TEST("scene v3: loading a v1 file then saving produces v3 (migration persists)") {
    std::string error;
    Scene scene;
    CHECK(scene.LoadFromString(kV1Scene, &error));
    const std::string text = scene.SerializeToString();
    nlohmann::json root = nlohmann::json::parse(text);
    CHECK_EQ(root["schemaVersion"].get<int>(), 3);
    bool allUuid = true;
    for (const auto& entry : root["entities"]) {
        if (!entry["components"].contains("uuid")) allUuid = false;
    }
    CHECK(allUuid);
}
