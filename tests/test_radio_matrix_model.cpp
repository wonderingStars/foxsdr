// Tests for core/radio_matrix_model.hpp - the per-radio summary table, the hover-to-listen policy and the follow-a-master relation.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/radio_matrix_model.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

#include "test_check.hpp"

using namespace cascade::core;

namespace {

RadioSummary row(int id, MatrixRadioState st = MatrixRadioState::Running) {
    RadioSummary r;
    r.id = id;
    r.state = st;
    r.centreHz = 100e6 + id * 1e6;
    r.sampleRateHz = 2.4e6;
    r.tunedHz = r.centreHz + 1000.0;
    r.levelDb = -80.0f;
    return r;
}

void testTable() {
    std::printf("  summary table: rows by id, replace, remove, bad ids and numbers, listening, running count\n");
    RadioMatrix m;
    CHECK(m.size() == 0u);
    CHECK(m.update(row(3)));
    CHECK(m.update(row(0)));
    CHECK(m.update(row(7, MatrixRadioState::Faulted)));
    CHECK(!m.update(row(8)));
    CHECK(!m.update(row(-1)));
    CHECK(m.size() == 3u);
    const std::vector<RadioSummary> rows = m.rows();
    CHECK(rows.size() == 3u);
    CHECK(rows.size() == 3u && rows[0].id == 0 && rows[1].id == 3 && rows[2].id == 7);   // sorted by id whatever the insertion order
    CHECK(m.runningCount() == 2u);
    RadioSummary r = row(3);
    r.centreHz = 7e6;
    CHECK(m.update(r));                                    // replaces, does not add
    CHECK(m.size() == 3u && m.find(3) != nullptr && m.find(3)->centreHz == 7e6);
    CHECK(m.find(5) == nullptr && m.find(-2) == nullptr && m.find(9) == nullptr);
    m.remove(3);
    CHECK(m.size() == 2u && m.find(3) == nullptr);
    m.remove(99);                                          // harmless
    CHECK(m.size() == 2u);
    // Non-finite numbers never reach a sum; a self-follow or out-of-range master is dropped.
    RadioSummary bad = row(1);
    bad.centreHz = std::nan("");
    bad.sampleRateHz = -5.0;
    bad.tunedHz = std::numeric_limits<double>::infinity();
    bad.levelDb = std::nanf("");
    bad.followsId = 1;
    CHECK(m.update(bad));
    CHECK(m.find(1)->centreHz == 0.0 && m.find(1)->sampleRateHz == 0.0 && m.find(1)->tunedHz == 0.0 && m.find(1)->levelDb == -200.0f);
    CHECK(m.find(1)->followsId == -1);
    bad.followsId = 12;
    CHECK(m.update(bad) && m.find(1)->followsId == -1);
    bad.followsId = 0;
    CHECK(m.update(bad) && m.find(1)->followsId == 0);
    // Listening: none, one, two flagged (inconsistent: lowest id wins).
    bool consistent = false;
    CHECK(m.listeningId(&consistent) == -1 && consistent);
    RadioSummary l5 = row(5);
    l5.listening = true;
    m.update(l5);
    CHECK(m.listeningId(&consistent) == 5 && consistent);
    RadioSummary l2 = row(2);
    l2.listening = true;
    m.update(l2);
    CHECK(m.listeningId(&consistent) == 2 && !consistent);
    CHECK(m.listeningId() == 2);
    m.clear();
    CHECK(m.size() == 0u && m.rows().empty());
    // All eight.
    for (int i = 0; i < kMatrixMaxRadios; ++i) { CHECK(m.update(row(i))); }
    CHECK(m.size() == 8u && m.runningCount() == 8u);
    CHECK(matrixRadioStateText(MatrixRadioState::Busy) != nullptr);
}

void testGrid() {
    std::printf("  grid: 1..8 cells, wider than tall, nothing outside\n");
    const int cols[9] = {0, 1, 2, 2, 2, 3, 3, 4, 4};
    const int rows[9] = {0, 1, 1, 2, 2, 2, 2, 2, 2};
    for (int n = 1; n <= 8; ++n) {
        const MatrixGrid g = matrixGridFor(n);
        CHECK(g.cols == cols[n] && g.rows == rows[n]);
        CHECK(g.cols * g.rows >= n && g.cols >= g.rows);
    }
    CHECK(matrixGridFor(0).cols == 0 && matrixGridFor(9).rows == 0 && matrixGridFor(-3).cols == 0);
}

void testHover() {
    std::printf("  hover to listen: dwell, grace, baseline, muted, sweeping across cells, bad clocks\n");
    HoverListen h;
    HoverListenConfig c;
    c.dwellS = 0.15;
    c.graceS = 0.30;
    h.configure(c);
    h.setBaseline(2);
    CHECK(h.baseline() == 2);
    double t = 0.0;
    auto step = [&](int cell, std::uint32_t muted = 0) {
        const HoverDecision d = h.update(cell, muted, t);
        t += 0.05;
        return d;
    };
    // Resting on radio 4 for the dwell routes to it, exactly once.
    CHECK(!step(4).changed);                       // t=0.00
    CHECK(!step(4).changed);                       // 0.05
    CHECK(!step(4).changed);                       // 0.10
    HoverDecision d = step(4);                     // 0.15
    CHECK(d.changed && d.listenId == 4);
    CHECK(h.hovering() == 4);
    CHECK(!step(4).changed);
    // Leaving: nothing until the grace has passed, then back to the baseline.
    CHECK(!step(-1).changed);                      // leave at this time
    CHECK(!step(-1).changed);
    CHECK(!step(-1).changed);
    CHECK(!step(-1).changed);
    CHECK(!step(-1).changed);
    CHECK(!step(-1).changed);                      // 0.25 s after leaving: still routed
    d = step(-1);                                  // 0.30 s after leaving
    CHECK(d.changed && d.listenId == 2);
    CHECK(h.hovering() == -1);
    CHECK(!step(-1).changed);                      // stays quiet
    // Coming back inside the grace keeps the audio on the radio, nothing changes.
    for (int i = 0; i < 4; ++i) { step(5); }       // routes to 5
    CHECK(h.hovering() == 5);
    step(-1);
    step(-1);
    CHECK(!step(5).changed && h.hovering() == 5);  // back on the same cell inside the grace
    step(-1);
    CHECK(!step(-1).changed);                      // the grace restarted from this leave, not the earlier one
    // Sweeping across cells faster than the dwell never routes anything.
    h.configure(c);
    h.setBaseline(-1);
    bool any = false;
    for (int i = 0; i < 40; ++i) { any = any || step(i % 8).changed; }   // 50 ms per cell
    CHECK(!any && h.hovering() == -1);
    // Moving from a routed radio to another: the new one needs its own dwell, and no "back to baseline" happens in between.
    h.configure(c);
    h.setBaseline(1);
    for (int i = 0; i < 4; ++i) { step(3); }
    CHECK(h.hovering() == 3);
    bool back = false;
    HoverDecision to6;
    for (int i = 0; i < 4; ++i) {
        const HoverDecision x = step(6);
        if (x.changed && x.listenId == 1) { back = true; }
        if (x.changed && x.listenId == 6) { to6 = x; }
    }
    CHECK(!back && to6.changed && to6.listenId == 6 && h.hovering() == 6);
    // With no baseline the route simply stops (listenId -1 = nothing from this model).
    h.configure(c);
    h.setBaseline(-1);
    for (int i = 0; i < 4; ++i) { step(0); }
    bool stopped = false;
    for (int i = 0; i < 10; ++i) {
        const HoverDecision x = step(-1);
        if (x.changed) { stopped = x.listenId == -1; }
    }
    CHECK(stopped);
    // Muted radios are skipped (and the pointer counts as being over nothing); the option can be turned off.
    h.configure(c);
    bool routed = false;
    for (int i = 0; i < 10; ++i) { routed = routed || step(3, 1u << 3).changed; }
    CHECK(!routed);
    c.skipMuted = false;
    h.configure(c);
    for (int i = 0; i < 4; ++i) { routed = routed || step(3, 1u << 3).changed; }
    CHECK(routed);
    // Immediate mode (dwell 0), as SDR Console's audio preview does: the first frame routes.
    c.dwellS = 0.0;
    c.skipMuted = true;
    h.configure(c);
    d = step(6);
    CHECK(d.changed && d.listenId == 6);
    // Bad ids and clocks.
    h.configure(c);
    CHECK(!step(8).changed && !step(-4).changed);
    h.setBaseline(9);
    CHECK(h.baseline() == -1);
    t = 10.0;
    CHECK(h.update(2, 0, 10.0).changed);
    CHECK(!h.update(3, 0, 5.0).changed);                      // a clock that went backwards is ignored
    CHECK(!h.update(3, 0, std::nan("")).changed);
    CHECK(h.hovering() == 2);
    HoverListenConfig z;
    z.dwellS = -1.0;
    z.graceS = std::nan("");
    h.configure(z);                                           // repaired to zeros, no crash
    CHECK(h.update(1, 0, 0.0).changed);
}

std::array<bool, kMatrixMaxRadios> allKnown() {
    std::array<bool, kMatrixMaxRadios> k{};
    k.fill(true);
    return k;
}

FollowMove moveOf(const std::vector<FollowMove>& v, int id) {
    for (const FollowMove& m : v) {
        if (m.radioId == id) { return m; }
    }
    FollowMove none;
    none.radioId = -99;
    return none;
}

void testFollow() {
    std::printf("  follow a master: offset kept, lock, chains in order, cycles refused, unknown and closed radios, clamping\n");
    FollowSet f;
    CHECK(f.link(1, 0, 145.5e6, 144.0e6) == FollowError::None);          // radio 1 sits 1.5 MHz above radio 0
    CHECK(f.masterOf(1) == 0 && f.offsetOf(1) == 1.5e6 && f.linkCount() == 1u);
    std::vector<FollowMove> mv = f.onMasterMoved(0, 146.0e6, allKnown());
    CHECK(mv.size() == 1u && moveOf(mv, 1).newHz == 147.5e6 && !moveOf(mv, 1).clamped);
    // The master moving by D moves the follower by D, many times over, with no drift.
    double masterHz = 146.0e6;
    for (int i = 0; i < 1000; ++i) {
        masterHz += (i % 2 == 0 ? 12500.0 : -7500.0);
        mv = f.onMasterMoved(0, masterHz, allKnown());
        CHECK(moveOf(mv, 1).newHz == masterHz + 1.5e6);
    }
    // LOCK (SDRuno's LO LOCK): the follower stays; unlocking brings it back on the next move.
    f.setLocked(1, true);
    CHECK(f.locked(1));
    CHECK(f.onMasterMoved(0, 150e6, allKnown()).empty());
    f.setLocked(1, false);
    CHECK(moveOf(f.onMasterMoved(0, 150e6, allKnown()), 1).newHz == 151.5e6);
    // A nudged follower keeps its new distance when the user says so.
    f.setOffset(1, 2.0e6);
    CHECK(moveOf(f.onMasterMoved(0, 150e6, allKnown()), 1).newHz == 152.0e6);
    f.setOffset(1, std::nan(""));
    CHECK(f.offsetOf(1) == 2.0e6);
    // A chain 0 <- 1 <- 2 <- 3: applied front to back, and a locked middle link stops everything behind it.
    CHECK(f.link(2, 1, 153.0e6, 152.0e6) == FollowError::None);
    CHECK(f.link(3, 2, 150.0e6, 153.0e6) == FollowError::None);
    mv = f.onMasterMoved(0, 150e6, allKnown());
    CHECK(mv.size() == 3u);
    CHECK(mv.size() == 3u && mv[0].radioId == 1 && mv[1].radioId == 2 && mv[2].radioId == 3);
    CHECK(moveOf(mv, 2).newHz == 153.0e6 && moveOf(mv, 3).newHz == 150.0e6);   // offsets kept all the way
    f.setLocked(2, true);
    mv = f.onMasterMoved(0, 160e6, allKnown());
    CHECK(mv.size() == 1u && mv[0].radioId == 1);                            // 2 is locked, so 3's master did not move
    f.setLocked(2, false);
    // A follower moving by itself does not move its master (only onMasterMoved moves anything).
    CHECK(f.onMasterMoved(3, 1e6, allKnown()).empty());
    // Cycles and bad links.
    CHECK(f.link(0, 3, 1e6, 1e6) == FollowError::Cycle);                     // 0 <- 1 <- 2 <- 3 <- 0
    CHECK(f.link(1, 4, 1e6, 1e6) == FollowError::AlreadyFollowing);
    CHECK(f.link(5, 5, 1e6, 1e6) == FollowError::SameRadio);
    CHECK(f.link(5, 8, 1e6, 1e6) == FollowError::BadId);
    CHECK(f.link(-1, 0, 1e6, 1e6) == FollowError::BadId);
    CHECK(f.link(5, 6, std::nan(""), 1e6) == FollowError::NotFinite);
    CHECK(f.link(5, 6, 1e6, std::numeric_limits<double>::infinity()) == FollowError::NotFinite);
    CHECK(f.masterOf(5) == -1);
    CHECK(followErrorText(FollowError::Cycle) != nullptr);
    // Radios that are not open are skipped (and so is what hangs behind them).
    std::array<bool, kMatrixMaxRadios> known = allKnown();
    known[2] = false;
    mv = f.onMasterMoved(0, 170e6, known);
    CHECK(mv.size() == 1u && mv[0].radioId == 1);
    // Clamping to the follower's range, flagged.
    FollowSet g;
    CHECK(g.link(1, 0, 6.0e9 - 1e6, 100e6) == FollowError::None);
    mv = g.onMasterMoved(0, 200e6, allKnown());
    CHECK(mv.size() == 1u && mv[0].newHz == 6.0e9 && mv[0].clamped);
    FollowLimits lim;
    lim.minHz = 24e6;
    lim.maxHz = 1.7e9;
    CHECK(g.link(2, 0, 30e6, 100e6) == FollowError::None);
    mv = g.onMasterMoved(0, 110e6, allKnown(), lim);
    CHECK(moveOf(mv, 2).newHz == 40e6 && !moveOf(mv, 2).clamped);            // offset -70 MHz: 110 - 70 = 40, inside the range
    mv = g.onMasterMoved(0, 90e6, allKnown(), lim);
    CHECK(moveOf(mv, 2).newHz == 24e6 && moveOf(mv, 2).clamped);             // 90 - 70 = 20: below the follower's range
}

void testFollowClosed() {
    std::printf("  follow: closing a radio releases its links both ways; unlink; a bad master move\n");
    FollowSet f;
    CHECK(f.link(1, 0, 2e6, 1e6) == FollowError::None);
    CHECK(f.link(2, 0, 3e6, 1e6) == FollowError::None);
    CHECK(f.link(3, 2, 4e6, 3e6) == FollowError::None);
    CHECK(f.linkCount() == 3u);
    f.radioClosed(0);                                           // its two followers are free
    CHECK(f.masterOf(1) == -1 && f.masterOf(2) == -1 && f.masterOf(3) == 2 && f.linkCount() == 1u);
    f.radioClosed(3);                                           // a follower closing frees only itself
    CHECK(f.linkCount() == 0u);
    CHECK(f.link(1, 0, 2e6, 1e6) == FollowError::None);
    f.unlink(1);
    CHECK(f.masterOf(1) == -1);
    f.unlink(9);                                                // harmless
    f.radioClosed(-1);
    f.setLocked(5, true);                                       // not linked: ignored
    CHECK(!f.locked(5));
    CHECK(f.onMasterMoved(0, std::nan(""), allKnown()).empty());
    CHECK(f.onMasterMoved(9, 1e6, allKnown()).empty());
}

// Source scan: the model files hold no identifier-shaped fields (names, serials, addresses) and print nothing.
void testSourceScan() {
    std::printf("  source scan: no name/serial/address field, no stdout or stderr output\n");
    // Found from this file's own compile-time path: tests/ -> repository root.
    std::string here = __FILE__;
    const std::size_t cut = here.find_last_of("/\\");
    const std::string root = cut == std::string::npos ? std::string(".") : here.substr(0, cut) + "/..";
    std::ifstream in(root + "/src/core/radio_matrix_model.hpp", std::ios::binary);
    if (!in) {
        std::printf("  (source not found from this build: scan skipped)\n");
        ++g_checksSkipped;
        return;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string src = ss.str();
    for (const char* bad : {"printf", "std::cout", "std::cerr", "fprintf", "puts(", "serial", "std::string name", "address;", "callsign"}) {
        // The words may appear in comments that DESCRIBE the rule; code lines are what is scanned.
        std::size_t pos = 0;
        while ((pos = src.find(bad, pos)) != std::string::npos) {
            const std::size_t ls = src.rfind('\n', pos);
            const std::size_t le = src.find('\n', pos);
            const std::string line = src.substr(ls == std::string::npos ? 0 : ls + 1, le - (ls == std::string::npos ? 0 : ls + 1));
            const std::size_t first = line.find_first_not_of(" \t");
            const bool comment = first != std::string::npos && line.compare(first, 2, "//") == 0;
            CHECK(comment);
            pos += 1;
        }
    }
}

}  // namespace

int main() {
    testTable();
    testGrid();
    testHover();
    testFollow();
    testFollowClosed();
    testSourceScan();
    return testSummary("test_radio_matrix_model");
}
