// RavenEngine — Lua scripting. Each ScriptComponent points at a .lua file under the
// project assets root. Scripts define onStart(entity) and/or onUpdate(entity, dt).
// The `Raven` global exposes a tiny engine API (transform access, logging, time).
// `entity` is an opaque integer handle: the raw ECS entity value.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

#include "raven/ecs/World.h"

struct lua_State;
typedef struct lua_Debug lua_Debug;

namespace raven {

class Scene;

class ScriptSystem {
public:
    ScriptSystem() = default;
    ~ScriptSystem();
    ScriptSystem(const ScriptSystem&) = delete;
    ScriptSystem& operator=(const ScriptSystem&) = delete;

    bool Initialize(const std::filesystem::path& assetsRoot = {});
    void Shutdown();
    bool IsInitialized() const { return m_State != nullptr; }

    // Must be called before Step(); binds the Raven API to this scene.
    void Bind(Scene* scene);

    // Calls onStart (once per enabled script entity), then onUpdate.
    void Step(float deltaSeconds, float timeSeconds);

    // Hot reload: drops all instances of a script file; they are recreated on next Step.
    void ReloadScript(const std::filesystem::path& path);

    // Watchdog: each Lua call (onStart/onUpdate) runs under an instruction budget.
    // A runaway script hits the budget, is aborted with an error (captured in
    // LastError()) and counted in WatchdogTrips(); other instances keep running.
    // 0 disables the watchdog.
    void SetWatchdogInstructionLimit(uint64_t instructionsPerCall) { m_WatchdogLimit = instructionsPerCall; }
    uint64_t WatchdogInstructionLimit() const { return m_WatchdogLimit; }
    uint64_t WatchdogTrips() const { return m_WatchdogTrips; }

    const std::string& LastError() const { return m_LastError; }

private:
    static int l_RavenLog(lua_State* L);
    static int l_GetPosition(lua_State* L);
    static int l_SetPosition(lua_State* L);
    static int l_GetRotation(lua_State* L);
    static int l_SetRotation(lua_State* L);
    static int l_GetScale(lua_State* L);
    static int l_SetScale(lua_State* L);
    static int l_GetName(lua_State* L);
    static int l_Time(lua_State* L);
    static int l_DeltaTime(lua_State* L);
    static void l_WatchdogHook(lua_State* L, lua_Debug* ar);

    // The C API functions need access to the current scene/time — stored here.
    static ScriptSystem* s_Active;

    bool CallScriptFunction(int envRef, const char* name, ecs::Entity entity, float dt);
    void CallWithWatchdog(lua_State* L);
    void ClearWatchdog(lua_State* L);
    bool LoadInstance(uint64_t entityRaw, const std::filesystem::path& absolutePath);
    void DropInstance(uint64_t entityRaw);

    struct ScriptInstance {
        int envRef = -1;
        bool started = false;
        std::string path;
    };

    lua_State* m_State = nullptr;
    Scene* m_Scene = nullptr;
    std::filesystem::path m_AssetsRoot;
    std::string m_LastError;
    float m_DeltaTime = 0.0f;
    float m_Time = 0.0f;
    uint64_t m_WatchdogLimit = 5000000;   // instructions per Lua call; 0 = disabled
    uint64_t m_WatchdogTrips = 0;         // times the watchdog aborted a script call
    std::unordered_map<uint64_t, ScriptInstance> m_Instances;  // raw entity -> instance
};

} // namespace raven
