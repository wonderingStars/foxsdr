// test_aor_control.cpp - AOR receiver control over the serial port: the
// documented start-up sequence byte for byte, the RF command's formatting,
// identification of a VR reply, and pairing a port by what it answers.
//
// WHERE THE EXPECTATIONS COME FROM: AOR's "Digital I/Q USB Interface
// Developer Information" Rev 1.1, "Receiver control" - EX, VR, @21, VFA, CR
// terminated, and RF0081.300000 for 81.3 MHz. The fake below is a scripted
// receiver (and, for the refusals, a scripted GPS puck and a silent port).
// Tested against a fake; not tested on hardware.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "source/aor_control.hpp"
#include "test_check.hpp"

namespace aor = cascade::source::aor;
namespace fs = std::filesystem;

namespace {

// A serial port with something (or nothing) on the other end. Each complete
// CR-terminated command written to it is looked up in `answers`; the answer,
// if any, is queued for reading.
class FakeLink final : public aor::ControlLink {
public:
    explicit FakeLink(std::string name) : name_(std::move(name)) {}
    std::map<std::string, std::string> answers;  // command (no CR) -> bytes sent back
    bool echo = false;                           // echo each command line back first
    std::string written;                         // every byte ever written
    bool failWrites = false;

    int write(const char* data, std::size_t len) override {
        if (failWrites) { return -1; }
        written.append(data, len);
        line_.append(data, len);
        std::size_t cr;
        while ((cr = line_.find('\r')) != std::string::npos) {
            const std::string cmd = line_.substr(0, cr);
            line_.erase(0, cr + 1);
            if (echo) { toRead_ += cmd + "\r\n"; }
            const auto it = answers.find(cmd);
            if (it != answers.end()) { toRead_ += it->second; }
        }
        return static_cast<int>(len);
    }
    int read(char* buf, std::size_t cap) override {
        if (toRead_.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return 0;
        }
        // A few bytes at a time, as a UART delivers them.
        const std::size_t n = std::min<std::size_t>({cap, toRead_.size(), 3});
        toRead_.copy(buf, n);
        toRead_.erase(0, n);
        return static_cast<int>(n);
    }
    std::string name() const override { return name_; }

private:
    std::string name_;
    std::string line_;
    std::string toRead_;
};

const char* kAr5700Reply = "C5700_V1.02\r";

bool contains(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

}  // namespace

int main() {
    // --- RF formatting ------------------------------------------------------
    {
        struct Case {
            double hz;
            const char* want;
        };
        const Case cases[] = {
            {81.3e6, "RF0081.300000"},          // the AOR document's own example
            {145.8e6, "RF0145.800000"},         // not exact in binary
            {5000.0e6, "RF5000.000000"},        // the 5 GHz boundary itself
            {4999999999.0, "RF4999.999999"},    // 1 Hz below it
            {5000000001.0, "RF5000.000001"},    // 1 Hz above it
            {500e3, "RF0000.500000"},           // sub-MHz
            {123.0, "RF0000.000123"},           // sub-kHz digits
            {9.0e3, "RF0000.009000"},
            {1.5, "RF0000.000002"},             // rounded to whole Hz
            {0.4, "RF0000.000000"},
            {0.0, "RF0000.000000"},
            {1234567890.0, "RF1234.567890"},
            {9999999999.0, "RF9999.999999"},    // the field's ceiling
        };
        for (const Case& c : cases) {
            std::string out;
            const bool ok = aor::formatTuneCommand(c.hz, out);
            if (!ok || out != c.want) { std::printf("  %.3f Hz -> \"%s\", want %s\n", c.hz, out.c_str(), c.want); }
            CHECK(ok && out == c.want);
        }
        std::string out = "untouched";
        CHECK(!aor::formatTuneCommand(10.0e9, out));
        CHECK(!aor::formatTuneCommand(-1.0, out));
        CHECK(!aor::formatTuneCommand(std::nan(""), out));
        CHECK(!aor::formatTuneCommand(std::numeric_limits<double>::infinity(), out));
        CHECK(out == "untouched");
    }

    // --- identifying a VR reply ------------------------------------------------
    {
        aor::Identity id = aor::identifyVr("  C5700_V1.02 \r\n");
        CHECK(id.isAor);
        CHECK(id.model == aor::Model::AR5700D);
        CHECK(id.verified);
        CHECK(id.sendCompatMode);
        CHECK(id.reply == "C5700_V1.02");
        id = aor::identifyVr("VR C2300_1.1");
        CHECK(id.isAor && id.model == aor::Model::AR2300 && !id.verified && !id.sendCompatMode);
        CHECK(aor::identifyVr("C5001_x").model == aor::Model::AR5001D);
        CHECK(aor::identifyVr("C6000_x").model == aor::Model::AR6000);
        CHECK(!aor::identifyVr("$GPRMC,123519,A,4807.038,N").isAor);
        CHECK(!aor::identifyVr("").isAor);
        CHECK(!aor::identifyVr("C5700").isAor);  // the documented token includes the underscore
        CHECK(!aor::identifyVr("AR5700D").isAor);
    }

    // --- the documented start-up, byte for byte --------------------------------
    {
        FakeLink link("COM7");
        link.answers["VR"] = kAr5700Reply;
        aor::ControlSession s(link);
        aor::Identity id;
        std::string error;
        CHECK(s.initialise(id, error));
        CHECK(error.empty());
        CHECK(link.written == "EX\rVR\r@21\rVFA\r");
        CHECK(id.model == aor::Model::AR5700D);
        CHECK(s.tune(81.3e6, error));
        CHECK(link.written == "EX\rVR\r@21\rVFA\rRF0081.300000\r");
    }
    // A receiver that echoes commands and answers with CRLF, EX with text.
    {
        FakeLink link("COM7");
        link.echo = true;
        link.answers["EX"] = "OK\r\n";
        link.answers["VR"] = "C5700_V1.02\r\n";
        aor::ControlSession s(link);
        aor::Identity id;
        std::string error;
        CHECK(s.initialise(id, error));
        CHECK(id.isAor);
        CHECK(link.written == "EX\rVR\r@21\rVFA\r");
    }
    // An unverified model: no @21 (documented for the AR5700D only).
    {
        FakeLink link("COM7");
        link.answers["VR"] = "C2300_V3\r";
        aor::ControlSession s(link);
        aor::Identity id;
        std::string error;
        CHECK(s.initialise(id, error));
        CHECK(!id.verified);
        CHECK(link.written == "EX\rVR\rVFA\r");
    }

    // --- refusing a port whose VR reply is not AOR ----------------------------
    {
        FakeLink link("COM3");
        link.answers["VR"] = "$GPGGA,092750.000,5321.6802,N\r\n";
        aor::ControlSession s(link);
        aor::Identity id;
        std::string error;
        CHECK(!s.initialise(id, error));
        CHECK(!id.isAor);
        // Nothing after VR went to it: no @21, no VFA, no RF.
        CHECK(link.written == "EX\rVR\r");
        CHECK(contains(error, "COM3 is not an AOR receiver"));
        CHECK(contains(error, "$GPGGA"));
        std::printf("refusal: %s\n", error.c_str());
    }
    {
        FakeLink link("COM4");  // silent
        aor::ControlSession s(link);
        aor::Identity id;
        std::string error;
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!s.initialise(id, error));
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        CHECK(link.written == "EX\rVR\r");
        CHECK(contains(error, "did not answer VR"));
        // Bounded: two reply waits and a little.
        CHECK(ms < 2 * aor::kReplyWait.count() + 300);
    }
    {
        FakeLink link("COM5");
        link.failWrites = true;
        aor::ControlSession s(link);
        aor::Identity id;
        std::string error;
        CHECK(!s.initialise(id, error));
        CHECK(contains(error, "stopped taking commands"));
    }

    // --- pairing by what each port answers --------------------------------------
    {
        std::vector<FakeLink*> opened;
        const auto opener = [&](const std::string& port, std::string& error)
            -> std::unique_ptr<aor::ControlLink> {
            if (port == "COM9") {
                error = "in use by another program";
                return nullptr;
            }
            auto l = std::make_unique<FakeLink>(port);
            if (port == "COM3") { l->answers["VR"] = "$GPRMC,1\r"; }   // a GPS on an FTDI cable
            if (port == "COM6") { l->answers["VR"] = kAr5700Reply; }  // the receiver
            if (port == "COM8") { l->answers["VR"] = kAr5700Reply; }  // a second receiver
            opened.push_back(l.get());
            return l;
        };
        // One receiver among a GPS, a silent port and a busy one.
        aor::Pairing p = aor::pairControlPort({"COM3", "COM4", "COM6", "COM9"}, "", opener);
        CHECK(p.ok);
        CHECK(p.port == "COM6");
        CHECK(p.identity.model == aor::Model::AR5700D);
        CHECK(p.tried.size() == 4);
        for (const std::string& t : p.tried) { std::printf("  tried %s\n", t.c_str()); }
        // Only EX and VR ever went to the ports that are not the receiver.
        CHECK(opened.size() == 3);
        for (FakeLink* l : opened) { CHECK(l->written == "EX\rVR\r"); }

        // Two receivers: refused, not guessed.
        p = aor::pairControlPort({"COM6", "COM8"}, "", opener);
        CHECK(!p.ok);
        CHECK(contains(p.error, "More than one AOR receiver answered (COM6, COM8)"));
        // None.
        p = aor::pairControlPort({"COM3", "COM4"}, "", opener);
        CHECK(!p.ok);
        CHECK(contains(p.error, "No serial port answered as an AOR receiver (2 FTDI port(s) asked)"));
        p = aor::pairControlPort({}, "", opener);
        CHECK(!p.ok);
        CHECK(contains(p.error, "No FTDI serial port was found"));
        // A named port that is not a receiver is refused with its answer.
        p = aor::pairControlPort({"COM6"}, "COM3", opener);
        CHECK(!p.ok);
        CHECK(contains(p.error, "COM3 is not an AOR receiver"));
        // A named port that is: only that one is asked.
        opened.clear();
        p = aor::pairControlPort({"COM3", "COM4"}, "COM6", opener);
        CHECK(p.ok && p.port == "COM6");
        CHECK(opened.size() == 1);
    }

    // --- FTDI candidates from a sysfs fixture ----------------------------------
    // Linux-only: ftdiPortsFromSysfs is a stub on Windows (the real body is
    // #ifdef'd out there), and building this fixture needs fs::create_symlink,
    // which fails on Windows without SeCreateSymbolicLinkPrivilege - it
    // crashed the whole test with an uncaught filesystem_error (ctest's
    // generic fail-fast exit 0xC0000409) on an ordinary, non-elevated account.
#ifndef _WIN32
    {
        const fs::path root = fs::temp_directory_path() / "foxsdr_test_aor_sysfs";
        fs::remove_all(root);
        const auto device = [&](const std::string& dev, const char* vid, const char* pid) {
            fs::create_directories(root / "devices" / dev / (dev + ":1.0"));
            std::ofstream(root / "devices" / dev / "idVendor") << vid << "\n";
            std::ofstream(root / "devices" / dev / "idProduct") << pid << "\n";
        };
        const auto tty = [&](const std::string& name, const std::string& dev) {
            fs::create_directories(root / "class" / "tty" / name);
            fs::create_symlink(root / "devices" / dev / (dev + ":1.0"),
                               root / "class" / "tty" / name / "device");
        };
        device("1-1", "0403", "6001");  // FTDI FT232R: a candidate
        device("1-2", "067b", "2303");  // Prolific: not
        device("1-3", "0403", "6015");  // FTDI, another chip: not
        device("1-4", "0403", "6001");
        tty("ttyUSB0", "1-1");
        tty("ttyUSB1", "1-2");
        tty("ttyUSB2", "1-3");
        tty("ttyUSB10", "1-4");
        fs::create_directories(root / "class" / "tty" / "ttyS0");  // not USB at all
        const std::vector<std::string> got =
            aor::ftdiPortsFromSysfs((root / "class" / "tty").string(), "/dev");
        CHECK(got.size() == 2);
        CHECK(got.size() == 2 && got[0] == "/dev/ttyUSB0" && got[1] == "/dev/ttyUSB10");
        CHECK(aor::ftdiPortsFromSysfs((root / "nowhere").string(), "/dev").empty());
        fs::remove_all(root);
    }
#endif

    return testSummary("test_aor_control");
}
