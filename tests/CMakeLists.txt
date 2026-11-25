# RavenEngine tests — self-contained runner (no external frameworks).
# GPU smoke test is built only when system Vulkan is available (CI: Lavapipe).

add_executable(raven_tests
    test_main.cpp
    test_render_graph.cpp
    test_scene.cpp
    test_camera.cpp
    test_uuid.cpp
    test_assets.cpp
    test_picking.cpp)

target_include_directories(raven_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(raven_tests PRIVATE raven_core)
target_compile_definitions(raven_tests PRIVATE
    RAVEN_SAMPLE_DIR="${CMAKE_SOURCE_DIR}/sample"
    RAVEN_ENGINE_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets/engine")

if(RAVEN_VULKAN_FOUND)
    target_sources(raven_tests PRIVATE test_vulkan_smoke.cpp)
    target_link_libraries(raven_tests PRIVATE raven_render)
endif()

if(MSVC)
    target_compile_options(raven_tests PRIVATE /W3)
else()
    target_compile_options(raven_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_test(NAME raven_tests COMMAND raven_tests)
