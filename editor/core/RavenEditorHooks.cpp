#include "editor/core/RavenEditorHooks.h"

#include <atomic>

#include "imgui.h"

#include "raven/core/EditorHooks.h"
#include "raven/core/Logger.h"
#include "raven/core/UUID.h"

#include "editor_app.h"

namespace raven {
namespace {

const char* AssetReloadKindName(AssetReloadKind kind) {
    switch (kind) {
        case AssetReloadKind::Added:    return "added";
        case AssetReloadKind::Modified: return "modified";
        case AssetReloadKind::Removed:  return "removed";
    }
    return "?";
}

// IEditorHooks implementation bound to the running EditorApp. Engine call sites run
// per-frame; before the app binds (or after shutdown) they see runtime-only defaults.
class RavenEditorHooks final : public IEditorHooks {
public:
    void Init(void* /*window*/) override { m_Ready = true; }
    void Shutdown() override {
        m_Ready = false;
        m_App.store(nullptr, std::memory_order_relaxed);
    }

    void BeginFrame() override {}
    void Render() override {}
    void EndFrame() override {}

    // ImGui capture queries — null context (early engine init) means "capture nothing".
    bool WantCaptureKeyboard() override {
        return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureKeyboard;
    }
    bool WantCaptureMouse() override {
        return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse;
    }

    // Play mode — the PlayState machine lands in Phase 4; Editing is the honest value today.
    PlayState GetPlayState() const override { return PlayState::Editing; }
    bool ConsumeStepRequest() override { return false; }

    void GetViewportState(EditorViewportState& out) override {
        out = EditorViewportState{};
        EditorApp* app = m_App.load(std::memory_order_relaxed);
        if (!app) return;

        out.hasCamera = true;
        const Camera& cam = app->ViewportCamera();
        out.position = cam.position;
        out.nearZ = cam.nearPlane;
        out.farZ = cam.farPlane;
        out.view = cam.ViewMatrix();
        out.projection = cam.ProjectionMatrix(app->LastAspect());

        const Scene& scene = app->GetScene();
        const ecs::Entity selected = app->Selected();
        if (scene.World().IsAlive(selected) && scene.World().HasComponent<UuidComponent>(selected))
            out.selectedEntities = {scene.World().GetComponent<UuidComponent>(selected).id};
    }

    void OnProjectChanged() override {}

    void OnFrameDebuggerNotice(const std::string& message) override {
        RAVEN_LOG_INFO("[frame-debugger] %s", message.c_str());
    }

    void OnAssetReloaded(const UUID& asset, AssetReloadKind kind) override {
        RAVEN_LOG_INFO("asset %s %s", asset.ToString().c_str(), AssetReloadKindName(kind));
    }

    void SetApp(EditorApp* app) { m_App.store(app, std::memory_order_relaxed); }

private:
    std::atomic<EditorApp*> m_App{nullptr};
    bool m_Ready = false;
};

RavenEditorHooks* s_Hooks = nullptr;

} // namespace

void InstallEditorHooks() {
    if (!s_Hooks) s_Hooks = new RavenEditorHooks();  // process-lifetime, intentionally leaked
    EditorHooks::Register(s_Hooks);
}

void BindEditorAppHooks(EditorApp* app) {
    if (s_Hooks) s_Hooks->SetApp(app);
}

} // namespace raven
