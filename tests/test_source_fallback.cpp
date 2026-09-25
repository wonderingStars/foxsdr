// The radio that was recognised and then replaced by the signal generator
// (0.99.36). See src/gui/source_fallback.hpp for the report and the paths.
//
// Every case is about a thing the user was TOLD or a thing the NEXT LAUNCH
// does, because those are the two ways "it switches to an internal source"
// reaches a person: a screen that says nothing (or says the wrong thing), and a
// config that quietly names the generator.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>
#include <vector>

#include "gui/source_fallback.hpp"
#include "gui/tune_control.hpp"
#include "source/sdrplay_source.hpp"
#include "test_check.hpp"

namespace {

using cascade::gui::RememberedSource;
using cascade::gui::SourceRowKey;

// --- 1. the advice under "Device stopped" -----------------------------------
void testTheGenericAdviceNeverContradictsTheDriver() {
    using cascade::gui::reconnectAdviceApplies;
    // Every SDRplay sentence that follows a lost session: picking the radio
    // again cannot work, so "pick the source again" must not be printed under
    // it.
    CHECK(!reconnectAdviceApplies(cascade::source::sdrPlaySessionLostSentence()));
    CHECK(!reconnectAdviceApplies(std::string("retune: ") +
                                  cascade::source::sdrPlayControlHungSentence()));
    CHECK(!reconnectAdviceApplies(std::string("stream: ") +
                                  cascade::source::sdrPlayStreamStalledSentence()));
    // The ServiceNotResponding fault is composed inside the driver; this is
    // its shape ("<what>: <sentence>").
    CHECK(!reconnectAdviceApplies(
        "retune: the SDRplay service stopped answering - restart the SDRplay API service, then "
        "restart FoxSDR"));
    // An unplugged radio is exactly what the generic advice is for.
    CHECK(reconnectAdviceApplies("device removed: the RSP was unplugged or the service released it"));
    CHECK(reconnectAdviceApplies("source thread: bulk read failed (Windows error 31)"));
    CHECK(reconnectAdviceApplies(""));
}

// --- 2. a failed switch keeps the radio that worked --------------------------
void testAFailedSwitchKeepsTheRadioThatWorked() {
    using cascade::gui::rememberAfterFailedSwitch;
    using cascade::gui::rememberedSourceAfterFailedOpen;
    using cascade::gui::sourceToSave;

    // THE FIELD SEQUENCE. An RSP1A through the API was playing; its service
    // died; the user picked it again as the screen said; the receiver closed
    // it and the open was refused. Live now: the generator.
    const RememberedSource closed = rememberedSourceAfterFailedOpen(
        "sdrplay", "driver=sdrplay", "serial=1811003EFB", "", 2000000.0);
    CHECK(closed.valid());
    const RememberedSource kept = rememberAfterFailedSwitch(RememberedSource{}, closed);
    CHECK(kept.valid());
    CHECK(kept.kind == "sdrplay");
    CHECK(kept.nativeArgs == "serial=1811003EFB");

    // WHAT THE NEXT LAUNCH OPENS. Before 0.99.36 nothing was kept and this was
    // "siggen" - every later start on the generator.
    const cascade::gui::SavedSource saved =
        sourceToSave("siggen", "", "serial=1811003EFB", "", 2000000.0, kept);
    std::printf("after a failed re-open the config names \"%s\" (%s)\n", saved.kind.c_str(),
                saved.nativeArgs.c_str());
    CHECK(saved.kind == "sdrplay");
    CHECK(saved.nativeArgs == "serial=1811003EFB");
    CHECK(saved.sampleRateHz == 2000000.0);

    // A RADIO ALREADY REMEMBERED WINS: a startup restore that failed is what
    // the user's config named, and a later failed attempt from the generator
    // (nothing closed) must not replace it with nothing.
    const RememberedSource restoreKept = rememberedSourceAfterFailedOpen(
        "rtlsdr", "", "serial=00000001", "", 2400000.0);
    CHECK(rememberAfterFailedSwitch(restoreKept, RememberedSource{}).kind == "rtlsdr");
    CHECK(rememberAfterFailedSwitch(restoreKept, closed).kind == "rtlsdr");

    // Nothing closed and nothing kept: nothing to remember, the live source
    // (the generator) is the truth - a user who was on the generator and
    // tried a radio that failed is still on the generator next time.
    CHECK(!rememberAfterFailedSwitch(RememberedSource{}, RememberedSource{}).valid());
    // The generator itself is never "a radio that worked".
    CHECK(!rememberAfterFailedSwitch(
               RememberedSource{},
               rememberedSourceAfterFailedOpen("siggen", "", "", "", 2000000.0))
               .valid());
}

// --- 3. the sentence --------------------------------------------------------
void testTheSentenceSaysWhichRadioWhyAndWhereTheReceiverIs() {
    using cascade::gui::radioNotOpenedSentence;
    const std::string why = cascade::source::sdrPlaySessionLostSentence();

    const std::string s = radioNotOpenedSentence("SDRplay RSPdx", why);
    std::printf("shown: \"%s\"\n", s.c_str());
    CHECK(s.find("SDRplay RSPdx") == 0);                      // which radio, first
    CHECK(s.find(why) != std::string::npos);                  // why, verbatim
    CHECK(s.find("restart FoxSDR") != std::string::npos);     // what to do (the driver's words)
    CHECK(s.find("signal generator") != std::string::npos);   // where the receiver is
    CHECK(s.find("..") == std::string::npos);

    // A driver reason that already ends in a full stop is not doubled.
    const std::string t =
        radioNotOpenedSentence("RTL2838UHIDIR", "the dongle is in use by another program.");
    CHECK(t.find("program. The receiver") != std::string::npos);
    CHECK(t.find("..") == std::string::npos);

    // No reason given still says something whole.
    const std::string u = radioNotOpenedSentence("HackRF One", "");
    CHECK(u.find("HackRF One did not open. The receiver") == 0);
}

// --- 4. the selection across a re-scan ----------------------------------------
void testTheSelectionFollowsTheRadioNotTheIndex() {
    using cascade::gui::refindSourceRow;
    const SourceRowKey gen{"siggen", ""};
    const SourceRowKey file{"file", ""};
    const SourceRowKey rsp{"sdrplay", "serial=2208054321"};
    const SourceRowKey miri{"mirisdr", "serial=2208054321"};
    const SourceRowKey pluto{"pluto", "uri=ip:192.168.2.1"};
    const SourceRowKey b200{"soapy", "driver=uhd,serial=31"};

    // THE RSPdx CASE: its row is gone from a re-scan and the Pluto's slides
    // into index 2. Before 0.99.36 the combo ticked - and previewed - the
    // Pluto, and showed the Pluto's address box.
    CHECK(refindSourceRow({gen, file, rsp, pluto}, 2, {gen, file, pluto}) == -1);
    // ...and with the claimed row kept (withClaimedSdrPlayRows), it moves to
    // wherever the RSP now is.
    CHECK(refindSourceRow({gen, file, rsp, pluto}, 2, {gen, file, pluto, rsp}) == 3);
    // SAME ARGS, DIFFERENT DRIVER: the native Mirics row of the same RSP is
    // NOT the API row the receiver opened.
    CHECK(refindSourceRow({gen, file, rsp, pluto}, 2, {gen, file, miri, pluto}) == -1);
    // A Soapy selection shifted by a native row appearing.
    CHECK(refindSourceRow({gen, file, pluto, b200}, 3, {gen, file, rsp, pluto, b200}) == 4);
    // Rows 0 and 1 never move, and -1 stays -1.
    CHECK(refindSourceRow({gen, file, rsp}, 0, {gen, file}) == 0);
    CHECK(refindSourceRow({gen, file, rsp}, 1, {gen, file}) == 1);
    CHECK(refindSourceRow({gen, file, rsp}, -1, {gen, file, rsp}) == -1);
    // An index that was already stale stays "not a row".
    CHECK(refindSourceRow({gen, file}, 7, {gen, file, rsp}) == -1);
}

}  // namespace

int main() {
    testTheGenericAdviceNeverContradictsTheDriver();
    testAFailedSwitchKeepsTheRadioThatWorked();
    testTheSentenceSaysWhichRadioWhyAndWhereTheReceiverIs();
    testTheSelectionFollowsTheRadioNotTheIndex();
    return testSummary("test_source_fallback");
}
