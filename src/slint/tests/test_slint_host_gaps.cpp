// The classic-window gaps' pure rules (src/slint/gaps_logic.hpp; docs/multi-radio-records/slint-gaps.md): the dotted frequency text and its reading (item 2), the frequency lock and the API table
// that enforces it (item 3), what a dropped file is (item 8) and the manual's address (item 10). No Slint, no engine: the table under the lock is a fake that records what reaches it.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/app_commands.hpp"
#include "gaps_logic.hpp"
#include "host_logic.hpp"
#include "test_check.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace foxsdr::slintgaps;

namespace {

void dotted() {
    CHECK(dottedFrequencyText(1090000000.0, 0) == "1.090.000.000");
    CHECK(dottedFrequencyText(1090000000.0, 1) == "1090.000.000");     // the tester's request: the MHz figure is not split
    CHECK(dottedFrequencyText(100300000.0, 0) == "100.300.000");
    CHECK(dottedFrequencyText(100300000.0, 1) == "100.300.000");       // below 1000 MHz the two are the same text
    CHECK(dottedFrequencyText(145500.0, 0) == "0.145.500");            // always two dots, so it cannot be read as a decimal MHz
    CHECK(dottedFrequencyText(145500.0, 1) == "0.145.500");
    CHECK(dottedFrequencyText(7074000.0, 1) == "7.074.000");
    CHECK(dottedFrequencyText(0.0, 0) == "0.000.000");
    CHECK(dottedFrequencyText(-5.0, 1) == "0.000.000");
}

void parse() {
    double hz = 0;
    CHECK(parseTypedFrequencyHz("1090.000.000", hz) && hz == 1090000000.0);
    CHECK(parseTypedFrequencyHz("1.090.000.000", hz) && hz == 1090000000.0);
    CHECK(parseTypedFrequencyHz("100.300.000", hz) && hz == 100300000.0);
    CHECK(parseTypedFrequencyHz("0.145.500", hz) && hz == 145500.0);
    CHECK(parseTypedFrequencyHz("1.090.000 kHz", hz) && hz == 1090000000.0);   // grouped digits are a whole number of the unit
    // the old entries read as before: one dot is a decimal
    CHECK(parseTypedFrequencyHz("145.5", hz) && hz == 145500000.0);
    CHECK(parseTypedFrequencyHz("7.074", hz) && std::fabs(hz - 7074000.0) < 1e-6);
    CHECK(parseTypedFrequencyHz("145.5 MHz", hz) && hz == 145500000.0);
    CHECK(parseTypedFrequencyHz("145500000", hz) && hz == 145500000.0);
    CHECK(parseTypedFrequencyHz("7,074", hz) && std::fabs(hz - 7074000.0) < 1e-6);
    CHECK(!parseTypedFrequencyHz("", hz));
    CHECK(!parseTypedFrequencyHz("1.2.x", hz));
    CHECK(!parseTypedFrequencyHz("1.0.0.0.0.0.0.0.0.0.0.0.0.0", hz));          // more digits than a frequency has
    // both styles, both ways, for a spread of values
    const double values[] = {145500.0, 7074000.0, 100300000.0, 433920000.0, 1090000000.0, 5999999999.0};
    for (const double v : values) {
        for (int style = 0; style <= 1; ++style) {
            double back = 0;
            CHECK(parseTypedFrequencyHz(dottedFrequencyText(v, style), back) && back == v);
        }
    }
}

// ---- the lock ----
struct Seen { std::vector<std::uint32_t> ops; std::vector<std::uint32_t> targets; int textCalls = 0; };
Seen g_seen;
int32_t FOXAPI_CALL fakeSubmit(FoxSession*, const FoxCommand* c, uint32_t n, FoxSubmitResult* r) {
    for (uint32_t i = 0; i < n; ++i) {
        g_seen.ops.push_back(c[i].op);
        g_seen.targets.push_back(c[i].target);
        if (r != nullptr) { r[i].structSize = sizeof(FoxSubmitResult); r[i].status = FOXAPI_OK; r[i].ticket = 100 + i; }
    }
    return static_cast<int32_t>(n);
}
int32_t FOXAPI_CALL fakeSubmitText(FoxSession*, const FoxCommand* c, const char*, uint32_t, FoxSubmitResult* r) {
    ++g_seen.textCalls;
    g_seen.ops.push_back(c->op);
    g_seen.targets.push_back(c->target);
    if (r != nullptr) { r->structSize = sizeof(FoxSubmitResult); r->status = FOXAPI_OK; r->ticket = 7; }
    return 1;
}
FoxCommand cmd(std::uint32_t op, std::uint32_t target = 0, std::int64_t i0 = 0) {
    FoxCommand c{};
    c.structSize = sizeof(c);
    c.op = op;
    c.target = target;
    c.ival[0] = i0;
    return c;
}

void tuningSet() {
    for (const std::uint32_t op : {FOXAPI_OP_SET_CENTRE, FOXAPI_OP_SET_FREQUENCY, FOXAPI_OP_SET_VFO_OFFSET, FOXAPI_OP_STEP_TUNE, FOXAPI_OP_BOOKMARK_TUNE, FOXAPI_OP_PLUGIN_PRESET}) {
        CHECK(isTuningCommand(cmd(op)));
    }
    CHECK(isTuningCommand(cmd(FOXAPI_OP_SCANNER_RUN, 0, 1)));      // a scanner START tunes
    CHECK(isTuningCommand(cmd(FOXAPP_OP_VFO_TO_ABSOLUTE)) && isTuningCommand(cmd(FOXAPP_OP_SET_VFO_OFFSET_FREE)) && isTuningCommand(cmd(FOXAPP_OP_USER_PRESET_APPLY)));   // click-to-tune, the VFO slider, a user preset
    CHECK(isTuningCommand(cmd(FOXAPP_OP_SCANNER_LIST, 0, 2)) && !isTuningCommand(cmd(FOXAPP_OP_SCANNER_LIST, 0, 1)));
    CHECK(isTuningCommand(cmd(FOXAPP_OP_AIRBAND, 0, 1)) && isTuningCommand(cmd(FOXAPP_OP_AIRBAND, 0, 2)) && isTuningCommand(cmd(FOXAPP_OP_AIRBAND, 0, 6)) && !isTuningCommand(cmd(FOXAPP_OP_AIRBAND, 0, 0)));
    CHECK(!isTuningCommand(cmd(FOXAPI_OP_SCANNER_RUN, 0, 0)));     // a stop does not
    for (const std::uint32_t op : {FOXAPI_OP_SET_MODE, FOXAPI_OP_SET_BANDWIDTH, FOXAPI_OP_SET_VOLUME, FOXAPI_OP_SET_SQUELCH, FOXAPI_OP_SET_MUTED, FOXAPI_OP_RUN, FOXAPI_OP_SET_DEEMPHASIS,
                                   FOXAPI_OP_SCANNER_SKIP, FOXAPI_OP_BOOKMARK_ADD, FOXAPI_OP_DECODER_START, FOXAPI_OP_RADIO_FOCUS}) {
        CHECK(!isTuningCommand(cmd(op)));
    }
}

void lockState() {
    FrequencyLock& lk = FrequencyLock::instance();
    lk.setMask(0);
    lk.setSelected(1);
    lk.resetRefusals();
    CHECK(!lk.refuses(cmd(FOXAPI_OP_SET_FREQUENCY)));
    lk.setMask(0x1FFu);
    CHECK(lk.mask() == 0xFFu);                                      // eight radios only
    lk.setMask(1u << 1);                                            // radio 2
    CHECK(!lk.locked(1) && lk.locked(2) && !lk.locked(0) && !lk.locked(9));
    lk.setSelected(2);
    CHECK(lk.refuses(cmd(FOXAPI_OP_SET_FREQUENCY)));                // no target: the selected radio
    CHECK(lk.lastRefusedRadio() == 2 && lk.refusals() == 1);
    CHECK(!lk.refuses(cmd(FOXAPI_OP_SET_FREQUENCY, 1)));            // a pane's gesture on radio 1 is aimed at it
    CHECK(lk.refuses(cmd(FOXAPI_OP_SET_FREQUENCY, 0), 2));          // the strip's own aim (seam path) beats the selection
    CHECK(!lk.refuses(cmd(FOXAPI_OP_SET_FREQUENCY, 1), 2));         // ...and the command's own target beats both
    CHECK(lk.refuses(cmd(FOXAPI_OP_STEP_TUNE, 2)));
    CHECK(!lk.refuses(cmd(FOXAPI_OP_SET_MODE, 2)));                 // mode, volume and the rest still work
    CHECK(!lk.refuses(cmd(FOXAPI_OP_SET_VOLUME)));
    CHECK(!lk.refuses(cmd(FOXAPI_OP_RUN, FOXAPI_TARGET_ALL_RADIOS)));
    lk.setMask(0);
    lk.resetRefusals();
}

void lockedTable() {
    FoxEngineApi real{};
    real.structSize = sizeof(real);
    real.submit = &fakeSubmit;
    real.submit_text = &fakeSubmitText;
    const FoxEngineApi* api = lockedApi(&real);
    CHECK(api != nullptr && api != &real && api->submit != real.submit);
    FrequencyLock& lk = FrequencyLock::instance();
    lk.setMask(0);
    lk.setSelected(1);
    lk.resetRefusals();

    // unlocked: everything passes
    g_seen = Seen{};
    FoxCommand one = cmd(FOXAPI_OP_SET_FREQUENCY);
    FoxSubmitResult r{};
    r.structSize = sizeof(r);
    CHECK(api->submit(nullptr, &one, 1, &r) == 1 && r.status == FOXAPI_OK && r.ticket == 100 && g_seen.ops.size() == 1);

    // radio 1 locked: its tuning is refused and never reaches the engine; the answer is a DENIED result with no ticket
    lk.setMask(1u);
    g_seen = Seen{};
    r = FoxSubmitResult{};
    r.structSize = sizeof(r);
    CHECK(api->submit(nullptr, &one, 1, &r) == 0 && r.status == FOXAPI_DENIED && r.ticket == 0 && g_seen.ops.empty());
    CHECK(lk.refusals() == 1);
    // the same op aimed at radio 2 goes through
    FoxCommand two = cmd(FOXAPI_OP_SET_FREQUENCY, 2);
    CHECK(api->submit(nullptr, &two, 1, &r) == 1 && g_seen.ops.size() == 1 && g_seen.targets[0] == 2u);
    // every kind of tuning gesture's op, one by one
    g_seen = Seen{};
    for (const std::uint32_t op : {FOXAPI_OP_SET_CENTRE, FOXAPI_OP_SET_VFO_OFFSET, FOXAPI_OP_STEP_TUNE, FOXAPI_OP_BOOKMARK_TUNE, FOXAPI_OP_PLUGIN_PRESET}) {
        FoxCommand c = cmd(op);
        CHECK(api->submit(nullptr, &c, 1, &r) == 0 && r.status == FOXAPI_DENIED);
    }
    FoxCommand scan = cmd(FOXAPI_OP_SCANNER_RUN, 0, 1);
    CHECK(api->submit(nullptr, &scan, 1, &r) == 0);
    CHECK(g_seen.ops.empty());
    // mode, volume and a scanner STOP still work
    for (const std::uint32_t op : {FOXAPI_OP_SET_MODE, FOXAPI_OP_SET_VOLUME, FOXAPI_OP_SET_BANDWIDTH}) {
        FoxCommand c = cmd(op);
        CHECK(api->submit(nullptr, &c, 1, &r) == 1 && r.status == FOXAPI_OK);
    }
    FoxCommand stop = cmd(FOXAPI_OP_SCANNER_RUN, 0, 0);
    CHECK(api->submit(nullptr, &stop, 1, &r) == 1);

    // a batch: the refused ones are answered DENIED in their places, the rest go on and keep their own answers
    g_seen = Seen{};
    FoxCommand batch[3] = {cmd(FOXAPI_OP_SET_FREQUENCY), cmd(FOXAPI_OP_SET_MODE), cmd(FOXAPI_OP_STEP_TUNE)};
    FoxSubmitResult rs[3] = {};
    for (auto& x : rs) { x.structSize = sizeof(x); }
    CHECK(api->submit(nullptr, batch, 3, rs) == 1);
    CHECK(g_seen.ops.size() == 1 && g_seen.ops[0] == FOXAPI_OP_SET_MODE);
    CHECK(rs[0].status == FOXAPI_DENIED && rs[1].status == FOXAPI_OK && rs[1].ticket == 100 && rs[2].status == FOXAPI_DENIED);

    // the text path is filtered too
    g_seen = Seen{};
    FoxCommand t = cmd(FOXAPI_OP_SET_FREQUENCY);
    CHECK(api->submit_text(nullptr, &t, "x", 1, &r) == 0 && r.status == FOXAPI_DENIED && g_seen.textCalls == 0);
    FoxCommand t2 = cmd(FOXAPI_OP_SETTING_SET);
    CHECK(api->submit_text(nullptr, &t2, "k=v", 3, &r) == 1 && g_seen.textCalls == 1);

    // unlocked again: passes
    lk.setMask(0);
    g_seen = Seen{};
    CHECK(api->submit(nullptr, &one, 1, &r) == 1 && g_seen.ops.size() == 1);
    lk.resetRefusals();
}

void drops() {
    CHECK(classifyDrop("C:\\rec\\a.wav") == DropKind::IqRecording);
    CHECK(classifyDrop("\"C:\\rec\\A.WAV\"") == DropKind::IqRecording);
    CHECK(classifyDrop("/home/u/x.Wav") == DropKind::IqRecording);
    CHECK(classifyDrop("freqs.xml") == DropKind::FrequencyList);
    CHECK(classifyDrop("C:\\a\\frequencies.CSV") == DropKind::FrequencyList);
    CHECK(classifyDrop("C:\\tools\\setup.exe") == DropKind::Unknown);
    CHECK(classifyDrop("notes.txt") == DropKind::Unknown);
    CHECK(classifyDrop("C:\\dir.with.dots\\noext") == DropKind::Unknown);
    CHECK(classifyDrop("") == DropKind::Unknown);
    CHECK(std::strncmp(manualUrl(), "https://", 8) == 0 && std::strstr(manualUrl(), "MANUAL.md") != nullptr);
}

}  // namespace

int main() {
    dotted();
    parse();
    tuningSet();
    lockState();
    lockedTable();
    drops();
    return testSummary("test_slint_host_gaps");
}
