// RavenEditor — semantic icon layer over the raw Phosphor glyph defines
// (phosphor_icons.h). Panels, menus and toolbars use the semantic names ONLY —
// swapping an glyph or a whole pack means touching this file, not 200 call sites.
//
// 1:1 in spirit with Luth widgets/Icons.h: every icon the UI uses gets a semantic
// name; raw ICON_PH_* may not leak into panel code (grep is the enforcement).
#pragma once

#include "phosphor_icons.h"

// --- app / chrome -----------------------------------------------------------
#define ICON_APP_LOGO            ICON_PH_ROCKET
#define ICON_GITHUB              ICON_PH_GITHUB_LOGO
#define ICON_INFO                ICON_PH_INFO
#define ICON_CLOSE               ICON_PH_X
#define ICON_SAVE                ICON_PH_FLOPPY_DISK
#define ICON_REFRESH             ICON_PH_ARROWS_CLOCKWISE
#define ICON_ADD                 ICON_PH_PLUS
#define ICON_COPY                ICON_PH_COPY
#define ICON_DELETE              ICON_PH_TRASH
#define ICON_SEARCH              ICON_PH_SLIDERS_HORIZONTAL
#define ICON_SETTINGS            ICON_PH_WRENCH
#define ICON_HELP                ICON_PH_INFO
#define ICON_PACKAGE             ICON_PH_PACKAGE

// --- menus / workspace ------------------------------------------------------
#define ICON_WORKSPACE_SAVE      ICON_PH_FLOPPY_DISK
#define ICON_WORKSPACE_RESET     ICON_PH_ARROWS_CLOCKWISE
#define ICON_THEME_DARK          ICON_PH_SUN
#define ICON_THEME_LIGHT         ICON_PH_SUN
#define ICON_LAYOUT_GRID         ICON_PH_GRID_FOUR

// --- panels (View menu + tabs) ----------------------------------------------
#define ICON_PANEL_HIERARCHY     ICON_PH_LIST
#define ICON_PANEL_INSPECTOR     ICON_PH_SLIDERS_HORIZONTAL
#define ICON_PANEL_ASSETS        ICON_PH_IMAGE
#define ICON_PANEL_PROFILER      ICON_PH_CHART_LINE
#define ICON_PANEL_CONSOLE       ICON_PH_TERMINAL
#define ICON_PANEL_GRID          ICON_PH_GRID_FOUR
#define ICON_PANEL_VIEWPORT      ICON_PH_FRAME_CORNERS

// --- viewport toolbar / gizmos -----------------------------------------------
#define ICON_GIZMO_MOVE          ICON_PH_ARROWS_OUT_CARDINAL
#define ICON_GIZMO_ROTATE        ICON_PH_ARROWS_CLOCKWISE
#define ICON_GIZMO_SCALE         ICON_PH_ARROWS_OUT_SIMPLE
#define ICON_SNAP                ICON_PH_MAGNET
#define ICON_LOCAL               ICON_PH_FRAME_CORNERS
#define ICON_FOCUS               ICON_PH_CROSSHAIR

// --- entity type glyphs -------------------------------------------------------
#define ICON_ENTITY_MESH         ICON_PH_CUBE
#define ICON_ENTITY_SUN          ICON_PH_SUN
#define ICON_ENTITY_LIGHT        ICON_PH_LIGHTBULB
#define ICON_ENTITY_CAMERA       ICON_PH_CAMERA
#define ICON_ENTITY_AUDIO        ICON_PH_MUSIC_NOTE
#define ICON_ENTITY_SCRIPT       ICON_PH_FILE_CODE
#define ICON_ENTITY_BODY         ICON_PH_PACKAGE
#define ICON_ENTITY_GENERIC      ICON_PH_SQUARES_FOUR

// --- asset type glyphs ---------------------------------------------------------
#define ICON_ASSET_FOLDER        ICON_PH_FOLDER
#define ICON_ASSET_SCENE         ICON_PH_FILM_STRIP
#define ICON_ASSET_MESH          ICON_PH_CUBE
#define ICON_ASSET_IMAGE         ICON_PH_IMAGE
#define ICON_ASSET_SCRIPT        ICON_PH_FILE_CODE
#define ICON_ASSET_AUDIO         ICON_PH_MUSIC_NOTE
#define ICON_ASSET_MATERIAL      ICON_PH_PALETTE
#define ICON_ASSET_PREFAB        ICON_PH_PACKAGE
#define ICON_ASSET_SHADER        ICON_PH_CPU
#define ICON_ASSET_GENERIC       ICON_PH_SLIDERS_HORIZONTAL

// --- console levels -------------------------------------------------------------
#define ICON_LOG_INFO            ICON_PH_INFO
#define ICON_LOG_WARN            ICON_PH_INFO
#define ICON_LOG_ERROR           ICON_PH_X
