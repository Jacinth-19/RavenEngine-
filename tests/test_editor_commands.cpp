// CommandStack: compounds, observer list, entry accessors (Phase 0 of the editor).
#include "test_framework.h"

#include <memory>
#include <string>
#include <vector>

#include "raven/core/Undo.h"

using namespace raven;

namespace {
// Records execute/unexecute side effects in a shared log.
std::shared_ptr<ICommand> MakeRecordingCommand(const std::string& name, std::vector<std::string>* log) {
    return std::make_shared<LambdaCommand>(name, [log, name] { log->push_back("do:" + name); },
                                               [log, name] { log->push_back("undo:" + name); });
}
} // namespace

RAVEN_TEST("commands: compound groups N commands into one undo entry") {
    CommandStack stack;
    std::vector<std::string> log;
    stack.BeginCompound("Paste Prefab");
    stack.PushExecuted(MakeRecordingCommand("a", &log));
    stack.PushExecuted(MakeRecordingCommand("b", &log));
    stack.PushExecuted(MakeRecordingCommand("c", &log));
    stack.EndCompound();

    CHECK_EQ(stack.Depth(), size_t(1));
    CHECK(stack.UndoName() == "Paste Prefab");
    CHECK(stack.UndoEntries().back()->IsCompound());
    auto compound = std::dynamic_pointer_cast<CompoundCommand>(stack.UndoEntries().back());
    CHECK(compound != nullptr);
    CHECK_EQ(compound->ChildCount(), size_t(3));

    CHECK(stack.Undo());
    CHECK_EQ(log.size(), size_t(6));  // 3 do + 3 undo
    CHECK(log[3] == "undo:c");  // reverse order
    CHECK(log[4] == "undo:b");
    CHECK(log[5] == "undo:a");
}

RAVEN_TEST("commands: compound redo replays children in order") {
    CommandStack stack;
    std::vector<std::string> log;
    stack.BeginCompound("Group");
    stack.PushExecuted(MakeRecordingCommand("x", &log));
    stack.PushExecuted(MakeRecordingCommand("y", &log));
    stack.EndCompound();

    stack.Undo();
    log.clear();
    stack.Redo();
    CHECK_EQ(log.size(), size_t(2));
    CHECK(log[0] == "do:x");
    CHECK(log[1] == "do:y");
}

RAVEN_TEST("commands: nested BeginCompound is rejected; EndCompound without begin fails cleanly") {
    CommandStack stack;
    CHECK(stack.BeginCompound("outer"));
    CHECK(!stack.BeginCompound("inner"));  // rejected, logged
    CHECK(stack.InCompound());
    CHECK(stack.CompoundName() == "outer");
    CHECK(stack.EndCompound());
    CHECK(!stack.EndCompound());  // no compound open anymore
    CHECK(stack.Depth() == 0);    // outer had no children, nothing sealed
}

RAVEN_TEST("commands: undo/redo are refused while a compound is open") {
    CommandStack stack;
    std::vector<std::string> log;
    stack.PushExecuted(MakeRecordingCommand("single", &log));
    stack.BeginCompound("wip");
    CHECK(!stack.Undo());  // refused mid-compound
    CHECK(!stack.Redo());
    stack.PushExecuted(MakeRecordingCommand("child", &log));
    stack.EndCompound();
    CHECK_EQ(stack.Depth(), size_t(2));
    CHECK(stack.Undo());  // undoes the compound only
    CHECK(stack.Undo());  // undoes "single"
}

RAVEN_TEST("commands: empty compound seals to nothing") {
    CommandStack stack;
    stack.BeginCompound("noop");
    stack.EndCompound();
    CHECK_EQ(stack.Depth(), size_t(0));
    CHECK(!stack.CanUndo());
}

RAVEN_TEST("commands: compound counts as one entry against max depth") {
    CommandStack stack(2);
    std::vector<std::string> log;
    stack.PushExecuted(MakeRecordingCommand("keep1", &log));
    stack.BeginCompound("group");
    stack.PushExecuted(MakeRecordingCommand("a", &log));
    stack.PushExecuted(MakeRecordingCommand("b", &log));
    stack.EndCompound();
    stack.PushExecuted(MakeRecordingCommand("keep2", &log));
    CHECK_EQ(stack.Depth(), size_t(2));  // keep1 was evicted
    CHECK(stack.UndoName() == "keep2");
}

RAVEN_TEST("commands: observers are notified on push/undo/redo/clear/compound seal") {
    CommandStack stack;
    std::vector<std::string> log;
    int notified = 0;
    auto handle = stack.AddObserver([&] { ++notified; });

    stack.PushExecuted(MakeRecordingCommand("a", &log));
    CHECK(notified >= 1);
    int afterPush = notified;
    stack.Undo();
    CHECK(notified > afterPush);
    stack.Redo();
    int afterRedo = notified;
    stack.BeginCompound("g");
    CHECK(notified > afterRedo);
    stack.EndCompound();
    CHECK(notified > afterRedo + 1);  // seal notifies
    stack.Clear();
    CHECK(notified > afterRedo + 1);

    CHECK(stack.RemoveObserver(handle));
    CHECK(!stack.RemoveObserver(handle));  // already removed
}

RAVEN_TEST("commands: observer removed during notify is not called again this dispatch") {
    CommandStack stack;
    std::vector<std::string> log;
    int count = 0;
    CommandStack::ObserverHandle handle = 0;
    handle = stack.AddObserver([&] {
        ++count;
        stack.RemoveObserver(handle);  // self-removal mid-notify must not iterate freed state
    });
    stack.PushExecuted(MakeRecordingCommand("a", &log));
    stack.PushExecuted(MakeRecordingCommand("b", &log));
    CHECK_EQ(count, 1);
}

RAVEN_TEST("commands: redo branch clears on compound seal, not on child pushes") {
    CommandStack stack;
    std::vector<std::string> log;
    stack.PushExecuted(MakeRecordingCommand("base", &log));
    stack.Undo();  // redo now holds "base"
    stack.BeginCompound("g");
    stack.PushExecuted(MakeRecordingCommand("child", &log));
    CHECK(stack.CanRedo());  // still redoable mid-compound
    stack.EndCompound();
    CHECK(!stack.CanRedo());  // sealed — redo branch cleared
}
