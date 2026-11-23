// Tests for core/radio_report.hpp: the radio key, the text rules (held to the Worker's own parity table), the scrub, the radio lines of the log ring
// and the device-argument readers. The queue and the sender are test_radio_reporter.cpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/health_events.hpp"
#include "core/radio_report.hpp"
#include "core/telemetry.hpp"
#include "test_check.hpp"

using namespace cascade::core;
using namespace cascade::core::radioreport;

#ifndef CASCADE_SOURCE_ROOT
#error "CASCADE_SOURCE_ROOT is defined by tests/CMakeLists.txt"
#endif

namespace {

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    // --- the contract's text rules, held to the Worker's parity table (557 strings) ----------------------------------------------------------
    {
        const std::string text = slurp(std::string(CASCADE_SOURCE_ROOT) + "/telemetry-worker/test-fixtures/radio-text-rules.json");
        CHECK(!text.empty());
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        CHECK(j.is_object() && j.contains("cases"));
        int n = 0;
        int bad = 0;
        if (j.is_object() && j.contains("cases")) {
            for (const nlohmann::json& c : j["cases"]) {
                const std::string want = c.value("code", std::string());
                const std::string got = textCode(c.value("text", std::string()), c.value("serial", std::string()));
                ++n;
                if (got != want) {
                    ++bad;
                    if (bad <= 8) { std::printf("PARITY %s: text=[%s] serial=[%s] want=[%s] got=[%s]\n", "radio-text-rules", c.value("text", std::string()).c_str(), c.value("serial", std::string()).c_str(), want.c_str(), got.c_str()); }
                }
            }
        }
        std::printf("text-rule parity: %d cases, %d differ\n", n, bad);
        CHECK(n >= 500);
        CHECK(bad == 0);
    }

    // --- the radio key (contract 1.1) -------------------------------------------------------------------------------------------------------
    CHECK(radioKey("uhd b200", "EDR04ZDB2") == "uhd-b200:EDR04ZDB2");
    CHECK(radioKey("RTL-SDR", "") == "rtl-sdr:-");
    CHECK(radioKey("Airspy HF+", "hf0001a2") == "airspy-hf:HF0001A2");   // the serial is upper-cased
    CHECK(modelSlug("  --Weird   Model!!  ") == "weird-model");
    CHECK(modelSlug(std::string(60, 'a')).size() == 40);
    CHECK(modelSlug("") == "");
    CHECK(radioKey("", "") == ":-");

    // --- which radio: the serial from the arguments, never an address ---------------------------------------------------------------------------
    CHECK(serialFromArgs("soapy", "driver=uhd, label=B200 EDR04ZDB2, product=B200, serial=EDR04ZDB2, type=b200") == "EDR04ZDB2");
    CHECK(serialFromArgs("rtlsdr", "serial=00000001") == "00000001");
    CHECK(serialFromArgs("rtlsdr", "index=0").empty());
    CHECK(serialFromArgs("rtltcp", "rtltcp=garage.lan:1234").empty());
    CHECK(serialFromArgs("rtltcp", "serial=1234").empty());   // an rtl_tcp server has no hardware serial the app can read
    CHECK(serialFromArgs("pluto", "uri=ip:192.168.2.1").empty());
    CHECK(serialFromArgs("soundcard", "serial=123456").empty());
    CHECK(serialFromArgs("airspyhf", "serial=HF0001A2") == "HF0001A2");
    CHECK(serialFromArgs("hackrf", "serial=a/b").empty());   // not shaped like a serial
    CHECK(serialFromArgs("hackrf", "serial=" + std::string(41, 'a')).empty());
    {
        const RadioIdentity a = identify("soapy", "driver=uhd, label=B200 EDR04ZDB2, product=B200, serial=EDR04ZDB2, type=b200");
        CHECK(a.driver == "soapy");
        CHECK(a.serial == "EDR04ZDB2");
        CHECK(a.model.find("EDR04ZDB2") == std::string::npos);
        CHECK(a.key == radioKey(a.model, a.serial));
        const RadioIdentity b = identify("rtlsdr", "serial=00000001", "RTL-SDR (serial 00000001)");
        CHECK(b.model == "RTL-SDR");
        CHECK(b.serial == "00000001");
        CHECK(b.key == "rtl-sdr:00000001");
        const RadioIdentity c = identify("rtltcp", "rtltcp=10.1.2.3:1234");
        CHECK(c.serial.empty());
        CHECK(c.model.find("10.1.2.3") == std::string::npos);
        CHECK(c.driver == "rtltcp");
        const RadioIdentity d = identify("somethingnew", "serial=XYZ12345");
        CHECK(d.driver == "other");
        const RadioIdentity e = soundCardIdentity();
        CHECK(e.driver == "soundcard" && e.model == "sound card" && e.serial.empty() && e.key == "sound-card:-");
        // the model never carries an address or a unit name, whatever the label said
        const RadioIdentity f = identify("airspy", "serial=ABCD1234", "Airspy 10.0.0.5 (serial ABCD1234)");
        CHECK(f.model.find("10.0.0.5") == std::string::npos);
        CHECK(scrubModel(f.model) == f.model);
    }

    // --- A SOAPYSDR RADIO IS NAMED BY ITS OWN MODEL, THE VENDOR DRIVER KEY IS ITS OWN FIELD (hardware check 2026-11-17, hw-findings item 5) -----------------------------------
    {
        // the real B200's enumerated row (hardware key and product "B200"): model B200, not the driver key "uhd"
        const RadioIdentity a = identify("soapy", "driver=uhd, label=B200 EDR04ZDB2, name=, product=B200, serial=EDR04ZDB2, type=b200");
        CHECK(a.model == "B200" && a.serial == "EDR04ZDB2" && a.driver == "soapy" && a.library == "SoapySDR uhd");
        CHECK(a.key == "b200:EDR04ZDB2");
        // a hand-written or older saved row that names only driver and serial: the driver key until a scan or an open says the model, then the model
        const std::string minimal = "driver=uhd,serial=EDR04ZDB2";
        const RadioIdentity b = identify("soapy", minimal);
        CHECK(b.model == "uhd" && b.library == "SoapySDR uhd");
        rememberSoapyModel(minimal, "B200");   // what an open reports (the hardware key) or a scanned row says
        const RadioIdentity b2 = identify("soapy", minimal);
        CHECK(b2.model == "B200" && b2.key == a.key && b2.serial == "EDR04ZDB2");
        rememberSoapyModel("driver=uhd", "Wrong");   // no serial: two radios of a driver would share it, so nothing is remembered
        CHECK(identify("soapy", "driver=uhd").model == "uhd");
        // the other SoapySDR-opened kinds, from the forms their modules enumerate
        const RadioIdentity rtl = identify("soapy", "driver=rtlsdr, label=Generic RTL2832U OEM :: 00000001, manufacturer=Realtek, product=RTL2838UHIDIR, serial=00000001, tuner=Rafael Micro R820T");
        CHECK(rtl.model == "RTL2838UHIDIR" && rtl.library == "SoapySDR rtlsdr" && rtl.serial == "00000001");
        const RadioIdentity rtlLabel = identify("soapy", "driver=rtlsdr, label=Generic RTL2832U OEM :: 00000001, serial=00000001");
        CHECK(rtlLabel.model == "Generic RTL2832U OEM");
        const RadioIdentity hack = identify("soapy", "driver=hackrf, label=HackRF One #0 0000000000000000a06063c8234e4d5f, serial=0000000000000000a06063c8234e4d5f");
        CHECK(hack.model == "HackRF One" && hack.library == "SoapySDR hackrf");
        const RadioIdentity lime = identify("soapy", "driver=lime, label=LimeSDR Mini [USB 3.0] 1D4F2A9B3C, serial=1D4F2A9B3C");
        CHECK(lime.model == "LimeSDR Mini");
        const RadioIdentity pluto = identify("soapy", "driver=plutosdr, label=PlutoSDR (ip:192.168.2.1), uri=ip:192.168.2.1");
        CHECK(pluto.model.find("192.168") == std::string::npos && pluto.model.find("ip:") == std::string::npos && pluto.serial.empty());
        // whatever the arguments say, the model and the library are text the contract accepts
        for (const RadioIdentity& x : {a, b, b2, rtl, rtlLabel, hack, lime, pluto}) {
            CHECK(!x.model.empty() && x.model.size() <= 32 && scrubModel(x.model) == x.model);
            CHECK(x.library.size() <= 48 && x.library.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._+ -") == std::string::npos);
        }
        CHECK(soapyDeviceModel("driver=uhd, label=B200 EDR04ZDB2, serial=EDR04ZDB2") == "B200");
        CHECK(soapyDeviceModel("driver=uhd, serial=EDR04ZDB2").empty());
    }

    // --- the scrub: the real D3 sentence, and every kind of thing the text rules refuse ------------------------------------------------------------
    {
        const std::string d3 = "the radio stopped answering while restoring the crystal trim: a control transfer failed (Windows error 31, serial airspyhf_sn:HF0001A2)";
        const std::string out = scrubReason(d3, "HF0001A2");
        CHECK(out.find("HF0001A2") == std::string::npos);
        CHECK(textCode(out, "HF0001A2").empty());
        CHECK(out.find("control transfer failed") != std::string::npos);   // the sentence itself survives
        CHECK(health::radioReasonWord(health::classifyRadioOpen("airspyhf", d3)) == std::string("control"));

        const std::vector<std::string> nasty = {
            "could not open C:\\Users\\alice\\AppData\\Local\\FoxSDR\\radio.cfg",
            "cannot read /home/bob/.config/foxsdr/x.json",
            "failed at \\\\NAS\\share\\iq\\file.wav now",
            "tuned to 433.920 MHz and 1.2 kHz away: failed",
            "the server 192.168.1.50:1234 refused",
            "device EDR04ZDB2 went away (bus 3, serial EDR04ZDB2)",
            "path D:/recordings/x.iq not found",
            "listening on 145.800MHz and 7,100.5 kHz",
        };
        for (const std::string& n : nasty) {
            const std::string o = scrubReason(n, "EDR04ZDB2");
            if (!textCode(o, "EDR04ZDB2").empty()) { std::printf("STILL REFUSED: [%s] -> [%s] (%s)\n", n.c_str(), o.c_str(), textCode(o, "EDR04ZDB2").c_str()); }
            CHECK(textCode(o, "EDR04ZDB2").empty());
            CHECK(o.find("alice") == std::string::npos);
            CHECK(o.find("192.168") == std::string::npos);
            CHECK(o.find("EDR04ZDB2") == std::string::npos);
            CHECK(!o.empty());
        }
        // every path becomes <path>
        CHECK(pathsToToken("opened C:\\Users\\<user>\\x\\y.log, then /home/<user>/z") == "opened <path>, then <path>");
        CHECK(pathsToToken("tcp://host:1 is not a path") == "tcp://host:1 is not a path");
        // A PATH WITH A SPACE IN A FOLDER NAME IS TAKEN WHOLE (field data 2026-11-19: 16 of 20 reports kept the tail of the SDRplay API path after
        // "<path>" because the path ended at the space of "C:/Program Files"; a folder with a name in it would have leaked the name the same way).
        CHECK(pathsToToken("SDRplay API - C:\\Program Files\\SDRplay\\API\\x64\\sdrplay_api.dll was not found") == "SDRplay API - <path> was not found");
        CHECK(pathsToToken("SDRplay API - C:\\Program Files\\SDRplay\\API\\x64\\<file>") == "SDRplay API - <path>");
        CHECK(pathsToToken("loaded C:\\Program Files (x86)\\Foo Bar\\x64\\a.dll, next") == "loaded <path>, next");
        CHECK(pathsToToken("cannot open C:\\Users\\John Smith\\AppData\\Local\\FoxSDR\\x.log;") == "cannot open <path>;");
        CHECK(pathsToToken("cannot open C:\\Users\\<user>\\AppData\\Local\\FoxSDR\\x.log;") == "cannot open <path>;");
        // ...but a plain word after a finished file name is not swallowed
        CHECK(pathsToToken("opened C:\\a\\b.log then C:\\c\\d.log") == "opened <path> then <path>");
        CHECK(pathsToToken("opened C:\\a\\b then it failed") == "opened <path> then it failed");
        CHECK(pathsToToken("no paths here") == "no paths here");
        // empty reason is still a reason; long text is cut on a character boundary
        CHECK(!scrubReason("", "").empty());
        const std::string longText(1000, 'x');
        CHECK(utf8Length(scrubReason(longText, "")) == 400);
        const std::string wide = scrubReason(std::string(300, 'a') + "\xC3\xA9\xC3\xA9\xC3\xA9", "");
        CHECK(utf8Length(wide) <= 400);
        CHECK(clipUtf8("a\xC3\xA9" "b", 2) == "a\xC3\xA9");
    }

    // --- R9 and L8 (security reviews 1 and 2): every kind of thing the scrub used to let through, by input. The secret goes IN to scrubReason; the scrubbed text is
    // searched for it. (textCode, the Worker's parity rules, is unchanged: this is the first line, the Worker's check the second.)
    {
        struct Leak { const char* input; const char* secret; };
        const Leak leaks[] = {
            // a host name in prose, not introduced by ip: host= uri= or addr=
            {"could not find the host mynas-pi", "mynas-pi"},
            {"the SoapyRemote stream could not be set up: could not find the host radio-shack", "radio-shack"},
            {"cannot listen for I/Q on raspberrypi:1234", "raspberrypi"},
            {"connect to studio.example.org failed", "studio.example.org"},
            {"gave up on mynas.lan after 3 tries", "mynas.lan"},
            // a MAC address
            {"adapter 00:1A:2B:3C:4D:5E refused the link", "00:1A:2B:3C:4D:5E"},
            {"adapter 00-1a-2b-3c-4d-5e refused the link", "00-1a-2b-3c-4d-5e"},
            // Linux and macOS paths
            {"cannot read /media/alice2/SDR/capture.iq", "alice2"},
            {"cannot read /run/media/alice2/SDR", "alice2"},
            {"mount /mnt/nas-secret/rf failed", "nas-secret"},
            {"cannot open /root/.config/secretdir", "secretdir"},
            {"libusb: no access to /dev/bus/usb/001/087", "/dev/bus"},
            {"wrote /tmp/secretdir/x", "secretdir"},
            {"opened /Volumes/AliceDisk/x", "AliceDisk"},
            // a Windows device interface path and a USB instance path (another device's serial rides in them)
            {"open failed on \\\\?\\usb#vid_0bda&pid_2838#0000AB12CD#{a5dcbf10-6530-11d2-901f-00c04fb951ed}", "0000AB12CD"},
            {"open failed on USB\\VID_0BDA&PID_2838\\0000AB12CD", "0000AB12CD"},
            {"open failed on \\\\.\\USB#VID_0BDA&PID_2838#0000AB12CD", "0000AB12CD"},
            // a bare file name
            {"could not read my_private_capture_88.iq", "my_private_capture_88"},
            {"profile alice-radio.cfg is damaged", "alice-radio"},
            {"cannot load D3ADBEEF-notes.txt", "D3ADBEEF-notes"},
            // another device's serial, with no word like serial in front of it
            {"the second dongle 0000AB12CD did not answer", "0000AB12CD"},
            {"device EDR99ZZ77 went away", "EDR99ZZ77"},
            // a frequency written without its unit
            {"tuned to 14200 failed", "14200"},
            {"freq 145.5 refused", "145.5"},
            {"the tuner could not centre on 7100.25", "7100.25"},
        };
        for (const Leak& l : leaks) {
            const std::string o = scrubReason(l.input, "SN0000AA");
            if (o.find(l.secret) != std::string::npos) { std::printf("LEAK: [%s] -> [%s] still holds [%s]\n", l.input, o.c_str(), l.secret); }
            CHECK(o.find(l.secret) == std::string::npos);
            CHECK(!o.empty());
        }
        // what must SURVIVE (the scrub is not a shredder): the sentence, a version, a rate, an error number, a port, a time
        CHECK(scrubReason("stream stalled for 250 ms at 2400000 S/s (Windows error 31)", "").find("stalled for 250 ms") != std::string::npos);
        CHECK(scrubReason("FoxSDR 0.99.109 build 1234 on port 8073", "").find("0.99.109") != std::string::npos);
        CHECK(scrubReason("FoxSDR 0.99.109 build 1234 on port 8073", "").find("8073") != std::string::npos);
        CHECK(scrubReason("the tuner PLL did not lock, e.g. after a retry", "").find("did not lock, e.g. after a retry") != std::string::npos);
        CHECK(scrubReason("could not open the device", "").find("could not open the device") != std::string::npos);
        // an input that is a megabyte of digits and dots is cut before any rule runs, and the answer is bounded
        const std::string huge = std::string(1000000, '1') + ".5 Hz";
        const std::string hugeOut = scrubReason(huge, "");
        CHECK(utf8Length(hugeOut) <= 400);
        const std::string hugeDots = [] { std::string t; for (int i = 0; i < 200000; ++i) { t += "1."; } return t + "x"; }();
        CHECK(utf8Length(scrubReason(hugeDots, "")) <= 400);
        // idempotent: scrubbing the scrubbed text again changes nothing
        const std::string once = scrubReason("could not find the host mynas-pi at /media/alice2/x.iq from 00:1A:2B:3C:4D:5E, tuned to 14200", "");
        CHECK(scrubReason(once, "") == once);
    }

    // --- the radio lines of the log ring ------------------------------------------------------------------------------------------------------
    {
        std::vector<std::string> ring;
        ring.push_back("12:01:07.120 info window: opened the main window");                                             // not a radio line
        ring.push_back("12:01:07.121 warn soapy: airspyhf open failed: control transfer failed (Windows error 31)");   // a radio line
        ring.push_back("12:01:07.200 info source: tuned to 100.000 MHz");                                                // a radio line, with a frequency
        ring.push_back("12:01:07.331 warn source: the saved radio (Airspy HF+, airspyhf) did not reopen (control) - the receiver is on the signal generator");
        ring.push_back("12:01:08.000 info patch: radio node 3 would not open: C:\\Users\\alice\\x.iq");                // a radio line with a path
        ring.push_back("12:01:09.000 info bookmarks: loaded 12");                                                       // not a radio line
        ring.push_back("12:01:09.500 info vendor: serial=EDR04ZDB2 attached");
        const std::vector<std::string> got = radioLines(ring, "EDR04ZDB2");
        CHECK(got.size() == 5);
        for (const std::string& l : got) {
            CHECK(textCode(l, "EDR04ZDB2").empty());
            CHECK(l.find(" info ") == std::string::npos && l.find(" warn ") == std::string::npos);   // the ring's level word is dropped
            CHECK(l.size() <= 240);
        }
        CHECK(!got.empty() && got.front().rfind("12:01:07.121 soapy: ", 0) == 0);
        CHECK(!got.empty() && got.back().find("EDR04ZDB2") == std::string::npos);
        // at most 40, the NEWEST kept, oldest first
        std::vector<std::string> many;
        for (int i = 0; i < 100; ++i) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "12:%02d:00.000 info source: line number %d", i % 60, i);
            many.push_back(buf);
        }
        const std::vector<std::string> cut = radioLines(many, "");
        CHECK(cut.size() == 40);
        CHECK(cut.back().find("line number 99") != std::string::npos);
        CHECK(cut.front().find("line number 60") != std::string::npos);
        CHECK(radioLines({}, "").empty());
    }

    // --- helpers -----------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string a = randomHex(16);
        const std::string b = randomHex(16);
        CHECK(a.size() == 16 && b.size() == 16 && a != b);
        for (char c : a) { CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')); }
        CHECK(cpuWithoutClock("Intel(R) Core(TM) i7-8700K CPU @ 3.70GHz") == "Intel(R) Core(TM) i7-8700K CPU");
        CHECK(textCode(cpuWithoutClock("Intel(R) Core(TM) i7-8700K CPU @ 3.70GHz"), "").empty());
        CHECK(versionText("libairspyhf 1.6.8 (x)") == "libairspyhf 1.6.8 x");
        CHECK(kRadioReportsLiveSend);   // THE ONE SWITCH IS ON in this build (0.99.109 sends; was: pinned false)
    }

    // --- 2026-11-23: THE VENDOR'S OWN WORDS REACH THE LOG LINE OF A FAILED OPEN (field: "(vendor)" with no text) -------------------------------------------
    {
        const std::string args = "driver=uhd, label=B200, serial=ABC12345, type=b200";
        const std::string said = "open failed: LookupError: KeyError: No devices found for -----> Device Address: driver: uhd serial: ABC12345 (args: " + args + ")";
        const std::string words = openFailureWords("soapy", args, said);
        CHECK(words.find("No devices found") != std::string::npos);      // the vendor's sentence is there ...
        CHECK(words.find("ABC12345") == std::string::npos);              // ... with the serial scrubbed out
        CHECK(words.size() <= 160);
        CHECK(openFailureWords("soapy", args, "") == "the driver gave no reason");
        CHECK(openFailureWords("soapy", args, "  \r\n") == "the driver gave no reason");
    }

    return testSummary("test_radio_report");
}
