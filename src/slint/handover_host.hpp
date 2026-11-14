// handover_host.hpp - THE NEW WINDOW'S SIDE OF "LEGACY IS A CHOICE" (engine fixes 7; the rules and the order are src/core/window_handover.hpp's).
//
// The Layouts menu's "Legacy" entry calls choose(). It checks that the Legacy program is there (the window stays where it is, with a sentence, when it is not), asks when a recording or a
// patch is running, and otherwise ends the window's run in the ordinary way: it hands the request to the HandoverAtExit object that main() declares FIRST, and quits the event loop. The
// launch itself happens when main() is gone (core::handover::HandoverAtExit). A window that was started BY a hand-over shows the sentence the other window left (noteSentence).
//
// Everything here is plain C++ over three std::functions, so tests/test_slint_host_handover.cpp holds it with no window.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <functional>
#include <string>

#include "core/window_handover.hpp"

namespace foxsdr::slinthost {

class HandoverHost {
public:
    struct Sheet {
        std::string title, text, goLabel, stayLabel;   // goLabel empty: a sentence with nothing to decide
    };
    struct Hooks {
        std::function<cascade::core::handover::Busy()> busy;   // what is running now
        std::function<void()> quit;                            // ends the event loop
        std::function<void(const Sheet&)> show;                // opens the sheet
        std::function<void()> hide;                            // closes it
        std::function<std::string(const char*)> translate;     // the translation call (a test passes the identity)
    };

    HandoverHost(Hooks hooks, cascade::core::handover::HandoverAtExit& atExit, std::string configPath, bool safePlugins)
        : hooks_(std::move(hooks)), atExit_(atExit), configPath_(std::move(configPath)), safePlugins_(safePlugins) {}

    // The menu entry (or the seam). `askAgain` false: the person already answered the question.
    void choose() {
        if (chosen_) { return; }
        namespace h = cascade::core::handover;
        const std::string missing = h::preflight(h::Window::Legacy);
        if (!missing.empty()) {
            show(missing, "");   // stays: the sentence, one key
            return;
        }
        const h::Busy b = hooks_.busy ? hooks_.busy() : h::Busy{};
        if (b.any()) {
            asking_ = true;
            Sheet s;
            s.title = tr("Switch to the Legacy window?");
            s.text = h::busyQuestion(b, h::Window::Legacy);
            s.goLabel = tr("Switch");
            s.stayLabel = tr("Stay here");
            hooks_.show(s);
            return;
        }
        go();
    }
    // The sheet's two keys.
    void confirm() {
        hide();
        if (asking_) {
            asking_ = false;
            go();
        }
    }
    void cancel() {
        asking_ = false;
        hide();
    }
    // A sentence another window left on this one at start (the other program was not found or would not start).
    void showNote(const std::string& sentence) {
        if (!sentence.empty()) { show(sentence, ""); }
    }
    bool chosen() const { return chosen_; }

private:
    std::string tr(const char* s) const { return hooks_.translate ? hooks_.translate(s) : std::string(s); }
    void show(const std::string& text, const std::string& go) {
        Sheet s;
        s.title = tr("Switching windows");
        s.text = text;
        s.goLabel = go;
        s.stayLabel = tr("OK");
        hooks_.show(s);
    }
    void hide() {
        if (hooks_.hide) { hooks_.hide(); }
    }
    void go() {
        namespace h = cascade::core::handover;
        chosen_ = true;
        h::Request r;
        r.from = h::Window::New;
        r.target = h::Window::Legacy;
        r.configPath = configPath_;
        r.safePlugins = safePlugins_;
        atExit_.set(r);
        if (hooks_.quit) { hooks_.quit(); }
    }

    Hooks hooks_;
    cascade::core::handover::HandoverAtExit& atExit_;
    std::string configPath_;
    bool safePlugins_ = false;
    bool chosen_ = false;
    bool asking_ = false;
};

}  // namespace foxsdr::slinthost
