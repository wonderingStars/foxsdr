// AOR receiver control. See aor_control.hpp for the source and the rules.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/aor_control.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <thread>

#include "core/diag_log.hpp"
#include "core/i18n.hpp"
#include "core/serial_port.hpp"
#include "core/utf8_text.hpp"
#include "source/aor_protocol.hpp"

#if defined(_WIN32)
// clang-format off
#include <windows.h>
#include <setupapi.h>
// clang-format on
#include "usb/usb_device.hpp"
#else
#include <dirent.h>
#endif

namespace cascade::source::aor {

using cascade::i18n::tr;

// --- the pure half ------------------------------------------------------------

bool formatTuneCommand(double hz, std::string& out) {
    if (!std::isfinite(hz) || hz < 0.0 || hz > kMaxFormattableHz) { return false; }
    const unsigned long long whole = static_cast<unsigned long long>(std::llround(hz));
    if (whole > 9999999999ull) { return false; }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "RF%04llu.%06llu", whole / 1000000ull, whole % 1000000ull);
    out = buf;
    return true;
}

namespace {

std::string trimmed(const std::string& s) {
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n' || s[a] == '\0')) {
        ++a;
    }
    while (b > a &&
           (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n' ||
            s[b - 1] == '\0')) {
        --b;
    }
    return s.substr(a, b - a);
}

// Printable ASCII only, for messages and the log: a reply from a device that
// is not a receiver can be anything.
std::string printable(const std::string& s) {
    std::string out;
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        out.push_back(u >= 0x20 && u < 0x7F ? c : '?');
        if (out.size() >= 64) {
            out += "...";
            break;
        }
    }
    return out;
}

}  // namespace

Identity identifyVr(const std::string& reply) {
    Identity id;
    id.reply = trimmed(reply);
    struct Row {
        const char* token;
        Model model;
        bool verified;
        bool compatMode;
    };
    // THE MODEL TABLE. Only the first row is in the AOR document.
    static const Row kRows[] = {
        {"C5700_", Model::AR5700D, true, true},    // documented
        {"C2300_", Model::AR2300, false, false},   // UNVERIFIED guess by analogy
        {"C5001_", Model::AR5001D, false, false},  // UNVERIFIED guess by analogy
        {"C6000_", Model::AR6000, false, false},   // UNVERIFIED guess by analogy
    };
    for (const Row& r : kRows) {
        if (id.reply.find(r.token) != std::string::npos) {
            id.isAor = true;
            id.model = r.model;
            id.verified = r.verified;
            id.sendCompatMode = r.compatMode;
            return id;
        }
    }
    return id;
}

const char* modelName(Model m) {
    switch (m) {
        case Model::AR5700D: return "AR5700D";
        case Model::AR2300: return "AR2300";
        case Model::AR5001D: return "AR5001D";
        case Model::AR6000: return "AR6000";
        case Model::Unknown: break;
    }
    return "unknown";
}

// --- the serial link --------------------------------------------------------------

namespace {

class SerialControlLink final : public ControlLink {
public:
    int write(const char* data, std::size_t len) override { return port_.write(data, len); }
    int read(char* buf, std::size_t cap) override { return port_.read(buf, cap); }
    std::string name() const override { return name_; }
    bool open(const std::string& port, std::string& error) {
        name_ = port;
        return port_.open(port, kControlBaud, error, /*readWrite=*/true);
    }

private:
    cascade::core::SerialPort port_;
    std::string name_;
};

}  // namespace

std::unique_ptr<ControlLink> openSerialControlLink(const std::string& port, std::string& error) {
    auto link = std::make_unique<SerialControlLink>();
    if (!link->open(port, error)) { return nullptr; }
    return link;
}

// --- the session ------------------------------------------------------------------

bool ControlSession::send(const std::string& command, std::string& error) {
    const std::string line = command + "\r";
    const int n = link_.write(line.data(), line.size());
    if (n != static_cast<int>(line.size())) {
        error = cascade::core::formatText(
            tr("The AOR receiver's control port %s stopped taking commands."), link_.name().c_str());
        return false;
    }
    return true;
}

std::string ControlSession::readLine(std::chrono::milliseconds wait, bool& gone) {
    gone = false;
    const auto deadline = std::chrono::steady_clock::now() + wait;
    for (;;) {
        // A complete line already in hand?
        for (;;) {
            const std::size_t eol = pending_.find_first_of("\r\n");
            if (eol == std::string::npos) { break; }
            const std::string line = trimmed(pending_.substr(0, eol));
            pending_.erase(0, eol + 1);
            if (!line.empty()) { return line; }
        }
        if (std::chrono::steady_clock::now() >= deadline) { return std::string(); }
        char buf[128];
        const int n = link_.read(buf, sizeof(buf));
        if (n < 0) {
            gone = true;
            return std::string();
        }
        if (n > 0) {
            pending_.append(buf, static_cast<std::size_t>(n));
            if (pending_.size() > 4096) { pending_.erase(0, pending_.size() - 4096); }
        }
    }
}

bool ControlSession::probe(Identity& id, std::string& error) {
    id = Identity{};
    pending_.clear();
    // EX FIRST, as the AOR document's tested sequence has it: it returns a
    // receiver left in a compatible mode to its normal command set, and VR
    // may not be answered until it has been.
    if (!send("EX", error)) { return false; }
    bool gone = false;
    readLine(kReplyWait, gone);  // whatever EX answers, if anything, is not VR's answer
    if (gone) {
        error = cascade::core::formatText(tr("The AOR receiver's control port %s stopped taking commands."),
                                          link_.name().c_str());
        return false;
    }
    pending_.clear();
    if (!send("VR", error)) { return false; }
    // Every line within the wait is looked at: a receiver that echoes the
    // command sends "VR" before its answer.
    const auto deadline = std::chrono::steady_clock::now() + kReplyWait;
    std::string first;
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) { break; }
        const std::string line =
            readLine(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now), gone);
        if (gone) {
            error = cascade::core::formatText(
                tr("The AOR receiver's control port %s stopped taking commands."), link_.name().c_str());
            return false;
        }
        if (line.empty()) { break; }
        if (first.empty()) { first = line; }
        const Identity candidate = identifyVr(line);
        if (candidate.isAor) {
            id = candidate;
            return true;
        }
    }
    id = identifyVr(first);
    return true;
}

bool ControlSession::initialise(Identity& id, std::string& error) {
    if (!probe(id, error)) { return false; }
    if (!id.isAor) {
        error = id.reply.empty()
                    ? cascade::core::formatText(
                          tr("%s did not answer VR - it is not an AOR receiver, or the receiver is "
                             "switched off."),
                          link_.name().c_str())
                    : cascade::core::formatText(
                          tr("%s is not an AOR receiver: it answered VR with \"%s\"."),
                          link_.name().c_str(), printable(id.reply).c_str());
        return false;
    }
    if (id.sendCompatMode && !send("@21", error)) { return false; }
    if (!send("VFA", error)) { return false; }
    pending_.clear();
    return true;
}

bool ControlSession::tune(double hz, std::string& error) {
    std::string cmd;
    if (!formatTuneCommand(hz, cmd)) {
        error = cascade::core::formatText(
            tr("%.6f MHz cannot be sent to an AOR receiver; the RF command carries 0 to 9999.999999 MHz."),
            hz / 1e6);
        return false;
    }
    return send(cmd, error);
}

// --- which serial port --------------------------------------------------------------

std::vector<std::string> ftdiPortsFromSysfs(const std::string& ttyClassDir,
                                            const std::string& devDir) {
    std::vector<std::string> out;
#if defined(_WIN32)
    (void)ttyClassDir;
    (void)devDir;
#else
    DIR* d = ::opendir(ttyClassDir.c_str());
    if (d == nullptr) { return out; }
    const auto readAttr = [](const std::string& path) {
        std::ifstream f(path);
        std::string s;
        std::getline(f, s);
        return trimmed(s);
    };
    for (struct dirent* e = ::readdir(d); e != nullptr; e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name.rfind("ttyUSB", 0) != 0) { continue; }
        // <tty>/device is the USB INTERFACE the port belongs to; its parent
        // is the USB DEVICE, which carries the ids. The kernel resolves
        // "device/.." through the symlink.
        const std::string base = ttyClassDir + "/" + name + "/device/../";
        const std::string vid = readAttr(base + "idVendor");
        const std::string pid = readAttr(base + "idProduct");
        if (vid == "0403" && pid == "6001") { out.push_back(devDir + "/" + name); }
    }
    ::closedir(d);
#endif
    return cascade::core::sortSerialPortNames(std::move(out));
}

std::vector<std::string> ftdiControlPortCandidates() {
#if defined(_WIN32)
    // The Ports device class, {4D36E978-E325-11CE-BFC1-08002BE10318}
    // (GUID_DEVCLASS_PORTS), spelled here rather than pulled in through
    // devguid.h and initguid.h ordering.
    static const GUID kPortsClass = {
        0x4d36e978, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
    std::vector<std::string> out;
    const HDEVINFO set = ::SetupDiGetClassDevsW(&kPortsClass, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) { return out; }
    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof(info);
    for (DWORD i = 0; ::SetupDiEnumDeviceInfo(set, i, &info) != FALSE; ++i) {
        wchar_t hw[512] = {0};
        if (::SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_HARDWAREID, nullptr,
                                                reinterpret_cast<PBYTE>(hw), sizeof(hw) - 2,
                                                nullptr) == FALSE) {
            continue;
        }
        // "FTDIBUS\COMPORT&VID_0403&PID_6001" - the pair is what matters.
        char narrowHw[512] = {0};
        ::WideCharToMultiByte(CP_UTF8, 0, hw, -1, narrowHw, sizeof(narrowHw) - 1, nullptr, nullptr);
        cascade::usb::UsbId id{0, 0};
        if (!cascade::usb::usbIdFromHardwareId(narrowHw, id) || id.vid != kFtdiVid ||
            id.pid != kFtdiPid) {
            continue;
        }
        const HKEY key = ::SetupDiOpenDevRegKey(set, &info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) { continue; }
        wchar_t portName[64] = {0};
        DWORD size = sizeof(portName) - sizeof(wchar_t);
        DWORD type = 0;
        if (::RegQueryValueExW(key, L"PortName", nullptr, &type, reinterpret_cast<LPBYTE>(portName),
                               &size) == ERROR_SUCCESS &&
            type == REG_SZ) {
            char narrow[64] = {0};
            ::WideCharToMultiByte(CP_UTF8, 0, portName, -1, narrow, sizeof(narrow) - 1, nullptr,
                                  nullptr);
            const std::string name = cascade::core::sanitiseSerialPortName(narrow);
            if (!name.empty()) { out.push_back(name); }
        }
        ::RegCloseKey(key);
    }
    ::SetupDiDestroyDeviceInfoList(set);
    return cascade::core::sortSerialPortNames(std::move(out));
#else
    return ftdiPortsFromSysfs("/sys/class/tty", "/dev");
#endif
}

Pairing pairControlPort(const std::vector<std::string>& candidates, const std::string& requested,
                        const LinkOpener& open) {
    Pairing p;
    std::vector<std::string> ports = candidates;
    if (!requested.empty()) { ports = {requested}; }
    if (ports.empty()) {
        p.error = tr("No FTDI serial port was found for the AOR receiver's control connection. "
                     "Connect the receiver's control USB cable and switch the receiver on.");
        return p;
    }
    std::vector<std::pair<std::string, Identity>> aors;
    for (const std::string& port : ports) {
        std::string error;
        std::unique_ptr<ControlLink> link = open(port, error);
        const std::string logName = cascade::core::loggableSerialPortName(port);
        if (link == nullptr) {
            p.tried.push_back(logName + ": could not open (" + error + ")");
            if (!requested.empty()) {
                p.error = error;
                return p;
            }
            continue;
        }
        ControlSession session(*link);
        Identity id;
        if (!session.probe(id, error)) {
            p.tried.push_back(logName + ": " + error);
            if (!requested.empty()) {
                p.error = error;
                return p;
            }
            continue;
        }
        p.tried.push_back(logName + ": VR -> \"" + printable(id.reply) + "\"" +
                          (id.isAor ? std::string(" (") + modelName(id.model) +
                                          (id.verified ? ")" : ", unverified model)")
                                    : std::string(" (not AOR)")));
        if (id.isAor) {
            aors.emplace_back(port, id);
        } else if (!requested.empty()) {
            p.error = id.reply.empty()
                          ? cascade::core::formatText(
                                tr("%s did not answer VR - it is not an AOR receiver, or the "
                                   "receiver is switched off."),
                                port.c_str())
                          : cascade::core::formatText(
                                tr("%s is not an AOR receiver: it answered VR with \"%s\"."),
                                port.c_str(), printable(id.reply).c_str());
            return p;
        }
    }
    if (aors.empty()) {
        p.error = cascade::core::formatText(
            tr("No serial port answered as an AOR receiver (%zu FTDI port(s) asked). Check that "
               "the receiver is switched on and its control USB cable is connected."),
            ports.size());
        return p;
    }
    if (aors.size() > 1) {
        std::string names;
        for (const auto& a : aors) { names += (names.empty() ? "" : ", ") + a.first; }
        p.error = cascade::core::formatText(
            tr("More than one AOR receiver answered (%s). FoxSDR cannot yet tell which control "
               "port belongs to which I/Q interface - connect one AOR receiver at a time."),
            names.c_str());
        return p;
    }
    p.ok = true;
    p.port = aors.front().first;
    p.identity = aors.front().second;
    return p;
}

}  // namespace cascade::source::aor
