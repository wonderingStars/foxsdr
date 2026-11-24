// handover_sheet.hpp - WHO OWNS THE ONE HANDOVER SHEET RIGHT NOW (0.99.113 review fix S1).
//
// The new window has ONE sheet (the Handover global: title, text, two keys) and three users of it: the Legacy-window question (HandoverHost, "Switch" / "Stay here" and its one-key notes), the
// radio hand-off question (RadioMoveHost, "Move it here" / "Leave it there") and the refused-radio popup (RefusalPopup, "Open the Patch page" / "Close"). The last user to put its words on the
// sheet owns it, and the sheet's two keys go to the owner and to nobody else. Before this, the keys were routed by "is the move question open?" then "is the refusal popup open?", so a
// popup that another user had taken the sheet from stayed "open" and a later press of another user's key was answered by it.
//
// Rules (all plain C++ over std::functions, so tests/test_handover_sheet.cpp holds them with no window):
//  - took(who): the user has just shown its words. A refusal popup that loses the sheet to anyone else is told so (refusalLost): it is no longer open and none of its keys fires later.
//  - released(who): the user put the sheet away; true when it was the owner or nobody owned it (the caller then closes the sheet; a user that no longer owns it must not close another user's words).
//  - mayRaiseRefusal(): false while the hand-off question or the Legacy question owns the sheet; the refusal popup never takes the sheet from either of them.
//  - go() / stay(): the owner's handler; with no owner the Legacy handler, as before (it is the user of the sheet's one-key notes).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <functional>

namespace foxsdr::slinthost {

class HandoverSheet {
public:
    enum class Who { None, Move, Legacy, Refusal };
    struct Users {
        std::function<void()> moveGo, moveStay;
        std::function<void()> legacyGo, legacyStay;
        std::function<void()> refusalGo, refusalStay;
        std::function<void()> refusalLost;   // the refusal popup was open and another user has taken the sheet
    };
    void setUsers(Users u) { users_ = std::move(u); }

    void took(Who who) {
        if (who_ == Who::Refusal && who != Who::Refusal && users_.refusalLost) { users_.refusalLost(); }
        who_ = who;
    }
    bool released(Who who) {
        if (who_ != who && who_ != Who::None) { return false; }
        who_ = Who::None;
        return true;
    }
    bool mayRaiseRefusal() const { return who_ != Who::Move && who_ != Who::Legacy; }
    Who owner() const { return who_; }

    void go() { call(who_ == Who::Move ? users_.moveGo : who_ == Who::Refusal ? users_.refusalGo : users_.legacyGo); }
    void stay() { call(who_ == Who::Move ? users_.moveStay : who_ == Who::Refusal ? users_.refusalStay : users_.legacyStay); }

private:
    static void call(const std::function<void()>& f) { if (f) { f(); } }
    Users users_;
    Who who_ = Who::None;
};

}  // namespace foxsdr::slinthost
