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
