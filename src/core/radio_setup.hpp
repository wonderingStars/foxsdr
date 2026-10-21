// radio_setup.hpp - the FIRST-RUN RADIO SETUP check: what is plugged in, what is
// installed for it, and the one thing to install when the two do not add up.
//
// WHY THIS EXISTS (2026-10-06). The usage report says 578 of 813 installs this
// month reported NO radio at all. Nobody can tell from a count how many of them
// own a radio, but the ways to own one and not see it are few and each has one
// fix: an RTL-SDR still wearing the DVB-T television driver Windows gives it
// (invisible to every SDR program until Zadig binds WinUSB to it), an SDRplay
// RSP with no SDRplay API service behind it (an RSP cannot even be LISTED
// without that service), a USRP with no UHD, or no radio at all. Until now the
// Source section printed one of those answers only once somebody opened the
// Source list, and a person who has never opened it saw a receiver on the signal
// generator and nothing else. This is the page that says it on the way in.
//
// THREE LAYERS, so each can be proven without the others:
//
//   Inventory      plain data: what the probes saw. Filled by collectInventory()
//                  (Windows: SetupAPI + the Service Control Manager + the file
//                  system; elsewhere "not probed"), by the application for the
//                  half only it knows (which radios ITS OWN enumeration listed),
//                  or by a test, by hand.
//   assess()       PURE: Inventory in, Findings out. No Windows, no thread, no
//                  clock, no file. tests/test_radio_setup.cpp feeds it the
//                  shapes a real machine produces.
//   Probe          the thread: collectInventory() runs on a worker of its own
//                  with a time budget, so a SetupAPI call that never returns
//                  costs a page that says "could not check" and never a frozen
//                  window. The GUI polls it once a frame.
//
// NOTHING IS OPENED. The Windows probe reads device properties - the same
// rule-1 reads the native drivers' own enumeration makes (usb/usb_device.hpp) -
// asks the Service Control Manager for the SDRplay service's state (read-only,
// source/sdrplay_service.hpp) and looks for two DLLs on disk. No radio is opened
// and no transfer is sent, so the check is as safe with a radio streaming as
// without one. NOTHING IS SENT ANYWHERE either: the inventory stays on this
// machine and reaches only the page and the diagnostics log (PRIVACY.md).
//
// THE DRIVER A FINDING NAMES IS THE ONE WINDOWS REPORTS, not a guess from the
// id: an RTL-SDR on "RTL2832UUSB (Realtek)" is named so, and one with no driver
// at all (problem code 28) says that instead.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_RADIO_SETUP_HPP
#define CASCADE_CORE_RADIO_SETUP_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace cascade::core::radiosetup {

// --- the families --------------------------------------------------------------
//
// A FAMILY is the unit the application's own enumeration reports in and the unit
// whose install instruction differs, so every table below speaks in them. The
// spellings are the native driver keys where one exists (device_source.hpp's
// NativeDeviceInfo::driver), so the application maps its lists with no table of
// its own.
inline constexpr const char* kFamilyRtl = "rtlsdr";
inline constexpr const char* kFamilyAirspy = "airspy";
inline constexpr const char* kFamilyAirspyHf = "airspyhf";
inline constexpr const char* kFamilyHackRf = "hackrf";
inline constexpr const char* kFamilySdrPlay = "sdrplay";
inline constexpr const char* kFamilyUsrp = "usrp";
inline constexpr const char* kFamilyLime = "lime";

// The family a driver key belongs to, or "" when it is none of the above. Takes
// the native driver keys and the SoapySDR "driver=" values alike ("uhd" is the
// USRP's, "lime" the LimeSDR's; the Mirics and SDRplay modules are one family
// because an RSP is reached through the SDRplay API whichever module lists it).
// Case-insensitive.
std::string familyForDriver(const std::string& driverKey);

// --- the known radios ---------------------------------------------------------

struct KnownDevice {
    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
    const char* family = "";
    const char* name = "";  // "RTL-SDR (RTL2832U)": a product name, never translated
};

// Every USB id the check recognises. The ids are taken from the vendors' own
// published tables (udev rules and sources, checked 2026-10-06), not from
// memory: the list and where each was read are in radio_setup.cpp.
const std::vector<KnownDevice>& knownDevices();
const KnownDevice* findKnownDevice(std::uint16_t vid, std::uint16_t pid);

// --- the inventory ---------------------------------------------------------------

// One devnode as the bus walk read it. A composite dongle (every RTL-SDR) is
// several of these sharing a deviceId: the device node itself, which runs
// usbccgp, and interface children (MI_00, MI_01), one of which Zadig binds.
struct UsbNode {
    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
    // The instance id of the USB DEVICE node this devnode belongs to - the
    // devnode itself, or its parent for an interface child. What makes "one
    // radio" decidable out of several devnodes.
    std::string deviceId;
    bool isDeviceNode = false;
    // The composite interface number ("&MI_00" in the instance id), -1 for the
    // device node itself. (Not "interface": Windows' headers #define that.)
    int interfaceNumber = -1;
    std::string service;      // the driver service bound to THIS devnode, "" = none
    std::string provider;     // the INF's provider: "libwdi", "Realtek", "Ettus Research"
    std::string deviceClass;  // "USBDevice", "USRPs", "USB"
    std::string description;  // what the bus calls it, "" when it says nothing
    unsigned problemCode = 0; // Device Manager's problem code; 0 = none, 28 = no driver
};

struct SdrPlayApi {
    bool dllFound = false;       // sdrplay_api.dll under Program Files\SDRplay\API\x64
    bool servicePresent = false; // the Windows service is installed
    bool serviceRunning = false;
    std::string serviceState;    // the log's words: "running, auto start", "not installed"
};

struct Inventory {
    // False: nothing was probed (not Windows, or the probe could not run). The
    // findings say so and the page is never raised for it.
    bool probed = false;
    // False: the bus listing itself failed, so an empty `usb` means UNKNOWN.
    bool usbListed = false;
    // Why it was not probed, for the log and the page ("not probed on this platform").
    std::string note;
    // Every devnode of every KNOWN radio that is on the bus (never other
    // people's devices: the check has no use for them, and the log should not
    // carry them).
    std::vector<UsbNode> usb;
    SdrPlayApi sdrplay;
    // uhd.dll was found on PATH or in a vendor install - the half of UHD that
    // is software. The USB driver half is read off the radio's own devnode.
    bool uhdRuntime = false;

    // WHAT THE APPLICATION'S OWN ENUMERATION LISTED, as families, and how many
    // radios it listed in all (a radio of no known family still counts: a
    // network radio is a radio, and the page must not tell its owner there is
    // none). Native families are checked by the probe itself; the SoapySDR
    // families are checked only after the application has run its Soapy scan,
    // which it does lazily on purpose (soapy_enum_proc.hpp) - so a USRP is not
    // declared invisible by a scan that never ran.
    std::vector<std::string> visible;
    int visibleCount = 0;
    bool nativeChecked = false;
    bool soapyChecked = false;
};

// --- the findings -----------------------------------------------------------------

enum class Kind {
    Usable,         // right driver, vendor software present, and listed (or not checkable yet)
    WrongDriver,    // on the bus, bound to the wrong driver or to none
    VendorMissing,  // on the bus, but the vendor API or service it needs is not there
    NotVisible,     // set up correctly, but the application's own scan did not list it
    NoDevice,       // no known radio on the bus
    NotChecked      // the bus could not be read, or this platform is not probed
};

const char* kindName(Kind k);  // "usable", "wrong-driver", ... for the log and the census

struct Finding {
    Kind kind = Kind::Usable;
    std::string family;      // "" for NoDevice and NotChecked
    std::string deviceName;  // "RTL-SDR (RTL2832U)"
    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
    // WHAT IS INSTALLED, as the log and a bug report want it: "RTL2832UUSB
    // (Realtek)", "no driver (problem code 28)". English, never translated.
    std::string installed;
    // The four things the page shows, already translated (tr()). Never empty:
    // a finding with nothing to install says so, and every finding has a link.
    std::string title;
    std::string explanation;
    std::string install;
    std::string url;
};

struct Findings {
    std::vector<Finding> items;
    bool anyUsable = false;
    // THE PAGE'S TRIGGER. True when the application's own enumeration listed no
    // radio, none of the findings is usable, and the probe actually ran: the one
    // condition under which the page opens by itself.
    bool showPage = false;
};

// THE DECISION. Pure. One finding per physical radio found on the bus, or one
// NoDevice finding when there is none.
Findings assess(const Inventory& inv);

// THE LINES FOR THE DIAGNOSTICS LOG, each a whole fact and each short enough
// for the log's 191-byte line (a longer one is cut where it stands, and the
// first version of this - everything on one line - lost its findings off the
// end): what the bus and the application's lists held, what is installed, one
// line per finding ("rtlsdr RTL-SDR (RTL2832U) wrong-driver [RTL2832UUSB
// (Realtek)]"), and whether the page is wanted. No serial numbers, no paths, no
// instance ids. Every line starts "radio setup: ".
std::vector<std::string> summaryLines(const Inventory& inv, const Findings& f);

// --- the probe -----------------------------------------------------------------------

// Reads this machine. Windows: the bus walk, the SDRplay and UHD checks and the
// application's native enumeration of the four USB-only families. Anywhere else:
// an Inventory with probed == false and a note saying so. Blocks for as long as
// Windows takes - call it from a worker (Probe does).
Inventory collectInventory();

// FOXSDR_FAKE_RADIO_SETUP: a synthetic inventory for a bounded run, so the page
// can be made to appear with a chosen finding on a machine with no radio (and
// not appear on one with several). Names, each a whole machine: "none",
// "unreadable", "rtl-dvbt", "rtl-nodriver", "rtl-winusb", "rtl-winusb-listed",
// "rsp-noapi", "rsp-ok", "usrp-nouhd", "usrp-ok", "usrp-unlisted". Returns false
// for any other text and leaves `out` alone.
bool fakeInventory(const std::string& name, Inventory& out);

// How long the application waits for a probe before it stops waiting. A healthy
// bus walk takes tens of milliseconds; thirty seconds is a wedged SetupAPI call.
inline constexpr std::chrono::milliseconds kProbeBudget{30000};

// RUNS collectInventory() (or any collector) ON A THREAD OF ITS OWN and hands
// the result back on the GUI thread's poll. The destructor never waits: a probe
// stuck inside Windows is detached, and the state it writes to is kept alive by
// the thread itself.
class Probe {
public:
    enum class State { Idle, Running, Done, TimedOut };
    using Collector = std::function<Inventory()>;

    explicit Probe(std::chrono::milliseconds budget = kProbeBudget) : budget_(budget) {}
    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;
    ~Probe() = default;  // shared_ptr state; a stuck thread owns its own reference

    // GUI thread. Starts a run unless one is in flight; returns whether it did.
    // A run that timed out is abandoned (its thread may still be inside
    // Windows) and a new one may start.
    bool start(Collector collector);
    // GUI thread, once a frame. True when the state changed on this call: a
    // result arrived, or the budget ran out.
    bool poll();

    State state() const { return state_; }
    bool running() const { return state_ == State::Running; }
    // The last finished run's inventory; meaningful once a run has been Done.
    const Inventory& inventory() const { return result_; }
    // How many runs have finished (Done or TimedOut): lets a caller tell "the
    // answer it already used" from a fresh one.
    unsigned finished() const { return finished_; }

private:
    struct Shared {
        std::mutex m;
        bool done = false;
        Inventory result;
    };
    std::chrono::milliseconds budget_;
    State state_ = State::Idle;
    std::shared_ptr<Shared> shared_;
    std::chrono::steady_clock::time_point startedAt_{};
    Inventory result_;
    unsigned finished_ = 0;
};

}  // namespace cascade::core::radiosetup

#endif  // CASCADE_CORE_RADIO_SETUP_HPP
