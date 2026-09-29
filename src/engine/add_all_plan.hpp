// add_all_plan.hpp - what ADD ALL picks, as one rule over plain rows
// (engine/stage3b-pre, docs/engine-stage3.md OPEN 5).
//
// ONE RULE, TWO CALLERS. The store window letters its ADD ALL key from it
// (gui::planAddAll, over the store model it draws), and the ENGINE runs the
// key from it (Engine::startAddAll, over its own catalogue and inventory) -
// so the words on the key and the run it starts cannot drift apart, and a
// headless engine can run ADD ALL with no window to ask. Moved here verbatim
// from gui/plugin_store_view.cpp (the rule and its sentences unchanged); a
// row carries only what the rule reads.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>
#include <vector>

#include "core/i18n.hpp"
#include "core/utf8_text.hpp"

namespace cascade::engine {

// One catalogue module, as the rule sees it.
struct AddAllRow {
    std::string name;
    bool fitted = false;
    bool hasUpdate = false;      // a newer build is planned for a fitted module
    bool hasNotice = false;      // the maker attached a legal notice
    // The same gate a single FIT goes through, asked as the module's own tick
    // stands, and asked as if its notice were acknowledged.
    std::string blockedReason;
    std::string blockedReasonIfAcknowledged;
};

// The catalogue as a whole.
struct AddAllCatalogue {
    bool haveCatalogue = false;
    bool listedNothing = false;    // read, and it listed no modules
    bool lastCheckFailed = false;  // asked, and no catalogue came back
    bool busy = false;             // a transfer or a fetch in flight
};

struct AddAllRule {
    // Indices into the rows, in catalogue order.
    std::vector<int> install;
    std::vector<int> update;
    // "NAME - reason", one per module the run will pass over.
    std::vector<std::string> skipped;
    // How many of `skipped` a notice alone holds back.
    int heldByNotice = 0;
    // The engraving on the key.
    std::string label;
    // Empty when the key may be pressed; a dead key always says why.
    std::string blockedReason;
};

inline AddAllRule planAddAllRows(const std::vector<AddAllRow>& rows, const AddAllCatalogue& cat,
                                 bool noticesAcknowledged) {
    using cascade::i18n::tr;
    AddAllRule plan;

    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const AddAllRow& sm = rows[static_cast<std::size_t>(i)];
        const std::string& name = sm.name;
        const std::string shown = name.empty() ? std::string(tr("(unnamed module)")) : name;
        if (sm.fitted) {
            // A FITTED MODULE IS ONLY EVER AN UPDATE HERE. It is never listed
            // as skipped: "already installed" is the outcome the user pressed
            // this key for, not a thing that went wrong, and a summary that
            // reported five of them as passed over would bury the one that
            // actually could not be fitted.
            if (sm.hasUpdate) { plan.update.push_back(i); }
            continue;
        }
        // THE SAME GATE A SINGLE FIT GOES THROUGH, asked of every row - with
        // the notice treated as acknowledged only when the user has ticked
        // the one box beside this key.
        const std::string& why =
            noticesAcknowledged ? sm.blockedReasonIfAcknowledged : sm.blockedReason;
        if (why.empty()) {
            plan.install.push_back(i);
            continue;
        }
        // HELD BY A NOTICE AND NOTHING ELSE is the one skip the user can undo
        // from this panel, so it is counted apart from the rest.
        if (sm.hasNotice && sm.blockedReasonIfAcknowledged.empty()) {
            ++plan.heldByNotice;
        }
        plan.skipped.push_back(shown + " - " + why);
    }

    const int n = static_cast<int>(plan.install.size());
    const int m = static_cast<int>(plan.update.size());
    std::string buf;
    // Singular and plural are whole keys, never an English "S" handed in by
    // %s: a translation has to be able to write its own plural.
    if (n > 0 && m > 0) {
        cascade::core::formatUtf8(buf,
                      n == 1 ? tr("ADD %d PLUGIN, UPDATE %d") : tr("ADD %d PLUGINS, UPDATE %d"),
                      n, m);
        plan.label = buf;
    } else if (n > 0) {
        // "ALL" ONLY WHEN IT REALLY IS ALL. A key engraved ADD ALL PLUGINS
        // that quietly passes over seven of them is the kind of copy this
        // window exists to refuse.
        if (plan.skipped.empty()) {
            plan.label = tr("ADD ALL PLUGINS");
        } else {
            cascade::core::formatUtf8(buf, n == 1 ? tr("ADD %d PLUGIN") : tr("ADD %d PLUGINS"),
                          n);
            plan.label = buf;
        }
    } else if (m > 0) {
        cascade::core::formatUtf8(buf, m == 1 ? tr("UPDATE %d PLUGIN") : tr("UPDATE %d PLUGINS"),
                      m);
        plan.label = buf;
    } else {
        plan.label = tr("ADD ALL PLUGINS");
    }

    // --- and why it may not be pressed --------------------------------------
    //
    // THE SAME FOUR CATALOGUE STATES the rest of the window distinguishes:
    // nobody has asked, it was asked and failed, it was asked and listed
    // nothing, or it was read. Telling a user whose check just failed to press
    // CHECK NOW is telling them to do again the thing that did not work.
    if (!cat.haveCatalogue) {
        if (cat.listedNothing) {
            plan.blockedReason = tr("the catalogue was read and it lists no modules at all");
        } else if (cat.lastCheckFailed) {
            plan.blockedReason =
                tr("the last check did not return a catalogue - its reason is under "
                   "CATALOGUE SOURCE");
        } else {
            plan.blockedReason =
                tr("no catalogue has been read yet - press CHECK NOW and this application "
                   "asks the source once");
        }
    } else if (cat.busy) {
        // One transfer at a time is what the downloader actually does, so a
        // second run started over the first would be two operations sharing
        // one progress bar and one CANCEL.
        plan.blockedReason = tr("a transfer is already in progress");
    } else if (n == 0 && m == 0) {
        plan.blockedReason =
            plan.skipped.empty()
                ? tr("every module in the catalogue is already fitted, and none has a "
                     "newer build")
                : tr("nothing in the catalogue can be fitted on this machine - each "
                     "module's own reason is on its row");
    }
    return plan;
}

}  // namespace cascade::engine
