# RavenEngine

RavenEngine is a **low-resource-focused C++20 Vulkan game engine and editor**. It targets
modest hardware: no bloated abstractions, deterministic engine services, and a renderer that
keeps frame budgets tight on integrated GPUs.

## Highlights

- **C++20 + Vulkan 1.3** — dynamic rendering, synchronization2, push descriptors; no legacy paths.
- **Render graph** — passes declare reads/writes; a compiler schedules them, inserts barriers,
  culls dead passes, and rejects resource forward references at validation time.
- **ECS** — sparse-set world with stable entity handles, views, and hierarchy support.
- **Asset pipeline** — UUID-addressed assets with `.meta` sidecar files, import settings
  (sRGB toggles, mipmaps), and a deterministic asset database (rescan → stable UUIDs).
- **Scene format** — JSON scenes with components (transform, mesh, camera, light, …), loaded
  deterministically.
- **Scripting** — embedded Lua 5.4 with engine bindings.
- **Audio** — miniaudio-backed mixer with 3D spatialization.
- **Physics** — lightweight rigid-body/collision core designed for small worlds.
- **Editor hooks** — ImGui (docking) + ImGuizmo vendored and ready for the editor UI.

## Repository layout

| Path          | Contents                                                            |
| ------------- | ------------------------------------------------------------------- |
| `engine/`     | The engine library (`raven_core`) and the Vulkan renderer (`raven_render`) |
| `editor/`     | Editor application stub (`RavenEditor`) — built when GUI deps exist  |
| `sandbox/`    | Minimal runnable app (`raven-sandbox`) — built when GUI deps exist   |
| `sample/`     | Sample game project (meshes, textures, scenes, materials)            |
| `assets/`     | Engine-provided assets (GLSL sources + compiled `.spv` shaders)      |
| `tests/`      | Self-contained test framework + suites (`raven_tests`)               |
| `third_party/`| Vendored, version-pinned dependencies (see `third_party/manifest.json`) |
| `scripts/`    | Asset generator + CycloneDX SBOM generator                           |

## Building

See **[docs/BUILDING.md](docs/BUILDING.md)** for Linux (Debian/Ubuntu incl. headless CPU-Vulkan
via Lavapipe), Windows (Vulkan SDK), and macOS (MoltenVK) instructions. Short version:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DRAVEN_BUILD_TESTS=ON
cmake --build build -j"$(nproc)"
./build/bin/raven_tests
```

The engine configures and builds **without** system Vulkan or windowing libraries —
`raven_render` and the GUI apps are skipped, and the core test suite still runs.

## CI & supply chain

GitHub Actions (`.github/workflows/ci.yml`) builds on Linux/macOS/Windows. The Linux job runs
the full suite on **Lavapipe** (CPU Vulkan) including an offscreen render smoke test, and uploads
a **CycloneDX 1.5 SBOM** (from `third_party/manifest.json`) on every run. See
[docs/SECURITY.md](docs/SECURITY.md) for vulnerability reporting and the audit procedure.

## Current status

- Tests: **30/30 passing** (`raven_tests`).
- Renderer gating verified: core suite builds and passes without a system Vulkan loader;
  `raven_render` + offscreen smoke test compile/run when Vulkan is available (CI).
- Not yet implemented: the full `RavenEditor` GUI (editor/ directory is a build stub), and
  `raven-sandbox` runtime. Engine services (ECS, scenes, assets, renderer, camera, …) are
  implemented, tested, and wired through CMake.

## License

MIT — see [LICENSE](LICENSE).
