# RavenEditor — the standalone editor application.
# Windowed by default; when the GUI is built headless (RAVEN_GUI_HEADLESS=ON, null
# platform GLFW / no display), the editor runs in an offscreen headless mode
# (--headless) that renders the scene + UI to an image and writes it to disk.
if(NOT RAVEN_GUI_ENABLED)
    message(STATUS "editor: skipped (no windowing available — see docs/BUILDING.md)")
    return()
endif()

add_executable(ravened
    main.cpp
    editor_app.cpp)

target_include_directories(ravened PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ravened PRIVATE
    raven_render
    raven_imgui
    ${RAVEN_GLFW_TARGET_NAME})

if(UNIX AND NOT APPLE)
    target_link_libraries(ravened PRIVATE dl)
endif()

target_compile_definitions(ravened PRIVATE
    RAVEN_VERSION="${PROJECT_VERSION}"
    RAVEN_SAMPLE_DIR="${CMAKE_SOURCE_DIR}/sample")

if(MSVC)
    target_compile_options(ravened PRIVATE /W3)
else()
    target_compile_options(ravened PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
