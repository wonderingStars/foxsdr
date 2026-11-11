// The digit grouping and the frequency lock as the window half remembers them (src/slint/workspace_prefs.hpp `dg=` and `fl=`; docs/multi-radio-records/slint-gaps.md items 2 and 3): the text is unchanged
// when neither is set, both come back, a build that does not know them reads the rest, every value is clamped and another entry of the same setting is untouched.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "workspace_prefs.hpp"
#include "view3d_prefs.hpp"
#include "test_check.hpp"

#include <string>

using namespace foxsdr::slinthost;

namespace {

void textRules() {
    SavedWorkspace w;
    const std::string plain = workspaceBody(w);
    CHECK(plain.find("dg=") == std::string::npos && plain.find("fl=") == std::string::npos);   // a record without them is what it was before
    w.dotStyle = 1;
    w.locked = 0x05u;
    const std::string body = workspaceBody(w);
    CHECK(body.find(",dg=1") != std::string::npos && body.find(",fl=5") != std::string::npos);
    const SavedWorkspace back = parseWorkspaceBody(body);
    CHECK(back.dotStyle == 1 && back.locked == 5u && back.layout == w.layout && back.splits == w.splits);
    // an old text: neither
    const SavedWorkspace old = parseWorkspaceBody(plain);
    CHECK(old.dotStyle == 0 && old.locked == 0u && !old.highContrast);
    SavedWorkspace hc;
    hc.highContrast = true;
    CHECK(workspaceBody(hc).find(",hc=1") != std::string::npos && parseWorkspaceBody(workspaceBody(hc)).highContrast);   // item 11: the high-contrast option
    CHECK(workspaceBody(SavedWorkspace{}).find("hc=") == std::string::npos);
    // values are clamped
    const SavedWorkspace wild = parseWorkspaceBody("lay=1,dg=9,fl=999");
    CHECK(wild.dotStyle == 1 && wild.locked == 255u && wild.layout == 1);
    const SavedWorkspace junk = parseWorkspaceBody("dg=x,fl=");
    CHECK(junk.dotStyle == 0 && junk.locked == 0u);
}

void inTheSetting() {
    SavedWorkspace w;
    w.dotStyle = 1;
    w.locked = 2u;
    const std::string text = settingsWithWorkspace("waterfall:on=1,az=-58.00,el=32.00,zoom=1.00", w);
    const SavedWorkspace back = workspaceFromSettings(text);
    CHECK(back.present && back.dotStyle == 1 && back.locked == 2u);
    CHECK(text.find("waterfall:on=1") != std::string::npos);      // the 3D view's entry is kept
}

void memory() {
    WorkspaceMemory m;
    m.load("");
    CHECK(!m.dirty() && m.saved().dotStyle == 0 && m.saved().locked == 0u);
    CHECK(m.setDotStyle(1) && m.dirty() && m.saved().dotStyle == 1);
    CHECK(!m.setDotStyle(1));                                      // no change, no write
    CHECK(m.setDotStyle(7) == false && m.saved().dotStyle == 1);   // 7 clamps to 1: no change
    m.markWritten();
    CHECK(m.setLocked(0x1FFu) && m.saved().locked == 0xFFu && m.dirty());
    CHECK(!m.setLocked(0xFFu));
    CHECK(m.setHighContrast(true) && m.saved().highContrast && !m.setHighContrast(true) && m.setHighContrast(false));
    CHECK(m.setLocked(0u) && m.saved().locked == 0u);
    // what was written loads back, in a new session
    m.setLocked(3u);
    const std::string text = m.text("");
    WorkspaceMemory next;
    next.load(text);
    CHECK(next.saved().dotStyle == 1 && next.saved().locked == 3u && !next.dirty());
}

}  // namespace

int main() {
    textRules();
    inTheSetting();
    memory();
    return testSummary("test_slint_gaps_prefs_host");
}
