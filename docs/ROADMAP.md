# RavenEngine target architecture (project owner's spec, 2026-11-27)

This document reproduces the feature specification the project owner supplied when
asking for the extended dependency set (see third_party/VERSIONS.md) and the editor
icon replacement. It is the **target** the engine is being built toward — it is not a
description of what is implemented today. Treat entries as unstarted until they land in
the codebase.

## Core architecture

- **Fiber job system.** One worker thread per core. Logical tasks run as lightweight,
  migratable fibers that yield to the scheduler instead of blocking the OS, so the CPU
  stays near-saturated. FLS, Chase-Lev work-stealing deques, lock-free queues, spin-locks
  on the hot path. The context switch is hand-written x86-64 assembly over `VirtualAlloc`
  stacks, so every fiber stack is AddressSanitizer-trackable.
- **Pipelined frames.** Game (N), Render (N-1), and GPU (N-2) run at the same time. The
  handoff between stages is an immutable per-frame snapshot, never shared mutable state.
- **Tagged allocators.** No `new`/`delete` on the hot path. A Naughty Dog-style Onion/Garlic
  split: CPU and GPU tagged page allocators (bulk-freed by tag, GPU pages retiring on the
  timeline) plus a per-frame linear allocator.
- **Bindless Vulkan 1.3.** Dynamic rendering and timeline semaphores only, no legacy render
  passes or fences. One global texture array and buffer device addresses mean draws carry
  integer indices instead of rebinding.
- **Render graph.** Each frame builds a DAG of passes that declare their reads and writes.
  The graph solves barriers, culls unused passes, and records commands in parallel across
  workers.
- **Dependencies** (all vendored/pinned this cycle): Vulkan SDK, Slang (shader compiler),
  SPIRV-Cross (reflection), VMA, EnTT, ImGui + ImGuizmo + Phosphor Icons (editor), Tracy,
  Nsight Aftermath (optional, proprietary), GLFW, GLM, spdlog, assimp, stb_image,
  bc7enc_rdo, nlohmann/json, Jolt Physics.

## Rendering

- **Denoising** — SVGF across three channels (diffuse DI, indirect GI, specular) behind a
  swappable `IDenoiser`.
- **Clustered Forward+** — log-slice light clusters with a slim G-buffer prepass,
  ECS-driven. Lights: directional sun, plus clustered and GPU-culled point and spot lights.
- **PBR Materials** — Cook-Torrance metallic/roughness with Opaque, Cutout, and
  order-independent Transparent modes.
- **Node Material Editor** — Blender-style graph that generates Slang per material,
  identical raster and RT output, live recompile, exposed parameters.
- **Shading Models** — clear coat, anisotropy, dielectric transmission (glass), sheen
  (cloth), subsurface scattering (skin), across the raster and path-traced paths.
- **Surface Detail** — parallax occlusion mapping and projected decals, authored in the
  node graph.
- **Shadows** — RT ray-query sun shadows by default; 4-cascade PSSM kept as an A/B toggle.
- **Volumetric Fog** — froxel grid: inject, integrate, temporal resolve, composite.
  Optional per-froxel RT fog shadows.
- **Ambient Occlusion** — GTAO half-res compute (prefilter, horizon integral, bilateral
  denoise).
- **IBL** — HDR skybox, diffuse irradiance, pre-filtered specular, BRDF LUT, split-sum
  ambient.
- **Anti-Aliasing** — TAA plus specular anti-aliasing.
- **Vertex Deformation** — GPU compute deformation shared by raster and the RT BLAS,
  driven by procedural wind.
- **GPU Culling** — compute frustum culling per cascade and main scene; indirect draws
  throughout.
- **Async Compute** — three-queue Vulkan topology (graphics, async compute, transfer);
  GTAO and RT chains overlap on the compute queue.
- **Bindless** — buffer device addresses and one global 16k-texture array; integer
  material and texture indices.
- **Post-Processing** — HDR pipeline: bloom, ACES and AgX tonemap operators, vignette,
  grain, chromatic aberration.
- **Shaders** — Slang-only asset pipeline with UUIDs, hot-reload, SPIRV-Cross reflection.
- **Pipeline Cache** — disk-persisted, lazy variant creation, targeted hot-reload
  invalidation.

## Animation

- **Sampling** — fiber-parallel keyframe evaluation.
- **GPU Skinning** — bone-matrix SSBO, vertex-shader skinning.
- **Blending** — SQT interpolation, crossfade transitions, layered override with bone masks.
- **Root Motion** — extracted and applied to the entity transform.
- **Debug** — bone overlay in the editor viewport.

## Physics

- **Backend** — Jolt Physics, jobified onto the fiber scheduler.
- **Rigid Bodies** — static / kinematic / dynamic with CCD; primitive, convex-hull, and
  mesh shapes.
- **Materials** — UUID-keyed friction/restitution/density with hot-reload.
- **Character Controller** — kinematic capsule (`JPH::CharacterVirtual`) with stair-stepping
  and stick-to-floor.
- **Queries** — raycast and overlap (box/sphere/capsule), layer-mask filtered.
- **Events** — contact and trigger add/remove, drained per frame.
- **Debug Draw** — wire colliders colored by motion state or ground state.

## Asset pipeline

- **Asset Database** — UUID registry with `.meta` sidecars; importers for shaders,
  textures, models, materials, animations.
- **Smart Import** — multi-strategy texture discovery, drag-and-drop with eager import,
  texture remap dialog.
- **Texture Compression** — import-time BCn: BC7 for color, BC5 for normals, uploaded on
  the transfer queue.
- **Model Import** — full DCC hierarchy, cameras, and lights, not just meshes; render-mode
  and cull flags from the source.
- **Hot Reload** — FileWatcher-driven live reload for shaders, textures, and project files.
- **Scene Format** — JSON `.luth` with dirty tracking and native file dialogs.

## Editor

- **Scene Interaction** — mouse picking, selection outlines, multi-select, shade modes
  (Lit / Wireframe / Unlit and debug views).
- **Inspector** — material editor, animation controls, light and shadow settings,
  per-component copy/paste, Add Component.
- **Live Preview** — orbit-camera 3D preview for material and model assets.
- **Play Mode** — Editing / Playing / Paused state machine, JSON scene snapshot,
  transport bar.
- **Game Panel** — camera-driven runtime view with letterbox, no overlays.
- **Project Panel** — folder navigation, search, rendered thumbnails, context menus.
- **Undo / Redo** — command pattern with UUID entity resolution, gizmo-drag coalescing,
  compound commands.
- **Frame Debugger** — freeze a frame, scrub through every draw, replay any single one.
- **Profiler** — scheduler, memory, and per-pass GPU dashboard with fiber-aware
  instrumentation.
- **GPU Crash Debugging** — Nsight Aftermath device-lost dumps plus tiered Vulkan
  validation.
- **Console** — per-subsystem log channels with level filter and search.
- **Persistence** — named workspaces: window layouts and panel state saved across sessions.
