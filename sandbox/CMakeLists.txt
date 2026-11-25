# raven-sandbox — minimal runnable app (also used for headless smoke runs).
if(NOT RAVEN_GUI_ENABLED)
    message(STATUS "sandbox: skipped (no windowing available — see docs/BUILDING.md)")
    return()
endif()

add_executable(raven-sandbox main.cpp)
target_link_libraries(raven-sandbox PRIVATE raven_render ${RAVEN_GLFW_TARGET_NAME})

if(UNIX AND NOT APPLE)
    target_link_libraries(raven-sandbox PRIVATE dl)
endif()

target_compile_definitions(raven-sandbox PRIVATE
    RAVEN_SAMPLE_DIR="${CMAKE_SOURCE_DIR}/sample"
    RAVEN_ENGINE_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets/engine")

if(MSVC)
    target_compile_options(raven-sandbox PRIVATE /W3)
else()
    target_compile_options(raven-sandbox PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
