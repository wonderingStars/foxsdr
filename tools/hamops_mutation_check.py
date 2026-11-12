#!/usr/bin/env python3
"""Break-it checks for the hamops lane: for each feature, remove or break one behaviour in the source, rebuild its test, expect RED, restore the
file with git (so its timestamp changes and MSBuild rebuilds it), rebuild, expect GREEN.

usage: python tools/hamops_mutation_check.py <build-dir> [name ...]     (no names: all)
Run from the worktree root. Prints one line per mutation: NAME  red=<bool> (compile|test)  restored-green=<bool>.
A mutation that does not change the source (pattern missing) is reported as ERROR, never as red.
"""
import subprocess
import sys

MUTATIONS = [
    # name, file, old, new, test target
    ("cabrillo-order", "src/core/cabrillo.hpp",
     "std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return qsos[a].unixTime < qsos[b].unixTime; });", "",
     "test_cabrillo"),
    ("macros-serial", "src/core/contest_macros.hpp", "void logged() { if (nextSerial_ < 99999) { ++nextSerial_; } }", "void logged() {}", "test_contest_macros"),
    ("adif-length", "src/core/adif_qso.hpp", "out += std::to_string(value.size());", "out += std::to_string(value.size() + 1);", "test_adif_qso"),
    ("adif-loopback", "src/net/adif_broadcast.hpp", "if (!lo::isLoopbackHost(host) && !cfg.allowNonLoopback) {", "if (false) {", "test_adif_broadcast"),
    ("score-dupe", "src/core/contest_score.hpp", "if (seen_.count(key) != 0) { ++dupes_;", "if (false) { ++dupes_;", "test_contest_score"),
    ("bandmap-ageout", "src/core/band_map.hpp", "return nowUnix - s.seenUnix > cfg_.maxAgeSec; }", "return nowUnix - s.seenUnix >= cfg_.maxAgeSec; }", "test_band_map"),
    ("hid-sign", "src/input_hid/hid_descriptor.hpp", "if (f.isSigned() && f.size < 32", "if (false && f.size < 32", "test_hid_report"),
    ("hid-forbidden", "src/input_hid/hid_map.hpp", "return l.rfind(\"transmit\", 0) == 0 || l.rfind(\"ptt\", 0) == 0;", "return false;", "test_hid_report"),
    ("xplane-latrange", "src/core/xplane_udp.hpp", "if (!(lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0)) { return false; }", "", "test_xplane_udp"),
    ("omnirig-txguard", "src/core/omnirig_client.hpp",
     "        if (port_->txState() == PM_TX) { error = \"the rig is transmitting\"; return false; }\n        if (!port_->putFreq(", "        if (!port_->putFreq(", "test_omnirig_client"),
    ("omnirig-keying-member", "src/core/omnirig_client.hpp", "    virtual bool putMode(std::uint32_t param) = 0;",
     "    virtual bool putMode(std::uint32_t param) = 0;\n    virtual bool putTx(std::uint32_t param) = 0;", "test_omnirig_client"),
    ("radioinfo-numref", "src/core/radioinfo_udp.hpp", "else { return false; }   // numeric and named references other than the five: refused",
     "else { out.push_back('?'); }", "test_radioinfo_udp"),
    ("genius-txguard", "src/core/antenna_genius.hpp", "if (p.tx) { error = \"the port is transmitting\"; return 0; }", "", "test_antenna_genius"),
    ("transcript-wordbound", "src/core/transcript_store.hpp", "while (segs_.size() > cfg_.maxSegments || words_ > cfg_.maxWords || bytes_ > cfg_.maxBytes)",
     "while (segs_.size() > cfg_.maxSegments || bytes_ > cfg_.maxBytes)", "test_transcript_store"),
    ("scan-stderr", "src/core/qso_log.hpp", "inline std::string upperAscii(std::string s) {",
     "inline void hamopsDebug() { std::fputs(\"x\", stderr); }\ninline std::string upperAscii(std::string s) {", "test_hamops_scan"),
    ("scan-dump", "src/input_hid/hid_map.hpp", "return j.dump(2, 0x20, false, nlohmann::json::error_handler_t::replace);", "return j.dump(2);", "test_hamops_scan"),
    ("xplane-send", "src/net/xplane_listener.hpp", "void stop() { lo::closeSock(sock_);", "void stop() { lo::udpSendTo(sock_, lo::kLoopbackHost, 9, \"x\", 1); lo::closeSock(sock_);", "test_xplane_udp"),
]


def run(cmd, cwd=None):
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def build_and_test(build_dir, target):
    rc, out = run(["cmake", "--build", build_dir, "--config", "Release", "--parallel", "3", "--target", target])
    if rc != 0:
        return "compile", out
    rc, out = run(["ctest", "-C", "Release", "-R", "^" + target + "$", "--timeout", "300"], cwd=build_dir)
    return ("pass" if rc == 0 else "test"), out


def main():
    build_dir = sys.argv[1]
    names = sys.argv[2:]
    for name, path, old, new, target in MUTATIONS:
        if names and name not in names:
            continue
        with open(path, encoding="utf-8", newline="") as f:
            text = f.read()
        crlf = "\r\n" in text
        probe_old = old.replace("\n", "\r\n") if crlf else old
        probe_new = new.replace("\n", "\r\n") if crlf else new
        if text.count(probe_old) != 1:
            print(f"{name:24s} ERROR pattern found {text.count(probe_old)} times in {path}")
            continue
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(text.replace(probe_old, probe_new))
        try:
            state, _ = build_and_test(build_dir, target)
        finally:
            run(["git", "checkout", "--", path])
        red = state in ("compile", "test")
        state2, out2 = build_and_test(build_dir, target)
        print(f"{name:24s} {target:24s} red={red} ({state})  restored-green={state2 == 'pass'}", flush=True)
        if state2 != "pass":
            print(out2[-800:])


if __name__ == "__main__":
    main()
