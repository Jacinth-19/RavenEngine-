// RavenEditor — real-time editor application (docking ImGui + Vulkan).
//
// The 3D scene renders behind the docking UI (the dockspace's central node is
// left EMPTY with PassthruCentralNode, so the scene shows through) and is
// framed by the panels: Hierarchy | Viewport Tools strip | Inspector,
// Console/Profiler/Assets along the bottom. The viewport interaction surface is
// the central node rect; orbit/pan/dolly/pick and the ImGuizmo operate on it.
//
// Two run modes (chosen in main.cpp):
//   windowed — GLFW window + swapchain; the scene and the UI render into the
//              swapchain in one frame (Renderer::RenderFrame with uiInScene)
//   headless — no window; scene + UI render into the offscreen target and the
//              result is written to a PNG (used on displays-less hosts/CI)
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <vulkan/vulkan.h>
#include "imgui.h"

#include "raven/scene/Scene.h"
#include "raven/scene/Components.h"
#include "raven/renderer/Renderer.h"
#include "raven/renderer/Camera.h"
#include "raven/renderer/Quality.h"

struct GLFWwindow;

namespace raven {

class EditorApp {
public:
    EditorApp(Renderer& renderer, QualityTier tier);
    EditorApp(const EditorApp&) = delete;
    EditorApp& operator=(const EditorApp&) = delete;

    // Project: directory containing project.ravenproject (defaultScene + assetsRoot).
    bool LoadProject(const std::filesystem::path& projectDir, std::string* error);

    // Editor lifetime
    void OnFrame(float deltaSeconds);   // builds the ImGui frame (panels + viewport overlay)
    void Shutdown();

    Camera& ViewportCamera() { return m_Camera; }
    const Scene& GetScene() const { return m_Scene; }
    Scene& GetScene() { return m_Scene; }
    // The frame always draws the UI overlay into its color target (swapchain when
    // windowed via RenderFrame, offscreen when headless via RenderOffscreen).
    FrameParams MakeFrameParams(bool uiInScene) const;

    // Selection
    ecs::Entity Selected() const { return m_Selected; }

    // --- shared helpers used by both modes ----------------------------------
    void RegisterLogCapture();          // hooks Logger -> console panel
    void AddLogLine(int level, const std::string& text);

    // Panel visibility toggles (View menu)
    bool m_ShowHierarchy = true;
    bool m_ShowInspector = true;
    bool m_ShowAssets = true;
    bool m_ShowProfiler = true;
    bool m_ShowConsole = true;

private:
    // --- panel builders ------------------------------------------------------
    void DrawMenuBar();
    void DrawHierarchyPanel();
    void DrawInspectorPanel();
    void DrawAssetsPanel();
    void DrawProfilerPanel();
    void DrawConsolePanel();
    void DrawViewportPanel();
    void DrawAboutModal();
    void DrawTransformSection(TransformComponent& t, ecs::Entity e);
    void DrawMeshRendererSection(MeshRendererComponent& mr);
    void DrawRigidbodySection(RigidbodyComponent& rb);
    void DrawDirectionalLightSection(DirectionalLightComponent& dl);
    void DrawScriptSection(ScriptComponent& sc);
    void DrawLodSection(LodComponent& lod);

    // --- 3D viewport helpers ---------------------------------------------------
    void UpdateOrbitCamera(uint32_t viewportWidth, uint32_t viewportHeight);
    // The 3D scene is rendered for the full target; interaction is constrained to
    // the central dock node (the "viewport hole").
    void HandleViewportInput(const ImVec2& viewportMin, const ImVec2& viewportMax, const ImVec2& fullExtent);
    void DrawGizmo(const ImVec2& fullExtent, float aspectRatio);
    ecs::Entity PickEntity(float u, float v, float aspectRatio) const;
    void FrameSelected();

    // --- hierarchy helpers ------------------------------------------------------
    void DrawEntityNode(ecs::Entity e);

    // --- logging -----------------------------------------------------------------
    struct LogEntry {
        int level;
        std::string text;
    };
    std::vector<LogEntry> m_Log;
    bool m_LogDirty = false;

    // --- scene / project ---------------------------------------------------------
    Scene m_Scene{"Scene"};
    std::filesystem::path m_ProjectDir;
    std::filesystem::path m_AssetsDir;
    std::filesystem::path m_ScenePath;
    std::string m_ProjectName = "project";

    // --- camera (orbit) -----------------------------------------------------------
    Camera m_Camera;
    float m_OrbitYaw = 0.6f;      // radians
    float m_OrbitPitch = 0.35f;   // radians
    float m_OrbitDistance = 8.5f;
    glm::vec3 m_OrbitFocus{0.0f, 1.0f, 0.0f};
    bool m_CameraInitialized = false;

    // --- viewport -----------------------------------------------------------------
    // Rect of the central (empty) dock node — the scene shows through it and it is
    // the interaction surface. In ImVec4 form: x, y, width, height (pixels).
    ImVec4 m_ViewportRect = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    bool m_GizmoSnap = false;
    int m_GizmoOperation = 0;   // 0 translate, 1 rotate, 2 scale
    bool m_GizmoLocal = true;

    // --- selection ---------------------------------------------------------------
    ecs::Entity m_Selected = ecs::NullEntity;
    std::vector<ecs::Entity> m_DeleteList;
    bool m_AboutOpen = false;
    bool m_DirtyTitle = false;
    std::string m_LastTitle;

    Renderer& m_Renderer;
    QualityTier m_Tier = QualityTier::High;
    bool m_ShowGrid = true;
    float m_FrameTimes[128] = {};
    int m_FrameTimeCursor = 0;
};

} // namespace raven
