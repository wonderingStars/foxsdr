// workspace_prefs.hpp - WHAT THE SCREEN REMEMBERS OF THE WORKSPACE (docs/multi-radio-records/screen-s5.md, stage S5): the layout (One / Side by side / 2 x 2), the page, the selected radio, each
// radio's splitter share, Link on or off and the radios the user took out of the group.
//
// WHERE IT IS SAVED. In the interface half of the settings, in the setting the 3D views already use (AppConfig::view3d, a text of `name:body;name:body`): one entry, `workspace`, whose body this unit
// reads and writes (view3d_prefs.hpp keeps it verbatim when a 3D page saves its own view). No key was added to the engine's half of the settings and no field to AppConfig.
// THE TEXT: `workspace:lay=2,pg=1,sel=3,lk=1,ex=4,sp=45.45.45.45.45.45.45.45` - layout 0..2, page 0..7, selected radio 1..8, Link 0/1, the radios left out of the group as a bitmask (bit i = radio i + 1),
// each radio's splitter share in percent (10..90). Reading is lenient (a bad number or an unknown key is skipped and the default stays; every value is clamped), so a text from a newer build reads.
//
// DEGRADE QUIETLY, KEEP WHAT WAS SAVED. The record holds what the USER chose, whatever radios exist: a restart that comes back with one radio shows One and radio 1 (the host clamps what it applies)
// and does NOT write the record back - only a change the user makes does, field by field (`SavedWorkspace` is changed field by field by the host; the fields it did not touch keep their saved
// values). So the layout of an eight-radio session is back when the eight radios are.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace foxsdr::slinthost {

struct SavedWorkspace {
    bool present = false;                 // the text had a workspace entry
    int layout = 0;                       // 0 One, 1 Side by side, 2 Four
    int page = 0;                         // from 0
    int selected = 1;                     // 1-based
    bool linkOn = false;
    std::uint32_t excluded = 0;           // radios taken out of the group, bit per slot
    std::array<float, 8> splits{0.45f, 0.45f, 0.45f, 0.45f, 0.45f, 0.45f, 0.45f, 0.45f};
    // slint-gaps.md (items 2 and 3): how the counter groups its digits (0 = 1.090.000.000, 1 = 1090.000.000) and the radios whose frequency is locked (bit i = radio i + 1). Written to the text
    // ONLY when not 0 (`dg=`, `fl=` after `sp=`), so a text without them is what it was before and an older build reads the rest.
    int dotStyle = 0;
    std::uint32_t locked = 0;
    bool operator==(const SavedWorkspace& o) const = default;
};

// The body of the entry (without the name), the same state always the same text.
std::string workspaceBody(const SavedWorkspace& w);
// Reads a body leniently; the fields it does not hold keep SavedWorkspace's defaults and `present` is true.
SavedWorkspace parseWorkspaceBody(const std::string& body);

// The whole interface-half text (`view3d`): the saved layout in it, and the text with a layout put in it (every other entry untouched).
SavedWorkspace workspaceFromSettings(const std::string& view3dText);
std::string settingsWithWorkspace(const std::string& view3dText, const SavedWorkspace& w);

// THE RECORD IN USE (the host's one instance): what was saved, loaded at start; changed only by what the USER does, field by field, so that a session that could not show what was saved (one radio
// where eight were saved) leaves the record as it was. `observe` takes the three things the screen holds that only the user changes (the layout, the splitters, Link); a selection or a page the user
// made is told through userSelected / userPage; a selection the engine forced (a radio went away) is not told and not saved.
class WorkspaceMemory {
public:
    // How long, from the start, a saved selection waits for its radio to come back (the engine restores its radios in the first seconds): 300 ticks of 33 ms, about ten seconds.
    static constexpr long kRestoreWindowTicks = 300;
    void load(const std::string& view3dText);
    const SavedWorkspace& saved() const { return saved_; }
    bool userSelected(int radio, int page);
    bool userPage(int page);
    // the digit grouping and the frequency lock are the user's choices, told here when they change (true = changed, saved a moment later like the rest)
    bool setDotStyle(int style);
    bool setLocked(std::uint32_t mask);
    bool observe(int layout, const std::array<float, 8>& splits, bool linkOn, std::uint32_t excluded);
    // The saved selection and page, once: while the window is open (`tick` within kRestoreWindowTicks), the user has not chosen anything yet and the saved radio is open (`openMask`, bit per slot).
    // Returns true with the radio (1-based) and page to apply; false when there is nothing to apply (yet).
    bool restoreDue(long tick, std::uint32_t openMask, int& radio, int& page);
    bool dirty() const { return dirty_; }
    void markWritten() { dirty_ = false; }
    std::string text(const std::string& view3dText) const { return settingsWithWorkspace(view3dText, saved_); }

private:
    SavedWorkspace saved_;
    bool armed_ = false;
    bool dirty_ = false;
};


}  // namespace foxsdr::slinthost
