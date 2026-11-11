// Link tuning, the saved layout, the page keys and the new key-table rows (src/slint/link_logic.hpp, workspace_prefs.hpp, workspace_logic.hpp stepPage, system_logic.hpp; docs/multi-radio-records/screen-s5.md,
// stage S5). No window, no engine: made-up readings stand for the radios' tuned frequencies, and a made-up "engine" (a table of readings the test moves by hand) stands for the follow commands.
//   LINK: off by default and then nothing moves; on, a move of one linked radio moves the others by the same amount and the gaps are kept; every way of tuning is one move (observed, not intercepted);
//   a radio that is out of the group is not moved and does not move the others; a range limit (known to a test): the radio stays at its limit, the others move, the intended gap is kept and comes back when the
//   group moves back; an unknown limit (the engine refuses and the radio does not move): found out when the follow command is not answered; a radio added while Link is on joins; a radio removed leaves and
//   nothing else is disturbed; noise below the tolerance is not a move; two radios moved in one tick; a follower that is still on its way is not taken for a mover.
//   SAVED LAYOUT: the text is read leniently and clamped, written the same way every time, shares the 3D views' setting without damaging their entries and without being damaged by them; the record in use
//   degrades quietly (nothing is written for what the engine could not restore) and a restored selection applies when its radio returns, once, inside the window.
//   (the ten new key-table rows are held by test_slint_host_keydispatch.)
//   PAGES: Alt+PageUp / Alt+PageDown turn the page in a multi-pane layout (clamped), and select the neighbouring radio where One is shown.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "link_logic.hpp"
#include "workspace_prefs.hpp"
#include "view3d_prefs.hpp"
#include "test_check.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace foxsdr::slinthost;

namespace {

// A made-up set of radios: each has a tuned frequency; the test applies the group's commands to them (a follower "arrives" at once, or never, as the test says).
struct Radios {
    std::array<double, kMaxRadios> hz{};
    std::uint32_t open = 0;
    std::array<LinkReading, kMaxRadios> readings() const {
        std::array<LinkReading, kMaxRadios> r{};
        for (int i = 0; i < kMaxRadios; ++i) { r[static_cast<std::size_t>(i)].valid = ((open >> i) & 1u) != 0u; r[static_cast<std::size_t>(i)].tunedHz = hz[static_cast<std::size_t>(i)]; }
        return r;
    }
    void openRadio(int slot, double f) { open |= 1u << slot; hz[static_cast<std::size_t>(slot)] = f; }
};

// One tick: step, then the commands "arrive" (the radios take what they are asked for).
std::vector<LinkMove> tick(LinkGroup& g, Radios& r, const std::function<LinkRange(int)>& range = nullptr, bool arrive = true) {
    const std::vector<LinkMove> moves = g.step(r.readings(), r.open, range);
    if (arrive) { for (const LinkMove& m : moves) { r.hz[static_cast<std::size_t>(m.slot)] = m.hz; } }
    return moves;
}

bool near(double a, double b, double tol = 0.01) { return std::fabs(a - b) <= tol; }

Radios four() {
    Radios r;
    r.openRadio(0, 100.0e6); r.openRadio(1, 100.5e6); r.openRadio(2, 144.8e6); r.openRadio(3, 433.5e6);
    return r;
}

EngineReads engineWith(const std::map<std::string, std::string>& facts, int* focused = nullptr, int32_t focusAnswer = FOXAPI_OK) {
    EngineReads e;
    e.fact = [facts](const char* name, std::string& out) -> int {
        const auto it = facts.find(name);
        if (it == facts.end()) { return -1; }
        out = it->second;
        return static_cast<int>(out.size());
    };
    e.focus = [focused, focusAnswer](std::uint32_t radio) -> int32_t { if (focused != nullptr) { *focused = static_cast<int>(radio); } return focusAnswer; };
    return e;
}

}  // namespace

int main() {
    std::printf("test_slint_radio_link_host\n");

    // ================= LINK =================
    {   // off by default: nothing is observed, nothing moves
        LinkGroup g;
        Radios r = four();
        CHECK(!g.on());
        for (int i = 0; i < 3; ++i) { CHECK(tick(g, r).empty()); }
        r.hz[0] += 10e3;
        CHECK(tick(g, r).empty());
        CHECK(near(r.hz[1], 100.5e6) && near(r.hz[2], 144.8e6) && near(r.hz[3], 433.5e6));
        CHECK(g.stateOf(0, r.open) == LinkState::Off && !g.linked(0, r.open));
    }
    {   // on: +10 kHz on radio 1 moves the other three by +10 kHz and the gaps are what they were
        LinkGroup g;
        Radios r = four();
        g.setOn(true, r.open);
        CHECK(g.on() && g.excluded() == 0u);
        CHECK(tick(g, r).empty());                     // the baseline
        r.hz[0] += 10e3;
        const auto m = tick(g, r);
        CHECK(m.size() == 3);
        CHECK(near(r.hz[0], 100.01e6) && near(r.hz[1], 100.51e6) && near(r.hz[2], 144.81e6) && near(r.hz[3], 433.51e6));
        CHECK(near(r.hz[1] - r.hz[0], 0.5e6) && near(r.hz[2] - r.hz[1], 44.3e6) && near(r.hz[3] - r.hz[2], 288.7e6));   // the gaps
        CHECK(tick(g, r).empty() && tick(g, r).empty());   // the followers' own arrival is not taken for a move: nothing ping-pongs
        // and the other way, from another radio, by a different amount (negative)
        r.hz[3] -= 2.5e6;
        CHECK(tick(g, r).size() == 3);
        CHECK(near(r.hz[0], 97.51e6) && near(r.hz[1], 98.01e6) && near(r.hz[2], 142.31e6) && near(r.hz[3], 431.01e6));
        CHECK(tick(g, r).empty());
        CHECK(g.stateOf(1, r.open) == LinkState::Linked);
    }
    {   // a radio the user took out is not moved and does not move the others; putting it back takes the gap it has then
        LinkGroup g;
        Radios r = four();
        g.setOn(true, r.open);
        tick(g, r);
        CHECK(g.setLinked(2, false, r.open) && !g.linked(2, r.open) && g.stateOf(2, r.open) == LinkState::Off);
        CHECK(!g.setLinked(2, false, r.open));          // nothing changed
        r.hz[0] += 25e3;
        CHECK(tick(g, r).size() == 2);
        CHECK(near(r.hz[2], 144.8e6) && near(r.hz[1], 100.525e6) && near(r.hz[3], 433.525e6));
        r.hz[2] += 1e6;                                  // the left-out radio is tuned: nobody follows
        CHECK(tick(g, r).empty());
        CHECK(near(r.hz[0], 100.025e6));
        CHECK(g.setLinked(2, true, r.open));
        tick(g, r);                                      // baseline at the new place
        r.hz[1] += 5e3;
        CHECK(tick(g, r).size() == 3);
        CHECK(near(r.hz[2], 145.805e6));                 // 144.8 + 1.0 + 0.005: the gap it had when it rejoined is kept
        // Link off refuses the pane's key; a radio that is not open cannot be linked
        LinkGroup off;
        CHECK(!off.setLinked(1, true, r.open));
        CHECK(!g.setLinked(6, true, r.open));
    }
    {   // a RANGE LIMIT (known to the test): the radio stays at its limit, the others move, the pane says so; the gap comes back
        LinkGroup g;
        Radios r;
        r.openRadio(0, 100.0e6); r.openRadio(1, 107.0e6);   // radio 2's device stops at 108 MHz
        const auto range = [](int slot) { LinkRange l; if (slot == 1) { l.known = true; l.loHz = 88.0e6; l.hiHz = 108.0e6; } return l; };
        g.setOn(true, r.open);
        tick(g, r, range);
        r.hz[0] += 4e6;                                  // radio 1 to 104 MHz: radio 2 would be at 111 MHz
        const auto m = tick(g, r, range);
        CHECK(m.size() == 1 && m[0].slot == 1 && near(m[0].hz, 108.0e6));
        CHECK(near(r.hz[0], 104.0e6) && near(r.hz[1], 108.0e6));   // radio 1 moved, radio 2 at its limit
        CHECK(g.stateOf(1, r.open) == LinkState::CouldNotFollow && g.stateOf(0, r.open) == LinkState::Linked);
        CHECK(near(g.intendedHz(1), 111.0e6) && near(g.heldHz(1), 107.0e6));
        tick(g, r, range);
        CHECK(g.stateOf(1, r.open) == LinkState::CouldNotFollow);   // it stays that way while it is short
        CHECK(near(g.heldHz(1), 108.0e6));
        r.hz[0] += 1e6;                                  // further up: radio 2 stays at its limit, no new command (it is already there)
        CHECK(tick(g, r, range).empty());
        CHECK(near(r.hz[1], 108.0e6) && g.stateOf(1, r.open) == LinkState::CouldNotFollow && near(g.intendedHz(1), 112.0e6));
        r.hz[0] -= 6e6;                                  // back down to 99 MHz: radio 2 follows to 106 MHz, the gap of 7 MHz is back
        const auto back = tick(g, r, range);
        CHECK(back.size() == 1 && near(back[0].hz, 106.0e6));
        CHECK(near(r.hz[1] - r.hz[0], 7.0e6));
        tick(g, r, range);
        CHECK(g.stateOf(1, r.open) == LinkState::Linked);
        // the lower limit as well
        r.hz[0] -= 15e6;                                 // radio 1 to 84 MHz: radio 2 at 91 MHz, fine
        tick(g, r, range);
        r.hz[0] -= 5e6;                                  // 79 MHz: radio 2 would be at 86 MHz, under 88
        const auto low = tick(g, r, range);
        CHECK(low.size() == 1 && near(low[0].hz, 88.0e6) && g.stateOf(1, r.open) == LinkState::CouldNotFollow);
    }
    {   // an UNKNOWN limit: the engine refuses and the radio stays where it was; after the give-up time the pane says it could not follow, and the others were never held back
        LinkGroup g;
        Radios r;
        r.openRadio(0, 100.0e6); r.openRadio(1, 107.0e6); r.openRadio(2, 120.0e6);
        g.setOn(true, r.open);
        tick(g, r);
        r.hz[0] += 4e6;
        std::vector<LinkMove> m = g.step(r.readings(), r.open);
        CHECK(m.size() == 2);
        for (const LinkMove& x : m) { if (x.slot == 2) { r.hz[2] = x.hz; } }   // radio 3 arrives; radio 2 refuses (its readback does not move)
        CHECK(near(r.hz[2], 124.0e6) && near(r.hz[1], 107.0e6));
        for (int i = 0; i < kLinkGiveUpTicks - 2; ++i) { CHECK(g.step(r.readings(), r.open).empty()); }
        CHECK(g.stateOf(1, r.open) == LinkState::Linked);   // not yet given up
        for (int i = 0; i < 4; ++i) { g.step(r.readings(), r.open); }
        CHECK(g.stateOf(1, r.open) == LinkState::CouldNotFollow && g.stateOf(2, r.open) == LinkState::Linked);
        CHECK(near(g.heldHz(1), 107.0e6) && near(g.intendedHz(1), 111.0e6));
        // the refusing radio is where it is: it did not turn into a mover when it was given up on
        CHECK(g.step(r.readings(), r.open).empty());
        CHECK(near(r.hz[0], 104.0e6) && near(r.hz[2], 124.0e6));
    }
    {   // a radio ADDED while Link is on joins (with the gap it has); a radio REMOVED leaves and the others are undisturbed; one radio left: nothing to move
        LinkGroup g;
        Radios r;
        r.openRadio(0, 100.0e6); r.openRadio(1, 100.2e6);
        g.setOn(true, r.open);
        tick(g, r);
        r.openRadio(2, 90.0e6);
        tick(g, r);                                       // its baseline
        CHECK(g.linked(2, r.open) && g.stateOf(2, r.open) == LinkState::Linked);
        r.hz[1] += 1e6;
        CHECK(tick(g, r).size() == 2);
        CHECK(near(r.hz[0], 101.0e6) && near(r.hz[2], 91.0e6) && near(r.hz[1], 101.2e6));
        tick(g, r);
        r.open &= ~(1u << 1);                             // radio 2 is removed
        g.forget(1);
        CHECK(tick(g, r).empty());
        CHECK(near(r.hz[0], 101.0e6) && near(r.hz[2], 91.0e6));
        r.hz[2] += 3e6;
        CHECK(tick(g, r).size() == 1 && near(r.hz[0], 104.0e6));    // the two that are left are still a group
        tick(g, r);                                       // (the follower settles)
        r.open &= ~(1u << 2);
        g.forget(2);
        r.hz[0] += 1e6;
        CHECK(tick(g, r).empty());                       // one radio: nothing to move
        r.openRadio(1, 50.0e6);                           // a radio of the same number comes back: it is a new radio, a new baseline
        CHECK(tick(g, r).empty());
        r.hz[0] += 1e6;
        const auto last = tick(g, r);
        CHECK(last.size() == 1 && near(r.hz[1], 51.0e6));
    }
    {   // a radio removed by ANOTHER session (nobody calls forget): when its number comes back at some other frequency it takes a new baseline, it is not read as the user tuning it by the difference
        LinkGroup g;
        Radios r;
        r.openRadio(0, 100.0e6); r.openRadio(1, 90.0e6);
        g.setOn(true, r.open);
        tick(g, r);
        r.open &= ~(1u << 1);                                // gone, and the group is told only by the open mask
        CHECK(tick(g, r).empty());
        r.openRadio(1, 50.0e6);                              // a new radio 2, 40 MHz lower than the one that was
        CHECK(tick(g, r).empty());
        CHECK(near(r.hz[0], 100.0e6) && near(r.hz[1], 50.0e6));
        r.hz[0] += 1e6;
        CHECK(tick(g, r).size() == 1 && near(r.hz[1], 51.0e6));
    }
    {   // a removed radio's place is not kept: forget() clears the "left out" mark so the next radio of that number is linked
        LinkGroup g;
        Radios r = four();
        g.setOn(true, r.open);
        tick(g, r);
        CHECK(g.setLinked(3, false, r.open) && (g.excluded() & 8u) != 0u);
        r.open &= ~(1u << 3);
        g.forget(3);
        CHECK((g.excluded() & 8u) == 0u);
    }
    {   // noise is not a move; two radios moved in one tick (the larger move leads, both keep where they were put)
        LinkGroup g;
        Radios r = four();
        g.setOn(true, r.open);
        tick(g, r);
        r.hz[0] += 1.0;                                   // a driver's rounding
        CHECK(tick(g, r).empty());
        r.hz[0] += 8e3; r.hz[1] += 3e3;                   // two radios tuned in one tick
        const auto m = tick(g, r);
        CHECK(m.size() == 2);                              // radios 3 and 4 move by the larger move
        CHECK(near(r.hz[2], 144.8e6 + 8.0e3, 2.0) && near(r.hz[0], 100.0e6 + 8.0e3, 2.0) && near(r.hz[1], 100.5e6 + 3e3, 2.0));
        CHECK(tick(g, r).empty());
    }
    {   // switching Link off and on again: a fresh group at the gaps of the moment; off leaves everything where it is
        LinkGroup g;
        Radios r = four();
        g.setOn(true, r.open);
        tick(g, r);
        g.setOn(false, r.open);
        r.hz[0] += 50e3;
        CHECK(tick(g, r).empty());
        g.setOn(true, r.open);
        tick(g, r);
        r.hz[3] += 10e3;
        CHECK(tick(g, r).size() == 3 && near(r.hz[0], 100.06e6));
    }
    {   // a follower still on its way (paced hardware) is not a mover, and a second move while it travels retargets it by the sum
        LinkGroup g;
        Radios r;
        r.openRadio(0, 100.0e6); r.openRadio(1, 200.0e6);
        g.setOn(true, r.open);
        tick(g, r);
        r.hz[0] += 10e3;
        auto m = tick(g, r, nullptr, false);               // the follower does not arrive yet
        CHECK(m.size() == 1 && near(m[0].hz, 200.01e6));
        CHECK(tick(g, r, nullptr, false).empty());          // still on its way: no new command, no mover
        r.hz[0] += 10e3;
        m = tick(g, r, nullptr, false);
        CHECK(m.size() == 1 && near(m[0].hz, 200.02e6));    // the target moved on by the second move
        r.hz[1] = 200.02e6;                                  // arrives
        CHECK(tick(g, r).empty() && tick(g, r).empty());
        CHECK(near(r.hz[0], 100.02e6) && g.stateOf(1, r.open) == LinkState::Linked);
    }
    {   // a radio whose state has not been read is neither moved nor does it move the others
        LinkGroup g;
        Radios r = four();
        g.setOn(true, r.open);
        tick(g, r);
        std::array<LinkReading, kMaxRadios> rd = r.readings();
        rd[1].valid = false;
        rd[0].tunedHz += 10e3;
        const auto m = g.step(rd, r.open);
        CHECK(m.size() == 2);
        for (const LinkMove& x : m) { CHECK(x.slot != 1); }
    }
    {   // restore (the saved layout): on and the left-out radios come back; a radio that is left out stays out
        LinkGroup g;
        g.restore(true, 0x04u);
        Radios r = four();
        CHECK(g.on() && g.excluded() == 4u && g.linked(0, r.open) && !g.linked(2, r.open));
        tick(g, r);
        r.hz[0] += 10e3;
        CHECK(tick(g, r).size() == 2 && near(r.hz[2], 144.8e6));
        // restored with radios that are not open (the engine did not restore them): the record is kept whole
        LinkGroup h;
        h.restore(true, 0x28u);
        CHECK(h.excluded() == 0x28u);
        Radios one;
        one.openRadio(0, 100e6);
        CHECK(tick(h, one).empty() && h.excluded() == 0x28u);
    }
    {   // the words
        CHECK(linkKeyLabel() == "Link tuning" && startTogetherLabel() == "Start together" && startTogetherTip() == "Start every open radio");
        CHECK(linkKeyTip() == "Linked radios move together by the same amount and keep the gaps between them.");
        CHECK(paneLinkLabel(3) == "Link Radio 3" && paneLinkTip(true) == "Linked: moves with the other linked radios" && paneLinkTip(false) == "Not linked: Link tuning never moves this radio");
        CHECK(couldNotFollowTip(2, "144.500000 MHz") == "Radio 2 could not follow the link and stays at 144.500000 MHz.");
    }

    // ================= THE SAVED LAYOUT =================
    {
        SavedWorkspace w;
        w.layout = 2; w.page = 1; w.selected = 6; w.linkOn = true; w.excluded = 0x0A; w.splits = {0.45f, 0.30f, 0.45f, 0.45f, 0.62f, 0.45f, 0.45f, 0.90f};
        const std::string body = workspaceBody(w);
        CHECK(body == "lay=2,pg=1,sel=6,lk=1,ex=10,sp=45.30.45.45.62.45.45.90");
        const SavedWorkspace back = parseWorkspaceBody(body);
        CHECK(back.present && back.layout == 2 && back.page == 1 && back.selected == 6 && back.linkOn && back.excluded == 10u && back.splits[1] == 0.30f && back.splits[4] == 0.62f && back.splits[7] == 0.90f);
        CHECK(workspaceBody(back) == body);               // the same state, the same text
        // lenient and clamped
        const SavedWorkspace bad = parseWorkspaceBody("lay=9,pg=-3,sel=0,lk=x,ex=999,sp=5.95.abc.45,future=1,,=,lay");
        CHECK(bad.layout == 2 && bad.page == 0 && bad.selected == 1 && !bad.linkOn && bad.excluded == 255u && bad.splits[0] == 0.10f && bad.splits[1] == 0.90f && bad.splits[2] == 0.45f && bad.splits[3] == 0.45f);
        CHECK(!workspaceFromSettings("").present && !workspaceFromSettings("radar:on=1,az=10.00").present && !workspaceFromSettings("garbage").present);
    }
    {   // the setting is shared with the 3D views: neither damages the other, whatever the order
        foxsdr::slintview3d::ViewPrefs views;
        foxsdr::slintview3d::ViewState s;
        s.on = true; s.azimuth = 40.0f; s.opt[1] = 3;
        views.set("radar", s);
        const std::string text1 = views.serialize();
        SavedWorkspace w;
        w.layout = 1; w.selected = 3;
        const std::string both = settingsWithWorkspace(text1, w);
        CHECK(both.find("radar:") != std::string::npos && both.find("workspace:lay=1,pg=0,sel=3") != std::string::npos);
        CHECK(workspaceFromSettings(both).selected == 3 && workspaceFromSettings(both).layout == 1);
        // the 3D page saves again (parse, set, serialize): the workspace entry survives, byte for byte
        foxsdr::slintview3d::ViewPrefs again = foxsdr::slintview3d::ViewPrefs::parse(both);
        s.azimuth = -20.0f;
        again.set("radar", s);
        const std::string after3d = again.serialize();
        CHECK(workspaceFromSettings(after3d) == workspaceFromSettings(both));
        CHECK(foxsdr::slintview3d::ViewPrefs::parse(after3d).get("radar", {}).azimuth == -20.0f);
        // the workspace saves again: the 3D view survives
        SavedWorkspace w2 = workspaceFromSettings(after3d);
        w2.layout = 2;
        const std::string afterWs = settingsWithWorkspace(after3d, w2);
        CHECK(foxsdr::slintview3d::ViewPrefs::parse(afterWs).get("radar", {}).azimuth == -20.0f && workspaceFromSettings(afterWs).layout == 2);
        // an oversized or damaged entry is dropped, not trusted
        CHECK(!workspaceFromSettings("workspace:" + std::string(400, 'x')).present);
        foxsdr::slintview3d::ViewPrefs v2;
        v2.setRaw("workspace", "a;b");
        CHECK(v2.raw("workspace").empty());
        v2.setRaw("radar", "x=1");                             // only the workspace entry is kept verbatim
        CHECK(v2.raw("radar").empty());
    }
    {   // THE RECORD IN USE: degrade quietly, keep what was saved
        SavedWorkspace saved;
        saved.layout = 2; saved.page = 1; saved.selected = 6; saved.linkOn = true; saved.excluded = 0x08;
        saved.splits[4] = 0.60f;
        const std::string text = settingsWithWorkspace("", saved);
        WorkspaceMemory m;
        m.load(text);
        CHECK(m.saved() == workspaceFromSettings(text) && !m.dirty());
        // the engine restored one radio: the screen applies layout Four (shown One), radio 1, and observes only what it applied: the record is not touched
        std::array<float, 8> splits = saved.splits;
        CHECK(!m.observe(2, splits, true, 0x08u) && !m.dirty());
        int radio = 0, page = 0;
        CHECK(!m.restoreDue(10, 1u, radio, page));          // radio 6 is not there
        CHECK(!m.dirty() && m.text("") == text);
        // radios 2 to 8 return inside the window: the saved selection applies, once
        CHECK(m.restoreDue(120, 0xFFu, radio, page) && radio == 6 && page == 1);
        CHECK(!m.restoreDue(121, 0xFFu, radio, page));
        CHECK(!m.dirty());
        // the user changes one thing: only that field is written; the rest of the record is as saved
        splits[0] = 0.30f;
        CHECK(m.observe(2, splits, true, 0x08u) && m.dirty());
        CHECK(m.saved().selected == 6 && m.saved().page == 1 && m.saved().layout == 2 && m.saved().linkOn && m.saved().excluded == 8u && m.saved().splits[0] == 0.30f && m.saved().splits[4] == 0.60f);
        m.markWritten();
        CHECK(!m.dirty());
        // a layout the user chose: the saved page follows it onto the page of that layout that holds the saved radio (radio 6: Four page 2 = index 1, Two page 3 = index 2, One = index 5)
        CHECK(m.observe(1, splits, true, 0x08u) && m.saved().layout == 1 && m.saved().page == 2 && m.dirty());
        CHECK(m.observe(0, splits, true, 0x08u) && m.saved().layout == 0 && m.saved().page == 5);
        CHECK(m.observe(2, splits, true, 0x08u) && m.saved().layout == 2 && m.saved().page == 1);
        CHECK(m.observe(1, splits, true, 0x08u) && m.saved().page == 2);
        m.markWritten();
        CHECK(m.userSelected(3, 1) && m.saved().selected == 3 && m.dirty());
        m.markWritten();
        CHECK(!m.userSelected(3, 1) && !m.dirty());
        CHECK(m.userPage(0) && m.saved().page == 0);
        CHECK(m.observe(1, splits, false, 0u) && !m.saved().linkOn && m.saved().excluded == 0u);
        // a record that was never saved: nothing is armed, nothing is written until the user acts
        WorkspaceMemory fresh;
        fresh.load("");
        CHECK(!fresh.saved().present && !fresh.dirty());
        CHECK(!fresh.restoreDue(1, 0xFFu, radio, page));
        CHECK(!fresh.observe(0, SavedWorkspace{}.splits, false, 0u) && !fresh.dirty());   // the defaults are what is there: still nothing to write
        CHECK(fresh.observe(1, SavedWorkspace{}.splits, false, 0u) && fresh.dirty());
        // the restore window closes: a radio that comes back later does not pull the selection away from where the user is
        WorkspaceMemory late;
        late.load(text);
        CHECK(!late.restoreDue(WorkspaceMemory::kRestoreWindowTicks + 1, 0xFFu, radio, page));
        CHECK(!late.restoreDue(5, 0xFFu, radio, page) && !late.dirty());
        // a selection the user made first ends the wait
        WorkspaceMemory chosen;
        chosen.load(text);
        CHECK(chosen.userSelected(2, 0));
        CHECK(!chosen.restoreDue(20, 0xFFu, radio, page));
    }

    // ================= THE PAGE KEYS =================
    {
        RadioSet radios;
        Workspace ws(radios);
        int focused = 0;
        const EngineReads eng = engineWith({}, &focused);
        ws.setRadios(0xFFu, 8);
        ws.setLayout(kLayoutFour);
        CHECK(ws.page() == 0 && ws.pageCount() == 2);
        CHECK(ws.stepPage(+1, 1600, 900, eng) && ws.page() == 1);
        CHECK(!ws.stepPage(+1, 1600, 900, eng) && ws.page() == 1);   // clamped at the end
        CHECK(ws.stepPage(-1, 1600, 900, eng) && ws.page() == 0);
        CHECK(!ws.stepPage(-1, 1600, 900, eng) && ws.page() == 0);
        ws.setLayout(kLayoutTwo);
        CHECK(ws.pageCount() == 4);
        CHECK(ws.stepPage(+1, 1600, 900, eng) && ws.stepPage(+1, 1600, 900, eng) && ws.page() == 2);
        // where One is shown (the small-window fallback) the page is a radio: the key selects the neighbour that exists
        ws.setLayout(kLayoutFour);
        CHECK(ws.select(1, eng));
        CHECK(ws.stepPage(+1, 1000, 700, eng) && ws.selectedRadio() == 2 && focused == 2);
        CHECK(ws.stepPage(-1, 1000, 700, eng) && ws.selectedRadio() == 1);
        CHECK(!ws.stepPage(-1, 1000, 700, eng) && ws.selectedRadio() == 1);
        ws.setRadios(0x05u, 3);                                  // radios 1 and 3: a gap
        ws.setLayout(kLayoutOne);
        CHECK(ws.select(1, eng) && ws.stepPage(+1, 1600, 900, eng) && ws.selectedRadio() == 3);
        CHECK(!ws.stepPage(+1, 1600, 900, eng) && ws.selectedRadio() == 3);
        // Ctrl+n: the radio that exists is selected, the page follows it; a radio that is not open is refused and nothing moves
        ws.setRadios(0xFFu, 8);
        ws.setLayout(kLayoutFour);
        CHECK(ws.select(6, eng) && ws.selectedRadio() == 6 && ws.page() == 1);
        ws.setRadios(0x0Fu, 4);
        CHECK(ws.selectedRadio() == 1);                          // the selected radio went away: the lowest that is left
        CHECK(!ws.select(6, eng) && ws.selectedRadio() == 1);
        const EngineReads refuse = engineWith({}, &focused, FOXAPI_NO_DEVICE);
        CHECK(!ws.select(2, refuse) && ws.selectedRadio() == 1);
    }

    return testSummary("test_slint_radio_link_host");
}
