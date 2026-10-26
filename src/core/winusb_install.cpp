// winusb_install.cpp - see winusb_install.hpp for what this is and why.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#if defined(_WIN32)
// UNICODE FOR THIS FILE: DiInstallDevice has a single export and its
// PSP_DRVINFO_DATA follows UNICODE, while every other call here uses the wide
// (W) structures explicitly. The project builds ANSI, which would hand it the
// narrow SP_DRVINFO_DATA_A layout for a W structure.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
// clang-format off
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
// clang-format on
#endif

#include "core/winusb_install.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "core/i18n.hpp"
#include "core/radio_setup.hpp"
#include "usb/usb_device.hpp"


namespace cascade::core::winusb {

using cascade::i18n::tr;

namespace {

std::string upper(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') { c = static_cast<char>(c - 'a' + 'A'); }
    }
    return s;
}

bool hasControlOrQuote(const std::string& s) {
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7f || c == '"') { return true; }
    }
    return false;
}

// "USB\VID_0BDA&PID_2838&MI_00" -> true when it is interface 0 of an RTL2832U id.
bool isRtlInterfaceZeroId(const std::string& hardwareId) {
    const std::string u = upper(hardwareId);
    if (u.rfind("USB\\VID_", 0) != 0) { return false; }
    // Exactly the interface-0 id: nothing after MI_00 (the REV_ form is a
    // different, longer id that Windows also lists; it carries the same VID/PID).
    const std::size_t mi = u.find("&MI_00");
    if (mi == std::string::npos) { return false; }
    const std::string tail = u.substr(mi + 6);
    if (!tail.empty()) { return false; }
    cascade::usb::UsbId id{0, 0};
    if (!cascade::usb::usbIdFromHardwareId(hardwareId, id)) { return false; }
    const auto* d = cascade::core::radiosetup::findKnownDevice(id.vid, id.pid);
    return d != nullptr && std::strcmp(d->family, cascade::core::radiosetup::kFamilyRtl) == 0;
}

}  // namespace

bool targetAllowed(const std::string& instanceId, const std::vector<std::string>& hardwareIds,
                   std::string& why) {
    if (instanceId.empty() || instanceId.size() > 200 || hasControlOrQuote(instanceId)) {
        why = "the device instance id is empty, too long or malformed";
        return false;
    }
    const std::string u = upper(instanceId);
    if (u.rfind("USB\\VID_", 0) != 0) {
        why = "not a USB device";
        return false;
    }
    // Interface 0 of a composite device: the instance id names the interface.
    const std::size_t mi = u.find("&MI_00\\");
    if (mi == std::string::npos) {
        why = "not interface 0 of a composite device (never the parent, never interface 1)";
        return false;
    }
    for (const std::string& h : hardwareIds) {
        if (isRtlInterfaceZeroId(h)) { return true; }
    }
    why = "the device's own hardware ids name no RTL2832U interface 0";
    return false;
}

const char* outcomeName(Outcome o) {
    switch (o) {
    case Outcome::Installed: return "installed";
    case Outcome::AlreadyWinUsb: return "already-winusb";
    case Outcome::Refused: return "refused";
    case Outcome::NotFound: return "not-found";
    case Outcome::NotAdmin: return "not-admin";
    case Outcome::Cancelled: return "cancelled";
    case Outcome::Failed: return "failed";
    }
    return "failed";
}

bool outcomeFromName(const std::string& name, Outcome& out) {
    for (Outcome o : {Outcome::Installed, Outcome::AlreadyWinUsb, Outcome::Refused,
                      Outcome::NotFound, Outcome::NotAdmin, Outcome::Cancelled, Outcome::Failed}) {
        if (name == outcomeName(o)) {
            out = o;
            return true;
        }
    }
    return false;
}

std::string formatResult(const Result& r) {
    std::string detail = r.detail;
    for (char& c : detail) {
        if (c == '\r' || c == '\n') { c = ' '; }
    }
    return std::string("outcome=") + outcomeName(r.outcome) + "\nreboot=" + (r.reboot ? "1" : "0") +
           "\ndetail=" + detail + "\n";
}

bool parseResult(const std::string& text, Result& out) {
    std::istringstream in(text);
    std::string line;
    bool haveOutcome = false;
    bool haveReboot = false;
    bool haveDetail = false;
    Result r;
    int lines = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (line.empty()) { continue; }
        if (++lines > 3) { return false; }
        if (line.rfind("outcome=", 0) == 0 && !haveOutcome) {
            if (!outcomeFromName(line.substr(8), r.outcome)) { return false; }
            haveOutcome = true;
        } else if (line.rfind("reboot=", 0) == 0 && !haveReboot) {
            const std::string v = line.substr(7);
            if (v != "0" && v != "1") { return false; }
            r.reboot = (v == "1");
            haveReboot = true;
        } else if (line.rfind("detail=", 0) == 0 && !haveDetail) {
            r.detail = line.substr(7);
            haveDetail = true;
        } else {
            return false;
        }
    }
    if (!haveOutcome || !haveReboot || !haveDetail) { return false; }
    out = r;
    return true;
}

bool parseHelperArgs(int argc, const char* const* argv, HelperArgs& out, std::string& error) {
    if (argc != 5 || argv == nullptr) {
        error = "usage: --install-winusb <device instance id> --result <file>";
        return false;
    }
    if (std::strcmp(argv[1], kHelperFlag) != 0 || std::strcmp(argv[3], kResultFlag) != 0) {
        error = "usage: --install-winusb <device instance id> --result <file>";
        return false;
    }
    const std::string id = argv[2] != nullptr ? argv[2] : "";
    const std::string path = argv[4] != nullptr ? argv[4] : "";
    if (id.empty() || id[0] == '-' || path.empty() || path[0] == '-') {
        error = "the device instance id and the result file must both be given";
        return false;
    }
    if (hasControlOrQuote(id) || hasControlOrQuote(path)) {
        error = "an argument holds a quote or a control character";
        return false;
    }
    out.instanceId = id;
    out.resultPath = path;
    return true;
}

std::string helperParameters(const std::string& instanceId, const std::string& resultPath) {
    if (instanceId.empty() || resultPath.empty() || hasControlOrQuote(instanceId) ||
        hasControlOrQuote(resultPath)) {
        return std::string();
    }
    // Windows' command-line rule: a backslash run is literal unless it precedes
    // a quote. Neither argument holds a quote, but the path can END in a
    // backslash, which would then escape the closing quote - so a trailing run
    // is doubled.
    const auto quoted = [](const std::string& s) {
        std::string t = s;
        std::size_t backslashes = 0;
        for (auto it = t.rbegin(); it != t.rend() && *it == '\\'; ++it) { ++backslashes; }
        t.append(backslashes, '\\');
        return "\"" + t + "\"";
    };
    return std::string(kHelperFlag) + " " + quoted(instanceId) + " " + kResultFlag + " " +
           quoted(resultPath);
}

std::string resultSentence(const Result& r) {
    switch (r.outcome) {
    case Outcome::Installed:
        return r.reboot ? tr("The driver is installed. Windows wants a restart before the radio "
                             "can be used; then press CHECK AGAIN.")
                        : tr("The driver is installed. If the radio is not listed yet, unplug it, "
                             "plug it back in and press CHECK AGAIN.");
    case Outcome::AlreadyWinUsb:
        return tr("This radio already has the right driver. Press CHECK AGAIN.");
    case Outcome::Cancelled:
        return tr("Windows did not get permission, so nothing was changed.");
    case Outcome::Refused:
        return tr("FoxSDR did not change this device: it is not an RTL-SDR's radio interface.");
    case Outcome::NotFound:
        return tr("The radio was unplugged before the driver could be installed. Plug it back "
                  "in and press CHECK AGAIN.");
    case Outcome::NotAdmin:
    case Outcome::Failed:
        break;
    }
    return tr("Windows would not install the driver. Install it with Zadig instead, as described "
              "above.");
}

#if defined(_WIN32)

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) { return std::wstring(); }
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const wchar_t* w) {
    if (w == nullptr || *w == L'\0') { return std::string(); }
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) { ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr); }
    return s;
}

std::string stepFailed(const char* step) {
    const DWORD e = ::GetLastError();
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s failed (Windows error %lu)", step,
                  static_cast<unsigned long>(e));
    return buf;
}

bool processIsElevated() {
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) { return false; }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const BOOL ok = ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    ::CloseHandle(token);
    return ok != FALSE && elevation.TokenIsElevated != 0;
}

std::vector<std::string> multiSz(HDEVINFO set, SP_DEVINFO_DATA& info, DWORD property) {
    std::vector<std::string> out;
    DWORD type = 0;
    DWORD needed = 0;
    ::SetupDiGetDeviceRegistryPropertyW(set, &info, property, &type, nullptr, 0, &needed);
    if (needed == 0) { return out; }
    std::vector<wchar_t> buf(needed / sizeof(wchar_t) + 2, L'\0');
    if (::SetupDiGetDeviceRegistryPropertyW(set, &info, property, &type,
                                            reinterpret_cast<PBYTE>(buf.data()),
                                            static_cast<DWORD>(buf.size() * sizeof(wchar_t)),
                                            nullptr) == FALSE) {
        return out;
    }
    for (const wchar_t* p = buf.data(); *p != L'\0'; p += std::wcslen(p) + 1) {
        out.push_back(narrow(p));
    }
    return out;
}

std::string stringProperty(HDEVINFO set, SP_DEVINFO_DATA& info, DWORD property) {
    const std::vector<std::string> v = multiSz(set, info, property);
    return v.empty() ? std::string() : v.front();
}

}  // namespace

Result installOn(const std::string& instanceId) {
    Result r;
    std::string why;

    const HDEVINFO set = ::SetupDiCreateDeviceInfoList(nullptr, nullptr);
    if (set == INVALID_HANDLE_VALUE) {
        r.detail = stepFailed("SetupDiCreateDeviceInfoList");
        return r;
    }
    struct Closer {
        HDEVINFO s;
        ~Closer() { ::SetupDiDestroyDeviceInfoList(s); }
    } closer{set};

    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof(info);
    const std::wstring wid = widen(instanceId);
    if (::SetupDiOpenDeviceInfoW(set, wid.c_str(), nullptr, 0, &info) == FALSE) {
        r.outcome = Outcome::NotFound;
        r.detail = stepFailed("SetupDiOpenDeviceInfo");
        return r;
    }
    // THE DEVICE ITSELF decides, not the command line that named it.
    const std::vector<std::string> hw = multiSz(set, info, SPDRP_HARDWAREID);
    if (!targetAllowed(instanceId, hw, why)) {
        r.outcome = Outcome::Refused;
        r.detail = why;
        return r;
    }
    if (upper(stringProperty(set, info, SPDRP_SERVICE)) == "WINUSB") {
        r.outcome = Outcome::AlreadyWinUsb;
        r.detail = "the interface already runs WinUSB";
        return r;
    }
    if (!processIsElevated()) {
        r.outcome = Outcome::NotAdmin;
        r.detail = "the helper is not running elevated";
        return r;
    }

    // THE ONE INF: the in-box, Microsoft-signed winusb.inf, and nothing else.
    wchar_t windir[MAX_PATH] = {0};
    if (::GetWindowsDirectoryW(windir, MAX_PATH) == 0) {
        r.detail = stepFailed("GetWindowsDirectory");
        return r;
    }
    const std::wstring inf = std::wstring(windir) + L"\\INF\\winusb.inf";
    SP_DEVINSTALL_PARAMS_W params{};
    params.cbSize = sizeof(params);
    if (::SetupDiGetDeviceInstallParamsW(set, &info, &params) == FALSE) {
        r.detail = stepFailed("SetupDiGetDeviceInstallParams");
        return r;
    }
    params.Flags |= DI_ENUMSINGLEINF;
    params.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
#if defined(DI_FLAGSEX_NO_CLASSLIST_NODE_MERGE)
    params.FlagsEx |= DI_FLAGSEX_NO_CLASSLIST_NODE_MERGE;
#endif
    if (inf.size() >= MAX_PATH) {
        r.detail = "the Windows INF path is too long";
        return r;
    }
    wcsncpy_s(params.DriverPath, MAX_PATH, inf.c_str(), _TRUNCATE);
    if (::SetupDiSetDeviceInstallParamsW(set, &info, &params) == FALSE) {
        r.detail = stepFailed("SetupDiSetDeviceInstallParams");
        return r;
    }
    if (::SetupDiBuildDriverInfoList(set, &info, SPDIT_CLASSDRIVER) == FALSE) {
        r.detail = stepFailed("SetupDiBuildDriverInfoList");
        return r;
    }

    // THE WINUSB NODE, by section name: the INF's WINUSB_LowerFilter model has
    // the same display name and would install a filter, not the function driver.
    SP_DRVINFO_DATA_W chosen{};
    bool found = false;
    for (DWORD i = 0;; ++i) {
        SP_DRVINFO_DATA_W drv{};
        drv.cbSize = sizeof(drv);
        if (::SetupDiEnumDriverInfoW(set, &info, SPDIT_CLASSDRIVER, i, &drv) == FALSE) { break; }
        DWORD needed = 0;
        ::SetupDiGetDriverInfoDetailW(set, &info, &drv, nullptr, 0, &needed);
        if (needed < sizeof(SP_DRVINFO_DETAIL_DATA_W)) { continue; }
        std::vector<unsigned char> buf(needed);
        auto* detail = reinterpret_cast<SP_DRVINFO_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DRVINFO_DETAIL_DATA_W);
        if (::SetupDiGetDriverInfoDetailW(set, &info, &drv, detail, needed, nullptr) == FALSE) {
            continue;
        }
        if (_wcsicmp(detail->SectionName, L"WINUSB") == 0) {
            chosen = drv;
            found = true;
            break;
        }
    }
    if (!found) {
        r.detail = "Windows offered no WINUSB driver node from winusb.inf for this device";
        return r;
    }
    if (::SetupDiSetSelectedDriverW(set, &info, &chosen) == FALSE) {
        r.detail = stepFailed("SetupDiSetSelectedDriver");
        return r;
    }
    BOOL reboot = FALSE;
    if (::DiInstallDevice(nullptr, set, &info, &chosen, 0, &reboot) == FALSE) {
        r.detail = stepFailed("DiInstallDevice");
        return r;
    }
    r.reboot = (reboot != FALSE);

    // THE INTERFACE GUID the manual route does not add, so libusb-based
    // programs find the interface as they find a Zadig install.
    HKEY key = ::SetupDiOpenDevRegKey(set, &info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_SET_VALUE);
    if (key == INVALID_HANDLE_VALUE) {
        key = ::SetupDiCreateDevRegKeyW(set, &info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, nullptr,
                                        nullptr);
    }
    if (key != INVALID_HANDLE_VALUE) {
        std::wstring guids = widen(kInterfaceGuid);
        guids.push_back(L'\0');  // REG_MULTI_SZ: the string, then the list's end
        guids.push_back(L'\0');
        ::RegSetValueExW(key, L"DeviceInterfaceGUIDs", 0, REG_MULTI_SZ,
                         reinterpret_cast<const BYTE*>(guids.data()),
                         static_cast<DWORD>(guids.size() * sizeof(wchar_t)));
        ::RegCloseKey(key);
    }

    // A RESTART OF THE DEVICE so the interface GUID is registered now rather
    // than at the next plug-in. A refusal is not a failed install: the driver is
    // in place, and a replug finishes the job (resultSentence says so).
    SP_PROPCHANGE_PARAMS change{};
    change.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    change.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    change.StateChange = DICS_PROPCHANGE;
    change.Scope = DICS_FLAG_GLOBAL;
    change.HwProfile = 0;
    if (::SetupDiSetClassInstallParamsW(set, &info, &change.ClassInstallHeader, sizeof(change)) !=
            FALSE) {
        ::SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &info);
    }

    r.outcome = Outcome::Installed;
    r.detail = "WinUSB installed from the Windows winusb.inf";
    return r;
}

#else  // !_WIN32

Result installOn(const std::string&) {
    Result r;
    r.outcome = Outcome::Failed;
    r.detail = "not on this platform";
    return r;
}

#endif

int runHelperMain(int argc, const char* const* argv) {
    HelperArgs args;
    std::string error;
    if (!parseHelperArgs(argc, argv, args, error)) {
        std::fprintf(stderr, "cascade: %s\n", error.c_str());
        return 2;
    }
    const Result r = installOn(args.instanceId);
    {
        std::ofstream out(args.resultPath, std::ios::binary | std::ios::trunc);
        out << formatResult(r);
    }
    std::printf("winusb: %s - %s\n", outcomeName(r.outcome), r.detail.c_str());
    std::fflush(stdout);
    return (r.outcome == Outcome::Installed || r.outcome == Outcome::AlreadyWinUsb) ? 0 : 3;
}

}  // namespace cascade::core::winusb
