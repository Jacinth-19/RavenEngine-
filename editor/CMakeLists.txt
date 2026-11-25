# RavenEditor — the standalone editor application.
if(NOT RAVEN_GUI_ENABLED)
    message(STATUS "editor: skipped (no windowing available — see docs/BUILDING.md)")
    return()
endif()
