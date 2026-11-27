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
#include <map>
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

#include "editor/core/EditorConfig.h"
#include "editor/core/Workspaces.h"
#include "editor/panels/PanelRegistry.h"

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

    // --- Phase 1 shell plumbing ----------------------------------------------
    float LastAspect() const { return m_LastAspect; }
    bool HasProject() const { return m_ProjectLoaded; }          // false → launcher is drawn
    const editor::EditorConfig& GetConfig() const { return m_EditorConfig; }
    // Layout persistence (workspaces/last_session). Headless capture passes false
    // so CI runs never write user config from a throwaway environment.
    void SetPersistLayout(bool enabled) { m_PersistLayout = enabled; }
    // Shell actions reachable from the windowed host (e.g. after file-drop).
    void RequestOpenProject(const std::filesystem::path& dir);

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
    void DrawLauncher();            // two-phase startup: recent projects / open / create
    void DrawSettingsWindow();      // Window > Settings… — two-pane (Editor / Assets)
    void DrawWorkspacePopups();     // Save-As + delete confirmations (menu driven)

    // --- shell helpers ---------------------------------------------------------
    bool BeginProjectFromDir(const std::filesystem::path& dir, std::string* error);  // shared launcher/open path
    std::map<std::string, bool> PanelVisibilityMap() const;
    void ApplyPanelVisibility(const std::map<std::string, bool>& panels);
    void SaveWorkspaceAs(const std::string& name);
    void LoadWorkspace(const std::string& name);
    void DeleteWorkspace(const std::string& name);
    void ResetLayout();
    void ApplyPendingWorkspace();      // applied at the top of the next OnFrame
    void PersistLastSession();         // graceful shutdown: ini + visibility
    void RestoreLastSession();         // startup: restore last_session.{ini,workspace.json}
    void Autosave();                   // throttled scene snapshot to <project>/.raven/autosave
    void ApplyThemeAndPersist(const std::string& themeId);
    void ExportLog();                  // writes the console ring to <project>/.raven/logs/console_*.txt
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
        int level = 2;               // LogLevel::Info
        std::string text;
        std::string channel;         // "General" until Logger gains channels (Phase 2)
    };
    std::vector<LogEntry> m_Log;
    bool m_LogDirty = false;
    bool m_LogCapped = false;        // set when the 1024-entry cap dropped older lines
    std::vector<std::string> m_ConsoleChannels;   // learned channel names, for the filter combo
    char m_ConsoleSearchBuf[128] = {};
    int m_ConsoleLevelFilter = 0;    // 0 all, 1 warn+, 2 error+
    int m_ConsoleChannelFilter = 0;  // 0 = all, 1..N → m_ConsoleChannels[n-1]
    bool m_ConsoleAutoScroll = true;

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

    Renderer& m_Renderer;
    QualityTier m_Tier = QualityTier::High;
    bool m_ShowGrid = true;
    float m_FrameTimes[128] = {};
    int m_FrameTimeCursor = 0;

    // --- Phase 1 shell state ---------------------------------------------------
    editor::PanelRegistry m_Panels;          // docked panels behind the error boundary
    editor::WorkspaceManager m_Workspaces;
    editor::EditorConfig m_EditorConfig = editor::LoadEditorConfig();
    float m_LastAspect = 16.0f / 9.0f;
    bool m_PersistLayout = true;             // false in headless capture
    bool m_LastSessionRestored = false;      // one-shot restore at the top of frame 0
    bool m_DefaultLayoutDone = false;        // one-shot DockBuilder default layout
    bool m_ProjectLoaded = false;            // false → DrawLauncher (windowed two-phase)
    bool m_ShowSettings = false;
    int m_SettingsSection = 0;               // 0 Editor, 1 Assets
    bool m_WorkspaceSaveAsOpen = false;
    char m_WorkspaceNameBuf[96] = {};
    std::string m_WorkspacePendingName;      // LoadWorkspace stored here…
    editor::WorkspaceManager::LoadedWorkspace m_WorkspacePending;  // …applied pre-frame
    double m_LastAutosaveTime = 0.0;
    char m_OpenPathBuf[512] = {};
    char m_NewProjectNameBuf[128] = {};
    char m_NewProjectParentBuf[512] = {};
    std::string m_LauncherError;
    bool m_AutoOpenOnShow = false;   // launcher auto-attempts one prefilled open
    bool m_DirtyTitle = false;
};

} // namespace raven
