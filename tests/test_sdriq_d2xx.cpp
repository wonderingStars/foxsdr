// test_sdriq_d2xx.cpp - the RFspace SDR-IQ link over FTDI's D2XX library (source/sdriq_d2xx), proven against a STAND-IN ftd2xx.dll that this project builds
// from FTDI's published programmer's guide (tests/fixtures/ftd2xx_standin.cpp), and the finished SdriqSource on top of it.
//
// What it proves: what the link and the opener do with a library that behaves as the guide says and with ones that do not (no library, not a library, a
// 32-bit one, one that exports too little, one whose dependency is missing, no devices, other FTDI devices only, a refused open, a refused timeout, a
// driver that writes past the buffer it was given, a read that reports more than it was asked, a read that fails, a cable pulled mid-stream, a write that
// writes nothing or three bytes at a time, a read that faults, a read that never returns), that a D2XX read timeout of 0 ("wait forever") is never
// passed, that nothing is written to an FTDI device that does not call itself an SDR-IQ unless its serial was named, and that the source runs, tunes and
// streams a tone found where the sample-clock error puts it. It cannot prove that the guide was read rightly, and no real SDR-IQ and no real FTDI driver
// was touched (UNVERIFIED ON HARDWARE).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "eol_fixture.hpp"
#include "source/sdriq_d2xx.hpp"
#include "source/sdriq_source.hpp"
#include "source/vendor_lib.hpp"
#include "test_check.hpp"

using namespace cascade::source;
namespace vl = cascade::source::vendorlib;
namespace as = cascade::source::ascp;
namespace sq = cascade::source::sdriq;
using Bytes = std::vector<std::uint8_t>;

namespace {

enum Key : int {
    kMisbehave = 1, kNumDevices = 2, kSdrIqIndex = 3, kLostAfterBlocks = 4, kToneHzX10 = 5, kPpmX10 = 6, kOpens = 16, kCloses = 17, kReads = 18,
    kWrites = 19, kZeroTimeouts = 20, kLastReadTimeout = 21, kWritesToOther = 22, kForbiddenItems = 23, kBlocks = 24, kRunning = 25,
    kBytesWritten = 26, kCalls = 27, kOpenHandles = 28, kMaxRead = 29,
};
enum : std::int64_t {
    mOverrun = 1 << 0, mOpenFails = 1 << 1, mReadCrashes = 1 << 2, mReadHangs = 1 << 3, mReadLies = 1 << 4, mReadFails = 1 << 5, mWriteFails = 1 << 6,
    mWriteZero = 1 << 7, mSilent = 1 << 8, mSetTimeoutsFails = 1 << 9, mListFails = 1 << 10, mNoSerialList = 1 << 11, mUnterminated = 1 << 12,
    mWriteShort = 1 << 13, mBadName = 1 << 14,
};

struct Fx {
    vl::Library lib;
    void (*set)(int, std::int64_t) = nullptr;
    std::int64_t (*get)(int) = nullptr;
    void (*reset)() = nullptr;
    std::string dir;
    bool load() {
        dir = eol::u8(eol::fixtureDir());
        if (!lib.open(dir + "\\ftd2xx.dll").ok()) { return false; }
        set = reinterpret_cast<void (*)(int, std::int64_t)>(lib.symbol("eol_set"));
        get = reinterpret_cast<std::int64_t (*)(int)>(lib.symbol("eol_get"));
        reset = reinterpret_cast<void (*)()>(lib.symbol("eol_reset"));
        return set != nullptr && get != nullptr && reset != nullptr;
    }
};
Fx g_fx;

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

void put16(Bytes& b, std::size_t at, std::uint32_t v) {
    b[at] = static_cast<std::uint8_t>(v);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(Bytes& b, std::size_t at, std::uint32_t v) {
    put16(b, at, v & 0xFFFF);
    put16(b, at + 2, v >> 16);
}
// A well-formed PE image (the same builder as test_vendor_lib.cpp), here a 32-bit DLL.
Bytes makeImage(std::uint32_t machine, std::uint32_t magic) {
    const std::size_t pe = 0x80;
    const std::size_t optSize = magic == 0x20B ? 240 : 224;
    const std::size_t table = pe + 24 + optSize;
    const std::size_t dataAt = (table + 40 + 0x1FF) & ~std::size_t{0x1FF};
    Bytes b(dataAt + 0x200, 0);
    b[0] = 'M';
    b[1] = 'Z';
    put32(b, 0x3C, static_cast<std::uint32_t>(pe));
    std::memcpy(&b[pe], "PE\0\0", 4);
    put16(b, pe + 4, machine);
    put16(b, pe + 6, 1);
    put16(b, pe + 20, static_cast<std::uint32_t>(optSize));
    put16(b, pe + 22, 0x2022);
    put16(b, pe + 24, magic);
    std::memcpy(&b[table], ".text", 5);
    put32(b, table + 16, 0x200);
    put32(b, table + 20, static_cast<std::uint32_t>(dataAt));
    return b;
}

std::string readText(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct PathGuard {  // PATH replaced for one scope
    std::wstring old;
    explicit PathGuard(const std::wstring& now) {
        wchar_t b[32768];
        const DWORD n = ::GetEnvironmentVariableW(L"PATH", b, 32768);
        old.assign(b, n < 32768 ? n : 0);
        ::SetEnvironmentVariableW(L"PATH", now.c_str());
    }
    ~PathGuard() { ::SetEnvironmentVariableW(L"PATH", old.c_str()); }
};

std::vector<std::complex<float>> collect(SdriqSource& s, std::size_t n, int timeoutMs = 8000) {
    std::vector<std::complex<float>> out;
    std::vector<std::complex<float>> tmp(8192);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (out.size() < n && std::chrono::steady_clock::now() < end) {
        const std::size_t got = s.read(tmp.data(), std::min(tmp.size(), n - out.size()));
        out.insert(out.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(got));
        if (got == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    }
    return out;
}
double toneHzNominal(const std::vector<std::complex<float>>& x, double fsNominal) {
    std::complex<double> acc = 0;
    for (std::size_t i = 1; i < x.size(); ++i) { acc += std::complex<double>(x[i]) * std::conj(std::complex<double>(x[i - 1])); }
    return std::arg(acc) * fsNominal / (2.0 * 3.14159265358979323846);
}

// ---------------------------------------------------------------------------------------------------------------------
void testNothingAtStart() {
    const std::uint64_t loads = vl::Library::silentLoadCount();
    {
        SdriqOpener op = d2xxSdriqOpener(g_fx.dir);
        CHECK(static_cast<bool>(op));
        SdriqSource s;
        CHECK(!s.isOpen());
    }
    CHECK(vl::Library::silentLoadCount() == loads);
}

void testAbsentAndBadLibraries() {
    const auto dir = eol::scratch("d2xxbad");
    const auto fx = eol::fixtureDir();
    const std::string scratchName = dir.filename().string();
    const std::uint64_t loads0 = vl::Library::silentLoadCount();
    std::string err;
    D2xxOpenInfo info;
    // No library in the folder: a plain sentence that names the file and FTDI's driver and where it comes from, never the folder.
    {
        auto l = openD2xxSdriq(eol::u8(dir), "", err, &info);
        CHECK(!l);
        CHECK(info.verdict == "not-found");
        CHECK(contains(err, "ftd2xx.dll") && contains(err, "FTDI") && contains(err, "D2XX") && contains(err, "ftdichip.com"));
        CHECK(contains(err, "does not include it and does not download it"));
        CHECK(!contains(err, scratchName.c_str()));
    }
    // A path that does not exist.
    {
        auto l = openD2xxSdriq(eol::u8(dir / "nope"), "", err, &info);
        CHECK(!l && info.verdict == "not-found" && contains(err, "FTDI"));
    }
    // PATH with nothing in it (the production search): the same sentence; the loader was never asked.
    {
        PathGuard pg(L"");
        auto l = openD2xxSdriq("", "", err, &info);
        CHECK(!l && info.verdict == "not-found" && contains(err, "ftd2xx.dll") && contains(err, "D2XX"));
    }
    CHECK(vl::Library::silentLoadCount() == loads0);
    // Not a library at all (a saved web page): refused from its bytes, the loader is not asked.
    {
        eol::writeFile(dir / "ftd2xx.dll", Bytes(3000, 'x'));
        auto l = openD2xxSdriq(eol::u8(dir), "", err, &info);
        CHECK(!l && info.verdict == "not-an-image" && contains(err, "ftd2xx.dll") && contains(err, "FTDI"));
        CHECK(vl::Library::silentLoadCount() == loads0);
    }
    // A 32-bit library (a 32-bit driver folder on PATH): named plainly, not loaded.
    {
        eol::writeFile(dir / "ftd2xx.dll", makeImage(vl::kMachineX86, 0x10B));
        auto l = openD2xxSdriq(eol::u8(dir), "", err, &info);
        CHECK(!l && info.verdict == "wrong-32-bit");
        CHECK(contains(err, "32-bit") && contains(err, "FTDI"));
        CHECK(vl::Library::silentLoadCount() == loads0);
    }
    // Too old: exports too little. The load itself runs under the silent error mode (the counter moves by one).
    {
        std::filesystem::copy_file(fx / "eol_partial.dll", dir / "ftd2xx.dll", std::filesystem::copy_options::overwrite_existing);
        auto l = openD2xxSdriq(eol::u8(dir), "", err, &info);
        CHECK(!l && info.verdict == "symbol-missing");
        CHECK(contains(err, "FT_ListDevices") && contains(err, "FT_Read"));
        CHECK(vl::Library::silentLoadCount() == loads0 + 1);
    }
    // A library whose own dependency is missing: error 126 as a sentence, no box (silent mode set once more).
    {
        std::filesystem::copy_file(fx / "eol_needs_dep.dll", dir / "ftd2xx.dll", std::filesystem::copy_options::overwrite_existing);
        auto l = openD2xxSdriq(eol::u8(dir), "", err, &info);
        CHECK(!l && info.verdict == "load-failed" && contains(err, "126"));
        CHECK(vl::Library::silentLoadCount() == loads0 + 2);
    }
    // The 64-bit driver's other file name is found when ftd2xx.dll is not there; a file named by the person is used as it is.
    {
        std::filesystem::remove(dir / "ftd2xx.dll");
        std::filesystem::copy_file(fx / "ftd2xx.dll", dir / "ftd2xx64.dll", std::filesystem::copy_options::overwrite_existing);
        g_fx.reset();
        auto l = openD2xxSdriq(eol::u8(dir), "", err, &info);
        CHECK(l && info.verdict == "ok");
        l.reset();
        l = openD2xxSdriq(eol::u8(dir / "ftd2xx64.dll"), "", err, &info);
        CHECK(l);
        l.reset();
        CHECK(g_fx.get(kOpens) == g_fx.get(kCloses));
    }
    g_fx.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void testDeviceChoice() {
    struct Case {
        std::int64_t mis;
        std::int64_t devices, sdriq;
        const char* serial;
        const char* verdict;
        const char* sentence;
    };
    const Case cases[] = {
        {0, 0, 0, "", "no-device", "lists no FTDI USB device"},
        {mListFails, 1, 0, "", "no-device", "lists no FTDI USB device"},
        {mOverrun, 1, 0, "", "bad-driver", "wrote outside the buffer"},
        {mUnterminated, 1, 0, "", "bad-driver", "wrote outside the buffer"},
        {0, 2, -1, "", "no-sdriq", "2 FTDI USB devices, none described as an SDR-IQ"},
        {0, 1, -1, "", "no-sdriq", "1 FTDI USB device, none described"},
        {mOpenFails, 1, 0, "", "open-failed", "another program"},
        {mSetTimeoutsFails, 1, 0, "", "open-failed", "would not accept its settings"},
        {0, 1, 0, "NOSUCH", "open-failed", "No FTDI device with that serial number"},
    };
    for (const Case& c : cases) {
        g_fx.reset();
        g_fx.set(kMisbehave, c.mis);
        g_fx.set(kNumDevices, c.devices);
        g_fx.set(kSdrIqIndex, c.sdriq);
        std::string err;
        D2xxOpenInfo info;
        auto l = openD2xxSdriq(g_fx.dir, c.serial, err, &info);
        CHECK(!l);
        CHECK(info.verdict == c.verdict);
        CHECK(contains(err, c.sentence));
        CHECK(g_fx.get(kOpens) == g_fx.get(kCloses));  // a refused open leaves no handle behind
        CHECK(g_fx.get(kOpenHandles) == 0);
    }
    // The SDR-IQ is the third of three FTDI devices: it is the one opened; the others are never opened or written to.
    {
        g_fx.reset();
        g_fx.set(kNumDevices, 3);
        g_fx.set(kSdrIqIndex, 2);
        std::string err;
        D2xxOpenInfo info;
        auto l = openD2xxSdriq(g_fx.dir, "", err, &info);
        CHECK(l && info.devicesSeen == 3);
        CHECK(g_fx.get(kOpens) == 1);
        l.reset();
        CHECK(g_fx.get(kCloses) == 1 && g_fx.get(kWritesToOther) == 0);
    }
    // A serial the person named is opened as given even if the device does not say it is an SDR-IQ; the source will ask it its name.
    {
        g_fx.reset();
        g_fx.set(kNumDevices, 2);
        g_fx.set(kSdrIqIndex, 1);
        std::string err;
        auto l = openD2xxSdriq(g_fx.dir, "FT0ABC0", err);
        CHECK(l);
        CHECK(l && l->write(reinterpret_cast<const std::uint8_t*>("hi"), 2, err));
        CHECK(g_fx.get(kWritesToOther) == 2);  // the person asked for it
        l.reset();
    }
    // A serial with a control character is refused before anything is loaded.
    {
        const std::uint64_t loads = vl::Library::silentLoadCount();
        std::string err;
        D2xxOpenInfo info;
        CHECK(!openD2xxSdriq(g_fx.dir, std::string("AB\x01"), err, &info) && info.verdict == "bad-serial");
        CHECK(vl::Library::silentLoadCount() == loads);
    }
    g_fx.reset();
}

void testLinkBytes() {
    g_fx.reset();
    std::string err;
    auto l = openD2xxSdriq(g_fx.dir, "", err);
    CHECK(l);
    if (!l) { return; }
    // The name request goes out and the radio's answer comes back byte for byte (interface specification 5.1.1).
    const Bytes req = as::encodeMessage(as::MsgType::Request, sq::ci::Name);
    CHECK(req == (Bytes{0x04, 0x20, 0x01, 0x00}));
    CHECK(l->write(req.data(), req.size(), err));
    CHECK(g_fx.get(kBytesWritten) == 4);
    Bytes got;
    std::uint8_t buf[64];
    for (int i = 0; i < 20 && got.size() < 11; ++i) {
        const int n = l->read(buf, sizeof buf, 50);
        CHECK(n >= 0);
        if (n > 0) { got.insert(got.end(), buf, buf + n); }
    }
    const Bytes want = as::encodeMessage(as::MsgType::Set, sq::ci::Name, Bytes{'S', 'D', 'R', '-', 'I', 'Q', 0});
    CHECK(got == want);
    // Nothing queued: a read returns 0 within the timeout (it is not an error).
    CHECK(l->read(buf, sizeof buf, 30) == 0);
    // A timeout of 0 is never passed (D2XX would wait forever); a huge one is clamped.
    CHECK(l->read(buf, sizeof buf, 0) == 0);
    CHECK(g_fx.get(kZeroTimeouts) == 0 && g_fx.get(kLastReadTimeout) == 1);
    CHECK(l->read(buf, sizeof buf, 100000000u) == 0);
    CHECK(g_fx.get(kLastReadTimeout) == 60000);
    // A read of nothing wanted is nothing.
    CHECK(l->read(buf, 0, 10) == 0);
    // interrupt() makes a read return at once with nothing.
    l->interrupt();
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(l->read(buf, sizeof buf, 5000) == 0);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500));
    // A write of nothing is true; a write after close is refused with a sentence.
    CHECK(l->write(req.data(), 0, err));
    l->close();
    l->close();  // idempotent
    CHECK(!l->write(req.data(), req.size(), err) && contains(err, "closed"));
    CHECK(l->read(buf, sizeof buf, 10) <= 0);
    CHECK(g_fx.get(kOpens) == 1 && g_fx.get(kCloses) == 1);
    l.reset();
    CHECK(g_fx.get(kCloses) == 1);  // the destructor does not close twice
}

void testLinkFaults() {
    std::string err;
    std::uint8_t buf[256];
    // A write that fails, one that writes nothing (gives up after three tries), and one that takes three bytes at a time (all arrive).
    {
        g_fx.reset();
        auto l = openD2xxSdriq(g_fx.dir, "", err);
        CHECK(l);
        const Bytes forty(40, 0x55);
        g_fx.set(kMisbehave, mWriteFails);
        CHECK(!l->write(forty.data(), forty.size(), err) && contains(err, "could not write"));
        l.reset();
        g_fx.reset();
        l = openD2xxSdriq(g_fx.dir, "", err);
        g_fx.set(kMisbehave, mWriteZero);
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!l->write(forty.data(), forty.size(), err) && contains(err, "timed out"));
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
        l.reset();
        g_fx.reset();
        l = openD2xxSdriq(g_fx.dir, "", err);
        g_fx.set(kMisbehave, mWriteShort);
        CHECK(l->write(forty.data(), forty.size(), err));
        CHECK(g_fx.get(kBytesWritten) == 40 && g_fx.get(kWrites) == 14);
        l.reset();
        CHECK(g_fx.get(kOpens) == g_fx.get(kCloses));
    }
    // A read that fails, one that claims more bytes than it was asked for, and a pulled cable: each is a failed link (-1), and stays failed.
    {
        for (std::int64_t mis : {mReadFails, mReadLies}) {
            g_fx.reset();
            auto l = openD2xxSdriq(g_fx.dir, "", err);
            g_fx.set(kMisbehave, mis);
            CHECK(l->read(buf, sizeof buf, 20) == -1);
            g_fx.set(kMisbehave, 0);
            CHECK(l->read(buf, sizeof buf, 20) == -1);  // the link is dead, not retried
            l.reset();
            CHECK(g_fx.get(kOpens) == g_fx.get(kCloses));
        }
        g_fx.reset();
        auto l = openD2xxSdriq(g_fx.dir, "", err);
        g_fx.set(kLostAfterBlocks, 1);
        g_fx.set(kBlocks, 1);  // the cable is already out
        CHECK(l->read(buf, sizeof buf, 20) == -1);
        l.reset();
    }
    // A fault INSIDE the library's read is a failed link, not a crashed program.
    {
        g_fx.reset();
        auto l = openD2xxSdriq(g_fx.dir, "", err);
        g_fx.set(kMisbehave, mReadCrashes);
        CHECK(l->read(buf, sizeof buf, 20) == -1);
        g_fx.set(kMisbehave, 0);
        CHECK(l->read(buf, sizeof buf, 20) == -1);
        l.reset();  // closes (or leaks) without crashing
        g_fx.reset();
        auto l2 = openD2xxSdriq(g_fx.dir, "", err);
        CHECK(l2);  // the next link works: the library is still usable
    }
    g_fx.reset();
}

void testSourceEndToEnd() {
    g_fx.reset();
    {
        SdriqSource s;
        s.setOpener(d2xxSdriqOpener(g_fx.dir));
        CHECK(s.open(""));
        CHECK(s.isOpen());
        CHECK(s.serialNumber() == "MT123456");
        CHECK(s.setCenterFrequencyHz(7100000.0));
        CHECK(s.centerFrequencyHz() == 7100000.0);
        CHECK(s.setSampleRateHz(196078.0));
        CHECK(s.start());
        CHECK(g_fx.get(kRunning) == 1);
        const auto x = collect(s, 40000);
        CHECK(x.size() >= 40000);
        // The tone is at 12345.6 Hz of the TRUE rate; the true rate is the nominal one plus 1500 ppm; at the nominal rate it is found at f / (1 + ppm).
        const double expect = 12345.6 / (1.0 + 1500e-6);
        const double f = toneHzNominal(x, 196078.0);
        CHECK(std::fabs(f - expect) < 8.0);
        CHECK(std::fabs(f - 12345.6) > 10.0);  // and it is NOT where the nominal rate would put it
        CHECK(s.dataBlocks() > 10);
        CHECK(!s.faulted());
        CHECK(g_fx.get(kForbiddenItems) == 0);  // the firmware, pass-through and security items are never sent
        CHECK(g_fx.get(kMaxRead) <= 16384);     // no read is larger than the reader's buffer
        s.stop();
        CHECK(g_fx.get(kRunning) == 0);
        // start / stop cycles, and the handle is balanced
        for (int i = 0; i < 10; ++i) {
            CHECK(s.start());
            CHECK(collect(s, 4096).size() >= 4096);
            s.stop();
        }
        CHECK(g_fx.get(kZeroTimeouts) == 0);
        s.closeDevice();
    }
    CHECK(g_fx.get(kOpens) == g_fx.get(kCloses));
    CHECK(g_fx.get(kOpenHandles) == 0);
    // Open by the serial number; the wrong serial is refused with a sentence.
    {
        SdriqSource s;
        s.setOpener(d2xxSdriqOpener(g_fx.dir));
        CHECK(!s.open("sdriq=OTHER"));
        CHECK(contains(s.lastError(), "No FTDI device with that serial number"));
        CHECK(s.open("sdriq=MT123456"));
        s.closeDevice();
    }
    // A device that is not an SDR-IQ (its name answer is another name) is refused and closed.
    {
        g_fx.reset();
        g_fx.set(kMisbehave, mBadName);
        SdriqSource s;
        s.setOpener(d2xxSdriqOpener(g_fx.dir));
        CHECK(!s.open(""));
        CHECK(contains(s.lastError(), "not an SDR-IQ"));
        CHECK(g_fx.get(kOpens) == g_fx.get(kCloses));
    }
    // A radio that answers nothing: the open gives up inside its timeout, with a sentence.
    {
        g_fx.reset();
        g_fx.set(kMisbehave, mSilent);
        SdriqSource s;
        s.setOpener(d2xxSdriqOpener(g_fx.dir));
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!s.open("timeout=300"));
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
        CHECK(!std::string(s.lastError()).empty());
        CHECK(g_fx.get(kOpens) == g_fx.get(kCloses));
    }
    // The cable is pulled while it streams: the source reports the fault and closes cleanly.
    {
        g_fx.reset();
        SdriqSource s;
        s.setOpener(d2xxSdriqOpener(g_fx.dir));
        CHECK(s.open(""));
        CHECK(s.start());
        CHECK(collect(s, 8192).size() >= 8192);
        g_fx.set(kLostAfterBlocks, g_fx.get(kBlocks) + 3);
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        while (!s.faulted() && std::chrono::steady_clock::now() < end) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        CHECK(s.faulted());
        CHECK(!std::string(s.lastError()).empty());
        s.stop();
        s.closeDevice();
        CHECK(g_fx.get(kOpens) == g_fx.get(kCloses));
    }
    g_fx.reset();
}

// The production opener (no setOpener) finds the library through PATH.
void testProductionOpenerThroughPath() {
    g_fx.reset();
    {
        PathGuard pg(std::wstring(eol::fixtureDir().wstring()));
        SdriqSource s;
        CHECK(s.open(""));
        CHECK(s.isOpen() && s.serialNumber() == "MT123456");
        s.closeDevice();
    }
    CHECK(g_fx.get(kOpens) == g_fx.get(kCloses));
    // The current directory is never searched: with PATH empty and the library only beside the process, the library is not found.
    {
        PathGuard pg(L"");
        SdriqSource s;
        CHECK(!s.open(""));
        CHECK(contains(s.lastError(), "D2XX"));
    }
    g_fx.reset();
}

void testSinks() {
    const std::string root = EOL_SRC_DIR;
    for (const char* f : {"src/source/sdriq_d2xx.cpp", "src/source/sdriq_d2xx.hpp", "src/source/ftd2xx_api.hpp"}) {
        const std::string t = readText(root + "/" + f);
        CHECK(!t.empty());
        CHECK(!contains(t, "stderr"));
        CHECK(!contains(t, "stdout"));
        CHECK(!contains(t, "std::cerr"));
        CHECK(!contains(t, "std::cout"));
        CHECK(!contains(t, " printf("));
        CHECK(!contains(t, "puts("));
        CHECK(!contains(t, "OutputDebugString"));
        CHECK(!contains(t, "settings_set"));
        // Receive only and nothing of FTDI's header: no EEPROM, baud, modem, bit-bang or reset calls are declared, and the header is not included.
        CHECK(!contains(t, "FT_EE_"));
        CHECK(!contains(t, "FT_SetBaudRate"));
        CHECK(!contains(t, "FT_SetBitMode"));
        CHECK(!contains(t, "FT_Program"));
        CHECK(!contains(t, "#include <ftd2xx.h>"));
        CHECK(!contains(t, "#include \"ftd2xx.h\""));
    }
}

// Last: it leaves a call to the library in flight on purpose (a read that does not return for 30 s) and checks that closing is bounded.
void testHangingRead() {
    g_fx.reset();
    std::string err;
    std::shared_ptr<SdriqLink> l = openD2xxSdriq(g_fx.dir, "", err);
    CHECK(l != nullptr);
    if (!l) { return; }
    g_fx.set(kMisbehave, mReadHangs);
    auto reader = std::make_shared<std::thread>([l]() {
        std::uint8_t b[16];
        l->read(b, sizeof b, 100);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto t0 = std::chrono::steady_clock::now();
    l->close();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(4));
    // The call never left the library: close() waited its bound, called FT_Close and (module leaked, not unloaded) returned.
    reader->detach();
}

}  // namespace

int main() {
    if (!g_fx.load()) {
        std::printf("FAIL cannot load the stand-in library\n");
        return 1;
    }
    testNothingAtStart();
    testAbsentAndBadLibraries();
    testDeviceChoice();
    testLinkBytes();
    testLinkFaults();
    testSourceEndToEnd();
    testProductionOpenerThroughPath();
    testSinks();
    testHangingRead();
    return testSummary("test_sdriq_d2xx");
}
