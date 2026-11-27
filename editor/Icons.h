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
#define ICON_WARNING             ICON_PH_WARNING_CIRCLE
#define ICON_CHECKMARK           ICON_PH_CHECK
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
#define ICON_FOLDER_NEW          ICON_PH_FOLDER_PLUS
#define ICON_NEW_FILE            ICON_PH_FILE_PLUS
#define ICON_REVEAL              ICON_PH_ARROW_SQUARE_OUT
#define ICON_EXPAND_ALL          ICON_PH_ARROWS_OUT_SIMPLE
#define ICON_COLLAPSE_ALL        ICON_PH_ARROWS_IN_SIMPLE
#define ICON_FOLDER_OPEN         ICON_PH_FOLDER_OPEN
#define ICON_LINK                ICON_PH_LINK
#define ICON_CHECK               ICON_PH_CHECK
#define ICON_ASSET_FOLDER        ICON_PH_FOLDER
#define ICON_ASSET_SCENE         ICON_PH_FILM_STRIP
#define ICON_ASSET_ANIM          ICON_PH_FILM_STRIP
#define ICON_ASSET_MESH          ICON_PH_CUBE
#define ICON_ASSET_IMAGE         ICON_PH_IMAGE
#define ICON_ASSET_SCRIPT        ICON_PH_FILE_CODE
#define ICON_ASSET_AUDIO         ICON_PH_MUSIC_NOTE
#define ICON_ASSET_MATERIAL      ICON_PH_PALETTE
#define ICON_ASSET_PREFAB        ICON_PH_PACKAGE
#define ICON_ASSET_SHADER        ICON_PH_CPU
#define ICON_ASSET_GENERIC       ICON_PH_SLIDERS_HORIZONTAL

// --- hierarchy row widgets -------------------------------------------------------
#define ICON_ROW_EYE             ICON_PH_EYE
#define ICON_ROW_EYE_OFF         ICON_PH_EYE_SLASH
#define ICON_ROW_LOCK            ICON_PH_LOCK
#define ICON_ROW_UNLOCK          ICON_PH_LOCK_OPEN

// --- history panel ----------------------------------------------------------------
#define ICON_HISTORY             ICON_PH_CLOCK_COUNTER_CLOCKWISE
#define ICON_HISTORY_COMPOUND    ICON_PH_STACK
#define ICON_HISTORY_COMMAND     ICON_PH_ARROWS_DOWN_UP
#define ICON_RENAME              ICON_PH_PENCIL
#define ICON_TEXT                ICON_PH_TEXT_T
#define ICON_LIST                ICON_PH_LIST_BULLETS

// --- console levels -------------------------------------------------------------
#define ICON_LOG_INFO            ICON_PH_INFO
#define ICON_LOG_DEBUG           ICON_PH_INFO
#define ICON_LOG_WARN            ICON_PH_WARNING
#define ICON_LOG_ERROR           ICON_PH_X_CIRCLE
#define ICON_UNDO                ICON_PH_ARROW_COUNTER_CLOCKWISE
#define ICON_REDO                ICON_PH_ARROW_CLOCKWISE
