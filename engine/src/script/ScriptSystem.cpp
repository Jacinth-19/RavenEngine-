#include <utility>
#include "raven/script/ScriptSystem.h"

// Lua headers ship without extern "C" guards — wrap them, or C++ mangles every call.
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <cstring>
#include <unordered_set>

#include "raven/scene/Scene.h"
#include "raven/scene/Components.h"
#include "raven/core/FileSystem.h"
#include "raven/core/Logger.h"

namespace raven {

ScriptSystem* ScriptSystem::s_Active = nullptr;

ScriptSystem::~ScriptSystem() {
    Shutdown();
}

bool ScriptSystem::Initialize(const std::filesystem::path& assetsRoot) {
    if (m_State) return true;
    m_AssetsRoot = assetsRoot;
    m_State = luaL_newstate();
    if (!m_State) {
        m_LastError = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(m_State);

    // Register the Raven API table.
    lua_getglobal(m_State, "_G");  // stack: table
    lua_newtable(m_State);         // stack: _G, ravenTable
    auto reg = [&](const char* name, lua_CFunction fn) {
        lua_pushcfunction(m_State, fn);
        lua_setfield(m_State, -2, name);
    };
    reg("log", l_RavenLog);
    reg("getPosition", l_GetPosition);
    reg("setPosition", l_SetPosition);
    reg("getRotation", l_GetRotation);
    reg("setRotation", l_SetRotation);
    reg("getScale", l_GetScale);
    reg("setScale", l_SetScale);
    reg("getName", l_GetName);
    reg("time", l_Time);
    reg("deltaTime", l_DeltaTime);
    lua_setfield(m_State, -2, "Raven");  // _G.Raven = table
    lua_pop(m_State, 1);

    s_Active = this;
    return true;
}

void ScriptSystem::Shutdown() {
    if (!m_State) return;
    m_Instances.clear();  // env refs die with the state
    lua_close(m_State);
    m_State = nullptr;
    if (s_Active == this) s_Active = nullptr;
}

void ScriptSystem::Bind(Scene* scene) {
    m_Scene = scene;
}

void ScriptSystem::DropInstance(uint64_t entityRaw) {
    auto it = m_Instances.find(entityRaw);
    if (it == m_Instances.end()) return;
    if (m_State && it->second.envRef != LUA_NOREF) luaL_unref(m_State, LUA_REGISTRYINDEX, it->second.envRef);
    m_Instances.erase(it);
}

bool ScriptSystem::LoadInstance(uint64_t entityRaw, const std::filesystem::path& absolutePath) {
    if (!m_State) return false;
    lua_State* L = m_State;

    // env = table with metatable { __index = _G }
    lua_newtable(L);                                  // env
    lua_newtable(L);                                  // metatable
    lua_getglobal(L, "_G");                           // mt, _G
    lua_setfield(L, -2, "__index");                   // mt.__index = _G
    lua_setmetatable(L, -2);                          // setmetatable(env, mt)

    if (luaL_loadfile(L, absolutePath.string().c_str()) != LUA_OK) {
        m_LastError = std::string("lua load error (") + absolutePath.string() + "): " + lua_tostring(L, -1);
        RAVEN_LOG_ERROR("ScriptSystem: %s", m_LastError.c_str());
        lua_pop(L, 2);  // chunk err + env
        return false;
    }
    // Give the chunk our fresh env as its _ENV (first upvalue of a Lua chunk).
    lua_pushvalue(L, -2);                 // chunk, env, env
    if (lua_setupvalue(L, -2, 1) == nullptr) {
        // C function chunk (shouldn't happen) — pop carefully.
        lua_pop(L, 3);
        m_LastError = "script chunk has no _ENV upvalue";
        return false;
    }
    // Stack: env, chunk
    CallWithWatchdog(L);
    const int loadStatus = lua_pcall(L, 0, 0, 0);
    ClearWatchdog(L);
    if (loadStatus != LUA_OK) {
        m_LastError = std::string("lua exec error (") + absolutePath.string() + "): " + lua_tostring(L, -1);
        RAVEN_LOG_ERROR("ScriptSystem: %s", m_LastError.c_str());
        lua_pop(L, 2);  // err + env
        return false;
    }
    // Stack: env
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);  // pops env, stores in registry
    ScriptInstance instance;
    instance.envRef = ref;
    instance.started = false;
    instance.path = absolutePath.string();
    m_Instances[entityRaw] = instance;
    return true;
}

bool ScriptSystem::CallScriptFunction(int envRef, const char* name, ecs::Entity entity, float dt) {
    lua_State* L = m_State;
    if (!L) return false;
    lua_rawgeti(L, LUA_REGISTRYINDEX, envRef);   // env
    lua_getfield(L, -1, name);                   // env, fn
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return true;  // not an error — script just doesn't define this hook
    }
    lua_pushinteger(L, static_cast<lua_Integer>(entity));
    if (std::strcmp(name, "onUpdate") == 0) lua_pushnumber(L, static_cast<lua_Number>(dt));
    int nargs = std::strcmp(name, "onUpdate") == 0 ? 2 : 1;
    CallWithWatchdog(L);
    const int callStatus = lua_pcall(L, nargs, 0, 0);
    ClearWatchdog(L);
    if (callStatus != LUA_OK) {
        m_LastError = std::string("lua runtime error in ") + name + ": " + lua_tostring(L, -1);
        RAVEN_LOG_ERROR("ScriptSystem: %s (entity %u)", m_LastError.c_str(), static_cast<uint32_t>(entity));
        lua_pop(L, 2);  // err + env
        return false;
    }
    lua_pop(L, 1);  // env
    return true;
}

void ScriptSystem::Step(float deltaSeconds, float timeSeconds) {
    if (!m_State || !m_Scene) return;
    m_DeltaTime = deltaSeconds;
    m_Time = timeSeconds;
    s_Active = this;

    auto& world = m_Scene->World();

    // Collect live (entity, path) pairs.
    std::unordered_set<uint64_t> live;
    std::vector<std::pair<ecs::Entity, std::string>> active;
    world.ForEach<ScriptComponent>([&](ecs::Entity e, const ScriptComponent& sc) {
        if (!sc.enabled || sc.scriptPath.empty()) return;
        std::filesystem::path absolute = m_AssetsRoot.empty() ? std::filesystem::path(sc.scriptPath)
                                                             : (m_AssetsRoot / sc.scriptPath);
        live.insert(static_cast<uint64_t>(e));
        active.emplace_back(e, absolute.string());
    });

    // Drop instances whose entity vanished / script disabled.
    std::vector<uint64_t> toDrop;
    for (const auto& [raw, instance] : m_Instances) {
        if (!live.count(raw)) toDrop.push_back(raw);
    }
    for (uint64_t raw : toDrop) DropInstance(raw);

    // Load new / reload-changed instances.
    for (auto& [entity, path] : active) {
        uint64_t raw = static_cast<uint64_t>(entity);
        auto it = m_Instances.find(raw);
        if (it != m_Instances.end() && it->second.path != path) {
            DropInstance(raw);  // script path changed → fresh instance
            it = m_Instances.end();
        }
        if (it == m_Instances.end()) LoadInstance(raw, path);
    }

    // Dispatch hooks.
    for (auto& [entity, path] : active) {
        uint64_t raw = static_cast<uint64_t>(entity);
        auto it = m_Instances.find(raw);
        if (it == m_Instances.end()) continue;
        if (!it->second.started) {
            if (CallScriptFunction(it->second.envRef, "onStart", entity, deltaSeconds)) it->second.started = true;
        }
        CallScriptFunction(it->second.envRef, "onUpdate", entity, deltaSeconds);
    }
}

void ScriptSystem::ReloadScript(const std::filesystem::path& path) {
    std::string target = fs::NormalizeSlashes(path).string();
    // Match exactly, or by suffix only on a path-component boundary (so reloading
    // "spin.lua" never drops instances of "myspin.lua").
    auto matches = [&](const std::string& instPath) {
        if (instPath == target) return true;
        if (target.size() >= instPath.size()) return false;
        const size_t offset = instPath.size() - target.size();
        if (instPath.compare(offset, target.size(), target) != 0) return false;
        return offset == 0 || instPath[offset - 1] == '/';
    };
    std::vector<uint64_t> toDrop;
    for (const auto& [raw, instance] : m_Instances) {
        std::string instPath = fs::NormalizeSlashes(fs::Path(instance.path)).string();
        if (matches(instPath)) toDrop.push_back(raw);
    }
    for (uint64_t raw : toDrop) DropInstance(raw);
    if (!toDrop.empty()) RAVEN_LOG_INFO("ScriptSystem: reloaded %zu instance(s) of %s", toDrop.size(), target.c_str());
}

void ScriptSystem::l_WatchdogHook(lua_State* L, lua_Debug* /*ar*/) {
    // Fired once per budget; abort the running chunk with a Lua error.
    lua_sethook(L, nullptr, 0, 0);
    if (s_Active) ++s_Active->m_WatchdogTrips;
    luaL_error(L, "script exceeded the per-call instruction budget");
}

void ScriptSystem::CallWithWatchdog(lua_State* L) {
    if (m_WatchdogLimit > 0) {
        const lua_Integer budget = static_cast<lua_Integer>(m_WatchdogLimit > 0x7FFFFFFF ? 0x7FFFFFFF : m_WatchdogLimit);
        lua_sethook(L, l_WatchdogHook, LUA_MASKCOUNT, static_cast<int>(budget));
    }
}

void ScriptSystem::ClearWatchdog(lua_State* L) {
    lua_sethook(L, nullptr, 0, 0);
}

// ------------------------------------------------------------------ C API

int ScriptSystem::l_RavenLog(lua_State* L) {
    const char* msg = luaL_optstring(L, 1, "");
    RAVEN_LOG_INFO("[lua] %s", msg);
    return 0;
}

static ecs::Entity EntityFromArg(lua_State* L, int arg) {
    lua_Integer raw = luaL_checkinteger(L, arg);
    if (raw < 0 || static_cast<uint64_t>(raw) > static_cast<uint64_t>(ecs::NullEntity))
        return ecs::NullEntity;  // out of range — IsAlive() will reject it
    return static_cast<ecs::Entity>(static_cast<uint32_t>(raw));
}

int ScriptSystem::l_GetPosition(lua_State* L) {
    if (!s_Active || !s_Active->m_Scene) return luaL_error(L, "no scene bound");
    ecs::Entity e = EntityFromArg(L, 1);
    auto& world = s_Active->m_Scene->World();
    if (!world.IsAlive(e) || !world.HasComponent<TransformComponent>(e)) return luaL_error(L, "entity has no transform");
    const auto& p = world.GetComponent<TransformComponent>(e).position;
    lua_pushnumber(L, p.x); lua_pushnumber(L, p.y); lua_pushnumber(L, p.z);
    return 3;
}

int ScriptSystem::l_SetPosition(lua_State* L) {
    if (!s_Active || !s_Active->m_Scene) return luaL_error(L, "no scene bound");
    ecs::Entity e = EntityFromArg(L, 1);
    auto& world = s_Active->m_Scene->World();
    if (!world.IsAlive(e) || !world.HasComponent<TransformComponent>(e)) return luaL_error(L, "entity has no transform");
    glm::vec3 p{static_cast<float>(luaL_checknumber(L, 2)), static_cast<float>(luaL_checknumber(L, 3)), static_cast<float>(luaL_checknumber(L, 4))};
    world.GetComponent<TransformComponent>(e).position = p;
    s_Active->m_Scene->MarkDirty();
    return 0;
}

int ScriptSystem::l_GetRotation(lua_State* L) {
    if (!s_Active || !s_Active->m_Scene) return luaL_error(L, "no scene bound");
    ecs::Entity e = EntityFromArg(L, 1);
    auto& world = s_Active->m_Scene->World();
    if (!world.IsAlive(e) || !world.HasComponent<TransformComponent>(e)) return luaL_error(L, "entity has no transform");
    const auto& r = world.GetComponent<TransformComponent>(e).rotation;
    lua_pushnumber(L, r.x); lua_pushnumber(L, r.y); lua_pushnumber(L, r.z);
    return 3;
}

int ScriptSystem::l_SetRotation(lua_State* L) {
    if (!s_Active || !s_Active->m_Scene) return luaL_error(L, "no scene bound");
    ecs::Entity e = EntityFromArg(L, 1);
    auto& world = s_Active->m_Scene->World();
    if (!world.IsAlive(e) || !world.HasComponent<TransformComponent>(e)) return luaL_error(L, "entity has no transform");
    glm::vec3 r{static_cast<float>(luaL_checknumber(L, 2)), static_cast<float>(luaL_checknumber(L, 3)), static_cast<float>(luaL_checknumber(L, 4))};
    world.GetComponent<TransformComponent>(e).rotation = r;
    s_Active->m_Scene->MarkDirty();
    return 0;
}

int ScriptSystem::l_GetScale(lua_State* L) {
    if (!s_Active || !s_Active->m_Scene) return luaL_error(L, "no scene bound");
    ecs::Entity e = EntityFromArg(L, 1);
    auto& world = s_Active->m_Scene->World();
    if (!world.IsAlive(e) || !world.HasComponent<TransformComponent>(e)) return luaL_error(L, "entity has no transform");
    const auto& s = world.GetComponent<TransformComponent>(e).scale;
    lua_pushnumber(L, s.x); lua_pushnumber(L, s.y); lua_pushnumber(L, s.z);
    return 3;
}

int ScriptSystem::l_SetScale(lua_State* L) {
    if (!s_Active || !s_Active->m_Scene) return luaL_error(L, "no scene bound");
    ecs::Entity e = EntityFromArg(L, 1);
    auto& world = s_Active->m_Scene->World();
    if (!world.IsAlive(e) || !world.HasComponent<TransformComponent>(e)) return luaL_error(L, "entity has no transform");
    glm::vec3 s{static_cast<float>(luaL_checknumber(L, 2)), static_cast<float>(luaL_checknumber(L, 3)), static_cast<float>(luaL_checknumber(L, 4))};
    world.GetComponent<TransformComponent>(e).scale = s;
    s_Active->m_Scene->MarkDirty();
    return 0;
}

int ScriptSystem::l_GetName(lua_State* L) {
    if (!s_Active || !s_Active->m_Scene) return luaL_error(L, "no scene bound");
    ecs::Entity e = EntityFromArg(L, 1);
    auto& world = s_Active->m_Scene->World();
    if (!world.IsAlive(e) || !world.HasComponent<NameComponent>(e)) return luaL_error(L, "entity has no name");
    lua_pushstring(L, world.GetComponent<NameComponent>(e).name.c_str());
    return 1;
}

int ScriptSystem::l_Time(lua_State* L) {
    lua_pushnumber(L, s_Active ? static_cast<lua_Number>(s_Active->m_Time) : 0.0);
    return 1;
}

int ScriptSystem::l_DeltaTime(lua_State* L) {
    lua_pushnumber(L, s_Active ? static_cast<lua_Number>(s_Active->m_DeltaTime) : 0.0);
    return 1;
}

} // namespace raven
