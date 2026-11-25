# raven-sandbox — minimal runnable app (also used for CI smoke tests).
if(NOT RAVEN_GUI_ENABLED)
    message(STATUS "sandbox: skipped (no windowing available — see docs/BUILDING.md)")
    return()
endif()
