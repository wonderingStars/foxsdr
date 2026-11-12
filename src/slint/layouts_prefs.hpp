// layouts_prefs.hpp - ONE-CLICK LAYOUTS (docs/multi-radio-records/screen-s1.md, lane S1 item 2; design D:/foxsdr-build/prep/pre-release-features-design.md section 12).
//
// A LAYOUT is the arrangement of the window: the bank and which of its sections are open, whether the rail is folded, the tab (RECEIVER / PATCH / SCOPE), the split between the spectrum and the
// waterfall, the radio workspace's layout (One / Side by side / 2 x 2) with Link, the radio that is selected, the 3D waterfall, and - only when the user ticked "Include tuning" - the frequency and mode.
// A LAYOUT NEVER starts or stops a radio, a recording or a decoder, and never changes a frequency unless it carries one.
//
// WITH SEVERAL RADIOS a layout captures the workspace layout (One / Side by side / 2 x 2), Link and which radio is selected; it does not capture which radios are open, their sources, their
// frequencies (unless "Include tuning", which is the SELECTED radio's) or their pages. A layout that wants more panes than there are radios shows what the workspace can show (the S5 rule: the
// chosen layout is kept and comes back when the radios do); a saved selection of a radio that is not open is skipped.
//
// WHERE THEY ARE SAVED. In the interface half's one text (AppConfig::view3d), as the raw entry `layouts` (view3d_prefs.hpp keeps it verbatim): `layouts:n=Airband,b=2,s=16,...|n=Mine,...`. Only the user's
// own layouts are written; the two stock layouts are code. Reading is lenient (an unknown key or a bad number is skipped, every value is clamped), so a file from a newer build reads and an
// older file (no entry) loads with the two stock layouts and none of the user's.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace foxsdr::slintlayouts {

constexpr std::size_t kMaxUserLayouts = 12;
constexpr std::size_t kMaxNameBytes = 24;

struct Layout {
    std::string name;
    bool builtin = false;
    int bank = 0;                    // 0 SIGNAL, 1 DECODE, 2 TOOLS, 3 EXTEND, 4 SYSTEM
    std::uint32_t sections = 0;      // bit i = section i of that bank is open
    bool railFolded = false;
    int view = 0;                    // 0 RECEIVER, 1 PATCH, 2 SCOPE (a plugin page is not part of a layout)
    int splitPct = 45;               // the spectrum's share of the plot column, percent 10..90
    int workspace = 0;               // 0 One, 1 Side by side, 2 Four
    bool link = false;
    bool wf3d = false;
    int selected = 0;                // radio 1..8, 0 = leave the selection alone
    bool tuning = false;             // "Include tuning"
    std::int64_t tuneHz = 0;
    int mode = 0;                    // the strip's mode index 0..7
    bool operator==(const Layout&) const = default;
};

// A name as the user may keep it: control characters and the settings text's separators (; | , = : ~) become spaces, runs of spaces and the ends are trimmed, at most kMaxNameBytes bytes
// (never in the middle of a UTF-8 sequence). "" = no usable name.
std::string cleanName(const std::string& raw);

// The two stock layouts: Airband (the TOOLS bank with the Airband monitor open, side by side) and HF DX (the SIGNAL bank with Source and Radio open, a wide waterfall).
const std::vector<Layout>& builtins();

// The user's layouts as the entry's body (no name, no ';'), and back. Same list, same text.
std::string body(const std::vector<Layout>& user);
std::vector<Layout> parse(const std::string& body);

enum class SaveResult { Saved, Replaced, BadName, ReservedName, Full };

// The user's layouts in use: loaded from the interface half's text, changed by save / rename / remove, written back into that text.
class LayoutBook {
public:
    void load(const std::string& view3dText);
    const std::vector<Layout>& user() const { return user_; }
    // builtins first, then the user's (the order the menu shows them)
    std::vector<Layout> all() const;
    const Layout* find(const std::string& name) const;       // exact, case-insensitive; built-ins too
    SaveResult save(const std::string& rawName, Layout layout);   // the same name (any case) replaces; a stock layout's name is refused
    SaveResult rename(const std::string& from, const std::string& rawTo);   // Saved; BadName (nothing usable, or no such layout); ReservedName (a stock layout's or another layout's name)
    bool remove(const std::string& name);                    // a user layout only
    bool dirty() const { return dirty_; }
    void markWritten() { dirty_ = false; }
    std::string text(const std::string& view3dText) const;   // the text with the layouts in it, every other entry untouched

private:
    std::vector<Layout> user_;
    bool dirty_ = false;
};

// THE ORDER A LAYOUT IS PUT BACK IN. The bank before its sections (the sections are the bank's), the tab before the split (a split is the receiver tab's), the workspace layout before the
// selection (a selection is a radio of the page that layout shows), the tuning last. The host performs the steps in this order and nothing else.
enum class Step { Bank, Sections, Rail, View, Split, Workspace, Link, Selection, Wf3d, Tuning };
std::vector<Step> stepsOf(const Layout& l);

// WHAT THE WINDOW IS NOW, and what a layout may do to it. `Window` is the host's reads; `Actions` is the ONLY thing a layout can reach: there is no member that starts or stops a radio, a recording or a
// decoder, a patch or a scan, so applying a layout cannot do any of them (test_slint_host_layouts holds the list of calls). The host fills both from its own state and its own setters.
struct Window {
    int bank = 0;
    std::uint32_t sections = 0;       // the open sections of `bank`
    bool railFolded = false;
    int view = 0;                     // 0 RECEIVER, 1 PATCH, 2 SCOPE, 3 and up a plugin page
    int splitPct = 45;
    int workspace = 0;
    bool link = false;
    bool wf3d = false;
    int selected = 1;                 // the selected radio, 1-based
    std::uint32_t openRadios = 1;     // bit i = radio i + 1 is open
    std::int64_t tunedHz = 0;
    int mode = 0;
    bool patchRunning = false;        // leaving the PATCH tab stops a running patch, so a layout does not leave it
};
struct Actions {
    std::function<void(int)> setBank;
    std::function<void(int bank, std::uint32_t mask)> setSections;
    std::function<void(bool)> setRail;
    std::function<void(int)> setView;
    std::function<void(int)> setSplitPct;
    std::function<void(int)> setWorkspace;
    std::function<void(bool)> setLink;
    std::function<void(int radio)> selectRadio;
    std::function<void(bool)> setWf3d;
    std::function<void(std::int64_t hz)> tune;
    std::function<void(int mode)> setMode;
};
// What the window is, as a layout (the user's own: not built in). Tuning only when `includeTuning`.
Layout capture(const Window& w, bool includeTuning);
// Put the window as the layout says, in stepsOf order. A step whose answer is already true is not taken again (no flicker, no side effect). Returns what was held back: 0 nothing,
// kHeldView = the tab was left alone because a patch is running, kHeldSelection = the selected radio is not open.
constexpr int kHeldView = 1, kHeldSelection = 2;
int apply(const Layout& l, const Window& now, const Actions& act);

}  // namespace foxsdr::slintlayouts
