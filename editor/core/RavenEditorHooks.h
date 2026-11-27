// RavenEditor — IEditorHooks implementation + install entry point.
//
// main.cpp calls raven::InstallEditorHooks() before any engine subsystem starts. The
// instance forwards to the running EditorApp (bound via BindEditorAppHooks from the app
// ctor); engine call sites that run before the app exists see a null binding and get
// runtime-only defaults.
#pragma once

namespace raven {

class EditorApp;

// Register the editor hooks with the engine registry (raven::EditorHooks::Register).
// Idempotent; safe to call once at process start.
void InstallEditorHooks();

// Bind (or unbind, with nullptr) the running EditorApp to the registered hooks.
void BindEditorAppHooks(EditorApp* app);

} // namespace raven
