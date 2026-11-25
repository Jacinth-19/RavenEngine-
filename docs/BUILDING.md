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
- **No X11/Wayland dev headers (Linux)** → the editor/sandbox GUI apps are skipped.
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

### Headless / CPU-only rendering (Lavapipe)

CI uses **Lavapipe** (Mesa's software Vulkan driver) to exercise the real Vulkan path without a
GPU. After `apt-get install mesa-vulkan-drivers`, `vulkaninfo --summary` should list a CPU
device (e.g. "llvmpipe"). The offscreen smoke test then renders the sample scene and checks the
resulting pixels; set `RAVEN_REQUIRE_GPU=1` to make any device/render failure fatal:

```sh
RAVEN_REQUIRE_GPU=1 ./build/bin/raven_tests
```

Note: the smoke test renders **offscreen** — no window or display server is required.

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

## Maintenance

- **SBOM**: `python3 scripts/generate_sbom.py --output sbom.cdx.json` (CycloneDX 1.5, offline).
- **Sample asset regeneration**: `python3 scripts/generate_sample_assets.py` (deterministic;
  regenerates `sample/` and its `.meta` UUID sidecars).
- **Dependency bumps**: edit `third_party/manifest.json`, replace the vendored sources, then
  re-run the SBOM generator. Third-party code is not package-manager managed; Dependabot is
  configured only for GitHub Actions.
