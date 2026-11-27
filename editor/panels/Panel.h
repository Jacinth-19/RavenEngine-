// RavenEditor — dockable panel base.
//
// Panels own exactly one ImGui window (or a small, fixed set) and keep ≤150 lines of
// draw logic. The registry (PanelRegistry) calls them behind an error boundary; panels
// never see it.
//
// Two ways in (Phase 1): subclass Panel directly (new panels), or wrap an existing
// member-function draw with CallablePanel (legacy panels during the incremental port).
#pragma once

#include <functional>
#include <string>

namespace raven::editor {

class Panel {
public:
    virtual ~Panel() = default;

    // Stable ImGui window id — unique, human-recognisable, never localized.
    virtual const char* Name() const = 0;

    // User intent (View menu / workspaces). Crashed panels stay registered.
    virtual bool IsOpen() const = 0;
    virtual void SetOpen(bool open) = 0;

    // Draw exactly one frame of the panel (ImGui Begin/End balanced within).
    virtual void Draw() = 0;
};

// Adapter: wraps a draw callable (usually an EditorApp member via std::bind / lambda)
// and references the external open-flag it toggles.
class CallablePanel final : public Panel {
public:
    CallablePanel(std::string name, bool* openFlag, std::function<void()> drawFn)
        : m_Name(std::move(name)), m_Open(openFlag), m_Draw(std::move(drawFn)) {}

    const char* Name() const override { return m_Name.c_str(); }
    bool IsOpen() const override { return m_Open ? *m_Open : true; }
    void SetOpen(bool open) override { if (m_Open) *m_Open = open; }
    void Draw() override { m_Draw(); }

private:
    std::string m_Name;
    bool* m_Open;
    std::function<void()> m_Draw;
};

} // namespace raven::editor
