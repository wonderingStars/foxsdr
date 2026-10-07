// Tests for core/radio_setup.hpp - the first-run radio setup check.
//
// The report behind it: 578 of 813 installs in a month reported no radio at
// all, and the few reasons a radio is invisible each have one fix (an RTL-SDR
// on the DVB-T driver, an RSP with no SDRplay API, a USRP with no UHD, no radio
// plugged in). assess() is pure, so every machine below is an Inventory built
// by hand - the shapes a real bus produces, including the one that fooled the
// first reading of the bench: a WORKING RTL-SDR whose second interface shows
// problem code 28 in Device Manager.
//
// Four questions are asked of each machine: which kind each radio gets, whether
// the page would open by itself, that every finding carries something to
// install and a link, and (for the probe thread) that a probe which never
// returns costs a state and not a frozen caller.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/radio_setup.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "test_check.hpp"

namespace rs = cascade::core::radiosetup;
using rs::Findings;
using rs::Inventory;
using rs::Kind;
using rs::UsbNode;

namespace {

// --- machines ------------------------------------------------------------------

UsbNode node(std::uint16_t vid, std::uint16_t pid, const char* deviceId, int interfaceNumber,
             const char* service, const char* provider, unsigned problem = 0) {
    UsbNode n;
    n.vid = vid;
    n.pid = pid;
    n.deviceId = deviceId;
    n.interfaceNumber = interfaceNumber;
    n.isDeviceNode = interfaceNumber < 0;
    n.service = service;
    n.provider = provider;
    n.problemCode = problem;
    return n;
}

Inventory machine() {
    Inventory inv;
    inv.probed = true;
    inv.usbListed = true;
    inv.nativeChecked = true;
    return inv;
}

// A composite RTL2832U dongle: usbccgp on the device node, the SDR half as
// interface 0 under `service`, and the remote-control half as interface 1
// with no driver (problem code 28 - which the bench RTL-SDR shows while
// streaming perfectly).
void addRtl(Inventory& inv, const char* deviceId, const char* service, const char* provider,
            unsigned problem = 0) {
    inv.usb.push_back(node(0x0bda, 0x2838, deviceId, -1, "usbccgp", "Microsoft"));
    inv.usb.push_back(node(0x0bda, 0x2838, deviceId, 0, service, provider, problem));
    inv.usb.push_back(node(0x0bda, 0x2838, deviceId, 1, "", "", 28));
}

void listedAs(Inventory& inv, const char* family) {
    inv.visible.push_back(family);
    inv.visibleCount += 1;
}

void addRsp(Inventory& inv, bool dll, bool running, bool present) {
    inv.usb.push_back(
        node(0x1df7, 0x3030, "USB\\VID_1DF7&PID_3030\\A", -1, "WinUSB", "SDRplay"));
    inv.sdrplay.dllFound = dll;
    inv.sdrplay.servicePresent = present;
    inv.sdrplay.serviceRunning = running;
}

void addUsrp(Inventory& inv, const char* service) {
    inv.usb.push_back(
        node(0x2500, 0x0020, "USB\\VID_2500&PID_0020\\B", -1, service, "Ettus Research"));
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// EVERY FINDING, whatever its kind, says what to install and where to read
// more: an empty line on the page is a person with nothing to do.
void checkComplete(const Findings& f) {
    CHECK(!f.items.empty());
    for (const rs::Finding& x : f.items) {
        CHECK(!x.title.empty());
        CHECK(!x.explanation.empty());
        CHECK(!x.install.empty());
        CHECK(!x.url.empty());
        CHECK(x.url.rfind("https://", 0) == 0);
    }
}

// --- the machines the report is about --------------------------------------------

void testEmptyBus() {
    Inventory inv = machine();
    const Findings f = rs::assess(inv);
    checkComplete(f);
    CHECK(f.items.size() == 1);
    CHECK(f.items[0].kind == Kind::NoDevice);
    CHECK(f.showPage);
    CHECK(!f.anyUsable);
    // It says what FoxSDR supports, so "plug in a radio" is actionable.
    CHECK(contains(f.items[0].explanation, "RTL-SDR"));
    CHECK(contains(f.items[0].explanation, "HackRF"));
    CHECK(contains(f.items[0].install, "Plug in"));
}

void testRtlOnTheTelevisionDriverNamesZadig() {
    Inventory inv = machine();
    addRtl(inv, "USB\\VID_0BDA&PID_2838\\1", "RTL2832UUSB", "Realtek");
    const Findings f = rs::assess(inv);
    checkComplete(f);
    CHECK(f.items.size() == 1);  // three devnodes, one radio
    const rs::Finding& x = f.items[0];
    CHECK(x.kind == Kind::WrongDriver);
    CHECK(x.family == rs::kFamilyRtl);
    // THE DRIVER WINDOWS REPORTS, by name, in the log's words and on the page.
    CHECK(x.installed == "RTL2832UUSB (Realtek)");
    CHECK(contains(x.explanation, "RTL2832UUSB"));
    CHECK(contains(x.install, "Zadig"));
    CHECK(contains(x.install, "Bulk-In, Interface (Interface 0)"));
    CHECK(contains(x.install, "WinUSB"));
    CHECK(x.url == "https://zadig.akeo.ie/");
    CHECK(f.showPage);
}

void testRtlWithNoDriverAtAll() {
    Inventory inv = machine();
    addRtl(inv, "USB\\VID_0BDA&PID_2838\\1", "", "", 28);
    const Findings f = rs::assess(inv);
    checkComplete(f);
    CHECK(f.items.size() == 1);
    CHECK(f.items[0].kind == Kind::WrongDriver);
    CHECK(f.items[0].installed == "no driver (problem code 28)");
    CHECK(contains(f.items[0].explanation, "no working driver"));
    CHECK(contains(f.items[0].install, "Zadig"));
    CHECK(f.showPage);
}

// THE BENCH'S OWN DONGLE: WinUSB on interface 0 (libwdi), problem code 28 on
// interface 1. Reading the second interface as "wrong driver" would have told
// a working owner to run Zadig.
void testRtlOnWinUsbWithTheIrInterfaceInTrouble() {
    Inventory inv = machine();
    addRtl(inv, "USB\\VID_0BDA&PID_2838\\1", "WinUSB", "libwdi");
    listedAs(inv, rs::kFamilyRtl);
    const Findings f = rs::assess(inv);
    checkComplete(f);
    CHECK(f.items.size() == 1);
    CHECK(f.items[0].kind == Kind::Usable);
    CHECK(f.anyUsable);
    CHECK(!f.showPage);
}

void testRtlBoundCorrectlyButNotListed() {
    Inventory inv = machine();
    addRtl(inv, "USB\\VID_0BDA&PID_2838\\1", "WinUSB", "libwdi");
    // The application's own enumeration ran and listed nothing.
    const Findings f = rs::assess(inv);
    checkComplete(f);
    CHECK(f.items.size() == 1);
    CHECK(f.items[0].kind == Kind::NotVisible);
    CHECK(contains(f.items[0].explanation, "WinUSB driver bound"));
    CHECK(contains(f.items[0].explanation, "did not list it"));
    // WHAT TO TRY, since there is nothing to install.
    CHECK(contains(f.items[0].install, "plug it back in"));
    CHECK(contains(f.items[0].install, "CHECK AGAIN"));
    CHECK(f.showPage);

    // ...but a scan that never RAN proves nothing: unchecked is not unlisted.
    inv.nativeChecked = false;
    const Findings g = rs::assess(inv);
    CHECK(g.items.size() == 1);
    CHECK(g.items[0].kind == Kind::Usable);
    CHECK(!g.showPage);
}

void testRspWithNoApi() {
    Inventory inv = machine();
    addRsp(inv, /*dll*/ false, /*running*/ false, /*present*/ false);
    const Findings f = rs::assess(inv);
    checkComplete(f);
    CHECK(f.items.size() == 1);
    CHECK(f.items[0].kind == Kind::VendorMissing);
    CHECK(f.items[0].family == rs::kFamilySdrPlay);
    CHECK(contains(f.items[0].install, "SDRplay API"));
    CHECK(contains(f.items[0].url, "sdrplay.com"));
    CHECK(f.showPage);
}

void testRspWithTheApiButTheServiceStopped() {
    Inventory inv = machine();
    addRsp(inv, /*dll*/ true, /*running*/ false, /*present*/ true);
    const Findings f = rs::assess(inv);
    CHECK(f.items.size() == 1);
    CHECK(f.items[0].kind == Kind::VendorMissing);
    // The DLL alone is not enough either way round.
    Inventory two = machine();
    addRsp(two, /*dll*/ false, /*running*/ true, /*present*/ true);
    CHECK(rs::assess(two).items[0].kind == Kind::VendorMissing);
    // Both present: usable, and not judged by a listing the API makes later.
    Inventory ok = machine();
    addRsp(ok, true, true, true);
    const Findings g = rs::assess(ok);
    CHECK(g.items[0].kind == Kind::Usable);
    CHECK(!g.showPage);
}

void testUsrpWithUhdAndListedIsUsable() {
    Inventory inv = machine();
    addUsrp(inv, "WINUSB");  // Windows reports the service in capitals
    inv.uhdRuntime = true;
    inv.soapyChecked = true;
    listedAs(inv, rs::kFamilyUsrp);
    const Findings f = rs::assess(inv);
    checkComplete(f);
    CHECK(f.items.size() == 1);
    CHECK(f.items[0].kind == Kind::Usable);
    CHECK(f.anyUsable);
    CHECK(!f.showPage);
}

void testUsrpWithoutUhd() {
    Inventory inv = machine();
    addUsrp(inv, "WINUSB");
    inv.uhdRuntime = false;
    const Findings f = rs::assess(inv);
    checkComplete(f);
    CHECK(f.items.size() == 1);
    CHECK(f.items[0].kind == Kind::VendorMissing);
    CHECK(contains(f.items[0].explanation, "UHD"));
    CHECK(contains(f.items[0].url, "ettus.com"));
    CHECK(f.showPage);
}

void testUsrpBoundToTheWrongDriverNamesUhd() {
    Inventory inv = machine();
    inv.usb.push_back(node(0x2500, 0x0020, "USB\\VID_2500&PID_0020\\B", -1, "", "", 28));
    inv.uhdRuntime = true;
    const Findings f = rs::assess(inv);
    CHECK(f.items.size() == 1);
    CHECK(f.items[0].kind == Kind::WrongDriver);
    CHECK(contains(f.items[0].install, "UHD"));
    CHECK(contains(f.items[0].url, "ettus.com"));
}

void testUsrpTheScanNeverRanAndTheScanThatFoundNothing() {
    Inventory inv = machine();
    addUsrp(inv, "WinUSB");
    inv.uhdRuntime = true;
    // The Soapy scan is lazy: until it ran, a USRP is not declared unlisted.
    inv.soapyChecked = false;
    CHECK(rs::assess(inv).items[0].kind == Kind::Usable);
    // After it ran and listed nothing, the vendor module is what to install.
    inv.soapyChecked = true;
    const Findings f = rs::assess(inv);
    checkComplete(f);
    CHECK(f.items[0].kind == Kind::NotVisible);
    CHECK(contains(f.items[0].install, "PothosSDR"));
    CHECK(contains(f.items[0].url, "myriadrf.org"));
    CHECK(f.showPage);
}

void testLimeNeedsOnlyAWorkingDriver() {
    Inventory inv = machine();
    inv.usb.push_back(node(0x1d50, 0x6108, "USB\\VID_1D50&PID_6108\\C", -1, "CyUsb3", "Cypress"));
    const Findings ok = rs::assess(inv);
    CHECK(ok.items.size() == 1);
    CHECK(ok.items[0].family == rs::kFamilyLime);
    CHECK(ok.items[0].kind == Kind::Usable);
    Inventory bad = machine();
    bad.usb.push_back(node(0x1d50, 0x6108, "USB\\VID_1D50&PID_6108\\C", -1, "", "", 28));
    const Findings f = rs::assess(bad);
    checkComplete(f);
    CHECK(f.items[0].kind == Kind::WrongDriver);
    CHECK(contains(f.items[0].install, "Lime Suite"));
}

void testTheOtherNativeFamiliesNeedWinUsb() {
    struct Row {
        std::uint16_t vid, pid;
        const char* family;
        const char* urlPart;
    };
    const Row rows[] = {
        {0x1d50, 0x60a1, rs::kFamilyAirspy, "airspy.com"},
        {0x03eb, 0x800c, rs::kFamilyAirspyHf, "airspy.com"},
        {0x1d50, 0x6089, rs::kFamilyHackRf, "zadig.akeo.ie"},
    };
    for (const Row& r : rows) {
        Inventory inv = machine();
        inv.usb.push_back(node(r.vid, r.pid, "USB\\X\\1", -1, "usbccgp", "Microsoft"));
        const Findings f = rs::assess(inv);
        checkComplete(f);
        CHECK(f.items.size() == 1);
        CHECK(f.items[0].kind == Kind::WrongDriver);
        CHECK(f.items[0].family == r.family);
        CHECK(contains(f.items[0].url, r.urlPart));
        CHECK(contains(f.items[0].install, "WinUSB"));
        // usbccgp is Windows' own composite driver, not a driver FOR the radio:
        // the page must not call it "the usbccgp driver".
        CHECK(!contains(f.items[0].explanation, "usbccgp"));

        Inventory ok = machine();
        ok.usb.push_back(node(r.vid, r.pid, "USB\\X\\1", -1, "WinUSB", "libwdi"));
        listedAs(ok, r.family);
        CHECK(rs::assess(ok).items[0].kind == Kind::Usable);
    }
}

// TWO RADIOS, one of them fine: no page, the problem still listed - a user with
// a working dongle and a second one on the wrong driver is told about the second
// in the page they open themselves, never interrupted for it.
void testTwoRadiosOneFine() {
    Inventory inv = machine();
    addRtl(inv, "USB\\VID_0BDA&PID_2838\\1", "RTL2832UUSB", "Realtek");
    addUsrp(inv, "WINUSB");
    inv.uhdRuntime = true;
    inv.soapyChecked = true;
    listedAs(inv, rs::kFamilyUsrp);
    const Findings f = rs::assess(inv);
    checkComplete(f);
    CHECK(f.items.size() == 2);
    CHECK(f.items[0].kind == Kind::WrongDriver);
    CHECK(f.items[1].kind == Kind::Usable);
    CHECK(f.anyUsable);
    CHECK(!f.showPage);

    // Two dongles of one model are two radios: one bound, one not.
    Inventory two = machine();
    addRtl(two, "USB\\VID_0BDA&PID_2838\\1", "WinUSB", "libwdi");
    addRtl(two, "USB\\VID_0BDA&PID_2838\\2", "RTL2832UUSB", "Realtek");
    listedAs(two, rs::kFamilyRtl);
    const Findings g = rs::assess(two);
    CHECK(g.items.size() == 2);
    CHECK(g.items[0].kind == Kind::Usable);
    CHECK(g.items[1].kind == Kind::WrongDriver);
    CHECK(!g.showPage);
}

// A radio the application listed that is on no USB bus this check knows (a
// network radio, a family not in the table) is still a radio: no page.
void testAnEnumeratedRadioOfNoKnownFamilySuppressesThePage() {
    Inventory inv = machine();
    inv.visibleCount = 1;  // no family named
    const Findings f = rs::assess(inv);
    CHECK(f.items.size() == 1);
    CHECK(f.items[0].kind == Kind::NoDevice);
    CHECK(!f.showPage);
}

void testNothingProbedRaisesNothing() {
    Inventory off;  // probed == false: not Windows, or the probe could not run
    off.note = "not probed on this platform";
    const Findings f = rs::assess(off);
    checkComplete(f);
    CHECK(f.items.size() == 1);
    CHECK(f.items[0].kind == Kind::NotChecked);
    CHECK(!f.showPage);
    CHECK(f.items[0].installed == "not probed on this platform");

    Inventory unreadable = machine();
    unreadable.usbListed = false;  // SetupAPI refused: empty means UNKNOWN
    const Findings g = rs::assess(unreadable);
    checkComplete(g);
    CHECK(g.items.size() == 1);
    CHECK(g.items[0].kind == Kind::NotChecked);
    CHECK(!g.showPage);
    CHECK(g.items[0].explanation != f.items[0].explanation);
}

// Devnodes of a radio this table does not know are none of its business.
void testUnknownDevicesAreIgnored() {
    Inventory inv = machine();
    inv.usb.push_back(node(0x046d, 0xc52b, "USB\\VID_046D&PID_C52B\\1", -1, "usbhub3", "Microsoft"));
    const Findings f = rs::assess(inv);
    CHECK(f.items.size() == 1);
    CHECK(f.items[0].kind == Kind::NoDevice);
}

// --- the tables -----------------------------------------------------------------------

void testTheIdTable() {
    struct Want {
        std::uint16_t vid, pid;
        const char* family;
    };
    // The ids the task named, each read from the vendor's own file (the table in
    // radio_setup.cpp says where).
    const Want want[] = {
        {0x0bda, 0x2838, rs::kFamilyRtl},     {0x0bda, 0x2832, rs::kFamilyRtl},
        {0x1d50, 0x60a1, rs::kFamilyAirspy},  {0x03eb, 0x800c, rs::kFamilyAirspyHf},
        {0x1d50, 0x6089, rs::kFamilyHackRf},  {0x1df7, 0x2500, rs::kFamilySdrPlay},
        {0x1df7, 0x3000, rs::kFamilySdrPlay}, {0x1df7, 0x3010, rs::kFamilySdrPlay},
        {0x1df7, 0x3020, rs::kFamilySdrPlay}, {0x1df7, 0x3030, rs::kFamilySdrPlay},
        {0x1df7, 0x3050, rs::kFamilySdrPlay}, {0x1df7, 0x3060, rs::kFamilySdrPlay},
        {0x2500, 0x0020, rs::kFamilyUsrp},    {0x2500, 0x0021, rs::kFamilyUsrp},
        {0x2500, 0x0022, rs::kFamilyUsrp},    {0x1d50, 0x6108, rs::kFamilyLime},
        {0x0403, 0x601f, rs::kFamilyLime},
    };
    for (const Want& w : want) {
        const rs::KnownDevice* d = rs::findKnownDevice(w.vid, w.pid);
        CHECK(d != nullptr);
        if (d != nullptr) { CHECK(std::string(d->family) == w.family); }
    }
    CHECK(rs::findKnownDevice(0x046d, 0xc52b) == nullptr);
    // 04b4:8613 and 04b4:00f1 are Cypress FX3's own ids, half of USB 3 shares
    // them: never claimed.
    CHECK(rs::findKnownDevice(0x04b4, 0x8613) == nullptr);
    CHECK(rs::findKnownDevice(0x04b4, 0x00f1) == nullptr);
    // No id twice (a second row would never be reached, and says one of the two
    // is wrong).
    std::set<std::pair<int, int>> seen;
    for (const rs::KnownDevice& d : rs::knownDevices()) {
        CHECK(seen.insert({d.vid, d.pid}).second);
        CHECK(d.name[0] != '\0');
    }
}

void testFamilyForDriver() {
    CHECK(rs::familyForDriver("rtlsdr") == rs::kFamilyRtl);
    CHECK(rs::familyForDriver("RTLSDR") == rs::kFamilyRtl);
    CHECK(rs::familyForDriver("hackrf") == rs::kFamilyHackRf);
    CHECK(rs::familyForDriver("airspy") == rs::kFamilyAirspy);
    CHECK(rs::familyForDriver("airspyhf") == rs::kFamilyAirspyHf);
    CHECK(rs::familyForDriver("sdrplay") == rs::kFamilySdrPlay);
    CHECK(rs::familyForDriver("miri") == rs::kFamilySdrPlay);
    CHECK(rs::familyForDriver("mirisdr") == rs::kFamilySdrPlay);
    CHECK(rs::familyForDriver("uhd") == rs::kFamilyUsrp);
    CHECK(rs::familyForDriver("lime") == rs::kFamilyLime);
    CHECK(rs::familyForDriver("pluto").empty());
    CHECK(rs::familyForDriver("").empty());
}

void testTheKindNames() {
    CHECK(std::string(rs::kindName(Kind::Usable)) == "usable");
    CHECK(std::string(rs::kindName(Kind::WrongDriver)) == "wrong-driver");
    CHECK(std::string(rs::kindName(Kind::VendorMissing)) == "vendor-missing");
    CHECK(std::string(rs::kindName(Kind::NotVisible)) == "not-visible");
    CHECK(std::string(rs::kindName(Kind::NoDevice)) == "no-device");
    CHECK(std::string(rs::kindName(Kind::NotChecked)) == "not-checked");
}

// --- the log line ------------------------------------------------------------------------

void testTheSummaryLinesNameTheDriverAndLeakNoIdentity() {
    Inventory inv = machine();
    addRtl(inv, "USB\\VID_0BDA&PID_2838\\SERIAL-1234", "RTL2832UUSB", "Realtek");
    const Findings f = rs::assess(inv);
    const std::vector<std::string> lines = rs::summaryLines(inv, f);
    std::string all;
    bool hasFinding = false;
    bool hasPage = false;
    for (const std::string& l : lines) {
        all += l + "\n";
        // Every line is a log line of its own: the prefix, and short enough that
        // the log (191 bytes a line, cut where it stands) keeps all of it. The
        // first version put everything on one line and lost the findings off
        // the end of it.
        CHECK(l.rfind("radio setup: ", 0) == 0);
        CHECK(l.size() < 150);
        if (contains(l, "wrong-driver [RTL2832UUSB (Realtek)]")) { hasFinding = true; }
        if (l == "radio setup: page wanted: yes") { hasPage = true; }
    }
    CHECK(hasFinding);
    CHECK(hasPage);
    CHECK(contains(all, "3 devnode(s) of known radios on the bus"));
    // No instance id, no serial, no path.
    CHECK(!contains(all, "SERIAL-1234"));
    CHECK(!contains(all, "USB\\"));

    // The worst case the bench produces - a long service state, several radios
    // listed - still fits.
    Inventory busy = machine();
    addRtl(busy, "USB\\VID_0BDA&PID_2838\\1", "WinUSB", "libwdi");
    busy.sdrplay.serviceState = "running, auto start (SDRplayAPIService)";
    busy.visible = {rs::kFamilyRtl, rs::kFamilyAirspy, rs::kFamilyHackRf, rs::kFamilyUsrp};
    busy.visibleCount = 4;
    for (const std::string& l : rs::summaryLines(busy, rs::assess(busy))) { CHECK(l.size() < 150); }

    Inventory off;
    off.note = "not probed on this platform";
    const std::vector<std::string> offLines = rs::summaryLines(off, rs::assess(off));
    CHECK(offLines.size() == 1);
    CHECK(contains(offLines[0], "not probed (not probed on this platform)"));

    Inventory unreadable = machine();
    unreadable.usbListed = false;
    const std::vector<std::string> badLines = rs::summaryLines(unreadable, rs::assess(unreadable));
    CHECK(badLines.size() == 1);
    CHECK(contains(badLines[0], "the USB bus could not be listed"));
}

// --- the synthetic machines the bounded run uses ---------------------------------------------

void testTheFakeMachines() {
    struct Row {
        const char* name;
        Kind kind;
        bool page;
    };
    const Row rows[] = {
        {"none", Kind::NoDevice, true},
        {"unreadable", Kind::NotChecked, false},
        {"rtl-dvbt", Kind::WrongDriver, true},
        {"rtl-nodriver", Kind::WrongDriver, true},
        {"rtl-winusb", Kind::NotVisible, true},
        {"rtl-winusb-listed", Kind::Usable, false},
        {"rsp-noapi", Kind::VendorMissing, true},
        {"rsp-ok", Kind::Usable, false},
        {"usrp-nouhd", Kind::VendorMissing, true},
        {"usrp-ok", Kind::Usable, false},
        {"usrp-unlisted", Kind::NotVisible, true},
    };
    for (const Row& r : rows) {
        Inventory inv;
        CHECK(rs::fakeInventory(r.name, inv));
        const Findings f = rs::assess(inv);
        checkComplete(f);
        CHECK(f.items.size() == 1);
        if (f.items.empty()) { continue; }
        if (f.items[0].kind != r.kind) {
            std::printf("  fake machine %s gave %s\n", r.name, rs::kindName(f.items[0].kind));
        }
        CHECK(f.items[0].kind == r.kind);
        CHECK(f.showPage == r.page);
        CHECK(inv.note == std::string("FOXSDR_FAKE_RADIO_SETUP=") + r.name);
    }
    // Any other text is refused and leaves the inventory alone.
    Inventory keep = machine();
    keep.note = "untouched";
    CHECK(!rs::fakeInventory("rtl-dvbt ", keep));
    CHECK(!rs::fakeInventory("", keep));
    CHECK(!rs::fakeInventory("RTL-DVBT", keep));
    CHECK(keep.note == "untouched");
}

// --- the probe thread --------------------------------------------------------------------------

using Clock = std::chrono::steady_clock;

// Polls until the probe leaves Running or `limit` passes; returns whether it left.
bool pollUntilSettled(rs::Probe& p, std::chrono::milliseconds limit) {
    const auto until = Clock::now() + limit;
    while (Clock::now() < until) {
        p.poll();
        if (!p.running()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void testTheProbeDeliversItsResult() {
    rs::Probe p(std::chrono::milliseconds(5000));
    CHECK(p.state() == rs::Probe::State::Idle);
    CHECK(!p.poll());
    CHECK(p.start([]() {
        Inventory inv = machine();
        inv.note = "from the worker";
        return inv;
    }));
    CHECK(p.running());
    CHECK(pollUntilSettled(p, std::chrono::milliseconds(3000)));
    CHECK(p.state() == rs::Probe::State::Done);
    CHECK(p.inventory().note == "from the worker");
    CHECK(p.inventory().probed);
    CHECK(p.finished() == 1);

    // On demand, again: a fresh run replaces the answer.
    CHECK(p.start([]() {
        Inventory inv = machine();
        inv.note = "second";
        return inv;
    }));
    CHECK(pollUntilSettled(p, std::chrono::milliseconds(3000)));
    CHECK(p.inventory().note == "second");
    CHECK(p.finished() == 2);
}

// A PROBE THAT NEVER RETURNS costs a state, not the caller: poll() answers at
// once, the budget turns it into TimedOut, and a new probe may start. The
// first worker is still parked in the collector, as one inside SetupAPI would
// be, and is released at the end.
void testAProbeThatNeverReturns() {
    std::mutex m;
    std::condition_variable cv;
    bool release = false;
    rs::Probe p(std::chrono::milliseconds(60));
    CHECK(p.start([&]() {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return release; });
        return machine();
    }));
    // Starting a second while one is in flight is refused.
    CHECK(!p.start([]() { return machine(); }));
    // poll() itself never blocks: a call costs microseconds however long the
    // collector sits there.
    const auto t0 = Clock::now();
    p.poll();
    const auto firstPoll = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0);
    CHECK(firstPoll.count() < 50);
    CHECK(pollUntilSettled(p, std::chrono::milliseconds(3000)));
    CHECK(p.state() == rs::Probe::State::TimedOut);
    // Probed but unreadable: assess() calls it "could not read the USB devices"
    // and never raises the page for it.
    CHECK(p.inventory().probed);
    CHECK(!p.inventory().usbListed);
    {
        const Findings g = rs::assess(p.inventory());
        CHECK(g.items.size() == 1);
        CHECK(g.items[0].kind == Kind::NotChecked);
        CHECK(!g.showPage);
        CHECK(g.items[0].explanation.find("USB") != std::string::npos);
    }
    CHECK(p.inventory().note.find("did not finish") != std::string::npos);
    CHECK(p.finished() == 1);
    // The page may ask again.
    CHECK(p.start([]() {
        Inventory inv = machine();
        inv.note = "fresh";
        return inv;
    }));
    CHECK(pollUntilSettled(p, std::chrono::milliseconds(3000)));
    CHECK(p.state() == rs::Probe::State::Done);
    CHECK(p.inventory().note == "fresh");
    {
        std::lock_guard<std::mutex> lk(m);
        release = true;
    }
    cv.notify_all();
    // Let the abandoned worker finish writing into the state it kept alive; the
    // test would crash here if the probe had handed it a dangling reference.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    p.poll();
    CHECK(p.state() == rs::Probe::State::Done);
}

void testAProbeThatThrows() {
    rs::Probe p(std::chrono::milliseconds(5000));
    CHECK(p.start([]() -> Inventory { throw std::runtime_error("setupapi exploded"); }));
    CHECK(pollUntilSettled(p, std::chrono::milliseconds(3000)));
    CHECK(p.state() == rs::Probe::State::Done);
    CHECK(p.inventory().probed);
    CHECK(!p.inventory().usbListed);
    CHECK(p.inventory().note.find("setupapi exploded") != std::string::npos);
    CHECK(rs::assess(p.inventory()).items[0].kind == Kind::NotChecked);
    CHECK(!p.start(nullptr));  // nothing to run
}

// --- the machine this runs on ---------------------------------------------------------------------

// REPORTED, NOT ASSERTED beyond "it ran and answered coherently": what a bench
// holds cannot be known here. The line is printed so a run on a desk with radios
// shows what the probe saw.
void testTheRealProbe() {
    const Inventory inv = rs::collectInventory();
#if defined(_WIN32)
    CHECK(inv.probed);
    CHECK(inv.usbListed);
    CHECK(inv.nativeChecked);
    for (const UsbNode& n : inv.usb) { CHECK(rs::findKnownDevice(n.vid, n.pid) != nullptr); }
#else
    CHECK(!inv.probed);
    CHECK(inv.note == "not probed on this platform");
#endif
    const Findings f = rs::assess(inv);
    checkComplete(f);
    for (const std::string& l : rs::summaryLines(inv, f)) {
        std::printf("  real probe: %s\n", l.c_str());
    }
}

}  // namespace

int main() {
    testEmptyBus();
    testRtlOnTheTelevisionDriverNamesZadig();
    testRtlWithNoDriverAtAll();
    testRtlOnWinUsbWithTheIrInterfaceInTrouble();
    testRtlBoundCorrectlyButNotListed();
    testRspWithNoApi();
    testRspWithTheApiButTheServiceStopped();
    testUsrpWithUhdAndListedIsUsable();
    testUsrpWithoutUhd();
    testUsrpBoundToTheWrongDriverNamesUhd();
    testUsrpTheScanNeverRanAndTheScanThatFoundNothing();
    testLimeNeedsOnlyAWorkingDriver();
    testTheOtherNativeFamiliesNeedWinUsb();
    testTwoRadiosOneFine();
    testAnEnumeratedRadioOfNoKnownFamilySuppressesThePage();
    testNothingProbedRaisesNothing();
    testUnknownDevicesAreIgnored();
    testTheIdTable();
    testFamilyForDriver();
    testTheKindNames();
    testTheSummaryLinesNameTheDriverAndLeakNoIdentity();
    testTheFakeMachines();
    testTheProbeDeliversItsResult();
    testAProbeThatNeverReturns();
    testAProbeThatThrows();
    testTheRealProbe();
    return testSummary("test_radio_setup");
}
