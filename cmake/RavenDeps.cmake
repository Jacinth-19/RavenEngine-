# RavenEngine — third-party dependency wiring.
# Everything is vendored under third_party/ at fixed commits (see third_party/manifest.json
# and third_party/VERSIONS.md). No network access is required to configure the project.
# Vulkan is linked from the system (SDK or loader packages); CI installs it per-platform.

# --- GLM (interface, header-only) ---------------------------------------------------
add_library(raven_glm INTERFACE)
target_include_directories(raven_glm INTERFACE ${CMAKE_SOURCE_DIR}/third_party/glm)
target_compile_definitions(raven_glm INTERFACE GLM_FORCE_RADIANS GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_SILENT_WARNINGS)

# --- nlohmann/json (interface, header-only single include) ---------------------------
add_library(raven_json INTERFACE)
target_include_directories(raven_json INTERFACE ${CMAKE_SOURCE_DIR}/third_party/json)

# --- stb (static, image load/write) --------------------------------------------------
add_library(raven_stb STATIC ${CMAKE_SOURCE_DIR}/engine/src/third_party/stb_impl.cpp)
target_include_directories(raven_stb PUBLIC ${CMAKE_SOURCE_DIR}/third_party/stb)
if(MSVC)
    target_compile_options(raven_stb PRIVATE /w)
else()
    target_compile_options(raven_stb PRIVATE -w)
endif()

# --- Lua 5.4 (static, core + libs, interpreter main excluded) -------------------------
file(GLOB RAVEN_LUA_SOURCES ${CMAKE_SOURCE_DIR}/third_party/lua/*.c)
list(REMOVE_ITEM RAVEN_LUA_SOURCES ${CMAKE_SOURCE_DIR}/third_party/lua/lua.c ${CMAKE_SOURCE_DIR}/third_party/lua/onelua.c)
add_library(raven_lua STATIC ${RAVEN_LUA_SOURCES})
target_include_directories(raven_lua PUBLIC ${CMAKE_SOURCE_DIR}/third_party/lua)
if(MSVC)
    target_compile_options(raven_lua PRIVATE /w)
else()
    target_compile_options(raven_lua PRIVATE -w)
endif()

# --- miniaudio (static impl, single header) -------------------------------------------
add_library(raven_miniaudio STATIC ${CMAKE_SOURCE_DIR}/engine/src/third_party/miniaudio_impl.cpp)
target_include_directories(raven_miniaudio PUBLIC ${CMAKE_SOURCE_DIR}/third_party/miniaudio)
if(WIN32)
    target_link_libraries(raven_miniaudio PUBLIC ole32 winmm avrt)
    #Compile-time linking needs headers for every enabled backend; we only have WASAPI/DSound/WinMM,
    #so disable the JACK backend entirely (we don't use it).
    target_compile_definitions(raven_miniaudio PRIVATE MA_NO_RUNTIME_LINKING MA_NO_JACK)
elseif(APPLE)
    target_link_libraries(raven_miniaudio PUBLIC "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreFoundation")
    # Apple is MA_UNIX: without these, miniaudio enables ALSA/PulseAudio/JACK compile-time includes.
    target_compile_definitions(raven_miniaudio PRIVATE MA_NO_RUNTIME_LINKING MA_NO_JACK MA_NO_ALSA MA_NO_PULSEAUDIO)
else()
    target_link_libraries(raven_miniaudio PUBLIC dl m pthread)
endif()
if(MSVC)
    target_compile_options(raven_miniaudio PRIVATE /w)
else()
    target_compile_options(raven_miniaudio PRIVATE -w)
endif()

# --- tinyobjloader (static, OBJ import) -------------------------------------------------
add_library(raven_tinyobj STATIC ${CMAKE_SOURCE_DIR}/third_party/tinyobjloader/tiny_obj_loader.cc)
target_include_directories(raven_tinyobj PUBLIC ${CMAKE_SOURCE_DIR}/third_party/tinyobjloader)
if(MSVC)
    target_compile_options(raven_tinyobj PRIVATE /w)
else()
    target_compile_options(raven_tinyobj PRIVATE -w)
endif()

# --- Vulkan-Headers (interface; also used by editor for ImGui backends) -----------------
add_library(raven_vulkan_headers INTERFACE)
target_include_directories(raven_vulkan_headers INTERFACE ${CMAKE_SOURCE_DIR}/third_party/Vulkan-Headers/include)

# --- Vulkan loader (system). Sets RAVEN_VULKAN_FOUND. -----------------------------------
# Install: Linux  -> libvulkan-dev (headers+loader) or Vulkan SDK
#          Windows-> Vulkan SDK (vulkan-1.lib)
#          macOS  -> brew install molten-vk vulkan-loader (plus MoltenVK at runtime)
# See docs/BUILDING.md. When absent, the renderer and GUI apps are skipped.
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    message(STATUS "RavenEngine: found Vulkan ${Vulkan_VERSION} (${Vulkan_INCLUDE_DIR})")
    set(RAVEN_VULKAN_FOUND TRUE)
    if(NOT TARGET raven::vulkan)
        add_library(raven_vulkan INTERFACE)
        target_link_libraries(raven_vulkan INTERFACE Vulkan::Vulkan)
        target_include_directories(raven_vulkan INTERFACE ${CMAKE_SOURCE_DIR}/third_party/Vulkan-Headers/include)
        add_library(raven::vulkan ALIAS raven_vulkan)
    endif()
else()
    message(STATUS "RavenEngine: system Vulkan NOT found — raven_render and GUI apps will be skipped. See docs/BUILDING.md.")
    set(RAVEN_VULKAN_FOUND FALSE)
endif()

option(RAVEN_GUI_HEADLESS "Build the GUI apps against the null-platform GLFW (no X11/Wayland); apps run in an offscreen headless fallback mode" OFF)

# --- GLFW (editor/sandbox windowing) -----------------------------------------------------
function(raven_find_or_build_glfw)
    find_package(glfw3 3.3 QUIET)
    if(glfw3_FOUND AND NOT RAVEN_FORCE_VENDORED_GLFW)
        message(STATUS "RavenEngine: using system GLFW")
        set(RAVEN_GLFW_FOUND TRUE PARENT_SCOPE)
        set(RAVEN_GLFW_TARGET_NAME glfw PARENT_SCOPE)
        return()
    endif()

    # On Linux GLFW needs X11 or Wayland development headers. Detect them; without
    # them either skip the GUI apps (default) or, with RAVEN_GUI_HEADLESS=ON, build
    # the vendored GLFW with the null windowing platform so the editor/sandbox run
    # in an offscreen headless mode instead.
    if(UNIX AND NOT APPLE)
        find_package(X11 QUIET)
        if(NOT X11_FOUND)
            if(RAVEN_GUI_HEADLESS)
                message(STATUS "RavenEngine: X11 dev headers not found — building null-platform GLFW (RAVEN_GUI_HEADLESS=ON); editor/sandbox will run headless")
                set(GLFW_BUILD_X11 OFF CACHE BOOL "" FORCE)
                set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
                set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
                set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
                set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
                set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
                add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/glfw EXCLUDE_FROM_ALL)
                set(RAVEN_GLFW_FOUND TRUE PARENT_SCOPE)
                set(RAVEN_GLFW_TARGET_NAME glfw PARENT_SCOPE)
                set(RAVEN_GUI_HEADLESS_RUNTIME TRUE PARENT_SCOPE)
                return()
            endif()
            message(STATUS "RavenEngine: X11 dev headers not found — editor/sandbox GUI will be skipped (or configure with -DRAVEN_GUI_HEADLESS=ON for headless null-platform GLFW). Install libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev (see docs/BUILDING.md)")
            set(RAVEN_GLFW_FOUND FALSE PARENT_SCOPE)
            return()
        endif()
    endif()

    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/glfw EXCLUDE_FROM_ALL)
    message(STATUS "RavenEngine: using vendored GLFW")
    set(RAVEN_GLFW_FOUND TRUE PARENT_SCOPE)
    set(RAVEN_GLFW_TARGET_NAME glfw PARENT_SCOPE)
endfunction()

# -----------------------------------------------------------------------------
# Extended dependency set (2026-11) — see third_party/manifest.json and
# third_party/VERSIONS.md for upstream URLs, pins and licenses.
# Everything below is either vendored under third_party/ or, for the two
# dependencies too large to vendor (Slang, assimp), a pinned git submodule that
# configures only when explicitly enabled. Nsight Aftermath is a proprietary
# NVIDIA SDK and is therefore NOT committed to the repository — see docs/BUILDING.md.
# -----------------------------------------------------------------------------
find_package(Threads REQUIRED)

# --- Vulkan Memory Allocator (static impl; header vendored) ---------------------------
add_library(raven_vma STATIC ${CMAKE_SOURCE_DIR}/engine/src/third_party/vma_impl.cpp)
add_library(raven::vma ALIAS raven_vma)
target_include_directories(raven_vma PUBLIC ${CMAKE_SOURCE_DIR}/third_party/vma)
target_include_directories(raven_vma PRIVATE ${CMAKE_SOURCE_DIR}/third_party/Vulkan-Headers/include)
target_link_libraries(raven_vma PUBLIC raven_vulkan_headers)

# --- EnTT (interface, header-only ECS) --------------------------------------------------
add_library(raven_entt INTERFACE)
add_library(raven::entt ALIAS raven_entt)
target_include_directories(raven_entt INTERFACE ${CMAKE_SOURCE_DIR}/third_party/entt/src)

# --- spdlog (header-only mode; formatting, sinks, coloring, thread safety) ---------------
add_library(raven_spdlog INTERFACE)
add_library(raven::spdlog ALIAS raven_spdlog)
target_include_directories(raven_spdlog INTERFACE ${CMAKE_SOURCE_DIR}/third_party/spdlog/include)
target_link_libraries(raven_spdlog INTERFACE Threads::Threads)

# --- SPIRV-Cross (static; shader reflection subset — no cross-compilation backends) -----
add_library(raven_spirv_cross STATIC
    ${CMAKE_SOURCE_DIR}/third_party/spirv-cross/spirv_cross.cpp
    ${CMAKE_SOURCE_DIR}/third_party/spirv-cross/spirv_parser.cpp
    ${CMAKE_SOURCE_DIR}/third_party/spirv-cross/spirv_cfg.cpp
    ${CMAKE_SOURCE_DIR}/third_party/spirv-cross/spirv_glsl.cpp
    ${CMAKE_SOURCE_DIR}/third_party/spirv-cross/spirv_cross_parsed_ir.cpp
    ${CMAKE_SOURCE_DIR}/third_party/spirv-cross/spirv_cross_util.cpp
    ${CMAKE_SOURCE_DIR}/third_party/spirv-cross/spirv_reflect.cpp)
add_library(raven::spirv_cross ALIAS raven_spirv_cross)
target_include_directories(raven_spirv_cross PUBLIC ${CMAKE_SOURCE_DIR}/third_party/spirv-cross)
target_include_directories(raven_spirv_cross PRIVATE ${CMAKE_SOURCE_DIR}/third_party/Vulkan-Headers/include)
set_target_properties(raven_spirv_cross PROPERTIES CXX_STANDARD 17)
if(MSVC)
    target_compile_options(raven_spirv_cross PRIVATE /w)
else()
    target_compile_options(raven_spirv_cross PRIVATE -w)
endif()

# --- bc7enc_rdo (static; BC7/BC5 texture compression, plain-C++ path, no ISPC) -----------
add_library(raven_bc7enc STATIC
    ${CMAKE_SOURCE_DIR}/third_party/bc7enc_rdo/bc7enc.cpp
    ${CMAKE_SOURCE_DIR}/third_party/bc7enc_rdo/bc7decomp.cpp
    ${CMAKE_SOURCE_DIR}/third_party/bc7enc_rdo/rgbcx.cpp
    ${CMAKE_SOURCE_DIR}/third_party/bc7enc_rdo/utils.cpp
    ${CMAKE_SOURCE_DIR}/third_party/bc7enc_rdo/lodepng.cpp)
# NOTE: ert.cpp + rdo_bc_encoder.cpp (the BC7 rate-distortion optimization pipeline) are
# intentionally NOT compiled. ert.cpp failed to compile on the Ubuntu 24.04 CI runner with
# "'const struct ert::color_rgba' has no member named 'm_c'" (bc7enc_rdo b943862) while the
# same sources build cleanly on GCC 12/13 locally — the cause is not yet understood and the
# TU is out of scope for import-time BCn. The files remain vendored untouched upstream code.
# Import-time BCn (docs/EDITOR_DESIGN.md Phase 3, engine/src/assets/BcnCodec.cpp) uses
# the plain encoder/decoder path only (rgbcx + bc7enc); the RDO pipeline stays excluded.
add_library(raven::bc7enc ALIAS raven_bc7enc)
target_include_directories(raven_bc7enc PUBLIC ${CMAKE_SOURCE_DIR}/third_party/bc7enc_rdo)
target_compile_definitions(raven_bc7enc PRIVATE SUPPORT_BC7E=0)
if(MSVC)
    target_compile_options(raven_bc7enc PRIVATE /w)
else()
    target_compile_options(raven_bc7enc PRIVATE -w)
endif()

# --- Tracy (frame profiler client; macros are no-ops unless RAVEN_ENABLE_TRACY=ON) -------
option(RAVEN_ENABLE_TRACY "Build and link the Tracy profiler client (enables TRACY_ZONE/ZoneScoped zones)" OFF)
if(RAVEN_ENABLE_TRACY)
    add_library(raven_tracy STATIC ${CMAKE_SOURCE_DIR}/third_party/tracy/public/TracyClient.cpp)
    add_library(raven::tracy ALIAS raven_tracy)
    target_include_directories(raven_tracy PUBLIC ${CMAKE_SOURCE_DIR}/third_party/tracy/public)
    target_compile_definitions(raven_tracy PUBLIC TRACY_ENABLE)
    target_link_libraries(raven_tracy PUBLIC dl Threads::Threads)
    if(MSVC)
        target_compile_options(raven_tracy PRIVATE /w)
    else()
        target_compile_options(raven_tracy PRIVATE -w)
    endif()
else()
    add_library(raven_tracy INTERFACE)
    add_library(raven::tracy ALIAS raven_tracy)
    target_include_directories(raven_tracy INTERFACE ${CMAKE_SOURCE_DIR}/third_party/tracy/public)
endif()

# --- Jolt Physics (static; full engine source vendored, built on demand) ------------------
option(RAVEN_BUILD_JOLT "Build the vendored Jolt Physics library" OFF)
if(RAVEN_BUILD_JOLT)
    if(NOT EXISTS ${CMAKE_SOURCE_DIR}/third_party/jolt/Build/CMakeLists.txt)
        message(FATAL_ERROR "RAVEN_BUILD_JOLT=ON but third_party/jolt is incomplete — restore it from the repository.")
    endif()
    set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
    set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
    set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
    set(USE_ASSERTS OFF CACHE BOOL "" FORCE)
    # Build/CMakeLists.txt sets PHYSICS_REPO_ROOT itself; target name is "Jolt" (Jolt::Jolt).
    add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/jolt/Build third_party-jolt-build)
else()
    message(STATUS "RavenEngine: Jolt Physics vendored but not built (RAVEN_BUILD_JOLT=ON to enable)")
endif()

# --- Phosphor Icons (editor icon font; runtime-loaded TTF, no compilation) ----------------
add_library(raven_phosphor INTERFACE)
add_library(raven::phosphor ALIAS raven_phosphor)
target_compile_definitions(raven_phosphor INTERFACE RAVEN_PHOSPHOR_FONT_DIR="${CMAKE_SOURCE_DIR}/third_party/phosphor/fonts")

# --- Slang (shader-slang/slang) — pinned git submodule, too large to vendor ----------------
# GitHub stores the gitlink; contents are fetched with:
#   git submodule update --init third_party/slang
option(RAVEN_USE_SLANG "Configure against the Slang shader compiler (submodule third_party/slang)" OFF)
if(RAVEN_USE_SLANG)
    if(NOT EXISTS ${CMAKE_SOURCE_DIR}/third_party/slang/CMakeLists.txt)
        message(FATAL_ERROR "RAVEN_USE_SLANG=ON but third_party/slang is not checked out. Run: git submodule update --init third_party/slang")
    endif()
    message(STATUS "RavenEngine: Slang submodule found at ${CMAKE_SOURCE_DIR}/third_party/slang")
endif()

# --- assimp (model importing) — pinned git submodule, too large to vendor ------------------
option(RAVEN_USE_ASSIMP "Configure against the assimp importer (submodule third_party/assimp)" OFF)
if(RAVEN_USE_ASSIMP)
    if(NOT EXISTS ${CMAKE_SOURCE_DIR}/third_party/assimp/CMakeLists.txt)
        message(FATAL_ERROR "RAVEN_USE_ASSIMP=ON but third_party/assimp is not checked out. Run: git submodule update --init third_party/assimp")
    endif()
    set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
    set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
    set(ASSIMP_INJECT_DEBUG_POSTFIX OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/assimp third_party-assimp-build EXCLUDE_FROM_ALL)
endif()
