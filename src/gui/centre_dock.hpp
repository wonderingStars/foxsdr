// centre_dock.hpp - WHICH WINDOW THE CENTRE OF A TABLET'S SCREEN IS SHOWING.
//
// WHY THIS EXISTS. On the desktop a plugin's window, an instrument face or a
// map is a PAGE: a cabinet with its own rail and keys that floats over the
// main window and can be torn off onto a second monitor (AppWindow::beginPage,
// and gui/viewport_policy.hpp for the one machine that could not). A tablet
// has one screen, no second monitor and no window manager to tear anything off
// to, so a page opened there lands ON TOP of the spectrum it was opened from
// and hides the radio: the owner's instruction was that "on Android a plugin
// should occupy the waterfall area when it loads so everything stays on one
// screen".
//
// So the centre panel becomes a row of keys - SPECTRUM, then one per open
// window - and the panel under them shows whichever is selected: the spectrum
// and waterfall as they always were, or the window's own body drawn into the
// waterfall's area with the spectrum collapsed to a tunable strip above it.
// Nothing about the top plate, the FUNCTION SELECT rail or the status column
// changes, and NOTHING ON THE DESKTOP CHANGES AT ALL - there the pages keep
// floating and tearing off, and this class is compiled but never asked
// anything (AppWindow::kCentreDockPresentation).
//
// WHY THE STATE IS A CLASS OF ITS OWN, with no ImGui in it. Every question it
// answers is a question about a SET OF NAMES and a CLOCK - which windows are
// open, which one is selected, which one a second tap just landed on, what
// happens to the selection when the selected window closes - and every one of
// them is answerable, and therefore checkable, without a frame, a screen or a
// device. tests/test_centre_dock.cpp pins the lot. The same split as
// gui/page_geometry.hpp and gui/rail_banks.hpp: WHAT is decided here, HOW it is
// drawn stays in app_window.cpp.
//
// THE TAB LIST IS NOT MAINTAINED - IT IS OBSERVED. A window in this
// application is drawn by a call site that has already decided it is open
// (`if (!pluginWindows_.shown(id)) continue;`, `if (!page.open) continue;`,
// `if (!demodScopeOpen_) return;`), so the set of windows that ASK to be drawn
// in a frame IS the set of open windows, and it needs no second registry to
// drift from. Each frame the pages offer themselves between beginFrame() and
// endFrame(); endFrame() publishes what was offered as the tab row. A window
// that stops being drawn - closed from its own key, hidden by the rail, its
// plugin removed - simply stops offering, and its tab goes with it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_CENTRE_DOCK_HPP
#define CASCADE_GUI_CENTRE_DOCK_HPP

#include <cstddef>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace cascade::gui {

// HOW CLOSE TWO TAPS ON ONE TAB HAVE TO BE TO COUNT AS A DOUBLE TAP.
//
// 300 ms is Android's own figure (ViewConfiguration's double-tap timeout), and
// the gesture is worth having because a map docked under a quarter-height
// spectrum strip is still a keyhole: the double tap gives the tab's window the
// whole centre panel and a second one gives the strip back. It is deliberately
// NOT ImGui's mouse double-click - that fires on the window under the pointer
// and these keys are drawn by hand into a draw list, so the timing is kept
// here where it can be tested with a clock that is just a number.
inline constexpr double kDockDoubleTapSeconds = 0.30;

// One tab: the ImGui identity of the window it stands for, and the word on the
// key. The title is the RAIL NAME the page already carries (beginPage's
// `title`), which every call site has already upper-cased, so a tab cannot
// letter a window differently from the page it replaces.
struct DockTab {
    std::string id;
    std::string title;
};

class CentreDock {
public:
    // --- the frame's gathering pass -----------------------------------------

    // Start collecting. Anything offered before this in a frame is lost, which
    // is the wanted reading: the pass is the frame's whole account of what is
    // open.
    void beginFrame() {
        pending_.clear();
        gathering_ = true;
    }

    // One page asking to be drawn. Called from the page path itself, so a
    // window that is skipped never offers. Repeat offers of one id in a frame
    // are folded (a defensive rule: two pages with one identity would collide
    // in ImGui long before they collided here).
    void offer(std::string id, std::string title) {
        if (!gathering_ || id.empty()) { return; }
        for (const DockTab& t : pending_) {
            if (t.id == id) { return; }
        }
        pending_.push_back(DockTab{std::move(id), std::move(title)});
    }

    // Publish the pass. Three things settle here and all three are the point:
    //
    //   A WINDOW THAT HAS JUST OPENED BECOMES THE SELECTED TAB. That is the
    //   whole of requirement "opening a window on Android makes it the active
    //   tab", and it is decided HERE rather than at the dozen call sites that
    //   can open one (the rail's rows, a plugin preset, the DEMOD SCOPE key,
    //   a demonstration instrument, a restored window at startup) - none of
    //   which has to know the dock exists. New means "was not in the last
    //   published list": the FIRST such in draw order wins, so two windows
    //   opened by one preset press land on a defined tab rather than on
    //   whichever the loop happened to reach last.
    //
    //   A SELECTION THAT IS NO LONGER OPEN FALLS BACK TO THE SPECTRUM. A
    //   window can go without anyone pressing its close key - the plugin was
    //   removed, a rescan took it, the rail hid it - and a dock left pointing
    //   at it would show an empty panel with no way back.
    //
    //   WHAT IS REMEMBERED ABOUT A WINDOW DIES WITH IT: the full-height flag
    //   and any unconsumed close request are dropped, so a window re-opened
    //   later starts at the strip like any other rather than at whatever it
    //   was left at an hour ago.
    void endFrame() {
        if (!gathering_) { return; }
        gathering_ = false;
        for (const DockTab& t : pending_) {
            if (!known(tabs_, t.id)) {
                active_ = t.id;
                break;
            }
        }
        tabs_ = pending_;
        pending_.clear();
        prune(full_);
        prune(closing_);
        if (!active_.empty() && !known(tabs_, active_)) { active_.clear(); }
    }

    // --- what the frame draws ------------------------------------------------

    const std::vector<DockTab>& tabs() const { return tabs_; }
    // The SPECTRUM tab is not in tabs(): it is always there, always first, and
    // is selected exactly when no window is. Kept as an empty id rather than a
    // reserved name so no plugin can ever publish a window that collides with
    // it.
    bool spectrumActive() const { return active_.empty(); }
    const std::string& activeId() const { return active_; }
    bool isActive(const std::string& id) const { return !id.empty() && id == active_; }

    // Whether the selected window has been given the whole centre panel, with
    // the spectrum strip hidden. A per-tab flag, not a mode: a map wants the
    // height and a pager does not, and switching between them must not carry
    // one window's choice onto another.
    bool fullHeight() const { return fullHeight(active_); }
    bool fullHeight(const std::string& id) const {
        if (id.empty()) { return false; }
        const auto it = full_.find(id);
        return it != full_.end() && it->second;
    }

    // --- what a hand does ----------------------------------------------------

    // The SPECTRUM key: the radio back, every tab kept.
    void showSpectrum() {
        active_.clear();
        lastTapId_.clear();
    }

    // A tab key. The first tap selects; a second tap on the SAME tab within
    // kDockDoubleTapSeconds toggles that tab's full height. The tap clock is
    // then reset rather than moved on, so three taps are one selection and one
    // toggle - a finger resting on a key must not flip the layout twice.
    //
    // `nowSec` is the caller's clock (ImGui::GetTime()), passed in for the
    // reason everything else here takes its inputs: a timing rule with its own
    // clock inside it cannot be tested.
    void tap(const std::string& id, double nowSec) {
        if (id.empty()) { return; }
        const bool again = (id == lastTapId_) && (nowSec - lastTapSec_ <= kDockDoubleTapSeconds);
        active_ = id;
        if (again) {
            const auto it = full_.find(id);
            const bool was = (it != full_.end()) && it->second;
            full_[id] = !was;
            lastTapId_.clear();
            lastTapSec_ = kNoTap;
            return;
        }
        lastTapId_ = id;
        lastTapSec_ = nowSec;
    }

    // The close key in the tab bar. It does NOT hide the window itself: the
    // page path owns that, and it already has one way of doing it (beginPage
    // clears the caller's `open` flag and the call site hides the window on
    // the way out), so the request is recorded here and consumed by that same
    // path on the next frame. One close route, not two, which is what keeps a
    // docked window and a floating page closing identically.
    //
    // The SELECTION moves back to the spectrum at once, though, rather than on
    // the frame the window actually goes: a key that visibly does nothing for
    // a frame reads as a key that missed.
    void requestClose(const std::string& id) {
        if (id.empty()) { return; }
        closing_.insert(id);
        if (active_ == id) { active_.clear(); }
    }

    // Consumed by the page path: true exactly once per request.
    bool takeCloseRequest(const std::string& id) { return closing_.erase(id) != 0u; }

    // For the tests and for a caller that wants to know whether anything is
    // pending without consuming it.
    bool closePending(const std::string& id) const { return closing_.count(id) != 0u; }
    std::size_t tabCount() const { return tabs_.size(); }

private:
    static constexpr double kNoTap = -1.0e9;

    static bool known(const std::vector<DockTab>& v, const std::string& id) {
        for (const DockTab& t : v) {
            if (t.id == id) { return true; }
        }
        return false;
    }

    // Drop anything remembered about a window that is no longer open. Written
    // twice over two container shapes rather than once over a template,
    // because two overloads read plainly and a template here would not.
    void prune(std::map<std::string, bool>& m) const {
        for (auto it = m.begin(); it != m.end();) {
            it = known(tabs_, it->first) ? std::next(it) : m.erase(it);
        }
    }
    void prune(std::set<std::string>& s) const {
        for (auto it = s.begin(); it != s.end();) {
            it = known(tabs_, *it) ? std::next(it) : s.erase(it);
        }
    }

    std::vector<DockTab> tabs_;     // published: what the tab row draws
    std::vector<DockTab> pending_;  // being gathered this frame
    std::string active_;            // empty = SPECTRUM
    std::map<std::string, bool> full_;
    std::set<std::string> closing_;
    std::string lastTapId_;
    double lastTapSec_ = kNoTap;
    bool gathering_ = false;
};

}  // namespace cascade::gui

#endif  // CASCADE_GUI_CENTRE_DOCK_HPP
