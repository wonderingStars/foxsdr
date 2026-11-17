// What the radio reports NEVER carry (owner decision 2026-11-15 and contract sections 2.4 and 4): a tuned or centre frequency, a bookmark, a file or
// recording name, a path, a position, anything decoded, a user or computer name, an address. A real session is scripted - a radio opened and tuned, a file
// opened, a bookmark made, a position set, a decoder printing - and the DIAGNOSTICS LOG RING of that session (which is where the radio lines of a failure
// report come from) holds every one of those things, in the forms the application really writes them. Then radios fail, the journal runs and the first
// session is made; the queue FILES (and every request the sender makes) are searched for each of them. None may be there.
//
// This extends the real-log search of tests/test_diagnostics.cpp to the radio reports' queue and messages. Its break-it is in the record: the scrub taken
// out of the reporter turns this test red.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/diag_log.hpp"
#include "core/radio_events.hpp"
#include "core/radio_report.hpp"
#include "core/radio_reporter.hpp"
#include "core/radio_setup.hpp"
#include "core/telemetry.hpp"
#include "core/winusb_install.hpp"
#include "test_check.hpp"

using namespace cascade::core;
namespace rr = cascade::core::radioreport;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// THE THINGS THAT MUST NEVER LEAVE, each with the forms it takes in a real log.
struct Secret {
    const char* what;
    const char* needle;   // searched for, case-insensitively, in everything queued and sent
};
const Secret kSecrets[] = {
    {"a tuned frequency in MHz", "433.920"},
    {"the same frequency in hertz", "433920000"},
    {"a second frequency (airband)", "118.100"},
    {"a frequency with a thousands comma", "433,920"},
    {"a bookmark's name", "MyPrivateBookmark"},
    {"a recording's file name", "zz_secret_capture"},
    {"a folder on the machine", "SecretFolder"},
    {"the user's account name", "alice"},
    {"the computer's name", "ALICE-DESKTOP"},
    {"a position (latitude)", "51.5007"},
    {"a position (longitude)", "0.1246"},
    {"decoded text", "meet at the old mill"},
    {"a callsign", "G4ZZZ"},
    {"a home network address", "192.168.7.77"},
    {"a hostname", "mynas.lan"},
    {"a password", "hunter2"},
    {"the radio's serial in a sentence", "SN9900AA"},
    // R9 and L8 (security reviews 1 and 2): what the scrub used to let through
    {"a host name in prose", "studio-nas7"},
    {"a MAC address", "00:1A:2B:3C:4D:5E"},
    {"a Linux user's media path", "alice2"},
    {"a file name on that path", "zz_secret_capture2"},
    {"a mount point", "nas-secret"},
    {"another device's serial in a device path", "0000AB12CD"},
    {"another device's serial with no word in front", "7F3A9C21B0"},
    {"a frequency in kHz with no unit", "14200"},
    {"a bare file name", "alice_notes_77"},
};

std::string lower(std::string s) {
    for (char& c : s) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); } }
    return s;
}

// The session's log: every kind of line the real code writes, with the user's data in it.
void scriptTheSession() {
    DiagLog& log = DiagLog::instance();
    log.resetForTest();
    diagLogf("source: opened Airspy HF+ (airspyhf) at 768000 S/s");
    diagLogf("source: tuned to 433.920000 MHz");
    diagLogf("source: asked for 433920000 Hz, the radio answered 433919998 Hz");
    diagLogf("soapy: the vendor reported centre 433,920 kHz and a 600 kHz span");
    diagLogf("source: opened 433920000 with serial=SN9900AA attached");
    diagLogf("source: retuned to 118.100 MHz (air band)");
    diagLogf("radio: bookmark 'MyPrivateBookmark' selected");
    diagLogf("recorder: opened C:\\Users\\alice\\Documents\\SecretFolder\\zz_secret_capture.iq for writing");
    diagLogf("source: playing I/Q file /home/alice/SecretFolder/zz_secret_capture.iq");
    diagLogf("patch: radio node 2 would not open: \\\\ALICE-DESKTOP\\share\\zz_secret_capture.iq");
    diagLogf("gps: position fix 51.5007 N, 0.1246 W");
    diagLogf("gps: the receiver is located at lat 51.5007 lon -0.1246");   // not a radio line: the filter leaves it out
    diagLogf("rtltcp: connecting to 192.168.7.77:1234 (mynas.lan)");
    diagLogf("vendor: libusb: error [windows_get_device_list] enumerating on \\\\ALICE-DESKTOP\\pipe\\usb");
    diagLogf("plugin: remote login as alice with password hunter2");   // not a radio line
    diagLogf("plugin: pager decoder printed: meet at the old mill, G4ZZZ calling");   // decoded text lives in a plugin line, never a radio line
    diagLogf("window: the plugin window for alice's profile opened");   // not a radio line: must not be sent at all
    diagLogf("source: could not find the host studio-nas7");
    diagLogf("vendor: adapter 00:1A:2B:3C:4D:5E refused the link");
    diagLogf("source: cannot read /media/alice2/SDR/zz_secret_capture2.iq");
    diagLogf("source: mount /mnt/nas-secret/rf failed");
    diagLogf("vendor: open failed on USB\\VID_0BDA&PID_2838\\0000AB12CD");
    diagLogf("source: the second dongle 7F3A9C21B0 did not answer");
    diagLogf("source: tuned to 14200 failed");
    diagLogf("source: could not read alice_notes_77.txt");
    diagWarnf("source: the saved radio (Airspy HF+, airspyhf) did not reopen (control) - the receiver is on the signal generator and the radio stays saved");
}

std::string everythingIn(const std::string& dir) {
    std::string all;
    for (const std::string& f : rr::readQueueFiles(dir)) { all += f; all += '\n'; }
    return all;
}

}  // namespace

int main() {
    std::string sent;   // every request the sender made
    std::mutex sentMutex;

    fs::path base = fs::temp_directory_path() / ("rrns-" + rr::randomHex(8));
    fs::create_directories(base);
    std::atomic<std::int64_t> clock{1794830400};

    auto identityOf = [](rr::Reporter::Options& o) {
        o.identity.installId = "a1b2c3d4e5f60718293a4b5c6d7e8f90";
        o.identity.version = "0.99.109";
        o.identity.os = "Windows 10.0.22631";
        o.identity.arch = "x64";
        o.identity.launches = 1;
    };
    // THE SHIPPED BUILD: the send is OFF, so what is searched is the queue as the files were written.
    rr::Reporter rep;
    rr::Reporter::Options o;
    o.dir = (base / "radio-reports").string();
    o.endpoint = "http://127.0.0.1:9/";
    o.liveSend = rr::kRadioReportsLiveSend;
    o.nowEpoch = [&clock]() { return clock.load(); };
    identityOf(o);
    rep.configure(o);
    // THE SAME SESSION WITH THE SEND ON, to a stand-in: what the wire would carry.
    fs::path base2 = fs::temp_directory_path() / ("rrns2-" + rr::randomHex(8));
    fs::create_directories(base2);
    rr::Reporter rep2;
    rr::Reporter::Options o2;
    o2.dir = (base2 / "radio-reports").string();
    o2.endpoint = "http://127.0.0.1:9/";
    o2.liveSend = true;
    o2.nowEpoch = [&clock]() { return clock.load(); };
    o2.transport = [&](const std::string&, const std::string& body) {
        std::lock_guard<std::mutex> lk(sentMutex);
        sent += body;
        sent += '\n';
        return rr::PostResult{true, 204, 0, ""};
    };
    identityOf(o2);
    rep2.configure(o2);

    scriptTheSession();
    const std::vector<std::string> ring = DiagLog::instance().ringSnapshot();

    // the whole session's reports, played into a reporter
    auto play = [&](rr::Reporter& rep) {
    // a radio fails to open: the reason is the driver's own sentence, which quotes everything it can
    rr::FailureInput f;
    f.driverKind = "airspyhf";
    f.args = "serial=SN9900AA";
    f.nativeLabel = "Airspy HF+ (serial SN9900AA)";
    f.reason = "could not open serial=SN9900AA at C:\\Users\\alice\\Documents\\SecretFolder\\radio.cfg tuned to 433.920 MHz via 192.168.7.77";
    f.reasonClass = "other";
    f.trigger = rr::Trigger::StartupRestore;
    f.openMs = 5230;
    f.sampleRateSps = 768000;
    rep.fileFailure(f, ring);
    // ...a second one whose sentence hides a frequency without a unit
    f.reason = "the stream at 433920000 stopped after host studio-nas7 (00:1A:2B:3C:4D:5E) lost /media/alice2/SDR/zz_secret_capture2.iq, dongle 7F3A9C21B0, tuned to 14200";
    f.trigger = rr::Trigger::UserPick;
    rep.fileFailure(f, ring);

    // the journal: hostile TEXT fields (an antenna named after a frequency, a reason that quotes a path), and a band that is a number
    const rr::RadioIdentity hf = rr::identify("airspyhf", "serial=SN9900AA", "Airspy HF+ (serial SN9900AA)");
    {
        rr::Event e;
        e.kind = "radio_settings";
        e.radio = hf;
        e.word = "refused";
        e.set("why", "user").set("refused", "ppm");
        e.text("reason", "refused 433.920 MHz at C:\\Users\\alice\\x.cfg (serial SN9900AA) by ALICE-DESKTOP", 200);
        e.text("antenna", "RX2 433.920MHz", 24);
        e.set("band", "433.920");   // a frequency where a band NAME belongs: the Worker would refuse it
        rep.event(e);
        rr::Event r;
        r.kind = "radio_recovery";
        r.radio = hf;
        r.word = "start";
        r.set("phase", "start").set("what", "reopen").text("reason", "stream stalled at 433,920 kHz after reading C:\\Users\\alice\\SecretFolder\\zz_secret_capture.iq", 400);
        rep.event(r);
        rr::Event c;
        c.kind = "radio_close";
        c.radio = hf;
        c.word = "died";
        c.set("cause", "died").set("openSec", 12).text("reason", "the radio at 192.168.7.77 (mynas.lan) went away", 400);
        rep.event(c);
        rr::Event env;
        env.kind = "session_env";
        env.text("cpu", "Intel(R) Core(TM) i7-8700K CPU @ 3.70GHz", 64);
        env.set("audioApi", "wasapi");
        env.text("gpu", "NVIDIA GeForce GTX 1060", 64);
        env.fields.emplace_back("usbHosts", rr::FieldValue(std::vector<std::string>{"Intel USB 3.1 eXtensible Host Controller", "host at 192.168.7.77"}));
        rep.event(env);
        // a forbidden NAME is refused whatever its value (contract 4)
        rr::Event bad;
        bad.kind = "radio_stream";
        bad.radio = hf;
        bad.set("windowSec", 60).set("samplesPerSec", 768000).set("drops", 0).set("tunedHz", 433920000LL);
        rep.event(bad);
        rr::Event bad2;
        bad2.kind = "radio_stream";
        bad2.radio = hf;
        bad2.set("windowSec", 60).set("samplesPerSec", 768000).set("drops", 0).set("bookmark", "MyPrivateBookmark");
        clock += 120;
        rep.event(bad2);
        rr::Event bad3;
        bad3.kind = "radio_open";
        bad3.radio = hf;
        bad3.word = "start";
        bad3.set("phase", "start").set("trigger", "user-pick").set("filename", "zz_secret_capture.iq").set("latitude", 51.5007);
        rep.event(bad3);
    }
    // the first session, from a usage record that names a plugin and a mode and an SDR whose model came with an address
    const std::string usage =
        "{\"id\":\"a1b2c3d4e5f60718293a4b5c6d7e8f90\",\"v\":\"0.99.109\",\"os\":\"Windows 10.0.22631\",\"arch\":\"x64\",\"launches\":1,\"crashes\":0,\"exits_died\":0,\"exits_killed\":0,"
        "\"exits_ended\":0,\"exits_unknown\":0,\"stalls\":0,\"health\":\"\",\"ch\":\"installer\",\"uch\":\"stable\",\"first\":\"2026-11-16\",\"fv\":\"0.99.109\",\"sessionSec\":61,"
        "\"sdr\":\"uhd 192.168.7.77 b200\",\"radios_max\":1,\"modes\":{\"WFM\":40},\"panels\":[\"map\"],\"plugins\":[\"ADS-B 1.0.0\"],\"position\":\"51.5007,0.1246\",\"freq\":433920000}";
    clock += 61;
    rep.firstSession(usage, "after-minute");
    };
    play(rep);
    const std::string queueOff = everythingIn(rep.dir());   // as written: nothing has been sent, the send is off
    CHECK(!queueOff.empty());
    CHECK(rep.stats().posts == 0);
    clock -= 120 + 61;   // (the clock is the session's; play() moved it on)
    play(rep2);
    rep2.flushAtQuit(500);   // sends what was queued (to the stand-in)
    // THE DRIVER-LEVEL EVENTS (mustfix-b), each handed the hostile text a real sentence could quote, into the process-wide reporter the helpers use: a radio that would not
    // start for a sweep, a sweep that stopped, a one-click install that failed, a setting the engine's watch reports, a first-run check that found a radio with no driver
    // (its instance id carries a planted serial, which the bus walk's finding must not carry), and the vendor-fault helper.
    fs::path base3 = fs::temp_directory_path() / ("rrns3-" + rr::randomHex(8));
    fs::create_directories(base3);
    {
        rr::Reporter::Options o3;
        o3.dir = (base3 / "radio-reports").string();
        o3.endpoint = "http://127.0.0.1:9/";
        o3.liveSend = true;
        o3.nowEpoch = [&clock]() { return clock.load(); };
        o3.transport = o2.transport;
        identityOf(o3);
        rr::global().configure(o3);
        const std::string hostile = "refused 433.920 MHz at C:\\Users\\alice\\Documents\\SecretFolder\\zz_secret_capture.iq (serial SN9900AA) via 192.168.7.77 and host studio-nas7 (00:1A:2B:3C:4D:5E), dongle 7F3A9C21B0, tuned to 14200";
        rr::fileSideRadioFailure("hackrf", "serial=SN9900AA", hostile);
        rr::noteSideRadioStopped("hackrf", "serial=SN9900AA", "the sweep at 433.920 MHz stopped", hostile);
        cascade::core::winusb::Result wr;
        wr.outcome = cascade::core::winusb::Outcome::Failed;
        wr.detail = hostile;
        cascade::core::winusb::noteInstallOutcome(wr);
        static int src = 0;
        rr::registerSource(&src, rr::identify("airspyhf", "serial=SN9900AA", "Airspy HF+ (serial SN9900AA)"));
        rr::RefusalWatch watch;
        watch.noteRefusalIfSilent(&src, "airspyhf", rr::Control::Frequency, hostile);
        rr::noteVendorCallFault(&src, "airspyhf", "vendorcall", hostile, false);
        radiosetup::Inventory inv;
        inv.probed = true;
        inv.usbListed = true;
        radiosetup::UsbNode n;
        n.vid = 0x38af;
        n.pid = 0x0001;
        n.deviceId = n.instanceId = "USB\\VID_38AF&PID_0001\\SN9900AA";
        n.isDeviceNode = true;
        n.problemCode = 28;
        inv.usb.push_back(n);
        radiosetup::noteFindings(radiosetup::assess(inv));
        rr::forgetRadio(rr::identify("airspyhf", "serial=SN9900AA", "Airspy HF+ (serial SN9900AA)").key);
        rr::global().flushAtQuit(500);
    }
    const std::string queueAfter = everythingIn(rep2.dir()) + everythingIn(rr::global().dir());
    std::string sentCopy;
    {
        std::lock_guard<std::mutex> lk(sentMutex);
        sentCopy = sent;
    }
    CHECK(!sentCopy.empty());   // the stand-in got the messages: the search below is not of an empty wire

    // THE SERIAL IS ALLOWED IN TWO PLACES - its own field and the radio key - and nowhere else: those two are taken out before it is searched for.
    auto withoutOwnSerial = [](std::string text) {
        for (const char* pat : {"\"serial\":\"SN9900AA\"", "\"serial\": \"SN9900AA\"", ":SN9900AA\"", ":SN9900AA|"}) {
            std::size_t at;
            while ((at = text.find(pat)) != std::string::npos) { text.erase(at, std::string(pat).size()); }
        }
        return text;
    };
    const std::string queueAfterS = withoutOwnSerial(queueAfter), queueOffS = withoutOwnSerial(queueOff), sentCopyS = withoutOwnSerial(sentCopy);
    int leaks = 0;
    for (const Secret& s : kSecrets) {
        const std::pair<const char*, const std::string*> hays[] = {{"the queue", &queueAfterS}, {"the queue (switch off)", &queueOffS}, {"the wire", &sentCopyS}};
        for (const auto& hay : hays) {
            if (lower(*hay.second).find(lower(s.needle)) != std::string::npos) {
                ++leaks;
                const std::size_t at = lower(*hay.second).find(lower(s.needle));
                std::printf("LEAK: %s (%s) found in %s: ...%s...\n", s.what, s.needle, hay.first, hay.second->substr(at > 60 ? at - 60 : 0, 140).c_str());
            }
        }
    }
    CHECK(leaks == 0);

    // the same, structurally: no forbidden key at any depth, no string with a path or a hertz number, and the serial only in its own field and the key
    int forbidden = 0;
    int messages = 0;
    std::vector<std::string> allBodies;
    for (const std::string& file : rr::readQueueFiles(rep.dir())) { allBodies.push_back(file); }
    for (const std::string& file : rr::readQueueFiles(rep2.dir())) { allBodies.push_back(file); }
    for (const std::string& file : rr::readQueueFiles(rr::global().dir())) { allBodies.push_back(file); }
    allBodies.push_back(sentCopy);   // and every request the sender made: one message a line
    for (const std::string& file : allBodies) {
        // a failure file is one JSON object; a journal segment is lines of them
        std::vector<std::string> docs;
        if (!file.empty() && file[0] == '{' && file.find('\n') == std::string::npos) { docs.push_back(file); }
        else {
            std::size_t pos = 0;
            while (pos < file.size()) {
                std::size_t nl = file.find('\n', pos);
                if (nl == std::string::npos) { nl = file.size(); }
                docs.push_back(file.substr(pos, nl - pos));
                pos = nl + 1;
            }
        }
        for (const std::string& d : docs) {
            const json j = json::parse(d, nullptr, false);
            if (!j.is_object()) { continue; }
            ++messages;
            std::vector<const json*> stack{&j};
            while (!stack.empty()) {
                const json* cur = stack.back();
                stack.pop_back();
                if (cur->is_object()) {
                    for (auto it = cur->begin(); it != cur->end(); ++it) {
                        const std::string k = lower(it.key());
                        for (const char* bad : {"freq", "hertz", "centre", "center", "tuned", "bookmark", "position", "latitude", "longitude", "gps", "ipaddr", "hostname", "username", "filename", "folder", "decoded", "callsign", "password", "secret"}) {
                            if (k.find(bad) != std::string::npos) { ++forbidden; std::printf("FORBIDDEN KEY %s\n", it.key().c_str()); }
                        }
                        if (k == "path" || k == "file" || k == "user" || k == "ip" || k == "token" || k == "lat" || k == "lon") { ++forbidden; }
                        stack.push_back(&it.value());
                    }
                } else if (cur->is_array()) {
                    for (const json& x : *cur) { stack.push_back(&x); }
                } else if (cur->is_string()) {
                    const std::string v = cur->get<std::string>();
                    if (rr::pathInText(v) || rr::frequencyInText(v)) { ++forbidden; std::printf("TEXT WITH A PATH OR A HERTZ NUMBER: %s\n", v.c_str()); }
                }
            }
        }
    }
    std::printf("never sent: %zu secrets searched in %d queued messages and the wire: %d leaks, %d forbidden\n", sizeof(kSecrets) / sizeof(kSecrets[0]), messages, leaks, forbidden);
    CHECK(messages >= 8);
    CHECK(forbidden == 0);

    // what WAS refused by the guard is counted (the three hostile events and the usage record's extra keys), never silently
    CHECK(rep.stats().guardRefused >= 3);

    rep.shutdown();
    rep2.shutdown();
    rr::global().shutdown();
    std::error_code ec;
    fs::remove_all(base3, ec);
    fs::remove_all(base, ec);
    fs::remove_all(base2, ec);
    return testSummary("test_radio_reports_never_sent");
}
