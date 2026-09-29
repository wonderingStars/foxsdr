// test_aor_source.cpp - the AOR digital-I/Q driver end to end, against a
// fake USB transport (src/usb/usb_fake.hpp) and a fake serial port.
//
// NOTHING HERE HAS TOUCHED AN AOR RECEIVER; nobody on the project has one.
// The fakes behave the way AOR's "Digital I/Q USB Interface Developer
// Information" Rev 1.1 says the receiver behaves - a running interface
// streams packed 8-byte words on iso IN 0x86 after START on bulk OUT 0x02,
// in packets of 0/512/1024/1536 bytes; the control port answers VR with a
// version containing C5700_ - and an UNPROGRAMMED interface is modelled as
// one whose isochronous endpoint cannot be armed, which is an assumption of
// this test (AOR has not said what an unprogrammed interface looks like; that
// is the open question at firmwareDecision()). Tested against a fake only.
//
// The firmware is SYNTHETIC: a four-record Intel-HEX file this test writes.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "core/config.hpp"
#include "source/aor_source.hpp"
#include "test_check.hpp"
#include "usb/usb_fake.hpp"

namespace aor = cascade::source::aor;
namespace fs = std::filesystem;
using cascade::source::AorSource;
using cascade::source::FirmwareStep;
using cascade::source::StreamProbe;
using cascade::usb::FakeIsoPacket;
using cascade::usb::FakeUsbDevice;
using cascade::usb::UsbDeviceInfo;

namespace {

// --- the serial side ---------------------------------------------------------

struct PortLog {
    std::mutex m;
    std::map<std::string, std::string> written;  // port -> every byte written, all opens
    std::map<std::string, int> opens;
};

class FakeLink final : public aor::ControlLink {
public:
    FakeLink(std::string name, std::string vrReply, std::shared_ptr<PortLog> log)
        : name_(std::move(name)), vr_(std::move(vrReply)), log_(std::move(log)) {}
    int write(const char* data, std::size_t len) override {
        {
            std::lock_guard<std::mutex> lk(log_->m);
            log_->written[name_].append(data, len);
        }
        line_.append(data, len);
        std::size_t cr;
        while ((cr = line_.find('\r')) != std::string::npos) {
            const std::string cmd = line_.substr(0, cr);
            line_.erase(0, cr + 1);
            if (cmd == "VR" && !vr_.empty()) { toRead_ += vr_ + "\r"; }
        }
        return static_cast<int>(len);
    }
    int read(char* buf, std::size_t cap) override {
        if (toRead_.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return 0;
        }
        const std::size_t n = std::min(cap, toRead_.size());
        toRead_.copy(buf, n);
        toRead_.erase(0, n);
        return static_cast<int>(n);
    }
    std::string name() const override { return name_; }

private:
    std::string name_;
    std::string vr_;
    std::shared_ptr<PortLog> log_;
    std::string line_;
    std::string toRead_;
};

// --- the I/Q side ---------------------------------------------------------------

struct Sample {
    std::int32_t i;
    std::int32_t q;
};

// The unpack formula backwards (see tests/test_aor_protocol.cpp).
void encodeHalf(std::int32_t s, bool first, std::uint8_t* b) {
    const std::uint32_t t = static_cast<std::uint32_t>(s) * 4u;
    b[0] = static_cast<std::uint8_t>(t >> 24);
    b[1] = static_cast<std::uint8_t>(((t >> 16) & 0xFEu) | (first ? 1u : 0u));
    b[2] = static_cast<std::uint8_t>((t >> 9) & 0xFFu);
    b[3] = static_cast<std::uint8_t>((t >> 2) & 0x7Eu);
}

std::vector<Sample> randomSamples(std::size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::int32_t> d(-(1 << 28), (1 << 28) - 1);
    std::vector<Sample> v(n);
    for (Sample& s : v) { s = Sample{d(rng) * 2, d(rng) * 2}; }
    return v;
}

std::vector<std::uint8_t> encode(const std::vector<Sample>& v) {
    std::vector<std::uint8_t> b(v.size() * 8);
    for (std::size_t k = 0; k < v.size(); ++k) {
        encodeHalf(v[k].i, true, b.data() + k * 8);
        encodeHalf(v[k].q, false, b.data() + k * 8 + 4);
    }
    return b;
}

// The byte stream cut into transfers of 128 packets of mixed actual lengths.
std::vector<std::vector<FakeIsoPacket>> transfersOf(const std::vector<std::uint8_t>& bytes,
                                                    unsigned seed) {
    std::mt19937 rng(seed);
    const std::size_t lens[4] = {0, 512, 1024, 1536};
    std::vector<std::vector<FakeIsoPacket>> out;
    std::size_t at = 0;
    while (at < bytes.size()) {
        std::vector<FakeIsoPacket> t(128);
        for (FakeIsoPacket& p : t) {
            const std::size_t n = std::min(lens[rng() % 4], bytes.size() - at);
            p.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                             bytes.begin() + static_cast<std::ptrdiff_t>(at + n));
            at += n;
        }
        out.push_back(std::move(t));
    }
    return out;
}

bool isStart(const FakeUsbDevice::BulkWrite& w) {
    return w.endpoint == 0x02 && w.data == std::vector<std::uint8_t>(aor::kStartCommand, aor::kStartCommand + 6);
}
bool isStop(const FakeUsbDevice::BulkWrite& w) {
    return w.endpoint == 0x02 && w.data == std::vector<std::uint8_t>(aor::kStopCommand, aor::kStopCommand + 6);
}

const char* kStartLine = "bulk-out 02 [5A A5 00 02 41 53]";
const char* kStopLine = "bulk-out 02 [5A A5 00 02 41 45]";

// A running interface: on START it delivers a startup transfer (junk the
// driver must discard) followed by `payload`; on STOP it delivers nothing
// more. Each START delivers the same thing again.
void makeRunning(FakeUsbDevice& dev, std::vector<std::vector<FakeIsoPacket>> payload) {
    FakeUsbDevice* self = &dev;
    dev.onBulkWrite = [self, payload](const FakeUsbDevice::BulkWrite& w) {
        if (isStart(w)) {
            // The startup transfer: marker-valid words whose samples are all
            // 0x7F7F.. - if the driver let these through, the comparison
            // below would see them.
            std::vector<Sample> junk(300, Sample{0x1FFFFFF0, 0x1FFFFFF0});
            std::vector<FakeIsoPacket> first(128);
            first[3].payload = encode(junk);
            first[3].payload.resize(1536);
            self->feedIso(first);
            for (const auto& t : payload) { self->feedIso(t); }
        } else if (isStop(w)) {
            self->clearIso();
        }
    };
}

// The whole fake world one test opens against.
struct World {
    std::shared_ptr<std::vector<std::string>> journal = std::make_shared<std::vector<std::string>>();
    std::shared_ptr<PortLog> ports = std::make_shared<PortLog>();
    std::vector<UsbDeviceInfo> listed;
    std::vector<UsbDeviceInfo> unbound;
    std::map<std::string, std::string> vr;  // port -> VR reply
    std::vector<std::string> candidates;
    // Built on each open of a path; the source owns it, the test keeps a peek.
    std::function<void(FakeUsbDevice&, const std::string& path)> build;
    std::vector<FakeUsbDevice*> opened;
    std::function<void()> afterList;  // lets a test move `listed` as time passes
    std::string firmware = "/nonexistent/foxsdr-test/fx2fw.hex";

    AorSource::TestTransport transport() {
        AorSource::TestTransport t;
        t.list = [this]() {
            if (afterList) { afterList(); }
            return listed;
        };
        t.unbound = [this]() { return unbound; };
        t.open = [this](const std::string& path, std::string&) -> std::unique_ptr<cascade::usb::UsbDevice> {
            auto d = std::make_unique<FakeUsbDevice>();
            d->journal = journal;
            d->setPath(path);
            d->note("open " + path);
            if (build) { build(*d, path); }
            opened.push_back(d.get());
            return d;
        };
        t.controlPorts = [this]() { return candidates; };
        t.openControl = [this](const std::string& port, std::string& error) -> std::unique_ptr<aor::ControlLink> {
            if (vr.find(port) == vr.end()) {
                error = "no such port";
                return nullptr;
            }
            {
                std::lock_guard<std::mutex> lk(ports->m);
                ++ports->opens[port];
            }
            return std::make_unique<FakeLink>(port, vr[port], ports);
        };
        t.firmwarePath = firmware;
        t.sleep = [](std::chrono::milliseconds) {};
        return t;
    }

    long indexOf(const std::string& line, std::size_t from = 0) const {
        for (std::size_t k = from; k < journal->size(); ++k) {
            if ((*journal)[k] == line) { return static_cast<long>(k); }
        }
        return -1;
    }
    long lastIndexOf(const std::string& line) const {
        for (std::size_t k = journal->size(); k > 0; --k) {
            if ((*journal)[k - 1] == line) { return static_cast<long>(k - 1); }
        }
        return -1;
    }
    std::size_t count(const std::string& prefix) const {
        std::size_t n = 0;
        for (const std::string& l : *journal) { n += l.rfind(prefix, 0) == 0 ? 1 : 0; }
        return n;
    }
    void dump() const {
        for (const std::string& l : *journal) { std::printf("    | %s\n", l.c_str()); }
    }
};

UsbDeviceInfo iqDevice(const std::string& path) {
    UsbDeviceInfo d;
    d.vid = 0x08D0;
    d.pid = 0xA001;
    d.path = path;
    d.description = "DIGI-RECEIVER";
    return d;
}

std::string record(std::uint8_t type, std::uint16_t addr, const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> r = {static_cast<std::uint8_t>(data.size()),
                                   static_cast<std::uint8_t>(addr >> 8),
                                   static_cast<std::uint8_t>(addr & 0xFF), type};
    r.insert(r.end(), data.begin(), data.end());
    unsigned sum = 0;
    for (std::uint8_t b : r) { sum += b; }
    r.push_back(static_cast<std::uint8_t>((0x100u - (sum & 0xFFu)) & 0xFFu));
    std::string s = ":";
    for (std::uint8_t b : r) {
        char h[3];
        std::snprintf(h, sizeof(h), "%02X", b);
        s += h;
    }
    return s + "\n";
}

std::string writeSyntheticFirmware() {
    const fs::path p = fs::temp_directory_path() / "foxsdr_test_aor_fx2fw.hex";
    std::ofstream f(p, std::ios::binary);
    f << record(0x00, 0x0000, {0x02, 0x00, 0x10, 0x00});
    f << record(0x00, 0x0010, {0x75, 0x81, 0x40, 0x80, 0xFE});
    f << record(0x00, 0x0100, {0xDE, 0xAD, 0xBE, 0xEF});
    f << record(0x01, 0x0000, {});
    return p.string();
}

bool contains(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

// Reads until `want` samples have come out or the bound runs out.
std::vector<std::complex<float>> drain(AorSource& src, std::size_t want) {
    std::vector<std::complex<float>> got;
    std::vector<std::complex<float>> buf(4096);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (got.size() < want && std::chrono::steady_clock::now() < deadline) {
        const std::size_t n = src.read(buf.data(), buf.size());
        got.insert(got.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
    }
    return got;
}

}  // namespace

int main() {
    // --- the decision, on its own -------------------------------------------------
    {
        StreamProbe p;
        CHECK(cascade::source::firmwareDecision(p) == FirmwareStep::LoadFirmware);
        p.isoArmed = true;
        p.startAccepted = true;
        CHECK(cascade::source::firmwareDecision(p) == FirmwareStep::LoadFirmware);
        p.aligned = true;
        CHECK(cascade::source::firmwareDecision(p) == FirmwareStep::Stream);
        p.firmwareLoadedThisOpen = true;
        CHECK(cascade::source::firmwareDecision(p) == FirmwareStep::Stream);
        p.aligned = false;
        CHECK(cascade::source::firmwareDecision(p) == FirmwareStep::GiveUp);
        // Aligned without START accepted is not trusted.
        StreamProbe q;
        q.isoArmed = true;
        q.aligned = true;
        CHECK(cascade::source::firmwareDecision(q) == FirmwareStep::LoadFirmware);
    }

    // --- enumeration, and the things there are only one of -------------------------
    {
        std::vector<UsbDeviceInfo> devs = {iqDevice("a"), iqDevice("b")};
        devs[0].serial = "A104RHPT";
        UsbDeviceInfo other = iqDevice("c");
        other.pid = 0x6001;
        devs.push_back(other);
        const auto rows = cascade::source::aorDevicesFrom(devs);
        CHECK(rows.size() == 2);
        CHECK(rows.size() == 2 && rows[0].driver == "aor" && rows[0].args == "serial=A104RHPT");
        CHECK(rows.size() == 2 && rows[1].args == "index=1");
        CHECK(cascade::source::aorUsbIds().size() == 1);
        CHECK(contains(cascade::source::aorFirmwarePath(), "resources"));
        CHECK(contains(cascade::source::aorFirmwarePath(), "fx2fw.hex"));
        AorSource src;
        CHECK(src.supportedSampleRatesHz() == std::vector<double>{1125000.0});
        CHECK(src.gains().empty());
        double lo = 0, hi = 0;
        CHECK(!src.frequencyRangeHz(lo, hi));
        // The config keeps "aor" as a source kind rather than resetting it.
        const fs::path cfg = fs::temp_directory_path() / "foxsdr_test_aor_config.json";
        std::ofstream(cfg) << "{\"schemaVersion\":1,\"sourceKind\":\"aor\","
                              "\"nativeArgs\":\"index=0\"}\n";
        cascade::core::AppConfig out;
        std::string err;
        CHECK(cascade::core::ConfigStore::load(cfg.string(), out, err));
        CHECK(out.sourceKind == "aor");
        fs::remove(cfg);
    }

    const std::vector<Sample> stream = randomSamples(60000, 77);
    const auto payload = transfersOf(encode(stream), 5);

    // --- open, START after arming, samples out, STOP before cancel, close ------------
    {
        World w;
        w.listed = {iqDevice("fake://aor/1")};
        w.candidates = {"COM3", "COM6"};
        w.vr = {{"COM3", "$GPRMC,1"}, {"COM6", "C5700_V1.02"}};
        w.build = [&](FakeUsbDevice& d, const std::string&) { makeRunning(d, payload); };
        AorSource src;
        src.setTransportForTest(w.transport());
        CHECK(src.open(""));
        if (!src.isOpen()) { std::printf("  open failed: %s\n", src.lastError()); }
        CHECK(!src.firmwareWasLoaded());
        CHECK(src.controlPort() == "COM6");
        CHECK(src.identity().model == aor::Model::AR5700D);
        CHECK(std::string(src.name()) == "AOR AR5700D");
        CHECK(src.sampleRateHz() == 1125000.0);
        // The control start-up went to the receiver and only EX/VR to the GPS.
        CHECK(w.ports->written["COM3"] == "EX\rVR\r");
        CHECK(w.ports->written["COM6"] == "EX\rVR\rEX\rVR\r@21\rVFA\rRF0100.000000\r");
        // The probe: armed, START, then STOP before the transfers were cancelled.
        const long probeBegin = w.indexOf("iso-begin 86");
        const long probeStart = w.indexOf(kStartLine);
        const long probeStop = w.indexOf(kStopLine);
        const long probeEnd = w.indexOf("iso-end");
        CHECK(probeBegin >= 0 && probeBegin < probeStart && probeStart < probeStop &&
              probeStop < probeEnd);
        CHECK(w.count("control") == 0);  // no firmware traffic at all

        w.journal->push_back("-- start --");
        const long mark = w.lastIndexOf("-- start --");
        CHECK(src.start());
        CHECK(src.running());
        const long armed = w.indexOf("iso-begin 86", static_cast<std::size_t>(mark));
        const long started = w.indexOf(kStartLine, static_cast<std::size_t>(mark));
        CHECK(armed > mark);
        CHECK(started > armed);  // START only after the transfers are armed

        const auto got = drain(src, stream.size());
        CHECK(got.size() == stream.size());
        bool same = got.size() == stream.size();
        for (std::size_t k = 0; same && k < got.size(); ++k) {
            same = got[k] == std::complex<float>(static_cast<float>(stream[k].i / 1073741824.0),
                                                 static_cast<float>(stream[k].q / 1073741824.0));
            if (!same) { std::printf("  sample %zu differs\n", k); }
        }
        CHECK(same);
        CHECK(src.transfersDiscarded() == 1);
        CHECK(src.alignmentLosses() == 0);

        src.stop();
        CHECK(!src.running());
        const long stopped = w.indexOf(kStopLine, static_cast<std::size_t>(started));
        const long cancelled = w.indexOf("iso-end", static_cast<std::size_t>(started));
        CHECK(stopped > started);
        CHECK(cancelled > stopped);  // STOP before the transfers are cancelled

        // Tuning goes to the control port, formatted as the document shows.
        CHECK(src.setCenterFrequencyHz(81.3e6));
        CHECK(src.centerFrequencyHz() == 81.3e6);
        CHECK(contains(w.ports->written["COM6"], "RF0081.300000\r"));
        CHECK(!src.setCenterFrequencyHz(12.0e9));
        CHECK(src.centerFrequencyHz() == 81.3e6);
        // The rate cannot move.
        CHECK(src.setSampleRateHz(2.4e6));
        CHECK(src.sampleRateHz() == 1125000.0);

        src.closeDevice();
        CHECK(!src.isOpen());
        CHECK(w.journal->back() == "closed");  // interface released on close
        // Restartable after a stop, from the same open... (re-opened here)
        std::printf("  journal (first open):\n");
        w.dump();
    }

    // --- the firmware file is missing: said in words, device not opened -------------
    {
        World w;
        w.listed = {iqDevice("fake://aor/1")};
        w.candidates = {"COM6"};
        w.vr = {{"COM6", "C5700_V1.02"}};
        w.build = [](FakeUsbDevice& d, const std::string&) { d.failBeginIsoAfter = 0; };
        AorSource src;
        src.setTransportForTest(w.transport());
        CHECK(!src.open(""));
        CHECK(!src.isOpen());
        const std::string err = src.lastError();
        std::printf("  firmware missing: %s\n", err.c_str());
        CHECK(contains(err, "the firmware file is not installed"));
        CHECK(contains(err, "/nonexistent/foxsdr-test/fx2fw.hex"));
        CHECK(contains(err, "The receiver was not opened"));
        CHECK(w.count("control") == 0);          // nothing written into the FX2
        CHECK(w.journal->back() == "closed");    // and the interface released
        CHECK(w.ports->written["COM6"] == "EX\rVR\r");  // no start-up on the control port
    }

    // --- an unprogrammed interface: firmware loaded, device waited for, then used ------
    {
        World w;
        w.firmware = writeSyntheticFirmware();
        w.listed = {iqDevice("fake://aor/boot")};
        w.candidates = {"COM6"};
        w.vr = {{"COM6", "C5700_V1.02"}};
        bool released = false;
        int polls = 0;
        w.build = [&](FakeUsbDevice& d, const std::string& path) {
            if (path == "fake://aor/boot") {
                d.failBeginIsoAfter = 0;  // no isochronous endpoint yet
                d.onControlOut = [&](const cascade::usb::FakeControl& c) {
                    if (c.value == 0xE600 && c.data == std::vector<std::uint8_t>{0x00}) { released = true; }
                };
            } else {
                makeRunning(d, payload);
            }
        };
        // After the release: gone for two polls, then back at a new path.
        w.afterList = [&]() {
            if (!released) { return; }
            ++polls;
            if (polls <= 2) {
                w.listed.clear();
            } else {
                w.listed = {iqDevice("fake://aor/running")};
            }
        };
        AorSource src;
        src.setTransportForTest(w.transport());
        CHECK(src.open(""));
        if (!src.isOpen()) { std::printf("  open failed: %s\n", src.lastError()); }
        CHECK(src.firmwareWasLoaded());
        // The FX2 transcript: hold, the image in address order, release.
        std::vector<std::string> fx2;
        for (const std::string& l : *w.journal) {
            if (l.rfind("control ", 0) == 0) { fx2.push_back(l.substr(8)); }
        }
        const std::vector<std::string> wantFx2 = {
            "OUT 40/A0 v=E600 i=0000 [01]",
            "OUT 40/A0 v=0000 i=0000 [02 00 10 00]",
            "OUT 40/A0 v=0010 i=0000 [75 81 40 80 FE]",
            "OUT 40/A0 v=0100 i=0000 [DE AD BE EF]",
            "OUT 40/A0 v=E600 i=0000 [00]",
        };
        CHECK(fx2 == wantFx2);
        // The unprogrammed handle was released BEFORE the running one opened.
        const long closedBoot = w.indexOf("closed");
        const long openedRunning = w.indexOf("open fake://aor/running");
        CHECK(closedBoot >= 0 && openedRunning > closedBoot);
        CHECK(src.start());
        const auto got = drain(src, stream.size());
        CHECK(got.size() == stream.size());
        src.closeDevice();
        std::printf("  journal (firmware load):\n");
        w.dump();
    }

    // --- firmware loaded and still no stream: one load, then words ------------------------
    {
        World w;
        w.firmware = writeSyntheticFirmware();
        w.listed = {iqDevice("fake://aor/boot")};
        w.candidates = {"COM6"};
        w.vr = {{"COM6", "C5700_V1.02"}};
        w.build = [](FakeUsbDevice& d, const std::string&) { d.failBeginIsoAfter = 0; };
        AorSource src;
        src.setTransportForTest(w.transport());
        CHECK(!src.open(""));
        std::printf("  still silent: %s\n", src.lastError());
        CHECK(contains(src.lastError(), "did not start streaming, even after its firmware was loaded"));
        CHECK(w.count("control OUT 40/A0 v=E600 i=0000 [01]") == 1);  // loaded exactly once
    }

    // --- the device never comes back after the load -------------------------------------
    {
        World w;
        w.firmware = writeSyntheticFirmware();
        w.listed = {iqDevice("fake://aor/boot")};
        w.candidates = {"COM6"};
        w.vr = {{"COM6", "C5700_V1.02"}};
        bool released = false;
        w.build = [&](FakeUsbDevice& d, const std::string&) {
            d.failBeginIsoAfter = 0;
            d.onControlOut = [&](const cascade::usb::FakeControl& c) {
                if (c.value == 0xE600 && c.data == std::vector<std::uint8_t>{0x00}) { released = true; }
            };
        };
        w.afterList = [&]() { if (released) { w.listed.clear(); } };
        AorSource src;
        src.setTransportForTest(w.transport());
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!src.open(""));
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        CHECK(contains(src.lastError(), "did not come back within 5 seconds"));
        CHECK(ms < 3000);  // the injected sleep: bounded by poll count, not wall time
    }

    // --- present but not bound to WinUSB -------------------------------------------------
    {
        World w;
        w.unbound = {iqDevice("")};
        AorSource src;
        src.setTransportForTest(w.transport());
        CHECK(!src.open(""));
        const std::string err = src.lastError();
        std::printf("  not bound: %s\n", err.c_str());
        CHECK(contains(err, "is plugged in but is not bound to WinUSB"));
        CHECK(contains(err, "AorAlpha"));
        CHECK(contains(err, "Zadig"));
        CHECK(w.opened.empty());  // nothing was opened
        const std::string advice = cascade::source::aorUnboundAdvice("DIGI-RECEIVER");
        CHECK(contains(advice, "DIGI-RECEIVER is plugged in but is not bound to WinUSB"));
        CHECK(contains(advice, "08D0:A001"));
        CHECK(contains(advice, "AorAlpha"));
    }
    // --- not there at all ------------------------------------------------------------------
    {
        World w;
        AorSource src;
        src.setTransportForTest(w.transport());
        CHECK(!src.open(""));
        CHECK(contains(src.lastError(), "No AOR I/Q interface (USB 08D0:A001) was found"));
    }
    // --- no receiver on the control ports: refused before anything is sent to the FX2 ------
    {
        World w;
        w.listed = {iqDevice("fake://aor/1")};
        w.candidates = {"COM3"};
        w.vr = {{"COM3", "$GPRMC,1"}};
        w.build = [&](FakeUsbDevice& d, const std::string&) { makeRunning(d, payload); };
        AorSource src;
        src.setTransportForTest(w.transport());
        CHECK(!src.open(""));
        CHECK(contains(src.lastError(), "No serial port answered as an AOR receiver"));
        CHECK(w.indexOf(kStartLine) < 0);
        CHECK(w.journal->back() == "closed");
        // A port named in the args that is not a receiver is refused by name.
        AorSource src2;
        src2.setTransportForTest(w.transport());
        CHECK(!src2.open("control=COM3"));
        CHECK(contains(src2.lastError(), "COM3 is not an AOR receiver"));
    }
    // --- the interface dies mid-stream: faulted, not hung ----------------------------------
    {
        World w;
        w.listed = {iqDevice("fake://aor/1")};
        w.candidates = {"COM6"};
        w.vr = {{"COM6", "C5700_V1.02"}};
        w.build = [&](FakeUsbDevice& d, const std::string&) { makeRunning(d, payload); };
        AorSource src;
        src.setTransportForTest(w.transport());
        CHECK(src.open(""));
        FakeUsbDevice* dev = w.opened.empty() ? nullptr : w.opened.back();
        if (dev != nullptr) { dev->failIsoAfter = dev->isoReads + 5; }
        CHECK(src.start());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        std::vector<std::complex<float>> buf(1024);
        while (!src.faulted() && std::chrono::steady_clock::now() < deadline) { src.read(buf.data(), buf.size()); }
        CHECK(src.faulted());
        CHECK(src.deviceDead());
        CHECK(src.faultedWhile() == "reading samples");
        const auto t0 = std::chrono::steady_clock::now();
        src.closeDevice();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        CHECK(ms < AorSource::kReaderJoinWait.count());
        CHECK(w.journal->back() == "closed");
    }

    return testSummary("test_aor_source");
}
