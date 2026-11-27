# Vendored dependencies — upstream and pinning info

RavenEngine is hermetic by construction: everything needed to configure and build the
engine from a fresh clone is in this tree (except the two large submodules below and the
system Vulkan loader). See `manifest.json` for machine-readable data (consumed by
`scripts/generate_sbom.py`) and this file for humans.

## Vendored copies (pinned commits, refreshed manually)

| Directory | Upstream | Version / commit | License | Used by |
|-----------|----------|------------------|---------|---------|
| `Vulkan-Headers/` | https://github.com/KhronosGroup/Vulkan-Headers | vulkan-sdk headers 290 (`include/`) | Apache-2.0 | renderer core + ImGui backends |
| `glfw/` | https://github.com/glfw/glfw | 3.4 | Zlib | editor / sandbox windowing |
| `glm/` | https://github.com/g-truc/glm | 1.0.1 | MIT | all linear algebra |
| `imgui/` | https://github.com/ocornut/imgui | 1.93.1-docking | MIT | editor UI |
| `imguizmo/` | https://github.com/CedricGuillemet/ImGuizmo | vendored snapshot | MIT | editor transform gizmos |
| `json/` | https://github.com/nlohmann/json | 3.11.3 | MIT | scene/asset/project JSON |
| `lua/` | https://www.lua.org/ | 5.4.9 | MIT | embedded scripting |
| `miniaudio/` | https://github.com/mackron/miniaudio | 0.11.26 | Unlicense/MIT-0 | audio |
| `stb/` | https://github.com/nothings/stb | current | MIT/PD | image load/write |
| `tinyobjloader/` | https://github.com/tinyobjloader/tinyobjloader | 2.0.0 | MIT | OBJ mesh import |
| `vma/` | https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator | 3.5.0 `0f1a757` | MIT | `raven_vma` (single impl TU) |
| `entt/` | https://github.com/skypjack/entt | 3.16.0 `b4e58bd` | MIT | `raven::entt` (header-only) |
| `spdlog/` | https://github.com/gabime/spdlog | 1.17.0 `79524dd` | MIT | `raven::spdlog` (header-only) — backend of `raven::Logger` |
| `spirv-cross/` | https://github.com/KhronosGroup/SPIRV-Cross | vulkan-sdk-1.4.363.0 `f11ba9f` | Apache-2.0 | `raven_spirv_cross` — **reflection subset only** (SPIRV-Cross core + `spirv_glsl` for `spirv_reflect`; no HLSL/MSL backends) |
| `jolt/` | https://github.com/jrouwe/JoltPhysics | 5.7.0 `cf16769` | MIT | `Jolt::Jolt` when built with `-DRAVEN_BUILD_JOLT=ON` |
| `bc7enc_rdo/` | https://github.com/richgel999/bc7enc_rdo | master `b943862` | MIT/PD | `raven_bc7enc` — BCn compression (ISPC path excluded); bundles `lodepng` (PNG) + `miniz` (zlib) |
| `tracy/` | https://github.com/wolfpld/tracy | 0.15.0 `f2a4f25` | BSD-3-Clause | `raven::tracy` (client only) — zones active with `-DRAVEN_ENABLE_TRACY=ON` |
| `phosphor/` | https://github.com/phosphor-icons/core | core v2.0.8 `d42782b` | MIT | editor icon font `Phosphor.ttf` (+ LICENSE); codepoints from `@phosphor-icons/web` 2.1.2 `selection.json` |

## Git submodules (pinned by gitlink SHA — fetch with `git submodule update --init`)

| Path | Upstream | Commit | License | Fetch |
|------|----------|--------|---------|-------|
| `slang/` | https://github.com/shader-slang/slang | `c7954ebd` (vulkan-sdk-1.4.363.0) | Apache-2.0 | `git submodule update --init third_party/slang` |
| `assimp/` | https://github.com/assimp/assimp | `a79ff6b` (v6.0.6) | BSD-3-Clause | `git submodule update --init third_party/assimp` |

These two are too large to vendor (~110 MB / ~380 MB). Slang is the planned shading-language
compiler (replacing direct GLSL authoring); assimp is the DCC-format importer (supersedes
tinyobjloader for FBX/glTF/USD). Both configure only when their CMake option is enabled:

```
cmake -S . -B build -DRAVEN_USE_SLANG=ON -DRAVEN_USE_ASSIMP=ON
```

## Not vendored / not committed

- **Vulkan SDK / loader** — system dependency, installed per platform (see docs/BUILDING.md).
  Only the headers are vendored (`Vulkan-Headers/`). CI installs the SDK on Windows/macOS
  and `mesa-vulkan-drivers` on Linux.
- **Nsight Aftermath** — proprietary NVIDIA SDK (license does not grant redistribution).
  To enable GPU crash dumps: download from developer.nvidia.com, unpack anywhere, and
  configure `-DNsightAftermath_ROOT=<path>` (reserved; integration pending). The engine
  degrades gracefully without it.
- **SwiftShader** — CPU Vulkan implementation used only for headless CI captures. Not a
  build dependency; see docs/BUILDING.md § headless.

## Updating vendored copies

1. Clone upstream at the desired tag/commit into a scratch dir.
2. Replace the directory contents above (excluding `.git`), keeping only what RavenEngine
   consumes — prune tests/samples/docs unless noted.
3. Update this table + `manifest.json` (version, pinnedCommit, license) and regenerate the
   SBOM: `python3 scripts/generate_sbom.py --output sbom.cdx.json`.
4. Rebuild and run the test suite before committing.
