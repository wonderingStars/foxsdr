// The strip, the audio mix, the status chips and the Add-a-radio card (src/slint/radio_mix_logic.hpp, docs/multi-radio-records/screen-s4.md, stage S4), with a MADE-UP ENGINE (the facts a
// real engine publishes as fact.radio.<n>.*): the MASTER lamps (one per open radio, lit when it runs, red when it has failed, dark when it is stopped), the selector following the page, STOP Rn
// against STOP ALL / START ALL, the audio route (cycle, words), the mix rows, the status chips ("--" where a radio has no state of its own, never another radio's), the card's devices (only
// those no open radio holds; what a second radio cannot open is not offered), and the question before a running or recording radio is removed.
// No window, no engine. The live engine path is shown by the drive scripts (tests/drive/deck-*.txt), not here.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "radio_mix_logic.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace foxsdr::slinthost;

namespace {

EngineReads engineWith(const std::map<std::string, std::string>& facts) {
    EngineReads e;
    e.fact = [facts](const char* name, std::string& out) -> int {
        const auto it = facts.find(name);
        if (it == facts.end()) { return -1; }
        out = it->second;
        return static_cast<int>(out.size());
    };
    return e;
}

FoxReceiverState stateWith(std::uint32_t flags, double tunedHz = 100.3e6, double volume = 0.5) {
    FoxReceiverState s{};
    s.structSize = sizeof(s);
    s.flags = flags;
    s.tunedHz = tunedHz;
    s.volume = volume;
    s.demodMode = FOXAPI_DEMOD_WFM;
    return s;
}

}  // namespace

int main() {
    std::printf("test_slint_radio_mix_host\n");

    // ---- the engine's words for a radio ----
    {
        const EngineReads e = engineWith({{"fact.radio.2.state", "running"}, {"fact.radio.2.device", "soapy|driver=rtlsdr"}, {"fact.radio.2.label", "RTL-SDR"}, {"fact.radio.2.audio", "left"}, {"fact.radio.3.state", "faulted"}, {"fact.radio.3.refusal", "in use by Radio 1"}});
        const RadioFacts f2 = readRadioFacts(e, 2), f3 = readRadioFacts(e, 3), f4 = readRadioFacts(e, 4);
        CHECK(f2.exists && f2.state == "running" && f2.device == "soapy|driver=rtlsdr" && f2.audio == "left" && f2.label == "RTL-SDR");
        CHECK(f3.exists && f3.state == "faulted" && f3.audio == "both" && f3.refusal == "in use by Radio 1");
        CHECK(!f4.exists && f4.state.empty());
        CHECK(!readRadioFacts(EngineReads{}, 2).exists);   // an engine with no facts
    }

    // ---- the audio route: the words, the cycle, an unknown word ----
    {
        CHECK(routeOfWord("both") == kRouteBoth && routeOfWord("left") == kRouteLeft && routeOfWord("right") == kRouteRight && routeOfWord("none") == kRouteNone);
        CHECK(routeOfWord("surround") == kRouteBoth && routeOfWord("") == kRouteBoth);
        for (int r = 0; r < kRouteCount; ++r) { CHECK(routeOfWord(routeWordOf(r)) == r); }
        CHECK(nextRoute(kRouteBoth) == kRouteLeft && nextRoute(kRouteLeft) == kRouteRight && nextRoute(kRouteRight) == kRouteNone && nextRoute(kRouteNone) == kRouteBoth);
        CHECK(nextRoute(-3) == kRouteLeft && nextRoute(77) == kRouteBoth);   // out of range counts as the end of the cycle
        CHECK(routeText(kRouteBoth) == "Both" && routeText(kRouteLeft) == "Left" && routeText(kRouteRight) == "Right" && routeText(kRouteNone) == "None");
    }

    // ---- the MASTER lamps: one per open radio, by state ----
    {
        std::array<RadioFacts, kMaxRadios> f{};
        std::array<FoxReceiverState, kMaxRadios> st{};
        std::uint32_t mask = 0;
        const auto open = [&](int slot, const char* word, std::uint32_t flags) { mask |= 1u << slot; f[static_cast<std::size_t>(slot)].exists = true; f[static_cast<std::size_t>(slot)].state = word; st[static_cast<std::size_t>(slot)] = stateWith(flags); };
        open(0, "running", FOXAPI_RX_RUNNING);
        open(1, "stopped", 0);
        open(2, "faulted", FOXAPI_RX_FAULTED);
        open(4, "running", FOXAPI_RX_RUNNING);              // a gap at slot 3: the lamp for radio 4 is not there
        const auto lamps = masterLamps(mask, 4, f.data(), st.data());
        CHECK(lamps.size() == 4);
        if (lamps.size() == 4) {
            CHECK(lamps[0].radio == 1 && lamps[0].state == MasterState::On && lamps[0].caption == "1" && !lamps[0].selected);
            CHECK(lamps[1].radio == 2 && lamps[1].state == MasterState::Off);
            CHECK(lamps[2].radio == 3 && lamps[2].state == MasterState::Failed);
            CHECK(lamps[3].radio == 5 && lamps[3].state == MasterState::On && lamps[3].selected);
            CHECK(lamps[3].name == "Radio 5 master" && lamps[3].tip.find("RUNNING") != std::string::npos);
            CHECK(lamps[2].tip.find("FAIL") != std::string::npos && lamps[1].tip.find("STOPPED") != std::string::npos);
        }
        // a fault read from the state alone is red as well
        f[1].state = "stopped"; st[1] = stateWith(FOXAPI_RX_FAULTED);
        CHECK(masterStateOf(f[1], st[1]) == MasterState::Failed);
        // a radio lent to the patch, or being opened, is dark and says so
        f[0].state = "lent";
        CHECK(masterStateOf(f[0], stateWith(0)) == MasterState::Off);
        CHECK(masterLamps(1u, 0, f.data(), st.data())[0].tip.find("LENT TO PATCH") != std::string::npos);
        f[0].state = "opening";
        CHECK(masterLamps(1u, 0, f.data(), st.data())[0].tip.find("OPENING") != std::string::npos);
        CHECK(masterLamps(0u, 0, f.data(), st.data()).empty());
    }

    // ---- the strip's selector follows the page ----
    {
        const std::uint32_t all8 = 0xFFu, three = 0x07u;
        CHECK((selectorRadios(kLayoutOne, 0, 0, all8) == std::vector<int>{1, 2, 3, 4}));
        CHECK((selectorRadios(kLayoutOne, 0, 5, all8) == std::vector<int>{5, 6, 7, 8}));          // One: the block of the selected radio
        CHECK((selectorRadios(kLayoutFour, 1, 0, all8) == std::vector<int>{5, 6, 7, 8}));         // 2 x 2, page two
        CHECK((selectorRadios(kLayoutFour, 0, 6, all8) == std::vector<int>{1, 2, 3, 4}));         // the page shown wins over the selection
        CHECK((selectorRadios(kLayoutTwo, 1, 0, all8) == std::vector<int>{1, 2, 3, 4}));          // Side by side, pair two (radios 3-4)
        CHECK((selectorRadios(kLayoutTwo, 2, 0, all8) == std::vector<int>{5, 6, 7, 8}));          // pair three (5-6)
        CHECK((selectorRadios(kLayoutFour, 0, 0, three) == std::vector<int>{1, 2, 3}));           // only the radios that exist
        CHECK(selectorRadios(kLayoutFour, 1, 0, three).empty());
        CHECK((selectorRadios(kLayoutOne, 0, 99, all8) == std::vector<int>{5, 6, 7, 8}));         // a wild slot is clamped
    }

    // ---- STOP Rn and STOP ALL / START ALL ----
    {
        const StripWords running = stripWords(3, true, true), someOther = stripWords(3, false, true), none = stripWords(2, false, false);
        CHECK(running.stopSelected == "STOP R3" && running.selectedRunning && running.stopAll == "STOP ALL" && running.anyRunning);
        CHECK(someOther.stopSelected == "STOP R3" && !someOther.selectedRunning && someOther.stopAll == "STOP ALL");
        CHECK(none.stopSelected == "STOP R2" && none.stopAll == "START ALL" && !none.anyRunning);
        CHECK(running.stopAllTip == "Stop every open radio" && none.stopAllTip == "Start every open radio");
    }

    // ---- the mix rows ----
    {
        RadioFacts f; f.exists = true; f.state = "running"; f.audio = "right";
        const MixRowData r = mixRow(3, f, stateWith(FOXAPI_RX_RUNNING | FOXAPI_RX_MUTED, 100e6, 0.72), 3);
        CHECK(r.radio == 3 && r.name == "Radio 3" && r.route == kRouteRight && r.routeText == "Right" && r.muted && r.solo && r.running);
        CHECK(r.routeLabel == "Output of Radio 3" && r.muteLabel == "Mute Radio 3" && r.soloLabel == "Solo Radio 3");
        CHECK(r.volumeText == "72%" && r.volume > 0.71f && r.volume < 0.73f && r.volumeLabel == "Radio 3: VOLUME");
        const MixRowData q = mixRow(2, f, stateWith(0, 100e6, 1.7), 3);
        CHECK(!q.muted && !q.solo && !q.running && q.volume == 1.0f);            // another radio is solo: this one is not; volume clamped
        CHECK(!mixRow(2, f, stateWith(0), 0).solo);
    }

    // ---- the status chips: a radio's own figures, "--" where it has none ----
    {
        RadioFacts f; f.exists = true; f.state = "running";
        const RadioChipData a = radioChip(2, f, stateWith(FOXAPI_RX_RUNNING, 145.5e6), true);
        CHECK(a.key == "R2" && a.value == "RUNNING" && a.tone == 1 && a.lampLit && !a.lampRed && a.radio == 2);
        CHECK(a.detail.find("145.500000 MHz") != std::string::npos && a.detail.find("WFM") != std::string::npos);
        const RadioChipData b = radioChip(3, f, stateWith(FOXAPI_RX_RUNNING, 145.5e6), false);
        CHECK(b.detail == "--");                                                  // never another radio's figure
        f.state = "faulted";
        const RadioChipData c = radioChip(4, f, stateWith(FOXAPI_RX_FAULTED), true);
        CHECK(c.value == "FAIL" && c.tone == 3 && c.lampRed);
        f.state = "stopped";
        const RadioChipData d = radioChip(5, f, stateWith(0), true);
        CHECK(d.value == "STOPPED" && d.tone == 0 && !d.lampLit);
        f.state = "lent";
        CHECK(radioChip(6, f, stateWith(0), true).value == "LENT TO PATCH");
    }

    // ---- the card: the devices not already held ----
    {
        const std::vector<DeviceRow> rows = {
            {0, "Signal generator", ""},
            {1, "I/Q file", ""},
            {2, "Sound card", ""},
            {3, "RTL-SDR #0", "rtlsdr:0"},                          // a native row
            {4, "Airspy Mini", "soapy:driver=airspy,serial=abc"},   // a Soapy row
            {5, "ADALM-PLUTO", "pluto:ip:192.168.2.1"},             // not a scanned device RADIO_ADD can open
            {6, "rtl_tcp", "rtltcp:127.0.0.1:1234"},
        };
        CHECK(deviceKeyOf(rows[3]) == "rtlsdr|0" && deviceKeyOf(rows[4]) == "soapy|driver=airspy,serial=abc" && deviceKeyOf(rows[0]).empty());
        const auto none = addChoices(rows, {});
        CHECK(none.size() == 3);
        if (none.size() == 3) {
            CHECK(none[0].id == "generator" && none[0].caption == "Signal generator" && none[0].accessible == "Add Signal generator");
            CHECK(none[1].id == "rtlsdr:0" && none[1].caption == "RTL-SDR #0 (native)");
            CHECK(none[2].id == "soapy:driver=airspy,serial=abc" && none[2].caption == "Airspy Mini");
        }
        const auto held = addChoices(rows, {"rtlsdr|0"});
        CHECK(held.size() == 2 && held[1].id == "soapy:driver=airspy,serial=abc");      // the held device is not offered
        CHECK(addChoices(rows, {"rtlsdr|0", "soapy|driver=airspy,serial=abc"}).size() == 1);   // only the generator is left, and it is always there: each radio has its own
        CHECK(addChoices({}, {}).empty());
        // the engine's answer: taken, or refused in its own words (shown unchanged, every holder named)
        CHECK(addOutcome(FOXAPI_OK, "").accepted && addOutcome(FOXAPI_NO_CHANGE, "that radio is open already").accepted && addOutcome(FOXAPI_NO_CHANGE, "that radio is open already").note.empty());
        const char* busy = "in use by Radio 1 and by patch radio 3: close one first";
        const char* limit = "At most 8 radios can be open at once. Open now: Radio 1, Radio 2, Radio 3, Radio 4, Radio 5, Radio 6, Radio 7, patch radio 1.";
        CHECK(!addOutcome(FOXAPI_BUSY, busy).accepted && addOutcome(FOXAPI_BUSY, busy).note == busy);
        CHECK(!addOutcome(FOXAPI_LIMIT, limit).accepted && addOutcome(FOXAPI_LIMIT, limit).note == limit);
        CHECK(!addOutcome(FOXAPI_NOT_FOUND, nullptr).accepted && addOutcome(FOXAPI_NOT_FOUND, nullptr).note.empty());
        CHECK(addCardTitle() == "Add a radio" && addCardRescan() == "Look for radios again" && addCardSlotName(3) == "Add a radio to pane 3");
    }

    // ---- removing a radio: a stopped one goes at once, a running or recording one asks first ----
    {
        CHECK(!removeNeedsConfirm(stateWith(0)));
        CHECK(removeNeedsConfirm(stateWith(FOXAPI_RX_RUNNING)));
        CHECK(removeNeedsConfirm(stateWith(FOXAPI_RX_RECORDING_IQ)));
        CHECK(removeNeedsConfirm(stateWith(FOXAPI_RX_RECORDING_AUDIO)));
        CHECK(!removeNeedsConfirm(stateWith(FOXAPI_RX_MUTED | FOXAPI_RX_DECODER_ACTIVE)));
        CHECK(removeQuestion(4) == "Remove Radio 4?");
        CHECK(removeExplain(stateWith(FOXAPI_RX_RUNNING)).find("running") != std::string::npos);
        CHECK(removeExplain(stateWith(FOXAPI_RX_RUNNING | FOXAPI_RX_RECORDING_IQ)).find("recording") != std::string::npos);
    }


    // ---- SCREEN STAGE S6, B3: the source kinds the engine accepts, and the sentences the pane shows ----
    {
        const auto kinds = addKindChoices();
        CHECK(kinds.size() == 4);
        if (kinds.size() == 4) {
            CHECK(kinds[0].entry && kinds[0].prefix == "file:" && kinds[0].caption == "I/Q file" && !kinds[0].placeholder.empty());
            CHECK(kinds[1].entry && kinds[1].prefix == "open-rtltcp:" && kinds[1].caption == "rtl_tcp");
            CHECK(kinds[2].entry && kinds[2].prefix == "open-pluto:");
            CHECK(!kinds[3].entry && kinds[3].id == "soundcard:" && kinds[3].caption == "Sound card");   // refused by the engine with its own sentence: offered, not hidden
            CHECK(kinds[0].accessible == "Add I/Q file" && kinds[3].accessible == "Add Sound card");
        }
        const std::vector<DeviceRow> rows = {{0, "Signal generator", ""}, {3, "RTL-SDR #0", "rtlsdr:0"}};
        const auto all = addAllChoices(rows, {});
        CHECK(all.size() == 2 + 4 && all[0].id == "generator" && all[1].id == "rtlsdr:0" && all[2].prefix == "file:");
        CHECK(addAllChoices(rows, {"rtlsdr|0"}).size() == 1 + 4);   // the held device goes, the kinds stay
        CHECK(addAllChoices({}, {}).size() == 4);
        // what is typed becomes RADIO_ADD's text; nothing typed sends nothing
        CHECK(addIdFor(all[2], "C:/iq/test.wav") == "file:C:/iq/test.wav");
        CHECK(addIdFor(all[2], "  \"C:/iq/a b.wav\"  ") == "file:C:/iq/a b.wav");   // a pasted path in quotes
        CHECK(addIdFor(all[2], "").empty() && addIdFor(all[2], "   ").empty());
        CHECK(addIdFor(all[3], "192.168.1.5:1234") == "open-rtltcp:192.168.1.5:1234");
        CHECK(addIdFor(all[4], "ip:pluto.local") == "open-pluto:ip:pluto.local");
        CHECK(addIdFor(all[5], "ignored") == "soundcard:" && addIdFor(all[0], "x") == "generator" && addIdFor(all[1], "x") == "rtlsdr:0");
        // THE SOLO READ-BACK (B4): the engine's fact, only for a radio that is open
        CHECK(soloFromFact("3", 0x7u) == 3 && soloFromFact("1", 0x1u) == 1 && soloFromFact("0", 0xFFu) == 0 && soloFromFact("", 0xFFu) == 0);
        CHECK(soloFromFact("3", 0x3u) == 0 && soloFromFact("9", 0xFFu) == 0 && soloFromFact("x", 0xFFu) == 0 && soloFromFact("-2", 0xFFu) == 0 && soloFromFact("8", 0x80u) == 8);
        // the pane's notice: the radio's own refusal first, else its line of fact.radio.restore (without the "Radio n: " the pane already names)
        const std::string restore = "Radio 2: RTL-SDR did not open: busy\nRadio 4: Airspy is not plugged in";
        CHECK(paneNotice("in use by Radio 1", restore, 2) == "in use by Radio 1");
        CHECK(paneNotice("", restore, 2) == "RTL-SDR did not open: busy");
        CHECK(paneNotice("", restore, 4) == "Airspy is not plugged in");
        CHECK(paneNotice("", restore, 3).empty() && paneNotice("", "", 2).empty());
        CHECK(paneNotice("", "Radio 22: not this one", 2).empty());
    }

    return testSummary("test_slint_radio_mix_host");
}
