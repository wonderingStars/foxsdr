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
// THE NAME CHECK IS A FUNCTION, AND IS TESTED AS ONE (nameWrittenInto, below,
// and checkTheNameCheckItself). Its first form - "does the line contain the
// device's name" - failed on the Linux CI runner although the product had done
// nothing wrong: the line there was
//   audio: output opened - ALSA, 2 channels, 48000 S/s, system default, latency 9 ms
// and ALSA hands PortAudio its PCM aliases as device names, verbatim
// (third_party/portaudio/src/hostapi/alsa/pa_linux_alsa.c, BuildDeviceList: the
// id of each entry under `pcm` in the ALSA configuration becomes the device's
// name, and the entry called "default" becomes the default output). "default" is
// a substring of the line's own words "system default", so the substring test
// reported a leak of a name that is the line's own vocabulary. The properties
// worth holding are the two the helper states: a name the line writes anywhere
// OUTSIDE its own fixed wording is a leak, and a name that is only a piece of
// that wording is not.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <portaudio.h>

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

// THE WORDS THE LINES CARRY BY CONSTRUCTION - every fixed phrase AudioOut::
// openLocked can write, plus the host API the open went through. Taken from
// src/sink/audio_out.cpp; if a phrase is reworded there and not here, a device
// whose name happens to be that phrase is reported as a leak, which is the
// loud direction to be wrong in.
std::vector<std::string> ownWords(const std::vector<std::string>& hostApis) {
    std::vector<std::string> w = {
        "audio: output opened - ",
        "audio: output could not be opened - ",
        "system default",
        "a chosen device",
        "channels",
        "channel",
        "S/s",
        "latency",
        "PortAudio did not start",
        "the request itself was unusable (a rate or a channel count)",
        "no default output device",
        "no such output device",
        "device has",
        "output channel(s)",
        "are needed",
        "refused the stream",
        "unknown host API",
    };
    for (const std::string& a : hostApis) {
        if (!a.empty()) { w.push_back(a); }
    }
    return w;
}

// Names shorter than this are not looked for: one to three characters match
// digits and punctuation the line has in every form ("2 channels", ", ").
constexpr std::size_t kShortestCheckedName = 4;

// Whether `name` is WRITTEN into `line`: true when it occurs anywhere that the
// line's own fixed wording does not account for. An occurrence that lies wholly
// inside the wording (the name "default" inside "system default", the name
// "WASAPI" inside the host API "Windows WASAPI") is the wording, not the name.
// EVERY occurrence is considered, so a name that appears once inside the
// wording and once outside it is still reported - the check cannot be satisfied
// by the wording happening to contain the name as well.
bool nameWrittenInto(const std::string& line, const std::string& name,
                     const std::vector<std::string>& hostApis) {
    if (name.size() < kShortestCheckedName) { return false; }
    std::vector<bool> own(line.size(), false);
    for (const std::string& phrase : ownWords(hostApis)) {
        for (std::size_t p = line.find(phrase); p != std::string::npos;
             p = line.find(phrase, p + 1)) {
            for (std::size_t i = 0; i < phrase.size(); ++i) { own[p + i] = true; }
        }
    }
    for (std::size_t p = line.find(name); p != std::string::npos; p = line.find(name, p + 1)) {
        bool explained = true;
        for (std::size_t i = 0; i < name.size() && explained; ++i) { explained = own[p + i]; }
        if (!explained) { return true; }
    }
    return false;
}

// Every host API PortAudio knows on this machine (valid while an AudioOut is
// alive: its constructor initialises PortAudio). A refusal names the host API of
// the device it was asked for, which need not be the one last opened.
std::vector<std::string> allHostApis() {
    std::vector<std::string> out;
    const PaHostApiIndex n = Pa_GetHostApiCount();
    for (PaHostApiIndex i = 0; i < n; ++i) {
        const PaHostApiInfo* api = Pa_GetHostApiInfo(i);
        if (api != nullptr && api->name != nullptr) { out.push_back(api->name); }
    }
    return out;
}

// NEVER THE DEVICE'S NAME: no output device on this machine is written into
// `line`. Every device is checked, because the one opened is not necessarily the
// default, and a device is reported by its index, not its name - the name is the
// thing this test exists to keep out of logs and out of pasted reports.
void checkNoDeviceNameIn(const std::string& line, const std::vector<AudioDevice>& devices,
                         const std::vector<std::string>& hostApis) {
    for (const AudioDevice& d : devices) {
        const bool leaked = nameWrittenInto(line, d.name, hostApis);
        if (leaked) { std::printf("  the line names output device #%d\n", d.index); }
        CHECK(!leaked);
    }
}

// THE CHECK, CHECKED. No hardware: lines and names are injected, so this runs
// the same way on every machine and does not depend on what sounds cards the
// machine has.
void checkTheNameCheckItself() {
    const std::vector<std::string> alsa = {"ALSA"};
    const std::vector<std::string> wasapi = {"MME", "Windows WASAPI"};

    // The line the Linux CI runner wrote, verbatim: it contains no device name.
    const std::string ci =
        "audio: output opened - ALSA, 2 channels, 48000 S/s, system default, latency 9 ms";

    // THE CI FAILURE, REPRODUCED. The original check was !contains(line, name).
    // ALSA names its default output "default": that check calls it a leak.
    CHECK(contains(ci, "default"));

    // -- NOT a leak: names that are only a piece of the line's own wording, or
    //    are not in the line at all. Every ALSA alias a runner can list among
    //    them, and the host API's own name and fragments of it.
    for (const char* alias : {"default", "sysdefault", "dmix", "front", "pulse", "iec958",
                              "surround40", "hdmi", "usbstream", "ALSA"}) {
        CHECK(!nameWrittenInto(ci, alias, alsa));
    }
    CHECK(!nameWrittenInto(ci, "system default", alsa));
    CHECK(!nameWrittenInto(ci, "latency", alsa));
    CHECK(!nameWrittenInto(ci, "channels", alsa));
    CHECK(!nameWrittenInto(ci, "Speakers (Realtek High Definition Audio)", alsa));
    CHECK(!nameWrittenInto(ci, "", alsa));  // an empty name is nothing to look for
    const std::string wasLine =
        "audio: output opened - Windows WASAPI, 2 channels, 48000 S/s, system default, "
        "latency 10 ms";
    for (const char* piece : {"Windows WASAPI", "WASAPI", "Windows"}) {
        CHECK(!nameWrittenInto(wasLine, piece, wasapi));  // a substring of the host API's name
    }
    const std::string chosen =
        "audio: output opened - MME, 1 channel, 48000 S/s, a chosen device, latency 90 ms";
    CHECK(!nameWrittenInto(chosen, "chosen device", wasapi));
    CHECK(!nameWrittenInto(chosen, "Speakers (Realtek High Definition Audio)", wasapi));
    const std::string refused = "audio: output could not be opened - no default output device";
    CHECK(!nameWrittenInto(refused, "default", alsa));
    CHECK(!nameWrittenInto(refused, "output device", alsa));

    // -- A LEAK: the name written where the line has no wording for it. Each of
    //    these is a product that did what the privacy rule forbids.
    // A card's name appended to the line.
    CHECK(nameWrittenInto(ci + " (Speakers (Realtek High Definition Audio))",
                          "Speakers (Realtek High Definition Audio)", alsa));
    // ...in place of the words "a chosen device".
    CHECK(nameWrittenInto(
        "audio: output opened - MME, 2 channels, 48000 S/s, Headphones (USB Audio Device), "
        "latency 90 ms",
        "Headphones (USB Audio Device)", wasapi));
    // THE ALIAS THE FIX EXISTS FOR, WRITTEN: the device NAMED "default" printed in
    // place of "system default" is still the name being written...
    CHECK(nameWrittenInto("audio: output opened - ALSA, 2 channels, 48000 S/s, default, "
                          "latency 9 ms",
                          "default", alsa));
    // ...and so is the same name printed IN ADDITION to the line's own "system
    // default" - the wording containing it must not excuse the extra copy.
    CHECK(nameWrittenInto(ci + " [default]", "default", alsa));
    CHECK(nameWrittenInto(ci + " (sysdefault)", "sysdefault", alsa));
    // A name that is a piece of the host API's, written somewhere else.
    CHECK(nameWrittenInto(wasLine + " (WASAPI)", "WASAPI", wasapi));
    // A refusal that names the device it was refused.
    CHECK(nameWrittenInto("audio: output could not be opened - Speakers (Realtek High "
                          "Definition Audio) refused the stream: Invalid sample rate",
                          "Speakers (Realtek High Definition Audio)", wasapi));
    // A name that straddles the wording and what follows it is not excused by
    // the part that overlaps.
    CHECK(nameWrittenInto("audio: output opened - ALSA, 2 channels, 48000 S/s, system default "
                          "monitor, latency 9 ms",
                          "default monitor", alsa));
}

}  // namespace

int main() {
    checkTheNameCheckItself();

    DiagLog::instance().configure(std::string(), false);  // ring only; no file
    DiagLog::instance().resetForTest();

    AudioOut ao;
    const std::vector<std::string> hostApis = allHostApis();

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
            checkNoDeviceNameIn(l, ao.listOutputDevices(), hostApis);
            // The generic aliases an ALSA machine lists as device names are the
            // line's own vocabulary, never a leak, whatever this machine lists.
            for (const char* alias : {"default", "sysdefault", "dmix", "front", "pulse"}) {
                CHECK(!nameWrittenInto(l, alias, hostApis));
            }
        }
        ao.close();
    }

    // --- A device opened BY INDEX is logged as a chosen one, and still not by name
    // The default-device open above says "system default" and never reaches the
    // wording a picked device gets. Here the default device is opened by its own
    // index and then up to three others, and each line is held to the same rule:
    // no device's name anywhere in it.
    {
        const std::vector<AudioDevice> devices = ao.listOutputDevices();
        std::vector<int> picked;
        for (const AudioDevice& d : devices) {
            if (d.isDefault) { picked.push_back(d.index); }
        }
        for (const AudioDevice& d : devices) {
            if (picked.size() >= 4u) { break; }
            if (!d.isDefault) { picked.push_back(d.index); }
        }
        int written = 0;
        for (const int index : picked) {
            const std::size_t before = outputLines().size();
            const bool ok = ao.open(index, 48000.0, 1);
            const std::vector<std::string> lines = outputLines();
            if (lines.size() == before + 1u) {
                const std::string& l = lines.back();
                ++written;
                if (ok) {
                    CHECK(contains(l, "audio: output opened"));
                    CHECK(contains(l, "a chosen device"));
                    CHECK(!contains(l, "system default"));
                    CHECK(contains(l, "1 channel"));
                    CHECK(!ao.openedHostApi().empty());
                    CHECK(contains(l, ao.openedHostApi().c_str()));
                } else {
                    CHECK(contains(l, "audio: output could not be opened"));
                }
                checkNoDeviceNameIn(l, devices, hostApis);
            } else {
                // A refusal for a reason already said writes nothing (the
                // once-a-minute rule above); a success always writes a line.
                CHECK(!ok);
            }
        }
        std::printf("opened %zu device(s) by index, %d line(s) written\n", picked.size(), written);
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
