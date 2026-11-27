// RavenEngine — editor hook registry.
//
// The engine never depends on the editor. Instead the editor (raven_editor) implements
// this interface and registers it with raven::EditorHooks::Register before any engine
// code runs (see ravened: `raven::InstallEditorHooks()` ahead of app construction).
// Runtime-only builds (raven-sandbox) leave the registry empty and the engine
// short-circuits every editor-specific code path — no #ifdefs anywhere.
//
// Porting notes: mirrors Luth EditorHooks.h (MIT) reduced to the phase-1 surface from
// docs/EDITOR_DESIGN.md §1. The launcher hooks (ShowProjectLauncher etc.) and
// SetActiveScene arrive with the shell in Phase 1 — adding virtuals with defaults is
// non-breaking.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "raven/core/UUID.h"

namespace raven {

// Editor-owned simulation state. The engine queries this via IEditorHooks to decide
// whether game systems (physics, scripts, animation) tick on the current frame.
enum class PlayState : uint8_t {
    Editing = 0,
    Playing = 1,
    Paused = 2,
};

// Physics debug visualization color scheme (consumed by the physics debug draw passes).
// Uniform paints every body with EditorViewportState::physicsUniformColor; ByMotionType
// uses fixed greys/blues/greens; BySleepState mirrors the solver's sleep classification.
enum class PhysicsDebugColorMode : uint8_t {
    Uniform = 0,
    ByMotionType = 1,
    BySleepState = 2,
};

// Why an asset notice fired (matches AssetChangedSignal payloads in the editor).
enum class AssetReloadKind : uint8_t {
    Added = 0,
    Modified = 1,
    Removed = 2,
};

// Per-frame snapshot of editor-owned state fed into the renderer. Default-constructed
// when no editor is registered (runtime-only build): identity camera, empty selection,
// editor grid off.
struct EditorViewportState {
    bool hasCamera = false;
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::vec3 position{0.0f};
    float nearZ = 0.1f;
    float farZ = 1000.0f;
    std::vector<UUID> selectedEntities;

    // Selection outline + editor grid.
    glm::vec4 outlineColor{1.0f, 0.6f, 0.0f, 1.0f};
    float outlineWidth = 1.5f;
    float outlineOccludedAlpha = 0.65f;
    glm::vec4 gridColor{0.41f, 0.41f, 0.41f, 0.5f};
    float gridFadeStart = 20.0f;
    float gridFadeEnd = 200.0f;
    float gridLineThickness = 1.0f;

    // Animate characters/cloth while PlayState::Editing so the scene view feels alive.
    bool previewAnimationInEditor = true;

    // Physics debug visualization. Paired toggles per pass: selected entities only vs.
    // all entities (All overrides Selected).
    bool physicsShapesSelected = true;
    bool physicsShapesAll = false;
    bool physicsAABBsSelected = false;
    bool physicsAABBsAll = false;
    bool physicsCoMSelected = false;
    bool physicsCoMAll = false;
    PhysicsDebugColorMode physicsColorMode = PhysicsDebugColorMode::Uniform;
    glm::vec4 physicsUniformColor{0.40f, 0.86f, 0.37f, 1.0f};
    uint32_t physicsDebugSegments = 32;
    float physicsAlphaUnselected = 0.6f;
};

// Interface the engine uses to drive the editor without ever depending on editor
// headers. The editor registers exactly one instance before the app/renderer come up.
struct IEditorHooks {
    virtual ~IEditorHooks() = default;

    // --- lifecycle ----------------------------------------------------------
    // window: opaque GLFWwindow* (editor passes it through; the engine never derefs).
    virtual void Init(void* window) = 0;
    virtual void Shutdown() = 0;

    // --- per-frame -----------------------------------------------------------
    virtual void BeginFrame() = 0;
    virtual void Render() = 0;
    virtual void EndFrame() = 0;

    // --- input capture (used by the input layer to suppress keys/mouse ImGui consumed)
    virtual bool WantCaptureKeyboard() = 0;
    virtual bool WantCaptureMouse() = 0;

    // --- play mode -----------------------------------------------------------
    // Defaults keep the runtime-only build "always ticking" game systems.
    virtual PlayState GetPlayState() const { return PlayState::Editing; }
    virtual bool ConsumeStepRequest() { return false; }  // Paused: one-frame step request

    // --- viewport state ------------------------------------------------------
    virtual void GetViewportState(EditorViewportState& out) { out = EditorViewportState{}; }

    // --- project lifecycle ---------------------------------------------------
    virtual void OnProjectChanged() {}

    // --- engine-side notices -------------------------------------------------
    virtual void OnFrameDebuggerNotice(const std::string& /*message*/) {}
    virtual void OnAssetReloaded(const UUID& /*asset*/, AssetReloadKind /*kind*/) {}
};

namespace EditorHooks {

// Register the editor-provided hooks (nullptr clears). The editor calls this once,
// before engine subsystems initialize.
void Register(IEditorHooks* hooks);

// Returns the registered hooks, or nullptr for runtime-only builds / before
// registration. All call sites must nullptr-check.
IEditorHooks* Get();

} // namespace EditorHooks

} // namespace raven
