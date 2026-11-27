// RavenEngine — undo/redo command stack (editor uses this for every
// user-facing mutation). Commands implement Execute/Uexecute; the stack
// itself lives in the core so it is unit-testable without the editor.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "raven/core/Logger.h"

namespace raven {

class ICommand {
public:
    virtual ~ICommand() = default;
    virtual void Execute() = 0;
    virtual void Uexecute() = 0;
    virtual std::string Name() const = 0;
    // Compound entries (see CommandStack::BeginCompound) override this.
    virtual bool IsCompound() const { return false; }
};

class LambdaCommand final : public ICommand {
public:
    LambdaCommand(std::string name, std::function<void()> execute, std::function<void()> unexecute)
        : m_Name(std::move(name)), m_Execute(std::move(execute)), m_Uexecute(std::move(unexecute)) {}

    void Execute() override { m_Execute(); }
    void Uexecute() override { m_Uexecute(); }
    std::string Name() const override { return m_Name; }

private:
    std::string m_Name;
    std::function<void()> m_Execute;
    std::function<void()> m_Uexecute;
};

// A group of commands sealed by EndCompound() into a single undo/redo unit.
// Execute replays children in order; Uexecute unwinds them in reverse order.
// Children are recorded already-executed (CommandStack does not re-run them at seal time).
class CompoundCommand final : public ICommand {
public:
    CompoundCommand(std::string name, std::vector<std::shared_ptr<ICommand>> children)
        : m_Name(std::move(name)), m_Children(std::move(children)) {}

    void Execute() override {
        for (auto& child : m_Children) child->Execute();
    }
    void Uexecute() override {
        for (auto it = m_Children.rbegin(); it != m_Children.rend(); ++it) (*it)->Uexecute();
    }
    std::string Name() const override { return m_Name; }
    bool IsCompound() const override { return true; }

    size_t ChildCount() const { return m_Children.size(); }
    const std::vector<std::shared_ptr<ICommand>>& Children() const { return m_Children; }

private:
    std::string m_Name;
    std::vector<std::shared_ptr<ICommand>> m_Children;
};

class CommandStack {
public:
    explicit CommandStack(size_t maxDepth = 100) : m_MaxDepth(maxDepth) {}

    // Executes the command immediately and records it. Clears the redo branch.
    // While a compound is open (BeginCompound), the command runs immediately but is
    // routed into the compound buffer instead of the undo stack; the group is sealed —
    // and the redo branch cleared — when EndCompound() is called.
    void PushExecuted(std::shared_ptr<ICommand> command) {
        if (!command) return;
        command->Execute();
        if (m_InCompound) {
            m_CompoundChildren.push_back(std::move(command));
            Notify();
            return;
        }
        m_Undo.push_back(std::move(command));
        m_Redo.clear();
        TrimToMaxDepth();
        Notify();
    }

    bool Undo() {
        if (m_InCompound) return false;  // refusing mid-compound keeps command pairs coherent
        if (m_Undo.empty()) return false;
        auto cmd = std::move(m_Undo.back());
        m_Undo.pop_back();
        cmd->Uexecute();
        m_Redo.push_back(std::move(cmd));
        Notify();
        return true;
    }

    bool Redo() {
        if (m_InCompound) return false;
        if (m_Redo.empty()) return false;
        auto cmd = std::move(m_Redo.back());
        m_Redo.pop_back();
        cmd->Execute();
        m_Undo.push_back(std::move(cmd));
        Notify();
        return true;
    }

    void Clear() {
        m_Undo.clear();
        m_Redo.clear();
        m_InCompound = false;
        m_CompoundChildren.clear();
        Notify();
    }

    // Drop the redo branch without touching undo history. Used by the live-coalescing
    // path in the editor (mutating the top burst command in place must not leave a
    // stale redo tail behind).
    void ClearRedo() {
        if (m_Redo.empty()) return;
        m_Redo.clear();
        Notify();
    }

    bool CanUndo() const { return !m_Undo.empty(); }
    bool CanRedo() const { return !m_Redo.empty(); }
    const std::string UndoName() const { return m_Undo.empty() ? std::string() : m_Undo.back()->Name(); }
    const std::string RedoName() const { return m_Redo.empty() ? std::string() : m_Redo.back()->Name(); }
    size_t Depth() const { return m_Undo.size(); }

    // Compound grouping — Luth CommandHistory::BeginCompound/EndCompound parity.
    // BeginCompound(name) routes subsequent commands into a single undo entry;
    // EndCompound seals it. Nested BeginCompound is rejected (logged, returns false).
    bool BeginCompound(const std::string& name);
    bool EndCompound();
    bool InCompound() const { return m_InCompound; }
    const std::string& CompoundName() const { return m_CompoundName; }

    // Direct read access for history UI (HistoryPanel renders entries top-down).
    const std::vector<std::shared_ptr<ICommand>>& UndoEntries() const { return m_Undo; }
    const std::vector<std::shared_ptr<ICommand>>& RedoEntries() const { return m_Redo; }

    // Observer list (HistoryPanel, status-bar undo label). Unlike SetChangeCallback,
    // arbitrary observers can be registered/removed independently of one another.
    using ObserverHandle = uint64_t;
    ObserverHandle AddObserver(std::function<void()> observer);
    bool RemoveObserver(ObserverHandle handle);

    // Editor subscribes to refresh button enabled-states etc.
    void SetChangeCallback(std::function<void()> cb) { m_OnChange = std::move(cb); }

private:
    void TrimToMaxDepth() {
        while (m_Undo.size() > m_MaxDepth) m_Undo.erase(m_Undo.begin());
    }
    void Notify() {
        if (m_OnChange) m_OnChange();
        if (m_Observers.empty()) return;
        std::vector<std::pair<ObserverHandle, std::function<void()>>> snapshot = m_Observers;  // observers may unsubscribe mid-notify
        for (auto& [handle, fn] : snapshot) {
            (void)handle;
            if (fn) fn();
        }
    }

    size_t m_MaxDepth;
    std::vector<std::shared_ptr<ICommand>> m_Undo;
    std::vector<std::shared_ptr<ICommand>> m_Redo;
    std::function<void()> m_OnChange;

    bool m_InCompound = false;
    std::string m_CompoundName;
    std::vector<std::shared_ptr<ICommand>> m_CompoundChildren;

    ObserverHandle m_NextObserverId = 1;  // 0 reserved
    std::vector<std::pair<ObserverHandle, std::function<void()>>> m_Observers;
};

inline bool CommandStack::BeginCompound(const std::string& name) {
    if (m_InCompound) {
        Logger::Instance().Log(LogLevel::Error, "CommandStack: nested BeginCompound('" + name + "') rejected — a compound is already open ('" + m_CompoundName + "')");
        return false;
    }
    m_InCompound = true;
    m_CompoundName = name;
    m_CompoundChildren.clear();
    Notify();
    return true;
}

inline bool CommandStack::EndCompound() {
    if (!m_InCompound) {
        Logger::Instance().Log(LogLevel::Error, "CommandStack: EndCompound() without BeginCompound — ignored");
        return false;
    }

    std::vector<std::shared_ptr<ICommand>> children = std::move(m_CompoundChildren);
    std::string name = std::move(m_CompoundName);
    m_InCompound = false;
    m_CompoundChildren.clear();

    if (children.empty()) {
        Notify();
        return true;  // sealing an empty compound is a no-op, not an error
    }

    m_Undo.push_back(std::make_shared<CompoundCommand>(std::move(name), std::move(children)));
    m_Redo.clear();
    TrimToMaxDepth();
    Notify();
    return true;
}

inline CommandStack::ObserverHandle CommandStack::AddObserver(std::function<void()> observer) {
    if (!observer) return 0;
    const ObserverHandle handle = m_NextObserverId++;
    m_Observers.emplace_back(handle, std::move(observer));
    return handle;
}

inline bool CommandStack::RemoveObserver(ObserverHandle handle) {
    for (size_t i = 0; i < m_Observers.size(); ++i) {
        if (m_Observers[i].first == handle) {
            m_Observers.erase(m_Observers.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

} // namespace raven
