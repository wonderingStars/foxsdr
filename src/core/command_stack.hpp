// command_stack.hpp - a generic UNDO / REDO STACK: commands made of redo and undo steps, a depth cap, groups, merging of repeated edits (a drag), and a
// "saved" mark. Nothing here knows about graphs; core/graph_edit.hpp builds the patch canvas's edits on it. Not wired.
//
// THE RULES (each tested):
//   - execute(label, redo, undo) RUNS redo() now; only when it returns true is the command recorded (a refused edit leaves no trace and does not
//     clear the redo list). A new recorded command discards everything that could have been redone.
//   - undo() runs the newest entry's undo steps in REVERSE order and moves it to the redo list; redo() runs its redo steps in order. Both return
//     false when there is nothing to do. A redo step that refuses (returns false) stops that redo and leaves the entry on the redo list: the stack
//     never claims a state it did not reach (with a linear history this does not happen unless the target changed behind the stack's back).
//   - GROUPS: beginGroup(label) .. endGroup() make everything executed in between ONE entry (nested groups join the outermost). A group in which
//     nothing was recorded leaves nothing. undo() and redo() are refused while a group is open.
//   - MERGING: execute(..., mergeKey) with a non-empty key, when the NEWEST entry is a single-step entry with the same key, no undo or redo has
//     happened since it was recorded and no group is open, REPLACES that entry's redo with the new one and keeps its original undo. Correct only for
//     steps that set an ABSOLUTE state (set the positions to X), which is what a drag is: one entry whose undo goes back to where the drag began.
//   - DEPTH CAP: at most `depth` entries are kept (1 .. 10000, default 100); the oldest is dropped when it is exceeded.
//   - SAVED MARK: markSaved() remembers the current position; isDirty() says whether the stack has moved off it (undoing back to it is clean again;
//     an entry dropped by the cap that was the saved position makes the stack permanently dirty).
// Single-threaded: the GUI thread owns it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace cascade::core::edit {

class CommandStack {
public:
    using RedoFn = std::function<bool()>;
    using UndoFn = std::function<void()>;

    static constexpr std::size_t kMinDepth = 1, kMaxDepth = 10000, kDefaultDepth = 100;

    explicit CommandStack(std::size_t depth = kDefaultDepth) { setDepth(depth); }

    void setDepth(std::size_t d) {
        depth_ = std::min(std::max(d, kMinDepth), kMaxDepth);
        trim();
    }
    std::size_t depth() const { return depth_; }

    // Runs redo; records the command when it returns true. Returns what redo returned.
    bool execute(const std::string& label, RedoFn redo, UndoFn undo, const std::string& mergeKey = std::string()) {
        if (!redo) { return false; }
        if (!redo()) { return false; }
        ++version_;
        if (groupDepth_ > 0) {
            group_.steps.push_back({std::move(redo), std::move(undo)});
            return true;
        }
        if (!mergeKey.empty() && canMerge_ && !done_.empty() && done_.back().mergeKey == mergeKey && done_.back().steps.size() == 1) {
            done_.back().steps[0].redo = std::move(redo);  // the original undo stays: it goes back to before the whole drag
            done_.back().serial = ++serialCounter_;        // the state moved: a saved mark on this entry no longer matches
            return true;
        }
        Entry e;
        e.label = label;
        e.mergeKey = mergeKey;
        e.serial = ++serialCounter_;
        e.steps.push_back({std::move(redo), std::move(undo)});
        commit(std::move(e));
        canMerge_ = !mergeKey.empty();
        return true;
    }

    void beginGroup(const std::string& label) {
        if (groupDepth_++ == 0) {
            group_ = Entry{};
            group_.label = label;
        }
    }
    // Closes the innermost group; the outermost one commits what was recorded in it. Returns true when an entry was committed.
    bool endGroup() {
        if (groupDepth_ == 0) { return false; }
        if (--groupDepth_ > 0) { return false; }
        if (group_.steps.empty()) { return false; }
        group_.serial = ++serialCounter_;
        commit(std::move(group_));
        group_ = Entry{};
        canMerge_ = false;
        return true;
    }
    bool inGroup() const { return groupDepth_ > 0; }

    bool undo() {
        if (groupDepth_ > 0 || done_.empty()) { return false; }
        Entry e = std::move(done_.back());
        done_.pop_back();
        for (std::size_t i = e.steps.size(); i-- > 0;) {
            if (e.steps[i].undo) { e.steps[i].undo(); }
        }
        undone_.push_back(std::move(e));
        canMerge_ = false;
        ++version_;
        return true;
    }
    bool redo() {
        if (groupDepth_ > 0 || undone_.empty()) { return false; }
        Entry& e = undone_.back();
        for (std::size_t i = 0; i < e.steps.size(); ++i) {
            if (!e.steps[i].redo()) {
                for (std::size_t k = i; k-- > 0;) {  // put back what this redo already did: the entry stays redo-able
                    if (e.steps[k].undo) { e.steps[k].undo(); }
                }
                return false;
            }
        }
        done_.push_back(std::move(e));
        undone_.pop_back();
        canMerge_ = false;
        ++version_;
        return true;
    }

    bool canUndo() const { return groupDepth_ == 0 && !done_.empty(); }
    bool canRedo() const { return groupDepth_ == 0 && !undone_.empty(); }
    std::size_t undoCount() const { return done_.size(); }
    std::size_t redoCount() const { return undone_.size(); }
    const std::string& undoLabel() const { return done_.empty() ? empty_ : done_.back().label; }
    const std::string& redoLabel() const { return undone_.empty() ? empty_ : undone_.back().label; }
    // Increases with every change of the stack (for a UI that redraws when it moves).
    std::uint64_t version() const { return version_; }

    void clear() {
        done_.clear();
        undone_.clear();
        group_ = Entry{};
        groupDepth_ = 0;
        canMerge_ = false;
        savedSerial_ = 0;
        savedValid_ = true;
        ++version_;
    }

    void markSaved() {
        savedSerial_ = done_.empty() ? 0 : done_.back().serial;
        savedValid_ = true;
    }
    bool isDirty() const {
        if (!savedValid_) { return true; }
        const std::uint64_t now = done_.empty() ? 0 : done_.back().serial;
        return now != savedSerial_;
    }

private:
    struct Step {
        RedoFn redo;
        UndoFn undo;
    };
    struct Entry {
        std::string label, mergeKey;
        std::vector<Step> steps;
        std::uint64_t serial = 0;
    };

    void commit(Entry&& e) {
        undone_.clear();
        // a saved position that was only reachable through the redo entries being discarded cannot be reached again
        if (savedSerial_ != 0 && (done_.empty() || done_.back().serial < savedSerial_)) { savedValid_ = false; }
        done_.push_back(std::move(e));
        trim();
    }
    void trim() {
        while (done_.size() > depth_) {
            // the saved position can no longer be reached when its entry (or the empty state) falls off the bottom
            if (savedSerial_ == 0 || done_.front().serial == savedSerial_) { savedValid_ = false; }
            done_.erase(done_.begin());
        }
    }

    std::size_t depth_ = kDefaultDepth;
    std::vector<Entry> done_, undone_;
    Entry group_;
    int groupDepth_ = 0;
    bool canMerge_ = false;
    std::uint64_t serialCounter_ = 0, savedSerial_ = 0, version_ = 0;
    bool savedValid_ = true;
    std::string empty_;
};

}  // namespace cascade::core::edit
