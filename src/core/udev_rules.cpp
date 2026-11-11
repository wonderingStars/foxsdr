// Implementation of core/udev_rules.hpp. Pure text and decisions; runs and is tested on Windows.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/udev_rules.hpp"

#include <algorithm>
#include <cstdio>
#include <map>

#include "core/linux_update_host.hpp"
#include "source/airspy_protocol.hpp"
#include "source/airspyhf_protocol.hpp"
#include "source/aor_protocol.hpp"
#include "source/hackrf_protocol.hpp"
#include "source/hydrasdr_protocol.hpp"
#include "source/msi2500.hpp"
#include "source/rtlsdr_source.hpp"
#include "source/rx888_protocol.hpp"

namespace cascade::core::udev {

namespace {

constexpr int kPkexecTimeoutMs = 180000;  // the password dialog waits on a person

std::string hex4(std::uint16_t v) {
    char b[8];
    std::snprintf(b, sizeof(b), "%04x", static_cast<unsigned>(v));
    return b;
}

bool parseHex4(const std::string& s, std::size_t at, std::uint16_t& out) {
    unsigned v = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const char c = s[at + i];
        unsigned d;
        if (c >= '0' && c <= '9') { d = static_cast<unsigned>(c - '0'); }
        else if (c >= 'a' && c <= 'f') { d = static_cast<unsigned>(c - 'a' + 10); }
        else { return false; }  // lower-case only, as generated
        v = v * 16 + d;
    }
    out = static_cast<std::uint16_t>(v);
    return true;
}

const std::string& rulePrefix() {
    static const std::string p = "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"";
    return p;
}

// Parses one rule line into its id; false for anything that is not EXACTLY ruleLine() of an id.
bool parseRuleLine(const std::string& line, std::uint16_t& vid, std::uint16_t& pid) {
    const std::string tmpl = ruleLine(0, 0);
    if (line.size() != tmpl.size()) { return false; }
    const std::size_t vAt = rulePrefix().size();
    const std::size_t pAt = tmpl.find("ATTR{idProduct}==\"") + std::string("ATTR{idProduct}==\"").size();
    if (!parseHex4(line, vAt, vid) || !parseHex4(line, pAt, pid)) { return false; }
    return line == ruleLine(vid, pid);
}

}  // namespace

std::string outcomeName(Outcome o) {
    switch (o) {
        case Outcome::Installed: return "installed";
        case Outcome::AlreadyInstalled: return "already-installed";
        case Outcome::Declined: return "declined";
        case Outcome::NoPkexec: return "no-pkexec";
        case Outcome::NoHelper: return "no-helper";
        case Outcome::HelperMismatch: return "helper-mismatch";
        case Outcome::AuthDismissed: return "auth-dismissed";
        case Outcome::NotAuthorized: return "not-authorized";
        case Outcome::WriteFailed: return "write-failed";
        case Outcome::Timeout: return "timeout";
    }
    return "unknown";
}

std::vector<RadioId> supportedRadioIds() {
    std::map<std::pair<std::uint16_t, std::uint16_t>, std::string> m;
    auto add = [&m](std::uint16_t v, std::uint16_t p, const char* fam) {
        m.emplace(std::make_pair(v, p), fam);  // the first family to name an id keeps it
    };
    for (const usb::UsbId& id : source::rtlSdrUsbIds()) { add(id.vid, id.pid, "RTL2832U-based RTL-SDR dongles"); }
    add(source::hackrf::kUsbVid, source::hackrf::kPidHackRfOne, "HackRF (One, Jawbreaker, rad1o)");
    add(source::hackrf::kUsbVid, source::hackrf::kPidJawbreaker, "HackRF (One, Jawbreaker, rad1o)");
    add(source::hackrf::kUsbVid, source::hackrf::kPidRad1o, "HackRF (One, Jawbreaker, rad1o)");
    add(source::rx888::kUsbVid, source::rx888::kPidStreamer, "RX888 mk2 (bootloader and streamer ids)");
    add(source::rx888::kUsbVid, source::rx888::kPidBootloader, "RX888 mk2 (bootloader and streamer ids)");
    for (const source::msi2500::DeviceModel& d : source::msi2500::deviceModels()) {
        add(d.vid, d.pid, "Mirics MSi2500 and the sticks built on it");
    }
    add(source::airspy::kUsbVid, source::airspy::kUsbPid, "Airspy R2 / Mini");
    add(source::airspyhf::kUsbVid, source::airspyhf::kUsbPid, "Airspy HF+");
    add(source::hydrasdr::kUsbVid, source::hydrasdr::kUsbPid, "HydraSDR RFOne");
    add(source::aor::kIqVid, source::aor::kIqPid, "AOR digital I/Q interface");
    std::vector<RadioId> out;
    for (const auto& e : m) { out.push_back(RadioId{e.first.first, e.first.second, e.second}); }
    return out;
}

std::string ruleLine(std::uint16_t vid, std::uint16_t pid) {
    return rulePrefix() + hex4(vid) + "\", ATTR{idProduct}==\"" + hex4(pid) +
           "\", TAG+=\"uaccess\", MODE=\"0660\", GROUP=\"plugdev\"";
}

std::string generateRulesText() {
    std::string t =
        "# udev rules for FoxSDR's native USB radio drivers. GENERATED from the application's own\n"
        "# device tables (core/udev_rules.cpp): do not edit by hand.\n"
        "# TAG+=\"uaccess\" gives the user at the seat access through systemd-logind; MODE/GROUP is the\n"
        "# fallback for a machine with no seat (add yourself to the plugdev group).\n"
        "# After a change, unplug and replug the radio.\n";
    std::string family;
    for (const RadioId& id : supportedRadioIds()) {
        (void)family;
        t += ruleLine(id.vid, id.pid) + "\n";
    }
    return t;
}

bool validateRulesText(const std::string& text, std::string& error) {
    error.clear();
    std::size_t i = 0;
    int lineNo = 0;
    while (i < text.size()) {
        std::size_t nl = text.find('\n', i);
        const bool last = nl == std::string::npos;
        if (last) { nl = text.size(); }
        const std::string line = text.substr(i, nl - i);
        ++lineNo;
        i = nl + 1;
        if (line.empty()) { continue; }
        if (line[0] == '#') {
            for (const char c : line) {
                if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e) {
                    error = "line " + std::to_string(lineNo) + ": a comment holds a control or non-ASCII character";
                    return false;
                }
            }
            continue;
        }
        std::uint16_t v, p;
        if (!parseRuleLine(line, v, p)) {
            error = "line " + std::to_string(lineNo) + " is not a FoxSDR radio rule";
            return false;
        }
    }
    return true;
}

std::vector<std::pair<std::uint16_t, std::uint16_t>> idsInRulesText(const std::string& text) {
    std::vector<std::pair<std::uint16_t, std::uint16_t>> out;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t nl = text.find('\n', i);
        if (nl == std::string::npos) { nl = text.size(); }
        std::string line = text.substr(i, nl - i);
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        i = nl + 1;
        std::uint16_t v, p;
        if (parseRuleLine(line, v, p)) { out.emplace_back(v, p); }
    }
    return out;
}

std::string generateHelperScript(const std::string& rulesText) {
    std::string err;
    if (!validateRulesText(rulesText, err) || rulesText.empty() || rulesText.back() != '\n') { return std::string(); }
    return std::string(
               "#!/bin/sh\n"
               "# foxsdr-udev-helper - installs FoxSDR's USB permission rules. GENERATED by core/udev_rules.cpp\n"
               "# (generateHelperScript); packaging/linux/foxsdr-udev-helper is a copy and a test keeps it equal.\n"
               "# Run through polkit (pkexec) with NO arguments, as root. It reads nothing from its caller:\n"
               "# the rules below are the whole of what it will ever write, to one fixed file.\n"
               "set -eu\n"
               "PATH=/usr/sbin:/usr/bin:/sbin:/bin\n"
               "export PATH\n"
               "if [ \"$#\" -ne 0 ]; then echo \"foxsdr-udev-helper takes no arguments\" >&2; exit 2; fi\n"
               "umask 022\n"
               "dest=") + kRulesPath + "\n"
           "tmp=/etc/udev/rules.d/.99-foxsdr-sdr.rules.new\n"
           "trap 'rm -f \"$tmp\"' EXIT\n"
           "cat > \"$tmp\" <<'FOXSDR_RULES_END'\n" +
           rulesText +
           "FOXSDR_RULES_END\n"
           "chmod 0644 \"$tmp\"\n"
           "mv -f \"$tmp\" \"$dest\"\n"
           "udevadm control --reload-rules\n"
           "udevadm trigger --subsystem-match=usb --action=add\n";
}

std::string generatePolicyXml() {
    return std::string(
               "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<!DOCTYPE policyconfig PUBLIC \"-//freedesktop//DTD PolicyKit Policy Configuration 1.0//EN\"\n"
               " \"http://www.freedesktop.org/standards/PolicyKit/1/policyconfig.dtd\">\n"
               "<policyconfig>\n"
               "  <vendor>FoxSDR</vendor>\n"
               "  <action id=\"") + kPolicyActionId + "\">\n"
           "    <description>Install FoxSDR's USB radio permission rules</description>\n"
           "    <message>Authentication is required to install FoxSDR's USB radio permission rules</message>\n"
           "    <defaults>\n"
           "      <allow_any>auth_admin</allow_any>\n"
           "      <allow_inactive>auth_admin</allow_inactive>\n"
           "      <allow_active>auth_admin</allow_active>\n"
           "    </defaults>\n"
           "    <annotate key=\"org.freedesktop.policykit.exec.path\">" + kHelperPath + "</annotate>\n"
           "  </action>\n"
           "</policyconfig>\n";
}

Plan planInstall(linuxupdate::UpdateHost& host) {
    Plan p;
    p.rulesPath = kRulesPath;
    p.rulesText = generateRulesText();
    p.manualSteps = std::string("Save the text above as ") + kRulesPath +
                    " (as root, for example with sudo tee), then run: sudo udevadm control --reload-rules && "
                    "sudo udevadm trigger. Then unplug and replug the radio.";
    std::string installed;
    if (host.readText(kRulesPath, installed) && installed == p.rulesText) {
        p.state = State::AlreadyInstalled;
        p.message = "The radio permission rules are already installed.";
        return p;
    }
    if (!host.exists(kPkexecPath)) {
        p.state = State::NoPkexec;
        p.message = "polkit's pkexec is not available here, so FoxSDR cannot ask for permission itself.";
        return p;
    }
    std::string helper;
    if (!host.exists(kHelperPath) || !host.readText(kHelperPath, helper)) {
        p.state = State::NoHelper;
        p.message = "The FoxSDR helper that installs the rules is not installed (it comes with the deb and rpm "
                    "packages, not the AppImage).";
        return p;
    }
    if (helper != generateHelperScript(p.rulesText)) {
        p.state = State::HelperMismatch;
        p.message = "The installed helper is not the one this version of FoxSDR expects, so it will not be run.";
        return p;
    }
    p.state = State::Ready;
    p.message = std::string("FoxSDR will ask for your password and write ") + kRulesPath + ".";
    return p;
}

Result installRules(linuxupdate::UpdateHost& host, bool userAgreed) {
    Result r;
    if (!userAgreed) {
        r.outcome = Outcome::Declined;
        r.detail = "nothing was run";
        return r;
    }
    const Plan plan = planInstall(host);  // re-decided now, never trusted from earlier
    r.detail = plan.message;
    switch (plan.state) {
        case State::AlreadyInstalled: r.outcome = Outcome::AlreadyInstalled; return r;
        case State::NoPkexec: r.outcome = Outcome::NoPkexec; return r;
        case State::NoHelper: r.outcome = Outcome::NoHelper; return r;
        case State::HelperMismatch: r.outcome = Outcome::HelperMismatch; return r;
        case State::Ready: break;
    }
    std::string out;
    // A fixed program and a fixed, argument-free helper path. No shell, nothing the user typed.
    const int code = host.runProgram(kPkexecPath, {kHelperPath}, kPkexecTimeoutMs, out);
    if (code == 126) { r.outcome = Outcome::AuthDismissed; r.detail = "the password dialog was closed"; return r; }
    if (code == 127) { r.outcome = Outcome::NotAuthorized; r.detail = "not authorised, or no polkit agent to ask"; return r; }
    if (code == -1) { r.outcome = Outcome::Timeout; r.detail = "the helper could not be started or did not finish"; return r; }
    if (code != 0) { r.outcome = Outcome::WriteFailed; r.detail = "the helper failed (exit " + std::to_string(code) + ")"; return r; }
    std::string now;
    if (!host.readText(kRulesPath, now) || now != plan.rulesText) {
        r.outcome = Outcome::WriteFailed;
        r.detail = "the helper reported success but the rules file is not what was expected";
        return r;
    }
    r.outcome = Outcome::Installed;
    r.detail = "installed; unplug and replug the radio";
    return r;
}

}  // namespace cascade::core::udev
