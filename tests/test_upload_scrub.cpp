// Tests that NO log line leaves the machine carrying a tuned, centre or
// transmit frequency, a hardware serial, a user-typed name or the user's
// account name - whoever wrote the line.
//
// WHY THIS FILE EXISTS. Until 0.99.33 the scrub (core::scrubVendorLine) ran
// only on DRIVER lines - the SoapySDR logger and captured stderr. The
// application's own lines went into crash reports, freeze reports and the
// diagnostics bundle verbatim, and at least five of them printed frequencies:
// "source: asked for 433.917000 MHz ...", "patch: radio 'Loft Airband'
// running ... 127.825000 MHz", "tx: keyed - FM at 145.487500 MHz", and the
// native Airspy driver printed its serial in a form ("AIRSPY_SN:...") the
// serial rule did not recognise. PRIVACY.md promised otherwise.
//
// HOW IT TESTS. The lines are written into the REAL log ring with the REAL
// format strings the shipped call sites used (so an older build's report, or a
// new call site written the old way, is covered), and then taken out through
// EVERY assembly function that puts log lines into something that leaves the
// machine:
//   1. a crash report - built from DiagLog::copyRingRaw, exactly as
//      crash_handler.cpp writes one, parsed and turned into the upload body by
//      parseReportText + uploadJson;
//   2. a freeze report - built from DiagLog::ringSnapshot, exactly as
//      hang_watchdog.cpp writes one, through the same two functions;
//   3. the diagnostics bundle - buildDiagnosticsBundle.
// Each is checked for the sensitive values (absent) AND for what the log is
// for - sample rates, error codes, counts, durations, versions, chip names
// (present), and a line with nothing sensitive in it must come through
// byte-for-byte.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/crash_upload.hpp"
#include "core/diag_log.hpp"
#include "core/diag_report.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "test_check.hpp"

using cascade::core::DiagLog;

namespace {

std::string lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

// The log line with its "HH:MM:SS.mmm " stamp removed, so a millisecond field
// that happens to read "433" cannot be mistaken for a leaked frequency.
std::string unstamped(const std::string& line) {
    if (line.size() >= 13 && line[2] == ':' && line[5] == ':' && line[8] == '.') {
        return line.substr(13);
    }
    return line;
}

std::string joinUnstamped(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& l : lines) {
        out += unstamped(l);
        out += "\n";
    }
    return out;
}

// Every value that must NOT leave the machine, lower-cased.
const std::vector<std::string>& forbidden() {
    static const std::vector<std::string> v = {
        // source: asked for / answered / range is
        "433.917", "433917", "433.91", "1766", "24.000000",
        // patch: radio '...' running ... at ... MHz
        "127.825", "loft airband", "00000417",
        // tx: keyed / tx: started
        "145.4875", "145.487",
        // the native Airspy's own open line
        "26a464dc28593e93",
        // the PLL warning
        "1090.0000", "1090",
        // a plugin's preset label
        "131.725",
        // the Windows account in a path
        "alice.smith",
        // a user-typed speaker name
        "kitchen radio",
        // a driver's centre frequency, masked digit by digit at capture
        "101100000",
        // Windows USB instance ids (device serials or location ids) from a
        // real 0.99.31 report, and the unrelated devices they inventory
        "7e59240920a2", "3032363330303934343834393433", "077233483938", "98f1ccd",
        "vid_0db0", "vid_045e", "vid_046d",
        // a Pluto's address and host name, and a network driver's peer
        "192.168.2.1", "shack-pluto", "192.168.1.77",
    };
    return v;
}

// What the log is FOR, which must survive the scrub.
const std::vector<std::string>& required() {
    static const std::vector<std::string> v = {
        "2048000 S/s",            // a patch radio's sample rate
        "2000000 S/s",            // the transmit sample rate
        "-10.00 dB",              // the transmit power
        "2400000 S/s",            // the receiver's sample rate
        "(-5)",                   // a driver error code
        "3 tunes",                // a count
        "12 ms",                  // a duration
        "v1.0.0-rc10-6-g4008185", // a firmware version on a line that also names a serial
        "adsb-decoder 1.8.0",     // a plugin version
        "0xC0000005",             // an exception code
        "R820T",                  // a tuner chip on a line that names a frequency
        "board id 0",
        "firmware 2.1",           // a version on a line that names hertz
        "ADS-B",                  // the plugin whose preset was applied
        // a driver error naming the radio keeps WHICH KIND of device it was
        "USB\\VID_0BDA&PID_2838\\<stripped>",
        "LIBUSB_ERROR_ACCESS",
        // the listing lines are counted, not silently lost
        "vendor: libusb: 3 lines listing this machine's USB devices left out",
        // Linux: a usbfs node and its errno survive, udev and label serials do not
        "/dev/bus/usb/001/004 busy (-16)",
        "ID_SERIAL_SHORT=<stripped>",
        "Generic RTL2832U OEM :: <stripped>",
        // the Pluto's port and daemon version survive its address
        "at <host>:30431 - iiod 0.25", "at ip:<host> refused a tune", "tcp://<host>:55132",
    };
    return v;
}

// The two lines that carry nothing sensitive, which must come through
// untouched - the scrub must not become the reason a report is unreadable.
const char* kPlainA = "plugin: loaded adsb-decoder 1.8.0";
const char* kPlainB = "source: read failed (-5) after 3 retries, 12 ms";

void writeTheRealLines() {
    DiagLog& log = DiagLog::instance();
    log.resetForTest();
    using cascade::core::diagLogf;
    using cascade::core::diagWarnf;

    diagLogf("%s", kPlainA);
    diagLogf("source: opened %s (%s) at %.0f S/s", "uhd b200", "soapy", 2400000.0);
    // src/gui/app_window.cpp noteTuneRefused (0.99.32)
    diagLogf("source: asked for %.6f MHz, the %s refused it - its range is "
             "%.6f to %.6f MHz",
             433917000.0 / 1.0e6, "RTL-SDR", 24000000.0 / 1.0e6, 1766000000.0 / 1.0e6);
    // src/gui/app_window.cpp noteTuneMismatch (0.99.32)
    diagLogf("source: asked for %.6f MHz, the %s answered %.6f MHz", 433917000.0 / 1.0e6,
             "RTL-SDR", 433910000.0 / 1.0e6);
    // src/gui/app_window_patch_radios.cpp (0.99.32): a user-typed node name, a
    // device label with the serial in it, and the centre frequency.
    diagLogf("patch: radio '%s' running %s at %.0f S/s, %.6f MHz", "Loft Airband",
             "RTL-SDR Blog V4 (serial 00000417)", 2048000.0, 127825000.0 / 1e6);
    diagLogf("patch: speaker '%s' -> %s%s%s", "Kitchen radio", "Playing on Speakers", "", "");
    // src/core/transmitter.cpp (0.99.32)
    diagLogf("tx: keyed - %s at %.6f MHz, %.0f S/s", "FM", 145487500.0 / 1e6, 2000000.0);
    // src/source/pluto_tx.cpp (0.99.32)
    diagLogf("tx: started - %.6f MHz, %.0f S/s, power %.2f dB", 145487500.0 / 1e6, 2000000.0,
             -10.0);
    // The native Airspy's open line, from a real report.
    diagLogf("airspy: opened %s - board id %u, firmware \"%s\", serial %s",
             "Airspy R2 (serial AIRSPY_SN:26A464DC28593E93)", 0u,
             "AirSpy NOS v1.0.0-rc10-6-g4008185",
             "000000000000000026a464dc28593e93");
    // src/source/rtlsdr_source.cpp (0.99.32)
    diagWarnf("source: the tuner's PLL did not lock at %.4f MHz - %s from a "
              "%.4f MHz reference; deaf there, though samples keep flowing",
              1090000000.0 / 1e6, "R820T", 28800000.0 / 1e6);
    diagWarnf("source: the tuner's PLL failed to lock on %d tune%s while this radio was open", 3,
              "s");
    diagLogf("rx888: opened %s - firmware %u.%u, ADC %u MHz%s", "RX888 MkII", 2u, 1u, 64u, "");
    // src/gui/app_window.cpp: a plugin preset label carries its frequency.
    diagLogf("plugin: %s %s - applied its preset %s", "ADS-B", "opened", "ACARS 131.725");
    // A helper path under the user's profile.
    diagWarnf("soapy: no enumeration helper could be started ('%s') - walking the bus "
              "in-process instead, which is not crash-isolated",
              "C:\\Users\\alice.smith\\AppData\\Local\\FoxSDR\\soapy_enum.exe");
    // A driver line exactly as the vendor capture records it (already masked).
    DiagLog::instance().write("info", cascade::core::scrubVendorLine(
                                          "vendor: Setting center freq: 101100000").c_str());
    // libusb's device listing, VERBATIM from a crash report FoxSDR 0.99.31
    // uploaded, recorded the way the stderr capture records it.
    for (const char* v : {
             "libusb: info [get_guid] no DeviceInterfaceGUID registered for "
             "'USB\\VID_0DB0&PID_0076\\7E59240920A2'",
             "libusb: info [get_guid] no DeviceInterfaceGUID registered for "
             "'USB\\VID_045E&PID_0B00\\3032363330303934343834393433'",
             "libusb: info [winusb_get_device_list] The following device has no driver: "
             "'USB\\VID_046D&PID_C336&LAMPARRAY\\9&98F1CCD&0&077233483938_SLOT00'",
         }) {
        DiagLog::instance().writef("info", "vendor: %s", cascade::core::scrubVendorLine(v).c_str());
    }
    // A libusb ERROR about the radio itself (constructed): kept, instance masked.
    DiagLog::instance().writef(
        "info", "vendor: %s",
        cascade::core::scrubVendorLine("libusb: error [winusb_claim_interface] could not claim "
                                       "USB\\VID_0BDA&PID_2838\\00000417: LIBUSB_ERROR_ACCESS")
            .c_str());
    // Linux equivalents (constructed in the shapes those sources write): a
    // usbfs node with its errno, udev's short serial, a SoapySDR label.
    diagWarnf("usbfs: %s busy (-16); udev ID_SERIAL_SHORT=%s", "/dev/bus/usb/001/004", "00000417");
    diagLogf("soapy: opened label=%s", "Generic RTL2832U OEM :: 00000417");
    diagWarnf("exception 0x%08X absorbed in %s", 0xC0000005u, "SoapyUHD.dll");
    // src/source/pluto_source.cpp up to 0.99.58: the address the user typed,
    // on the open line (fields shortened to stay inside the ring's width).
    diagLogf("pluto: opened %s at %s:%u - iiod %s, firmware \"%s\", serial %s; phy %s",
             "ADALM-Pluto", "192.168.2.1", 30431u, "0.25", "v0.38", "1044", "ad9361-phy");
    // src/gui/app_window.cpp noteTuneRefused, with a Pluto's source name.
    diagLogf("source: the %s refused a tune %s", "Pluto: ADALM-Pluto at ip:shack-pluto.local",
             "below its range");
    // A network driver's own line, in the shape SoapyRemote writes a URL.
    DiagLog::instance().write("info", "soapy: SoapyRemote: tcp://192.168.1.77:55132 replied");
    diagLogf("%s", kPlainB);
}

std::string crashHeader() {
    return "kind: crash\n"
           "reason: access violation\n"
           "code: 0xC0000005\n"
           "address: cascade.exe+0x1A2B\n"
           "signature: 0123456789ABCDEF\n"
           "thread: 24180\n"
           "--- context ---\n"
           "version: 0.99.33\n"
           "commit: abc123def456\n"
           "os: Windows 10.0.22631\n"
           "arch: x64\n"
           "mode: WFM\n"
           "source: soapy\n"
           "sample-rate: 2400000\n"
           "device-open: yes\n"
           "sdr-model: uhd b200\n"
           "plugin: ADS-B 1.8.0\n"
           "--- stack (thread 24180) ---\n"
           "  cascade.exe+0x1A2B\n"
           "--- modules ---\n"
           "  cascade.exe base=0x00007FF700000000 size=0x2C8000 pdb=cascade.pdb "
           "build=651FD5EB776649E7B91461B1EB1EB8C525\n";
}

std::string hangHeader() {
    return "kind: hang\n"
           "note: the interface thread stopped answering\n"
           "stalled-ms: 7213\n"
           "threshold-ms: 5000\n"
           "signature: FEDCBA9876543210\n"
           "threads: 1\n"
           "--- context ---\n"
           "version: 0.99.33\n"
           "commit: abc123def456\n"
           "os: Windows 10.0.22631\n"
           "arch: x64\n"
           "mode: NFM\n"
           "source: soapy\n"
           "sample-rate: 2048000\n"
           "device-open: yes\n"
           "sdr-model: rtlsdr\n"
           "plugin: (none)\n"
           "--- modules ---\n"
           "  cascade.exe base=0x00007FF700000000 size=0x2C8000 pdb=cascade.pdb "
           "build=651FD5EB776649E7B91461B1EB1EB8C525\n"
           "--- thread 100 (gui, stalled) ---\n"
           "  cascade.exe+0x9999\n";
}

std::vector<std::string> uploadedLog(const std::string& reportText, bool& parsed) {
    cascade::core::ParsedReport r;
    parsed = cascade::core::parseReportText(reportText, r);
    std::vector<std::string> out;
    if (!parsed) { return out; }
    const nlohmann::json j = nlohmann::json::parse(cascade::core::uploadJson(r, std::string()),
                                                   nullptr, false);
    if (!j.is_object() || !j.contains("log") || !j["log"].is_array()) { return out; }
    for (const nlohmann::json& l : j["log"]) {
        if (l.is_string()) { out.push_back(l.get<std::string>()); }
    }
    return out;
}

// Path 1: the crash writer's own copy of the ring (copyRingRaw), not a
// snapshot - the bytes a real crash report carries.
std::vector<std::string> crashPathLines(bool& parsed) {
    std::vector<char> raw(static_cast<std::size_t>(DiagLog::kRingLines) * DiagLog::kLineBytes + 1);
    const std::size_t used = DiagLog::instance().copyRingRaw(raw.data(), raw.size());
    const std::string text = crashHeader() + "--- log (last 17 of 17 lines) ---\n" +
                             std::string(raw.data(), used);
    return uploadedLog(text, parsed);
}

// Path 2: the freeze writer's ring snapshot.
std::vector<std::string> hangPathLines(bool& parsed) {
    std::string text = hangHeader() + "--- log (last 17 of 17 lines) ---\n";
    for (const std::string& l : DiagLog::instance().ringSnapshot()) { text += l + "\n"; }
    return uploadedLog(text, parsed);
}

// Path 3: the diagnostics bundle's log section.
std::vector<std::string> bundleLines() {
    cascade::core::DiagBundleInput in;
    in.context.version = "0.99.33";
    in.logLines = DiagLog::instance().ringSnapshot();
    in.logLinesTotal = DiagLog::instance().linesWritten();
    const std::string bundle = cascade::core::buildDiagnosticsBundle(in);
    std::vector<std::string> out;
    const std::size_t at = bundle.find("--- log ---\n");
    if (at == std::string::npos) { return out; }
    std::size_t p = at + 12;
    while (p < bundle.size()) {
        const std::size_t nl = bundle.find('\n', p);
        if (nl == std::string::npos) { break; }
        out.push_back(bundle.substr(p, nl - p));
        p = nl + 1;
    }
    return out;
}

void checkPath(const char* name, const std::vector<std::string>& lines) {
    std::printf("--- %s: %zu lines ---\n", name, lines.size());
    for (const std::string& l : lines) { std::printf("  %s\n", l.c_str()); }
    // Every line the ring holds, less the three device-listing lines, plus the
    // one line that says they were left out.
    CHECK(lines.size() == DiagLog::instance().ringSnapshot().size() - 3u + 1u);
    const std::string text = joinUnstamped(lines);
    const std::string low = lower(text);
    for (const std::string& f : forbidden()) {
        const bool leaked = low.find(f) != std::string::npos;
        if (leaked) { std::printf("LEAK in %s: \"%s\"\n", name, f.c_str()); }
        CHECK(!leaked);
    }
    for (const std::string& want : required()) {
        const bool kept = text.find(want) != std::string::npos;
        if (!kept) { std::printf("LOST in %s: \"%s\"\n", name, want.c_str()); }
        CHECK(kept);
    }
    // The plain lines, byte for byte, stamp included.
    const std::vector<std::string> ring = DiagLog::instance().ringSnapshot();
    for (const char* plain : {kPlainA, kPlainB}) {
        std::string original;
        for (const std::string& l : ring) {
            if (unstamped(l) == std::string("info ") + plain) { original = l; }
        }
        CHECK(!original.empty());
        const bool same = std::find(lines.begin(), lines.end(), original) != lines.end();
        if (!same) { std::printf("CHANGED in %s: \"%s\"\n", name, original.c_str()); }
        CHECK(same);
    }
}

// Each part of the rule on its own, so a break in one names itself.
void expectScrub(const std::string& in, const std::string& want) {
    const std::string got = cascade::core::scrubUploadLine(in);
    if (got != want) {
        std::printf("scrubUploadLine(\"%s\")\n   gave \"%s\"\n   want \"%s\"\n", in.c_str(),
                    got.c_str(), want.c_str());
    }
    CHECK(got == want);
}

void unitRules() {
    using cascade::core::scrubUploadLine;

    // NOTHING SENSITIVE: byte for byte, including apostrophes, versions,
    // build ids, commits, error codes, rates, counts and durations.
    for (const char* plain : {
             "plugin: loaded adsb-decoder 1.8.0",
             "source: read failed (-5) after 3 retries, 12 ms",
             "source: the tuner's PLL failed to lock on 3 tunes while this radio was open",
             "source: opened uhd b200 (soapy) at 2400000 S/s",
             "rtlsdr: opened RTL-SDR Blog V4 - R828D, 2.4000 MS/s, 16 buffers",
             "exception 0xC0000005 absorbed in SoapyUHD.dll+0x1A2B",
             "diagnostics: build 651FD5EB776649E7B91461B1EB1EB8C525, commit 750ae69",
             "audio: underran 4 times in 60 s, 512 samples short",
             "mode: WFM, bandwidth 200000",
             "it isn't: snr=12 dB",
             "12:34:56.789 info plugin: loaded APRS 1.0.2",
             "",
         }) {
        expectScrub(plain, plain);
    }

    // THE STAMP survives a line that is otherwise masked.
    expectScrub("12:34:56.789 info tune 145500000", "12:34:56.789 info tune #");

    // FREQUENCIES: a hertz unit, or a word that says so, and no digits left.
    expectScrub("Setting center freq: 101100000", "Setting center freq: #");
    expectScrub("tuned to 7074000", "tuned to #");
    expectScrub("scanner: hit at 145.500 MHz, 3 tunes", "scanner: hit at # MHz, 3 tunes");
    expectScrub("at 446.00625MHz", "at #MHz");
    expectScrub("frequency 1.4550e+08", "frequency #");
    expectScrub("frequency 145,500,000", "frequency #");
    expectScrub("centre 145500000, rate 2400000 S/s", "centre #, rate 2400000 S/s");
    // A hertz unit masks even what would otherwise be kept (a small negative).
    expectScrub("vfo offset -500 Hz", "vfo offset # Hz");
    expectScrub("offset -12500 Hz", "offset # Hz");
    // ...and a driver's error code on a tuning line is kept.
    expectScrub("rtlsdr_set_center_freq returned (-5)", "rtlsdr_set_center_freq returned (-5)");
    // A version after its word, a chip name and an id are kept on such a line.
    expectScrub("rx888: firmware 2.1, ADC 64 MHz", "rx888: firmware 2.1, ADC # MHz");
    expectScrub("tune failed on R820T, board id 3, error 7", "tune failed on R820T, board id 3, error 7");
    // A hex id that starts with digits is an id, not a number with a unit.
    expectScrub("retune failed in commit 750ae69", "retune failed in commit 750ae69");
    // A digit run already masked at capture says nothing about its band.
    expectScrub("[INFO] [B200] Actual RX Freq: ###.###### MHz...",
                "[INFO] [B200] Actual RX Freq: # MHz...");
    // A line cut at the ring's width lost the unit that named its number.
    {
        std::string cut = "12:34:56.789 info patch: node 3 running a radio at 2048000 S/s";
        while (cut.size() < static_cast<std::size_t>(DiagLog::kLineBytes) - 1u - 9u) { cut += ' '; }
        cut += ", 127.825";
        CHECK(cut.size() == static_cast<std::size_t>(DiagLog::kLineBytes) - 1u);
        const std::string got = scrubUploadLine(cut);
        CHECK(got.find("127") == std::string::npos);
        CHECK(got.find("2048000 S/s") != std::string::npos);
        // One byte shorter is a whole line, and a whole line with no frequency
        // word is left alone.
        const std::string whole = cut.substr(0, cut.size() - 1);
        CHECK(scrubUploadLine(whole) == whole);
    }

    // SERIALS, in every form a driver or a label writes one.
    expectScrub("opened Airspy R2 (serial AIRSPY_SN:26A464DC28593E93)",
                "opened Airspy R2 (serial <stripped>)");
    expectScrub("found AIRSPY_SN:26A464DC28593E93 on the bus",
                "found AIRSPY_SN:<stripped> on the bus");
    expectScrub("hackrf: serial 000000000000000026a464dc28593e93",
                "hackrf: serial <stripped>");
    expectScrub("device serial_number=ABC123 ready", "device serial_number=<stripped> ready");
    expectScrub("Serial: 00000001, Product: RTL2838UHIDIR", "Serial: <stripped>, Product: RTL2838UHIDIR");
    expectScrub("SN: 00000417", "SN: <stripped>");
    expectScrub("S/N=XYZ9 ok", "S/N=<stripped> ok");
    // The vendor rule shares the serial rule, so a driver's line is fixed too.
    CHECK(cascade::core::scrubVendorLine("opened (serial AIRSPY_SN:26A464DC28593E93)") ==
          "opened (serial <stripped>)");

    // THE ACCOUNT NAME in a path, spaces and all.
    expectScrub("loaded C:\\Users\\John Smith\\radioconda\\x.dll",
                "loaded C:\\Users\\<user>\\radioconda\\x.dll");
    expectScrub("state in /home/alice/.local/state/foxsdr", "state in /home/<user>/.local/state/foxsdr");

    // USER-TYPED NAMES in quotes, an apostrophe inside one included, and a
    // name the line was cut off inside.
    expectScrub("patch: radio 'Bob's rig' switched on", "patch: radio '<name>' switched on");
    expectScrub("patch: radio 'Loft Air", "patch: radio '<name>");

    // A DEVICE LABEL THE OPERATING SYSTEM HANDED BACK, in parentheses, not
    // quotes - a Bluetooth headset or a paired phone used as a microphone,
    // where the make and model that follow the possessive are the useful
    // part of the line and the person's name is not (2026-09-28 review; the
    // first is the exact PortAudio label the review's leak probe used).
    expectScrub("source: opened the sound card Headset (Alice's AirPods Pro) (wasapi) at 48000 Hz, stereo",
                "source: opened the sound card Headset (<name>'s AirPods Pro) (wasapi) at # Hz, stereo");
    expectScrub("source: found input device Microphone (Bob's iPhone)",
                "source: found input device Microphone (<name>'s iPhone)");
    // A possessive OUTSIDE parentheses is prose, not a device label, and is
    // left alone - the existing "the tuner's PLL" case above already proves
    // this for a non-possessive apostrophe; this is the possessive case.
    expectScrub("note: it's connected", "note: it's connected");

    // USB INSTANCE IDS: the three real lines, one at a time, keep the kind of
    // device and lose the instance segment...
    expectScrub("vendor: libusb: info [get_guid] no DeviceInterfaceGUID registered for "
                "'USB\\VID_0DB0&PID_0076\\7E59240920A2'",
                "vendor: libusb: info [get_guid] no DeviceInterfaceGUID registered for "
                "'USB\\VID_0DB0&PID_0076\\<stripped>'");
    expectScrub("vendor: libusb: info [get_guid] no DeviceInterfaceGUID registered for "
                "'USB\\VID_045E&PID_0B00\\3032363330303934343834393433'",
                "vendor: libusb: info [get_guid] no DeviceInterfaceGUID registered for "
                "'USB\\VID_045E&PID_0B00\\<stripped>'");
    expectScrub("vendor: libusb: info [winusb_get_device_list] The following device has no "
                "driver: 'USB\\VID_046D&PID_C336&LAMPARRAY\\9&98F1CCD&0&077233483938_SLOT00'",
                "vendor: libusb: info [winusb_get_device_list] The following device has no "
                "driver: 'USB\\VID_046D&PID_C336&LAMPARRAY\\<stripped>'");
    // ...the interface-path form too, the class GUID after it untouched...
    expectScrub("open \\\\?\\usb#vid_0bda&pid_2838#00000417#{a5dcbf10-6530-11d2-901f-00c04fb951ed}",
                "open \\\\?\\usb#vid_0bda&pid_2838#<stripped>#{a5dcbf10-6530-11d2-901f-00c04fb951ed}");
    // ...and as a WHOLE LOG the listing lines are left out, a run of them
    // becoming one line that counts them.
    {
        const std::vector<std::string> in = {
            "12:00:00.001 info plugin: loaded APRS 1.0.2",
            "12:00:00.002 info vendor: libusb: info [get_guid] no DeviceInterfaceGUID registered "
            "for 'USB\\VID_0DB0&PID_0076\\7E59240920A2'",
            "12:00:00.003 info vendor: libusb: debug [get_guid] x 'USB\\VID_045E&PID_0B00\\30'",
            "12:00:00.004 info source: read failed (-5)",
            "12:00:00.005 info vendor: libusb: info [winusb_get_device_list] The following device "
            "has no driver: 'USB\\VID_046D&PID_C336&LAMPARRAY\\9&98F1CCD&0&077233483938_SLOT00'",
            "12:00:00.006 info vendor: libusb: error [x] USB\\VID_0BDA&PID_2838\\00000417 gone",
        };
        const std::vector<std::string> want = {
            "12:00:00.001 info plugin: loaded APRS 1.0.2",
            "12:00:00.002 info vendor: libusb: 2 lines listing this machine's USB devices left out",
            "12:00:00.004 info source: read failed (-5)",
            "12:00:00.005 info vendor: libusb: 1 line listing this machine's USB devices left out",
            "12:00:00.006 info vendor: libusb: error [x] USB\\VID_0BDA&PID_2838\\<stripped> gone",
        };
        const std::vector<std::string> got = cascade::core::scrubUploadLog(in);
        CHECK(got == want);
        if (got != want) {
            for (const std::string& g : got) { std::printf("  scrubUploadLog gave: %s\n", g.c_str()); }
        }
        CHECK(cascade::core::scrubUploadLog({}).empty());
    }

    // LINUX: udev's serial keys, a SoapySDR label, and the usbfs node path
    // (bus and device numbers, which name a port for this session, not a
    // device) left alone.
    expectScrub("udev: ID_SERIAL=Realtek_RTL2838UHIDIR_00000417", "udev: ID_SERIAL=<stripped>");
    expectScrub("udev: ID_SERIAL_SHORT=00000417", "udev: ID_SERIAL_SHORT=<stripped>");
    expectScrub("soapy: label=Generic RTL2832U OEM :: 00000417, driver=rtlsdr",
                "soapy: label=Generic RTL2832U OEM :: <stripped>, driver=rtlsdr");
    expectScrub("usbfs: /dev/bus/usb/001/004 busy (-16)", "usbfs: /dev/bus/usb/001/004 busy (-16)");
}

// NETWORK ADDRESSES AND HOST NAMES (0.99.59). PRIVACY.md has always said a
// report never carries "your IP address" or "your machine name", but no rule
// enforced it: the Pluto's open line printed the address the user typed
// ("pluto: opened ADALM-Pluto at 192.168.2.1:30431 ..."), its source name
// ("Pluto: ADALM-Pluto at ip:pluto.local") reached tune lines, and a
// network driver's own lines (SoapyRemote, rtl_tcp, SpyServer) carry
// addresses and URLs. An IPv4 or IPv6 literal anywhere, and a host name in
// the places FoxSDR and those drivers write one, become <host>; the port
// stays, and so does every version string the report needs.
void networkAddresses() {
    // IPv4 LITERALS, anywhere on the line, the port kept.
    expectScrub("pluto: opened ADALM-Pluto at 192.168.2.1:30431 - iiod 0.25, firmware \"v0.38\"",
                "pluto: opened ADALM-Pluto at <host>:30431 - iiod 0.25, firmware \"v0.38\"");
    expectScrub("soapy: rtl_tcp connected 10.20.30.40", "soapy: rtl_tcp connected <host>");
    expectScrub("vendor: peer (172.16.254.1) closed", "vendor: peer (<host>) closed");
    // ...on a line that also names a frequency the number rule still runs.
    expectScrub("pluto: at 192.168.2.1:30431 tuned to 433920000",
                "pluto: at <host>:# tuned to #");

    // HOST NAMES after the keys FoxSDR and the network drivers write them
    // with - a single label (a machine name) as well as a dotted one.
    expectScrub("source: the Pluto: ADALM-Pluto at ip:192.168.2.1 refused a tune below its range",
                "source: the Pluto: ADALM-Pluto at ip:<host> refused a tune below its range");
    expectScrub("Pluto: ADALM-Pluto at ip:pluto.local", "Pluto: ADALM-Pluto at ip:<host>");
    expectScrub("Pluto TX: ADALM-Pluto at ip:STEVE-PC", "Pluto TX: ADALM-Pluto at ip:<host>");
    expectScrub("soapy: opened driver=remote,remote=shack-pi.local:55132,remote:driver=rtlsdr",
                "soapy: opened driver=remote,remote=<host>:55132,remote:driver=rtlsdr");
    expectScrub("soapy: args host=SHACK-PC, port=5555", "soapy: args host=<host>, port=5555");
    expectScrub("soapy: hostname=radio-room ok", "soapy: hostname=<host> ok");
    expectScrub("soapy: rtltcp=192.168.1.20:1234", "soapy: rtltcp=<host>:1234");
    expectScrub("pluto: uri=ip:192.168.2.1", "pluto: uri=ip:<host>");
    expectScrub("vendor: addr=sdr.example.org", "vendor: addr=<host>");
    expectScrub("vendor: server=spy.example.net:5555", "vendor: server=<host>:5555");
    // ...after a URL scheme, a user name and password with it, the path kept.
    expectScrub("soapy: SoapyRemote: tcp://192.168.1.77:55132 replied",
                "soapy: SoapyRemote: tcp://<host>:55132 replied");
    expectScrub("vendor: rtsp://admin:secret@cam.example.com:554/stream1",
                "vendor: rtsp://<host>:554/stream1");
    expectScrub("vendor: GET http://shack-pi/api/status", "vendor: GET http://<host>/api/status");
    expectScrub("vendor: https://[fe80::1]:8443/x", "vendor: https://<host>:8443/x");
    // ...after " at ", " to " and " from ", when it is plainly a host: a
    // dotted name, or a name with a port after it.
    expectScrub("could not reach the Pluto at sdr.example.org:30431 - refused",
                "could not reach the Pluto at <host>:30431 - refused");
    expectScrub("nothing at shack-pi:30431 answered as an IIO daemon: closed",
                "nothing at <host>:# answered as an IIO daemon: closed");
    expectScrub("vendor: Connecting to spy.example.net:5555...", "vendor: Connecting to <host>:5555...");
    expectScrub("vendor: reply from pluto.local.", "vendor: reply from <host>.");
    // ...in double quotes, as a dotted name, or after the word "host".
    expectScrub("could not find \"pluto.local\" on the network",
                "could not find \"<host>\" on the network");
    expectScrub("could not find the host \"STEVE-PC\" on the network",
                "could not find the host \"<host>\" on the network");
    // ...a local-network name anywhere at all...
    expectScrub("vendor: resolved shack-pi.lan first", "vendor: resolved <host> first");
    // ...and the machine in a Windows network path.
    expectScrub("plugins: \\\\NAS-01\\radio\\plugins (this is not a Store package)",
                "plugins: \\\\<host>\\radio\\plugins (this is not a Store package)");

    // IPv6 LITERALS, bracketed with a port and a zone, compressed, full,
    // and with an IPv4 tail.
    expectScrub("vendor: connected to [fe80::1ff:fe23:4567:890a%12]:1234",
                "vendor: connected to [<host>]:1234");
    expectScrub("vendor: peer 2001:db8::8a2e:370:7334 refused", "vendor: peer <host> refused");
    expectScrub("vendor: peer 2001:0db8:85a3:0000:0000:8a2e:0370:7334 gone",
                "vendor: peer <host> gone");
    expectScrub("vendor: mapped ::ffff:192.168.1.5 ok", "vendor: mapped <host> ok");

    // THIS MACHINE TALKING TO ITSELF names nobody, and says whether a
    // listener was bound to every interface or to loopback only: kept.
    for (const char* plain : {
             "web: listening on 0.0.0.0:8080",
             "web: bound 127.0.0.1:8080",
             "vendor: [::1]:1234 refused",
             "pluto: uri=ip:localhost",
         }) {
        expectScrub(plain, plain);
    }

    // WHAT MUST NOT CHANGE: versions in every shape a log line prints one
    // (the starting line, a package version after its word, the Store
    // package's full name, the OS build), times, rates, tuner and error
    // numbers, a USB VID:PID, an out-of-range dotted quad, module names
    // after " at ", C++ scope operators, file names, and prose.
    for (const char* plain : {
             "12:34:56.789 info FoxSDR 0.99.58 (80e2998) starting",
             "store update: package version 1.99.58.0 is installed",
             "plugins: C:\\Program Files\\WindowsApps\\x (this is a Store package: "
             "hedgerowlabs.FoxSDR_1.99.58.0_x64__8wekyb3d8bbwe)",
             "os: Windows 10.0.22631.4317",
             "firmware 1.2.3.4 on board id 0",
             "beyond 255: 300.168.1.1 and 1.2.3.256 stay, 1.2.3 too",
             "rtlsdr: opened at 2400000 S/s, tuner 0, read failed (-5)",
             "libusb: USB\\VID_0BDA&PID_2838 0bda:2838 at 12:34:56",
             "exception 0xC0000005 absorbed in SoapyUHD.dll at cascade.exe+0x1A2B",
             "soapy: SoapySDR::Device::make() failed at cascade::core::open",
             "bookmarks: loaded plugins.json from settings.ini to bands.csv",
             "source: switched to WFM at 48000 S/s - nothing at all to do",
             "source: the RTL-SDR answered a tune somewhere else (at the edge of its range, +3 ppm)",
             "aor: opened AOR AR-DV1 - VR \"AR-DV1\", control on COM, I/Q at 1.125 MS/s",
         }) {
        expectScrub(plain, plain);
    }
}

// SERIAL PORT NAMES (0.99.59): the number Windows gave a port, or the
// number on a Linux tty node, is masked in uploads - "COM5" becomes "COM#".
// A diagnosis needs to know a port was tried, opened or refused, and in what
// order; which number this machine happened to give it identifies nothing
// the report needs.
void serialPortNames() {
    expectScrub("aor: control port COM5: VR -> \"AR-DV1\" (AR-DV1)",
                "aor: control port COM#: VR -> \"AR-DV1\" (AR-DV1)");
    expectScrub("aor: open abandoned: More than one AOR receiver answered (COM5, COM17).",
                "aor: open abandoned: More than one AOR receiver answered (COM#, COM#).");
    expectScrub("gps: \\\\.\\COM12 could not be opened: Access is denied.",
                "gps: \\\\.\\COM# could not be opened: Access is denied.");
    expectScrub("gps: listening on /dev/ttyUSB0 at 9600", "gps: listening on /dev/ttyUSB# at 9600");
    expectScrub("gps: /dev/ttyACM12 gone", "gps: /dev/ttyACM# gone");
    for (const char* plain : {
             "telecom3 and COMMAND 3 and the COM port",
             "usbfs: /dev/bus/usb/001/004 busy (-16)",
             "gps: (a typed device path, 27 chars) could not be opened",
         }) {
        expectScrub(plain, plain);
    }
}

// Sets (or, with nullptr, clears) an environment variable in the copy the
// CRT's getenv reads. The library is linked statically into this test, so it
// shares that copy.
void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value != nullptr ? value : "");
#else
    if (value != nullptr) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

std::string bundleField(const std::string& bundle, const std::string& name) {
    const std::size_t at = bundle.find("\n" + name + ": ");
    if (at == std::string::npos) { return "(missing)"; }
    const std::size_t v = at + 1 + name.size() + 2;
    return bundle.substr(v, bundle.find('\n', v) - v);
}

// THE BUNDLE'S OWN HEADER. `log-path` and `crash-dir` are paths under the
// user's profile, and a real bundle pasted into a bug report (0.99.32) read
// "log-path: C:\Users\Utente\AppData\Local\FoxSDR\logs/foxsdr.log". The base
// directory is written as the variable it came from, and anything left that
// still names an account goes through the same rule as a log line.
void bundleHeaderPaths() {
    const char* var =
#if defined(_WIN32)
        "LOCALAPPDATA";
#else
        "HOME";
#endif
    const char* saved = std::getenv(var);
    const std::string keep = saved != nullptr ? saved : "";

    struct Case {
        const char* base;      // the variable's value for this case
        const char* logPath;   // what the application would have passed
        const char* crashDir;
        const char* wantLog;   // what the bundle must say
        const char* wantCrash;
        const char* name;      // the account name that must not appear
    };
    const Case cases[] = {
#if defined(_WIN32)
        // The ordinary install: the base is the variable, and says so.
        {"C:\\Users\\Utente\\AppData\\Local", "C:\\Users\\Utente\\AppData\\Local\\FoxSDR\\logs/foxsdr.log",
         "C:\\Users\\Utente\\AppData\\Local\\FoxSDR\\crashes",
         "%LOCALAPPDATA%\\FoxSDR\\logs/foxsdr.log", "%LOCALAPPDATA%\\FoxSDR\\crashes", "utente"},
        // A redirected profile NOT under \Users\ - only the variable finds it.
        {"D:\\Profili\\Utente\\AppData\\Local", "D:\\Profili\\Utente\\AppData\\Local\\FoxSDR\\logs/foxsdr.log",
         "D:\\Profili\\Utente\\AppData\\Local\\FoxSDR\\crashes",
         "%LOCALAPPDATA%\\FoxSDR\\logs/foxsdr.log", "%LOCALAPPDATA%\\FoxSDR\\crashes", "utente"},
        // A path outside the variable (the FOXSDR_DIAG_DIR override) is
        // still shown, with the account name masked.
        {"C:\\Users\\Other\\AppData\\Local", "C:\\Users\\Utente\\diag\\logs/foxsdr.log",
         "C:\\Users\\Utente\\diag\\crashes", "C:\\Users\\<user>\\diag\\logs/foxsdr.log",
         "C:\\Users\\<user>\\diag\\crashes", "utente"},
#else
        {"/home/utente", "/home/utente/.local/state/foxsdr/logs/foxsdr.log",
         "/home/utente/.local/state/foxsdr/crashes", "$HOME/.local/state/foxsdr/logs/foxsdr.log",
         "$HOME/.local/state/foxsdr/crashes", "utente"},
        {"/srv/people/utente", "/srv/people/utente/.local/state/foxsdr/logs/foxsdr.log",
         "/srv/people/utente/.local/state/foxsdr/crashes",
         "$HOME/.local/state/foxsdr/logs/foxsdr.log", "$HOME/.local/state/foxsdr/crashes", "utente"},
        {"/home/other", "/home/utente/diag/logs/foxsdr.log", "/home/utente/diag/crashes",
         "/home/<user>/diag/logs/foxsdr.log", "/home/<user>/diag/crashes", "utente"},
#endif
    };
    for (const Case& c : cases) {
        setEnv(var, c.base);
        cascade::core::DiagBundleInput in;
        in.context.version = "0.99.33";
        in.logPath = c.logPath;
        in.crashDir = c.crashDir;
        const std::string bundle = cascade::core::buildDiagnosticsBundle(in);
        const std::string gotLog = bundleField(bundle, "log-path");
        const std::string gotCrash = bundleField(bundle, "crash-dir");
        std::printf("bundle header: log-path: %s | crash-dir: %s\n", gotLog.c_str(),
                    gotCrash.c_str());
        CHECK(gotLog == c.wantLog);
        CHECK(gotCrash == c.wantCrash);
        // Nowhere in the WHOLE bundle, not just those two lines.
        CHECK(lower(bundle).find(c.name) == std::string::npos);
    }
    // Empty stays "(none)": a bundle with the file off says so.
    {
        cascade::core::DiagBundleInput in;
        const std::string bundle = cascade::core::buildDiagnosticsBundle(in);
        CHECK(bundleField(bundle, "log-path") == "(none)");
        CHECK(bundleField(bundle, "crash-dir") == "(none)");
    }
    setEnv(var, saved != nullptr ? keep.c_str() : nullptr);
}

// A PLUGIN'S NAME IS NOT A FREQUENCY (GitHub issue 5's bundle, 0.99.43). The
// header of that bundle read "plugin: 406 MHz Beacons 1.0.0" and every log
// line naming the same plugin read "plugin: loaded # MHz Beacons #": the name
// carries "MHz", so rule 8 masked every number on the line, the version with
// it. The header is the same inventory, unscrubbed, a few lines up - masking
// it in the log protected nothing and cost the report its plugin versions.
// The names the report itself lists are kept wherever they appear; every
// other number on such a line is still masked.
void pluginNamesSurviveTheScrub() {
    const std::string loaded = "15:41:13.441 info plugin: loaded 406 MHz Beacons 1.0.0";
    const std::string other = "15:41:13.441 info plugin: loaded GOES HRIT / LRIT 0.2.0";
    // A real frequency on a line that also names the plugin: still masked.
    const std::string tuned = "15:41:20.000 info plugin: 406 MHz Beacons 1.0.0 - receiver asked "
                              "for 406.028000 MHz";
    const std::string plainTune = "15:41:21.000 info source: asked for 406.028000 MHz";
    const std::vector<std::string> plugins = {"406 MHz Beacons 1.0.0", "GOES HRIT / LRIT 0.2.0"};

    // Path 3, the diagnostics bundle, exactly as the report page builds it.
    {
        cascade::core::DiagBundleInput in;
        in.context.version = "0.99.44";
        in.context.plugins = plugins;
        in.logLines = {loaded, other, tuned, plainTune};
        const std::string bundle = cascade::core::buildDiagnosticsBundle(in);
        const std::string log = bundle.substr(bundle.find("--- log ---\n"));
        std::printf("bundle log section:\n%s", log.c_str());
        CHECK(log.find(loaded + "\n") != std::string::npos);
        CHECK(log.find(other + "\n") != std::string::npos);
        CHECK(log.find("# MHz Beacons #") == std::string::npos);
        CHECK(log.find("406.028") == std::string::npos);
        CHECK(log.find("15:41:20.000 info plugin: 406 MHz Beacons 1.0.0 - receiver asked for # MHz") !=
              std::string::npos);
        CHECK(log.find("15:41:21.000 info source: asked for # MHz") != std::string::npos);
    }
    // Paths 1 and 2, the crash and freeze uploads: the report's own
    // "plugin:" context lines are the inventory.
    {
        std::string header = crashHeader();
        const std::string adsb = "plugin: ADS-B 1.8.0\n";
        header.replace(header.find(adsb), adsb.size(),
                       "plugin: 406 MHz Beacons 1.0.0\nplugin: GOES HRIT / LRIT 0.2.0\n");
        const std::string text = header + "--- log (last 4 of 4 lines) ---\n" + loaded + "\n" +
                                 other + "\n" + tuned + "\n" + plainTune + "\n";
        bool parsed = false;
        const std::vector<std::string> up = uploadedLog(text, parsed);
        CHECK(parsed);
        const std::string joined = [&] {
            std::string s;
            for (const std::string& l : up) { s += l + "\n"; }
            return s;
        }();
        std::printf("crash upload log:\n%s", joined.c_str());
        CHECK(joined.find(loaded + "\n") != std::string::npos);
        CHECK(joined.find(other + "\n") != std::string::npos);
        CHECK(joined.find("406.028") == std::string::npos);
    }
    // And a list that names nothing changes nothing: rule 8 as it was.
    CHECK(cascade::core::scrubUploadLog({loaded}).front() ==
          "15:41:13.441 info plugin: loaded # MHz Beacons #");

    // 0.99.44 REPAIR: the kept name is the ONLY thing on the line that reads
    // like a frequency ("406 MHz"), and there is a genuine, unrelated bare
    // number elsewhere on the same line with no OTHER frequency word beside
    // it. Before the fix, tokenising the name away before judging whether the
    // line "mentions a frequency" made this line look like it mentioned none
    // at all, so the bare number escaped masking entirely - the exact
    // opposite of what the scrubber exists to do. The name must still come
    // back unmasked; the unrelated number must not.
    {
        const std::string bareNumberBesideKeptName =
            "15:41:23.000 info plugin: 406 MHz Beacons 1.0.0 - decoded a burst, count 144800000";
        const std::string got =
            cascade::core::scrubUploadLog({bareNumberBesideKeptName}, plugins).front();
        std::printf("bare number beside a kept name: %s\n", got.c_str());
        CHECK(got.find("406 MHz Beacons 1.0.0") != std::string::npos);
        CHECK(got.find("144800000") == std::string::npos);
        CHECK(got ==
              "15:41:23.000 info plugin: 406 MHz Beacons 1.0.0 - decoded a burst, count #");
    }
}

}  // namespace

int main() {
    unitRules();
    networkAddresses();
    serialPortNames();
    bundleHeaderPaths();
    pluginNamesSurviveTheScrub();

    writeTheRealLines();

    bool parsed = false;
    const std::vector<std::string> crash = crashPathLines(parsed);
    CHECK(parsed);
    checkPath("crash report upload", crash);

    parsed = false;
    const std::vector<std::string> hang = hangPathLines(parsed);
    CHECK(parsed);
    checkPath("freeze report upload", hang);

    checkPath("diagnostics bundle", bundleLines());

    DiagLog::instance().resetForTest();
    return testSummary("test_upload_scrub");
}
