# RavenEngine engine libraries.
#
# raven_core   — renderer-agnostic: core systems, ECS, scene, assets, physics, scripting, audio.
# raven_render — Vulkan renderer (skipped when no system Vulkan is available; CI has it).

file(GLOB_RECURSE RAVEN_CORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ecs/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assets/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/physics/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/script/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/RenderGraph.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/Camera.cpp)

file(GLOB_RECURSE RAVEN_RENDER_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/vk/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/Renderer.cpp)

add_library(raven_core STATIC ${RAVEN_CORE_SOURCES})
add_library(raven::core ALIAS raven_core)

target_include_directories(raven_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(raven_core
    PUBLIC
        raven_glm
        raven_json
        raven_stb
        raven_tinyobj
        raven_lua
        raven_miniaudio)

if(RAVEN_MSVC)
    target_compile_options(raven_core PRIVATE /W3)
    target_compile_definitions(raven_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(raven_core PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

if(WIN32)
    target_link_libraries(raven_core PUBLIC bcrypt) # entropy source
endif()

if(RAVEN_ENABLE_ASAN)
    target_compile_options(raven_core PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(raven_core PUBLIC -fsanitize=address)
endif()

# --- Renderer (requires system Vulkan) ---------------------------------------------------
if(RAVEN_VULKAN_FOUND)
    add_library(raven_render STATIC ${RAVEN_RENDER_SOURCES})
    add_library(raven::render ALIAS raven_render)
    target_include_directories(raven_render
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(raven_render
        PUBLIC
            raven_core
            raven::vulkan)
    if(RAVEN_MSVC)
        target_compile_options(raven_render PRIVATE /W3)
    else()
        target_compile_options(raven_render PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
    endif()
    message(STATUS "RavenEngine: raven_render enabled (system Vulkan)")
else()
    message(STATUS "RavenEngine: raven_render DISABLED (no system Vulkan found)")
endif()
