// RavenEditor — signals that cross the process-wide EventBus (editor §Phase 2).
//
// Payloads are small PODs/strings; never pointers to frame data. The editor
// republishes engine notices here; panels subscribe with Unsubscribe in teardown
// or outlive the bus. EventBus::Drain() is called once per frame by the editor.
#pragma once

#include <string>

#include "raven/core/EventBus.h"
#include "raven/core/FileWatcher.h"
#include "raven/core/UUID.h"
#include "raven/core/EditorHooks.h"       // AssetReloadKind

namespace raven::editor {

// Selection changed — consumers: hierarchy highlight, inspector, viewport outline.
struct SelectionChangedSignal : raven::EditorSignal {
    uint64_t version = 0;    // monotonic; bumped on every mutation
    raven::UUID primary;     // nil UUID = selection cleared
};

// Scene dirty state flipped (menus, title marker, autosave gating).
struct SceneDirtyChangedSignal : raven::EditorSignal {
    bool dirty = false;
};

// Console entry mirrored onto the bus (Logger callback → this). Level is raven::LogLevel's
// underlying int to keep this header free of Logger.h.
struct LogEntrySignal : raven::EditorSignal {
    int level = 2;           // raven::LogLevel (Info)
    std::string channel;     // "General" until the logger grows channels
    std::string text;
    uint64_t id = 0;         // monotonic per editor session
};

// Asset hot-reload notice (FileWatcher lands in Phase 3; editor hooks fill this today).
struct AssetChangedSignal : raven::EditorSignal {
    raven::UUID asset;
    raven::AssetReloadKind kind = raven::AssetReloadKind::Modified;
};

// Raw file-system notice from the assets-root watcher (produced on the watcher
// thread). The editor consumes it on the main thread at frame time: maps the
// path to an asset UUID, rescans the database, and republishes as AssetChangedSignal.
struct AssetFileChangedSignal : raven::EditorSignal {
    std::string path;                    // absolute path of the file that changed
    raven::FileEvent event = raven::FileEvent::Modified;
};

// The resource-mode asset selection changed (nil = asset inspector closed).
struct AssetSelectionChangedSignal : raven::EditorSignal {
    raven::UUID asset;   // nil = cleared
};

// Workspaces (Window > Workspaces) changed membership/current selection.
struct WorkspaceChangedSignal : raven::EditorSignal {
    std::string name;        // current workspace after the change
};

// A project was opened (launcher path or File > Open Project).
struct ProjectChangedSignal : raven::EditorSignal {
    std::string name;
    std::string path;
};

// Play state machine (Phase 4). Editing/Playing/Paused as int (raven::PlayState).
struct PlayStateChangedSignal : raven::EditorSignal {
    int state = 0;           // raven::PlayState
};

// Frame debugger freeze/step notices (Phase 5).
struct FrameDebuggerNoticeSignal : raven::EditorSignal {
    std::string message;
};

// Import pipeline reported a mapping ambiguity or completion (Phase 3).
struct ImportReportSignal : raven::EditorSignal {
    std::string summary;
};

} // namespace raven::editor
