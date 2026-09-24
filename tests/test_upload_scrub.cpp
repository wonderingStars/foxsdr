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

}  // namespace

int main() {
    unitRules();
    bundleHeaderPaths();

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
