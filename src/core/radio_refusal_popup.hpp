// radio_refusal_popup.hpp - THE POPUP for a refused radio choice that a Radio part of the patch explains (0.99.113): its words, and the small state machine both windows drive.
//
// The sentence on the Source line and the data a refusal carries are core/radio_refusal.hpp's. This is the second thing a person is told, only when the radio is refused BECAUSE A RADIO PART OF
// THE PATCH NAMES IT: a notice with two keys, "Open the Patch page" and "Close". It is a notice and nothing else: it never answers the hand-off question, never counts a failed open or files a
// report, and no key changes the patch, a part or who holds the radio.
//
// The decision "may the popup be shown at all" is ONE function in each window (mayShowRefusalPopup), so a remote feature word can switch it off later.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_RADIO_REFUSAL_POPUP_HPP
#define CASCADE_CORE_RADIO_REFUSAL_POPUP_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "core/i18n.hpp"
#include "core/radio_refusal.hpp"
#include "core/utf8_text.hpp"
#include "foxsdr_api.h"

namespace cascade::core::refusal {

// A refused SELECT_SOURCE / RADIO_ADD answer, read for its holder: true (and the number) only for a refusal BECAUSE A RADIO PART OF THE PATCH NAMES THE RADIO.
inline bool isPatchPartRefusal(const FoxCommandResult& r, int& part) {
    if ((r.op != FOXAPI_OP_SELECT_SOURCE && r.op != FOXAPI_OP_RADIO_ADD) || r.status != FOXAPI_BUSY || (r.flags & FOXAPI_RESULT_REFUSED) == 0u) { return false; }
    if (static_cast<int>(r.applied[0]) != kHolderPatchPart) { return false; }
    part = static_cast<int>(r.applied[1]);
    return true;
}

// The popup's words. The window supplies the radio's screen label (the row it clicked) and the Radio part's caption as the Patch page shows it (see partCaptionFor). TWO WHOLE-SENTENCE KEYS, never a
// fragment glued in front of a sentence: with a label, "<label> is set as ..."; without one, the Source line's own sentence (a language may order the words as it must in each).
struct Popup {
    std::string title, text, goLabel, stayLabel;
};
inline Popup popupFor(const std::string& radioLabel, const std::string& partCaption) {
    Popup p;
    p.title = i18n::tr("Radio in use");
    if (radioLabel.empty()) {
        p.text = sourceSentence(kHolderPatchPart, 0, partCaption);
    } else {
        p.text = formatText(i18n::tr("%s is set as the radio of the part \"%s\" on the Patch page. Change or remove that part, or start the patch to use the radio there."), radioLabel.c_str(),
                            partCaptionFor(partCaption).c_str());
    }
    p.goLabel = i18n::tr("Open the Patch page");
    p.stayLabel = i18n::tr("Close");
    return p;
}

// THE POPUP'S STATE, one per window. A window tells it which tickets it submitted for a radio choice (noteSubmitted, with the label of the row it clicked) and hands it
// every answer its own session polls (onAnswer). Only an answer to a ticket THIS window noted, that is a patch-part refusal, opens the popup: an answer to another
// session's command never arrives here, and an answer to any ticket not noted (a script, a remote client) is not this person's click. One popup per refused click.
class RefusalPopup {
public:
    struct Hooks {
        std::function<bool()> mayShow;                   // the window's ONE decision: may the popup be shown at all (mayShowRefusalPopup)
        std::function<void(const Popup&)> show;          // draw it
        std::function<void()> hide;                      // put it away
        std::function<void()> openPatchPage;             // the window's own route to the Patch page
        std::function<std::string(int part)> partCaption;   // the caption the Patch page shows on that part (looked up by the part's node id in the window's own draft); empty when it cannot say
    };
    explicit RefusalPopup(Hooks hooks) : hooks_(std::move(hooks)) {}

    void noteSubmitted(std::uint64_t ticket, const std::string& radioLabel) {
        if (ticket == 0u) { return; }
        if (asked_.size() > 64u) { asked_.clear(); }   // bounded: an answer that never comes is not kept for ever
        asked_[ticket] = radioLabel;
    }
    // True when this answer was one of the window's own choices (so a caller may stop looking for another owner of it).
    bool onAnswer(const FoxCommandResult& r) {
        const auto it = asked_.find(r.ticket);
        if (it == asked_.end()) { return false; }
        const std::string label = it->second;
        asked_.erase(it);
        int part = 0;
        if (!isPatchPartRefusal(r, part)) { return true; }
        if (hooks_.mayShow && !hooks_.mayShow()) { return true; }
        open_ = true;
        part_ = part;
        ++shown_;
        if (hooks_.show) { hooks_.show(popupFor(label, hooks_.partCaption ? hooks_.partCaption(part) : std::string())); }
        return true;
    }
    // The popup's two keys. "Open the Patch page" shows the page and closes the popup; "Close" only closes it. Neither changes the patch, the part or who holds the radio.
    void goToPatchPage() {
        if (!open_) { return; }
        close();
        if (hooks_.openPatchPage) { hooks_.openPatchPage(); }
    }
    void dismiss() {
        if (open_) { close(); }
    }
    // Another user of the window's one sheet (a hand-off or Legacy question) has taken it over: this popup is no longer open, and neither key of it fires later. Nothing is hidden: the sheet is theirs now.
    void superseded() { open_ = false; }
    bool open() const { return open_; }
    int part() const { return part_; }
    int shown() const { return shown_; }   // how many popups this window has raised (a test counts them)

private:
    void close() {
        open_ = false;
        if (hooks_.hide) { hooks_.hide(); }
    }
    Hooks hooks_;
    std::map<std::uint64_t, std::string> asked_;
    bool open_ = false;
    int part_ = 0;
    int shown_ = 0;
};

}  // namespace cascade::core::refusal

#endif  // CASCADE_CORE_RADIO_REFUSAL_POPUP_HPP
