// test_engine_api_source_kinds.cpp - THE SOURCES A SECOND RADIO CAN BE ON (multi-radio engine, stage 7; docs/multi-radio-records/engine-stage-7.md).
//
// Held against the real engine through the C table and its own passes. Radio 1 stays on the signal generator throughout; radios 2 and 3 are put on an I/Q FILE (a fixture of the
// slint drive set), an RTL_TCP SERVER (a server in this process that streams a real tone) and a PLUTO (the fake IIO daemon of the transmit tests) - the three kinds RADIO_ADD takes
// besides a scanned device and the generator.
//
//   A  A FILE REALLY PLAYS AS RADIO 2: its spectrum is the file's carrier (not the generator's tone), its level is the file's, its rate is the file's, and radio 1's spectrum, rate and
//      level do not move; removing radio 2 leaves radio 1 as it was;
//   B  AN RTL_TCP SERVER AS RADIO 2: the server's tone is in radio 2's spectrum at the offset it was generated at; the server claims the ledger (the same server twice is refused,
//      naming the holder); an unreachable server leaves the radio on the generator with the reason in fact.radio.<n>.refusal;
//   C  THE SOURCE KIND IS SAVED AND COMES BACK: file, rtl_tcp and Pluto radios are written with their real kind and arguments, restored after a restart on the same sources; a missing file,
//      an unreachable server and a Pluto that does not answer follow the existing rule (the generator, named in fact.radio.restore and fact.radio.<n>.refusal, the next save still naming
//      the real source);
//   D  A PLUTO AS RADIO 2 REALLY OPENS (through the same worker as the first radio's panel), and the transmitter follows it: transmitRadioSlot is that radio, with the board opened from
//      radio 2's address, and again after a restart;
//   E  THE SOUND CARD STAYS REFUSED.
//
// The fixture folder is argument 1 (src/slint/tests/drive/fixtures).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "check.hpp"
#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/receiver.hpp"
#include "engine_api_test_env.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"
#include "iiod_fake_server.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
std::vector<cascade::source::NativeDeviceInfo> noNativeRadios() { return {}; }
std::vector<cascade::source::SoapyDeviceInfo> noSoapyRadios() { return {}; }
}  // namespace

namespace cascade::gui {
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static void installHooks() {
        Engine::testHooks_.nativeScan = &noNativeRadios;
        Engine::testHooks_.soapyScan = &noSoapyRadios;
    }
    static cascade::core::AppConfig snapshot(Engine& e) {
        e.publishConfig();
        return e.configSnapshot();
    }
    static void restore(Engine& e, const cascade::core::AppConfig& c) { e.applyConfig(c); }
    static std::size_t transmitSlot(const Engine& e) { return e.transmitRadioSlot(); }
    static std::string transmitArgs(const Engine& e) { return e.transmitArgs_; }
    static std::string deviceArgs(Engine& e, std::size_t slot) { return e.receivers_[slot] ? e.receivers_[slot]->deviceArgs_ : std::string("<no radio>"); }
    static std::string kind(Engine& e, std::size_t slot) { return e.receivers_[slot] ? e.receivers_[slot]->sourceKind_ : std::string("<no radio>"); }
    static std::string seed(const Engine& e) { return e.transmitArgsSeed(); }
};
}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using Engine = cascade::engine::Engine;
using cascade::core::AppConfig;
using cascade::core::ConfigStore;

namespace {

constexpr const char* kHooks = "testHooks=1;token=s3cret";
constexpr std::uint64_t kGrants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_AUDIO | FOXAPI_GRANT_TRANSMIT;

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

fs::path g_fixtures;

// --- one engine ---------------------------------------------------------------------------------------------------------------------------

struct Rig {
    Engine eng;
    std::unique_ptr<testing::Session> s;

    explicit Rig(const AppConfig* cfg = nullptr) {
        eng.configureApi(kHooks);
        eng.initialise();
        if (cfg != nullptr) { Access::restore(eng, cascade::core::startupState(*cfg)); }
        eng.controlPass();
        s = std::make_unique<testing::Session>(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, kGrants);
        CHECK(s->ok());
        pump(3);
    }
    void pump(int n = 1) {
        for (int i = 0; i < n; ++i) {
            eng.controlPass();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    template <class F>
    bool until(F done, int timeoutMs = 15000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < end) {
            eng.controlPass();
            if (done()) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        return false;
    }
    testing::Outcome send(const FoxCommand& c) {
        testing::Outcome o;
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        const int32_t n = api()->submit(s->raw(), &c, 1, &sr);
        o.submitStatus = n < 0 ? n : sr.status;
        if (o.submitStatus != FOXAPI_OK) { return o; }
        for (int pass = 0; pass < 40 && !o.landed; ++pass) {
            eng.controlPass();
            FoxCommandResult r[16];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(s->raw(), r, 16);
            for (int32_t i = 0; i < got; ++i) {
                if (r[i].ticket == sr.ticket) {
                    o.landed = true;
                    o.result = r[i];
                }
            }
        }
        return o;
    }
    std::string fact(const std::string& key) {
        char buf[8192];
        const int32_t n = api()->get_setting(s->raw(), ("fact." + key).c_str(), buf, sizeof(buf));
        return n >= 0 ? std::string(buf) : std::string("<absent>");
    }
    FoxReceiverState radio(std::uint32_t n, std::int32_t* status = nullptr) {
        FoxReceiverState st{};
        st.structSize = sizeof(st);
        const std::int32_t r = api()->read_radio_state(s->raw(), n, &st);
        if (status != nullptr) { *status = r; }
        return st;
    }
    FoxCommand aimed(std::uint32_t op, std::uint32_t target, double n0 = 0.0, long long i0 = 0, const char* text = nullptr) {
        FoxCommand c = testing::cmd(op, n0, i0, text);
        c.target = target;
        return c;
    }
    // RADIO_ADD with a source id; the radio is started as soon as it is open.
    testing::Outcome add(long long number, const std::string& id, bool run = true) {
        FoxCommand c = testing::cmd(FOXAPI_OP_RADIO_ADD, 0.0, number, id.c_str());
        c.ival[1] = run ? 1 : 0;
        return send(c);
    }
    struct Spectrum {
        std::int32_t status = FOXAPI_NO_CHANGE;
        FoxSpectrumInfo info{};
        std::vector<float> bins;
    };
    Spectrum spectrum(std::uint32_t n) {
        Spectrum sp;
        sp.bins.assign(4096, 0.0f);
        sp.info.structSize = sizeof(sp.info);
        sp.status = api()->read_radio_spectrum(s->raw(), n, 0, &sp.info, sp.bins.data(), static_cast<std::uint32_t>(sp.bins.size()));
        if (sp.status == FOXAPI_OK) { sp.bins.resize(sp.info.binCount); }
        return sp;
    }
};

// The bin a tone `offsetHz` from the centre lands in, and the strongest bin; how far the strongest stands above the median.
std::size_t binFor(const Rig::Spectrum& sp, double offsetHz) {
    const double n = static_cast<double>(sp.info.binCount);
    return static_cast<std::size_t>(std::llround((offsetHz / sp.info.spanHz + 0.5) * n)) % sp.info.binCount;
}
std::size_t peakBin(const Rig::Spectrum& sp) { return static_cast<std::size_t>(std::max_element(sp.bins.begin(), sp.bins.end()) - sp.bins.begin()); }
float standout(const Rig::Spectrum& sp) {
    std::vector<float> v = sp.bins;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
    return sp.bins[peakBin(sp)] - v[v.size() / 2];
}
// The spectrum has a clear peak within `tol` bins of the one `offsetHz` names.
bool peakAt(const Rig::Spectrum& sp, double offsetHz, int tol = 3) {
    if (sp.status != FOXAPI_OK || sp.bins.empty()) { return false; }
    const long long want = static_cast<long long>(binFor(sp, offsetHz));
    const long long got = static_cast<long long>(peakBin(sp));
    return std::llabs(want - got) <= tol && standout(sp) > 15.0f;
}

AppConfig throughTheFile(Engine& e) {
    AppConfig out;
    std::string error;
    const std::string text = ConfigStore::serialize(Access::snapshot(e));
    CHECK(ConfigStore::parse(text, "test", out, error));
    return out;
}

// --- an rtl_tcp server that streams a real tone ---------------------------------------------------------------------------------------------

// Serves the rtl_tcp protocol: the 12-byte header, then unsigned 8-bit I/Q at the rate the client last asked for (command 0x02), a complex tone `toneHz` from the centre at about
// -12 dBFS over a little noise, paced to real time. Commands are read and logged (the last rate and frequency are kept). One connection at a time is enough for this test.
class ToneServer {
public:
    ~ToneServer() { stop(); }
    bool start() {
        if (!cascade::test::startNetwork()) { return false; }
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == cascade::test::kBadSock) { return false; }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) { return false; }
        cascade::test::SockLen len = sizeof(addr);
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) { return false; }
        port_ = ::ntohs(addr.sin_port);
        if (::listen(listener_, 4) != 0) { return false; }
        run_ = true;
        thread_ = std::thread([this] { loop(); });
        return true;
    }
    void stop() {
        run_ = false;
        if (thread_.joinable()) { thread_.join(); }
        if (listener_ != cascade::test::kBadSock) {
            cascade::test::closeSock(listener_);
            listener_ = cascade::test::kBadSock;
        }
    }
    std::uint16_t port() const { return port_; }
    std::string address() const { return "127.0.0.1:" + std::to_string(port_); }
    std::atomic<double> toneHz{100000.0};
    std::atomic<double> rateHz{2400000.0};
    std::atomic<int> connections{0};
    std::atomic<std::uint64_t> bytesSent{0};

private:
    void loop() {
        while (run_) {
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(listener_, &rf);
            timeval tv{};
            tv.tv_usec = 50 * 1000;
#if defined(_WIN32)
            const int ready = ::select(0, &rf, nullptr, nullptr, &tv);
#else
            const int ready = ::select(listener_ + 1, &rf, nullptr, nullptr, &tv);
#endif
            if (ready <= 0) { continue; }
            sockaddr_in from{};
            cascade::test::SockLen len = sizeof(from);
            const cascade::test::Sock s = ::accept(listener_, reinterpret_cast<sockaddr*>(&from), &len);
            if (s == cascade::test::kBadSock) { continue; }
            ++connections;
            serve(s);
            cascade::test::closeSock(s);
        }
    }
    void serve(cascade::test::Sock s) {
        const unsigned char header[12] = {'R', 'T', 'L', '0', 0, 0, 0, 5, 0, 0, 0, 29};
        if (cascade::test::sendBytes(s, reinterpret_cast<const char*>(header), 12) != 12) { return; }
        // Non-blocking from here on: commands are read between chunks.
#if defined(_WIN32)
        u_long nb = 1;
        ::ioctlsocket(s, FIONBIO, &nb);
#else
        ::fcntl(s, F_SETFL, ::fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
        std::vector<unsigned char> pending;
        double phase = 0.0;
        std::uint32_t noise = 12345u;
        auto t0 = std::chrono::steady_clock::now();
        double owed = 0.0;   // samples the clock says have been due and were not yet sent
        auto last = t0;
        while (run_) {
            char buf[256];
            const int r = cascade::test::recvBytes(s, buf, sizeof(buf));
            if (r == 0) { return; }
            if (r > 0) {
                pending.insert(pending.end(), buf, buf + r);
                while (pending.size() >= 5) {
                    const unsigned id = pending[0];
                    const std::uint32_t v = (static_cast<std::uint32_t>(pending[1]) << 24) | (static_cast<std::uint32_t>(pending[2]) << 16) | (static_cast<std::uint32_t>(pending[3]) << 8) | pending[4];
                    if (id == 0x02) { rateHz = static_cast<double>(v); }
                    pending.erase(pending.begin(), pending.begin() + 5);
                }
            }
            const auto now = std::chrono::steady_clock::now();
            owed += std::chrono::duration<double>(now - last).count() * rateHz.load();
            last = now;
            const std::size_t samples = static_cast<std::size_t>(std::min(owed, 16384.0));
            if (samples < 512) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            owed -= static_cast<double>(samples);
            if (owed > 2.0e6) { owed = 0.0; }
            std::vector<unsigned char> out(samples * 2);
            const double step = 2.0 * 3.14159265358979323846 * toneHz.load() / rateHz.load();
            for (std::size_t i = 0; i < samples; ++i) {
                noise = noise * 1664525u + 1013904223u;
                const double n1 = (static_cast<double>(noise >> 24) - 127.5) / 127.5 * 0.01;
                noise = noise * 1664525u + 1013904223u;
                const double n2 = (static_cast<double>(noise >> 24) - 127.5) / 127.5 * 0.01;
                out[2 * i] = static_cast<unsigned char>(std::lround(127.5 + 127.5 * (0.25 * std::cos(phase) + n1)));
                out[2 * i + 1] = static_cast<unsigned char>(std::lround(127.5 + 127.5 * (0.25 * std::sin(phase) + n2)));
                phase += step;
                if (phase > 6.283185307179586) { phase -= 6.283185307179586; }
            }
            std::size_t sent = 0;
            while (sent < out.size() && run_) {
                const int w = cascade::test::sendBytes(s, reinterpret_cast<const char*>(out.data()) + sent, static_cast<int>(out.size() - sent));
                if (w > 0) {
                    sent += static_cast<std::size_t>(w);
                } else {
#if defined(_WIN32)
                    if (::WSAGetLastError() != WSAEWOULDBLOCK) { return; }
#else
                    if (errno != EAGAIN && errno != EWOULDBLOCK) { return; }
#endif
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            bytesSent += sent;
        }
    }
    cascade::test::Sock listener_ = cascade::test::kBadSock;
    std::uint16_t port_ = 0;
    std::atomic<bool> run_{false};
    std::thread thread_;
};

// A port that nothing listens on: bound, read, closed.
std::string closedAddress() {
    cascade::test::startNetwork();
    cascade::test::Sock l = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    ::bind(l, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    cascade::test::SockLen len = sizeof(a);
    ::getsockname(l, reinterpret_cast<sockaddr*>(&a), &len);
    const unsigned short port = ::ntohs(a.sin_port);
    cascade::test::closeSock(l);
    return "127.0.0.1:" + std::to_string(port);
}

// The Pluto: the fake IIO daemon of the transmit tests with the receive half's attributes added, serving a quiet stream so that the open completes.
struct Pluto {
    cascade::test::FakeTxIiod d;
    std::string address;   // "127.0.0.1:<port>"
    bool start() {
        std::string err;
        if (!d.start(err)) { return false; }
        cascade::test::stockTxBoard(d);
        cascade::test::stockRxBoard(d);
        address = "127.0.0.1:" + std::to_string(static_cast<unsigned>(d.port()));
        return true;
    }
    std::string uriArgs() const { return "uri=ip:" + address; }
};

fs::path fixture(const char* name) { return g_fixtures / name; }

// ---- A -----------------------------------------------------------------------------------------------------------------------------------------

void testAFileAsRadioTwo() {
    std::printf("A: an I/Q file really plays as radio 2\n");
    Rig r;
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, 1, 0.0, 1)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.spectrum(1).status == FOXAPI_OK; }));
    // radio 1 as it is before: a generator, its own rate, its own spectrum
    r.pump(60);
    const FoxReceiverState one0 = r.radio(1);
    const Rig::Spectrum sp1a = r.spectrum(1);
    CHECK_EQ(sp1a.status, FOXAPI_OK);
    const std::size_t peak1a = peakBin(sp1a);
    CHECK(standout(sp1a) > 15.0f);

    const std::string path = fixture("scope.wav").string();   // an AM carrier at +22.5 kHz of a 48 kS/s file, 4 s, looped
    const testing::Outcome o = r.add(2, "file:" + path);
    CHECK_EQ(o.status(), FOXAPI_OK);
    // the file opens on its own worker: until it lands the generator stands in; then the radio is on the file's rate
    CHECK(r.until([&] { return r.fact("radio.2.source") == "file"; }));
    CHECK_EQ(r.fact("radio.2.source"), std::string("file"));
    CHECK_EQ(r.fact("radio.2.refusal"), std::string("<absent>"));
    CHECK_EQ(r.fact("radio.1.source"), std::string("siggen"));
    CHECK(r.until([&] { return r.radio(2).sampleRateHz == 48000.0 && r.spectrum(2).status == FOXAPI_OK && peakAt(r.spectrum(2), 22500.0); }, 20000));
    const FoxReceiverState two = r.radio(2);
    CHECK_EQ(two.sampleRateHz, 48000.0);
    CHECK((two.flags & FOXAPI_RX_RUNNING) != 0u);
    {
        // the spectrum is the FILE's: the carrier at +22.5 kHz, 48 kHz wide, nothing of the generator's
        const Rig::Spectrum sp2 = r.spectrum(2);
        CHECK_EQ(sp2.status, FOXAPI_OK);
        CHECK(std::fabs(sp2.info.spanHz - 48000.0) < 1.0);
        CHECK(peakAt(sp2, 22500.0));
    }
    // the level: put the channel on the carrier and the file's power is read (the generator radio 1 reads something else)
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_VFO_OFFSET, 2, 22500.0)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_MODE, 2, 0.0, FOXAPI_DEMOD_AM)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_BANDWIDTH, 2, 3000.0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(2).signalDb > -60.0; }, 10000));
    const FoxReceiverState two2 = r.radio(2);
    CHECK(two2.signalDb > -60.0 && two2.signalDb < 0.0);
    CHECK(std::fabs(two2.signalDb - r.radio(1).signalDb) > 0.05 || two2.sampleRateHz != r.radio(1).sampleRateHz);
    // and the level is the FILE's: moving the channel off the carrier (to the centre, 22.5 kHz away) drops it by tens of dB
    {
        const double onCarrier = r.radio(2).signalDb;
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_VFO_OFFSET, 2, 0.0)).status(), FOXAPI_OK);
        CHECK(r.until([&] { return r.radio(2).signalDb < onCarrier - 20.0; }, 10000));
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_VFO_OFFSET, 2, 22500.0)).status(), FOXAPI_OK);
    }

    // RADIO 1 IS UNAFFECTED: same rate, same peak, same device, still running
    r.pump(30);
    const FoxReceiverState one1 = r.radio(1);
    CHECK_EQ(one1.sampleRateHz, one0.sampleRateHz);
    CHECK_EQ(one1.centreHz, one0.centreHz);
    CHECK_EQ(one1.vfoOffsetHz, one0.vfoOffsetHz);
    CHECK((one1.flags & FOXAPI_RX_RUNNING) != 0u);
    const Rig::Spectrum sp1b = r.spectrum(1);
    CHECK_EQ(peakBin(sp1b), peak1a);
    CHECK(std::fabs(sp1b.info.spanHz - sp1a.info.spanHz) < 1.0);
    CHECK_EQ(std::string(one1.deviceName), std::string(one0.deviceName));

    // removing radio 2 leaves radio 1 as it was, and the slot can be used again by another file
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RADIO_REMOVE, 1, 0.0, 2)).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("radio.count"), std::string("1"));
    CHECK_EQ(r.add(2, "file:" + path).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("radio.2.source") == "file" && r.radio(2).sampleRateHz == 48000.0; }));
    CHECK_EQ(peakBin(r.spectrum(1)), peak1a);
    // a file that is not there: the radio stays on the generator and says why
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RADIO_REMOVE, 1, 0.0, 2)).status(), FOXAPI_OK);
    CHECK_EQ(r.add(2, "file:" + (g_fixtures / "no-such-file.wav").string()).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("radio.2.refusal") != "<absent>"; }));
    CHECK_EQ(r.fact("radio.2.source"), std::string("siggen"));
    CHECK(r.fact("radio.2.refusal").find("cannot open file") != std::string::npos);
}

// Two files opened IN THE SAME PASS, one for each radio: each lands in the radio that asked (the workers are the radios' own).
void testAFilesOnTwoRadiosAtOnce() {
    std::printf("A2: two radios open their files in the same pass\n");
    Rig r;
    const std::string cw = fixture("cw-decoder.wav").string();      // 48 kS/s
    const std::string tw = fixture("twotone-decoder.wav").string(); // 96 kS/s
    const std::string scope = fixture("scope.wav").string();        // 48 kS/s, a carrier at +22.5 kHz
    FoxCommand add = testing::cmd(FOXAPI_OP_RADIO_ADD, 0.0, 2, ("file:" + tw).c_str());
    add.ival[1] = 1;
    // radio 2 exists first (its file is the 96 kS/s one); then radio 1 and radio 3 are both asked for a file before the engine's next pass
    CHECK_EQ(r.send(add).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(2).sampleRateHz == 96000.0; }));
    FoxCommand add3 = testing::cmd(FOXAPI_OP_RADIO_ADD, 0.0, 3, ("file:" + scope).c_str());
    add3.ival[1] = 1;
    FoxCommand sel1 = r.aimed(FOXAPI_OP_SELECT_SOURCE, 1, 0.0, 0, ("file:" + cw).c_str());
    FoxSubmitResult a{}, b{};
    a.structSize = b.structSize = sizeof(a);
    CHECK(api()->submit(r.s->raw(), &sel1, 1, &a) >= 0);
    CHECK(api()->submit(r.s->raw(), &add3, 1, &b) >= 0);
    CHECK_EQ(a.status, FOXAPI_OK);
    CHECK_EQ(b.status, FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("radio.3.source") == "file" && r.fact("radio.1.source") == "file" && r.radio(1).sampleRateHz == 48000.0 && r.radio(3).sampleRateHz == 48000.0; }, 20000));
    // radio 1 holds the cw file (no carrier at +22.5 kHz of a strong, steady kind), radio 3 the scope file (a steady carrier at +22.5 kHz), radio 2 still the 96 kS/s one
    CHECK(r.until([&] { return peakAt(r.spectrum(3), 22500.0); }));
    CHECK_EQ(r.radio(2).sampleRateHz, 96000.0);
    CHECK_EQ(r.fact("radio.2.source"), std::string("file"));
    CHECK_EQ(r.fact("radio.1.refusal"), std::string("<absent>"));
}

// ---- B -----------------------------------------------------------------------------------------------------------------------------------------

void testBRtlTcpAsRadioTwo() {
    std::printf("B: an rtl_tcp server as radio 2\n");
    ToneServer srv;
    CHECK(srv.start());
    srv.toneHz = 100000.0;
    Rig r;
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, 1, 0.0, 1)).status(), FOXAPI_OK);
    r.pump(40);
    const Rig::Spectrum sp1a = r.spectrum(1);
    const std::size_t peak1a = peakBin(sp1a);
    const FoxReceiverState one0 = r.radio(1);

    CHECK_EQ(r.add(2, "open-rtltcp:rtltcp=" + srv.address()).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("radio.2.source") == "rtltcp"; }, 20000));
    CHECK_EQ(r.fact("radio.2.source"), std::string("rtltcp"));
    CHECK_EQ(r.fact("radio.2.refusal"), std::string("<absent>"));
    CHECK_EQ(r.fact("radio.2.device"), std::string("rtltcp|rtltcp=" + srv.address()));
    CHECK(srv.connections.load() >= 1);
    // the tone the SERVER generated is in radio 2's spectrum, at the offset it was generated at
    CHECK(r.until([&] { return r.spectrum(2).status == FOXAPI_OK && r.radio(2).sampleRateHz == srv.rateHz.load() && peakAt(r.spectrum(2), 100000.0, 4); }, 20000));
    const FoxReceiverState two = r.radio(2);
    CHECK_EQ(two.sampleRateHz, srv.rateHz.load());
    CHECK(peakAt(r.spectrum(2), 100000.0, 4));
    CHECK(srv.bytesSent.load() > 100000u);
    // radio 1 is unaffected
    const Rig::Spectrum sp1b = r.spectrum(1);
    CHECK_EQ(peakBin(sp1b), peak1a);
    CHECK_EQ(r.radio(1).sampleRateHz, one0.sampleRateHz);
    CHECK_EQ(r.fact("radio.1.source"), std::string("siggen"));
    // the server is held: the same server again - in either spelling - is refused, naming the holder, and no third radio appears
    {
        const testing::Outcome again = r.add(3, "open-rtltcp:" + srv.address());
        CHECK_EQ(again.status(), FOXAPI_BUSY);
        CHECK(std::string(again.result.message).find("Radio 2") != std::string::npos);
        const testing::Outcome again2 = r.add(3, "open-rtltcp:rtltcp=" + srv.address());
        CHECK_EQ(again2.status(), FOXAPI_BUSY);
        CHECK_EQ(r.fact("radio.count"), std::string("2"));
    }
    // the tone moves with the server: a new tone is a new peak (radio 2 really reads the socket)
    srv.toneHz = -60000.0;
    CHECK(r.until([&] { return peakAt(r.spectrum(2), -60000.0, 4); }, 15000));
    // an unreachable server: the radio is added on the generator and the refusal says why
    const std::string dead = closedAddress();
    CHECK_EQ(r.add(3, "open-rtltcp:" + dead).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("radio.3.refusal") != "<absent>"; }, 30000));
    CHECK_EQ(r.fact("radio.3.source"), std::string("siggen"));
    CHECK(!r.fact("radio.3.refusal").empty());
    // a removed radio lets go of the server: it can be opened again
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RADIO_REMOVE, 1, 0.0, 2)).status(), FOXAPI_OK);
    CHECK_EQ(r.add(2, "open-rtltcp:" + srv.address()).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("radio.2.source") == "rtltcp"; }, 20000));
}

// ---- C -----------------------------------------------------------------------------------------------------------------------------------------

void testCSavedWithTheirRealKind() {
    std::printf("C: file, rtl_tcp and Pluto radios are saved with their real kind and come back\n");
    ToneServer srv;
    CHECK(srv.start());
    srv.toneHz = 80000.0;
    Pluto pluto;
    CHECK(pluto.start());
    const std::string path = fixture("scope.wav").string();
    AppConfig file;
    {
        Rig a;
        CHECK_EQ(a.add(2, "file:" + path).status(), FOXAPI_OK);
        CHECK_EQ(a.add(3, "open-rtltcp:rtltcp=" + srv.address()).status(), FOXAPI_OK);
        CHECK_EQ(a.add(4, "open-pluto:" + pluto.uriArgs()).status(), FOXAPI_OK);
        CHECK_EQ(a.add(5, "").status(), FOXAPI_OK);   // a generator beside them
        CHECK(a.until([&] { return a.fact("radio.2.source") == "file" && a.fact("radio.3.source") == "rtltcp" && a.fact("radio.4.source") == "pluto"; }, 30000));
        CHECK_EQ(a.send(a.aimed(FOXAPI_OP_SET_VOLUME, 3, 0.31)).status(), FOXAPI_OK);
        a.pump(20);
        file = throughTheFile(a.eng);
        CHECK_EQ(file.radios.size(), static_cast<std::size_t>(4));
        CHECK_EQ(file.radios[0].number, 2);
        CHECK_EQ(file.radios[0].sourceKind, std::string("file"));
        CHECK_EQ(file.radios[0].args, path);
        CHECK_EQ(file.radios[1].number, 3);
        CHECK_EQ(file.radios[1].sourceKind, std::string("rtltcp"));
        CHECK_EQ(file.radios[1].args, "rtltcp=" + srv.address());
        CHECK_EQ(file.radios[2].number, 4);
        CHECK_EQ(file.radios[2].sourceKind, std::string("pluto"));
        CHECK_EQ(file.radios[2].args, pluto.uriArgs());
        CHECK_EQ(file.radios[3].sourceKind, std::string("siggen"));
        // the text of the file says the same
        const json j = json::parse(ConfigStore::serialize(Access::snapshot(a.eng)));
        CHECK_EQ(j["radios"][0]["sourceKind"].get<std::string>(), std::string("file"));
        CHECK_EQ(j["radios"][1]["sourceKind"].get<std::string>(), std::string("rtltcp"));
        CHECK_EQ(j["radios"][2]["sourceKind"].get<std::string>(), std::string("pluto"));
        CHECK_EQ(j["sourceKind"].get<std::string>(), std::string("siggen"));   // radio 1's keys are what they always were
    }
    // A RESTART from the file alone: every radio is back on its own source
    {
        Rig b(&file);
        CHECK_EQ(b.fact("radio.count"), std::string("5"));
        CHECK_EQ(b.fact("radio.restore"), std::string("<absent>"));
        CHECK_EQ(b.fact("radio.2.source"), std::string("file"));
        CHECK_EQ(b.fact("radio.3.source"), std::string("rtltcp"));
        CHECK_EQ(b.fact("radio.4.source"), std::string("pluto"));
        CHECK_EQ(b.fact("radio.5.source"), std::string("siggen"));
        CHECK(b.until([&] { return b.radio(2).sampleRateHz == 48000.0; }, 20000));
        CHECK(b.until([&] { return b.radio(3).sampleRateHz == srv.rateHz.load() && peakAt(b.spectrum(3), 80000.0, 4); }, 20000));
        CHECK(std::fabs(b.radio(3).volume - 0.31) < 1e-6);
        CHECK_EQ(Access::kind(b.eng, 3), std::string("pluto"));
        CHECK_EQ(Access::deviceArgs(b.eng, 3), pluto.uriArgs());
        // and the save from the restored engine is the file it came from
        const AppConfig again = throughTheFile(b.eng);
        CHECK_EQ(again.radios.size(), file.radios.size());
        for (std::size_t i = 0; i < again.radios.size() && i < file.radios.size(); ++i) {
            CHECK_EQ(again.radios[i].sourceKind, file.radios[i].sourceKind);
            CHECK_EQ(again.radios[i].args, file.radios[i].args);
        }
    }
    // THE SOURCES ARE NOT THERE: a missing file, an unreachable server, a Pluto that does not answer
    AppConfig gone = file;
    gone.radios[0].args = (g_fixtures / "no-such-file.wav").string();
    const std::string dead = closedAddress();
    gone.radios[1].args = "rtltcp=" + dead;
    gone.radios[2].args = "uri=ip:" + closedAddress();
    {
        Rig c(&gone);
        CHECK_EQ(c.fact("radio.count"), std::string("5"));
        const std::string notes = c.fact("radio.restore");
        CHECK(notes != "<absent>");
        CHECK(notes.find("Radio 2:") != std::string::npos);
        CHECK(notes.find("Radio 3:") != std::string::npos);
        CHECK(notes.find("Radio 4:") != std::string::npos);
        for (int n = 2; n <= 4; ++n) {
            CHECK_EQ(c.fact("radio." + std::to_string(n) + ".source"), std::string("siggen"));
            CHECK(c.fact("radio." + std::to_string(n) + ".refusal") != "<absent>");
        }
        // the generator stands in and the radios sit there, but the NEXT SAVE still names the real sources
        const AppConfig again = throughTheFile(c.eng);
        CHECK_EQ(again.radios.size(), gone.radios.size());
        CHECK_EQ(again.radios[0].sourceKind, std::string("file"));
        CHECK_EQ(again.radios[0].args, gone.radios[0].args);
        CHECK_EQ(again.radios[1].sourceKind, std::string("rtltcp"));
        CHECK_EQ(again.radios[1].args, gone.radios[1].args);
        CHECK_EQ(again.radios[2].sourceKind, std::string("pluto"));
        CHECK_EQ(again.radios[2].args, gone.radios[2].args);
        // the radios beside them came back whole
        CHECK_EQ(c.fact("radio.5.source"), std::string("siggen"));
        CHECK_EQ(c.fact("radio.5.refusal"), std::string("<absent>"));
    }
}

// ---- D -----------------------------------------------------------------------------------------------------------------------------------------

void testDPlutoAsRadioTwoAndTheTransmitter() {
    std::printf("D: a Pluto as radio 2 opens, and the transmitter follows it\n");
    Pluto pluto;
    CHECK(pluto.start());
    Rig r;
    CHECK_EQ(Access::transmitSlot(r.eng), static_cast<std::size_t>(0));
    CHECK_EQ(r.fact("transmit.radio"), std::string("1"));
    CHECK_EQ(Access::seed(r.eng), std::string("uri=ip:192.168.2.1"));   // (no Pluto anywhere: the board's factory address)
    CHECK_EQ(r.add(2, "open-pluto:" + pluto.uriArgs()).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("radio.2.source") == "pluto"; }, 30000));
    CHECK_EQ(r.fact("radio.2.source"), std::string("pluto"));
    // the Transmit address is seeded from the Pluto that is RADIO 2 (radio 1 is the generator)
    CHECK_EQ(Access::seed(r.eng), pluto.uriArgs());
    CHECK_EQ(r.fact("radio.2.refusal"), std::string("<absent>"));
    CHECK_EQ(Access::deviceArgs(r.eng, 1), pluto.uriArgs());
    CHECK_EQ(r.fact("radio.2.device"), std::string("pluto|" + pluto.uriArgs()));
    CHECK_EQ(r.fact("radio.1.source"), std::string("siggen"));
    // the board is held: the same Pluto again is refused, naming the holder
    {
        const testing::Outcome again = r.add(3, "open-pluto:" + pluto.uriArgs());
        CHECK_EQ(again.status(), FOXAPI_BUSY);
        CHECK(std::string(again.result.message).find("Radio 2") != std::string::npos);
    }
    // THE TRANSMITTER: opened with no address of its own it takes the Pluto's (radio 2's, not radio 1's generator), and follows radio 2
    CHECK_EQ(Access::transmitSlot(r.eng), static_cast<std::size_t>(0));   // (no transmitter open yet: the rule's first answer)
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_TX_OPEN, 1, 0.0, 0, pluto.uriArgs().c_str())).status(), FOXAPI_OK);
    CHECK_EQ(Access::transmitArgs(r.eng), pluto.uriArgs());
    CHECK_EQ(Access::transmitSlot(r.eng), static_cast<std::size_t>(1));
    r.pump(5);
    CHECK_EQ(r.fact("transmit.radio"), std::string("2"));
    // a radio's stop ends the key only when that radio is the transmitter's: removing radio 2 gives the transmitter back to radio 1
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RADIO_REMOVE, 1, 0.0, 2)).status(), FOXAPI_OK);
    r.pump(5);
    CHECK_EQ(r.fact("transmit.radio"), std::string("1"));
}

void testDPlutoSurvivesARestart() {
    std::printf("D2: the Pluto radio and the transmitter's rule after a restart\n");
    Pluto pluto;
    CHECK(pluto.start());
    AppConfig file;
    {
        Rig a;
        CHECK_EQ(a.add(2, "open-pluto:" + pluto.uriArgs()).status(), FOXAPI_OK);
        CHECK(a.until([&] { return a.fact("radio.2.source") == "pluto"; }, 30000));
        file = throughTheFile(a.eng);
        file.transmitArgs = pluto.uriArgs();
    }
    Rig b(&file);
    CHECK_EQ(b.fact("radio.2.source"), std::string("pluto"));
    CHECK_EQ(b.fact("radio.restore"), std::string("<absent>"));
    CHECK_EQ(Access::deviceArgs(b.eng, 1), pluto.uriArgs());
    // the transmitter's address in the file is the board radio 2 is on: the rule gives radio 2 once the board is open
    CHECK_EQ(b.send(b.aimed(FOXAPI_OP_TX_OPEN, 1, 0.0, 0, pluto.uriArgs().c_str())).status(), FOXAPI_OK);
    CHECK_EQ(Access::transmitSlot(b.eng), static_cast<std::size_t>(1));
    CHECK_EQ(b.fact("transmit.radio"), std::string("2"));
}

// ---- E -----------------------------------------------------------------------------------------------------------------------------------------

void testESoundCardStaysRefused() {
    std::printf("E: the sound card stays the first radio's\n");
    Rig r;
    for (const char* id : {"soundcard:open", "soundcard:0", "row:soundcard", "row:iqfile"}) {
        const testing::Outcome o = r.add(2, id);
        CHECK_EQ(o.status(), FOXAPI_UNSUPPORTED);
        CHECK_EQ(r.fact("radio.count"), std::string("1"));
    }
    CHECK(std::string(r.add(2, "soundcard:open").result.message).find("sound card") != std::string::npos);
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_source_kinds\n");
    engine_api_test::hermetic("source_kinds");
    if (argc < 2) {
        std::printf("usage: test_engine_api_source_kinds <fixtures folder>\n");
        return 2;
    }
    g_fixtures = argv[1];
    if (!fs::exists(g_fixtures / "scope.wav")) {
        std::printf("the fixture scope.wav is not in %s\n", g_fixtures.string().c_str());
        return 2;
    }
    Access::installHooks();
    testAFileAsRadioTwo();
    testAFilesOnTwoRadiosAtOnce();
    testBRtlTcpAsRadioTwo();
    testCSavedWithTheirRealKind();
    testDPlutoAsRadioTwoAndTheTransmitter();
    testDPlutoSurvivesARestart();
    testESoundCardStaysRefused();
    return check::finish("test_engine_api_source_kinds");
}
