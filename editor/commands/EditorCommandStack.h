// RavenEditor — the editor's command stack (§6.2).
//
// One process-wide instance wraps a core raven::CommandStack (cap 100, compounds,
// observers, redo-clearing semantics — Phase 0). Adds:
//   * Quiet-window debounce (150 ms default) for burst sources — commands pushed
//     with a tag inside the window replace-merge into one pending entry instead of
//     spamming undo. The merged command executes at flush time; recording and
//     execution happen together, so history never sees half-applied bursts.
//   * Status label helpers ("Undo: …"/"Redo: …") for menus/HistoryPanel.
//   * RAII compound guard.
//
// Header-only: included by the editor app AND by tests directly (no editor lib link).
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "raven/core/Undo.h"
#include "editor/commands/ICoalescable.h"

namespace raven::editor {

// Process-wide editor command stack. Thread affinity: main thread only.
class EditorCommandStack {
public:
    EditorCommandStack() = default;

    // Immediate: executes and records now.
    void Push(const std::shared_ptr<raven::ICommand>& cmd) {
        if (!cmd) return;
        m_Stack.PushExecuted(cmd);
    }

    // Debounced: the command is queued, not executed; repeated pushes with the same
    // tag inside the quiet window are CoalesceWith-ed into the pending entry (first
    // before-state wins, last after-state wins). The quiet timer restarts on every
    // push. When the window expires the command executes and lands on the undo stack.
    void PushDebounced(const std::string& tag, std::shared_ptr<raven::ICommand> cmd,
                       float quietSeconds = 0.15f) {
        if (!cmd) return;
        if (quietSeconds <= 0.0f) { Push(cmd); return; }
        auto& pending = m_Pending[tag];
        if (pending.cmd) {
            auto* coal = dynamic_cast<ICoalescable*>(pending.cmd.get());
            auto* newer = dynamic_cast<const ICoalescable*>(cmd.get());
            if (coal && newer) coal->CoalesceWith(*newer);
            else pending.cmd = cmd;   // not coalescable: latest wins entirely
            pending.quiet = 0.0f;
            pending.quietLimit = quietSeconds;
            return;
        }
        pending.cmd = std::move(cmd);
        pending.quiet = 0.0f;
        pending.quietLimit = quietSeconds;
    }

    // Advance all quiet timers; flush the entries whose window expired. Call once per
    // frame (OnFrame) — and freely from tests with an arbitrary dt.
    void DrainDebounce(float deltaSeconds) {
        for (auto it = m_Pending.begin(); it != m_Pending.end();) {
            Pending& p = it->second;
            p.quiet += deltaSeconds;
            if (p.quiet >= p.quietLimit) {
                std::shared_ptr<raven::ICommand> cmd = std::move(p.cmd);
                it = m_Pending.erase(it);
                Push(cmd);
            } else {
                ++it;
            }
        }
    }

    // Force-flush every pending debounced entry (shutdown, tests).
    void FlushDebounce() {
        while (!m_Pending.empty()) DrainDebounce(1e6f);
    }

    size_t PendingDebounceCount() const { return m_Pending.size(); }

    // RAII compound: opens a compound on scope entry, seals on exit.
    struct CompoundGuard {
        EditorCommandStack& stack;
        explicit CompoundGuard(EditorCommandStack& stack, const std::string& name) : stack(stack) {
            stack.m_Stack.BeginCompound(name);
        }
        ~CompoundGuard() { stack.m_Stack.EndCompound(); }
        CompoundGuard(const CompoundGuard&) = delete;
        CompoundGuard& operator=(const CompoundGuard&) = delete;
    };

    // Live burst path for continuous widgets (gizmo drags, inspector DragFloat3s):
    // executes IMMEDIATELY (the viewport stays live) and, within the quiet window,
    // folds the push into the still-open history entry instead of recording a new
    // one. When the quiet window expires the burst seals; the next push opens a
    // fresh entry. Keeps one undo entry per gesture with per-frame application.
    void PushCoalesced(const std::string& tag, std::shared_ptr<raven::ICommand> cmd,
                       float quietSeconds = 0.15f) {
        if (!cmd) return;
        if (quietSeconds <= 0.0f) { Push(cmd); return; }
        auto it = m_Live.find(tag);
        if (it != m_Live.end() && it->second.age < it->second.limit) {
            auto* folded = dynamic_cast<ICoalescable*>(it->second.cmd.get());
            const auto* newer = dynamic_cast<const ICoalescable*>(cmd.get());
            if (folded && newer) {
                folded->CoalesceWith(*newer);      // keep first before, take latest after
                it->second.age = 0.0f;
                m_Stack.ClearRedo();               // new edit invalidates redo tail
                it->second.cmd->Execute();         // apply the folded value now
                return;
            }
        }
        Push(cmd);                                  // cold start of a burst (executes + records)
        m_Live[tag] = LiveEntry{cmd, 0.0f, quietSeconds};
    }

    // Advance live-burst ages; expired bursts seal (the entry stays on the undo stack).
    void DrainLiveBursts(float deltaSeconds) {
        for (auto it = m_Live.begin(); it != m_Live.end();) {
            it->second.age += deltaSeconds;
            if (it->second.age >= it->second.limit) it = m_Live.erase(it);
            else ++it;
        }
    }

    size_t PendingLiveBurstCount() const { return m_Live.size(); }

    // Passthroughs (the HistoryPanel binds to the observer list directly).
    raven::CommandStack& Raw() { return m_Stack; }
    const raven::CommandStack& Raw() const { return m_Stack; }

    // Status label, e.g. "Undo: Gizmo Transform" / "Redo: Paste Component".
    std::string UndoLabel() const { return m_Stack.UndoName().empty() ? std::string() : "Undo: " + m_Stack.UndoName(); }
    std::string RedoLabel() const { return m_Stack.RedoName().empty() ? std::string() : "Redo: " + m_Stack.RedoName(); }

private:
    struct Pending {
        std::shared_ptr<raven::ICommand> cmd;
        float quiet = 0.0f;
        float quietLimit = 0.15f;
    };
    struct LiveEntry {
        std::shared_ptr<raven::ICommand> cmd;
        float age = 0.0f;
        float limit = 0.15f;
    };
    raven::CommandStack m_Stack{100};
    std::unordered_map<std::string, Pending> m_Pending;
    std::unordered_map<std::string, LiveEntry> m_Live;
};

// Process-wide editor command stack (lazily created, intentionally leaked).
inline EditorCommandStack& Commands() {
    static EditorCommandStack* s_Instance = new EditorCommandStack();
    return *s_Instance;
}

} // namespace raven::editor
