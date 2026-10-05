// Every attempt to open the sound output says so in the log, and says whether
// it worked - so a "no audio from my speakers" report answers its first
// question itself.
//
// THE FIELD REPORT. FoxSDR 0.99.58, an NESDR SMArt v5, "no audio from my
// speakers". Five minutes of log, and not one line said whether an output
// device had been opened at all: the only audio line the application writes is
// the starvation digest, which is SILENT when nothing starved - and nothing
// starves a stream that is open and being fed zeros (a closed squelch, the
// mute, a volume of nothing), nothing starves a stream whose callback never
// runs, and the digest is not even evaluated when no device ever opened. Four
// different situations, one identical log. The Sinks panel could have told
// them apart, and the report did not carry it.
//
// WHAT THIS PINS. AudioOut::open() writes exactly one "audio: output ..." line
// per attempt that changes the story:
//   - success: the host API, the channel count, the rate, and whether it was
//     the system default or a chosen device;
//   - failure: the reason in words, and a refusal that REPEATS (the watchdog
//     retries a dead stream once a second) is said once, not sixty times a
//     minute, because the ring holds 256 lines and a retry loop would push the
//     whole session out of it.
// The device's NAME is never in the line (the log goes into a bundle people
// paste into public bug reports, and an operating-system device label is often
// a person's name - see source::loggableSoundCardDescription).
//
// No hardware is assumed. A request for a device that cannot exist fails the
// same way on every machine; a request for the default device either opens or
// fails depending on the machine, and the test requires the matching line
// whichever happened.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include "core/diag_log.hpp"
#include "sink/audio_out.hpp"
#include "test_check.hpp"

using cascade::core::DiagLog;
using cascade::sink::AudioDevice;
using cascade::sink::AudioOut;

namespace {

// The "audio: output ..." lines in the ring, oldest first.
std::vector<std::string> outputLines() {
    std::vector<std::string> out;
    for (const std::string& l : DiagLog::instance().ringSnapshot()) {
        if (l.find("audio: output ") != std::string::npos) { out.push_back(l); }
    }
    return out;
}

bool contains(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

}  // namespace

int main() {
    DiagLog::instance().configure(std::string(), false);  // ring only; no file
    DiagLog::instance().resetForTest();

    AudioOut ao;

    // --- A refused open is logged, with a reason ----------------------------
    {
        CHECK(!ao.open(1 << 20, 48000.0, 1));  // a device index nothing has
        const std::vector<std::string> lines = outputLines();
        std::printf("after a refused open: %zu line(s)\n", lines.size());
        for (const std::string& l : lines) { std::printf("  %s\n", l.c_str()); }
        CHECK(lines.size() == 1u);
        if (lines.size() == 1u) {
            CHECK(contains(lines[0], "audio: output could not be opened"));
            // A reason, not just a verdict.
            CHECK(contains(lines[0], "no such"));
        }
    }

    // --- ...and the same refusal repeated is said ONCE ----------------------
    // The audio watchdog retries a dead stream once a second; a line per retry
    // would fill the log's 256 lines in four minutes and push out everything
    // that explains why the stream died.
    {
        for (int i = 0; i < 20; ++i) { CHECK(!ao.open(1 << 20, 48000.0, 1)); }
        CHECK(outputLines().size() == 1u);
    }

    // --- A DIFFERENT refusal is a new fact and is logged --------------------
    {
        CHECK(!ao.open(-1, -1.0, 1));  // nonsense rate
        const std::vector<std::string> lines = outputLines();
        CHECK(lines.size() == 2u);
        if (lines.size() == 2u) { CHECK(contains(lines[1], "audio: output could not be opened")); }
    }

    // --- An open of the default device is logged whichever way it goes ------
    {
        const std::size_t before = outputLines().size();
        const bool ok = ao.open(-1, 48000.0, 2);
        const std::vector<std::string> lines = outputLines();
        std::printf("default device open %s: %zu line(s) now\n", ok ? "worked" : "failed",
                    lines.size());
        CHECK(lines.size() == before + 1u);
        if (lines.size() == before + 1u) {
            const std::string& l = lines.back();
            std::printf("  %s\n", l.c_str());
            if (ok) {
                CHECK(contains(l, "audio: output opened"));
                CHECK(contains(l, "2 channels"));
                CHECK(contains(l, "48000 S/s"));
                CHECK(contains(l, "system default"));
                // The host API is named - it is the first thing anyone asks
                // about a Windows audio fault (MME, WASAPI, DirectSound, WDM-KS).
                CHECK(!ao.openedHostApi().empty());
                CHECK(contains(l, ao.openedHostApi().c_str()));
            } else {
                // A machine with no usable output (a build agent): the same
                // line, saying so.
                CHECK(contains(l, "audio: output could not be opened"));
            }
            // NEVER THE DEVICE'S NAME. Every output device on this machine is
            // checked, because the default is not necessarily the one opened.
            for (const AudioDevice& d : ao.listOutputDevices()) {
                if (d.name.size() >= 4u) { CHECK(!contains(l, d.name.c_str())); }
            }
        }
        ao.close();
    }

    // --- A success after a refusal is logged: the recovery is visible -------
    // (The watchdog's reopen is an ordinary open(); the line it writes is what
    // tells a reader the stream died and came back.) Skipped where the machine
    // has no output at all, where there is nothing to recover to.
    {
        if (ao.open(-1, 48000.0, 1)) {
            const std::size_t before = outputLines().size();
            CHECK(!ao.open(1 << 20, 48000.0, 1));  // a refusal closes the stream first
            CHECK(ao.open(-1, 48000.0, 1));        // and this is the recovery
            const std::vector<std::string> lines = outputLines();
            CHECK(lines.size() >= before + 1u);
            if (!lines.empty()) { CHECK(contains(lines.back(), "audio: output opened")); }
            CHECK(contains(lines.empty() ? std::string() : lines.back(), "1 channel"));
            ao.close();
        } else {
            std::printf("SKIP-ish: no output device opens on this machine; recovery line not "
                        "exercised\n");
        }
    }

    return testSummary("test_audio_open_log");
}
