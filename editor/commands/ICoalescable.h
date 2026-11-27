// RavenEditor — coalescing contract for burst sources (inspector drags, gizmos,
// material edits). A command that can fold a newer command of the same kind into
// itself implements ICoalescable; EditorCommandStack folds bursts through it.
// Header-only, no dependencies.
#pragma once

namespace raven::editor {

// Commands that can be merged with a later instance of themselves carry the merge
// semantics here (kept first-before/last-after).
class ICoalescable {
public:
    virtual ~ICoalescable() = default;
    // Fold `newer` into this command (keep this one's "before" state).
    virtual void CoalesceWith(const ICoalescable& newer) = 0;
};

} // namespace raven::editor
