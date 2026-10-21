// radio_setup.cpp - see radio_setup.hpp for what this is and why.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/radio_setup.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <exception>
#include <thread>
#include <utility>

#include "core/i18n.hpp"
#include "core/telemetry.hpp"
#include "core/utf8_text.hpp"
#include "source/rtlsdr_source.hpp"

#if defined(_WIN32)
// clang-format off
#include <windows.h>
// initguid.h must precede the headers that DECLARE the property keys, or they
// are only extern references and the link fails (usb/winusb_device.cpp).
#include <initguid.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpkey.h>
// clang-format on

#include "source/airspy_source.hpp"
#include "source/airspyhf_source.hpp"
#include "source/hackrf_source.hpp"
#include "source/sdrplay_service.hpp"
#include "source/soapy_modules.hpp"
#include "usb/usb_device.hpp"
#endif

namespace cascade::core::radiosetup {

using cascade::i18n::tr;

namespace {

// --- the places a finding sends a person ------------------------------------
//
// Official vendor pages only, each answered a HEAD request with HTTP 200 on
// 2026-10-06 (the list and the result are in the release notes' build log).
//
// SDRplay: www.sdrplay.com/downloads now redirects to the SDRconnect
// application's page, which is not where the API is; www.sdrplay.com/api
// redirects to sdrplay.com/hardware-api ("Hardware API"), the page that carries
// the API installer, and that is the one used.
constexpr const char* kZadigUrl = "https://zadig.akeo.ie/";
constexpr const char* kSdrPlayUrl = "https://www.sdrplay.com/api/";
constexpr const char* kUhdUrl = "https://files.ettus.com/binaries/uhd/";
constexpr const char* kAirspyUrl = "https://airspy.com/download/";
constexpr const char* kLimeUrl = "https://wiki.myriadrf.org/Lime_Suite";
constexpr const char* kPothosUrl = "https://downloads.myriadrf.org/builds/PothosSDR/";

bool iequals(const std::string& a, const char* b) {
    std::size_t i = 0;
    for (; i < a.size() && b[i] != '\0'; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return i == a.size() && b[i] == '\0';
}

std::string lowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    return s;
}

}  // namespace

// --- families ----------------------------------------------------------------

std::string familyForDriver(const std::string& driverKey) {
    const std::string k = lowerAscii(driverKey);
    if (k == "rtlsdr") { return kFamilyRtl; }
    if (k == "airspy") { return kFamilyAirspy; }
    if (k == "airspyhf") { return kFamilyAirspyHf; }
    if (k == "hackrf") { return kFamilyHackRf; }
    // The Mirics-family modules and the SDRplay API are one thing to a person:
    // an RSP, reached through the API, whichever row lists it
    // (gui/device_scan_plan.hpp, soapyModulesForFamily).
    if (k == "sdrplay" || k == "miri" || k == "mirisdr") { return kFamilySdrPlay; }
    if (k == "uhd") { return kFamilyUsrp; }
    if (k == "lime" || k == "lms7") { return kFamilyLime; }
    return std::string();
}

// --- the known radios ----------------------------------------------------------
//
// WHERE EVERY ID WAS READ (2026-10-06, from the vendors' own files fetched that
// day, not from memory):
//
//   RTL-SDR     0bda:2832 and 0bda:2838  osmocom/rtl-sdr rtl-sdr.rules, lines 19
//               and 22; the rest of the RTL2832U family is the list the native
//               driver already opens (rtlSdrUsbIds, from librtlsdr's table).
//   Airspy      1d50:60a1                airspy/airspyone_host 52-airspy.rules
//   Airspy HF+  03eb:800c                airspy/airspyhf 52-airspyhf.rules
//   HackRF      1d50:6089 (One), 604b (Jawbreaker), cc15 (rad1o)
//                                        greatscottgadgets/hackrf 53-hackrf.rules
//   SDRplay RSP 1df7:2500 RSP1, 3000 RSP1A, 3010 RSP2, 3020 RSPduo, 3030 RSPdx,
//               3050 RSP1B, 3060 RSPdx-R2
//                                        SDRplay's own udev rules and usb.ids
//                                        entries (install_lib.sh in
//                                        srcejon/sdrplayapi, lines 151-157 and
//                                        233). 1df7:2500 is also the Mirics
//                                        reference design (source/msi2500.cpp),
//                                        so a television stick on that id is
//                                        named an RSP1 here; the instruction it
//                                        gets (the SDRplay API) is still the
//                                        only one that applies to the id.
//   USRP        2500:0020 B200/B210, 0021 B200mini, 0022 B205mini, 0023 B206mini,
//               3923:7813 B200 and 3923:7814 B210 (National Instruments' id)
//                                        EttusResearch/uhd host/utils/
//                                        uhd-usrp.rules. The FX3 bootloader ids
//                                        are left out: UHD itself never searches
//                                        them.
//   LimeSDR     1d50:6108 (FX3) and 0403:601f (FT601)
//                                        myriadrf/LimeSuite udev-rules/
//                                        64-limesuite.rules. The same file's
//                                        04b4:8613 and 04b4:00f1 are the bare
//                                        Cypress FX3 ids half the USB 3 world
//                                        shares, so they are not claimed.
const std::vector<KnownDevice>& knownDevices() {
    static const std::vector<KnownDevice> table = []() {
        std::vector<KnownDevice> t = {
            {0x1d50, 0x60a1, kFamilyAirspy, "Airspy"},
            {0x03eb, 0x800c, kFamilyAirspyHf, "Airspy HF+"},
            {0x1d50, 0x6089, kFamilyHackRf, "HackRF One"},
            {0x1d50, 0x604b, kFamilyHackRf, "HackRF Jawbreaker"},
            {0x1d50, 0xcc15, kFamilyHackRf, "rad1o (HackRF firmware)"},
            {0x1df7, 0x2500, kFamilySdrPlay, "SDRplay RSP1"},
            {0x1df7, 0x3000, kFamilySdrPlay, "SDRplay RSP1A"},
            {0x1df7, 0x3010, kFamilySdrPlay, "SDRplay RSP2"},
            {0x1df7, 0x3020, kFamilySdrPlay, "SDRplay RSPduo"},
            {0x1df7, 0x3030, kFamilySdrPlay, "SDRplay RSPdx"},
            {0x1df7, 0x3050, kFamilySdrPlay, "SDRplay RSP1B"},
            {0x1df7, 0x3060, kFamilySdrPlay, "SDRplay RSPdx-R2"},
            {0x2500, 0x0020, kFamilyUsrp, "USRP B200/B210"},
            {0x2500, 0x0021, kFamilyUsrp, "USRP B200mini"},
            {0x2500, 0x0022, kFamilyUsrp, "USRP B205mini"},
            {0x2500, 0x0023, kFamilyUsrp, "USRP B206mini"},
            {0x3923, 0x7813, kFamilyUsrp, "USRP B200"},
            {0x3923, 0x7814, kFamilyUsrp, "USRP B210"},
            {0x1d50, 0x6108, kFamilyLime, "LimeSDR-USB"},
            {0x0403, 0x601f, kFamilyLime, "LimeSDR-USB"},
        };
        // The RTL2832U family: every id the native driver can open, which is
        // every id Windows can have bound a driver to.
        for (const cascade::usb::UsbId& id : cascade::source::rtlSdrUsbIds()) {
            t.push_back({id.vid, id.pid, kFamilyRtl, "RTL-SDR (RTL2832U)"});
        }
        return t;
    }();
    return table;
}

const KnownDevice* findKnownDevice(std::uint16_t vid, std::uint16_t pid) {
    for (const KnownDevice& d : knownDevices()) {
        if (d.vid == vid && d.pid == pid) { return &d; }
    }
    return nullptr;
}

const char* kindName(Kind k) {
    switch (k) {
    case Kind::Usable: return "usable";
    case Kind::WrongDriver: return "wrong-driver";
    case Kind::VendorMissing: return "vendor-missing";
    case Kind::NotVisible: return "not-visible";
    case Kind::NoDevice: return "no-device";
    case Kind::NotChecked: return "not-checked";
    }
    return "unknown";
}

// --- assess ------------------------------------------------------------------------

namespace {

bool listed(const Inventory& inv, const std::string& family) {
    return std::find(inv.visible.begin(), inv.visible.end(), family) != inv.visible.end();
}

// WHAT IS BOUND TO THIS RADIO, in the words of the log: the service Windows
// reports, with the INF's provider beside it, or "no driver". A composite
// dongle's interface 0 is where the SDR half lives and where Zadig binds, so it
// is read first; the parent (usbccgp, Windows' own composite driver) and the
// remote-control interface are only the answer when nothing better is.
struct Bound {
    bool hasDriver = false;     // a service other than usbccgp is bound somewhere
    bool winusb = false;        // WinUSB is bound to some devnode of the radio
    bool anyWorking = false;    // some devnode has a service and no problem code
    std::string service;        // the service named, "" when none
    std::string installed;      // for the log and the finding
};

Bound boundOf(const std::vector<const UsbNode*>& nodes) {
    Bound b;
    std::vector<const UsbNode*> order = nodes;
    // Interface 0 first, then the rest in the order found; the device node
    // (interface -1) sorts after any interface child of the same radio.
    std::stable_sort(order.begin(), order.end(), [](const UsbNode* x, const UsbNode* y) {
        const auto rank = [](const UsbNode* n) { return n->interfaceNumber == 0 ? 0 : (n->interfaceNumber > 0 ? 1 : 2); };
        return rank(x) < rank(y);
    });
    for (const UsbNode* n : order) {
        if (iequals(n->service, "WinUSB")) { b.winusb = true; }
        if (!n->service.empty() && n->problemCode == 0) { b.anyWorking = true; }
    }
    for (const UsbNode* n : order) {
        if (n->service.empty() || iequals(n->service, "usbccgp")) { continue; }
        b.hasDriver = true;
        b.service = n->service;
        b.installed = n->service;
        if (!n->provider.empty()) { b.installed += " (" + n->provider + ")"; }
        return b;
    }
    unsigned code = 0;
    for (const UsbNode* n : order) {
        if (n->problemCode != 0) {
            code = n->problemCode;
            if (code == 28) { break; }
        }
    }
    b.installed = "no driver";
    if (code != 0) { b.installed += " (problem code " + std::to_string(code) + ")"; }
    return b;
}

Finding makeFinding(Kind kind, const KnownDevice& d, const std::string& installed,
                    const std::string& title, const std::string& explanation,
                    const std::string& install, const char* url) {
    Finding f;
    f.kind = kind;
    f.family = d.family;
    f.deviceName = d.name;
    f.vid = d.vid;
    f.pid = d.pid;
    f.installed = installed;
    f.title = title;
    f.explanation = explanation;
    f.install = install;
    f.url = url;
    return f;
}

Finding wrongDriver(const KnownDevice& d, const Bound& b) {
    const std::string family = d.family;
    const std::string title = formatText(tr("%s: wrong driver"), d.name);
    const std::string why =
        b.hasDriver ? formatText(tr("%s is plugged in, but Windows has given it the %s driver, so "
                                    "SDR software cannot open it."),
                                 d.name, b.service.c_str())
                    : formatText(tr("%s is plugged in, but Windows has no working driver for it, "
                                    "so SDR software cannot open it."),
                                 d.name);
    std::string install;
    const char* url = kZadigUrl;
    if (family == kFamilyRtl) {
        install = tr("Install the WinUSB driver with Zadig: tick Options -> List All Devices, "
                     "select \"Bulk-In, Interface (Interface 0)\", choose WinUSB and click "
                     "Replace Driver, then press CHECK AGAIN.");
    } else if (family == kFamilyHackRf) {
        install = formatText(tr("Install the WinUSB driver with Zadig: tick Options -> List All "
                                "Devices, select %s, choose WinUSB and click Replace Driver, then "
                                "press CHECK AGAIN."),
                             d.name);
    } else if (family == kFamilyAirspy || family == kFamilyAirspyHf) {
        install = formatText(tr("Install the WinUSB driver for %s: run Airspy's WinUSB "
                                "Compatibility Driver from airspy.com, or use Zadig "
                                "(zadig.akeo.ie), then press CHECK AGAIN."),
                             d.name);
        url = kAirspyUrl;
    } else if (family == kFamilySdrPlay) {
        install = tr("Install the SDRplay API 3.x from sdrplay.com (it installs the service and "
                     "the RSP's driver), then press CHECK AGAIN.");
        url = kSdrPlayUrl;
    } else if (family == kFamilyUsrp) {
        install = tr("Install UHD from Ettus Research (it installs the driver and the library), "
                     "then unplug the radio, plug it back in and press CHECK AGAIN.");
        url = kUhdUrl;
    } else {
        install = tr("Install the Lime Suite driver package, then unplug the radio, plug it back "
                     "in and press CHECK AGAIN.");
        url = kLimeUrl;
    }
    return makeFinding(Kind::WrongDriver, d, b.installed, title, why, install, url);
}

Finding judge(const KnownDevice& d, const std::vector<const UsbNode*>& nodes,
              const Inventory& inv) {
    const std::string family = d.family;
    const Bound b = boundOf(nodes);

    // The radios FoxSDR's own USB transport drives: they must be on WinUSB, and
    // then its own enumeration should list them.
    if (family == kFamilyRtl || family == kFamilyAirspy || family == kFamilyAirspyHf ||
        family == kFamilyHackRf) {
        if (!b.winusb) { return wrongDriver(d, b); }
        if (inv.nativeChecked && !listed(inv, family)) {
            return makeFinding(Kind::NotVisible, d, b.installed,
                               formatText(tr("%s: not listed by FoxSDR"), d.name),
                               formatText(tr("%s has the WinUSB driver bound, but FoxSDR's own "
                                             "scan did not list it."),
                                          d.name),
                               tr("Unplug the radio and plug it back in, then press CHECK AGAIN. "
                                  "Close any other program that uses it."),
                               kZadigUrl);
        }
    } else if (family == kFamilySdrPlay) {
        // An RSP is driven by the SDRplay API service, which holds the USB
        // handle; without sdrplay_api.dll and a running service it cannot even
        // be listed (source/sdrplay_service.hpp).
        if (!(inv.sdrplay.dllFound && inv.sdrplay.serviceRunning)) {
            return makeFinding(Kind::VendorMissing, d, inv.sdrplay.serviceState,
                               formatText(tr("%s: vendor software missing"), d.name),
                               formatText(tr("%s is plugged in, but the SDRplay API service is "
                                             "not installed or not running, and an RSP cannot be "
                                             "listed or opened without it."),
                                          d.name),
                               tr("Install the SDRplay API 3.x from sdrplay.com (it installs the "
                                  "service and the RSP's driver), then press CHECK AGAIN."),
                               kSdrPlayUrl);
        }
        if (!b.anyWorking) { return wrongDriver(d, b); }
    } else if (family == kFamilyUsrp) {
        if (!b.winusb) { return wrongDriver(d, b); }
        if (!inv.uhdRuntime) {
            return makeFinding(Kind::VendorMissing, d, b.installed,
                               formatText(tr("%s: vendor software missing"), d.name),
                               formatText(tr("%s is plugged in and has its driver, but UHD "
                                             "(Ettus Research's driver library) was not found on "
                                             "this computer, and FoxSDR cannot open a USRP "
                                             "without it."),
                                          d.name),
                               tr("Install UHD from Ettus Research (it installs the driver and "
                                  "the library), then unplug the radio, plug it back in and "
                                  "press CHECK AGAIN."),
                               kUhdUrl);
        }
    } else {
        // The LimeSDR's driver comes from its own package and its name is not
        // one this check can vouch for, so only "no working driver" is judged.
        if (!b.anyWorking) { return wrongDriver(d, b); }
    }

    // Both families that FoxSDR reaches only through a SoapySDR vendor module:
    // judged only once the application has actually run that scan.
    if ((family == kFamilyUsrp || family == kFamilyLime) && inv.soapyChecked &&
        !listed(inv, family)) {
        return makeFinding(Kind::NotVisible, d, b.installed,
                           formatText(tr("%s: not listed by FoxSDR"), d.name),
                           formatText(tr("%s has the right driver, but FoxSDR reaches it through "
                                         "a SoapySDR vendor module and its scan found none."),
                                      d.name),
                           tr("Install PothosSDR or radioconda (both carry the SoapySDR modules), "
                              "then press Refresh in the Source section."),
                           kPothosUrl);
    }

    return makeFinding(Kind::Usable, d, b.installed, formatText(tr("%s: ready"), d.name),
                       formatText(tr("Windows shows %s with the right driver and FoxSDR can use "
                                     "it."),
                                  d.name),
                       tr("Nothing to install."), cascade::core::kHomepageUrl);
}

}  // namespace

Findings assess(const Inventory& inv) {
    Findings out;

    if (!inv.probed || !inv.usbListed) {
        Finding f;
        f.kind = Kind::NotChecked;
        f.installed = inv.note;
        f.title = tr("Radio drivers were not checked");
        f.explanation =
            !inv.probed ? std::string(tr("FoxSDR does not check radio drivers on this system."))
                        : std::string(tr("FoxSDR could not read this computer's list of USB "
                                         "devices, so it cannot say which driver is missing."));
        f.install = tr("Nothing to install.");
        f.url = cascade::core::kHomepageUrl;
        out.items.push_back(std::move(f));
        return out;  // showPage stays false: nothing is known
    }

    // One radio = the devnodes that share a USB device node, in the order the
    // bus walk met them.
    std::vector<std::string> order;
    for (const UsbNode& n : inv.usb) {
        if (findKnownDevice(n.vid, n.pid) == nullptr) { continue; }
        if (std::find(order.begin(), order.end(), n.deviceId) == order.end()) {
            order.push_back(n.deviceId);
        }
    }
    for (const std::string& id : order) {
        std::vector<const UsbNode*> nodes;
        for (const UsbNode& n : inv.usb) {
            if (n.deviceId == id && findKnownDevice(n.vid, n.pid) != nullptr) { nodes.push_back(&n); }
        }
        if (nodes.empty()) { continue; }
        const KnownDevice* d = findKnownDevice(nodes.front()->vid, nodes.front()->pid);
        out.items.push_back(judge(*d, nodes, inv));
    }

    if (out.items.empty()) {
        Finding f;
        f.kind = Kind::NoDevice;
        f.title = tr("No radio found on the USB ports");
        f.explanation =
            tr("Windows lists no supported radio on any USB port. Plug one in - directly, not "
               "through a hub - and press CHECK AGAIN. FoxSDR supports RTL-SDR, Airspy, Airspy "
               "HF+, HackRF, SDRplay RSP, USRP B2xx and LimeSDR radios; the signal generator and "
               "IQ file playback need no hardware.");
        f.install = tr("Plug in a supported radio, then press CHECK AGAIN.");
        f.url = cascade::core::kHomepageUrl;
        out.items.push_back(std::move(f));
    }

    for (const Finding& f : out.items) {
        if (f.kind == Kind::Usable) { out.anyUsable = true; }
    }
    // THE PAGE OPENS BY ITSELF only for a person who has no radio the program
    // can use: nothing usable on the bus AND nothing its own enumeration
    // listed (a radio on a network, or of a family this table does not know,
    // is still a radio).
    out.showPage = !out.anyUsable && inv.visibleCount == 0 && inv.visible.empty();
    return out;
}

std::vector<std::string> summaryLines(const Inventory& inv, const Findings& f) {
    std::vector<std::string> lines;
    if (!inv.probed) {
        lines.push_back("radio setup: not probed (" +
                        (inv.note.empty() ? std::string("no reason given") : inv.note) + ")");
        return lines;
    }
    if (!inv.usbListed) {
        lines.push_back("radio setup: the USB bus could not be listed" +
                        (inv.note.empty() ? std::string() : " (" + inv.note + ")"));
        return lines;
    }
    std::string bus = "radio setup: " + std::to_string(inv.usb.size()) +
                      " devnode(s) of known radios on the bus; the application lists " +
                      std::to_string(inv.visibleCount) + " radio(s)";
    if (!inv.visible.empty()) {
        bus += " [";
        for (std::size_t i = 0; i < inv.visible.size(); ++i) {
            bus += (i == 0 ? "" : ",") + inv.visible[i];
        }
        bus += "]";
    }
    lines.push_back(bus);
    lines.push_back("radio setup: sdrplay api dll " +
                    std::string(inv.sdrplay.dllFound ? "found" : "not found") + ", service " +
                    (inv.sdrplay.serviceState.empty() ? std::string("unknown")
                                                      : inv.sdrplay.serviceState) +
                    "; uhd " + std::string(inv.uhdRuntime ? "found" : "not found"));
    for (const Finding& x : f.items) {
        std::string s = "radio setup: ";
        if (!x.family.empty()) { s += x.family + " " + x.deviceName + " "; }
        s += kindName(x.kind);
        if (!x.installed.empty()) { s += " [" + x.installed + "]"; }
        lines.push_back(std::move(s));
    }
    lines.push_back(std::string("radio setup: page wanted: ") + (f.showPage ? "yes" : "no"));
    return lines;
}

// --- the probe thread -----------------------------------------------------------------

bool Probe::start(Collector collector) {
    if (state_ == State::Running || !collector) { return false; }
    auto shared = std::make_shared<Shared>();
    shared_ = shared;
    state_ = State::Running;
    startedAt_ = std::chrono::steady_clock::now();
    // DETACHED, and owning its own reference to the state it writes: a probe
    // wedged inside SetupAPI can outlive this object, the window and the
    // budget, and must find something to write to when it finally returns.
    std::thread([shared, collector = std::move(collector)]() {
        Inventory r;
        try {
            r = collector();
        } catch (const std::exception& e) {
            r = Inventory{};
            r.probed = true;  // it RAN, and could not read the bus: not "not probed here"
            r.note = std::string("the probe threw: ") + e.what();
        } catch (...) {
            r = Inventory{};
            r.probed = true;
            r.note = "the probe threw";
        }
        std::lock_guard<std::mutex> lk(shared->m);
        shared->result = std::move(r);
        shared->done = true;
    }).detach();
    return true;
}

bool Probe::poll() {
    if (state_ != State::Running) { return false; }
    {
        std::lock_guard<std::mutex> lk(shared_->m);
        if (shared_->done) {
            result_ = std::move(shared_->result);
            state_ = State::Done;
            ++finished_;
            shared_.reset();
            return true;
        }
    }
    if (std::chrono::steady_clock::now() - startedAt_ > budget_) {
        // Stop waiting; the thread keeps its own reference and is left to end
        // (or not) on its own.
        // Probed but unreadable: assess() says "could not read the USB
        // devices", which is what happened, and never raises the page.
        result_ = Inventory{};
        result_.probed = true;
        result_.note = "the probe did not finish in time";
        state_ = State::TimedOut;
        ++finished_;
        shared_.reset();
        return true;
    }
    return false;
}

// --- the synthetic machines --------------------------------------------------------------

namespace {

UsbNode node(std::uint16_t vid, std::uint16_t pid, const char* deviceId, int interfaceNumber,
             const char* service, const char* provider, const char* cls, unsigned problem) {
    UsbNode n;
    n.vid = vid;
    n.pid = pid;
    n.deviceId = deviceId;
    n.interfaceNumber = interfaceNumber;
    n.isDeviceNode = interfaceNumber < 0;
    n.service = service;
    n.provider = provider;
    n.deviceClass = cls;
    n.problemCode = problem;
    return n;
}

// A composite RTL2832U dongle as Windows builds it: the device node under
// usbccgp, the SDR half as interface 0 and the remote-control half as
// interface 1 (which shows problem code 28 on a machine that is working
// perfectly - the bench RTL-SDR does exactly this).
void addRtl(Inventory& inv, const char* sdrService, const char* sdrProvider, unsigned sdrProblem) {
    const char* id = "USB\\VID_0BDA&PID_2838\\FAKE0001";
    inv.usb.push_back(node(0x0bda, 0x2838, id, -1, "usbccgp", "Microsoft", "USB", 0));
    inv.usb.push_back(node(0x0bda, 0x2838, id, 0, sdrService, sdrProvider,
                           sdrService[0] != '\0' ? "USBDevice" : "", sdrProblem));
    inv.usb.push_back(node(0x0bda, 0x2838, id, 1, "", "", "", 28));
}

}  // namespace

bool fakeInventory(const std::string& name, Inventory& out) {
    Inventory inv;
    inv.probed = true;
    inv.usbListed = true;
    inv.nativeChecked = true;
    inv.note = "FOXSDR_FAKE_RADIO_SETUP=" + name;
    if (name == "none") {
        // nothing on the bus
    } else if (name == "unreadable") {
        inv.usbListed = false;
    } else if (name == "rtl-dvbt") {
        addRtl(inv, "RTL2832UUSB", "Realtek", 0);
    } else if (name == "rtl-nodriver") {
        addRtl(inv, "", "", 28);
    } else if (name == "rtl-winusb") {
        addRtl(inv, "WinUSB", "libwdi", 0);
    } else if (name == "rtl-winusb-listed") {
        addRtl(inv, "WinUSB", "libwdi", 0);
        inv.visible.push_back(kFamilyRtl);
        inv.visibleCount = 1;
    } else if (name == "rsp-noapi" || name == "rsp-ok") {
        inv.usb.push_back(node(0x1df7, 0x3030, "USB\\VID_1DF7&PID_3030\\FAKE0002", -1, "WinUSB",
                               "SDRplay", "USBDevice", 0));
        if (name == "rsp-ok") {
            inv.sdrplay.dllFound = true;
            inv.sdrplay.servicePresent = true;
            inv.sdrplay.serviceRunning = true;
            inv.sdrplay.serviceState = "running, auto start";
        } else {
            inv.sdrplay.serviceState = "not installed";
        }
    } else if (name == "usrp-nouhd" || name == "usrp-ok" || name == "usrp-unlisted") {
        inv.usb.push_back(node(0x2500, 0x0020, "USB\\VID_2500&PID_0020\\FAKE0003", -1, "WinUSB",
                               "Ettus Research", "USRPs", 0));
        inv.uhdRuntime = (name != "usrp-nouhd");
        inv.soapyChecked = (name != "usrp-nouhd");
        if (name == "usrp-ok") {
            inv.visible.push_back(kFamilyUsrp);
            inv.visibleCount = 1;
        }
    } else {
        return false;
    }
    out = std::move(inv);
    return true;
}

// --- reading this machine ------------------------------------------------------------------

#if defined(_WIN32)

namespace {

std::string narrow(const wchar_t* w) {
    if (w == nullptr || *w == L'\0') { return std::string(); }
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) { return std::string(); }
    std::string out(static_cast<std::size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) { return std::wstring(); }
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) { return std::wstring(); }
    std::wstring out(static_cast<std::size_t>(n - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

// One string property, or the first string of a list; "" when absent. Property
// reads, never an open (usb/usb_device.hpp, rule 1).
std::wstring stringProperty(HDEVINFO set, SP_DEVINFO_DATA& info, const DEVPROPKEY& key) {
    DEVPROPTYPE type = 0;
    DWORD needed = 0;
    ::SetupDiGetDevicePropertyW(set, &info, &key, &type, nullptr, 0, &needed, 0);
    if (needed == 0) { return std::wstring(); }
    std::vector<BYTE> buf(needed + sizeof(wchar_t), 0);
    if (::SetupDiGetDevicePropertyW(set, &info, &key, &type, buf.data(), needed, &needed, 0) ==
        FALSE) {
        return std::wstring();
    }
    return std::wstring(reinterpret_cast<const wchar_t*>(buf.data()));
}

unsigned uintProperty(HDEVINFO set, SP_DEVINFO_DATA& info, const DEVPROPKEY& key) {
    DEVPROPTYPE type = 0;
    DWORD value = 0;
    DWORD needed = 0;
    if (::SetupDiGetDevicePropertyW(set, &info, &key, &type, reinterpret_cast<PBYTE>(&value),
                                    sizeof(value), &needed, 0) == FALSE ||
        type != DEVPROP_TYPE_UINT32) {
        return 0;
    }
    return static_cast<unsigned>(value);
}

std::wstring instanceIdOf(DEVINST inst) {
    wchar_t buf[MAX_DEVICE_ID_LEN + 1] = {0};
    if (::CM_Get_Device_IDW(inst, buf, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) {
        return std::wstring();
    }
    return std::wstring(buf);
}

// The hardware ids are a multi-string: the first one carries the VID and PID.
std::string firstHardwareId(HDEVINFO set, SP_DEVINFO_DATA& info) {
    return narrow(stringProperty(set, info, DEVPKEY_Device_HardwareIds).c_str());
}

bool fileExists(const std::wstring& path) {
    const DWORD a = ::GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string env(const char* name) {
    char buf[1024];
    const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf, n) : std::string();
}

// "C:\Program Files" + "\\" + tail, tolerating a trailing separator.
std::string joined(std::string a, const std::string& b) {
    if (!a.empty() && a.back() != '\\' && a.back() != '/') { a += '\\'; }
    return a + b;
}

bool findUhdRuntime() {
    wchar_t found[MAX_PATH];
    if (::SearchPathW(nullptr, L"uhd.dll", nullptr, MAX_PATH, found, nullptr) > 0) { return true; }
    const std::string pf = env("ProgramFiles");
    if (!pf.empty() && fileExists(widen(joined(joined(joined(pf, "UHD"), "bin"), "uhd.dll")))) {
        return true;
    }
    // The vendor installs the SoapySDR scan already knows about: PothosSDR and
    // radioconda carry uhd.dll beside their other libraries.
    const auto getenvFn = [](const char* n) { return env(n); };
    for (const auto& [name, root] : cascade::source::candidateVendorRoots(getenvFn)) {
        (void)name;
        if (fileExists(widen(joined(joined(root, "bin"), "uhd.dll")))) { return true; }
    }
    return false;
}

SdrPlayApi probeSdrPlay() {
    SdrPlayApi api;
    // The path the driver itself tries first (source/sdrplay_source.cpp), under
    // both spellings of Program Files so a 32-bit process is not blind to it.
    for (const char* var : {"ProgramFiles", "ProgramW6432"}) {
        const std::string pf = env(var);
        if (pf.empty()) { continue; }
        if (fileExists(widen(joined(joined(joined(joined(pf, "SDRplay"), "API"), "x64"),
                                    "sdrplay_api.dll")))) {
            api.dllFound = true;
        }
    }
    const cascade::source::SdrPlayServiceStatus s = cascade::source::querySdrPlayService();
    api.servicePresent = cascade::source::sdrPlayServiceFound(s);
    api.serviceRunning = (s.state == cascade::source::SdrPlayServiceState::Running);
    api.serviceState = cascade::source::sdrPlayServiceSummary(s);
    return api;
}

}  // namespace

Inventory collectInventory() {
    Inventory inv;
    inv.probed = true;

    // THE BUS WALK: every present devnode under the USB enumerator, kept only
    // when its VID/PID is a known radio's. Properties only; nothing is opened.
    const HDEVINFO set =
        ::SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) {
        inv.usbListed = false;
        inv.note = "SetupDiGetClassDevs failed";
    } else {
        inv.usbListed = true;
        SP_DEVINFO_DATA info{};
        info.cbSize = sizeof(info);
        for (DWORD i = 0; ::SetupDiEnumDeviceInfo(set, i, &info) != FALSE; ++i) {
            cascade::usb::UsbId id{0, 0};
            if (!cascade::usb::usbIdFromHardwareId(firstHardwareId(set, info), id)) { continue; }
            if (findKnownDevice(id.vid, id.pid) == nullptr) { continue; }

            UsbNode n;
            n.vid = id.vid;
            n.pid = id.pid;
            const std::wstring instance = instanceIdOf(info.DevInst);
            const std::size_t mi = instance.find(L"&MI_");
            if (mi != std::wstring::npos && mi + 6 <= instance.size()) {
                n.interfaceNumber = static_cast<int>(
                    std::wcstol(instance.substr(mi + 4, 2).c_str(), nullptr, 16));
                DEVINST parent = 0;
                n.deviceId = (::CM_Get_Parent(&parent, info.DevInst, 0) == CR_SUCCESS)
                                 ? narrow(instanceIdOf(parent).c_str())
                                 : narrow(instance.c_str());
            } else {
                n.deviceId = narrow(instance.c_str());
            }
            n.isDeviceNode = n.interfaceNumber < 0;
            n.service = narrow(stringProperty(set, info, DEVPKEY_Device_Service).c_str());
            n.provider = narrow(stringProperty(set, info, DEVPKEY_Device_DriverProvider).c_str());
            n.deviceClass = narrow(stringProperty(set, info, DEVPKEY_Device_Class).c_str());
            n.description =
                narrow(stringProperty(set, info, DEVPKEY_Device_BusReportedDeviceDesc).c_str());
            n.problemCode = uintProperty(set, info, DEVPKEY_Device_ProblemCode);
            inv.usb.push_back(std::move(n));
        }
        ::SetupDiDestroyDeviceInfoList(set);
    }

    inv.sdrplay = probeSdrPlay();
    inv.uhdRuntime = findUhdRuntime();

    // THE APPLICATION'S OWN ENUMERATION of the four radios it drives through
    // its own USB transport: the very functions the Source list calls, run here
    // on the worker so the answer does not depend on the Source list having
    // been opened. They read the same properties and open nothing.
    const auto note = [&inv](const char* family, std::size_t n) {
        if (n == 0) { return; }
        inv.visible.push_back(family);
        inv.visibleCount += static_cast<int>(n);
    };
    note(kFamilyRtl, cascade::source::enumerateRtlSdr().size());
    note(kFamilyHackRf, cascade::source::enumerateHackRf().size());
    note(kFamilyAirspy, cascade::source::enumerateAirspy().size());
    note(kFamilyAirspyHf, cascade::source::enumerateAirspyHf().size());
    inv.nativeChecked = true;
    return inv;
}

#else  // !_WIN32

Inventory collectInventory() {
    // NOT PROBED, and said so. The Linux failure that matters here - a device
    // node the user may not read, which the udev rule in installer/linux/
    // grants - is a permissions question usbfs answers at open time, and a
    // rules-file check would be a guess about where a distribution put the
    // file. The page is never raised on this evidence.
    Inventory inv;
    inv.probed = false;
    inv.note = "not probed on this platform";
    return inv;
}

#endif  // _WIN32

}  // namespace cascade::core::radiosetup
