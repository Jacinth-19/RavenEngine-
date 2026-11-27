#include "editor_app.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <nlohmann/json.hpp>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_vulkan.h"
#include "ImGuizmo.h"
#include "Icons.h"

#include "editor/core/EditorConfig.h"
#include "editor/core/StylePresets.h"
#include "editor/core/RavenEditorHooks.h"
#include "editor/panels/Panel.h"

#include "raven/core/Logger.h"
#include "raven/assets/AssetManager.h"
#include "raven/renderer/Renderer.h"

namespace raven {

EditorApp::EditorApp(Renderer& renderer, QualityTier tier)
    : m_Renderer(renderer), m_Tier(tier) {
    m_Camera.fovDegrees = 55.0f;
    m_Camera.nearPlane = 0.1f;
    m_Camera.farPlane = 500.0f;
    m_OrbitYaw = 0.55f;
    m_OrbitPitch = 0.38f;
    m_OrbitDistance = 8.5f;
    m_OrbitFocus = glm::vec3(0.0f, 1.0f, 0.0f);
    UpdateOrbitCamera(960, 540);
    std::snprintf(m_WorkspaceNameBuf, sizeof(m_WorkspaceNameBuf), "Workspace");

    // Register the docked panels behind the error boundary (Phase 1 shell).
    m_Panels.Register(std::make_unique<editor::CallablePanel>("Viewport Tools", nullptr, [this] { DrawViewportPanel(); }));
    m_Panels.Register(std::make_unique<editor::CallablePanel>("Hierarchy", &m_ShowHierarchy, [this] { DrawHierarchyPanel(); }));
    m_Panels.Register(std::make_unique<editor::CallablePanel>("Inspector", &m_ShowInspector, [this] { DrawInspectorPanel(); }));
    m_Panels.Register(std::make_unique<editor::CallablePanel>("Assets", &m_ShowAssets, [this] { DrawAssetsPanel(); }));
    m_Panels.Register(std::make_unique<editor::CallablePanel>("Profiler", &m_ShowProfiler, [this] { DrawProfilerPanel(); }));
    m_Panels.Register(std::make_unique<editor::CallablePanel>("Console", &m_ShowConsole, [this] { DrawConsolePanel(); }));

    BindEditorAppHooks(this);  // engine hooks forward to this app (InstallEditorHooks in main)
}

// ---------------------------------------------------------------------------
// Project / scene
// ---------------------------------------------------------------------------

bool EditorApp::LoadProject(const std::filesystem::path& projectDir, std::string* error) {
    m_ProjectDir = projectDir;
    const std::filesystem::path projectFile = projectDir / "project.ravenproject";
    std::ifstream in(projectFile);
    if (!in) {
        if (error) *error = "not a RavenEngine project (missing project.ravenproject): " + projectDir.string();
        return false;
    }
    nlohmann::json projectJson;
    try {
        in >> projectJson;
    } catch (const std::exception& e) {
        if (error) *error = std::string("project.ravenproject parse error: ") + e.what();
        return false;
    }
    m_ProjectName = projectJson.value("name", projectDir.filename().string());
    const std::string assetsRoot = projectJson.value("assetsRoot", std::string("assets"));
    const std::string defaultScene = projectJson.value("defaultScene", std::string(""));
    m_AssetsDir = projectDir / assetsRoot;
    if (!defaultScene.empty()) m_ScenePath = projectDir / defaultScene;

    std::string sceneError;
    if (!m_ScenePath.empty() && !m_Scene.Load(m_ScenePath, &sceneError)) {
        RAVEN_LOG_ERROR("Editor: failed to load scene %s: %s", m_ScenePath.string().c_str(), sceneError.c_str());
        return false;   // main decides whether to keep going with an empty scene
    }
    m_ProjectLoaded = true;   // shell usable from here on (scene may still have failed — logged above)
    RAVEN_LOG_INFO("Editor: project '%s' loaded (%zu entities, scene: %s)", m_ProjectName.c_str(),
                   m_Scene.World().Entities().size(), m_ScenePath.filename().string().c_str());

    // Select the first renderable entity so the gizmo is visible on startup.
    auto roots = m_Scene.RootEntities();
    for (ecs::Entity e : roots) {
        if (m_Scene.World().HasComponent<MeshRendererComponent>(e)) { m_Selected = e; break; }
    }
    return true;
}

void EditorApp::RegisterLogCapture() {
    Logger::Instance().AddCallback([this](LogLevel level, const std::string& message) {
        AddLogLine(static_cast<int>(level), message);
    });
}

void EditorApp::AddLogLine(int level, const std::string& text) {
    const std::string channel = "General";
    m_Log.push_back({level, text, channel});
    if (std::find(m_ConsoleChannels.begin(), m_ConsoleChannels.end(), channel) == m_ConsoleChannels.end())
        m_ConsoleChannels.push_back(channel);
    // Fixed 1024-entry ring; oldest lines are dropped in one batch.
    if (m_Log.size() > 1024) {
        m_Log.erase(m_Log.begin(), m_Log.begin() + 200);
        m_LogCapped = true;
    }
    m_LogDirty = true;
}

FrameParams EditorApp::MakeFrameParams(bool uiInScene) const {
    FrameParams params;
    params.drawGrid = m_ShowGrid;
    params.deltaTime = 0.0f;
    if (uiInScene) {
        params.uiOverlay = [](VkCommandBuffer cb, const FrameOverlayTarget&) {
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cb);
        };
    }
    return params;
}

// ---------------------------------------------------------------------------
// Orbit camera
// ---------------------------------------------------------------------------

void EditorApp::UpdateOrbitCamera(uint32_t /*width*/, uint32_t /*height*/) {
    const float cp = std::cos(m_OrbitPitch), sp = std::sin(m_OrbitPitch);
    const glm::vec3 offset(cp * std::cos(m_OrbitYaw), sp, cp * std::sin(m_OrbitYaw));
    m_Camera.position = m_OrbitFocus + offset * m_OrbitDistance;
    m_Camera.LookAt(m_Camera.position, m_OrbitFocus);
    m_CameraInitialized = true;
}

void EditorApp::FrameSelected() {
    if (!m_Scene.World().IsAlive(m_Selected)) return;
    if (!m_Scene.World().HasComponent<TransformComponent>(m_Selected)) return;
    const glm::mat4 world = m_Scene.GetWorldMatrix(m_Selected);
    m_OrbitFocus = glm::vec3(world[3]);
    m_OrbitDistance = 5.0f;
    UpdateOrbitCamera(0, 0);
}

ecs::Entity EditorApp::PickEntity(float u, float v, float aspectRatio) const {
    const Ray ray = m_Camera.RayFromScreen(u, v, aspectRatio);
    ecs::Entity best = ecs::NullEntity;
    float bestScore = 1.0f;  // must be closer to the ray than its bounding radius
    for (ecs::Entity e : m_Scene.World().Entities()) {
        if (!m_Scene.World().HasComponent<MeshRendererComponent>(e)) continue;
        const glm::mat4 world = m_Scene.GetWorldMatrix(e);
        const glm::vec3 center(world[3]);
        float radius = 1.0f;
        if (m_Scene.World().HasComponent<LodComponent>(e)) {
            radius = std::max(0.25f, m_Scene.World().GetComponent<LodComponent>(e).boundingRadius);
        }
        const glm::vec3 oc = ray.origin - center;
        const float tca = glm::dot(oc, ray.direction);
        const float d2 = glm::dot(oc, oc) - tca * tca;   // squared perpendicular distance
        if (d2 > radius * radius) continue;
        const float score = std::sqrt(std::max(0.0f, d2)) / radius;
        if (score < bestScore) { bestScore = score; best = e; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Frame entry point
// ---------------------------------------------------------------------------

void EditorApp::OnFrame(float deltaSeconds) {
    (void)deltaSeconds;

    // Layout persistence (last session restore + pending workspace loads) happens at
    // the very top of the frame, before any ImGui window exists.
    RestoreLastSession();
    ApplyPendingWorkspace();

    const ImGuiIO& io = ImGui::GetIO();
    m_LastAspect = io.DisplaySize.y > 1.0f ? io.DisplaySize.x / io.DisplaySize.y : 16.0f / 9.0f;

    // Two-phase startup: the launcher replaces the shell until a project is opened.
    if (!m_ProjectLoaded) {
        DrawLauncher();
        return;
    }

    // Keyboard shortcuts (when not typing into a field).
    if (!io.WantTextInput) {
        const bool hasSel = m_Scene.World().IsAlive(m_Selected);
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) FrameSelected();
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) m_GizmoOperation = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) m_GizmoOperation = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) m_GizmoOperation = 2;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) m_Selected = ecs::NullEntity;
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && hasSel) {
            m_DeleteList.push_back(m_Selected);
            m_Selected = ecs::NullEntity;
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            std::string saveError;
            if (m_Scene.Save(&saveError)) RAVEN_LOG_INFO("Editor: saved scene %s", m_ScenePath.string().c_str());
            else RAVEN_LOG_ERROR("Editor: save failed: %s", saveError.c_str());
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && hasSel) {
            m_Selected = m_Scene.DuplicateEntity(m_Selected);
            m_Scene.MarkDirty();
        }
    }

    // Autosave (opt-in via Settings): throttled scene snapshot; the user's scene
    // path and dirty flag are untouched.
    if (m_EditorConfig.autosaveSeconds > 0 && m_Scene.IsDirty()) {
        const double now = ImGui::GetTime();
        if (now - m_LastAutosaveTime > static_cast<double>(m_EditorConfig.autosaveSeconds)) Autosave();
    }

    // Rolling CPU frame-time history for the profiler plot.
    const FrameStats stats = m_Renderer.GetFrameStats();
    m_FrameTimes[m_FrameTimeCursor] = stats.cpuFrameMs;
    m_FrameTimeCursor = (m_FrameTimeCursor + 1) % 128;

    // PassthruCentralNode: the (empty central-node background) lets the 3D scene show through the dockspace.
    // PassthruCentralNode: the central dock node stays EMPTY so the 3D scene (rendered
    // into the target beneath the UI) shows through it; the Viewport toolbar strip is a
    // thin docked panel on top of the hole.
    const ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_AutoHideTabBar | ImGuiDockNodeFlags_PassthruCentralNode);
    DrawMenuBar();

    // One-time default layout (suppressed when a workspace/last-session ini was applied —
    // it already docked everything). Left/right strips are equal width so the hole is
    // horizontally centered (the scene NDC maps 1:1 onto the full target).
    if (!m_DefaultLayoutDone) {
        m_DefaultLayoutDone = true;
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);
        ImGuiID main = dockspaceId;
        ImGuiID top = ImGui::DockBuilderSplitNode(main, ImGuiDir_Up, 0.065f, nullptr, &main);
        ImGuiID left = ImGui::DockBuilderSplitNode(main, ImGuiDir_Left, 0.18f, nullptr, &main);
        ImGuiID right = ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, 0.22f, nullptr, &main);
        ImGuiID bottom = ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, 0.32f, nullptr, &main);
        ImGui::DockBuilderDockWindow("Viewport Tools", top);
        ImGui::DockBuilderDockWindow("Hierarchy", left);
        ImGui::DockBuilderDockWindow("Inspector", right);
        ImGui::DockBuilderDockWindow("Console", bottom);
        ImGui::DockBuilderDockWindow("Profiler", bottom);
        ImGui::DockBuilderDockWindow("Assets", bottom);
        ImGui::DockBuilderFinish(dockspaceId);
    }

    // Rect of the central (empty) node for this frame — the viewport interaction surface.
    if (ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspaceId)) {
        if (central->Size.x > 4.0f && central->Size.y > 4.0f)
            m_ViewportRect = ImVec4(central->Pos.x, central->Pos.y, central->Size.x, central->Size.y);
    }

    m_Panels.DrawAll();   // docked panels behind the error boundary + crashed placeholders
    DrawAboutModal();
    DrawSettingsWindow();
    DrawWorkspacePopups();

    // Deferred deletions (safe outside ImGui iteration).
    for (ecs::Entity e : m_DeleteList) {
        if (m_Scene.World().IsAlive(e)) m_Scene.DestroyEntity(e);
        if (m_Selected == e) m_Selected = ecs::NullEntity;
        m_Scene.MarkDirty();
    }
    m_DeleteList.clear();
}

void EditorApp::Shutdown() {
    PersistLastSession();
    BindEditorAppHooks(nullptr);
    m_Log.clear();
}

// ---------------------------------------------------------------------------
// Menu bar / docking
// ---------------------------------------------------------------------------

void EditorApp::DrawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem(ICON_SAVE "\tSave Scene\tCtrl+S")) {
            std::string saveError;
            if (m_Scene.Save(&saveError)) RAVEN_LOG_INFO("Editor: saved scene %s", m_ScenePath.string().c_str());
            else RAVEN_LOG_ERROR("Editor: save failed: %s", saveError.c_str());
        }
        if (ImGui::MenuItem(ICON_REFRESH "\tReload Scene")) {
            if (m_Scene.IsDirty()) {
                std::string saveError;
                if (m_Scene.Save(&saveError)) RAVEN_LOG_INFO("Editor: auto-saved before reload");
            }
            std::string loadError;
            if (m_Scene.Load(m_ScenePath, &loadError)) { RAVEN_LOG_INFO("Editor: scene reloaded"); m_Selected = ecs::NullEntity; }
            else RAVEN_LOG_ERROR("Editor: reload failed: %s", loadError.c_str());
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_APP_LOGO "\tOpen Project…")) {
            m_ProjectLoaded = false;   // back to the launcher; scene stays loaded until one is opened
            m_LauncherError.clear();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit\tEsc (window)")) { /* handled by the host window */ }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Entity")) {
        if (ImGui::MenuItem(ICON_ADD "\tCreate Empty Entity")) {
            m_Selected = m_Scene.CreateEntity("Entity");
            m_Scene.MarkDirty();
        }
        const bool hasSel = m_Scene.World().IsAlive(m_Selected);
        if (ImGui::MenuItem(ICON_COPY "\tDuplicate Selected\tCtrl+D", nullptr, false, hasSel)) {
            m_Selected = m_Scene.DuplicateEntity(m_Selected);
            m_Scene.MarkDirty();
        }
        if (ImGui::MenuItem(ICON_DELETE "\tDelete Selected\tDel", nullptr, false, hasSel)) {
            m_DeleteList.push_back(m_Selected);
            m_Selected = ecs::NullEntity;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem(ICON_SETTINGS "\tPreferences…")) m_ShowSettings = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem(ICON_PANEL_HIERARCHY "\tHierarchy", nullptr, &m_ShowHierarchy);
        ImGui::MenuItem(ICON_PANEL_INSPECTOR "\tInspector", nullptr, &m_ShowInspector);
        ImGui::MenuItem(ICON_PANEL_ASSETS "\tAssets", nullptr, &m_ShowAssets);
        ImGui::MenuItem(ICON_PANEL_PROFILER "\tProfiler", nullptr, &m_ShowProfiler);
        ImGui::MenuItem(ICON_PANEL_CONSOLE "\tConsole", nullptr, &m_ShowConsole);
        ImGui::MenuItem(ICON_PANEL_GRID "\tGrid", nullptr, &m_ShowGrid);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window")) {
        if (ImGui::BeginMenu(ICON_LAYOUT_GRID "\tWorkspaces")) {
            const std::vector<std::string> names = m_Workspaces.List();
            for (const std::string& w : names) {
                if (ImGui::MenuItem(w.c_str(), nullptr, m_Workspaces.Current() == w)) LoadWorkspace(w);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_WORKSPACE_SAVE "\tSave Workspace As…")) {
                m_WorkspaceSaveAsOpen = true;
                ImGui::OpenPopup("Save Workspace As");
            }
            if (ImGui::MenuItem(ICON_WORKSPACE_RESET "\tReset Layout")) ResetLayout();
            if (ImGui::BeginMenu(ICON_DELETE "\tDelete Workspace")) {
                for (const std::string& w : names) {
                    if (w == editor::WorkspaceManager::kBuiltinDefault) continue;
                    if (ImGui::MenuItem(w.c_str())) DeleteWorkspace(w);
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(ICON_THEME_DARK "\tTheme")) {
            if (ImGui::MenuItem("Default Dark", nullptr, m_EditorConfig.theme == "dark")) ApplyThemeAndPersist("dark");
            if (ImGui::MenuItem("Default Light", nullptr, m_EditorConfig.theme == "light")) ApplyThemeAndPersist("light");
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(ICON_SETTINGS "\tSettings…")) m_ShowSettings = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem(ICON_INFO "\tAbout RavenEditor")) m_AboutOpen = true;
        ImGui::EndMenu();
    }

    // Right-aligned title: editor + project + dirty marker.
    std::string title = "RavenEditor";
    if (m_ProjectLoaded) title += " — " + m_ProjectName;
    if (m_Scene.IsDirty()) title += " *";
    const float textWidth = ImGui::CalcTextSize(title.c_str()).x;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - textWidth - 8.0f);
    ImGui::TextDisabled("%s", title.c_str());
    ImGui::EndMainMenuBar();
}

void EditorApp::DrawAboutModal() {
    if (m_AboutOpen) ImGui::OpenPopup("About RavenEditor");
    if (!ImGui::BeginPopupModal("About RavenEditor", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::Text(ICON_APP_LOGO "  RavenEditor %s", RAVEN_VERSION);
    ImGui::TextDisabled("RavenEngine — low-resource C++20 Vulkan game engine");
    ImGui::Separator();
    ImGui::Text("GPU: %s", m_Renderer.GpuName().c_str());
    ImGui::Text("Scene: %s%s", m_ScenePath.empty() ? "(none)" : m_ScenePath.filename().string().c_str(),
                m_Scene.IsDirty() ? " *" : "");
    ImGui::Separator();
    ImGui::TextDisabled(ICON_GITHUB "  github.com/Jacinth-19/RavenEngine-");
    ImGui::Separator();
    if (ImGui::Button(ICON_CLOSE " Close", ImVec2(120, 0))) { m_AboutOpen = false; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// Viewport panel (toolbar strip + gizmo over the scene hole)
// ---------------------------------------------------------------------------

void EditorApp::DrawViewportPanel() {
    // Thin docked strip at the top of the central hole: gizmo ops + toggles.
    ImGui::Begin("Viewport Tools", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // Gizmo operation trio: icon toggle buttons with tooltips (Move / Rotate / Scale).
    const ImVec2 toolButton(34.0f, 0.0f);
    const char* gizmoOps[3] = { ICON_GIZMO_MOVE, ICON_GIZMO_ROTATE, ICON_GIZMO_SCALE };
    const char* gizmoTooltips[3] = { "Move (translate)", "Rotate", "Scale" };
    for (int op = 0; op < 3; ++op) {
        const bool active = m_GizmoOperation == op;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button((std::string(gizmoOps[op]) + "##gizmoOp" + std::to_string(op)).c_str(), toolButton)) m_GizmoOperation = op;
        if (active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", gizmoTooltips[op]);
        ImGui::SameLine();
    }
    if (ImGui::Checkbox(ICON_SNAP " Snap", &m_GizmoSnap)) {}
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Snap to grid increments");
    ImGui::SameLine();
    if (ImGui::Checkbox(ICON_PANEL_GRID " Grid", &m_ShowGrid)) {}
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle the viewport grid");
    ImGui::SameLine();
    if (ImGui::Checkbox(ICON_LOCAL " Local", &m_GizmoLocal)) {}
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Local vs world gizmo orientation");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FOCUS " Focus##focusSel")) FrameSelected();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Frame the selected entity");

    ImGui::End();

    // The scene is already in the target being drawn (rendered beneath this UI):
    // the central dock node is a transparent hole. The gizmo and picking live on the
    // full target extent; interaction is gated to the hole rect.
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 fullExtent(io.DisplaySize.x, io.DisplaySize.y);
    const ImVec2 viewportMin(m_ViewportRect.x, m_ViewportRect.y);
    const ImVec2 viewportMax(m_ViewportRect.x + m_ViewportRect.z, m_ViewportRect.y + m_ViewportRect.w);
    const float aspectRatio = fullExtent.y > 0.0f ? fullExtent.x / fullExtent.y : 1.0f;

    HandleViewportInput(viewportMin, viewportMax, fullExtent);
    DrawGizmo(fullExtent, aspectRatio);
}

void EditorApp::HandleViewportInput(const ImVec2& viewportMin, const ImVec2& viewportMax, const ImVec2& fullExtent) {
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 viewportSize(viewportMax.x - viewportMin.x, viewportMax.y - viewportMin.y);
    if (viewportSize.x <= 4.0f || viewportSize.y <= 4.0f || fullExtent.x <= 0.0f) return;

    const bool hovered = ImGui::IsMouseHoveringRect(viewportMin, viewportMax);
    const ImVec2 mouse = io.MousePos;

    // Wheel: dolly.
    if (hovered && io.MouseWheel != 0.0f) {
        m_OrbitDistance = std::clamp(m_OrbitDistance * std::exp(-io.MouseWheel * 0.12f), 2.0f, 40.0f);
        UpdateOrbitCamera(0, 0);
    }

    // Drag state for orbit (LMB) / pan (RMB); click (no drag) = pick.
    static bool draggingOrbit = false;
    static bool draggingPan = false;
    static float dragDistancePx = 0.0f;

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGuizmo::IsUsing()) {
        draggingOrbit = true;
        dragDistancePx = 0.0f;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        draggingPan = true;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (draggingOrbit && dragDistancePx < 4.0f && hovered) {
            // Rays are computed in FULL target space — the scene projects onto the full image.
            const float u = mouse.x / fullExtent.x;
            const float v = mouse.y / fullExtent.y;
            const float aspect = fullExtent.x / fullExtent.y;
            const ecs::Entity picked = PickEntity(u, v, aspect);
            if (picked != ecs::NullEntity) m_Selected = picked;
        }
        draggingOrbit = false;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) draggingPan = false;

    if (draggingOrbit && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        const ImVec2 delta = io.MouseDelta;
        dragDistancePx += std::fabs(delta.x) + std::fabs(delta.y);
        if (!ImGuizmo::IsUsing()) {
            m_OrbitYaw -= delta.x * 0.006f;
            m_OrbitPitch = std::clamp(m_OrbitPitch + delta.y * 0.006f, -1.45f, 1.45f);
            UpdateOrbitCamera(0, 0);
        }
    }
    if (draggingPan && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f)) {
        const ImVec2 delta = io.MouseDelta;
        const glm::vec3 right = m_Camera.Right();
        const glm::vec3 up = m_Camera.Up();
        const float panScale = m_OrbitDistance * 0.0016f;
        m_OrbitFocus -= right * (delta.x * panScale);
        m_OrbitFocus += up * (delta.y * panScale);
        UpdateOrbitCamera(0, 0);
    }
}

void EditorApp::DrawGizmo(const ImVec2& fullExtent, float aspectRatio) {
    if (m_Selected == ecs::NullEntity || !m_Scene.World().IsAlive(m_Selected)) return;
    if (!m_Scene.World().HasComponent<TransformComponent>(m_Selected)) return;

    ImGuizmo::BeginFrame();
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());   // over the dockspace hole
    ImGuizmo::SetRect(0.0f, 0.0f, fullExtent.x, fullExtent.y);

    glm::mat4 view = m_Camera.ViewMatrix();
    glm::mat4 proj = m_Camera.ProjectionMatrix(aspectRatio);

    const auto& t = m_Scene.World().GetComponent<TransformComponent>(m_Selected);
    const glm::mat4 parentWorld = t.parent != ecs::NullEntity && t.parent != m_Selected
        ? m_Scene.GetWorldMatrix(t.parent)
        : glm::mat4(1.0f);
    glm::mat4 world = parentWorld * m_Scene.GetLocalMatrix(t);

    ImGuizmo::OPERATION op = m_GizmoOperation == 0 ? ImGuizmo::TRANSLATE
                           : m_GizmoOperation == 1 ? ImGuizmo::ROTATE : ImGuizmo::SCALE;
    const float snapValues[3] = {0.5f, 15.0f, 0.1f};

    if (ImGuizmo::Manipulate(&view[0][0], &proj[0][0], op,
                             m_GizmoLocal ? ImGuizmo::LOCAL : ImGuizmo::WORLD,
                             &world[0][0], nullptr, m_GizmoSnap ? snapValues : nullptr)) {
        const glm::mat4 local = glm::inverse(parentWorld) * world;
        float translation[3], rotationDeg[3], scale[3];
        ImGuizmo::DecomposeMatrixToComponents(&local[0][0], translation, rotationDeg, scale);
        auto& transform = m_Scene.World().GetComponent<TransformComponent>(m_Selected);
        transform.position = glm::vec3(translation[0], translation[1], translation[2]);
        transform.rotation = glm::radians(glm::vec3(rotationDeg[0], rotationDeg[1], rotationDeg[2]));
        transform.scale = glm::vec3(scale[0], scale[1], scale[2]);
        m_Scene.MarkDirty();
    }
}

// ---------------------------------------------------------------------------
// Hierarchy panel
// ---------------------------------------------------------------------------

// Editor row glyph for an entity, picked from its first "interesting" component.
const char* EntityTypeGlyph(const ecs::World& world, ecs::Entity e) {
    if (world.HasComponent<DirectionalLightComponent>(e)) return ICON_ENTITY_SUN;
    if (world.HasComponent<PointLightComponent>(e)) return ICON_ENTITY_LIGHT;
    if (world.HasComponent<CameraComponent>(e)) return ICON_ENTITY_CAMERA;
    if (world.HasComponent<AudioSourceComponent>(e)) return ICON_ENTITY_AUDIO;
    if (world.HasComponent<ScriptComponent>(e)) return ICON_ENTITY_SCRIPT;
    if (world.HasComponent<MeshRendererComponent>(e)) return ICON_ENTITY_MESH;
    if (world.HasComponent<RigidbodyComponent>(e)) return ICON_ENTITY_BODY;
    return ICON_ENTITY_GENERIC;
}

void EditorApp::DrawEntityNode(ecs::Entity e) {
    auto& world = m_Scene.World();
    const std::string name = world.HasComponent<NameComponent>(e) ? world.GetComponent<NameComponent>(e).name
                                                                  : ("Entity " + std::to_string(static_cast<uint32_t>(e)));
    const auto& children = m_Scene.ChildrenOf(e);
    const char* typeGlyph = EntityTypeGlyph(world, e);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    if (e == m_Selected) flags |= ImGuiTreeNodeFlags_Selected;

    const std::string label = std::string(typeGlyph) + "  " + name;
    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uintptr_t>(e) + 1), flags, "%s", label.c_str());
    if (ImGui::IsItemClicked()) m_Selected = e;

    // Per-row renderer visibility toggle.
    if (world.HasComponent<MeshRendererComponent>(e)) {
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 24.0f);
        auto& mr = world.GetComponent<MeshRendererComponent>(e);
        if (ImGui::Checkbox(("##vis" + std::to_string(static_cast<uint32_t>(e))).c_str(), &mr.visible)) {
            m_Scene.MarkDirty();
        }
    }

    if (open) {
        for (ecs::Entity child : children) DrawEntityNode(child);
        ImGui::TreePop();
    }
}

void EditorApp::DrawHierarchyPanel() {
    if (!ImGui::Begin("Hierarchy", &m_ShowHierarchy)) { ImGui::End(); return; }

    if (ImGui::SmallButton(ICON_ADD " Add##addEntity")) {
        m_Selected = m_Scene.CreateEntity("Entity");
        m_Scene.MarkDirty();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Create empty entity");
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_COPY " Duplicate##dupEntity") && m_Selected != ecs::NullEntity) {
        m_Selected = m_Scene.DuplicateEntity(m_Selected);
        m_Scene.MarkDirty();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Duplicate selected (Ctrl+D)");
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_DELETE " Delete##delEntity") && m_Selected != ecs::NullEntity) {
        m_DeleteList.push_back(m_Selected);
        m_Selected = ecs::NullEntity;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete selected (Del)");
    ImGui::Separator();

    const auto roots = m_Scene.RootEntities();
    for (ecs::Entity root : roots) DrawEntityNode(root);

    // Click empty space to deselect.
    const ImVec2 winPos = ImGui::GetWindowPos();
    const ImVec2 winSize = ImGui::GetWindowSize();
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        ImGui::IsMouseHoveringRect(winPos, ImVec2(winPos.x + winSize.x, winPos.y + winSize.y))) {
        if (!ImGui::IsAnyItemHovered()) m_Selected = ecs::NullEntity;
        (void)0;
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Inspector panel
// ---------------------------------------------------------------------------

void EditorApp::DrawTransformSection(TransformComponent& t, ecs::Entity e) {
    (void)e;
    if (!ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) return;

    bool dirty = false;
    dirty |= ImGui::DragFloat3("Position", &t.position.x, 0.05f);
    glm::vec3 rotDeg = glm::degrees(t.rotation);
    if (ImGui::DragFloat3("Rotation", &rotDeg.x, 0.5f)) {
        t.rotation = glm::radians(rotDeg);
        dirty = true;
    }
    dirty |= ImGui::DragFloat3("Scale", &t.scale.x, 0.02f, 0.001f, 1000.0f);
    if (dirty) m_Scene.MarkDirty();
}

void EditorApp::DrawMeshRendererSection(MeshRendererComponent& mr) {
    if (!ImGui::CollapsingHeader("Mesh Renderer", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::TextDisabled("Mesh");
    ImGui::SameLine();
    ImGui::TextUnformatted(mr.mesh.ToString().c_str());
    ImGui::TextDisabled("Material");
    ImGui::SameLine();
    ImGui::TextUnformatted(mr.material.ToString().c_str());
    if (ImGui::Checkbox("Visible", &mr.visible)) m_Scene.MarkDirty();
    if (ImGui::Checkbox("Cast Shadow", &mr.castShadow)) m_Scene.MarkDirty();
}

void EditorApp::DrawRigidbodySection(RigidbodyComponent& rb) {
    if (!ImGui::CollapsingHeader("Rigidbody")) return;
    const char* types[] = {"Static", "Dynamic"};
    int typeIndex = rb.type == RigidbodyType::Dynamic ? 1 : 0;
    if (ImGui::Combo("Type", &typeIndex, types, 2)) {
        rb.type = typeIndex == 1 ? RigidbodyType::Dynamic : RigidbodyType::Static;
        m_Scene.MarkDirty();
    }
    if (ImGui::DragFloat("Mass", &rb.mass, 0.05f, 0.01f, 10000.0f)) m_Scene.MarkDirty();
    if (ImGui::DragFloat("Restitution", &rb.restitution, 0.01f, 0.0f, 1.0f)) m_Scene.MarkDirty();
    if (ImGui::Checkbox("Use Gravity", &rb.useGravity)) m_Scene.MarkDirty();
    if (ImGui::Checkbox("Awake", &rb.isAwake)) m_Scene.MarkDirty();
    ImGui::TextDisabled("Velocity: %.3f, %.3f, %.3f", rb.linearVelocity.x, rb.linearVelocity.y, rb.linearVelocity.z);
}

void EditorApp::DrawDirectionalLightSection(DirectionalLightComponent& dl) {
    if (!ImGui::CollapsingHeader("Directional Light", ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (ImGui::DragFloat3("Direction", &dl.direction.x, 0.01f)) m_Scene.MarkDirty();
    if (ImGui::DragFloat("Intensity", &dl.intensity, 0.05f, 0.0f, 100.0f)) m_Scene.MarkDirty();
    if (ImGui::ColorEdit3("Color", &dl.color.x)) m_Scene.MarkDirty();
}

void EditorApp::DrawScriptSection(ScriptComponent& sc) {
    if (!ImGui::CollapsingHeader("Script")) return;
    char pathBuf[512];
    std::snprintf(pathBuf, sizeof(pathBuf), "%s", sc.scriptPath.c_str());
    ImGui::InputText("Path", pathBuf, sizeof(pathBuf), ImGuiInputTextFlags_ReadOnly);
    if (ImGui::Checkbox("Enabled", &sc.enabled)) m_Scene.MarkDirty();
}

void EditorApp::DrawLodSection(LodComponent& lod) {
    if (!ImGui::CollapsingHeader("LOD")) return;
    ImGui::Text("Levels: %zu", lod.levels.size());
    ImGui::Text("Screen heights: %zu", lod.screenHeights.size());
    if (ImGui::DragFloat("Bounding Radius", &lod.boundingRadius, 0.05f, 0.05f, 10000.0f)) m_Scene.MarkDirty();
    if (ImGui::DragFloat("LOD Bias", &lod.lodBias, 0.01f, -4.0f, 4.0f)) m_Scene.MarkDirty();
}

void EditorApp::DrawInspectorPanel() {
    if (!ImGui::Begin("Inspector", &m_ShowInspector)) { ImGui::End(); return; }

    // Header: scene save/load + dirty state.
    const bool dirty = m_Scene.IsDirty();
    if (ImGui::Button("Save Scene")) {
        std::string saveError;
        if (m_Scene.Save(&saveError)) RAVEN_LOG_INFO("Editor: saved scene %s", m_ScenePath.string().c_str());
        else RAVEN_LOG_ERROR("Editor: save failed: %s", saveError.c_str());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s%s", m_ScenePath.filename().string().c_str(), dirty ? " *" : "");
    ImGui::Separator();

    if (m_Selected == ecs::NullEntity || !m_Scene.World().IsAlive(m_Selected)) {
        ImGui::TextDisabled("No entity selected");
        ImGui::End();
        return;
    }

    auto& world = m_Scene.World();
    const auto entity = m_Selected;

    // Name
    if (world.HasComponent<NameComponent>(entity)) {
        auto& name = world.GetComponent<NameComponent>(entity).name;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s", name.c_str());
        if (ImGui::InputText("Name", buf, sizeof(buf))) {
            name = buf;
            m_Scene.MarkDirty();
        }
    }
    ImGui::Text("Entity #%u", static_cast<uint32_t>(entity));
    ImGui::Separator();

    if (world.HasComponent<TransformComponent>(entity)) DrawTransformSection(world.GetComponent<TransformComponent>(entity), entity);
    if (world.HasComponent<MeshRendererComponent>(entity)) DrawMeshRendererSection(world.GetComponent<MeshRendererComponent>(entity));
    if (world.HasComponent<RigidbodyComponent>(entity)) DrawRigidbodySection(world.GetComponent<RigidbodyComponent>(entity));
    if (world.HasComponent<DirectionalLightComponent>(entity)) DrawDirectionalLightSection(world.GetComponent<DirectionalLightComponent>(entity));
    if (world.HasComponent<ScriptComponent>(entity)) DrawScriptSection(world.GetComponent<ScriptComponent>(entity));
    if (world.HasComponent<LodComponent>(entity)) DrawLodSection(world.GetComponent<LodComponent>(entity));

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Assets panel (project browser)
// ---------------------------------------------------------------------------

namespace {
struct AssetEntry {
    std::filesystem::path absolute;
    std::string relative;   // relative to project root, forward slashes
    bool isDirectory = false;
};

std::vector<AssetEntry> ScanAssets(const std::filesystem::path& projectDir, const std::filesystem::path& assetsDir) {
    std::vector<AssetEntry> entries;
    std::error_code ec;
    if (!std::filesystem::is_directory(assetsDir, ec)) return entries;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(assetsDir)) {
        AssetEntry item;
        item.absolute = entry.path();
        item.isDirectory = entry.is_directory();
        std::string rel = std::filesystem::relative(entry.path(), projectDir, ec).string();
        std::replace(rel.begin(), rel.end(), '\\', '/');
        item.relative = std::move(rel);
        entries.push_back(std::move(item));
    }
    std::sort(entries.begin(), entries.end(), [](const AssetEntry& a, const AssetEntry& b) {
        if (a.isDirectory != b.isDirectory) return a.isDirectory > b.isDirectory;
        return a.relative < b.relative;
    });
    return entries;
}

const char* AssetGlyph(const AssetEntry& e) {
    if (e.isDirectory) return ICON_ASSET_FOLDER;
    const std::string ext = e.absolute.extension().string();
    if (ext == ".ravenscene") return ICON_ASSET_SCENE;
    if (ext == ".ravenmesh" || ext == ".obj" || ext == ".fbx") return ICON_ASSET_MESH;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".hdr") return ICON_ASSET_IMAGE;
    if (ext == ".lua") return ICON_ASSET_SCRIPT;
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac") return ICON_ASSET_AUDIO;
    if (ext == ".ravenmaterial") return ICON_ASSET_MATERIAL;
    if (ext == ".ravenprefab") return ICON_ASSET_PREFAB;
    if (ext == ".spv") return ICON_ASSET_SHADER;
    return ICON_ASSET_GENERIC;
}
} // namespace

void EditorApp::DrawAssetsPanel() {
    if (!ImGui::Begin("Assets", &m_ShowAssets)) { ImGui::End(); return; }

    static std::vector<AssetEntry> entries;
    static double lastScan = -1000.0;
    const double now = ImGui::GetTime();
    const bool needsScan = (now - lastScan) > static_cast<double>(m_EditorConfig.assetRescanSeconds);
    if (ImGui::SmallButton(ICON_REFRESH " Refresh##refreshAssets") || needsScan) {
        entries = ScanAssets(m_ProjectDir, m_AssetsDir);
        lastScan = now;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rescan project assets");
    ImGui::SameLine();
    static char filter[128] = "";
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##assetFilter", "filter…", filter, sizeof(filter));
    ImGui::SameLine();
    ImGui::TextDisabled("%zu item(s)", entries.size());

    ImGui::Separator();
    const std::string filterStr = filter;
    for (const AssetEntry& entry : entries) {
        if (!filterStr.empty() && entry.relative.find(filterStr) == std::string::npos) continue;
        if (entry.isDirectory) {
            ImGui::TextDisabled("%s %s", AssetGlyph(entry), entry.relative.c_str());
            continue;
        }
        ImGui::PushID(entry.relative.c_str());
        if (ImGui::Selectable((std::string(AssetGlyph(entry)) + " " + entry.relative).c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && entry.absolute.extension() == ".ravenscene") {
                if (m_Scene.IsDirty()) {
                    std::string saveError;
                    if (m_Scene.Save(&saveError)) RAVEN_LOG_INFO("Editor: auto-saved before switching scene");
                }
                std::string loadError;
                if (m_Scene.Load(entry.absolute, &loadError)) {
                    RAVEN_LOG_INFO("Editor: opened scene %s", entry.absolute.string().c_str());
                    m_ScenePath = entry.absolute;
                    m_Selected = ecs::NullEntity;
                } else {
                    RAVEN_LOG_ERROR("Editor: cannot open scene: %s", loadError.c_str());
                }
            }
        }
        ImGui::PopID();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Profiler panel
// ---------------------------------------------------------------------------

void EditorApp::DrawProfilerPanel() {
    if (!ImGui::Begin("Profiler", &m_ShowProfiler)) { ImGui::End(); return; }

    const FrameStats stats = m_Renderer.GetFrameStats();

    ImGui::Text("CPU frame: %.2f ms", stats.cpuFrameMs);
    ImGui::Text("GPU frame: %.2f ms", stats.gpuFrameMs);
    ImGui::TextDisabled("Draws: %u   Triangles: %u   Meshes: %u   LOD culled: %u",
                        stats.drawCalls, stats.triangles, stats.meshesDrawn, stats.lodCulled);
    ImGui::TextDisabled("Culling passes skipped: %u", stats.culledPasses);

    ImGui::PlotLines("CPU ms", m_FrameTimes, 128, m_FrameTimeCursor, nullptr, 0.0f, 50.0f, ImVec2(0, 48));

    ImGui::Separator();
    ImGui::Text("Pass timings (last frame):");
    if (ImGui::BeginTable("passTimes", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Pass");
        ImGui::TableSetupColumn("ms");
        ImGui::TableHeadersRow();
        for (const PassTiming& pass : stats.passTimes) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(pass.name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", pass.milliseconds);
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::TextDisabled("GPU: %s", stats.gpuName.c_str());
    ImGui::TextDisabled("VRAM: %.1f MiB   dynamic-rendering: %s   timeline-sem: %s%s",
                        static_cast<double>(stats.vramBytes) / (1024.0 * 1024.0),
                        stats.dynamicRendering ? "yes" : "no",
                        stats.timelineSemaphores ? "yes" : "no",
                        stats.cpuDevice ? "   [CPU device]" : "");
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Console panel
// ---------------------------------------------------------------------------

void EditorApp::DrawConsolePanel() {
    if (!ImGui::Begin("Console", &m_ShowConsole)) { ImGui::End(); return; }

    // Filter row: level toggles + channel combo + search + actions.
    ImGui::RadioButton("All", &m_ConsoleLevelFilter, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Warn+", &m_ConsoleLevelFilter, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Error+", &m_ConsoleLevelFilter, 2);
    ImGui::SameLine();

    ImGui::SetNextItemWidth(120.0f);
    const char* channelPreview = (m_ConsoleChannelFilter == 0 || m_ConsoleChannelFilter > static_cast<int>(m_ConsoleChannels.size()))
                                     ? "All channels"
                                     : m_ConsoleChannels[static_cast<size_t>(m_ConsoleChannelFilter - 1)].c_str();
    if (ImGui::BeginCombo("##consoleChannel", channelPreview)) {
        if (ImGui::Selectable("All channels", m_ConsoleChannelFilter == 0)) m_ConsoleChannelFilter = 0;
        for (int i = 0; i < static_cast<int>(m_ConsoleChannels.size()); ++i) {
            const bool selected = m_ConsoleChannelFilter == i + 1;
            if (ImGui::Selectable(m_ConsoleChannels[static_cast<size_t>(i)].c_str(), selected)) m_ConsoleChannelFilter = i + 1;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputTextWithHint("##consoleSearch", "search…", m_ConsoleSearchBuf, sizeof(m_ConsoleSearchBuf));
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) { m_Log.clear(); m_LogCapped = false; m_LogDirty = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Export…")) ExportLog();
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &m_ConsoleAutoScroll);
    if (m_LogCapped) ImGui::TextDisabled("oldest entries dropped (cap 1024)");
    ImGui::Separator();

    const int minLevel = m_ConsoleLevelFilter == 0 ? 0 : (m_ConsoleLevelFilter == 1 ? static_cast<int>(LogLevel::Warn) : static_cast<int>(LogLevel::Error));
    const std::string search = m_ConsoleSearchBuf;
    std::string searchLower = search;
    std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool filtering = !searchLower.empty();
    const std::string activeChannel = (m_ConsoleChannelFilter >= 1 && m_ConsoleChannelFilter <= static_cast<int>(m_ConsoleChannels.size()))
                                          ? m_ConsoleChannels[static_cast<size_t>(m_ConsoleChannelFilter - 1)]
                                          : std::string();

    int matches = 0;
    ImGui::BeginChild("logScroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    for (const LogEntry& entry : m_Log) {
        if (entry.level < minLevel) continue;
        if (!activeChannel.empty() && entry.channel != activeChannel) continue;
        if (filtering) {
            std::string textLower = entry.text;
            std::transform(textLower.begin(), textLower.end(), textLower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (textLower.find(searchLower) == std::string::npos) continue;
        }
        ++matches;
        ImVec4 color;
        switch (static_cast<LogLevel>(entry.level)) {
            case LogLevel::Warn: color = ImVec4(0.95f, 0.80f, 0.45f, 1.0f); break;
            case LogLevel::Error:
            case LogLevel::Fatal: color = ImVec4(0.95f, 0.45f, 0.45f, 1.0f); break;
            case LogLevel::Debug: color = ImVec4(0.62f, 0.68f, 0.75f, 1.0f); break;
            default: color = ImVec4(0.82f, 0.84f, 0.86f, 1.0f); break;
        }
        if (!entry.channel.empty() && entry.channel != "General") {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.70f, 0.85f, 1.0f));
            ImGui::Text("[%s]", entry.channel.c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine();
        }
        ImGui::TextColored(color, "%s", entry.text.c_str());
    }
    if (filtering) ImGui::TextDisabled("%d match(es)", matches);
    if (m_LogDirty) {
        if (m_ConsoleAutoScroll) ImGui::SetScrollHereY(1.0f);
        m_LogDirty = false;
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Phase 1 shell — workspaces, settings, autosave, persistence, launcher
// ---------------------------------------------------------------------------

void EditorApp::RequestOpenProject(const std::filesystem::path& dir) {
    std::snprintf(m_OpenPathBuf, sizeof(m_OpenPathBuf), "%s", dir.string().c_str());
    m_ProjectLoaded = false;    // → launcher with the path prefilled
    m_LauncherError.clear();
    m_AutoOpenOnShow = true;
}

bool EditorApp::BeginProjectFromDir(const std::filesystem::path& dir, std::string* error) {
    std::string localError;
    if (!LoadProject(dir, &localError)) {
        if (error) *error = localError;
        m_LauncherError = localError;
        return false;
    }
    m_LauncherError.clear();
    editor::AddRecentProject(m_EditorConfig, m_ProjectName, dir.string());
    m_LastAutosaveTime = ImGui::GetTime();
    return true;
}

std::map<std::string, bool> EditorApp::PanelVisibilityMap() const {
    return {{"Hierarchy", m_ShowHierarchy}, {"Inspector", m_ShowInspector}, {"Assets", m_ShowAssets},
            {"Profiler", m_ShowProfiler},  {"Console", m_ShowConsole},     {"Viewport Tools", true}};
}

void EditorApp::ApplyPanelVisibility(const std::map<std::string, bool>& panels) {
    auto apply = [&panels](const char* key, bool& flag) {
        auto it = panels.find(key);
        if (it != panels.end()) flag = it->second;
    };
    apply("Hierarchy", m_ShowHierarchy);
    apply("Inspector", m_ShowInspector);
    apply("Assets", m_ShowAssets);
    apply("Profiler", m_ShowProfiler);
    apply("Console", m_ShowConsole);
}

void EditorApp::SaveWorkspaceAs(const std::string& name) {
    std::string error;
    if (m_Workspaces.SaveAs(name, PanelVisibilityMap(), &error)) {
        RAVEN_LOG_INFO("Editor: workspace saved as '%s'", m_Workspaces.Current().c_str());
    } else {
        RAVEN_LOG_ERROR("Editor: workspace save failed: %s", error.c_str());
    }
}

void EditorApp::LoadWorkspace(const std::string& name) {
    std::string error;
    editor::WorkspaceManager::LoadedWorkspace loaded = m_Workspaces.Load(name, &error);
    if (!loaded.ok) {
        RAVEN_LOG_ERROR("Editor: workspace '%s' failed to load: %s", name.c_str(), error.c_str());
        return;
    }
    // Applied at the top of the next frame, before any window exists.
    m_WorkspacePendingName = name;
    m_WorkspacePending = std::move(loaded);
}

void EditorApp::DeleteWorkspace(const std::string& name) {
    std::string error;
    if (m_Workspaces.Delete(name, &error)) RAVEN_LOG_INFO("Editor: workspace '%s' deleted", name.c_str());
    else RAVEN_LOG_ERROR("Editor: workspace delete failed: %s", error.c_str());
}

void EditorApp::ResetLayout() {
    ImGui::LoadIniSettingsFromMemory("", 0);   // clear docking state → builtin layout applies
    ApplyPanelVisibility(editor::WorkspaceManager::DefaultVisibility());
    m_Workspaces.SetCurrent(editor::WorkspaceManager::kBuiltinDefault);
    m_DefaultLayoutDone = false;               // re-run the builtin DockBuilder layout
    RAVEN_LOG_INFO("Editor: layout reset to Default");
}

void EditorApp::ApplyPendingWorkspace() {
    if (m_WorkspacePendingName.empty()) return;

    const std::string name = m_WorkspacePendingName;
    editor::WorkspaceManager::LoadedWorkspace loaded = std::move(m_WorkspacePending);
    m_WorkspacePendingName.clear();

    ImGui::LoadIniSettingsFromMemory(loaded.iniText.c_str(), loaded.iniText.size());
    ApplyPanelVisibility(loaded.panelVisibility);
    // Empty (builtin Default) ini re-runs the builtin DockBuilder layout; an explicit
    // workspace ini already contains dock positions, so skip the builtin pass.
    m_DefaultLayoutDone = !loaded.iniText.empty();
    RAVEN_LOG_INFO("Editor: workspace '%s' applied", name.c_str());
}

void EditorApp::RestoreLastSession() {
    if (!m_PersistLayout || m_LastSessionRestored) return;
    m_LastSessionRestored = true;

    const std::string ini = editor::LoadLastSessionLayout();
    if (!ini.empty()) {
        ImGui::LoadIniSettingsFromMemory(ini.c_str(), ini.size());
        m_DefaultLayoutDone = true;
    } else {
        m_DefaultLayoutDone = false;
    }

    const nlohmann::json panels = editor::LoadLastSessionVisibility();
    if (panels.is_object() && !panels.empty()) {
        std::map<std::string, bool> vis;
        for (const auto& [k, v] : panels.items())
            if (v.is_boolean()) vis[k] = v.get<bool>();
        ApplyPanelVisibility(vis);
    }
}

void EditorApp::PersistLastSession() {
    if (!m_PersistLayout) return;
    size_t iniSize = 0;
    const char* ini = ImGui::SaveIniSettingsToMemory(&iniSize);
    editor::SaveLastSessionLayout(ini ? std::string(ini, iniSize) : std::string());

    nlohmann::json panels = nlohmann::json::object();
    for (const auto& [k, v] : PanelVisibilityMap()) panels[k] = v;
    editor::SaveLastSessionVisibility(panels);
}

void EditorApp::Autosave() {
    namespace fs = std::filesystem;
    const fs::path dir = m_ProjectDir / ".raven" / "autosave";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        RAVEN_LOG_WARN("Editor: autosave cannot create %s: %s", dir.string().c_str(), ec.message().c_str());
        m_LastAutosaveTime = ImGui::GetTime();
        return;
    }
    const std::string stem = m_ScenePath.empty() ? std::string("untitled") : m_ScenePath.stem().string();
    const fs::path out = dir / (stem + ".autosave.ravenscene");
    std::ofstream f(out, std::ios::trunc);
    if (!f) {
        RAVEN_LOG_WARN("Editor: autosave could not open %s", out.string().c_str());
        m_LastAutosaveTime = ImGui::GetTime();
        return;
    }
    f << m_Scene.SerializeToString();
    m_LastAutosaveTime = ImGui::GetTime();
    RAVEN_LOG_DEBUG("Editor: autosaved scene snapshot to %s", out.string().c_str());
}

void EditorApp::ExportLog() {
    namespace fs = std::filesystem;
    const fs::path dir = m_ProjectLoaded ? (m_ProjectDir / ".raven" / "logs") : (editor::ConfigDir() / "logs");
    std::error_code ec;
    fs::create_directories(dir, ec);

    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tmNow{};
#if defined(_WIN32)
    localtime_s(&tmNow, &now);
#else
    localtime_r(&now, &tmNow);
#endif
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d_%02d%02d%02d",
                  tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday, tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec);
    const fs::path out = dir / (std::string("console_") + stamp + ".txt");

    std::ofstream f(out, std::ios::trunc);
    if (!f) {
        RAVEN_LOG_ERROR("Editor: export failed — cannot write %s", out.string().c_str());
        return;
    }
    for (const LogEntry& entry : m_Log) {
        std::string text = entry.text;
        for (char& c : text) {
            const unsigned char uc = static_cast<unsigned char>(c);
            if (uc < 0x20) c = (uc == '\n' || uc == '\r') ? ' ' : '.';
        }
        f << '[' << LogLevelToString(static_cast<LogLevel>(entry.level)) << "] [" << entry.channel << "] " << text << '\n';
    }
    RAVEN_LOG_INFO("Editor: console exported to %s (%zu lines)", out.string().c_str(), m_Log.size());
}

void EditorApp::ApplyThemeAndPersist(const std::string& themeId) {
    editor::ApplyTheme(themeId);
    m_EditorConfig.theme = themeId;
    editor::SaveEditorConfig(m_EditorConfig);
}

void EditorApp::DrawWorkspacePopups() {
    if (!m_WorkspaceSaveAsOpen) return;
    ImGui::SetNextWindowSize(ImVec2(380.0f, 150.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Save Workspace As", &m_WorkspaceSaveAsOpen, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize)) {
        ImGui::TextUnformatted("Name for the new workspace");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##workspaceName", m_WorkspaceNameBuf, sizeof(m_WorkspaceNameBuf));
        ImGui::Spacing();
        if (ImGui::Button("Save", ImVec2(140.0f, 0.0f))) {
            const std::string name = m_WorkspaceNameBuf;
            SaveWorkspaceAs(name);
            m_WorkspaceSaveAsOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(140.0f, 0.0f))) {
            m_WorkspaceSaveAsOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorApp::DrawSettingsWindow() {
    if (!m_ShowSettings) return;

    ImGui::SetNextWindowSize(ImVec2(680.0f, 430.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Settings", &m_ShowSettings, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    // Two-pane: section list on the left, section body on the right.
    ImGui::BeginChild("settingsSections", ImVec2(150.0f, 0.0f), ImGuiChildFlags_Borders);
    const char* sections[] = {"Editor", "Assets"};
    for (int i = 0; i < 2; ++i) {
        if (ImGui::Selectable(sections[i], m_SettingsSection == i)) m_SettingsSection = i;
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("settingsBody", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);

    bool cfgDirty = false;
    if (m_SettingsSection == 0) {
        ImGui::TextDisabled("EDITOR");
        ImGui::Separator();

        const char* themeNames[] = {"Default Dark", "Default Light"};
        int themeIndex = m_EditorConfig.theme == "light" ? 1 : 0;
        ImGui::TextDisabled("Theme");
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::Combo("##theme", &themeIndex, themeNames, 2)) {
            ApplyThemeAndPersist(themeIndex == 1 ? "light" : "dark");
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Persistence");
        ImGui::Separator();
        int autosave = m_EditorConfig.autosaveSeconds;
        if (ImGui::DragInt("Autosave interval (s)", &autosave, 1.0f, 0, 600)) {
            m_EditorConfig.autosaveSeconds = std::clamp(autosave, 0, 600);
            cfgDirty = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 disables. Snapshots go to <project>/.raven/autosave/ — the scene path is never redirected.");

        ImGui::Spacing();
        ImGui::TextDisabled("Layout");
        ImGui::Separator();
        if (ImGui::Button(ICON_WORKSPACE_RESET " Reset to Default layout")) ResetLayout();
        ImGui::SameLine();
        if (ImGui::Button(ICON_WORKSPACE_SAVE " Save current workspace")) {
            std::string error;
            if (!m_Workspaces.SaveCurrent(PanelVisibilityMap(), &error)) RAVEN_LOG_ERROR("Editor: save workspace failed: %s", error.c_str());
            else RAVEN_LOG_INFO("Editor: workspace '%s' saved", m_Workspaces.Current().c_str());
        }
        ImGui::TextDisabled("Current: %s", m_Workspaces.Current().empty() ? "(unsaved layout)" : m_Workspaces.Current().c_str());
    } else {
        ImGui::TextDisabled("ASSETS");
        ImGui::Separator();
        ImGui::TextDisabled("Project directory");
        ImGui::TextWrapped("%s", m_ProjectDir.string().c_str());
        ImGui::TextDisabled("Assets root");
        ImGui::TextWrapped("%s", m_AssetsDir.string().c_str());
        ImGui::TextDisabled("Scene");
        ImGui::TextWrapped("%s", m_ScenePath.empty() ? "(none)" : m_ScenePath.string().c_str());

        ImGui::Spacing();
        float rescan = m_EditorConfig.assetRescanSeconds;
        if (ImGui::DragFloat("Auto-rescan interval (s)", &rescan, 0.1f, 0.5f, 30.0f, "%.1f")) {
            m_EditorConfig.assetRescanSeconds = std::clamp(rescan, 0.5f, 30.0f);
            cfgDirty = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How often the Assets panel re-scans the project for changes");
    }

    if (cfgDirty) editor::SaveEditorConfig(m_EditorConfig);

    ImGui::EndChild();
    ImGui::End();
}

void EditorApp::DrawLauncher() {
    const ImGuiIO& io = ImGui::GetIO();

    // Full-window dark backdrop so the shell never flashes through.
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize, IM_COL32(17, 17, 21, 255));

    // Lazily seed the new-project parent with the user's home directory.
    static bool parentSeeded = false;
    if (!parentSeeded) {
        parentSeeded = true;
        const char* home = std::getenv("HOME");
#if defined(_WIN32)
        if (!home) home = std::getenv("USERPROFILE");
#endif
        if (home && m_NewProjectParentBuf[0] == '\0') std::snprintf(m_NewProjectParentBuf, sizeof(m_NewProjectParentBuf), "%s", home);
    }

    // Auto-open one pending request (e.g. file-drop before the launcher showed).
    if (m_AutoOpenOnShow) {
        m_AutoOpenOnShow = false;
        std::string error;
        BeginProjectFromDir(std::filesystem::path(m_OpenPathBuf), &error);
        if (m_ProjectLoaded) return;
    }

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(780.0f, 540.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Welcome to RavenEditor", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

    ImGui::SetWindowFontScale(1.8f);
    ImGui::Text(ICON_APP_LOGO "  RavenEditor");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("RavenEngine — low-resource C++20 game editor  ·  v%s", RAVEN_VERSION);
    ImGui::Separator();

    // --- left column: recent projects -------------------------------------------
    const std::vector<editor::RecentProject> recents = m_EditorConfig.recentProjects;  // copy: the list may be mutated below
    ImGui::BeginChild("recentProjects", ImVec2(330.0f, 0.0f), ImGuiChildFlags_Borders);
    ImGui::TextDisabled("RECENT PROJECTS");
    ImGui::Separator();
    if (recents.empty()) {
        ImGui::TextDisabled("No recent projects yet.");
        ImGui::TextDisabled("Open or create one on the right.");
    }
    for (const editor::RecentProject& p : recents) {
        ImGui::PushID(p.path.c_str());
        if (ImGui::Selectable(p.name.c_str(), false)) {
            std::string error;
            BeginProjectFromDir(p.path, &error);
        }
        ImGui::TextDisabled("%s", p.path.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // --- right column: open / create ---------------------------------------------
    ImGui::BeginChild("projectActions", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
    ImGui::TextDisabled("OPEN PROJECT");
    ImGui::Separator();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##openProjectPath", "path to a project.ravenproject folder…", m_OpenPathBuf, sizeof(m_OpenPathBuf));
    if (ImGui::Button(ICON_APP_LOGO " Open", ImVec2(120.0f, 0.0f))) {
        std::string error;
        BeginProjectFromDir(std::filesystem::path(m_OpenPathBuf), &error);
    }

    ImGui::Spacing();
    if (!m_LauncherError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.45f, 1.0f));
        ImGui::TextWrapped("%s", m_LauncherError.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    ImGui::TextDisabled("NEW PROJECT");
    ImGui::Separator();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##newProjectName", "project name…", m_NewProjectNameBuf, sizeof(m_NewProjectNameBuf));
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##newProjectParent", "parent folder…", m_NewProjectParentBuf, sizeof(m_NewProjectParentBuf));
    if (ImGui::Button(ICON_ADD " Create", ImVec2(120.0f, 0.0f))) {
        namespace fs = std::filesystem;
        std::string name = m_NewProjectNameBuf;
        std::string parent = m_NewProjectParentBuf;
        const auto trim = [](std::string& v) {
            const size_t b = v.find_first_not_of(" \t\r\n");
            const size_t e = v.find_last_not_of(" \t\r\n");
            v = (b == std::string::npos) ? std::string() : v.substr(b, e - b + 1);
        };
        trim(name);
        trim(parent);
        if (name.empty() || parent.empty()) {
            m_LauncherError = "both a name and a parent folder are required";
        } else {
            const fs::path dir = fs::path(parent) / name;
            std::error_code ec;
            fs::create_directories(dir / "assets", ec);
            fs::create_directories(dir / "scenes", ec);
            if (ec) {
                m_LauncherError = "cannot create " + dir.string() + ": " + ec.message();
            } else {
                nlohmann::json project;
                project["name"] = name;
                project["assetsRoot"] = "assets";
                project["defaultScene"] = "scenes/default.ravenscene";
                std::ofstream pf(dir / "project.ravenproject", std::ios::trunc);
                pf << project.dump(2);
                Scene blank(name);
                std::ofstream sf(dir / "scenes" / "default.ravenscene", std::ios::trunc);
                sf << blank.SerializeToString();
                std::string error;
                if (!BeginProjectFromDir(dir, &error)) m_LauncherError = error;
            }
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace raven
