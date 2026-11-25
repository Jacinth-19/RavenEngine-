// RavenEngine — undo/redo command stack (editor uses this for every
// user-facing mutation). Commands implement Execute/Uexecute; the stack
// itself lives in the core so it is unit-testable without the editor.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace raven {

class ICommand {
public:
    virtual ~ICommand() = default;
    virtual void Execute() = 0;
    virtual void Uexecute() = 0;
    virtual std::string Name() const = 0;
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

class CommandStack {
public:
    explicit CommandStack(size_t maxDepth = 100) : m_MaxDepth(maxDepth) {}

    // Executes the command immediately and records it. Clears the redo branch.
    void PushExecuted(std::shared_ptr<ICommand> command) {
        if (!command) return;
        command->Execute();
        m_Undo.push_back(std::move(command));
        m_Redo.clear();
        TrimToMaxDepth();
        Notify();
    }

    bool Undo() {
        if (m_Undo.empty()) return false;
        auto cmd = std::move(m_Undo.back());
        m_Undo.pop_back();
        cmd->Uexecute();
        m_Redo.push_back(std::move(cmd));
        Notify();
        return true;
    }

    bool Redo() {
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
        Notify();
    }

    bool CanUndo() const { return !m_Undo.empty(); }
    bool CanRedo() const { return !m_Redo.empty(); }
    const std::string UndoName() const { return m_Undo.empty() ? std::string() : m_Undo.back()->Name(); }
    const std::string RedoName() const { return m_Redo.empty() ? std::string() : m_Redo.back()->Name(); }
    size_t Depth() const { return m_Undo.size(); }

    // Editor subscribes to refresh button enabled-states etc.
    void SetChangeCallback(std::function<void()> cb) { m_OnChange = std::move(cb); }

private:
    void TrimToMaxDepth() {
        while (m_Undo.size() > m_MaxDepth) m_Undo.erase(m_Undo.begin());
    }
    void Notify() {
        if (m_OnChange) m_OnChange();
    }

    size_t m_MaxDepth;
    std::vector<std::shared_ptr<ICommand>> m_Undo;
    std::vector<std::shared_ptr<ICommand>> m_Redo;
    std::function<void()> m_OnChange;
};

} // namespace raven
