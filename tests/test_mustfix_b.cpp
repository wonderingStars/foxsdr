// Must-fix lane B tests: the radio failures that still reported to nobody (docs/multi-radio-records/mustfix-b.md).
//   1. the failure CLASS of every sentence the drivers and the USB transport really produce (each sentence names where it is in the code), and `other` carrying the text;
//   2. a refusal the driver did not say is said once by the engine's watch, and one the driver did say is not said twice;
//   3. a radio that is not a registered source (the sweeper, the one-click installer): a failed start is a radio_fail, a stopped sweep a radio_close;
//   4. the first-run check: every family of the native drivers is seen from a stand-in bus, a timed-out probe ends as "not checked", radios with no usable driver are
//      filed as radio_found, a usable one is not, and nothing a person could be identified by goes out.
// No radio, no network (a stand-in transport), no window.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/health_events.hpp"
#include "core/radio_events.hpp"
#include "core/radio_report.hpp"
#include "core/radio_reporter.hpp"
#include "core/radio_setup.hpp"
#include "core/winusb_install.hpp"
#include "test_check.hpp"

namespace rr = cascade::core::radioreport;
namespace health = cascade::core::health;
namespace rs = cascade::core::radiosetup;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

const char* const kInstallId = "a1b2c3d4e5f60718293a4b5c6d7e8f90";

struct Rig {
    fs::path dir;
    std::mutex mu;
    std::vector<std::string> bodies;
    Rig() {
        dir = fs::temp_directory_path() / ("mfb-" + rr::randomHex(8));
        fs::create_directories(dir);
    }
    ~Rig() {
        rr::global().shutdown();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    void start(bool usage = true) {
        rr::Reporter::Options o;
        o.dir = (dir / "radio-reports").string();
        o.endpoint = "http://127.0.0.1:9/";
        o.liveSend = true;
        o.transport = [this](const std::string&, const std::string& body) {
            std::lock_guard<std::mutex> lk(mu);
            bodies.push_back(body);
            return rr::PostResult{true, 204, 0, ""};
        };
        if (usage) { o.identity.installId = kInstallId; }
        o.identity.version = "0.99.109";
        o.identity.os = "Windows 10.0.22631";
        o.identity.arch = "x64";
        o.identity.launches = 3;
        rr::global().configure(o);
    }
    std::vector<json> flush() {
        {
            std::lock_guard<std::mutex> lk(mu);
            bodies.clear();
        }
        rr::global().runBatchNow();
        std::lock_guard<std::mutex> lk(mu);
        std::vector<json> out;
        for (const std::string& b : bodies) { out.push_back(json::parse(b)); }
        return out;
    }
};

std::vector<json> ofKind(const std::vector<json>& all, const char* kind) {
    std::vector<json> out;
    for (const json& m : all) {
        if (m.value("kind", std::string()) == kind) { out.push_back(m); }
    }
    return out;
}

int g_source = 0;
const rr::RadioIdentity kRtl = rr::identify("rtlsdr", "serial=00000001");

struct Sentence {
    const char* kind;
    const char* said;
    health::RadioReason want;
    const char* where;   // the code that says it
};

}  // namespace

int main() {
    std::printf("test_mustfix_b\n");
    using R = health::RadioReason;

    // ---- 1. THE CLASS OF EVERY REAL SENTENCE ---------------------------------------------------------------------------------------------------------
    const std::vector<Sentence> sentences = {
        // the USB transport (usb/winusb_device.cpp lastErrorText: "<what> failed (Windows error N)")
        {"rtlsdr", "the radio stopped answering while starting the receiver: a control transfer failed (Windows error 31)", R::Control, "airspy/hackrf transports"},
        {"rtlsdr", "a control transfer failed (Windows error 31)", R::Control, "winusb_device.cpp control transfer"},
        {"rtlsdr", "a control transfer did not answer within its timeout", R::Timeout, "winusb_device.cpp control transfer timeout"},
        {"rtlsdr", "a bulk read failed (Windows error 31)", R::Control, "winusb_device.cpp bulk read"},
        {"rtlsdr", "queueing a bulk read failed (Windows error 995)", R::Control, "winusb_device.cpp queueBulk"},
        {"rtlsdr", "waiting for a bulk read failed (Windows error 6)", R::Control, "winusb_device.cpp bulk wait"},
        {"airspy", "a bulk write failed (Windows error 121)", R::Timeout, "winusb_device.cpp bulk write, ERROR_SEM_TIMEOUT"},
        {"rtlsdr", "WinUsb_ResetPipe failed (Windows error 31)", R::Control, "winusb_device.cpp resetPipe (the name holds 'winusb': not a driver-binding fault)"},
        {"rtlsdr", "WinUsb_SetPipePolicy(RAW_IO) failed (Windows error 87)", R::Control, "winusb_device.cpp beginBulkStream"},
        {"rtlsdr", "an isochronous read failed (Windows error 31)", R::Control, "winusb_device.cpp readIso"},
        {"rtlsdr", "opening the radio failed (Windows error 5)", R::Bind, "winusb_device.cpp openWinUsb, ERROR_ACCESS_DENIED"},
        {"rtlsdr", "opening the radio failed (Windows error 32)", R::Busy, "winusb_device.cpp openWinUsb, ERROR_SHARING_VIOLATION"},
        {"rtlsdr", "opening the radio failed (Windows error 1167)", R::Absent, "winusb_device.cpp openWinUsb, ERROR_DEVICE_NOT_CONNECTED"},
        {"rtlsdr", "opening the radio failed (Windows error 433)", R::Absent, "winusb_device.cpp openWinUsb, ERROR_NO_SUCH_DEVICE"},
        {"rtlsdr", "opening the radio failed (Windows error 1117)", R::Control, "winusb_device.cpp openWinUsb, ERROR_IO_DEVICE"},
        {"rtlsdr", "WinUsb_Initialize (is the device bound to WinUSB?) failed (Windows error 31)", R::Bind, "winusb_device.cpp openWinUsb: stays a binding fault"},
        // the RTL-SDR driver's own open sentences (rtlsdr_source.cpp, tuner_r82xx.cpp)
        {"rtlsdr", "the tuner stopped answering on the I2C bus", R::Timeout, "tuner_r82xx.cpp"},
        {"rtlsdr", "the tuner's PLL did not report lock", R::Control, "tuner_r82xx.cpp"},
        {"rtlsdr", "the tuner's PLL would not lock for its filter calibration", R::Control, "tuner_r82xx.cpp"},
        {"rtlsdr", "the tuner would not initialise: a control transfer failed [r82xx]", R::Control, "rtlsdr_source.cpp"},
        {"rtlsdr", "the tuner would not accept its default frequency", R::Control, "rtlsdr_source.cpp"},
        {"rtlsdr", "the demodulator would not initialise: something", R::Control, "rtlsdr_source.cpp"},
        {"rtlsdr", "the demodulator would not accept its intermediate-frequency settings", R::Control, "rtlsdr_source.cpp"},
        {"rtlsdr", "the demodulator's I2C repeater would not switch on", R::Control, "rtlsdr_source.cpp"},
        {"rtlsdr", "the radio's bulk endpoint would not start: x", R::Control, "rtlsdr_source.cpp startStreamLocked"},
        {"rtlsdr", "the radio would not reset its endpoint before streaming", R::Control, "rtlsdr_source.cpp startStreamLocked"},
        // unchanged classes
        {"rtlsdr", "the radio is already in use by another program", R::Busy, "usb_device.hpp kInUseError"},
        {"hackrf", "no HackRF with serial 0000 is connected", R::Absent, "hackrf_source.cpp"},
        // what nothing recognises is still `other`
        {"rtlsdr", "the radio would not open", R::Other, "rtlsdr_source.cpp"},
        {"hackrf", "something unforeseen", R::Other, "-"},
    };
    for (const Sentence& s : sentences) {
        const R got = health::classifyRadioOpen(s.kind, s.said);
        if (got != s.want) { std::printf("FAIL (%s) %s said \"%s\": %s, wanted %s\n", s.where, s.kind, s.said, health::radioReasonWord(got), health::radioReasonWord(s.want)); }
        CHECK(got == s.want);
    }

    // `other` carries the scrubbed sentence in its report, so the next unknown one is seen
    {
        Rig t;
        t.start();
        rr::FailureInput in;
        in.driverKind = "rtlsdr";
        in.args = "serial=00000001";
        const std::string said = "the radio would not open after the hotplug at C:\\Users\\someone\\x serial 00000001";
        in.reason = said;
        in.reasonClass = health::radioReasonWord(health::classifyRadioOpen("rtlsdr", said));
        rr::global().fileFailure(in, {});
        const std::vector<json> fails = ofKind(t.flush(), "radio_fail");
        CHECK(fails.size() == 1);
        if (!fails.empty()) {
            CHECK(fails[0]["body"]["reasonClass"] == "other");
            const std::string reason = fails[0]["body"]["reason"];
            CHECK(reason.find("the radio would not open") != std::string::npos);
            CHECK(reason.find("someone") == std::string::npos);     // the path is gone
            CHECK(reason.find("00000001") == std::string::npos);    // so is the serial
        }
    }

    // ---- 2. THE ENGINE'S WATCH: said once, never twice ---------------------------------------------------------------------------------------------
    {
        Rig t;
        t.start();
        rr::registerSource(&g_source, kRtl);
        {   // the driver says nothing: the engine says it
            rr::RefusalWatch w;
            w.noteRefusalIfSilent(&g_source, "rtlsdr", rr::Control::Antenna, "no such antenna");
        }
        {   // the driver already said it on this thread: the engine adds nothing
            rr::RefusalWatch w;
            rr::noteControlRefused(&g_source, "rtlsdr", rr::Control::Gain, "the tuner refused that gain");
            w.noteRefusalIfSilent(&g_source, "rtlsdr", rr::Control::Gain, "the radio refused");
        }
        {   // a refusal said on ANOTHER thread does not count as this call's own driver saying it
            rr::RefusalWatch w;
            std::thread([] { rr::noteControlRefused(&g_source, "rtlsdr", rr::Control::Bandwidth, "elsewhere"); }).join();
            w.noteRefusalIfSilent(&g_source, "rtlsdr", rr::Control::Ppm, "this call's own refusal");
        }
        const std::vector<json> ev = ofKind(t.flush(), "radio_settings");
        int antenna = 0, gain = 0, ppm = 0, bandwidth = 0;
        for (const json& m : ev) {
            const std::string r = m["body"].value("refused", std::string());
            if (r == "antenna") { ++antenna; }
            if (r == "gain") { ++gain; }
            if (r == "ppm") { ++ppm; }
            if (r == "bandwidth") { ++bandwidth; }
        }
        CHECK(antenna == 1);
        CHECK(gain == 1);
        CHECK(ppm == 1);
        CHECK(bandwidth == 1);
        rr::forgetRadio(kRtl.key);
    }
    {   // the sequence form used across two functions (the tune refusal)
        Rig t;
        t.start();
        rr::registerSource(&g_source, kRtl);
        const std::uint64_t mark = rr::refusalSequence();
        rr::noteRefusalIfSilentSince(mark, &g_source, "rtlsdr", rr::Control::Frequency, "outside the range this radio can do", true);
        const std::vector<json> ev = ofKind(t.flush(), "radio_settings");
        CHECK(ev.size() == 1);
        if (!ev.empty()) {
            const std::string reason = ev[0]["body"]["reason"];
            CHECK(reason.rfind("out of range:", 0) == 0);
            CHECK(!ev[0]["body"].contains("refused"));   // the contract has no word for a frequency
        }
        rr::forgetRadio(kRtl.key);
    }

    // ---- 3. A RADIO THAT IS NOT A REGISTERED SOURCE ----------------------------------------------------------------------------------------------
    {
        Rig t;
        t.start(/*usage=*/false);   // the failure report needs no install id
        rr::fileSideRadioFailure("hackrf", "serial=0000000000000000a06063c8234e925f", "the HackRF refused the sweep settings", "other");
        const std::vector<json> fails = ofKind(t.flush(), "radio_fail");
        CHECK(fails.size() == 1);
        if (!fails.empty()) {
            CHECK(fails[0]["body"]["driver"] == "hackrf");
            CHECK(fails[0]["body"]["trigger"] == "other");
            CHECK(fails[0]["body"]["serial"] == "0000000000000000a06063c8234e925f");
        }
    }
    {
        Rig t;
        t.start();
        rr::noteSideRadioStopped("hackrf", "serial=0000000000000000a06063c8234e925f", "the sweep stream stopped", "a bulk read failed: the device was unplugged");
        const std::vector<json> closes = ofKind(t.flush(), "radio_close");
        CHECK(closes.size() == 1);
        if (!closes.empty()) { CHECK(closes[0]["body"]["cause"] == "unplugged"); }
    }
    {   // the one-click installer: a failed install is a radio_fail, a finished or declined one is not
        Rig t;
        t.start(/*usage=*/false);
        namespace wu = cascade::core::winusb;
        for (const wu::Outcome o : {wu::Outcome::Installed, wu::Outcome::AlreadyWinUsb, wu::Outcome::Cancelled}) {
            wu::Result r;
            r.outcome = o;
            wu::noteInstallOutcome(r);
        }
        CHECK(ofKind(t.flush(), "radio_fail").empty());
        for (const wu::Outcome o : {wu::Outcome::Refused, wu::Outcome::NotFound, wu::Outcome::NotAdmin, wu::Outcome::Failed}) {
            wu::Result r;
            r.outcome = o;
            r.detail = "the install step failed (Windows error 5)";
            wu::noteInstallOutcome(r);
        }
        const std::vector<json> fails = ofKind(t.flush(), "radio_fail");
        CHECK(fails.size() == 4);
        for (const json& m : fails) { CHECK(m["body"]["driver"] == "rtlsdr"); }
    }

    // ---- 4. THE FIRST-RUN CHECK -----------------------------------------------------------------------------------------------------------------------
    {   // every family of the native drivers is on the list, from the driver's own match table
        struct Fam { std::uint16_t vid, pid; const char* family; };
        const Fam fams[] = {
            {0x38af, 0x0001, rs::kFamilyHydra}, {0x08d0, 0xa001, rs::kFamilyAor},
            {0x0c26, 0x0023, rs::kFamilyIcr8600}, {0x0c26, 0x0022, rs::kFamilyIcr8600},
            {0x1d50, 0x60a1, rs::kFamilyAirspy}, {0x03eb, 0x800c, rs::kFamilyAirspyHf}, {0x1d50, 0x6089, rs::kFamilyHackRf}, {0x0bda, 0x2838, rs::kFamilyRtl},
        };
        for (const Fam& f : fams) {
            const rs::KnownDevice* d = rs::findKnownDevice(f.vid, f.pid);
            CHECK(d != nullptr);
            if (d != nullptr) { CHECK(std::string(d->family) == f.family); }
            // a stand-in bus: the radio is on it and Windows has bound nothing usable to it
            rs::Inventory inv;
            inv.probed = true;
            inv.usbListed = true;
            rs::UsbNode n;
            n.vid = f.vid;
            n.pid = f.pid;
            n.deviceId = "USB\\VID_0000&PID_0000\\5&1&0&1";
            n.instanceId = n.deviceId;
            n.isDeviceNode = true;
            n.problemCode = 28;
            inv.usb.push_back(n);
            const rs::Findings found = rs::assess(inv);
            CHECK(!found.items.empty());
            if (!found.items.empty()) { CHECK(found.items[0].kind == rs::Kind::WrongDriver); CHECK(found.items[0].family == f.family); }
            // and with WinUSB bound it is usable
            inv.usb[0].service = "WinUSB";
            inv.usb[0].problemCode = 0;
            const rs::Findings ok = rs::assess(inv);
            if (!ok.items.empty() && std::string(f.family) != rs::kFamilyAirspy && std::string(f.family) != rs::kFamilyAirspyHf && std::string(f.family) != rs::kFamilyHackRf &&
                std::string(f.family) != rs::kFamilyRtl) {
                CHECK(ok.items[0].kind == rs::Kind::Usable);
            }
        }
        // the RX888's ids are Cypress FX3's own and are never claimed by id (the existing decision)
        CHECK(rs::findKnownDevice(0x04b4, 0x00f1) == nullptr);
        CHECK(rs::findKnownDevice(0x04b4, 0x00f3) == nullptr);
        CHECK(rs::familyForDriver("AOR") == rs::kFamilyAor);
    }
    {   // a probe that never finishes ends as "not checked": the setup.not-checked token is what reaches the owner
        rs::Probe probe(std::chrono::milliseconds(50));
        std::atomic<bool> release{false};
        probe.start([&release]() {
            while (!release.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
            return rs::Inventory{};
        });
        const auto t0 = std::chrono::steady_clock::now();
        while (!probe.poll() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        CHECK(probe.state() == rs::Probe::State::TimedOut);
        const rs::Findings f = rs::assess(probe.inventory());
        CHECK(f.items.size() == 1);
        if (!f.items.empty()) { CHECK(f.items[0].kind == rs::Kind::NotChecked); CHECK(std::string(rs::kindName(f.items[0].kind)) == "not-checked"); }
        release = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    {   // radio_found for a radio with no usable driver: filed once, with the USB id and the product name, no serial; a usable radio is not filed
        Rig t;
        t.start();
        rs::Inventory inv;
        inv.probed = true;
        inv.usbListed = true;
        rs::UsbNode bad;
        bad.vid = 0x38af;
        bad.pid = 0x0001;
        bad.deviceId = bad.instanceId = "USB\\VID_38AF&PID_0001\\SECRETSERIAL1";
        bad.isDeviceNode = true;
        bad.problemCode = 28;
        inv.usb.push_back(bad);
        rs::UsbNode good;
        good.vid = 0x0bda;
        good.pid = 0x2838;
        good.deviceId = good.instanceId = "USB\\VID_0BDA&PID_2838\\00000001";
        good.isDeviceNode = true;
        good.service = "WinUSB";
        inv.usb.push_back(good);
        inv.nativeChecked = false;
        const rs::Findings f = rs::assess(inv);
        rs::noteFindings(f);
        rs::noteFindings(f);   // the second time says nothing new
        const std::vector<json> found = ofKind(t.flush(), "radio_found");
        CHECK(found.size() == 1);
        if (!found.empty()) {
            CHECK(found[0]["body"]["driver"] == "hydrasdr");
            CHECK(found[0]["body"]["binding"] == "wrong-driver");
            CHECK(found[0]["body"]["source"] == "first-run-check");
            CHECK(found[0]["body"]["usbId"] == "38af:0001");
            CHECK(found[0]["body"]["serial"] == "");
            CHECK(found[0].dump().find("SECRETSERIAL1") == std::string::npos);
        }
    }

    return testSummary("test_mustfix_b");
}
