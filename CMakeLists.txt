cmake_minimum_required(VERSION 3.24...4.0)
project(RavenEngine
    VERSION 0.1.0
    DESCRIPTION "RavenEngine — a low-resource-focused C++20 Vulkan game engine and editor"
    HOMEPAGE_URL "https://github.com/Jacinth-19/RavenEngine-"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Default to Release when not specified (multi-config generators ignore this).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# -----------------------------------------------------------------------------------------
# Options
# -----------------------------------------------------------------------------------------
option(RAVEN_BUILD_EDITOR        "Build the RavenEngine editor (RavenEditor)" ON)
option(RAVEN_BUILD_SANDBOX       "Build the minimal sandbox runtime (raven-sandbox)" ON)
option(RAVEN_BUILD_TESTS         "Build unit tests" ON)
option(RAVEN_ENABLE_ASAN         "Enable AddressSanitizer for engine targets" OFF)
option(RAVEN_FORCE_VENDORED_GLFW  "Ignore system GLFW and build the vendored one" OFF)

include(${CMAKE_SOURCE_DIR}/cmake/RavenDeps.cmake)

set(RAVEN_GUI_ENABLED OFF)
if(RAVEN_BUILD_EDITOR OR RAVEN_BUILD_SANDBOX)
    raven_find_or_build_glfw()
    if(RAVEN_GLFW_FOUND AND RAVEN_VULKAN_FOUND)
        set(RAVEN_GUI_ENABLED ON)
    endif()
endif()

# -----------------------------------------------------------------------------------------
# Editor third-party libraries (only when a GUI is possible)
# -----------------------------------------------------------------------------------------
if(RAVEN_GUI_ENABLED)
    # Dear ImGui (docking branch) + Vulkan/GLFW backends.
    add_library(raven_imgui STATIC
        third_party/imgui/imgui.cpp
        third_party/imgui/imgui_draw.cpp
        third_party/imgui/imgui_tables.cpp
        third_party/imgui/imgui_widgets.cpp
        third_party/imgui/imgui_demo.cpp
        third_party/imgui/backends/imgui_impl_glfw.cpp
        third_party/imgui/backends/imgui_impl_vulkan.cpp
        third_party/imguizmo/ImGuizmo.cpp)
    target_include_directories(raven_imgui PUBLIC
        ${CMAKE_SOURCE_DIR}/third_party/imgui
        ${CMAKE_SOURCE_DIR}/third_party/imgui/backends
        ${CMAKE_SOURCE_DIR}/third_party/imguizmo
        ${CMAKE_SOURCE_DIR}/third_party/Vulkan-Headers/include)
    target_link_libraries(raven_imgui PUBLIC raven::vulkan ${RAVEN_GLFW_TARGET_NAME})
    # GLFW is used for windowing/input only — never for OpenGL (GL dev headers may be absent).
    target_compile_definitions(raven_imgui PUBLIC GLFW_INCLUDE_NONE)
    if(MSVC)
        target_compile_options(raven_imgui PRIVATE /W0)
    else()
        target_compile_options(raven_imgui PRIVATE -w)
    endif()
endif()

# -----------------------------------------------------------------------------------------
# Sub-projects
# -----------------------------------------------------------------------------------------
add_subdirectory(engine)
add_subdirectory(sandbox)
add_subdirectory(editor)
if(RAVEN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "")
message(STATUS "RavenEngine ${PROJECT_VERSION} configuration")
message(STATUS "  Build type        : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Vulkan            : ${RAVEN_VULKAN_FOUND}")
message(STATUS "  GUI (editor/sandbox): ${RAVEN_GUI_ENABLED}")
message(STATUS "  Tests             : ${RAVEN_BUILD_TESTS}")
message(STATUS "  Install prefix    : ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")
