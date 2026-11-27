# RavenEditor — one-to-one parity design with Luth's editor

Status: **design only — nothing in this document is implemented.**
Target: behaviour and UX parity with Luth `main` @ `135b9778` (2026-09-10, MIT —
<https://github.com/Hekbas/Luth>). Luth is the reference implementation; this document
says **how** RavenEngine reaches it, file by file. The feature *specification* remains
`docs/ROADMAP.md` (project owner's spec). Where this document and ROADMAP.md disagree on
what to build, ROADMAP.md wins and this document is wrong by definition.

One-to-one claims apply at the **editor layer**. Engine-layer parity (fibers, bindless,
SVGF, clustered Forward+, Slang pipeline…) is staged through ROADMAP.md and deliberately
not re-specified here; the editor consumes those systems as they land.

Conventions:

- "Luth:" rows reference the actual Luth source tree names (paths relative to repo root,
  e.g. `luthien/source/luthien/Editor.cpp`).
- "RE:" rows name the target files in this repository.
- Phases are gates for implementation order, not for design validity. Skipping a phase is
  an owner decision, not a design shortcut.
- "1:1" means observable behaviour. Implementation language is C++20 either way.

---

## 0. What exists today (baseline inventory, 2026-11-28)

Honest baseline — the editor is far smaller than Luth's today:

| Area | RavenEngine today | Luth today |
|---|---|---|
| Editor binary | `ravened` exe, ~1.2k lines (`editor/`), single `EditorApp` class, no panel base | `Luthien.lib` static lib + thin `Luthien.exe` (`luthien/`, `runtime/`), `Panel` base class, ~30 panels |
| ImGui | docking branch 1.93.1, docking+viewports off-by-default-ini, Phosphor Regular merged into default font | docking + multi-viewports, Regular+Fill merged into 2 font copies + 64px icon bakes, semantic `ICON_*` layer |
| Panels | Hierarchy, Inspector (fixed sections), Viewport Tools, Assets (flat list), Console, Profiler (minimal) | Scene, Hierarchy, Inspector (registry + resource modes), Project (tree+grid+thumbnails), Console, Profiler, Frame Debugger, Resource, History, Game, Render, Material Graph, Texture Remap, Settings window, Launcher, + 7 resource inspectors |
| Undo/redo | `raven::CommandStack` exists in core with `ICommand`/`LambdaCommand`, **unused by the editor** | `CommandHistory`, 14 typed commands keyed on entity UUID, coalescing, compounds, HistoryPanel |
| Entity identity | `ecs::Entity` (EnTT handle); **no stable entity UUID** | UUID per entity, `Scene::FindEntityByUUID` |
| Selection | single `m_Selected` | multi-select singleton with version counter + signals |
| Scene | `.ravenscene` JSON, schema v2, `SerializeToString`/`LoadFromString` round-trip (play-mode snapshot ready), dirty tracking | `.luth` JSON, dirty tracking, snapshot on play |
| Jobs | `JobSystem` (future-based, 1 worker/core, `ParallelFor`) — no fibers, no async yield | fiber job system (FLS, work-stealing, asm context switch) |
| Render graph | `raven::RenderGraph` — pure scheduling/barrier solver, unit-tested; **not recording passes** | full DAG w/ parallel command recording, RG snapshot for frame debugger |
| Asset DB | `AssetDatabase` exists (UUID + `.meta`, rescan, orphans) — **not wired to the editor** | UUID registry, importers, smart import, BCn, hot reload, thumbnails |
| Memory | no allocator layer (`new`/`delete` everywhere) | Onion/Garlic tagged allocators + linear allocator |
| Event bus | none | `EventBus` + typed signals (SelectionChanged, LogEntry, AssetChanged…) |
| Play mode | none (Scene round-trip exists as the snapshot mechanism) | `PlayState` machine, transport bar, GamePanel, snapshot/restore |
| Workspaces | none (`io.IniFilename = "raven_editor.ini"` crude) | named workspaces, builtin/user, sidecar panel visibility |
| CI | Linux (Lavapipe) + Windows core-only legs | Windows MSBuild Debug/Release/Dist |
| Icons | Phosphor Regular merged, ~50 call sites use raw `ICON_PH_*` | Regular+Fill, 64px bakes, semantic remap macros |

What this baseline buys us: the **pure, unit-testable cores already exist** for render
graph barriers, command stack, scene serialization, asset DB, UUIDs, and logging — the
missing 80% is the editor-layer shell (hooks, panels, selection, commands UI) and the
threading model. That is exactly how Luth is layered.

---

## 1. Layering and build targets

Luth: `Luth.lib` (engine, zero `luthien/` includes) → `IEditorHooks` registry →
`Luthien.lib` (editor) → `Luthien.exe` (runtime, `InstallLuthienEditorHooks()` **before**
`App()` ctor).

RE target:

```
engine/   raven           STATIC — engine, zero editor deps (unchanged rule)
editor/   raven_editor    STATIC — ALL editor code (new)
editor/   ravened         EXE — thin entry: install hooks, parse args, run (was main.cpp)
sandbox/  raven-sandbox   EXE — unchanged; links raven only (never raven_editor)
```

- `engine/include/raven/core/EditorHooks.h` — `IEditorHooks` (virtual, nullptr-safe
  registry `raven::EditorHooks::Register/Get`). Engine call sites guard
  `if (auto* h = EditorHooks::Get())`. Runtime-only builds leave the registry empty and
  short-circuit — no `#ifdef`s.
- `editor/core/RavenEditorHooks.cpp` — `class RavenEditorHooks : public IEditorHooks`
  forwarding to static `Editor::` APIs. `editor/main.cpp` calls
  `raven::InstallEditorHooks()` before anything engine-side runs.
- CMake: `editor/CMakeLists.txt` defines `raven_editor` (static lib; all editor sources
  below) then `ravened` = `main.cpp` linking `raven_editor`. `raven-sandbox` must not
  gain an editor dependency, ever — enforce with a CI cmake-target assertion
  (`scripts/check_layering.py`, new): greps the link graph; fail on `raven-sandbox →
  raven_editor`.
- 1:1 hook surface (Luth `IEditorHooks` mirrored, Raven names kept lowercase-camel per
  repo style): `Init/Shutdown`, `BeginFrame/Render/EndFrame`, `WantCaptureKeyboard/Mouse`,
  `GetPlayState`, `ConsumeStepRequest`, `GetViewportState`, `OnProjectChanged`,
  `OnFrameDebuggerNotice`, `OnAssetReloaded` (new — Luth signals instead; we do both).

`GetViewportState` mirrors Luth's `EditorViewportState`: selected entities (UUIDs),
preview-animation toggle, physics debug toggles. The engine reads it per-frame; it never
pulls editor singletons directly.

---

## 2. ImGui platform layer

Luth: docking + viewports enabled; two merged font copies (Regular / Fill) + standalone
64px icon-only bakes; semantic `ICON_*` macros overgenerated `IconsPhosphor(Fill).h`;
data-driven `EditorStyle` JSON presets applied via `ImGuiStyle`; multi-viewport
`UpdatePlatformWindows`/`RenderPlatformWindowsDefault` in `EndFrame`.

RE target:

### 2.1 Fonts (`editor/core/Fonts.cpp`, upgrade of `LoadRavenFonts`)

- Main font: keep ImGui default (Lato) but load with Luth's config — `OversampleH=3,
  OversampleV=2, PixelSnapH=true`. (Embedding a third-party main font is optional;
  decision: keep default to avoid a new license/asset dependency, note in open questions.)
- **Copy A** (`Editor::GetFont()` semantics): default font + merged `Phosphor.ttf` at
  13px, `PixelSnapH`. Opens the editor.
- **Copy B** (`Editor::GetIconFillFont()`): second copy of default font + merged
  `Phosphor-Fill.ttf` at 13px. Used for active/toggled accent states.
- **64px icon-only bakes** (`Editor::GetIconLarge(bool fill)`): `Phosphor.ttf` /
  `Phosphor-Fill.ttf` at 64px, `MergeMode=false`, ranges `{0xE000,0xF8FF}` — for the
  project grid and large empty-state glyphs (Play/pause transport buttons).
- Ranges stay `{ 0xE000, 0xF8FF, 0 }` (entire PUA block) — both weights live there.
- Fill font: vendored alongside Regular from `@phosphor-icons/web@2.1.2` — we currently
  only ship `Phosphor.ttf`; `Phosphor-Fill.ttf` must be added to
  `third_party/phosphor/fonts/` (same MIT license, same package) with manifest/SBOM
  bump (trivial, same entry pattern).
- All glyph defines already in `editor/phosphor_icons.h`; add `ICON_MIN_PH`/`ICON_MAX_PH`/
  `ICON_MIN_PHF`/`ICON_MAX_PHF` to it.

### 2.2 Semantic icon layer (`editor/Icons.h`) — new file

Two-level scheme copied 1:1 from Luth `widgets/Icons.h`: raw defines stay generated and
untouched; `editor/Icons.h` binds semantic names for every call site:

```cpp
#define ICON_SEARCH   ICON_PH_MAGNIFYING_GLASS
#define ICON_UNDO     ICON_PH_ARROW_COUNTER_CLOCKWISE
#define ICON_PLAY     ICON_PH_PLAY
#define ICON_MOVE     ICON_PH_ARROWS_OUT_CARDINAL
// ... full tables follow Luth Icons.h group-for-group: Generic UI, Status/logs,
// Edit/history/transport, Viewport/gizmo, Scene/components, Assets/import, Inspector
```

Macros (not constexpr) so string-literal concatenation keeps working
(`ImGui::MenuItem(ICON_SAVE "  Save\tCtrl+S")`). `_FILL` variants point at
`ICON_PHF_*` defines added in `IconsPhosphorFill.h` (new generated file, same format as
`phosphor_icons.h`, header documents the remap indirection).

Migration: mechanical rewrite of ~60 call sites + hierarchy/asset glyph switch statements
to semantic names. No behavioral change.

### 2.3 Style presets (`editor/core/StylePreset.{h,cpp}`)

- `StylePreset` struct mirroring Luth `StyleFile`/`StylePreset`: name, font config,
  rounding/border/padding/spacing/alpha scalars, `ImVec4[ImGuiCol_COUNT] Colors`.
- JSON (de)serialization (`Serialise`/`Deserialise`/`LoadFromFile`/`SaveToFile`/
  `LoadBuiltin`) — built-ins: `Default Dark` (matches current StyleColorsDark values,
  byte-comparable), plus `Default Light`. Built-ins read-only.
- `EditorStyle::Apply(preset)` writes `ImGuiStyle`. Applied after context creation and on
  preference commit. Ships under `editor/assets/styles/Default Dark.json` (or reuse
  `sample/assets`? No — engine assets, see §9 paths).
- `Edit > Preferences > General > Theme` selects preset; live-apply (no restart).

### 2.4 Docking, settings, viewports

- `io.ConfigFlags |= ImGuiConfigFlags_DockingEnable` (exists) and
  `ImGuiConfigFlags_ViewportsEnable` (new, guarded by a setting — on by default on
  Win32/GLFW > 3.3, like Luth; off in headless mode).
- `io.IniFilename = nullptr` always; layout is owned by **workspaces** (§8), not by
  ImGui's ini autosave. `Editor::Shutdown` saves the active workspace sidecar.
- `ImGuiIO::ConfigWindowsMoveFromTitleBarOnly = true`; `DockingSeparatorSize = 2`;
  `WindowMenuButtonPosition = ImGuiDir_None` (Luth parity).
- `EndFrame`: `if (viewportsEnabled) { ImGui::UpdatePlatformWindows();
  ImGui::RenderPlatformWindowsDefault(); }`.

### 2.5 UI utilities (`editor/widgets/`)

Promote the ad-hoc helpers in `editor_app.cpp` into widgets with Luth names:

- `Property(label, T&)` family — 2-column table, colored axis DragFloats via
  `COLOR_EDITING`-style axis buttons (X red / Y green / Z blue), `PropertyColor`,
  `PropertyAsset(label, UUID&, AssetType)` (drag-drop accept + type validation + clear),
  `PropertyCombo`, `PropertyCheckbox`.
- `BeginCollapsingHeader(label, bool* enabled, defaultOpen)` — feature-gated header
  used by material inspector lobes (checkbox-in-header).
- `AssetSlot`, `FilterBox`, `ButtonGroup`, `CategoryPopup`, `HelpMarker`,
  `InspectorHeader`, `Splitter`, `Charts` (CPU/GPU ms history used by profiler),
  `InfoTable`, `TexturePreview`, `Icons.h`.
- `ThumbnailCache`, `ThumbnailGenerator`, `ThumbnailPreviewScene` — see §10.

---

## 3. Panel architecture (`editor/panels/`)

Luth: `Panel` base with `OnInit/OnGather/OnDraw/OnEvent/OnShutdown`; introspection
(`IsVisible/IsFocused/IsDocked/GetWindowFlags/GetWindowID`); `BeginWindow()` wraps
`ImGui::Begin`; per-panel crash boundary.

### 3.1 Base class (`editor/panels/Panel.h`)

```cpp
namespace raven::editor {

class EditorSnapshot;          // §4
class Panel {
public:
    virtual ~Panel() = default;
    virtual void OnInit() {}                              // once, after construction
    virtual void OnGather(class EditorSnapshotBuilder&) {} // worker fiber, NO ImGui/Vk
    virtual void OnDraw(const EditorSnapshot&) = 0;       // main thread, ImGui legal here
    virtual void OnEvent(const struct EditorSignal&) {}   // bus drain, main thread
    virtual void OnShutdown() {}

    bool IsVisible() const { return m_Visible; }   // per-frame
    bool IsFocused() const { return m_Focused; }
    bool IsDocked()  const { return m_Docked; }
    int  GetWindowFlags() const;                   // ImGuiWindowFlags_No* applied
    const char* GetWindowID() const { return m_WindowID; }  // literal, stable dock IDs

    virtual bool BeginWindow(const char* fallbackTitle = nullptr); // Begin + introspection
    void EndWindow();                                             // End + m_Visible update

    bool m_Open = true;     // user-toggled (Window menu / X button) — persists via workspace
protected:
    bool m_Visible = true, m_Focused = false, m_Docked = true;
    const char* m_WindowID;      // set by ctor, e.g. "Hierarchy" (docking persists)
    ImGuiWindowFlags m_WindowFlags = ImGuiWindowFlags_None;
    int  m_CrashStreak = 0;
    bool m_Crashed = false;
};
```

Two-phase visibility, 1:1 with Luth: `m_Open` (user intent, workspace-persisted) vs
`m_Visible` (this frame drawn). The registry skips drawing closed panels but still
delivers `OnEvent`.

### 3.2 Error boundary (`Editor::GatherPanel/DrawPanelGuarded`)

- Catch `std::exception` and `...` around both gather and draw.
- Log through `raven::Logger::LogChannel(LogLevel::Error, "Editor", …)` (§7) with
  `StackTrace::LogStackTrace(1, 32)` — new `raven/core/StackTrace.h`: POSIX `backtrace()`
  + `dladdr1`, Windows `CaptureStackBackTrace` + `dbghelp` (symbolized when
  `dbghelp.dll` available; otherwise raw addresses). Unit-testable for "doesn't crash on
  bad PC", not for symbol content.
- `m_CrashStreak >= 3` → `m_Crashed = true`. Crashed panels draw
  `DrawCrashedPlaceholder`: same window ID, red warning text, **Reset** button
  (`m_CrashStreak = 0; m_Crashed = false`) and stack-trace dump button. Docking layout is
  preserved because the window ID survives.

### 3.3 Panel inventory — 1:1 mapping

| Luth panel | RE target file | Phase | Notes |
|---|---|---|---|
| ScenePanel (viewport) | `panels/ScenePanel.cpp` + `viewport/ViewportRenderer.*` + `viewport/GizmoController.*` | 1–2 | §5 |
| HierarchyPanel | `panels/HierarchyPanel.cpp` | 2 | §5, multi-select |
| InspectorPanel | `panels/InspectorPanel.cpp` + `inspectors/*ComponentDrawerRegistry*` | 2 | §5, §6 |
| ProjectPanel | `panels/ProjectPanel.cpp` | 3 | folder tree + grid + thumbnails §10 |
| ConsolePanel | `panels/ConsolePanel.cpp` | 3 | bus hop, 1024 cap, clipper §7 |
| ProfilerPanel | `panels/ProfilerPanel.cpp` | 5 | §11 |
| FrameDebuggerPanel | `panels/FrameDebuggerPanel.cpp` | 5 | §12 |
| ResourcePanel | `panels/ResourcePanel.cpp` | 5 | AssetDatabase + refcounts |
| HistoryPanel | `panels/HistoryPanel.cpp` | 2 | CommandStack observer |
| GamePanel | `panels/GamePanel.cpp` | 4 | first Camera entity view |
| RenderPanel | `panels/RenderPanel.cpp` | 4 | quality tier + post settings once post stack exists |
| MaterialGraphPanel | `panels/MaterialGraphPanel.cpp` | 6 | ImGuizmo GraphEditor + Slang §13 |
| TextureRemapDialog | `panels/TextureRemapDialog.cpp` | 3 | smart-import ambiguity UX §10 |
| ProjectLauncher | `panels/ProjectLauncher.cpp` | 1 | two-phase startup §9 |
| EditorSettingsWindow | `panels/EditorSettingsWindow.cpp` | 1 | two-pane, sections; **not** a DockPanel — plain window |
| Resource inspectors | `inspectors/{Material,Texture,Shader,PhysicsMaterial,Model,Font,Scene}Editor.*` | 3 (Material/Texture), 6 (rest) | resource-mode inspector, shares layout patterns |

Widget/panel support files: `editor/widgets/*` (§2.5). Every panel keeps a ≤150-line
.cpp discipline; draw logic splits into free functions or drawer classes (React-style
function components, matching how `EditorApp` methods already decompose).

---

## 4. Frame lifecycle: gather → snapshot → draw

Luth: `Editor::Render()` fires `OnGather` on worker fibers filling per-panel fragments in
panel-local `LinearAllocator`s; main assembles type-keyed read-only `EditorSnapshot`;
`OnDraw` consumes it. ImGui is only ever called on the main thread. Rationale recorded in
their `arch/editor.md` — same reasoning applies or doesn't, depending on threading
decisions below.

### 4.1 RE design

- `raven::memory::LinearAllocator` — new engine-core unit (`engine/include/raven/memory/
  LinearAllocator.h`). Bump allocation over a `std::vector<std::byte>` arena; `Reset()`
  rewinds; `New<T>(args...)` placement-new; `Alloc(bytes, align)`; destructor runs only
  for `New<T>` elements (non-trivial types) via a small deferred list. Unit-tested in
  `tests/test_linear_allocator.cpp`. This is also a ROADMAP prerequisite (per-frame
  linear allocator) — implement once, share.
- `editor/core/EditorSnapshot.h` — 1:1 with Luth `EditorSnapshot`/`EditorSnapshotBuilder`:
  type-keyed map (`std::type_index → const void*`), one fragment per panel by design,
  builder allocates into the panel's own `m_GatherAlloc`. `OnDraw` does
  `snapshot.Get<HierarchyFragment>()`.
- **Threading decision (explicit):** the *pattern* (gather produces an immutable
  fragment; draw consumes it; ImGui main-thread only) lands in Phase 1 with gather
  executing **synchronously on the main thread** (as today, just factored). The
  *parallel execution* on worker fibers lands in Phase 2, gated behind
  `RAVEN_EDITOR_PARALLEL_GATHER` (default ON once allocator + job counters exist). No
  semantic change — just wall-clock on big panels (thumbnails, hierarchy with 50k rows).
  This avoids blocking icon/menu work on the fiber rewrite.
- Gather contract (1:1): no ImGui, no Vk, no shared mutable state, no locks; reads scene
  tables through `Scene&` const views only. Anything else → main thread or event bus.

### 4.2 Per-frame call order (windowed)

```
glfwPollEvents
App::OnUpdate (input snapshot)
EditorHooks: BeginFrame()
  └─ ImGui_ImplGlfw_NewFrame / ImGui_ImplVulkan_NewFrame / ImGui::NewFrame
Editor::Gather()         — for each open panel: OnGather (worker or main)
Editor::AssembleSnapshot()
Editor::Draw()           — for each open panel: DrawPanelGuarded(OnDraw)
Editor::DrawCrashedPlaceholders()
ImGui::Render()
EditorHooks: EndFrame()
  └─ UpdatePlatformWindows / RenderPlatformWindowsDefault (if viewports)
Renderer::RenderFrame    — scene offscreen → (RG UI pass later) → swapchain
  └─ viewport panel samples scene texture from previous frame (descriptor cache §5.1)
Editor::DrainEventBus()  — deliver OnEvent to panels (after draw, before shutdown)
```

Headless mode (`--headless`): same sequence minus GLFW/viewports; `RenderOffscreen` +
`ReadOffscreenToPng` (exists). All panels still gather/draw — headless captures become
byte-identical editor screenshots (CI artifact).

---

## 5. Viewport, selection, gizmos

### 5.1 Viewport texture flow

Luth: `RenderingSystem` renders scene color offscreen; `ViewportRenderer` samples it via
cached `VkDescriptorSet`s (`ImGui_ImplVulkan_AddTexture`) with weak_ptr auto-cleanup;
letterboxed draw; per-instance resize callback resizes the offscreen target.

RE (mostly additive to what exists):

- Keep `Renderer::RenderOffscreen` + `OffscreenColorView()` as the viewport source for
  Phases 1–4 (it already renders the scene without a swapchain).
- New `editor/viewport/ViewportRenderer.{h,cpp}`: owns viewport size + aspect-corrected
  inner rect (letter/pillarbox), the descriptor-set cache
  (`UI::GetTextureID(VkImageView)` in `editor/widgets/TextureCache.{h,cpp}` with
  weak_ptr cleanup — 1:1 copy of Luth `UI::GetTextureID`), and the resize callback
  `SetOnResize(std::function<void(u32,u32)>)`. `Editor` wires it to the renderer's
  `RequestOffscreenResize(w,h)` (new small API — recreates the offscreen target;
  analogous to `RequestSwapchainResize` which exists).
- The viewport `ImGui::Image` uses `ImGui::GetBackgroundDrawList`-style layering: scene
  image underneath, ImGui overlays (gizmo via ImGuizmo in `ImGuiWindowFlags_NoInputs`
  child, marquee rect drawn on `GetWindowDrawList`) on top.
- Phase 5 (frame debugger) upgrades the pipeline so the **UI pass lives inside the render
  graph** (`RG::GraphPass "ui"` with a side effect, scene-color as input); until then the
  image sampling is identical either way. No design change.

### 5.2 ScenePanel + gizmo (`editor/viewport/GizmoController.*`)

- Camera: reuse `Camera` (orbit/pan/zoom exist in editor? today: basic orbit via mouse
  drag — formalize): `EditorCamera` orbit (RMB), pan (MMB), zoom (wheel), fly (WASD+RMB),
  entity tracking (F/Shift+F), near/far per-viewport.
- ImGuizmo: TRS ops, `Ctrl` snap, Local/Global toggle, `Snap` increments (0.5 / 15° /
  0.1), `Scale` uniform via `ImGuizmo::MODE::LOCAL`. 1:1 with current toolbar but driven
  by the new semantic icons + `GizmoController` owning op state, replacing raw enum in
  `EditorApp`.
- **Gizmo-drag undo coalescing (1:1)**: on `ImGuizmo::IsUsing` begin → push
  `GizmoTransformCommand` snapshot {uuid, t, r, s}; on drag end → command pushes onto
  `CommandStack` as one entry (coalesced); mid-drag Transform writes are direct (no
  command spam).
- **Picking & selection escalation (1:1)**: plain click selects the clicked mesh's
  **root** entity; second click on the same root escalates to the exact part; Ctrl extends,
  Shift range-selects in visible order; marquee rect selects by screen-projected origin
  (Shift adds, Ctrl toggles); `Delete` batch-deletes as one `EntityDestroyMultipleCommand`.
- **Outline**: 1:1 intent, two stages. Phase 2: ImGui draw-list overlay — project each
  selected entity's world AABB to screen, draw corner brackets + name tag (cheap, always
  works, zero renderer dependency). Phase 4+: renderer outline pass (Stencil+depth
  prepass) behind `RAVEN_RENDER_OUTLINE=ON` once the post stack exists (ROADMAP).
- **Shade modes**: Lit (current), Wireframe, Unlit/Debug normals/depth cascade — these
  are ROADMAP renderer features; the viewport carries the *toggle UI* in Phase 1
  (checked/active states stored in `EditorViewportState`), wired as they land.

### 5.3 Selection singleton (`editor/selection/EditorSelection.{h,cpp}`)

1:1 with Luth `EditorSelection`:

- `s_Selected`: `std::vector<raven::UUID>` (entity UUIDs — see §6.1), primary = back.
  `Select(uuid)` replace / `Add` / `Toggle` / `Remove` / `Clear`; `s_Version` bumped on
  every mutation; getters `Selected()`, `Primary()`, `Version()`.
- Publishes `SelectionChangedSignal { version, primary }` on the bus (§7.2).
- Consumers: HierarchyPanel (selection highlight, span anchor), InspectorPanel (primary
  edit + MultiEdit fan-out §6.3), ScenePanel (outline, gizmo on active), AssetBrowser
  stays resource-selection (separate `s_SelectedResource` with same shape for asset UUIDs).

### 5.4 HierarchyPanel

1:1 behavior list (this is the checklist to implement against):

- Tree with expand/collapse arrows + type glyph (already present: keep via `Icons.h`
  semantic glyphs + `DrawEntityNode` recursion).
- Substring search filter (case-insensitive) above the tree; matching rows + ancestors
  shown expanded.
- Drag-drop: entity → entity reparent (command, with cycle check `IsDescendantOf`),
  entity → root detach, model from ProjectPanel → instantiate under target.
- Context menu: Create Empty Child / Duplicate (Ctrl+D) / Delete (Del) / Rename (F2 inline
  `InputText` swap) / Reparent via drag.
- Deferred action queue (`m_PendingOps`) — scene mutation requests during draw iteration
  are queued and executed in `OnEvent` phase, 1:1 (Luth does exactly this to avoid
  mutating the vector being iterated).
- Multi-select: Ctrl-click toggle, Shift-click span in *visible row order* (cache visible
  row list per frame), double-click → focus gizmo (F).
- Row widgets right-aligned: visibility eye (existing), lock (new, skips gizmo/pick),
  enabled checkbox when ScriptComponent present.
- Footer: "N selected" + Add Component is in Inspector not here.

---

## 6. Commands, undo/redo, entity UUIDs

### 6.1 Entity UUIDs (engine change, prerequisite)

Luth commands resolve entities via `Scene::FindEntityByUUID()` because EnTT recycles
handles. RE does the same:

- `components::UuidComponent { UUID value; }` — added to every entity in
  `Scene::CreateEntityInternal`. Generated from `raven::UUID` (existing 128-bit type).
- `Scene::FindEntityByUUID(UUID) -> std::optional<ecs::Entity>`; O(n) index rebuilt on
  demand, memoized by `m_UuidIndexVersion` bumped on create/destroy (fine to O(n) for
  editor-scale scenes; add hash-map index if profiling says otherwise).
- **Scene format v3**: `"schemaVersion": 3`; entity objects gain `"uuid"` field. Reader:
  v1/v2 files load, missing UUIDs are minted on load and scene marked dirty (save upgrades).
  Writer always emits v3. `kSchemaVersion = 3`; migration tests for v1→v3, v2→v3,
  round-trip v3→v3 with stable UUIDs.
- Serialization lives in `Scene.cpp` as today; `SerializeToString` gains UUIDs too
  (play-mode snapshot §7/4 needs identity continuity across snapshot edits — actually
  play mode discards snapshot on stop, but UUIDs make snapshot diff/bug-repro reports
  deterministic).

### 6.2 Command layer (`editor/commands/`)

- Promote `raven::CommandStack` as-is (it already matches Luth `CommandHistory`
  semantics: cap 100, `PushExecuted` clears redo, `Undo/Redo`, `Notify()` observer for
  HistoryPanel). No rewrite — this is the rare case where the prior work was done right.
- All commands resolve entities through `FindEntityByUUID` at execute/unexecute time.
  Handles never cross frames.
- Command set (1:1 with Luth's 14):

| # | Luth command | RE implementation notes |
|---|---|---|
| 1 | ComponentPropertyCommand<T, Member> | `editor/commands/PropertyCommand.h` — template; stores `UUID, T member value` via pointer-to-member; used by **all** inspector `Property()` writes |
| 2 | ComponentAddCommand<T> | adds default-constructed component; stores prior absence state |
| 3 | ComponentRemoveCommand<T> | stores removed component JSON snapshot; undo re-attaches |
| 4 | ComponentCopy/Paste (clipboard) | JSON fragment; paste creates via ComponentPasteCommand |
| 5 | GizmoTransformCommand | coalescible (see §5.2); stores t/r/s triple |
| 6 | EntityCreateCommand | stores name, parent uuid, uuid (pre-minted so undo of create→redo keeps UUID) |
| 7 | EntityDestroyCommand | stores full subtree JSON (recursive); undo re-materializes with original UUIDs |
| 8 | EntityDestroyMultipleCommand | N subtrees in one entry (marquee/batch delete) |
| 9 | EntityRenameCommand | old/new strings |
| 10 | EntityReparentCommand | old parent/new parent (+index) |
| 11 | EntityReorderCommand | sibling index move (drag reorder in hierarchy) |
| 12 | EntityDuplicateCommand | duplicates subtree with fresh UUIDs, returns mapping for reselect |
| 13 | MaterialSnapshotCommand | debounced (150 ms quiet window) snapshot of material JSON; one entry per burst; autosave after push |
| 14 | CompoundCommand | `BeginCompound/EndCompound` on the stack itself — Luth has this in CommandHistory; add to `raven::CommandStack` (opens a group, single undo unit) |

- Editor-side handle `editor/commands/EditorCommandStack.h`: global instance
  `raven::editor::Commands()` wrapping a core `CommandStack`; adds `BeginCompound/EndCompound`,
  the 150 ms debounce for MaterialSnapshotCommand, and the observer list for
  HistoryPanel + status-bar undo label ("Undo: Gizmo Transform").
- Shortcuts: Ctrl+Z / Ctrl+Y (+Ctrl+Shift+Z) — bound in `Editor::Init` via
  `ImGuiShortcutKey`; toolbar Undo/Redo buttons (disabled state from `CanUndo/CanRedo`).
- `HistoryPanel`: timeline list (undo stack top-down, redo greyed below), per-command
  type icon (semantic icons), ExpandCompound rows, click entry → undo/redo *to* that
  point (loop Undo/Redo), footer `Undo N · Redo M · Clear`.
- **Every editor mutation routes through a command.** Acceptance rule for code review:
  no direct `World().GetComponent<...>().x = v` outside `editor/commands/*` — enforce in
  review + `scripts/check_editor_no_direct_writes.py` for the top 4 components as a lint.

### 6.3 InspectorPanel mechanics

- Modes: **entity mode** when an entity is primary-selected, **resource mode** when a
  ProjectPanel asset is selected (material/texture/shader/model/font/scene viewer), or
  **none** (placeholder with "Select an entity or asset"). `EditorSelection` holds both
  selections; resource mode wins when both set (Luth behavior).
- `inspectors/ComponentDrawerRegistry`: `RegisterDrawer<T>("Transform", &DrawTransformSection)`;
  `DrawComponent<T>()` does header (icon + name + enable-checkbox-if-any + help marker),
  body via drawer, overflow context menu (Copy Component / Paste Over / Reset / Remove
  Component / Copy As JSON). This replaces the fixed if-chain in `DrawInspectorPanel`.
- **Add Component popup**: searchable list of registered drawers (category grouped:
  Transform/Rendering/Physics/Audio/Script/Light/Prefab), creates via
  ComponentAddCommand.
- **MultiEdit fan-out (1:1)**: when ≥2 entities selected and the user edits a field whose
  drawer opts in (`MultiEdit::Broadcast<T, Member>`), the edit is applied to *every*
  selected entity holding T, as one compound command ("Multi Edit (3)"); Reset/Remove/
  Add Component apply to all. Combos apply to primary only. Field rows show `MIXED`
  when values differ (rendered as label `[mixed]`), cleared on next uniform write.
- Per-component enable/disable: `Enabled` toggle per drawable component where the
  component has an enable bit (Script today, render-ables via `visible`/`enabled`).
- Breadcrumb header: `<scene name> / <parent chain up to root> / <entity>` (InspectorHeader
  widget); click crumb → select that entity.

---

## 7. Messaging, console, logging

### 7.1 Logger upgrade (`engine/src/core/Logger.cpp`)

- `LogChannel(LogLevel, std::string_view channel, const char* fmt, ...)` — new overload;
  `RAVEN_LOG_*` macros unchanged (channel = ""), new `RAVEN_LOG_C(tag, ...)` macros pass
  the channel. Channel string is *not* prepended eagerly for the color sink — it is a
  property of the entry handed to sinks (see below).
- Internal `LogEntry { time_point, level, channel, text }` struct — sinks receive the
  struct, not a preformatted string. The stderr sink formats locally
  (`HH:MM:SS.mmm [level][channel] msg`); the editor console sink enqueues the struct; the
  file sink formats the same way. This keeps the editor from double-parsing text.
- `RAVEN_LOG_LEVEL` env override unchanged.

### 7.2 Event bus (`engine/include/raven/core/EventBus.h`)

1:1 with Luth `EventBus`, reduced:

```cpp
namespace raven {

struct EditorSignal {  // payloads are small PODs/strings; never pointers to frame data
    virtual ~EditorSignal() = default;
};
template <typename T> struct TypedSignal : EditorSignal {
    T payload;                       // e.g. SelectionChangedSignal{ u64 version; UUID primary; }
};

class EventBus {
public:
    static EventBus& Instance();     // one per process (engine-global)
    template <typename T> void Publish(T payload);          // any thread, lock-free MPSC enqueue
    template <typename T> void Subscribe(std::function<void(const T&)> fn);  // main thread
    void Drain();                    // main thread; called between draw and shutdown
    template <typename T> std::vector<std::function<void(const T&)>>& Handlers();
};
}
```

- MPSC queue (mutex-guarded inbox is fine at editor rates — hundreds of signals/frame
  max; lock-free is a later optimization, noted not designed).
- Main-thread drain only; handlers may publish (re-entrant drain with depth cap 8,
  overflow → logs error and drops).
- **Editor signals** (`editor/events/EditorSignals.h`): `SelectionChangedSignal`,
  `SceneDirtyChangedSignal`, `LogEntrySignal{LogEntryEntry}` (mirrors Logger channel —
  actually: editor subscribes to Logger's callback directly and republishes as
  `LogEntrySignal` to keep engine free of editor deps), `AssetChangedSignal{UUID, kind
  (Modified/Added/Removed)}`, `WorkspaceChangedSignal` (future-proof, no in-tree
  subscribers), `FrameDebuggerNoticeSignal{std::string}`, `PlayStateChangedSignal`,
  `ProjectChangedSignal`, `ImportReportSignal` (remap dialog).
- Tests: publish/subscribe/drain order, re-entrancy cap, cross-thread publish.

### 7.3 ConsolePanel (1:1)

- Editor installs a Logger callback (existing `AddCallback` API retained) that re-pushes
  entries into a fixed **1024-slot deque** as `LogEntrySignal`s drained once per frame
  (event bus hop — spec §7.2). Each entry: time, level, channel, text, monotonic id.
- UI: filter row — channel combo (All + per-channel learned dynamically), level toggles
  (Trace/Debug/Info/Warn/Error/Critical as icon toggle buttons), case-insensitive search
  box with **match count**, auto-scroll toggle, Clear (own buffer only — never touches
  engine state), Export… (writes sanitized log to `.raven/logs/console_<timestamp>.txt`).
- Rendering: `ImGuiListClipper` row loop (1:1 — 10k entries must not blow up; cap is 1024
  but scroll math stays clipper-based), zebra rows, per-level icon+color from semantic
  table, wrap off, horizontal scrollbar on.
- `[xN]` collapse for identical consecutive messages (count shown) — 1:1 QoL copy.
- Error boundary: exceptions inside ConsolePanel itself are caught by §3.2 like any panel.

---

## 8. Workspaces & persistence (`editor/core/Workspace.*`, `EditorSettings.*`)

1:1 semantics:

- **Workspace** = ImGui dock-state file + per-panel visibility sidecar:
  - Built-in: `<engineAssets>/workspaces/Default.ini` + `Default.workspace.json` (read-only;
    shadows any same-named user copy). Engine assets path resolves as
    `FileSystem::EngineAssetsPath("workspaces")` — new engine API (compile-time
    `RAVEN_ENGINE_ASSETS_DIR` macro falling back to `<repo>/editor/assets` in dev).
  - User: `<project>/.raven/layouts/<name>.ini` + `<name>.workspace.json`.
  - Sidecar schema: `{ "schema": 1, "panels": { "Hierarchy": true, "Inspector": true, … } }`.
    Captured on `SaveActiveWorkspaceSidecar()` (end of shutdown), applied on
    `LoadWorkspace` (flips each panel's `m_Open`; unknown keys ignored).
- **EditorSettings** (`<configDir>/RavenEngine/settings.json`; configDir = XDG_CONFIG_HOME
  or ~/.config (Linux), %APPDATA% (Windows), ~/Library/Application Support (macOS)):
  active workspace name, theme preset name, camera/skybox defaults, window geometry
  hints, recent projects list (max 10), autosave interval (minutes, 0=off), default
  assets root name. Versioned schema; unknown keys preserved.
- **Load order**: engine boot → `Editor::Init` creates context → panels constructed →
  `LoadBuiltin(activeWorkspace)` applied **after** first window begin (deferred one frame
  in `Render` so panel windows + dock node exist before docking state reload) — Luth's
  `s_NeedActiveWorkspaceLoad` trick, 1:1.
- **Commands**: `LoadWorkspace/SaveWorkspaceAs/Rename/Delete/ResetWorkspaceToBuiltin/
  SaveActiveWorkspaceSidecar/GetWorkspaces` — statics on `Editor`, mirroring Luth.
- **Menus**: `Window > Workspaces >` (Switch…, Save As…, Rename…, Delete, Reset to
  Built-in), `Window > Reset Layout`, checkbox per panel directly under Window (toggles
  `m_Open`, separate from per-frame `m_Visible`).
- `Editor::Shutdown`: save settings → save active sidecar (skipped when active workspace
  is built-in — read-only).
- Headless mode: workspaces disabled entirely; layout always ephemeral.

---

## 9. Projects, launcher, two-phase startup

### 9.1 Project model

- Manifest: keep existing `project.ravenproject` (name, projectVersion, assetsRoot,
  scenes[], defaultScene) — no extension rename. `FileSystem::LoadProject(path)` parses
  + validates (already exists? — `sample/project.ravenproject` exists; wire through).
- Project root = manifest's directory. Hidden metadata dir: `<project>/.raven/`
  (`layouts/`, `thumbnails/`, `logs/`, `dumps/`, `pipeline_cache/`, `autosave/`).
- Recent projects: `%APPDATA%/RavenEngine/recent_projects.json` (platform configDir),
  max 10, MRU order, stored paths; missing entries tolerated silently.

### 9.2 Two-phase startup (1:1)

- Phase A: engine/sandbox boots with **no project**. `Editor::Init` runs; all panels
  exist; ProjectPanel shows "No project loaded" placeholder; Inspector/Hierarchy show
  empty-scene placeholder; menu has File>Open Project…, File>Project Launcher….
- Phase B: `App::LoadProject(path)` (engine, triggered by launcher selection, recents
  shortcut, CLI `--project`, or `.ravenproject` drag-drop onto window) → engine calls
  `hooks->OnProjectChanged()` (guarded) → editor reloads settings, clears scene,
  refreshes panels, registers assets root with `AssetDatabase` (Initialize on
  `assetsRoot`), hydrates thumbnail disk cache, loads default scene, enters normal
  operation.
- Recent projects persisted on every successful load. Launcher overlay (grid of cards:
  name, path, relative time, version) — 1:1 `ProjectLauncher`; New Project dialog
  (name + location → creates layout `<dir>/<name>/{<name>.ravenproject, assets/…}`);
  right-click recent → remove from list; double-click opens.
- CLI: `ravened [--project <dir>] [--scene <file>] [--headless] [--frames N]
  [--capture out.png] [--validate-gpu]` — extend existing arg parser.

### 9.3 Autosave (1:1)

`EditorAutoSave`: timer thread (not fiber), interval from settings (default 5 min),
writes `<project>/.raven/autosave/<scene>.<epoch>.ravenscene`, keeps last 3, skipped in
Play mode and when scene not dirty. Status-bar toast on save ("Autosaved HH:MM:SS").
Restore flow is manual (File > Recent Autosaves…) — no magic recovery in this phase.

---

## 10. Assets: browser, thumbnails, import pipeline

### 10.1 ProjectPanel (1:1)

- Left pane: recursive folder tree (dirs only, icons from semantic set, current folder
  highlighted; collapse/expand all). Right pane: grid (4–8 cols by width) of entries with
  64×64 thumbnail + name + ext badge; or details-list mode toggle.
- Search: top bar, recursive case-insensitive substring over relative paths; results show
  flattened list; Esc restores prior folder context.
- Context menus: background (New Folder / New Material / New Shader / Import… / Reveal in
  File Manager / Refresh), item (open/edit per type, Rename F2, Duplicate, Delete (to .raven
  trash? No — hard delete via command chain, documented), Copy Path, Copy UUID, Open…).
- Drag-drop **out** of panel: `ASSET_UUID` payload with type byte; accepted by inspector
  asset slots (§6.3) and material maps; hierarchy model-drop instantiation (§5.4).
- Drag-drop **into** panel: OS file drop → Import pipeline (§10.3), eager import with
  progress toast; ambiguous texture discovery for models triggers TextureRemapDialog
  (§10.4).
- Selection: single + Ctrl toggle (multi-select shows in inspector as type-grouped
  "N assets selected" — batch ops: Reimport, Copy UUID list).
- Refresh: button + FileWatcher-driven; rescans AssetDatabase; new `.meta`s minted.

### 10.2 Thumbnails (`editor/widgets/Thumbnail*.{h,cpp}`)

1:1 implementation:

- `ThumbnailCache` — in-memory map `UUID → {pixels?, state, lastWrite}`; states
  Pending/Ready/Failed; persisted PNG disk cache `<project>/.raven/thumbnails/<uuid>.png`
  re-hydrated by `ScanDiskCache()` lazily on first project open (parallel job).
- `ThumbnailGenerator` — runs on `JobSystem` workers (never blocking UI):
  - **Textures**: `stb_image` decode → aspect-preserving fit into 128px box → PNG encode
    (stb_image_write, already vendored) → write + cache push.
  - **Models**: enqueue a hidden `ThumbnailPreviewScene` render (directional key + rim
    light, 3/4 orbit framing on AABB) via the offscreen renderer; read back → PNG.
  - **Materials**: same preview scene with a lit sphere + checkered HDRI intensity
    background; fragball-style variant for emissive test (v2).
  - Failures: Failed state → generic type icon with error tint; never block the grid.
- Cascade invalidation: `AssetChangedSignal{uuid, Modified}` → material thumbnails
  referencing that texture are marked stale (dep set recorded at bake time, 1:1 with
  Luth `SetEntryDeps`); scene-saved events don't invalidate.
- UI: grid cells request `ThumbnailCache::Get(uuid)` → `Ready` paints `ImGui::Image`
  (descriptor cache §5.1), `Pending` paints placeholder + spinner, `Failed` type icon.
- Memory: textures kept as RGBA8 in VMA/GPU? No — thumbnails are CPU-side PNGs decoded
  to RAM, uploaded once to ImGui pool textures (descriptor cache). Cap in-flight bakes
  at `2×worker count`.

### 10.3 Import pipeline wiring (engine to editor)

- `AssetManager::LoadAsync` → UI progress (toast + determinate bar) through the bus.
- New assets drop in → `AssetDatabase::Initialize` rescan; type from extension table
  (centralize `AssetTypes.cpp` mapping — extend with `.ravenmaterial`, `.ravenshader`,
  `.ravenanim`).
- **BCn import** (`raven_bc7enc`, already wired as a target, unused): on texture import,
  if `ImportSettings::compress == true` (project settings, default ON for color/normal
  maps): BC7 for color/roughness/metalness/emissive, BC5 for normals; encode on job
  thread; store under `<project>/.raven/cache/bc7/<uuid-hash>.dds` with `.meta`
  `sourceHash`; runtime picks `_color.dds/_normal.dds` when present, falls back to
  source. ISPC path stays excluded.
- **Hot reload**: `FileWatcher` (exists) on assetsRoot → `AssetChangedSignal` →
  consumers (material graph, texture cache, script system) reload in place. Shader cache
  invalidation hook (§12).
- **Smart import (1:1)**: model importer (`assimp` submodule, `RAVEN_USE_ASSIMP=ON`)
  gathers candidate textures per map slot via name rules (`_d, _n, _orm, _rough…`);
  all-confident → import directly; any slot ambiguous → `ImportReport` →
  `ImportReportSignal` → `TextureRemapDialog` modal (candidate combo + browse fallback);
  commit writes `.meta` texture remap section + triggers reimport.

### 10.4 TextureRemapDialog (1:1)

Modal, non-docked; title "Texture Remap — <model>"; table rows per slot
(Slot | Detected candidates (combo) | Browse… button | Status icon); bottom: Import
(disabled until all rows resolved) / Cancel; commit → `.meta` `textureRemap` JSON section
+ reimport + toast. Escape cancels. Results persist across sessions via the model's
`.meta` — dialog never re-asks for the same model+slot.

---

## 11. Profiler (`panels/ProfilerPanel.cpp`)

Two cooperating layers, 1:1 with Luth:

1. **Tracy** (deep dive): keep `RAVEN_ENABLE_TRACY=ON` opt-in; add `ZoneScopedN` to panel
   gathers (`PanelName::Gather`) and to `AssetManager::Import`, `CommandStack::Undo/Redo`,
   `thumbnail bake`. `TRACY_FIBERS` + `TRACY_ON_DEMAND` defines when ON. Docs unchanged.
2. **In-editor dashboard** (always available):
   - **Frame**: CPU frame graph (existing PlotLines, restyled 120-sample), GPU frame
     graph (from renderer's GPU timestamps — extend `FrameStats` with per-pass gpu ms
     once RG records timing; today: CPU ms only, GPU column shows "n/a").
   - **Scheduler**: worker count, pending jobs (`JobSystem::PendingJobs`), ParallelFor
     invocations/frame, (fiber-aware counters appear when fiber job system lands: fiber
     count, migrations, steal rate — counters reserved in the stats struct now).
   - **Memory**: `raven::MemoryTracker` (new, §11.1) per-category current/peak/budget;
     VMA heaps via `vmaGetHeapBudgets`; **leak report**: on shutdown, any category whose
     current != 0 at exit prints the top-8 allocation sites (thread-hashed stack in debug
     builds; plain counters in release) to console + `.raven/logs/leaks_<ts>.txt`.
   - **GPU**: draw calls, triangles, mesh count, LOD culled (existing `FrameStats`),
     VRAM used/budget, dynamic-rendering/timeline-semaphore status line (existing).
   - Update throttled to 10 Hz; `PROFILER_OFF` build flag removes the timers.

### 11.1 MemoryTracker (`engine/include/raven/core/MemoryTracker.h`)

```cpp
namespace raven {
enum class MemCategory : uint8_t { Core, Renderer, Assets, Editor, Physics, Audio, Script, Other, kCount };
struct MemCategoryStats { std::atomic<int64_t> current{0}, peak{0}, totalAlloc{0}, totalFree{0}; };
class MemoryTracker {
public:
    static MemoryTracker& Instance();
    void Add(MemCategory, size_t bytes);          // +bytes → current/peak/totalAlloc
    void Sub(MemCategory, size_t bytes);          // −bytes → current/totalFree
    MemCategoryStats Snapshot(MemCategory) const;  // racy-by-design read for UI
    std::string LeakReport() const;                // called at shutdown
};
}
```
No allocation-site tracking in v1 (hash of `__builtin_return_address(0)` optional debug
only). Every engine `new` stays untouched — tracker adoption is incremental, starting
with renderer + assets + editor.

---

## 12. Frame debugger (`panels/FrameDebuggerPanel.cpp`)

Requires renderer cooperation; phased:

- **v1 (freeze + scrub, no replay)**: with `-DRAVEN_FRAME_DEBUGGER=ON`, `Renderer` wraps
  each pass region with `Tracy` zones (exists) **and** captures a
  `RenderGraphDebugSnapshot` at encode time: per pass — name, barrier list, resource
  reads/writes with byte ranges, pipeline/shader names, draw count, timestamps (GPU
  query pool). Stored in a 4-frame ring in the renderer, copied out on demand.
  - Panel: freeze toggle (pauses scene updates, keeps UI live), pass tree (left),
    per-pass attribute table (middle), resource list w/ sizes (right), draw slider
    scrubbing to any draw event (shows pipeline state + resource bindings), "Replay
    single draw" **disabled in v1** (button present, tooltip explains v2).
  - Notice: entering freeze publishes `FrameDebuggerNoticeSignal` → console entry.
- **v2 (replay)**: re-record the graph from the snapshot restricted to the selected draw
  (+ required resource producers up-chain) into the viewport target. Depends on RG
  command recording (Phase 5 engine work) and shader-reflection side tables
  (`raven_spirv_cross` wired). Estimate: L; not scheduled until RG records draws.
- Aftermath: `RAVEN_ENABLE_AFTERMATH=ON` + `NsightAftermath_ROOT` (proprietary, not in
  repo, already documented in BUILDING.md) — device-lost handler dumps
  `.nv-gpudmp` to `.raven/dumps/` + `FrameDebuggerNoticeSignal("Device lost — dump …")`;
  console error path tested but not the dump content.

---

## 13. Material node editor (`panels/MaterialGraphPanel.cpp`, phase 6)

Last feature; heavy dependency chain (Slang asset pipeline must exist).

- **GraphEditor**: Luth uses ImGuizmo's `GraphEditor` (CedricGuillemet/ImGuizmo v1.83 —
  MIT; **already vendored but trimmed to ImGuizmo.cpp only**). RE: add `GraphEditor.cpp/
  .h` from the same upstream snapshot into `third_party/imguizmo/` (+ attribution line in
  file header — upstream is MIT, retain), register in `raven_imgui` sources. No other
  ImGuizmo modules needed (GraphEditor only includes imgui/imgui_internal).
- Model: `raven::renderer::MaterialGraph` (engine) — nodes/pins/links, JSON serde
  (`SaveMeta`/`LoadMeta`), stable node ids, dirty flag. Built-in node library v1:
  Texture Sample, Constant (scalar/color), Combine/Append, Multiply/Add/Lerp, Normal
  Transform, Output (single fragment output), Parameter (graph-exposed named uniform).
  No cycles (topo sort on edit), type checking on link (float/vec2/vec3/vec4/texture).
- Codegen: `MaterialGraphCodegen::EmitSlang(graph) -> std::string` — flat, unrolled
  straight-line Slang with named temps (`t0`, `t1`…), identical for raster + RT paths
  (both consume the same `.slang` fragment via Slang's module system).
  Reflection via `spirv-cross` (vendored) extracts exposed Parameters → drives
  Material inspector "Parameters" section (1:1 with Luth).
- Live recompile: graph edit → debounce 300 ms → write `.raven/cache/materials/<uuid>.slang`
  → slangc via `RAVEN_USE_SLANG` path (submodule) → on success swap pipeline (pipeline
  cache invalidation targets just `<uuid>` variants), on failure retain old binary +
  red squiggle on offending node + console error with Slang diagnostics.
- Inspector integration (Material inspector, §6.3): "Surface Settings / Base / lobes
  (Clear Coat, Anisotropy, Transmission, Sheen, Subsurface — checkbox-in-header, **weight
  is the single source of truth**, `m_RevealMask` transient bit) / Parameters (graph-
  exposed) / Advanced (raw uniforms, hidden when empty)". Mask Map packs metal/rough
  (metallic=B, roughness=G, glTF convention); hides scalar sliders while the packed slot
  is bound (1:1).
- 3D preview footer: `ThumbnailPreviewScene` instance pinned to inspector bottom;
  orbit-drag; updates on any material change.

Deferred (v2, not designed here): SSS/sheen shader models, RT parity for custom graphs,
decal node, POM node — all ROADMAP.md material items.

---

## 14. Settings window & preferences (`panels/EditorSettingsWindow.cpp`)

Two-pane (1:1): left section list (resizable `Splitter`, search box on top filtering
rows across sections), right scrollable body; commit on each control (no Apply button,
except window-geometry ones) → `SaveSettings` (§8) + `ApplyPersistence` (camera
defaults, skybox rotation, grid toggle) live.

Sections: **Editor** (theme preset, icon size 11/13/16, autosave minutes, layout preset
waterfall) · **Rendering** (quality tier: Performance/Balanced/Ultra — maps onto existing
`raven::Quality` presets; exposure, vsync) · **Camera** (fly speed, orbit sensitivity,
invert Y) · **Physics** (debug draw toggles: shapes/AABBs/CoM, scope Selected/All, color
mode, alpha) · **Assets** (assets root, BCn compress default, parallel import workers)
· **Shortcuts** (static table v1; editable v2 — stored as JSON overrides) ·
**Advanced** (enable frame debugger UI, enable parallel gather, log level).

---

## 15. File/directory layout (target)

```
editor/
  CMakeLists.txt          # raven_editor STATIC + ravened EXE
  main.cpp                # thin entry: args, InstallEditorHooks, run (no UI logic)
  core/                   # Editor.{h,cpp}, EditorHooksImpl, ConsoleBridge, Fonts.cpp,
                          # StylePreset.*, EditorStyle.h, Workspace.*, EditorSettings.*,
                          # EditorSnapshot.h, EditorCommandStack.h
  events/EditorSignals.h
  selection/EditorSelection.*
  commands/               # ICommand impls + compounds + debounce + factory
  panels/                 # Panel.h, registry, all panels (§3.3)
  inspectors/             # ComponentDrawerRegistry + per-component drawers + resource editors
  viewport/               # ViewportRenderer, EditorCamera, GizmoController, Marquee, OutlineOverlay
  widgets/                # Property*, AssetSlot, CollapsingHeader, Charts, Thumbnail*,
                          # TexturePreview, FilterBox, Splitter, …
  Icons.h                 # semantic layer (new)
  phosphor_icons.h        # raw defines (generated, unchanged ownership)
  icons_phosphor_fill.h   # raw Fill defines (generated, new)
  assets/styles/Default Dark.json, Default Light.json
third_party/imguizmo/GraphEditor.{h,cpp}   # added, MIT attribution
engine/include/raven/core/{EventBus.h, EditorHooks.h, StackTrace.h, MemoryTracker.h, LogEntry.h}
engine/include/raven/memory/LinearAllocator.h
engine/src/core/{EventBus.cpp, EditorHooks.cpp, MemoryTracker.cpp, StackTrace*.cpp}
tests/test_eventbus.cpp test_linear_allocator.cpp test_editor_commands.cpp
       test_scene_uuid.cpp test_workspace_sidecar.cpp test_selection.cpp
docs/EDITOR_DESIGN.md    # this file
```

---

## 16. Phased delivery & acceptance criteria

**Phase 0 — Engine prerequisites** (≈ days; low risk)
EventBus + tests; `IEditorHooks` + registry + layering check script; entity `UuidComponent`
+ scene schema v3 + migration tests; `LinearAllocator` + tests; `MemoryTracker` + tests;
`StackTrace` (POSIX only v1); `CommandStack` extended (`BeginCompound/EndCompound`,
observer entries list) + tests.
*Done when:* `ctest` ≥ 90% pass including new suites; `raven-sandbox` links no editor
code; scene v1/v2/v3 round-trips pass; layering script passes.

**Phase 1 — Shell parity** (≈ 1–1.5 weeks)
Editor static-lib split; two-phase startup + launcher + recent projects; semantic icon
layer migration; font triple (Regular/Fill merge + 64px bakes + Fill vendored); style
presets + theme switch live; Docking + viewports flag; Panel base + registry + error
boundary + crashed placeholders; workspaces end-to-end (builtin Default, save/load,
sidecar, Reset Layout, Window>Workspaces); settings window (Editor+Assets sections);
autosave; console channels scaffold; headless capture parity screenshot.
*Done when:* fresh clone → `cmake -B build && cmake --build` → `ravened --headless
--frames 3 --capture` shows docked layout with all placeholder panels; workspace save →
edit layout → restart → layout restored; kill -SEGV after 3 panel throws → placeholder
shown, other panels alive, editor survives.

**Phase 2 — Editing core** (≈ 2 weeks)
Selection singleton + bus wiring; hierarchy multi-select/interactions/deferred queue;
Inspector registry + MultiEdit + Add Component + copy/paste/clipboard; gizmo controller
with coalesced undo; 12 command types (material commands stubbed to MaterialSnapshot
JSON); shortcuts; HistoryPanel; ConsolePanel bus hop + 1024 cap + clipper + channels.
*Done when:* every toolbar/menu/inspector edit path is Ctrl+Z-able in tests
(`test_editor_commands.cpp` drives headless scene mutations incl. gizmo coalescing);
multi-edit of 3 entities → one undo restores all; HistoryPanel click-to-time-travel
works; 10k console entries at 144 fps.

**Phase 3 — Assets & browser** (≈ 2 weeks)
ProjectPanel tree+grid+search; thumbnails full stack; disk cache; drag-drop payloads;
browser context menus; AssetDatabase wired (rescan, signals); BCn import path behind
settings; FileWatcher hot reload → AssetChangedSignal; smart import + remap dialog;
model import via assimp (RAVEN_USE_ASSIMP=ON path); material/texture resource inspectors.
*Done when:* drop a glTF with OCMR naming ambiguity → dialog resolves → import completes
→ grid shows rendered previews; thumbnail survives project reload via disk cache;
shader/texture edit hot-reloads visible in viewport.

**Phase 4 — Runtime parity** (≈ 1 week)
PlayState machine; PlayModeController; transport bar + GamePanel (letterbox, camera view);
scene snapshot round trip on Play/Stop (stop restores exactly, dirty flag preserved);
Pause + Step; physics substep early-out in Editing (PhysicsWorld gets
`SetSimulationEnabled`); scripts gated (ScriptSystem respects m_RunGameSystems);
FrameDebuggerNotices surfaced; aftermath flag exists (docs-only OK).
*Done when:* Play → move things → Stop → pre-play scene restored bit-for-bit (JSON
diff empty); Paused frame-step works with physics bodies frozen in editor; transport
keyboard shortcuts work (Ctrl+P play, Ctrl+Shift+P pause, Esc stop).

**Phase 5 — Tooling** (≈ 1–2 weeks)
Profiler v2 (frame/scheduler/memory/GPU at 10 Hz, leak report on exit); FrameDebugger v1
(freeze, pass tree, draw scrub, pipeline/resource tables, notice signal); ResourcePanel
(DB table + filters); Validation layer: tiered validation settings (off/basic/sync) in
preferences; aftermath hook compiled only with `RAVEN_ENABLE_AFTERMATH=ON`.
*Done when:* leak report flags a deliberate leak (test-mode alloc); freeze frame →
scrub to draw 412 → attribute table shows pipeline hash + resource bindings; GPU time
per pass displayed on RG builds that record timing.

**Phase 6 — Material graph** (≈ 2–3 weeks, gated on Slang pipeline)
GraphEditor vendored; MaterialGraph + codegen + reflection; Material editor inspector
full feature gating + mask map behavior; 3D preview footer; pipeline-cache targeted
invalidation; ShaderEditor/ModelViewer/FontViewer/SceneViewer arrive opportunistically.
*Done when:* graph edit → 300 ms → viewport shows updated material; failing graph
retains last good binary; parameter exposed in graph appears in inspector; screenshot
diff vs reference sphere matches tolerance.

**Total: ~8–12 focused weeks.** Phases 1–4 deliver the "feels like Luth" editor; 5–6
deliver "is Luth". Phases are strictly ordered; each lands as its own merge train of
small PRs (5–8 files per PR max), on per-phase branches (e.g. `arena/ae2e82a6-ravenengine` carries this document + Phase 0).

---

## 17. Parity matrix (Luth → RavenEngine)

✓ = exists and wired · ◐ = exists, unwired/partial · ✗ = missing · Phase column = §16

| # | Luth subsystem | Luth source | RE today | RE target | Phase |
|---|---|---|---|---|---|
| 1 | Engine/editor hook registry | `EditorHooks.h` | ✗ | `IEditorHooks` + `EditorHooks::Get` | 0 |
| 2 | EventBus + signals | `EventBus` | ✗ | core `EventBus`, `EditorSignals.h` | 0 |
| 3 | Entity UUID | `UUIDComponent` | ✗ (EnTT handles) | `UuidComponent` + schema v3 | 0 |
| 4 | CommandStack cap 100 + compounds | `CommandHistory` | ◐ (core exists, unused) | extend + `EditorCommandStack` | 0/2 |
| 5 | LinearAllocator | `memory/LinearAllocator` | ✗ | `raven::memory::LinearAllocator` | 0 |
| 6 | Panel base + begin introspection | `panels/Panel.h` | ✗ | `panels/Panel.h` | 1 |
| 7 | Error boundary 3-strike + placeholder | `Editor::DrawPanelGuarded` | ✗ | editor registry guard | 1 |
| 8 | EditorSnapshot type-keyed | `EditorSnapshot.h` | ✗ | gather/draw snapshot | 1–2 |
| 9 | Two-phase startup + launcher | `ProjectLauncher` | ✗ | launcher + recents + `OnProjectChanged` | 1 |
| 10 | Phosphor Regular merge | fonts | ✓ (done this cycle) | + Fill + 64px bakes | 1 |
| 11 | Semantic ICON layer | `widgets/Icons.h` | ✗ | `editor/Icons.h` | 1 |
| 12 | Style presets JSON | `EditorStyle` | ✗ | `StylePreset` + Live apply | 1 |
| 13 | Workspaces (ini + sidecar) | `Workspace` | ✗ | full subsystem | 1 |
| 14 | Settings window two-pane | `EditorSettingsWindow` | ✗ | two-pane, search, sections | 1 |
| 15 | Auto-save | `EditorAutoSave` | ✗ | timer thread + `.raven/autosave` | 1 |
| 16 | Multi-select singleton + signals | `EditorSelection` | ✗ (single m_Selected) | singleton + `SelectionChangedSignal` | 2 |
| 17 | Hierarchy multi-select/DnD/F2/deferred ops | `HierarchyPanel` | ◐ (single select, no DnD) | full rework | 2 |
| 18 | Inspector registry + drawers | `ComponentDrawerRegistry` | ✗ (fixed if-chain) | registry + reflection-ish table | 2 |
| 19 | MultiEdit fan-out | `MultiEdit.h` | ✗ | broadcast context | 2 |
| 20 | Add Component popup | InspectorPanel | ✗ | searchable category popup | 2 |
| 21 | Gizmo coalesced command | GizmoController | ✗ | `GizmoTransformCommand` | 2 |
| 22 | Clipboard copy/paste component | `EditorClipboard` | ✗ | JSON fragment clipboard | 2 |
| 23 | HistoryPanel | `HistoryPanel` | ✗ | CommandStack observer UI | 2 |
| 24 | Console bus + cap + clipper + channels | `ConsolePanel` | ◐ (AddCallback deque, no cap via bus, no channels) | full rewrite | 2–3 |
| 25 | ProjectPanel tree+grid+thumbs | `ProjectPanel` | ◐ (flat list, no thumbs) | §10 | 3 |
| 26 | Thumbnail baker + disk cache | `Thumbnail*` | ✗ | §10.2 | 3 |
| 27 | BCn import (BC7/BC5) | bc7enc integration | ◐ (dep wired, unused) | import-time toggle | 3 |
| 28 | Hot reload round-trip | FileWatcher + signals | ◐ (watcher exists, no signals) | `AssetChangedSignal` chain | 3 |
| 29 | Smart import + remap dialog | assimp + `ImportReport` | ✗ | pattern match + dialog | 3 |
| 30 | Drag-drop payload ASSET_UUID | panels | ◐ (none) | payload codec + accept slots | 3 |
| 31 | PlayState machine + snapshot | `PlayModeController` | ◐ (Scene round-trip exists) | controller + transport bar | 4 |
| 32 | GamePanel camera view | `GamePanel` | ✗ | letterboxed camera panel | 4 |
| 33 | Physics/script gating by PlayState | engine hook reads | ✗ | `m_RunGameSystems` logic | 4 |
| 34 | Profiler dashboard 10 Hz | `ProfilerPanel` | ◐ (minimal PlotLines) | v2 §11 | 5 |
| 35 | VMA/memory stats + leak report | Memory (Luth-wide) | ✗ | `MemoryTracker` + VMA budgets | 5 |
| 36 | FrameDebugger v1 freeze/scrub | `FrameDebuggerPanel` | ✗ | RG debug snapshot §12 | 5 |
| 37 | ResourcePanel DB table | `ResourcePanel` | ✗ | AssetDatabase + refcounts | 5 |
| 38 | Material node graph + Slang | `MaterialGraphPanel` | ✗ | §13 (phase 6) | 6 |
| 39 | Mask map channel semantics + lobes | MaterialEditor | ✗ | §13 inspector | 6 |
| 40 | 64px icon bakes + grid | EditorStyle/ProjectPanel | ✗ | §2.1 | 1 |
| 41 | Outline (selection) | ScenePanel | ✗ (ImGuizmo highlight only) | overlay brackets (p2) → outline pass (p4+) | 2/4 |
| 42 | Marquee select | ScenePanel | ✗ | screen-projection origin test | 2 |
| 43 | Aftermath crash dump | Nsight (opt-in) | ✗ | flag + dump dir (proprietary, docs-only) | 5 |
| 44 | 4-cascade PSSM A/B toggle | Renderer | ✗ | ROADMAP renderer, viewport toggle UI | 4 |
| 45 | Workspaces/recent projects/persistence | `%APPDATA%` files | ✗ | §8/§9 | 1 |

Net: **2 ✓, 8 ◐, 35 ✗**. Parity is achievable but it is a program, not a tweak — hence
the phase plan.

---

## 18. Risks & non-goals

**Risks**
- *Scope creep from "one-to-one".* This doc bounds parity to the editor layer; engine
  systems land via ROADMAP.md. Reject PRs that sneak renderer rewrites into editor phases.
- *Fiber job system.* Gather executes on main thread until fibers land; do not block
  Phases 1–4 on the fiber rewrite (ROADMAP core item). Parallel gather flag default-flips
  only after fibers exist.
- *Scene schema v3 migration.* Mitigate with migration tests pinned in `ctest`; keep
  v1/v2 readers permanently (cheap), emit only v3.
- *GraphEditor vendoring.* Version skew with our imgui (docking 1.93.1) — build must stay
  green; if upstream GraphEditor breaks, pin the file to the v1.83 snapshot and never
  rebase it independently.
- *Memory tracker overhead.* Counters are atomic adds on hot paths; acceptable for
  editor builds; `RAVEN_MEMORY_TRACKING` define gates for paranoid release builds.
- *Windows-first truth.* Luth is Windows-only; RavenEngine stays cross-platform — nothing
  in this design may *require* Windows (StackTrace POSIX first, configDir portable),
  but parity testing happens primarily on Windows + Linux.
- *Licensing.* Luth is MIT. We may port **code** with attribution (headers RETAIN upstream
  copyright when files are copies, e.g. GraphEditor) or **techniques** without. Default
  is technique port; code copy is called out explicitly in file headers when used
  (Fonts.cpp, StylePreset.* etc. are clean-room reimplementations).

**Non-goals (explicitly out of this design)**
- Implementing SVGF/clustered lighting/RT/bindless/etc. — ROADMAP.md.
- Node graph *runtime parity* (SSS/sheen lobes in custom Slang graphs), POM node, decal
  node — v2 of Phase 6.
- Editor themes beyond Dark/Light built-ins (no community theme format).
- Touching `.ravenscene` semantics beyond the v3 UUID addition.
- ImGuizmo replacement; ImGui multi-viewport polish on Wayland (flag stays opt-in there).
- Editor automation/testing of GUI clicks (all logic tested below the UI layer only).

---

## 19. Open questions for the project owner

1. **Merge target for this design** — commit `docs/EDITOR_DESIGN.md` into PR #4 (expands
   that PR), or into a fresh branch once #4 merges? (File currently untracked.)
2. **Implementation go-ahead** — start Phase 0 (engine prerequisites) immediately, or wait
   for PR #4 merge to avoid conflicts in `engine/`?
3. **Entity UUID schema bump** approval (v2→v3, auto-mint on load) — OK?
4. **Editor asset folder** `editor/assets/` for built-in styles/workspaces (vs
   `sample/assets`) — OK?
5. **Multi-viewports** default ON (Win/X11) — OK, or keep single-viewport until Phase 4?
6. **Phosphor-Fill** vendored now (needed for Phase 1) — approved, or Regular-only for
   the first cut?
7. **GraphEditor code copy** (MIT, attribution-retained) accepted, or technique-port-only
   even for it?
8. **Autosave cadence** default (5 min, keep 3) OK?
9. **Transport shortcuts** — Ctrl+P / Ctrl+Shift+P / Esc (Luth: same) or editor-conventional
   F5/F6/F7?
10. **Aftermath** — docs-only (current state), or wire `RAVEN_ENABLE_AFTERMATH` skeleton
    in Phase 5 with a stub dump test (no NVIDIA SDK on CI)?
