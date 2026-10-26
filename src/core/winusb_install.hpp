// winusb_install.hpp - ONE CLICK INSTEAD OF ZADIG (0.99.88).
//
// WHY. The usage reports of the 30 days to 2026-10-24: 969 installs, 493 of
// which ever had a radio open - and an RTL-SDR, two thirds of the radios that
// are found, arrives on Windows with no driver (problem code 28) or with the
// DVB-T television driver, invisible to every SDR program until somebody runs
// Zadig, picks "Bulk-In, Interface (Interface 0)" from a list of every USB
// device on the machine, and replaces its driver. The RADIO SETUP page already
// says that (core/radio_setup.hpp). This does it: one key, one Windows
// permission prompt.
//
// HOW, AND WHY NOTHING HAS TO BE SIGNED. Windows 8 and later ship winusb.sys
// with an in-box, Microsoft-signed winusb.inf whose "WinUsb Device" model
// (section WINUSB, class USBDevice) Device Manager offers for ANY USB device
// under "Let me pick from a list" - Microsoft Learn, "WinUSB (Winusb.sys)
// installation". This is the programmatic equivalent, on ONE device instance:
// SetupDiBuildDriverInfoList restricted to that one INF (DI_ENUMSINGLEINF +
// DriverPath), the WINUSB node picked by section name (the INF's second model,
// WINUSB_LowerFilter, has the same display name and would install a FILTER),
// SetupDiSetSelectedDriver, DiInstallDevice - then the interface GUID the
// manual route does not add (DeviceInterfaceGUIDs under Device Parameters),
// and a property-change restart so it takes effect. No INF of ours, no
// catalogue, no certificate added to anybody's machine (which is what
// libwdi/Zadig do, and what an LGPL/GPL tool would have brought in).
//
// WHAT IT WILL TOUCH, decided by targetAllowed() and checked AGAIN by the
// elevated helper against the device itself, never taken on trust from its
// command line: interface 0 (&MI_00) of a composite device whose own hardware
// ids name an RTL2832U id the native driver opens. Never the composite parent
// (that would take the whole dongle from usbccgp), never interface 1 (the IR
// receiver), never another device.
//
// WHERE IT RUNS. In the application's own executable, started elevated:
//     cascade --install-winusb <device instance id> --result <file>
// No configuration, window, radio, crash handler or telemetry: it does the one
// install, writes the result file and exits (runHelperMain). The application
// starts it through the shell's "runas" verb, waits for it off the GUI thread,
// reads the file and checks the radios again.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_WINUSB_INSTALL_HPP
#define CASCADE_CORE_WINUSB_INSTALL_HPP

#include <string>
#include <vector>

namespace cascade::core::winusb {

// The device interface GUID registered for an interface this installer binds,
// so programs that look for one (libusb, librtlsdr, SDR#) find the dongle the
// way they find a Zadig install. FoxSDR's own transport needs no GUID of its
// own (usb/winusb_device.cpp falls back to GUID_DEVINTERFACE_WINUSB).
inline constexpr const char* kInterfaceGuid = "{ADE7CA4A-B957-4A9E-B141-2CD86B16AA23}";

// The flag that starts the elevated helper, and its result-file flag.
inline constexpr const char* kHelperFlag = "--install-winusb";
inline constexpr const char* kResultFlag = "--result";

// PURE. May this devnode be given WinUSB? `instanceId` is the devnode's
// instance id ("USB\VID_0BDA&PID_2838&MI_00\7&38A180A5&0&0000"), `hardwareIds`
// its hardware ids as Windows lists them. True only for interface 0 of a
// composite device whose hardware ids include USB\VID_xxxx&PID_yyyy&MI_00 for
// an RTL2832U id (radiosetup::findKnownDevice, family rtlsdr). `why` says what
// failed, in English, for the log and the result file.
bool targetAllowed(const std::string& instanceId, const std::vector<std::string>& hardwareIds,
                   std::string& why);

enum class Outcome {
    Installed,      // WinUSB is now the driver of that interface
    AlreadyWinUsb,  // it already was: nothing done
    Refused,        // not a device this installer may touch (targetAllowed)
    NotFound,       // no present device has that instance id
    NotAdmin,       // the helper was not elevated
    Cancelled,      // the person said no to the permission prompt (set by the caller)
    Failed          // Windows refused a step; detail says which
};

// "installed", "already-winusb", ... - the result file's and the log's word.
const char* outcomeName(Outcome o);
bool outcomeFromName(const std::string& name, Outcome& out);

struct Result {
    Outcome outcome = Outcome::Failed;
    std::string detail;     // English; a step and a Windows error code, never an instance id
    bool reboot = false;    // Windows asked for a restart
};

// The result file: three lines, "outcome=<name>", "reboot=0|1",
// "detail=<one line>". parseResult refuses anything else, so a truncated or
// foreign file is a failure and not a success.
std::string formatResult(const Result& r);
bool parseResult(const std::string& text, Result& out);

// The helper's arguments, exactly: argv[1] == kHelperFlag, argv[2] the
// instance id, argv[3] == kResultFlag, argv[4] the result path. Anything else
// is refused, so no combination of a real session's arguments can become an
// install.
struct HelperArgs {
    std::string instanceId;
    std::string resultPath;
};
bool parseHelperArgs(int argc, const char* const* argv, HelperArgs& out, std::string& error);

// The parameter string the application hands the shell for the helper:
// every argument quoted. Refuses (returns "") an argument holding a quote or a
// control character, which no instance id or temp path has.
std::string helperParameters(const std::string& instanceId, const std::string& resultPath);

// The person-facing sentence for a result, already translated (tr()).
std::string resultSentence(const Result& r);

// Windows: the install itself, on the device with that instance id. Must run
// elevated. Elsewhere: Failed, "not on this platform".
Result installOn(const std::string& instanceId);

// The whole of `cascade --install-winusb ...`: parse, install, write the
// result file. Exit code 0 for Installed or AlreadyWinUsb, 2 for bad
// arguments, 3 for anything else (the file says what).
int runHelperMain(int argc, const char* const* argv);

}  // namespace cascade::core::winusb

#endif  // CASCADE_CORE_WINUSB_INSTALL_HPP
