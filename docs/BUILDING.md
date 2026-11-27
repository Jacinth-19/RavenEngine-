# Building RavenEngine

RavenEngine is written in C++20 and builds with CMake ≥ 3.24. Dependencies are vendored under
`third_party/` (pinned — see `third_party/manifest.json`), so **no network fetch is required at
configure time**. The only external requirements are a C++20 compiler, CMake, Python 3 (for the
SBOM/asset scripts), and — optionally — a system Vulkan loader and windowing libraries.

## Requirements

| Component          | Linux                       | Windows                 | macOS                       |
| ------------------ | --------------------------- | ----------------------- | --------------------------- |
| Compiler           | g++ ≥ 11 / clang ≥ 14       | MSVC 2022 (v143)        | AppleClang / clang ≥ 14     |
| CMake              | ≥ 3.24                      | ≥ 3.24                  | ≥ 3.24                      |
| Python             | 3.8+                        | 3.8+                    | 3.8+                        |
| Vulkan loader      | `libvulkan-dev`             | Vulkan SDK (LunarG)     | `molten-vk` + `vulkan-loader` (brew) |
| X11/Wayland dev    | `libx11-dev` + friends      | (SDK provides)          | (n/a)                       |

## Degradable builds

The CMake scripts are designed to **degrade gracefully** when optional system pieces are missing:

- **No system Vulkan** → `raven_render` and the GUI apps are skipped. `raven_core` and the
  `raven_tests` suite still build and run.
- **No X11/Wayland dev headers (Linux)** → the editor/sandbox GUI apps are skipped by default;
  configure with `-DRAVEN_GUI_HEADLESS=ON` to build the vendored GLFW with the **null windowing
  platform** instead. `ravened` and `raven-sandbox` then run in an offscreen headless mode
  (scene + UI rendered to the offscreen target; `ravened --headless` also writes a PNG capture).
- **No Vulkan on the runner** → the GPU smoke test self-skips, unless `RAVEN_REQUIRE_GPU=1` is
  set, in which case device-creation failure is a hard test failure (used on CI).

## Linux (Debian/Ubuntu)

```sh
sudo apt-get update
sudo apt-get install -y cmake ninja-build g++ \
    libvulkan-dev mesa-vulkan-drivers vulkan-tools \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev xorgproto \
    python3

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DRAVEN_BUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

### Headless editor / sandbox smoke runs

Without a display (CI, null-platform GLFW), both apps fall back to an offscreen path:

```sh
# Editor: render N frames of the sample project (scene + UI) and write a PNG
./build/bin/ravened --headless --frames 3 --capture editor_capture.png

# Sandbox: render N frames offscreen and print average CPU frame time
./build/bin/raven-sandbox --offscreen --frames 120 --tier medium
```

`ravened --help` lists all flags. In windowed mode, `ravened` opens the docking editor
(Hierarchy / Inspector / Viewport / Assets / Profiler / Console); the Viewport panel
previews the offscreen scene render, with an ImGuizmo translate/rotate/scale gizmo,
entity picking, and save/load of the sample scene.

### Headless / CPU-only rendering (Lavapipe)

CI uses **Lavapipe** (Mesa's software Vulkan driver) to exercise the real Vulkan path without a
GPU. After `apt-get install mesa-vulkan-drivers`, `vulkaninfo --summary` should list a CPU
device (e.g. "llvmpipe"). The offscreen smoke test then renders the sample scene and checks the
resulting pixels; set `RAVEN_REQUIRE_GPU=1` to make any device/render failure fatal:

```sh
RAVEN_REQUIRE_GPU=1 ./build/bin/raven_tests
```

Note: the smoke test renders **offscreen** — no window or display server is required.

## CPU Vulkan via SwiftShader (headless, no GPU, no apt)

Verified locally: the full `raven_tests` suite (38/38, incl. the offscreen render smoke
test) runs on **[SwiftShader](https://github.com/google/swiftshader)** — Google's CPU
Vulkan implementation — with **no system Vulkan, no X11, no apt access**. This is also a
reasonable smoke path for constrained CI machines.

Build (≈10–20 min on 2 cores):

```sh
# 1) Vulkan loader (tiny)
git clone --depth 1 https://github.com/KhronosGroup/Vulkan-Loader.git
git clone --depth 1 --branch v1.4.369 https://github.com/KhronosGroup/Vulkan-Headers.git
cmake -S Vulkan-Headers -B /tmp/vh-build -DCMAKE_BUILD_TYPE=Release
cmake --install /tmp/vh-build --prefix /tmp/vh-prefix
pkg-config --version   # needs a pkg-config stub if your pool lacks XCB; disable WSI instead:
cmake -S Vulkan-Loader -B /tmp/loader-build -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTS=OFF -DBUILD_WSI_XCB_SUPPORT=OFF -DBUILD_WSI_XLIB_SUPPORT=OFF \
    -DBUILD_WSI_WAYLAND_SUPPORT=OFF -DVulkanHeaders_DIR=/tmp/vh-prefix/share/cmake/VulkanHeaders
cmake --build /tmp/loader-build -j

# 2) SwiftShader (Subzero backend is much lighter than LLVM)
git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/google/swiftshader.git
cmake -S swiftshader -B /tmp/ss-build -DCMAKE_BUILD_TYPE=Release \
    -DREACTOR_BACKEND=Subzero -DSWIFTSHADER_BUILD_TESTS=OFF \
    -DSWIFTSHADER_BUILD_WSI_XCB=OFF -DSWIFTSHADER_BUILD_WSI_WAYLAND=OFF \
    -DSWIFTSHADER_WARNINGS_AS_ERRORS=OFF
cmake --build /tmp/ss-build --target vk_swiftshader -j

# 3) Build RavenEngine against the loader + vendored headers, then run on SwiftShader
cmake -S . -B build-vk -DCMAKE_BUILD_TYPE=Release -DRAVEN_BUILD_TESTS=ON \
    -DVulkan_INCLUDE_DIR=$PWD/third_party/Vulkan-Headers/include \
    -DVulkan_LIBRARY=/tmp/loader-build/loader/libvulkan.so
cmake --build build-vk -j
LD_LIBRARY_PATH=/tmp/ss-build:/tmp/ss-build/Linux \
VK_ICD_FILENAMES=/tmp/ss-build/Linux/vk_swiftshader_icd.json \
RAVEN_REQUIRE_GPU=1 ./build-vk/bin/raven_tests
```

Expected: `Renderer: initialized on 'SwiftShader Device (Subzero)' (CPU device: yes)` and
**38 passed, 0 failed** — the offscreen smoke test renders the sample scene to a real
pixel buffer on the CPU. CI uses Mesa Lavapipe the same way; the SBOM and the smoke
render are uploaded as workflow artifacts.

Note for artifact re-generation: the committed `docs/media/*.png|gif` were captured with
exactly this setup (orbit frames assembled into `sample-orbit.gif`).

## Windows (MSVC)

1. Install [Vulkan SDK](https://vulkan.lunarg.com/) (Chocolatey: `choco install vulkan-sdk -y`).
2. Configure with the SDK paths (CMake's `FindVulkan` picks up `VULKAN_SDK` automatically):

```powershell
cmake -S . -B build -A x64 -DRAVEN_BUILD_TESTS=ON
cmake --build build --config Release -j
ctest --test-dir build -C Release --output-on-failure
```

If CMake cannot locate Vulkan, pass it explicitly:

```powershell
cmake -S . -B build -A x64 -DVulkan_INCLUDE_DIR="$env:VULKAN_SDK/Include" -DVulkan_LIBRARY="$env:VULKAN_SDK/Lib/vulkan-1.lib"
```

The GPU smoke test self-skips on runners without a usable Vulkan device; the remaining suites
always run.

## macOS (MoltenVK)

```sh
brew install cmake ninja vulkan-loader molten-vk
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DRAVEN_BUILD_TESTS=ON
cmake --build build -j"$(sysctl -n hw.ncpu)"
ctest --test-dir build --output-on-failure
```

MoltenVK is a **portability** driver: Vulkan instance creation needs the
`VK_KHR_portability_enumeration` extension and the `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`
flag, plus the `VK_KHR_portability_subset` device extension. RavenEngine enables these
automatically when requested *and supported by the loader* — but you must still opt in via
`RendererConfig::enablePortabilityEnumeration = true` (already set for Apple platforms in the
smoke test). macOS runners without a Metal device available will skip the GPU smoke test.

## Useful CMake options

| Option                    | Default | Meaning                                    |
| ------------------------- | ------- | ------------------------------------------ |
| `RAVEN_BUILD_TESTS`       | ON      | Build `raven_tests`                        |
| `RAVEN_BUILD_EDITOR`      | ON      | Build the editor (skipped without GUI deps)|
| `RAVEN_BUILD_SANDBOX`     | ON      | Build the sandbox (skipped without GUI deps)|
| `RAVEN_ENABLE_ASAN`       | OFF     | AddressSanitizer for engine targets        |
| `RAVEN_FORCE_VENDORED_GLFW` | OFF   | Ignore system GLFW, build vendored 3.4     |
| `RAVEN_GUI_HEADLESS`      | OFF     | No X11/Wayland: build null-platform GLFW so the GUI apps run headless (offscreen) |
| `RAVEN_ENABLE_TRACY`      | OFF     | Build/link the Tracy profiler client (`TRACY_ENABLE` zones) |
| `RAVEN_BUILD_JOLT`        | OFF     | Build vendored Jolt Physics (`Jolt::Jolt`) |
| `RAVEN_USE_SLANG`         | OFF     | Configure Slang from the `third_party/slang` submodule |
| `RAVEN_USE_ASSIMP`        | OFF     | Configure assimp from the `third_party/assimp` submodule |

## Extended dependency set (2026-11)

Newly wired in this cycle (all pinned — see `third_party/VERSIONS.md` and `third_party/manifest.json`):

- **VMA** (`raven_vma`) — Vulkan Memory Allocator; `engine/src/third_party/vma_impl.cpp` is the
  single implementation TU. Linked into `raven_render`.
- **EnTT** (`raven::entt`) — header-only ECS, ready for renderer-side adoption.
- **spdlog** (`raven::spdlog`, header-only) — logging backend. `raven::Logger` keeps its API
  and line format; RAVEN_LOG_* macros are unchanged. The previous hand-rolled logger is gone.
- **SPIRV-Cross** (`raven_spirv_cross`) — reflection subset for the future Slang-reflection
  pipeline (exact set/binding extraction).
- **Jolt Physics** (`RAVEN_BUILD_JOLT=ON`) — full Jolt source vendored (samples/tests omitted).
- **bc7enc_rdo** (`raven_bc7enc`) — BCn texture compression at import time; bundles `lodepng`
  (PNG load/save) and `miniz` (zlib) for its utils.
- **Tracy** (`RAVEN_ENABLE_TRACY=ON`) — client only; `ZoneScopedN("Renderer::RenderFrame")`
  is active in `Renderer::RenderFrame`. Connect the Tracy GUI to a ravened/raven-sandbox run
  to profile. Off by default: macros compile to no-ops.
- **Phosphor Icons** (`raven::phosphor`) — icon font merged into the editor font at startup;
  glyph constants in `editor/phosphor_icons.h`. Used by the viewport toolbar, hierarchy,
  asset list, menu bar and about dialog.
- **Slang** / **assimp** — git submodules (too large to vendor), off by default.
  `git submodule update --init third_party/slang third_party/assimp` then
  `-DRAVEN_USE_SLANG=ON -DRAVEN_USE_ASSIMP=ON`.

### Not redistributable: Nsight Aftermath

Nsight Aftermath (GPU crash dumps on device-lost) is a **proprietary NVIDIA SDK** and is not
committed to this repository. To enable it: download the SDK from developer.nvidia.com,
unpack it anywhere, and point your build at it (`-DNsightAftermath_ROOT=<path>`; reserved
for the Aftermath integration — not yet wired into the Vulkan context). Without it the
engine runs normally; crash dumps are simply unavailable.

### Vulkan SDK

Vulkan is a **system dependency**: headers are vendored (`third_party/Vulkan-Headers`),
the loader comes from your platform (see the platform sections above). The Vulkan SDK from
LunarG also bundles tooling we use but do not redistribute (glslang, SPIRV-Tools, validation
layers); install it if you need the toolchain, but it is not required to build.


## Maintenance

- **SBOM**: `python3 scripts/generate_sbom.py --output sbom.cdx.json` (CycloneDX 1.5, offline).
- **Sample asset regeneration**: `python3 scripts/generate_sample_assets.py` (deterministic;
  regenerates `sample/` and its `.meta` UUID sidecars).
- **Dependency bumps**: edit `third_party/manifest.json`, replace the vendored sources, then
  re-run the SBOM generator. Third-party code is not package-manager managed; Dependabot is
  configured only for GitHub Actions.
