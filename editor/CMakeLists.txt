# RavenEditor — editor library + the standalone editor application.
#
# raven_editor STATIC — ALL editor code (panels, workspaces, settings, launcher, shell).
#   The engine never links this; raven-sandbox never links this (check_layering.py).
# ravened EXE — thin entry point (arg parsing, window/headless bring-up, loop).
#
# Headless: when the GUI is built headless (RAVEN_GUI_HEADLESS=ON, null-platform
# GLFW / no display), the editor runs an offscreen --headless mode and writes a PNG.
if(NOT RAVEN_GUI_ENABLED)
    message(STATUS "editor: skipped (no windowing available — see docs/BUILDING.md)")
    return()
endif()

add_library(raven_editor STATIC
    editor_app.cpp
    core/EditorConfig.cpp
    core/Fonts.cpp
    core/RavenEditorHooks.cpp
    core/StylePresets.cpp
    core/AssetOperations.cpp
    core/Clipboard.cpp
    core/Workspaces.cpp
    widgets/ThumbnailCache.cpp
    widgets/ThumbnailGenerator.cpp
    widgets/ImportProgress.cpp
    widgets/TextureRemapDialog.cpp
    core/TextureRemap.cpp
    inspectors/ComponentDrawerRegistry.cpp
    inspectors/MaterialEditor.cpp
    inspectors/TextureEditor.cpp
    inspectors/ResourceInspector.cpp
    panels/PanelRegistry.cpp
    panels/ProjectPanel.cpp)
add_library(raven::editor ALIAS raven_editor)

target_include_directories(raven_editor
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR})

target_link_libraries(raven_editor
    PUBLIC
        raven_render
        raven_imgui
        raven_phosphor
        ${RAVEN_GLFW_TARGET_NAME})

if(UNIX AND NOT APPLE)
    target_link_libraries(raven_editor PRIVATE ${CMAKE_DL_LIBS})
endif()

target_compile_definitions(raven_editor PRIVATE
    RAVEN_VERSION="${PROJECT_VERSION}"
    RAVEN_SAMPLE_DIR="${CMAKE_SOURCE_DIR}/sample"
    RAVEN_EDITOR_STYLE_DIR="${CMAKE_SOURCE_DIR}/editor/assets/styles")

if(MSVC)
    target_compile_options(raven_editor PRIVATE /W3 /utf-8)
else()
    target_compile_options(raven_editor PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_executable(ravened main.cpp)
target_link_libraries(ravened PRIVATE raven_editor)

target_compile_definitions(ravened PRIVATE
    RAVEN_VERSION="${PROJECT_VERSION}"
    RAVEN_SAMPLE_DIR="${CMAKE_SOURCE_DIR}/sample")

if(MSVC)
    target_compile_options(ravened PRIVATE /W3 /utf-8)
else()
    target_compile_options(ravened PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
