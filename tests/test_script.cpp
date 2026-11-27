// Lua scripting: lifecycle hooks, watchdog abort of runaway scripts, hot-reload
// matching, and out-of-range entity handles from Lua.
#include "test_framework.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <glm/glm.hpp>

#include "raven/script/ScriptSystem.h"
#include "raven/scene/Scene.h"
#include "raven/scene/Components.h"

using namespace raven;
namespace fs = std::filesystem;

namespace {

fs::path PrepareScriptDir(const char* subdir) {
    const fs::path dir = fs::temp_directory_path() / subdir;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void WriteScript(const fs::path& dir, const char* name, const char* contents) {
    std::ofstream out(dir / name, std::ios::trunc);
    out << contents;
}

struct ScriptFixture {
    fs::path dir = PrepareScriptDir("raven_test_lua");
    Scene scene{"Scripts"};
    ScriptSystem sys;

    ScriptFixture() {
        sys.Initialize(dir);
        sys.Bind(&scene);
    }

    ecs::Entity MakeScriptedEntity(const char* name, const char* script) {
        const ecs::Entity e = scene.CreateEntity(name);
        ScriptComponent sc;
        sc.scriptPath = script;
        scene.World().AddComponent<ScriptComponent>(e, sc);
        return e;
    }

    glm::vec3 Pos(ecs::Entity e) { return scene.World().GetComponent<TransformComponent>(e).position; }
    void SetPos(ecs::Entity e, glm::vec3 p) { scene.World().GetComponent<TransformComponent>(e).position = p; }
};

} // namespace

RAVEN_TEST("script: onStart/onUpdate run and mutate the entity") {
    ScriptFixture fx;
    WriteScript(fx.dir, "mover.lua",
        "function onStart(e)\n"
        "  Raven.setPosition(e, 0.0, 5.0, 0.0)\n"
        "end\n"
        "function onUpdate(e, dt)\n"
        "  local x, y, z = Raven.getPosition(e)\n"
        "  Raven.setPosition(e, x + 1.0, y, z)\n"
        "end\n");
    const ecs::Entity e = fx.MakeScriptedEntity("Mover", "mover.lua");
    fx.sys.Step(1.0f / 60.0f, 1.0f / 60.0f);
    const glm::vec3 p = fx.Pos(e);
    CHECK(p.x == 1.0f);
    CHECK(p.y == 5.0f);
}

RAVEN_TEST("script: watchdog aborts a runaway onUpdate and other scripts still run") {
    ScriptFixture fx;
    WriteScript(fx.dir, "runaway.lua",
        "function onUpdate(e, dt)\n"
        "  while true do end\n"
        "end\n");
    WriteScript(fx.dir, "sane.lua",
        "function onUpdate(e, dt)\n"
        "  local x, y, z = Raven.getPosition(e)\n"
        "  Raven.setPosition(e, x + 1.0, y, z)\n"
        "end\n");
    const ecs::Entity runaway = fx.MakeScriptedEntity("Runaway", "runaway.lua");
    const ecs::Entity sane = fx.MakeScriptedEntity("Sane", "sane.lua");
    fx.sys.SetWatchdogInstructionLimit(100000);

    fx.sys.Step(1.0f / 60.0f, 1.0f / 60.0f);

    CHECK(!fx.sys.LastError().empty());
    CHECK(fx.sys.WatchdogTrips() >= 1);
    CHECK(fx.Pos(runaway) == glm::vec3(0.0f));      // aborted before it could do work
    CHECK(fx.Pos(sane).x == 1.0f);                  // sibling kept ticking in the same frame
}

RAVEN_TEST("script: watchdog also guards chunk load (top-level infinite loop)") {
    ScriptFixture fx;
    WriteScript(fx.dir, "loop.lua", "while true do end\n");
    const ecs::Entity e = fx.MakeScriptedEntity("Loop", "loop.lua");
    fx.sys.SetWatchdogInstructionLimit(100000);
    fx.sys.Step(1.0f / 60.0f, 1.0f / 60.0f);
    CHECK(fx.sys.WatchdogTrips() >= 1);
    CHECK(fx.Pos(e) == glm::vec3(0.0f));            // runaway chunk never established
}

RAVEN_TEST("script: ReloadScript drops only the matching file's instances") {
    ScriptFixture fx;
    WriteScript(fx.dir, "foo.lua",
        "function onStart(e)\n"
        "  Raven.setPosition(e, 100.0, 0.0, 0.0)\n"
        "end\n");
    WriteScript(fx.dir, "xfoo.lua",
        "function onStart(e)\n"
        "  Raven.setPosition(e, 999.0, 0.0, 0.0)\n"
        "end\n");
    const ecs::Entity foo = fx.MakeScriptedEntity("Foo", "foo.lua");
    const ecs::Entity xfoo = fx.MakeScriptedEntity("XFoo", "xfoo.lua");
    fx.sys.Step(1.0f / 60.0f, 1.0f / 60.0f);
    REQUIRE(fx.Pos(foo).x == 100.0f);
    REQUIRE(fx.Pos(xfoo).x == 999.0f);

    fx.SetPos(foo, {0.0f, 0.0f, 0.0f});
    fx.SetPos(xfoo, {0.0f, 0.0f, 0.0f});
    fx.sys.ReloadScript("foo.lua");   // relative name — suffix match on a path boundary
    fx.sys.Step(1.0f / 60.0f, 2.0f / 60.0f);

    CHECK(fx.Pos(foo).x == 100.0f);   // reloaded → onStart ran again
    CHECK(fx.Pos(xfoo).x == 0.0f);    // "xfoo.lua" must NOT be dropped by reloading "foo.lua"
}

RAVEN_TEST("script: out-of-range entity handle from Lua does not alias entity 0") {
    ScriptFixture fx;
    WriteScript(fx.dir, "probe.lua",
        "function onUpdate(e, dt)\n"
        "  Raven.setPosition(4294967296, 5.0, 5.0, 5.0)\n"  /* 1 << 32; used to wrap to 0 */
        "end\n");
    const ecs::Entity victim = fx.MakeScriptedEntity("Victim", "probe.lua");
    fx.SetPos(victim, {7.0f, 8.0f, 9.0f});
    fx.sys.Step(1.0f / 60.0f, 1.0f / 60.0f);
    CHECK(!fx.sys.LastError().empty());
    CHECK(fx.Pos(victim) == glm::vec3(7.0f, 8.0f, 9.0f));   // untouched by the bogus handle
}

RAVEN_TEST("script: watchdogTrips reports zero for well-behaved scripts") {
    ScriptFixture fx;
    WriteScript(fx.dir, "ok.lua", "function onUpdate(e, dt)\nend\n");
    fx.MakeScriptedEntity("Ok", "ok.lua");
    fx.sys.Step(1.0f / 60.0f, 1.0f / 60.0f);
    CHECK(fx.sys.WatchdogTrips() == 0);
}
