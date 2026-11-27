// RavenEditor — inspector registry implementation (§6.3). Built-in drawers + the
// shared section/add-component plumbing declared in ComponentDrawerRegistry.h.
#include "editor/inspectors/ComponentDrawerRegistry.h"

#include "editor/Icons.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <string>

#include "raven/scene/Components.h"

namespace raven::editor {

ComponentClipboard& InspectorClipboard() {
    static ComponentClipboard s_Clipboard;
    return s_Clipboard;
}

namespace {

nlohmann::json JsonVec3(const glm::vec3& v) { return nlohmann::json::array({v.x, v.y, v.z}); }
nlohmann::json JsonUuid(const raven::UUID& id) { return id.IsNil() ? nlohmann::json("") : nlohmann::json(id.ToString()); }

// Fan a command out to the primary + every multi-target, wrapped in one compound when > 1.
void PushForTargets(Scene& scene, const std::vector<raven::UUID>& targets,
                    const std::function<std::shared_ptr<raven::ICommand>(Scene&, const raven::UUID&)>& factory,
                    const std::string& compoundName) {
    if (targets.empty()) return;
    if (targets.size() == 1) {
        Commands().Push(factory(scene, targets.front()));
        return;
    }
    std::string label = compoundName + " (" + std::to_string(targets.size()) + ")";
    Commands().Raw().BeginCompound(label);
    for (const raven::UUID& id : targets)
        if (auto cmd = factory(scene, id)) Commands().Push(cmd);
    Commands().Raw().EndCompound();
}

std::vector<raven::UUID> SelectedTargetsWithout(Scene& scene,
                                                const std::function<bool(const ecs::World&, ecs::Entity)>& present) {
    std::vector<raven::UUID> targets;
    for (const raven::UUID& id : Selection()) {
        auto e = scene.FindByUUID(id);
        if (e && !present(scene.World(), *e)) targets.push_back(id);
    }
    return targets;
}

// Overflow context menu shared by every section.
void DrawOverflowMenu(Scene& scene, ecs::Entity entity, const ComponentDrawerEntry& entry,
                      const std::vector<raven::UUID>& targetsWith) {
    if (!ImGui::BeginPopupContextItem()) return;   // opened by the header right-click

    const nlohmann::json current = scene.SerializeComponent(entity, entry.key);

    if (ImGui::MenuItem("Copy Component")) {
        InspectorClipboard().valid = true;
        InspectorClipboard().key = entry.key;
        InspectorClipboard().json = current;
        ImGui::SetClipboardText(current.dump(2).c_str());
    }
    const bool canPaste = InspectorClipboard().valid && InspectorClipboard().key == entry.key;
    if (ImGui::MenuItem("Paste Component", nullptr, false, canPaste)) {
        const nlohmann::json after = InspectorClipboard().json;
        PushForTargets(scene, targetsWith,
                       [&after, &entry](Scene& s, const raven::UUID& id) {
                           return std::make_shared<ComponentJsonCommand>(s, id, entry.key, after, "Paste Component");
                       },
                       "Paste Component");
    }
    if (ImGui::MenuItem("Reset", nullptr, false, entry.canReset && entry.defaultJson != nullptr)) {
        const nlohmann::json def = entry.defaultJson();
        PushForTargets(scene, targetsWith,
                       [&def, &entry](Scene& s, const raven::UUID& id) {
                           return std::make_shared<ComponentJsonCommand>(s, id, entry.key, def, "Reset Component");
                       },
                       "Reset Component");
    }
    ImGui::Separator();
    const bool canRemove = entry.canRemove && entry.removeCommand != nullptr;
    if (ImGui::MenuItem("Remove Component", nullptr, false, canRemove)) {
        PushForTargets(scene, targetsWith, entry.removeCommand, "Remove Component");
    }
    if (ImGui::MenuItem("Copy As JSON")) {
        ImGui::SetClipboardText(current.dump(2).c_str());
    }
    ImGui::EndPopup();
}

} // namespace

void DrawComponentSection(Scene& scene, ecs::Entity entity, const raven::UUID& uuid, const ComponentDrawerEntry& entry) {
    ecs::World& world = scene.World();
    if (!entry.present(world, entity)) return;

    // Other selected entities holding the same component — the MultiEdit fan-out set.
    std::vector<raven::UUID> others;
    for (const raven::UUID& id : Selection()) {
        if (id == uuid) continue;
        auto other = scene.FindByUUID(id);
        if (other && entry.present(world, *other)) others.push_back(id);
    }
    std::vector<raven::UUID> targetsWith;
    targetsWith.push_back(uuid);
    targetsWith.insert(targetsWith.end(), others.begin(), others.end());

    const bool multi = targetsWith.size() > 1;
    std::string header = multi ? entry.name + " (" + std::to_string(targetsWith.size()) + ")" : entry.name;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap;
    const bool open = ImGui::CollapsingHeader(header.c_str(), flags);
    DrawOverflowMenu(scene, entity, entry, targetsWith);
    if (!open) return;

    DrawerContext ctx;
    ctx.scene = &scene;
    ctx.entity = entity;
    ctx.uuid = uuid;
    ctx.others = std::move(others);
    entry.draw(ctx);
}

void DrawAddComponentPopup(Scene& scene, ecs::Entity entity, const raven::UUID& uuid) {
    ecs::World& world = scene.World();
    if (ImGui::Button(ICON_ADD "  Add Component", ImVec2(-1.0f, 0.0f))) ImGui::OpenPopup("addComponentPopup");

    if (!ImGui::BeginPopup("addComponentPopup")) return;

    static char search[64] = {};
    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputTextWithHint("##addComponentSearch", "search…", search, sizeof(search));
    const std::string needle = [] {
        std::string out;
        for (const char* c = search; *c; ++c) out += static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
        return out;
    }();

    // Category-grouped list of *addable*, *currently absent* entries.
    std::map<std::string, std::vector<const ComponentDrawerEntry*>> byCategory;
    for (const auto& entry : ComponentDrawerRegistry::Instance().Entries()) {
        if (!entry.canAdd || !entry.addCommand) continue;
        if (entry.present(world, entity)) continue;
        if (!needle.empty()) {
            std::string hay = entry.name + " " + entry.category;
            std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (hay.find(needle) == std::string::npos) continue;
        }
        byCategory[entry.category].push_back(&entry);
    }

    if (byCategory.empty()) {
        ImGui::TextDisabled("Nothing to add");
    }
    for (const auto& [category, entries] : byCategory) {
        ImGui::SeparatorText(category.c_str());
        for (const ComponentDrawerEntry* entry : entries) {
            if (ImGui::MenuItem(entry->name.c_str())) {
                // Add applies to every selected entity lacking the component (MultiEdit).
                std::vector<raven::UUID> targets = SelectedTargetsWithout(scene, entry->present);
                PushForTargets(scene, targets, entry->addCommand, "Add Component");
            }
        }
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// Built-in drawers
// ---------------------------------------------------------------------------

void EnsureBuiltinDrawersRegistered() {
    static const bool registered = [] {
        auto& registry = ComponentDrawerRegistry::Instance();

        // ---- Transform -----------------------------------------------------
        {
            ComponentDrawerEntry e;
            e.key = "transform";
            e.name = "Transform";
            e.category = "Core";
            e.canRemove = false;
            e.canReset = true;
            e.present = [](const ecs::World& w, ecs::Entity ent) { return w.HasComponent<TransformComponent>(ent); };
            e.defaultJson = [] {
                return nlohmann::json{{"position", JsonVec3(glm::vec3(0.0f))},
                                      {"rotation", JsonVec3(glm::vec3(0.0f))},
                                      {"scale", JsonVec3(glm::vec3(1.0f))}};
            };
            e.draw = [](DrawerContext& ctx) {
                PropertyRow(ctx, "Position", &TransformComponent::position, "Edit Position", 0.05f, -100000.0f, 100000.0f);
                glm::vec3 rotDeg = glm::degrees(ctx.scene->World().GetComponent<TransformComponent>(ctx.entity).rotation);
                // Rotation is edited in degrees; rotation stores radians — handle with a dedicated write path.
                if (ImGui::DragFloat3("Rotation", &rotDeg.x, 0.5f)) {
                    glm::vec3 radians = glm::radians(rotDeg);
                    std::vector<raven::UUID> targets;
                    targets.push_back(ctx.uuid);
                    targets.insert(targets.end(), ctx.others.begin(), ctx.others.end());
                    const std::string tag = "prop:" + ctx.uuid.ToString() + ":Edit Rotation";
                    if (targets.size() == 1) {
                        Commands().PushCoalesced(tag, MakePropertyCommand(*ctx.scene, ctx.uuid, &TransformComponent::rotation, std::move(radians), "Edit Rotation"));
                    } else {
                        Commands().PushCoalesced(tag, MakeMultiEditCommand(*ctx.scene, std::move(targets), &TransformComponent::rotation, std::move(radians), "Edit Rotation"));
                    }
                }
                PropertyRow(ctx, "Scale", &TransformComponent::scale, "Edit Scale", 0.02f, 0.001f, 1000.0f);
            };
            registry.Register(std::move(e));
        }

        // ---- Mesh Renderer -------------------------------------------------
        {
            ComponentDrawerEntry e;
            e.key = "meshRenderer";
            e.name = "Mesh Renderer";
            e.category = "Rendering";
            e.present = [](const ecs::World& w, ecs::Entity ent) { return w.HasComponent<MeshRendererComponent>(ent); };
            e.addCommand = [](Scene& s, const raven::UUID& id) { return std::make_shared<ComponentAddCommand<MeshRendererComponent>>(s, id, MeshRendererComponent{}, "Add Mesh Renderer"); };
            e.removeCommand = [](Scene& s, const raven::UUID& id) { return std::make_shared<ComponentRemoveCommand<MeshRendererComponent>>(s, id, "Remove Mesh Renderer"); };
            e.defaultJson = [] {
                return nlohmann::json{{"mesh", JsonUuid(raven::UUID::Nil())}, {"material", JsonUuid(raven::UUID::Nil())},
                                      {"castShadow", true}, {"visible", true}};
            };
            e.draw = [](DrawerContext& ctx) {
                const auto& mr = ctx.scene->World().GetComponent<MeshRendererComponent>(ctx.entity);
                ImGui::TextDisabled("Mesh");
                ImGui::SameLine();
                ImGui::TextUnformatted(mr.mesh.ToString().c_str());
                ImGui::TextDisabled("Material");
                ImGui::SameLine();
                ImGui::TextUnformatted(mr.material.ToString().c_str());
                PropertyRow(ctx, "Visible", &MeshRendererComponent::visible, "Toggle Visibility", 0.0f, 0.0f, 1.0f);
                PropertyRow(ctx, "Cast Shadow", &MeshRendererComponent::castShadow, "Toggle Cast Shadow", 0.0f, 0.0f, 1.0f);
            };
            registry.Register(std::move(e));
        }

        // ---- LOD -----------------------------------------------------------
        {
            ComponentDrawerEntry e;
            e.key = "lod";
            e.name = "LOD";
            e.category = "Rendering";
            e.present = [](const ecs::World& w, ecs::Entity ent) { return w.HasComponent<LodComponent>(ent); };
            e.addCommand = [](Scene& s, const raven::UUID& id) { return std::make_shared<ComponentAddCommand<LodComponent>>(s, id, LodComponent{}, "Add LOD"); };
            e.removeCommand = [](Scene& s, const raven::UUID& id) { return std::make_shared<ComponentRemoveCommand<LodComponent>>(s, id, "Remove LOD"); };
            e.defaultJson = [] {
                return nlohmann::json{{"levels", nlohmann::json::array()}, {"screenHeights", std::vector<float>{1.0f}},
                                      {"boundingRadius", 1.0f}, {"lodBias", 0.0f}};
            };
            e.draw = [](DrawerContext& ctx) {
                const auto& lod = ctx.scene->World().GetComponent<LodComponent>(ctx.entity);
                ImGui::Text("Levels: %zu", lod.levels.size());
                PropertyRow(ctx, "Bounding Radius", &LodComponent::boundingRadius, "Edit Bounding Radius", 0.05f, 0.05f, 10000.0f);
                PropertyRow(ctx, "LOD Bias", &LodComponent::lodBias, "Edit LOD Bias", 0.01f, -4.0f, 4.0f);
            };
            registry.Register(std::move(e));
        }

        // ---- Directional Light ---------------------------------------------
        {
            ComponentDrawerEntry e;
            e.key = "directionalLight";
            e.name = "Directional Light";
            e.category = "Light";
            e.present = [](const ecs::World& w, ecs::Entity ent) { return w.HasComponent<DirectionalLightComponent>(ent); };
            e.addCommand = [](Scene& s, const raven::UUID& id) { return std::make_shared<ComponentAddCommand<DirectionalLightComponent>>(s, id, DirectionalLightComponent{}, "Add Directional Light"); };
            e.removeCommand = [](Scene& s, const raven::UUID& id) { return std::make_shared<ComponentRemoveCommand<DirectionalLightComponent>>(s, id, "Remove Directional Light"); };
            e.defaultJson = [] {
                return nlohmann::json{{"direction", JsonVec3(glm::vec3(0.0f, -1.0f, 0.0f))},
                                      {"color", JsonVec3(glm::vec3(1.0f))},
                                      {"intensity", 3.0f},
                                      {"castShadow", true}};
            };
            e.draw = [](DrawerContext& ctx) {
                PropertyRow(ctx, "Direction", &DirectionalLightComponent::direction, "Edit Direction", 0.01f, -1.0f, 1.0f);
                ColorRow(ctx, "Color", &DirectionalLightComponent::color, "Edit Light Color");
                PropertyRow(ctx, "Intensity", &DirectionalLightComponent::intensity, "Edit Intensity", 0.05f, 0.0f, 100.0f);
                PropertyRow(ctx, "Cast Shadow", &DirectionalLightComponent::castShadow, "Toggle Light Shadow", 0.0f, 0.0f, 1.0f);
            };
            registry.Register(std::move(e));
        }

        // ---- Point Light ---------------------------------------------------
        {
            ComponentDrawerEntry e;
            e.key = "pointLight";
            e.name = "Point Light";
            e.category = "Light";
            e.present = [](const ecs::World& w, ecs::Entity ent) { return w.HasComponent<PointLightComponent>(ent); };
            e.addCommand = [](Scene& s, const raven::UUID& id) { return std::make_shared<ComponentAddCommand<PointLightComponent>>(s, id, PointLightComponent{}, "Add Point Light"); };
            e.removeCommand = [](Scene& s, const raven::UUID& id) { return std::make_shared<ComponentRemoveCommand<PointLightComponent>>(s, id, "Remove Point Light"); };
            e.defaultJson = [] {
                return nlohmann::json{{"color", JsonVec3(glm::vec3(1.0f))}, {"intensity", 10.0f}, {"radius", 5.0f}};
            };
            e.draw = [](DrawerContext& ctx) {
                ColorRow(ctx, "Color", &PointLightComponent::color, "Edit Light Color");
                PropertyRow(ctx, "Intensity", &PointLightComponent::intensity, "Edit Intensity", 0.05f, 0.0f, 1000.0f);
                PropertyRow(ctx, "Radius", &PointLightComponent::radius, "Edit Radius", 0.05f, 0.0f, 1000.0f);
            };
            registry.Register(std::move(e));
        }

        // ---- Camera --------------------------------------------------------
        {
            ComponentDrawerEntry e;
            e.key = "camera";
            e.name = "Camera";
            e.category = "Core";
            e.present = [](const ecs::World& w, ecs::Entity ent) { return w.HasComponent<CameraComponent>(ent); };
            e.addCommand = [](Scene& s, const raven::UUID& id) { return std::make_shared<ComponentAddCommand<CameraComponent>>(s, id, CameraComponent{}, "Add Camera"); };
            e.removeCommand = [](Scene& s, const raven::UUID& id) { return std::make_shared<ComponentRemoveCommand<CameraComponent>>(s, id, "Remove Camera"); };
            e.defaultJson = [] {
                return nlohmann::json{{"fovDegrees", 60.0f}, {"nearPlane", 0.1f}, {"farPlane", 250.0f}, {"isPrimary", false}};
            };
            e.draw = [](DrawerContext& ctx) {
                PropertyRow(ctx, "FOV (deg)", &CameraComponent::fovDegrees, "Edit FOV", 0.5f, 10.0f, 160.0f);
                PropertyRow(ctx, "Near Plane", &CameraComponent::nearPlane, "Edit Near Plane", 0.01f, 0.001f, 100.0f);
                PropertyRow(ctx, "Far Plane", &CameraComponent::farPlane, "Edit Far Plane", 0.5f, 1.0f, 100000.0f);
                PropertyRow(ctx, "Primary", &CameraComponent::isPrimary, "Toggle Primary Camera", 0.0f, 0.0f, 1.0f);
            };
            registry.Register(std::move(e));
        }

        // ---- Rigidbody -----------------------------------------------------
        {
            ComponentDrawerEntry e;
            e.key = "rigidbody";
            e.name = "Rigidbody";
            e.category = "Physics";
            e.present = [](const ecs::World& w, ecs::Entity ent) { return w.HasComponent<RigidbodyComponent>(ent); };
            e.addCommand = [](Scene& s, const raven::UUID& id) { return std::make_shared<ComponentAddCommand<RigidbodyComponent>>(s, id, RigidbodyComponent{}, "Add Rigidbody"); };
            e.removeCommand = [](Scene& s, const raven::UUID& id) { return std::make_shared<ComponentRemoveCommand<RigidbodyComponent>>(s, id, "Remove Rigidbody"); };
            e.defaultJson = [] {
                return nlohmann::json{{"type", "dynamic"}, {"mass", 1.0f}, {"linearVelocity", JsonVec3(glm::vec3(0.0f))},
                                      {"useGravity", true}, {"restitution", 0.35f}};
            };
            e.draw = [](DrawerContext& ctx) {
                const RigidbodyType currentType = ctx.scene->World().GetComponent<RigidbodyComponent>(ctx.entity).type;
                const char* types[] = {"Static", "Dynamic"};
                int typeIndex = currentType == RigidbodyType::Dynamic ? 1 : 0;
                if (ImGui::Combo("Type", &typeIndex, types, 2)) {
                    const RigidbodyType selected = typeIndex == 1 ? RigidbodyType::Dynamic : RigidbodyType::Static;
                    // Combo applies uniformly across the multi-edit set — one compound entry.
                    std::vector<raven::UUID> targets;
                    targets.push_back(ctx.uuid);
                    targets.insert(targets.end(), ctx.others.begin(), ctx.others.end());
                    PushForTargets(*ctx.scene, targets,
                                   [selected](Scene& s, const raven::UUID& id) {
                                       return MakePropertyCommand(s, id, &RigidbodyComponent::type, selected, "Set Rigidbody Type");
                                   },
                                   "Set Rigidbody Type");
                }
                PropertyRow(ctx, "Mass", &RigidbodyComponent::mass, "Edit Mass", 0.05f, 0.01f, 10000.0f);
                PropertyRow(ctx, "Restitution", &RigidbodyComponent::restitution, "Edit Restitution", 0.01f, 0.0f, 1.0f);
                PropertyRow(ctx, "Use Gravity", &RigidbodyComponent::useGravity, "Toggle Gravity", 0.0f, 0.0f, 1.0f);
                PropertyRow(ctx, "Awake", &RigidbodyComponent::isAwake, "Toggle Awake", 0.0f, 0.0f, 1.0f);
                const auto& rb = ctx.scene->World().GetComponent<RigidbodyComponent>(ctx.entity);
                ImGui::TextDisabled("Velocity: %.3f, %.3f, %.3f", rb.linearVelocity.x, rb.linearVelocity.y, rb.linearVelocity.z);
            };
            registry.Register(std::move(e));
        }

        // ---- Box Collider --------------------------------------------------
        {
            ComponentDrawerEntry e;
            e.key = "boxCollider";
            e.name = "Box Collider";
            e.category = "Physics";
            e.present = [](const ecs::World& w, ecs::Entity ent) { return w.HasComponent<BoxColliderComponent>(ent); };
            e.addCommand = [](Scene& s, const raven::UUID& id) { return std::make_shared<ComponentAddCommand<BoxColliderComponent>>(s, id, BoxColliderComponent{}, "Add Box Collider"); };
            e.removeCommand = [](Scene& s, const raven::UUID& id) { return std::make_shared<ComponentRemoveCommand<BoxColliderComponent>>(s, id, "Remove Box Collider"); };
            e.defaultJson = [] { return nlohmann::json{{"halfExtents", JsonVec3(glm::vec3(0.5f))}}; };
            e.draw = [](DrawerContext& ctx) {
                PropertyRow(ctx, "Half Extents", &BoxColliderComponent::halfExtents, "Edit Half Extents", 0.02f, 0.001f, 100.0f);
            };
            registry.Register(std::move(e));
        }

        // ---- Audio Source --------------------------------------------------
        {
            ComponentDrawerEntry e;
            e.key = "audioSource";
            e.name = "Audio Source";
            e.category = "Audio";
            e.present = [](const ecs::World& w, ecs::Entity ent) { return w.HasComponent<AudioSourceComponent>(ent); };
            e.addCommand = [](Scene& s, const raven::UUID& id) { return std::make_shared<ComponentAddCommand<AudioSourceComponent>>(s, id, AudioSourceComponent{}, "Add Audio Source"); };
            e.removeCommand = [](Scene& s, const raven::UUID& id) { return std::make_shared<ComponentRemoveCommand<AudioSourceComponent>>(s, id, "Remove Audio Source"); };
            e.defaultJson = [] {
                return nlohmann::json{{"clip", JsonUuid(raven::UUID::Nil())}, {"volume", 1.0f},
                                      {"loop", false}, {"playOnStart", false}, {"is3D", true}};
            };
            e.draw = [](DrawerContext& ctx) {
                const auto& a = ctx.scene->World().GetComponent<AudioSourceComponent>(ctx.entity);
                ImGui::TextDisabled("Clip");
                ImGui::SameLine();
                ImGui::TextUnformatted(a.clip.ToString().c_str());
                PropertyRow(ctx, "Volume", &AudioSourceComponent::volume, "Edit Volume", 0.01f, 0.0f, 1.0f);
                PropertyRow(ctx, "Loop", &AudioSourceComponent::loop, "Toggle Loop", 0.0f, 0.0f, 1.0f);
                PropertyRow(ctx, "Play On Start", &AudioSourceComponent::playOnStart, "Toggle Play On Start", 0.0f, 0.0f, 1.0f);
                PropertyRow(ctx, "Is 3D", &AudioSourceComponent::is3D, "Toggle Is 3D", 0.0f, 0.0f, 1.0f);
            };
            registry.Register(std::move(e));
        }

        // ---- Script --------------------------------------------------------
        {
            ComponentDrawerEntry e;
            e.key = "script";
            e.name = "Script";
            e.category = "Scripting";
            e.present = [](const ecs::World& w, ecs::Entity ent) { return w.HasComponent<ScriptComponent>(ent); };
            e.addCommand = [](Scene& s, const raven::UUID& id) { return std::make_shared<ComponentAddCommand<ScriptComponent>>(s, id, ScriptComponent{}, "Add Script"); };
            e.removeCommand = [](Scene& s, const raven::UUID& id) { return std::make_shared<ComponentRemoveCommand<ScriptComponent>>(s, id, "Remove Script"); };
            e.defaultJson = [] { return nlohmann::json{{"scriptPath", ""}, {"enabled", true}}; };
            e.draw = [](DrawerContext& ctx) {
                const auto& sc = ctx.scene->World().GetComponent<ScriptComponent>(ctx.entity);
                char pathBuf[512];
                std::snprintf(pathBuf, sizeof(pathBuf), "%s", sc.scriptPath.c_str());
                ImGui::InputText("Path", pathBuf, sizeof(pathBuf), ImGuiInputTextFlags_ReadOnly);
                PropertyRow(ctx, "Enabled", &ScriptComponent::enabled, "Toggle Script", 0.0f, 0.0f, 1.0f);
            };
            registry.Register(std::move(e));
        }

        // ---- Prefab Instance (read-only) -----------------------------------
        {
            ComponentDrawerEntry e;
            e.key = "prefabInstance";
            e.name = "Prefab Instance";
            e.category = "Prefab";
            e.canAdd = false;
            e.canReset = false;
            e.present = [](const ecs::World& w, ecs::Entity ent) { return w.HasComponent<PrefabInstanceComponent>(ent); };
            e.removeCommand = [](Scene& s, const raven::UUID& id) { return std::make_shared<ComponentRemoveCommand<PrefabInstanceComponent>>(s, id, "Remove Prefab Instance"); };
            e.draw = [](DrawerContext& ctx) {
                const auto& p = ctx.scene->World().GetComponent<PrefabInstanceComponent>(ctx.entity);
                ImGui::TextDisabled("Prefab");
                ImGui::SameLine();
                ImGui::TextUnformatted(p.prefab.ToString().c_str());
            };
            registry.Register(std::move(e));
        }

        return true;
    }();
    (void)registered;
}

} // namespace raven::editor
