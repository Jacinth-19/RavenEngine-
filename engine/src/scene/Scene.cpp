#include "raven/scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "raven/core/FileSystem.h"
#include "raven/core/Logger.h"

namespace raven {

namespace {

nlohmann::json Vec3ToJson(const glm::vec3& v) { return nlohmann::json::array({v.x, v.y, v.z}); }
nlohmann::json Vec4ToJson(const glm::vec4& v) { return nlohmann::json::array({v.x, v.y, v.z, v.w}); }

glm::vec3 JsonToVec3(const nlohmann::json& j, const glm::vec3& fallback = glm::vec3(0.0f)) {
    if (!j.is_array() || j.size() < 3) return fallback;
    return glm::vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
}

nlohmann::json UUIDToJson(const UUID& id) { return id.IsNil() ? nlohmann::json("") : nlohmann::json(id.ToString()); }
UUID JsonToUUID(const nlohmann::json& j) {
    if (!j.is_string()) return UUID::Nil();
    auto parsed = UUID::Parse(j.get<std::string>());
    return parsed.value_or(UUID::Nil());
}

} // namespace

// ---------------------------------------------------------------- creation

ecs::Entity Scene::CreateEntityInternal(const std::string& name, bool markDirty) {
    ecs::Entity e = m_World.CreateEntity();
    m_World.AddComponent<NameComponent>(e, NameComponent{name});
    m_World.AddComponent<TransformComponent>(e);
    if (markDirty) m_Dirty = true;
    return e;
}

ecs::Entity Scene::CreateEntity(const std::string& name) {
    return CreateEntityInternal(name, true);
}

void Scene::DestroyEntity(ecs::Entity entity) {
    if (!m_World.IsAlive(entity)) return;

    // Detach from parent so the child list stays consistent.
    if (m_World.HasComponent<TransformComponent>(entity)) {
        auto& transform = m_World.GetComponent<TransformComponent>(entity);
        ecs::Entity parent = transform.parent;
        if (parent != ecs::NullEntity && m_World.IsAlive(parent)) {
            auto& children = m_World.GetComponent<TransformComponent>(parent).children;
            children.erase(std::remove(children.begin(), children.end(), entity), children.end());
        }
    }

    // Destroy children recursively (copy because the vector mutates).
    if (m_World.HasComponent<TransformComponent>(entity)) {
        std::vector<ecs::Entity> children = m_World.GetComponent<TransformComponent>(entity).children;
        for (ecs::Entity child : children) DestroyEntity(child);
    }

    m_World.DestroyEntity(entity);
    m_Dirty = true;
}

ecs::Entity Scene::DuplicateEntity(ecs::Entity entity) {
    if (!m_World.IsAlive(entity)) return ecs::NullEntity;

    std::string baseName = "Entity";
    if (m_World.HasComponent<NameComponent>(entity)) baseName = m_World.GetComponent<NameComponent>(entity).name;

    ecs::Entity copy = CreateEntityInternal(baseName + " (Copy)", false);

    auto copyComp = [&](auto tag, auto&& fn) { fn(tag); };
    (void)copyComp;

    if (m_World.HasComponent<TransformComponent>(entity)) {
        const auto& src = m_World.GetComponent<TransformComponent>(entity);
        auto& dst = m_World.GetComponent<TransformComponent>(copy);
        dst.position = src.position;
        dst.rotation = src.rotation;
        dst.scale = src.scale;
        dst.parent = src.parent;
        if (src.parent != ecs::NullEntity && m_World.IsAlive(src.parent))
            m_World.GetComponent<TransformComponent>(src.parent).children.push_back(copy);
    }
    if (m_World.HasComponent<MeshRendererComponent>(entity))
        m_World.AddComponent<MeshRendererComponent>(copy, m_World.GetComponent<MeshRendererComponent>(entity));
    if (m_World.HasComponent<DirectionalLightComponent>(entity))
        m_World.AddComponent<DirectionalLightComponent>(copy, m_World.GetComponent<DirectionalLightComponent>(entity));
    if (m_World.HasComponent<PointLightComponent>(entity))
        m_World.AddComponent<PointLightComponent>(copy, m_World.GetComponent<PointLightComponent>(entity));
    if (m_World.HasComponent<CameraComponent>(entity))
        m_World.AddComponent<CameraComponent>(copy, m_World.GetComponent<CameraComponent>(entity));
    if (m_World.HasComponent<RigidbodyComponent>(entity))
        m_World.AddComponent<RigidbodyComponent>(copy, m_World.GetComponent<RigidbodyComponent>(entity));
    if (m_World.HasComponent<BoxColliderComponent>(entity))
        m_World.AddComponent<BoxColliderComponent>(copy, m_World.GetComponent<BoxColliderComponent>(entity));
    if (m_World.HasComponent<AudioSourceComponent>(entity))
        m_World.AddComponent<AudioSourceComponent>(copy, m_World.GetComponent<AudioSourceComponent>(entity));
    if (m_World.HasComponent<ScriptComponent>(entity))
        m_World.AddComponent<ScriptComponent>(copy, m_World.GetComponent<ScriptComponent>(entity));

    // Duplicate children recursively.
    std::vector<ecs::Entity> children = m_World.GetComponent<TransformComponent>(entity).children;
    for (ecs::Entity child : children) {
        ecs::Entity childCopy = DuplicateEntity(child);
        if (childCopy != ecs::NullEntity) SetParent(childCopy, copy);
    }

    m_Dirty = true;
    return copy;
}

void Scene::Clear() {
    m_World.Clear();
    m_Name = "Untitled";
    m_Path.clear();
    m_Dirty = false;
}

// ---------------------------------------------------------------- hierarchy

void Scene::RebuildChildLists() {
    // Reset child vectors.
    for (ecs::Entity e : m_World.Entities()) {
        if (m_World.HasComponent<TransformComponent>(e)) m_World.GetComponent<TransformComponent>(e).children.clear();
    }
    for (ecs::Entity e : m_World.Entities()) {
        if (!m_World.HasComponent<TransformComponent>(e)) continue;
        auto& t = m_World.GetComponent<TransformComponent>(e);
        if (t.parent != ecs::NullEntity && m_World.IsAlive(t.parent) && t.parent != e) {
            m_World.GetComponent<TransformComponent>(t.parent).children.push_back(e);
        } else {
            t.parent = ecs::NullEntity;
        }
    }
}

bool Scene::IsDescendantOf(ecs::Entity entity, ecs::Entity potentialAncestor) const {
    ecs::Entity current = entity;
    for (int depth = 0; depth < 64; ++depth) {
        if (!m_World.IsAlive(current) || !m_World.HasComponent<TransformComponent>(current)) return false;
        ecs::Entity parent = m_World.GetComponent<TransformComponent>(current).parent;
        if (parent == ecs::NullEntity) return false;
        if (parent == potentialAncestor) return true;
        current = parent;
    }
    return false;
}

void Scene::SetParent(ecs::Entity child, ecs::Entity newParent) {
    if (child == newParent) return;
    if (!m_World.IsAlive(child)) return;
    if (newParent != ecs::NullEntity) {
        if (!m_World.IsAlive(newParent)) return;
        if (IsDescendantOf(newParent, child)) {
            RAVEN_LOG_WARN("SetParent rejected: would create a hierarchy cycle");
            return;
        }
    }

    auto& transform = m_World.GetComponent<TransformComponent>(child);
    ecs::Entity oldParent = transform.parent;
    if (oldParent == newParent) return;

    if (oldParent != ecs::NullEntity && m_World.IsAlive(oldParent)) {
        auto& children = m_World.GetComponent<TransformComponent>(oldParent).children;
        children.erase(std::remove(children.begin(), children.end(), child), children.end());
    }

    transform.parent = newParent;
    if (newParent != ecs::NullEntity) m_World.GetComponent<TransformComponent>(newParent).children.push_back(child);
    m_Dirty = true;
}

glm::mat4 Scene::GetLocalMatrix(const TransformComponent& t) const {
    glm::mat4 local(1.0f);
    local = glm::translate(local, t.position);
    local *= glm::mat4_cast(glm::quat(t.rotation));
    local = glm::scale(local, t.scale);
    return local;
}

glm::mat4 Scene::GetWorldMatrix(ecs::Entity entity) const {
    if (!m_World.IsAlive(entity) || !m_World.HasComponent<TransformComponent>(entity)) return glm::mat4(1.0f);
    const auto& t = m_World.GetComponent<TransformComponent>(entity);
    glm::mat4 local = GetLocalMatrix(t);
    if (t.parent == ecs::NullEntity || t.parent == entity) return local;
    return GetWorldMatrix(t.parent) * local;
}

std::vector<ecs::Entity> Scene::RootEntities() const {
    std::vector<ecs::Entity> roots;
    for (ecs::Entity e : m_World.Entities()) {
        if (!m_World.HasComponent<TransformComponent>(e)) continue;
        ecs::Entity parent = m_World.GetComponent<TransformComponent>(e).parent;
        if (parent == ecs::NullEntity || !m_World.IsAlive(parent)) roots.push_back(e);
    }
    return roots;
}

const std::vector<ecs::Entity>& Scene::ChildrenOf(ecs::Entity entity) const {
    static const std::vector<ecs::Entity> kEmpty;
    if (!m_World.IsAlive(entity) || !m_World.HasComponent<TransformComponent>(entity)) return kEmpty;
    return m_World.GetComponent<TransformComponent>(entity).children;
}

std::optional<ecs::Entity> Scene::FindByName(const std::string& name) const {
    for (ecs::Entity e : m_World.Entities()) {
        if (m_World.HasComponent<NameComponent>(e) && m_World.GetComponent<NameComponent>(e).name == name)
            return e;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------- serialize

std::string Scene::SerializeToString() const {
    nlohmann::json root;
    root["schemaVersion"] = kSchemaVersion;
    root["name"] = m_Name;

    const auto& entities = m_World.Entities();
    // Stable file ids = position within this list.
    std::unordered_map<ecs::Entity, int> fileIds;
    fileIds.reserve(entities.size());
    for (size_t i = 0; i < entities.size(); ++i) fileIds[entities[i]] = static_cast<int>(i);

    nlohmann::json jsonEntities = nlohmann::json::array();
    for (size_t i = 0; i < entities.size(); ++i) {
        ecs::Entity e = entities[i];
        nlohmann::json components = nlohmann::json::object();

        if (m_World.HasComponent<NameComponent>(e)) {
            components["name"] = {{"name", m_World.GetComponent<NameComponent>(e).name}};
        }
        if (m_World.HasComponent<TransformComponent>(e)) {
            const auto& t = m_World.GetComponent<TransformComponent>(e);
            int parentFileId = -1;
            auto it = fileIds.find(t.parent);
            if (it != fileIds.end()) parentFileId = it->second;
            components["transform"] = {{"position", Vec3ToJson(t.position)},
                                       {"rotation", Vec3ToJson(t.rotation)},
                                       {"scale", Vec3ToJson(t.scale)},
                                       {"parent", parentFileId}};
        }
        if (m_World.HasComponent<MeshRendererComponent>(e)) {
            const auto& m = m_World.GetComponent<MeshRendererComponent>(e);
            components["meshRenderer"] = {{"mesh", UUIDToJson(m.mesh)},
                                          {"material", UUIDToJson(m.material)},
                                          {"castShadow", m.castShadow},
                                          {"visible", m.visible}};
        }
        if (m_World.HasComponent<DirectionalLightComponent>(e)) {
            const auto& l = m_World.GetComponent<DirectionalLightComponent>(e);
            components["directionalLight"] = {{"direction", Vec3ToJson(l.direction)},
                                              {"color", Vec3ToJson(l.color)},
                                              {"intensity", l.intensity},
                                              {"castShadow", l.castShadow}};
        }
        if (m_World.HasComponent<PointLightComponent>(e)) {
            const auto& l = m_World.GetComponent<PointLightComponent>(e);
            components["pointLight"] = {{"color", Vec3ToJson(l.color)},
                                        {"intensity", l.intensity},
                                        {"radius", l.radius}};
        }
        if (m_World.HasComponent<CameraComponent>(e)) {
            const auto& c = m_World.GetComponent<CameraComponent>(e);
            components["camera"] = {{"fovDegrees", c.fovDegrees},
                                    {"nearPlane", c.nearPlane},
                                    {"farPlane", c.farPlane},
                                    {"isPrimary", c.isPrimary}};
        }
        if (m_World.HasComponent<RigidbodyComponent>(e)) {
            const auto& r = m_World.GetComponent<RigidbodyComponent>(e);
            components["rigidbody"] = {{"type", r.type == RigidbodyType::Dynamic ? "dynamic" : "static"},
                                       {"mass", r.mass},
                                       {"linearVelocity", Vec3ToJson(r.linearVelocity)},
                                       {"useGravity", r.useGravity},
                                       {"restitution", r.restitution}};
        }
        if (m_World.HasComponent<BoxColliderComponent>(e)) {
            const auto& c = m_World.GetComponent<BoxColliderComponent>(e);
            components["boxCollider"] = {{"halfExtents", Vec3ToJson(c.halfExtents)}};
        }
        if (m_World.HasComponent<AudioSourceComponent>(e)) {
            const auto& a = m_World.GetComponent<AudioSourceComponent>(e);
            components["audioSource"] = {{"clip", UUIDToJson(a.clip)},
                                         {"volume", a.volume},
                                         {"loop", a.loop},
                                         {"playOnStart", a.playOnStart},
                                         {"is3D", a.is3D}};
        }
        if (m_World.HasComponent<ScriptComponent>(e)) {
            const auto& s = m_World.GetComponent<ScriptComponent>(e);
            components["script"] = {{"scriptPath", s.scriptPath}, {"enabled", s.enabled}};
        }
        if (m_World.HasComponent<PrefabInstanceComponent>(e)) {
            const auto& p = m_World.GetComponent<PrefabInstanceComponent>(e);
            components["prefabInstance"] = {{"prefab", UUIDToJson(p.prefab)}};
        }

        jsonEntities.push_back({{"id", fileIds[e]}, {"components", components}});
    }
    root["entities"] = std::move(jsonEntities);
    return root.dump(2);
}

bool Scene::LoadFromString(const std::string& text, std::string* error) {
    nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded()) {
        if (error) *error = "scene file is not valid JSON";
        return false;
    }
    int schema = root.value("schemaVersion", 0);
    if (schema > kSchemaVersion) {
        if (error) *error = "scene file schema (" + std::to_string(schema) + ") is newer than this engine supports (" + std::to_string(kSchemaVersion) + ")";
        return false;
    }
    if (!root.contains("entities") || !root["entities"].is_array()) {
        if (error) *error = "scene file has no 'entities' array";
        return false;
    }

    Clear();
    m_Name = root.value("name", std::string("Untitled"));

    // Pass 1: create entities in file order; remember file id -> Entity.
    std::vector<ecs::Entity> fileMap;
    fileMap.reserve(root["entities"].size());
    for (const auto& entry : root["entities"]) {
        std::string name = "Entity";
        if (entry.contains("components") && entry["components"].contains("name"))
            name = entry["components"]["name"].value("name", name);
        fileMap.push_back(CreateEntityInternal(name, false));
    }

    // Pass 2: apply components.
    for (size_t i = 0; i < root["entities"].size(); ++i) {
        const auto& components = root["entities"][i].value("components", nlohmann::json::object());
        ecs::Entity e = fileMap[i];

        if (components.contains("transform")) {
            const auto& t = components["transform"];
            auto& transform = m_World.GetComponent<TransformComponent>(e);
            transform.position = JsonToVec3(t.value("position", nlohmann::json::array()));
            transform.rotation = JsonToVec3(t.value("rotation", nlohmann::json::array()));
            transform.scale = JsonToVec3(t.value("scale", nlohmann::json::array({1, 1, 1})), glm::vec3(1.0f));
            int parentFileId = t.value("parent", -1);
            if (parentFileId >= 0 && static_cast<size_t>(parentFileId) < fileMap.size())
                transform.parent = fileMap[static_cast<size_t>(parentFileId)];
            else
                transform.parent = ecs::NullEntity;
        }
        if (components.contains("meshRenderer")) {
            const auto& m = components["meshRenderer"];
            MeshRendererComponent mc;
            mc.mesh = JsonToUUID(m.value("mesh", nlohmann::json()));
            mc.material = JsonToUUID(m.value("material", nlohmann::json()));
            mc.castShadow = m.value("castShadow", true);
            mc.visible = m.value("visible", true);
            m_World.AddComponent<MeshRendererComponent>(e, mc);
        }
        if (components.contains("directionalLight")) {
            const auto& l = components["directionalLight"];
            DirectionalLightComponent lc;
            lc.direction = glm::normalize(JsonToVec3(l.value("direction", nlohmann::json::array()), glm::vec3(0, -1, 0)));
            lc.color = JsonToVec3(l.value("color", nlohmann::json::array({1, 1, 1})), glm::vec3(1));
            lc.intensity = l.value("intensity", 3.0f);
            lc.castShadow = l.value("castShadow", true);
            m_World.AddComponent<DirectionalLightComponent>(e, lc);
        }
        if (components.contains("pointLight")) {
            const auto& l = components["pointLight"];
            PointLightComponent lc;
            lc.color = JsonToVec3(l.value("color", nlohmann::json::array({1, 1, 1})), glm::vec3(1));
            lc.intensity = l.value("intensity", 10.0f);
            lc.radius = l.value("radius", 5.0f);
            m_World.AddComponent<PointLightComponent>(e, lc);
        }
        if (components.contains("camera")) {
            const auto& c = components["camera"];
            CameraComponent cc;
            cc.fovDegrees = c.value("fovDegrees", 60.0f);
            cc.nearPlane = c.value("nearPlane", 0.1f);
            cc.farPlane = c.value("farPlane", 250.0f);
            cc.isPrimary = c.value("isPrimary", false);
            m_World.AddComponent<CameraComponent>(e, cc);
        }
        if (components.contains("rigidbody")) {
            const auto& r = components["rigidbody"];
            RigidbodyComponent rc;
            rc.type = r.value("type", std::string("dynamic")) == "static" ? RigidbodyType::Static : RigidbodyType::Dynamic;
            rc.mass = r.value("mass", 1.0f);
            rc.linearVelocity = JsonToVec3(r.value("linearVelocity", nlohmann::json::array()));
            rc.useGravity = r.value("useGravity", true);
            rc.restitution = r.value("restitution", 0.35f);
            m_World.AddComponent<RigidbodyComponent>(e, rc);
        }
        if (components.contains("boxCollider")) {
            const auto& c = components["boxCollider"];
            BoxColliderComponent bc;
            bc.halfExtents = JsonToVec3(c.value("halfExtents", nlohmann::json::array({0.5, 0.5, 0.5})), glm::vec3(0.5f));
            m_World.AddComponent<BoxColliderComponent>(e, bc);
        }
        if (components.contains("audioSource")) {
            const auto& a = components["audioSource"];
            AudioSourceComponent ac;
            ac.clip = JsonToUUID(a.value("clip", nlohmann::json()));
            ac.volume = a.value("volume", 1.0f);
            ac.loop = a.value("loop", false);
            ac.playOnStart = a.value("playOnStart", false);
            ac.is3D = a.value("is3D", true);
            m_World.AddComponent<AudioSourceComponent>(e, ac);
        }
        if (components.contains("script")) {
            const auto& s = components["script"];
            ScriptComponent sc;
            sc.scriptPath = s.value("scriptPath", std::string());
            sc.enabled = s.value("enabled", true);
            m_World.AddComponent<ScriptComponent>(e, sc);
        }
        if (components.contains("prefabInstance")) {
            PrefabInstanceComponent pc;
            pc.prefab = JsonToUUID(components["prefabInstance"].value("prefab", nlohmann::json()));
            m_World.AddComponent<PrefabInstanceComponent>(e, pc);
        }
    }

    RebuildChildLists();
    m_Dirty = false;
    return true;
}

bool Scene::Save(const std::filesystem::path& output, std::string* error) {
    std::string text = SerializeToString();
    if (!fs::WriteText(output, text, error)) return false;
    m_Path = output;
    m_Dirty = false;
    return true;
}

bool Scene::Save(std::string* error) {
    if (m_Path.empty()) {
        if (error) *error = "scene has no path; use Save(path)";
        return false;
    }
    return Save(m_Path, error);
}

bool Scene::Load(const std::filesystem::path& path, std::string* error) {
    auto text = fs::ReadText(path, error);
    if (!text) return false;
    if (!LoadFromString(*text, error)) return false;
    m_Path = path;
    m_Dirty = false;
    return true;
}

} // namespace raven
