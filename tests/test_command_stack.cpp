// The generic undo / redo stack (src/core/command_stack.hpp): recording, refusal, order, groups, merging, the depth cap and the saved mark.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>
#include <vector>

#include "core/command_stack.hpp"
#include "test_check.hpp"

using cascade::core::edit::CommandStack;

namespace {

// A value the commands act on, with a log of what ran in what order.
struct Box {
    int v = 0;
    std::vector<std::string> log;
};

bool set(CommandStack& s, Box& b, int to, const std::string& key = std::string()) {
    const int before = b.v;
    return s.execute(
        "set " + std::to_string(to),
        [&b, to]() {
            b.v = to;
            b.log.push_back("do" + std::to_string(to));
            return true;
        },
        [&b, before]() {
            b.v = before;
            b.log.push_back("undo" + std::to_string(before));
        },
        key);
}

void testBasic() {
    CommandStack s;
    Box b;
    CHECK(!s.canUndo() && !s.canRedo() && !s.undo() && !s.redo());
    CHECK(set(s, b, 1) && set(s, b, 2) && set(s, b, 3));
    CHECK(b.v == 3 && s.undoCount() == 3 && s.undoLabel() == "set 3");
    CHECK(s.undo() && b.v == 2 && s.redoCount() == 1 && s.redoLabel() == "set 3");
    CHECK(s.undo() && s.undo() && b.v == 0 && !s.undo());
    CHECK(s.redo() && s.redo() && s.redo() && b.v == 3 && !s.redo());
    // A new command discards the redo list.
    s.undo();
    s.undo();
    CHECK(s.canRedo());
    CHECK(set(s, b, 9));
    CHECK(!s.canRedo() && b.v == 9 && s.undoCount() == 2);
    CHECK(s.undo() && b.v == 1);
    // A refused command leaves no trace and does NOT clear the redo list.
    CHECK(s.canRedo());
    CHECK(!s.execute("refused", []() { return false; }, []() {}));
    CHECK(s.canRedo() && s.undoCount() == 1);
    CHECK(!s.execute("no redo function", CommandStack::RedoFn(), []() {}));
    // Version moves with every change.
    const auto v0 = s.version();
    s.redo();
    CHECK(s.version() > v0);
    s.clear();
    CHECK(!s.canUndo() && !s.canRedo());
}

void testGroups() {
    CommandStack s;
    Box b;
    s.beginGroup("three");
    set(s, b, 1);
    set(s, b, 2);
    s.beginGroup("inner");  // nested groups join the outermost
    set(s, b, 3);
    CHECK(!s.endGroup());
    CHECK(s.inGroup());
    CHECK(!s.undo() && !s.redo() && !s.canUndo());  // refused while open
    CHECK(s.endGroup());
    CHECK(!s.inGroup() && s.undoCount() == 1 && s.undoLabel() == "three");
    b.log.clear();
    CHECK(s.undo());
    CHECK(b.v == 0);
    CHECK((b.log == std::vector<std::string>{"undo2", "undo1", "undo0"}));  // reverse order
    b.log.clear();
    CHECK(s.redo());
    CHECK((b.log == std::vector<std::string>{"do1", "do2", "do3"}));  // forward order
    // An empty group, and a group in which every command was refused, leave nothing.
    s.beginGroup("empty");
    CHECK(!s.endGroup());
    s.beginGroup("refused");
    s.execute("x", []() { return false; }, []() {});
    CHECK(!s.endGroup());
    CHECK(s.undoCount() == 1);
    CHECK(!s.endGroup());  // no group open
    // A redo step that refuses part way stops there, puts back what it did and keeps the entry redo-able.
    CommandStack t;
    int a = 0, c = 0;
    bool allow = true;
    t.beginGroup("two steps");
    t.execute("a", [&]() { a = 1; return true; }, [&]() { a = 0; });
    t.execute("c", [&]() { if (!allow) { return false; } c = 1; return true; }, [&]() { c = 0; });
    t.endGroup();
    t.undo();
    CHECK(a == 0 && c == 0);
    allow = false;
    CHECK(!t.redo());
    CHECK(a == 0 && c == 0 && t.redoCount() == 1 && t.undoCount() == 0);  // nothing half done
    allow = true;
    CHECK(t.redo() && a == 1 && c == 1);
}

void testMerge() {
    CommandStack s;
    Box b;
    set(s, b, 1);
    set(s, b, 10, "drag");
    set(s, b, 11, "drag");
    set(s, b, 12, "drag");
    CHECK(b.v == 12 && s.undoCount() == 2);  // the drag is one entry
    CHECK(s.undo() && b.v == 1);              // back to where the drag began
    CHECK(s.redo() && b.v == 12);             // forward to where it ended
    set(s, b, 20, "other");                   // another key does not merge
    CHECK(s.undoCount() == 3);
    s.undo();
    set(s, b, 13, "drag");                    // after an undo nothing merges
    CHECK(s.undoCount() == 3);
    // A group is never merged into, and an empty key never merges.
    set(s, b, 14);
    set(s, b, 15);
    CHECK(s.undoCount() == 5);
    s.beginGroup("g");
    set(s, b, 16, "drag");
    set(s, b, 17, "drag");
    s.endGroup();
    CHECK(s.undoCount() == 6);
}

void testDepthCap() {
    CommandStack s(5);
    Box b;
    for (int i = 1; i <= 20; ++i) { set(s, b, i); }
    CHECK(s.undoCount() == 5 && b.v == 20);
    int n = 0;
    while (s.undo()) { ++n; }
    CHECK(n == 5 && b.v == 15);  // the 15 older commands are gone: the state they made is the new bottom
    while (s.redo()) {}
    CHECK(b.v == 20);
    s.setDepth(2);
    CHECK(s.undoCount() == 2);
    s.setDepth(0);
    CHECK(s.depth() == CommandStack::kMinDepth);
    s.setDepth(1000000);
    CHECK(s.depth() == CommandStack::kMaxDepth);
    // Grouped entries count as one against the cap.
    CommandStack g(3);
    for (int i = 0; i < 10; ++i) {
        g.beginGroup("g");
        set(g, b, i);
        set(g, b, i + 100);
        g.endGroup();
    }
    CHECK(g.undoCount() == 3);
}

void testSavedMark() {
    CommandStack s;
    Box b;
    CHECK(!s.isDirty());
    set(s, b, 1);
    CHECK(s.isDirty());
    s.markSaved();
    CHECK(!s.isDirty());
    set(s, b, 2);
    CHECK(s.isDirty());
    s.undo();
    CHECK(!s.isDirty());  // back on the saved position
    s.redo();
    CHECK(s.isDirty());
    s.undo();
    // Saved at "after set 1"; undo it (now before), then do something else: the saved position was only reachable through the redo list that is
    // discarded, so the stack is dirty for good, even with the same number of entries.
    s.undo();
    CHECK(s.isDirty());
    set(s, b, 7);
    CHECK(s.isDirty());
    s.markSaved();
    CHECK(!s.isDirty());
    // The empty state as the saved one.
    CommandStack e;
    e.markSaved();
    set(e, b, 1);
    CHECK(e.isDirty());
    e.undo();
    CHECK(!e.isDirty());
    // A merged drag moves the state: a mark on the entry before the merge no longer matches.
    CommandStack m;
    set(m, b, 1, "drag");
    m.markSaved();
    set(m, b, 2, "drag");
    CHECK(m.isDirty());
    // The saved entry falling off the bottom makes it dirty for good.
    CommandStack c(2);
    set(c, b, 1);
    c.markSaved();
    set(c, b, 2);
    CHECK(c.isDirty());
    c.undo();
    CHECK(!c.isDirty());
    c.redo();
    set(c, b, 3);
    set(c, b, 4);  // the entry marked saved has been dropped
    c.undo();
    c.undo();
    CHECK(c.isDirty());
    // The saved empty state is lost once anything is dropped.
    CommandStack d(2);
    d.markSaved();
    set(d, b, 1);
    set(d, b, 2);
    set(d, b, 3);
    while (d.undo()) {}
    CHECK(d.isDirty());
    // clear() makes the current state the saved one.
    d.clear();
    CHECK(!d.isDirty());
}

}  // namespace

int main() {
    testBasic();
    testGroups();
    testMerge();
    testDepthCap();
    testSavedMark();
    return testSummary("test_command_stack");
}
