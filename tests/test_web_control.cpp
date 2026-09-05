// Tests for control-request parsing (net/web_control.hpp).
//
// This is the validation layer in front of the only endpoint that MOVES the
// radio, so almost everything below asserts a refusal. The parse is pure, so
// each refusal can be exercised exactly, which is the whole reason it was
// separated from the handler that queues the result.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <string>

#include "dsp/demod.hpp"
#include "net/web_control.hpp"
#include "test_check.hpp"

using namespace cascade::net;
using cascade::dsp::DemodMode;

namespace {

// Parses and reports only whether it was accepted, for the many cases where
// the reason text is not the point.
bool accepts(const std::string& body) {
    ControlRequest r;
    std::string error;
    return parseControlRequest(body, r, error);
}

void testAcceptsEachField() {
    ControlRequest r;
    std::string error;

    CHECK(parseControlRequest("{\"running\":true}", r, error));
    CHECK(error.empty());
    CHECK(r.running.has_value());
    CHECK(r.running.value_or(false));
    // Absent fields must stay absent, not acquire a default that would be
    // applied to the radio as though it had been asked for.
    CHECK(!r.centerHz.has_value());
    CHECK(!r.mode.has_value());
    CHECK(!r.volume.has_value());

    CHECK(parseControlRequest("{\"running\":false}", r, error));
    CHECK(r.running.has_value());
    CHECK(!r.running.value_or(true));

    CHECK(parseControlRequest("{\"centerHz\":144800000}", r, error));
    CHECK(r.centerHz.value_or(0.0) == 144800000.0);

    CHECK(parseControlRequest("{\"vfoOffsetHz\":-125000.5}", r, error));
    CHECK(r.vfoOffsetHz.value_or(0.0) == -125000.5);

    CHECK(parseControlRequest("{\"bandwidthHz\":12500}", r, error));
    CHECK(r.bandwidthHz.value_or(0.0) == 12500.0);

    CHECK(parseControlRequest("{\"squelchDb\":-63.5}", r, error));
    CHECK(r.squelchDb.value_or(0.0) == -63.5);

    CHECK(parseControlRequest("{\"volume\":0.25}", r, error));
    CHECK(r.volume.value_or(-1.0) == 0.25);

    // Several at once, which is the shape the page actually sends.
    CHECK(parseControlRequest(
        "{\"centerHz\":95500000,\"mode\":\"WFM\",\"bandwidthHz\":150000}", r, error));
    CHECK(r.centerHz.value_or(0.0) == 95500000.0);
    CHECK(r.bandwidthHz.value_or(0.0) == 150000.0);
    CHECK(r.mode.value_or(DemodMode::RAW) == DemodMode::WFM);
    CHECK(!r.running.has_value());
}

void testEveryModeName() {
    // Every mode the DSP layer names must be accepted, and must come back as
    // the same mode. This is what stops the browser's vocabulary drifting from
    // the receiver's.
    for (std::size_t i = 0; i < cascade::dsp::kDemodModeCount; ++i) {
        const auto m = static_cast<DemodMode>(i);
        const std::string body = std::string("{\"mode\":\"") +
                                 cascade::dsp::modeName(m) + "\"}";
        ControlRequest r;
        std::string error;
        const bool ok = parseControlRequest(body, r, error);
        CHECK(ok);
        if (ok) {
            CHECK(r.mode.has_value());
            CHECK(r.mode.value_or(DemodMode::RAW) == m);
        }
    }
}

void testRejectsBadModes() {
    CHECK(!accepts("{\"mode\":\"FM\"}"));
    CHECK(!accepts("{\"mode\":\"\"}"));
    // Exact and case-sensitive: a client sending lower case has a bug, and
    // guessing at it would hide the bug rather than fix it.
    CHECK(!accepts("{\"mode\":\"wfm\"}"));
    CHECK(!accepts("{\"mode\":\"Wfm\"}"));
    CHECK(!accepts("{\"mode\":\" WFM\"}"));
    CHECK(!accepts("{\"mode\":7}"));
    CHECK(!accepts("{\"mode\":null}"));
}

void testRejectsMalformedBodies() {
    CHECK(!accepts(""));
    CHECK(!accepts("not json"));
    CHECK(!accepts("[1,2,3]"));       // array root
    CHECK(!accepts("42"));            // number root
    CHECK(!accepts("\"running\""));   // string root
    CHECK(!accepts("null"));
    // An empty object meant to say something and did not.
    CHECK(!accepts("{}"));
}

void testRejectsUnknownFields() {
    // The rule that separates this from the config store: an unknown key in a
    // LIVE instruction is a client bug, and answering "accepted" to a request
    // that will not be carried out is worse than refusing it.
    CHECK(!accepts("{\"centreHz\":100000000}"));   // British spelling
    CHECK(!accepts("{\"freqHz\":100000000}"));
    CHECK(!accepts("{\"CenterHz\":100000000}"));   // wrong case
    CHECK(!accepts("{\"gain\":30}"));
    // ...even alongside fields that ARE valid, so a typo cannot ride along
    // with a good field and appear to have worked.
    CHECK(!accepts("{\"centerHz\":100000000,\"gaain\":30}"));

    ControlRequest r;
    std::string error;
    CHECK(!parseControlRequest("{\"gaain\":30}", r, error));
    CHECK(error.find("gaain") != std::string::npos);  // names the offender
}

void testRejectsWrongTypes() {
    CHECK(!accepts("{\"running\":1}"));         // number, not boolean
    CHECK(!accepts("{\"running\":\"true\"}"));
    CHECK(!accepts("{\"centerHz\":\"100e6\"}"));
    CHECK(!accepts("{\"centerHz\":null}"));
    CHECK(!accepts("{\"centerHz\":[100]}"));
    CHECK(!accepts("{\"volume\":\"loud\"}"));
    CHECK(!accepts("{\"squelchDb\":true}"));    // a bool is not a number
}

void testRejectsOutOfRange() {
    CHECK(!accepts("{\"centerHz\":-1}"));
    CHECK(!accepts("{\"centerHz\":7e9}"));
    CHECK(!accepts("{\"vfoOffsetHz\":2e8}"));
    CHECK(!accepts("{\"vfoOffsetHz\":-2e8}"));
    CHECK(!accepts("{\"bandwidthHz\":0}"));
    CHECK(!accepts("{\"bandwidthHz\":-1000}"));
    CHECK(!accepts("{\"bandwidthHz\":2e7}"));
    CHECK(!accepts("{\"squelchDb\":-500}"));
    CHECK(!accepts("{\"squelchDb\":100}"));
    CHECK(!accepts("{\"volume\":-0.1}"));
    CHECK(!accepts("{\"volume\":1.5}"));

    // The boundaries themselves are IN range.
    CHECK(accepts("{\"centerHz\":0}"));
    CHECK(accepts("{\"volume\":0}"));
    CHECK(accepts("{\"volume\":1}"));
    CHECK(accepts("{\"squelchDb\":-200}"));
    CHECK(accepts("{\"squelchDb\":20}"));
    CHECK(accepts("{\"bandwidthHz\":100}"));
}

void testRejectsNonFinite() {
    // JSON has no literal for these, so they arrive as the text a lax encoder
    // emits. Either the parser refuses them outright or the finite check does;
    // what must never happen is a NaN centre frequency reaching a source
    // setter, where no later clamp can repair it.
    CHECK(!accepts("{\"centerHz\":NaN}"));
    CHECK(!accepts("{\"centerHz\":Infinity}"));
    CHECK(!accepts("{\"centerHz\":-Infinity}"));
    CHECK(!accepts("{\"volume\":NaN}"));
    // A finite value expressed in exponential form is fine.
    CHECK(accepts("{\"centerHz\":1.0e8}"));
}

void testDisplayAndAudioFields() {
    ControlRequest r;
    std::string error;

    CHECK(parseControlRequest("{\"dbMin\":-95,\"dbMax\":-10}", r, error));
    CHECK(r.dbMin.value_or(0.0) == -95.0);
    CHECK(r.dbMax.value_or(0.0) == -10.0);

    // A display range is only meaningful as a pair: inverted or degenerate
    // divides by zero where dB is mapped to pixels.
    CHECK(!accepts("{\"dbMin\":0,\"dbMax\":-50}"));      // inverted
    CHECK(!accepts("{\"dbMin\":-20,\"dbMax\":-15}"));    // 5 dB span
    CHECK(!accepts("{\"dbMin\":-80,\"dbMax\":-70}"));    // exactly 10, strict
    CHECK(accepts("{\"dbMin\":-80,\"dbMax\":-69}"));
    // One end alone is fine — the application applies the span rule against
    // the end it already holds.
    CHECK(accepts("{\"dbMin\":-120}"));
    CHECK(accepts("{\"dbMax\":10}"));

    CHECK(parseControlRequest("{\"deemphasisIndex\":1}", r, error));
    CHECK(r.deemphasisIndex.value_or(-1) == 1);
    // Indexes a THREE-entry table; out of range would read past it.
    CHECK(!accepts("{\"deemphasisIndex\":3}"));
    CHECK(!accepts("{\"deemphasisIndex\":-1}"));
    CHECK(!accepts("{\"deemphasisIndex\":1.5}"));
    CHECK(accepts("{\"deemphasisIndex\":0}"));
    CHECK(accepts("{\"deemphasisIndex\":2}"));

    CHECK(parseControlRequest(
        "{\"nrEnabled\":true,\"nrStrength\":0.75,\"notchEnabled\":true,"
        "\"notchFreqHz\":700,\"notchQ\":25,\"autoNotch\":true,\"stereoEnabled\":false}",
        r, error));
    CHECK(r.nrEnabled.value_or(false));
    CHECK(r.nrStrength.value_or(0.0) == 0.75);
    CHECK(r.notchEnabled.value_or(false));
    CHECK(r.notchFreqHz.value_or(0.0) == 700.0);
    CHECK(r.notchQ.value_or(0.0) == 25.0);
    CHECK(r.autoNotch.value_or(false));
    CHECK(!r.stereoEnabled.value_or(true));

    // Every new boolean is strict, like running.
    CHECK(!accepts("{\"nrEnabled\":1}"));
    CHECK(!accepts("{\"notchEnabled\":\"yes\"}"));
    CHECK(!accepts("{\"autoNotch\":null}"));
    CHECK(!accepts("{\"stereoEnabled\":\"true\"}"));

    // Ranges, matching what the config store sanitizes to.
    CHECK(!accepts("{\"nrStrength\":1.5}"));
    CHECK(!accepts("{\"nrStrength\":-0.1}"));
    CHECK(!accepts("{\"notchFreqHz\":5}"));
    CHECK(!accepts("{\"notchFreqHz\":30000}"));
    CHECK(!accepts("{\"notchQ\":0}"));
    CHECK(!accepts("{\"notchQ\":2000}"));
    CHECK(accepts("{\"notchFreqHz\":10}"));
    CHECK(accepts("{\"notchFreqHz\":20000}"));
    CHECK(accepts("{\"notchQ\":0.1}"));
    CHECK(accepts("{\"notchQ\":1000}"));
}

void testSourceFields() {
    ControlRequest r;
    std::string error;

    CHECK(parseControlRequest("{\"sourceKind\":\"siggen\"}", r, error));
    CHECK(r.sourceKind.value_or("") == "siggen");
    CHECK(parseControlRequest(
        "{\"sourceKind\":\"soapy\",\"soapyArgs\":\"driver=uhd,serial=ABC\"}", r, error));
    CHECK(r.soapyArgs.value_or("") == "driver=uhd,serial=ABC");

    // "file" IS REFUSED ON PURPOSE. Letting a network client name a path on
    // the host is a file-read primitive dressed as a source selector, and that
    // is a capability the owner should choose to add, not one that arrives
    // with a feature.
    CHECK(!accepts("{\"sourceKind\":\"file\"}"));
    CHECK(!accepts("{\"sourceKind\":\"file\",\"iqFilePath\":\"C:/secrets.wav\"}"));
    // ...and iqFilePath is not even a field this version knows.
    CHECK(!accepts("{\"iqFilePath\":\"C:/secrets.wav\"}"));
    CHECK(!accepts("{\"sourceKind\":\"rtlsdr\"}"));
    CHECK(!accepts("{\"sourceKind\":\"\"}"));
    CHECK(!accepts("{\"sourceKind\":7}"));

    CHECK(parseControlRequest("{\"antenna\":\"TX/RX\"}", r, error));
    CHECK(r.antenna.value_or("") == "TX/RX");
    CHECK(parseControlRequest("{\"scanDevices\":true}", r, error));
    CHECK(r.scanDevices.value_or(false));
    CHECK(!accepts("{\"scanDevices\":1}"));

    CHECK(parseControlRequest("{\"sampleRateHz\":2000000}", r, error));
    CHECK(r.sampleRateHz.value_or(0.0) == 2000000.0);
    CHECK(!accepts("{\"sampleRateHz\":100}"));      // below any SDR's range
    CHECK(!accepts("{\"sampleRateHz\":1e9}"));

    // A gain is a NAME and a VALUE; either alone cannot be acted on, and
    // accepting half would be a control that reported success and did nothing.
    CHECK(parseControlRequest("{\"gainName\":\"PGA\",\"gainDb\":42}", r, error));
    CHECK(r.gainName.value_or("") == "PGA");
    CHECK(r.gainDb.value_or(0.0) == 42.0);
    CHECK(!accepts("{\"gainName\":\"PGA\"}"));
    CHECK(!accepts("{\"gainDb\":42}"));
    CHECK(!accepts("{\"gainName\":\"PGA\",\"gainDb\":500}"));

    // Strings are bounded, so a hostile client cannot make the application
    // hold megabytes of nonsense while it is matched against a list.
    const std::string huge(300, 'x');
    CHECK(!accepts("{\"antenna\":\"" + huge + "\"}"));
    CHECK(!accepts("{\"soapyArgs\":\"" + huge + "\"}"));
    CHECK(accepts("{\"antenna\":\"" + std::string(256, 'x') + "\"}"));
}

void testRecorderBookmarkScannerFields() {
    ControlRequest r;
    std::string error;

    CHECK(parseControlRequest("{\"recordIq\":true,\"recordAudio\":false}", r, error));
    CHECK(r.recordIq.value_or(false));
    CHECK(r.recordAudio.has_value());
    CHECK(!r.recordAudio.value_or(true));
    CHECK(!accepts("{\"recordIq\":1}"));
    // The recording DIRECTORY is not a field: it would let a network client
    // choose where the application writes on the host.
    CHECK(!accepts("{\"recordDir\":\"C:/anywhere\"}"));

    CHECK(parseControlRequest("{\"bookmarkAdd\":\"BBC Lancashire\"}", r, error));
    CHECK(r.bookmarkAdd.value_or("") == "BBC Lancashire");
    CHECK(!accepts("{\"bookmarkAdd\":\"\"}"));
    CHECK(!accepts("{\"bookmarkAdd\":42}"));

    CHECK(parseControlRequest("{\"bookmarkTune\":3}", r, error));
    CHECK(r.bookmarkTune.value_or(-1) == 3);
    CHECK(parseControlRequest("{\"bookmarkRemove\":0}", r, error));
    CHECK(r.bookmarkRemove.value_or(-1) == 0);
    CHECK(!accepts("{\"bookmarkTune\":-1}"));
    CHECK(!accepts("{\"bookmarkRemove\":-5}"));
    CHECK(!accepts("{\"bookmarkTune\":1.5}"));
    CHECK(!accepts("{\"bookmarkTune\":\"0\"}"));

    CHECK(parseControlRequest(
        "{\"scannerActive\":true,\"scanStartHz\":88000000,\"scanStopHz\":108000000,"
        "\"scanStepHz\":100000}", r, error));
    CHECK(r.scannerActive.value_or(false));
    CHECK(r.scanStartHz.value_or(0.0) == 88000000.0);
    CHECK(r.scanStopHz.value_or(0.0) == 108000000.0);
    CHECK(r.scanStepHz.value_or(0.0) == 100000.0);
    // A range that runs backwards, or has no width, cannot be scanned.
    CHECK(!accepts("{\"scanStartHz\":108000000,\"scanStopHz\":88000000}"));
    CHECK(!accepts("{\"scanStartHz\":90000000,\"scanStopHz\":90000000}"));
    CHECK(!accepts("{\"scanStepHz\":0}"));
    CHECK(!accepts("{\"scannerActive\":\"yes\"}"));
    // One end alone is fine — the application holds the other.
    CHECK(accepts("{\"scanStartHz\":88000000}"));
}

void testFailureLeavesNothingBehind() {
    // A caller that ignores the return value must not find a usable request
    // sitting in `out` from a body that was refused.
    ControlRequest r;
    std::string error;
    CHECK(parseControlRequest("{\"centerHz\":100000000}", r, error));
    CHECK(r.centerHz.has_value());

    CHECK(!parseControlRequest("{\"centerHz\":\"nonsense\"}", r, error));
    CHECK(!error.empty());
    CHECK(r.empty());
    CHECK(!r.centerHz.has_value());
}

}  // namespace

int main() {
    testAcceptsEachField();
    testEveryModeName();
    testRejectsBadModes();
    testRejectsMalformedBodies();
    testRejectsUnknownFields();
    testRejectsWrongTypes();
    testRejectsOutOfRange();
    testRejectsNonFinite();
    testDisplayAndAudioFields();
    testSourceFields();
    testRecorderBookmarkScannerFields();
    testFailureLeavesNothingBehind();
    return testSummary("test_web_control");
}
