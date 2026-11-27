// Phase 2 commands + selection: scene-level tests driving headless mutations.
// Includes the editor command headers directly (they are header-only and link only
// raven_core) — the editor UI itself is exercised via CI capture.
#include "test_framework.h"

#include <memory>
#include <string>
#include <vector>

#include "editor/commands/EditorCommandStack.h"
#include "editor/commands/EntityCommands.h"
#include "editor/commands/ComponentCommands.h"
#include "editor/commands/PropertyCommand.h"
#include "editor/commands/GizmoTransformCommand.h"
#include "editor/commands/MaterialSnapshotCommand.h"
#include "editor/commands/AssetMetaSnapshotCommand.h"
#include "editor/commands/MultiEditPropertyCommand.h"
#include "editor/commands/ComponentJsonCommand.h"
#include "editor/selection/EditorSelection.h"
#include "raven/scene/Scene.h"

using namespace raven;
using namespace raven::editor;

namespace {

// Scene with: root "Parent" -> child "Child" -> grandchild "Leaf" (mesh renderer on Child).
struct Fixture {
    Scene scene{"Test"};
    ecs::Entity parent = ecs::NullEntity;
    ecs::Entity child = ecs::NullEntity;
    ecs::Entity leaf = ecs::NullEntity;
    UUID ParentUuid, ChildUuid, LeafUuid;

    Fixture() {
        parent = scene.CreateEntity("Parent");
        child = scene.CreateEntity("Child");
        leaf = scene.CreateEntity("Leaf");
        scene.SetParent(child, parent);
        scene.SetParent(leaf, child);
        scene.World().AddComponent<MeshRendererComponent>(child, MeshRendererComponent{});
        scene.World().GetComponent<TransformComponent>(child).position = glm::vec3(1.0f, 2.0f, 3.0f);
        ParentUuid = scene.World().GetComponent<UuidComponent>(parent).id;
        ChildUuid = scene.World().GetComponent<UuidComponent>(child).id;
        LeafUuid = scene.World().GetComponent<UuidComponent>(leaf).id;
        scene.ClearDirty();
    }
};

} // namespace

RAVEN_TEST("phase2: entity create via command is stable across undo/redo (same UUID)") {
    Scene scene{"T"};
    CommandStack stack;
    auto cmd = std::make_shared<EntityCreateCommand>(scene, "Created", UUID::Nil());
    const UUID expected = cmd->NewUuid();
    stack.PushExecuted(cmd);

    auto found = scene.FindByUUID(expected);
    CHECK(found.has_value());
    CHECK(scene.World().GetComponent<NameComponent>(*found).name == std::string("Created"));

    CHECK(stack.Undo());
    CHECK(!scene.FindByUUID(expected).has_value());
    CHECK(stack.Redo());
    CHECK(scene.FindByUUID(expected).has_value());  // same identity restored
}

RAVEN_TEST("phase2: destroy + undo restores the full subtree with UUIDs and order") {
    Fixture f;
    CommandStack stack;
    stack.PushExecuted(std::make_shared<EntityDestroyCommand>(f.scene, f.ChildUuid));

    CHECK(!f.scene.FindByUUID(f.ChildUuid).has_value());
    CHECK(!f.scene.FindByUUID(f.LeafUuid).has_value());
    CHECK(f.scene.FindByUUID(f.ParentUuid).has_value());

    CHECK(stack.Undo());
    auto child = f.scene.FindByUUID(f.ChildUuid);
    auto leaf = f.scene.FindByUUID(f.LeafUuid);
    CHECK(child.has_value());
    CHECK(leaf.has_value());
    CHECK(f.scene.World().HasComponent<MeshRendererComponent>(*child));
    // hierarchy re-attached under the same parent
    auto& ct = f.scene.World().GetComponent<TransformComponent>(*child);
    CHECK(ct.parent != ecs::NullEntity);
    auto& lt = f.scene.World().GetComponent<TransformComponent>(*leaf);
    CHECK(lt.parent == *child);
    auto& pt = f.scene.World().GetComponent<TransformComponent>(*f.scene.FindByUUID(f.ParentUuid));
    CHECK_EQ(pt.children.size(), size_t(1));
    CHECK_EQ(pt.children.front(), *child);
}

RAVEN_TEST("phase2: component property command round-trips a transform") {
    Fixture f;
    CommandStack stack;
    auto cmd = std::make_shared<ComponentPropertyCommand<TransformComponent, glm::vec3>>(
        f.scene, f.ChildUuid, &TransformComponent::position, glm::vec3(9.0f, 8.0f, 7.0f), "Edit Position");
    stack.PushExecuted(cmd);
    CHECK(f.scene.World().GetComponent<TransformComponent>(*f.scene.FindByUUID(f.ChildUuid)).position == glm::vec3(9.0f, 8.0f, 7.0f));
    CHECK(stack.Undo());
    CHECK(f.scene.World().GetComponent<TransformComponent>(*f.scene.FindByUUID(f.ChildUuid)).position == glm::vec3(1.0f, 2.0f, 3.0f));
}

RAVEN_TEST("phase2: component add/remove round-trip") {
    Fixture f;
    CommandStack stack;
    CHECK(f.scene.World().HasComponent<MeshRendererComponent>(*f.scene.FindByUUID(f.ChildUuid)));

    stack.PushExecuted(std::make_shared<ComponentRemoveCommand<MeshRendererComponent>>(f.scene, f.ChildUuid));
    CHECK(!f.scene.World().HasComponent<MeshRendererComponent>(*f.scene.FindByUUID(f.ChildUuid)));
    CHECK(stack.Undo());
    CHECK(f.scene.World().HasComponent<MeshRendererComponent>(*f.scene.FindByUUID(f.ChildUuid)));

    // Point light: default add, undo removes.
    stack.PushExecuted(std::make_shared<ComponentAddCommand<PointLightComponent>>(f.scene, f.ChildUuid));
    CHECK(f.scene.World().HasComponent<PointLightComponent>(*f.scene.FindByUUID(f.ChildUuid)));
    CHECK(stack.Undo());
    CHECK(!f.scene.World().HasComponent<PointLightComponent>(*f.scene.FindByUUID(f.ChildUuid)));
}

RAVEN_TEST("phase2: reparent moves an entity and rejects cycles") {
    Fixture f;
    CommandStack stack;

    // cycle: reparent Parent under Leaf (its own descendant) must be rejected/no-op
    stack.PushExecuted(std::make_shared<EntityReparentCommand>(f.scene, f.ParentUuid, f.LeafUuid));
    auto& pt = f.scene.World().GetComponent<TransformComponent>(*f.scene.FindByUUID(f.ParentUuid));
    CHECK(pt.parent == ecs::NullEntity);   // unchanged

    // legit move: reparent Leaf to Parent
    stack.PushExecuted(std::make_shared<EntityReparentCommand>(f.scene, f.LeafUuid, f.ParentUuid));
    auto leaf = f.scene.FindByUUID(f.LeafUuid);
    CHECK(leaf.has_value());
    CHECK(f.scene.World().GetComponent<TransformComponent>(*leaf).parent ==
          *f.scene.FindByUUID(f.ParentUuid));
    CHECK(stack.Undo());
    leaf = f.scene.FindByUUID(f.LeafUuid);
    CHECK(f.scene.World().GetComponent<TransformComponent>(*leaf).parent ==
          *f.scene.FindByUUID(f.ChildUuid));
}

RAVEN_TEST("phase2: duplicate mints fresh UUIDs and one undo removes the copy tree") {
    Fixture f;
    CommandStack stack;
    auto cmd = std::make_shared<EntityDuplicateCommand>(f.scene, f.ChildUuid);
    stack.PushExecuted(cmd);
    const UUID created = cmd->CreatedRootUuid();
    CHECK(!created.IsNil());
    CHECK(created != f.ChildUuid);

    auto createdRoot = f.scene.FindByUUID(created);
    CHECK(createdRoot.has_value());
    CHECK(f.scene.World().GetComponent<NameComponent>(*createdRoot).name == std::string("Child (Copy)"));
    // children were duplicated too, with fresh UUIDs
    const auto& children = f.scene.ChildrenOf(*createdRoot);
    CHECK_EQ(children.size(), size_t(1));
    CHECK(f.scene.World().GetComponent<UuidComponent>(children.front()).id != f.LeafUuid);

    CHECK(stack.Undo());
    CHECK(!f.scene.FindByUUID(created).has_value());
    CHECK(stack.Redo());
    auto restored = f.scene.FindByUUID(created);   // redo restores the exact set
    CHECK(restored.has_value());
}

RAVEN_TEST("phase2: subtree serialization snapshots round-trip components") {
    Fixture f;
    nlohmann::json subtree = f.scene.SerializeSubtree(*f.scene.FindByUUID(f.ChildUuid));
    CHECK(subtree.contains("entities"));
    CHECK_EQ(subtree["entities"].size(), size_t(2));   // child + leaf

    Scene copy{"Copy"};
    ecs::Entity root = copy.InstantiateSubtree(subtree, ecs::NullEntity, /*preserveUuids=*/true);
    CHECK(root != ecs::NullEntity);
    CHECK(copy.FindByUUID(f.ChildUuid).has_value());
    CHECK(copy.FindByUUID(f.LeafUuid).has_value());
    CHECK(copy.World().HasComponent<MeshRendererComponent>(*copy.FindByUUID(f.ChildUuid)));
    CHECK(copy.World().GetComponent<TransformComponent>(*copy.FindByUUID(f.ChildUuid)).position == glm::vec3(1.0f, 2.0f, 3.0f));

    // SubtreeEntities walks root-first (same order as the snapshot).
    // NOTE: bind the optional to a name — `*f.scene.FindByUUID(...)` dereferenced inline
    // dangles once the temporary optional is destroyed at the end of the CHECK_EQ initializer.
    auto childAgain = f.scene.FindByUUID(f.ChildUuid);
    REQUIRE(childAgain.has_value());
    const auto order = f.scene.SubtreeEntities(*childAgain);
    CHECK_EQ(order.size(), size_t(2));
    CHECK_EQ(order.front(), *childAgain);
}

RAVEN_TEST("phase2: destroy compound restores two independent subtrees in one entry") {
    Scene scene{"T"};
    ecs::Entity a = scene.CreateEntity("A");
    ecs::Entity b = scene.CreateEntity("B");
    scene.World().AddComponent<PointLightComponent>(b, PointLightComponent{});
    const UUID ua = scene.World().GetComponent<UuidComponent>(a).id;
    const UUID ub = scene.World().GetComponent<UuidComponent>(b).id;

    CommandStack stack;
    stack.BeginCompound("Delete Selection");
    stack.PushExecuted(std::make_shared<EntityDestroyCommand>(scene, ua));
    stack.PushExecuted(std::make_shared<EntityDestroyCommand>(scene, ub));
    stack.EndCompound();
    CHECK_EQ(stack.Depth(), size_t(1));
    CHECK(!scene.FindByUUID(ua).has_value());
    CHECK(!scene.FindByUUID(ub).has_value());

    CHECK(stack.Undo());   // one entry undoes the whole selection
    CHECK(scene.FindByUUID(ua).has_value());
    CHECK(scene.FindByUUID(ub).has_value());
    CHECK(scene.World().HasComponent<PointLightComponent>(*scene.FindByUUID(ub)));
}

RAVEN_TEST("phase2: gizmo command restores before-state; coalesced pushes collapse to one entry") {
    Fixture f;
    EditorCommandStack stack;
    const auto& uuid = f.ChildUuid;

    TransformSnapshot before{glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(0.0f), glm::vec3(1.0f)};
    TransformSnapshot after{glm::vec3(4.0f, 5.0f, 6.0f), glm::vec3(0.5f), glm::vec3(2.0f)};
    stack.Push(std::make_shared<GizmoTransformCommand>(f.scene, uuid, before, after));
    CHECK_EQ(stack.Raw().Depth(), size_t(1));
    auto& t = f.scene.World().GetComponent<TransformComponent>(*f.scene.FindByUUID(uuid));
    CHECK(t.position == glm::vec3(4.0f, 5.0f, 6.0f));
    CHECK(stack.Raw().Undo());
    auto& t2 = f.scene.World().GetComponent<TransformComponent>(*f.scene.FindByUUID(uuid));
    CHECK(t2.position == glm::vec3(1.0f, 2.0f, 3.0f));

    // programmatic burst through the debounce path: three pushes, one history entry
    EditorCommandStack debounced;
    const float quiet = 10.0f;   // never expires on its own here — flush explicitly
    for (int i = 1; i <= 3; ++i) {
        TransformSnapshot a{glm::vec3(1.0f + i, 2.0f, 3.0f), glm::vec3(0.0f), glm::vec3(1.0f)};
        debounced.PushDebounced("gizmo:burst", std::make_shared<GizmoTransformCommand>(f.scene, uuid, before, a), quiet);
    }
    CHECK_EQ(debounced.PendingDebounceCount(), size_t(1));
    debounced.FlushDebounce();
    CHECK_EQ(debounced.Raw().Depth(), size_t(1));   // three pushes collapsed
    CHECK(debounced.Raw().UndoName() == "Gizmo Transform");
}

RAVEN_TEST("phase2: material snapshot debounce merges a burst into one entry") {
    int applierCalls = 0;
    std::string lastApplied;
    std::function<void(const std::string&)> applier = [&](const std::string& json) {
        ++applierCalls;
        lastApplied = json;
    };
    UUID material{};   // stand-in asset id — the material system is Phase 6
    EditorCommandStack stack;
    stack.PushDebounced("mat:" + material.ToString(),
                        std::make_shared<MaterialSnapshotCommand>(material, "before", "after-1", applier));
    stack.PushDebounced("mat:" + material.ToString(),
                        std::make_shared<MaterialSnapshotCommand>(material, "after-1", "after-2", applier));
    stack.PushDebounced("mat:" + material.ToString(),
                        std::make_shared<MaterialSnapshotCommand>(material, "after-2", "after-3", applier));
    CHECK_EQ(stack.PendingDebounceCount(), size_t(1));
    stack.FlushDebounce();
    CHECK_EQ(stack.Raw().Depth(), size_t(1));
    CHECK(lastApplied == std::string("after-3"));
    applierCalls = 0;
    CHECK(stack.Raw().Undo());
    CHECK(lastApplied == std::string("before"));   // first before-state won
    CHECK_EQ(applierCalls, 1);
}

RAVEN_TEST("import settings snapshot debounce merges a burst into one entry") {
    int applierCalls = 0;
    std::string lastApplied;
    std::function<void(const std::string&)> applier = [&](const std::string& json) {
        ++applierCalls;
        lastApplied = json;
    };
    UUID asset{};   // stand-in asset id
    EditorCommandStack stack;
    stack.PushDebounced("importSettings:" + asset.ToString(),
                        std::make_shared<AssetMetaSnapshotCommand>(asset, "before", "after-1", applier));
    stack.PushDebounced("importSettings:" + asset.ToString(),
                        std::make_shared<AssetMetaSnapshotCommand>(asset, "after-1", "after-2", applier));
    stack.PushDebounced("importSettings:" + asset.ToString(),
                        std::make_shared<AssetMetaSnapshotCommand>(asset, "after-2", "after-3", applier));
    CHECK_EQ(stack.PendingDebounceCount(), size_t(1));
    stack.FlushDebounce();
    CHECK_EQ(stack.Raw().Depth(), size_t(1));
    CHECK(stack.Raw().UndoName() == std::string("Import Settings Snapshot"));
    CHECK(lastApplied == std::string("after-3"));
    applierCalls = 0;
    CHECK(stack.Raw().Undo());
    CHECK(lastApplied == std::string("before"));   // first before-state won
    CHECK_EQ(applierCalls, 1);
    CHECK(stack.Raw().Redo());
    CHECK(lastApplied == std::string("after-3"));  // redo replays the last after-state
}

RAVEN_TEST("phase2: multi-edit compound — two property writes, one undo restores both") {
    Fixture f;
    CommandStack stack;
    stack.BeginCompound("Multi Edit (2)");
    stack.PushExecuted(MakePropertyCommand(f.scene, f.ParentUuid, &TransformComponent::scale, glm::vec3(2.0f), "Scale"));
    stack.PushExecuted(MakePropertyCommand(f.scene, f.ChildUuid, &TransformComponent::scale, glm::vec3(2.0f), "Scale"));
    stack.EndCompound();

    auto& pt = f.scene.World().GetComponent<TransformComponent>(*f.scene.FindByUUID(f.ParentUuid));
    auto& ct = f.scene.World().GetComponent<TransformComponent>(*f.scene.FindByUUID(f.ChildUuid));
    CHECK(pt.scale == glm::vec3(2.0f));
    CHECK(ct.scale == glm::vec3(2.0f));
    CHECK_EQ(stack.Depth(), size_t(1));
    CHECK(stack.Undo());
    auto& pt2 = f.scene.World().GetComponent<TransformComponent>(*f.scene.FindByUUID(f.ParentUuid));
    auto& ct2 = f.scene.World().GetComponent<TransformComponent>(*f.scene.FindByUUID(f.ChildUuid));
    CHECK(pt2.scale == glm::vec3(1.0f));
    CHECK(ct2.scale == glm::vec3(1.0f));
}

RAVEN_TEST("phase2: live-coalesced pushes execute immediately and fold into one entry") {
    Fixture f;
    EditorCommandStack stack;
    const auto& uuid = f.ChildUuid;
    auto& world = f.scene.World();

    const glm::vec3 burst[3] = {glm::vec3(10.0f, 2.0f, 3.0f), glm::vec3(20.0f, 2.0f, 3.0f), glm::vec3(30.0f, 2.0f, 3.0f)};
    for (const glm::vec3& target : burst) {
        stack.PushCoalesced("prop:pos:" + uuid.ToString(),
                            MakePropertyCommand(f.scene, uuid, &TransformComponent::position, target, "Move"));
    }
    // All three applied immediately (live), folded into a single history entry.
    CHECK(world.GetComponent<TransformComponent>(*f.scene.FindByUUID(uuid)).position == glm::vec3(30.0f, 2.0f, 3.0f));
    CHECK_EQ(stack.Raw().Depth(), size_t(1));

    // Burst seals when the quiet window expires; the next push opens a new entry.
    stack.DrainLiveBursts(1.0f);
    CHECK_EQ(stack.PendingLiveBurstCount(), size_t(0));
    stack.PushCoalesced("prop:pos:" + uuid.ToString(),
                        MakePropertyCommand(f.scene, uuid, &TransformComponent::position, glm::vec3(40.0f, 2.0f, 3.0f), "Move"));
    CHECK_EQ(stack.Raw().Depth(), size_t(2));

    CHECK(stack.Raw().Undo());   // back to end-of-burst (30,2,3)
    CHECK(world.GetComponent<TransformComponent>(*f.scene.FindByUUID(uuid)).position == glm::vec3(30.0f, 2.0f, 3.0f));
    CHECK(stack.Raw().Undo());   // back to before the first burst (1,2,3) — first before won
    CHECK(world.GetComponent<TransformComponent>(*f.scene.FindByUUID(uuid)).position == glm::vec3(1.0f, 2.0f, 3.0f));
}

RAVEN_TEST("phase2: selection singleton add/toggle/clear + version bumps") {
    Fixture f;
    ClearSelection();
    const uint64_t v0 = SelectionVersion();
    SelectOnly(f.ParentUuid);
    SelectAdd(f.ChildUuid);
    CHECK_EQ(SelectionCount(), size_t(2));
    CHECK(PrimarySelection() == f.ChildUuid);
    CHECK(SelectionVersion() > v0);
    SelectToggle(f.ChildUuid);   // toggles off
    CHECK_EQ(SelectionCount(), size_t(1));
    CHECK(PrimarySelection() == f.ParentUuid);
    SelectToggle(f.ChildUuid);   // toggles on again
    CHECK_EQ(SelectionCount(), size_t(2));
    SelectRemove(f.ParentUuid);
    CHECK_EQ(SelectionCount(), size_t(1));
    ClearSelection();
    CHECK(!HasSelection());
    CHECK(PrimarySelection().IsNil());
}

// --- MultiEdit fan-out (§6.3) -------------------------------------------------

RAVEN_TEST("phase2: multi-edit of 3 entities — one command, one undo restores all") {
    Fixture f;
    // Give all three entities a transform (they all have one); set distinct scales.
    auto& wp = f.scene.World();
    wp.GetComponent<TransformComponent>(*f.scene.FindByUUID(f.ParentUuid)).scale = glm::vec3(1.0f);
    wp.GetComponent<TransformComponent>(*f.scene.FindByUUID(f.ChildUuid)).scale = glm::vec3(2.0f);
    wp.GetComponent<TransformComponent>(*f.scene.FindByUUID(f.LeafUuid)).scale = glm::vec3(3.0f);
    f.scene.ClearDirty();

    CommandStack stack;
    auto cmd = MakeMultiEditCommand<TransformComponent>(
        f.scene, {f.ParentUuid, f.ChildUuid, f.LeafUuid},
        &TransformComponent::scale, glm::vec3(9.0f), "Scale");
    stack.PushExecuted(cmd);

    CHECK_EQ(cmd->TargetCount(), size_t(3));
    CHECK(stack.UndoName() == "Scale (3)");   // count suffix for the history row
    CHECK_EQ(stack.Depth(), size_t(1));
    CHECK(wp.GetComponent<TransformComponent>(*f.scene.FindByUUID(f.ParentUuid)).scale == glm::vec3(9.0f));
    CHECK(wp.GetComponent<TransformComponent>(*f.scene.FindByUUID(f.ChildUuid)).scale  == glm::vec3(9.0f));
    CHECK(wp.GetComponent<TransformComponent>(*f.scene.FindByUUID(f.LeafUuid)).scale   == glm::vec3(9.0f));

    CHECK(stack.Undo());   // ONE undo restores all three before-states
    CHECK(wp.GetComponent<TransformComponent>(*f.scene.FindByUUID(f.ParentUuid)).scale == glm::vec3(1.0f));
    CHECK(wp.GetComponent<TransformComponent>(*f.scene.FindByUUID(f.ChildUuid)).scale  == glm::vec3(2.0f));
    CHECK(wp.GetComponent<TransformComponent>(*f.scene.FindByUUID(f.LeafUuid)).scale   == glm::vec3(3.0f));
}

RAVEN_TEST("phase2: multi-edit skips targets lacking the component") {
    Fixture f;
    auto& wp = f.scene.World();
    wp.AddComponent<RigidbodyComponent>(*f.scene.FindByUUID(f.ChildUuid), RigidbodyComponent{});
    // Parent and Leaf have no rigidbody — only Child counts as a target.

    auto cmd = MakeMultiEditCommand<RigidbodyComponent>(
        f.scene, {f.ParentUuid, f.ChildUuid, f.LeafUuid},
        &RigidbodyComponent::mass, 7.5f, "Mass");
    CHECK_EQ(cmd->TargetCount(), size_t(1));

    CommandStack stack;
    stack.PushExecuted(cmd);
    CHECK(wp.GetComponent<RigidbodyComponent>(*f.scene.FindByUUID(f.ChildUuid)).mass == 7.5f);
    CHECK(stack.UndoName() == "Mass (1)");
}

RAVEN_TEST("phase2: multi-edit coalescing keeps first before, last after") {
    Fixture f;
    EditorCommandStack stack;
    const std::vector<UUID> targets{f.ParentUuid, f.ChildUuid};
    const std::string tag = "multiedit:scale:" + f.ParentUuid.ToString();

    stack.PushCoalesced(tag, MakeMultiEditCommand<TransformComponent>(
        f.scene, targets, &TransformComponent::scale, glm::vec3(2.0f), "Scale"));
    stack.PushCoalesced(tag, MakeMultiEditCommand<TransformComponent>(
        f.scene, targets, &TransformComponent::scale, glm::vec3(4.0f), "Scale"));
    stack.PushCoalesced(tag, MakeMultiEditCommand<TransformComponent>(
        f.scene, targets, &TransformComponent::scale, glm::vec3(8.0f), "Scale"));

    CHECK_EQ(stack.Raw().Depth(), size_t(1));   // the whole gesture is one entry
    CHECK(stack.Raw().UndoName() == "Scale (2)");
    CHECK(stack.Raw().Undo());                   // back to the ORIGINAL before-states (1,1,1)
    CHECK(f.scene.World().GetComponent<TransformComponent>(*f.scene.FindByUUID(f.ParentUuid)).scale == glm::vec3(1.0f));
    CHECK(f.scene.World().GetComponent<TransformComponent>(*f.scene.FindByUUID(f.ChildUuid)).scale  == glm::vec3(1.0f));
}

// --- ComponentJsonCommand (copy/paste overflow) ----------------------------------

RAVEN_TEST("phase2: JSON paste onto missing component — undo removes it again") {
    Fixture f;
    auto& wp = f.scene.World();
    CHECK(!wp.HasComponent<RigidbodyComponent>(*f.scene.FindByUUID(f.ParentUuid)));

    const nlohmann::json after = {{"type", "dynamic"},
                                  {"mass", 5.0f},
                                  {"linearVelocity", {1.0f, 0.0f, 0.0f}},
                                  {"useGravity", false},
                                  {"restitution", 0.9f}};
    CommandStack stack;
    auto cmd = std::make_shared<ComponentJsonCommand>(f.scene, f.ParentUuid, "rigidbody", after, "Paste Component");
    stack.PushExecuted(cmd);

    REQUIRE(wp.HasComponent<RigidbodyComponent>(*f.scene.FindByUUID(f.ParentUuid)));
    CHECK(wp.GetComponent<RigidbodyComponent>(*f.scene.FindByUUID(f.ParentUuid)).mass == 5.0f);

    CHECK(stack.Undo());   // component was absent before — undo removes it
    CHECK(!wp.HasComponent<RigidbodyComponent>(*f.scene.FindByUUID(f.ParentUuid)));

    CHECK(stack.Redo());   // redo re-adds with the pasted values
    REQUIRE(wp.HasComponent<RigidbodyComponent>(*f.scene.FindByUUID(f.ParentUuid)));
    CHECK(wp.GetComponent<RigidbodyComponent>(*f.scene.FindByUUID(f.ParentUuid)).mass == 5.0f);
}

RAVEN_TEST("phase2: JSON paste over existing component — undo restores previous values") {
    Fixture f;
    auto& wp = f.scene.World();
    RigidbodyComponent existing;
    existing.mass = 2.0f;
    existing.restitution = 0.1f;
    wp.AddComponent<RigidbodyComponent>(*f.scene.FindByUUID(f.ParentUuid), existing);

    const nlohmann::json after = {{"type", "static"},
                                  {"mass", 42.0f},
                                  {"linearVelocity", {0.0f, 0.0f, 0.0f}},
                                  {"useGravity", false},
                                  {"restitution", 0.95f}};
    CommandStack stack;
    stack.PushExecuted(std::make_shared<ComponentJsonCommand>(f.scene, f.ParentUuid, "rigidbody", after, "Paste Component"));
    REQUIRE(wp.HasComponent<RigidbodyComponent>(*f.scene.FindByUUID(f.ParentUuid)));
    CHECK(wp.GetComponent<RigidbodyComponent>(*f.scene.FindByUUID(f.ParentUuid)).mass == 42.0f);

    CHECK(stack.Undo());   // restores the previous JSON (mass 2, restitution 0.1)
    const auto& restored = wp.GetComponent<RigidbodyComponent>(*f.scene.FindByUUID(f.ParentUuid));
    CHECK(restored.mass == 2.0f);
    CHECK(restored.restitution == 0.1f);
    CHECK(restored.type == RigidbodyType::Dynamic);
}

// --- Scene::RemoveComponentKey ----------------------------------------------------

RAVEN_TEST("phase2: RemoveComponentKey refuses entity backbone; removes removable keys") {
    Fixture f;
    auto& wp = f.scene.World();
    auto parent = *f.scene.FindByUUID(f.ParentUuid);

    CHECK(!f.scene.RemoveComponentKey(parent, "transform"));
    CHECK(!f.scene.RemoveComponentKey(parent, "name"));
    CHECK(!f.scene.RemoveComponentKey(parent, "uuid"));
    CHECK(!f.scene.RemoveComponentKey(parent, "no-such-key"));
    CHECK(wp.HasComponent<TransformComponent>(parent));   // backbone untouched

    wp.AddComponent<RigidbodyComponent>(parent, RigidbodyComponent{});
    CHECK(wp.HasComponent<RigidbodyComponent>(parent));
    CHECK(f.scene.RemoveComponentKey(parent, "rigidbody"));
    CHECK(!wp.HasComponent<RigidbodyComponent>(parent));
}
