// test_engine_api_s4b_rtltcp.cpp - screen lane S4b's rtl_tcp server through the REAL engine: a stand-in radio (a tone source at 240 kS/s) is open, the wirec settings turn the server on,
// a stand-in client (a plain loopback socket) reads the header and the samples and sends commands. Everything binds 127.0.0.1; no radio is opened.
//
//   R1  off by default: nothing listens, a bad port is refused
//   R2  on: the header, then 8-bit unsigned samples of the radio's tone (zero at 128, amplitude 0.3 of full scale), the scope says this computer only
//   R3  the client retunes the radio (set frequency) through the engine's tuning command: the stand-in radio's centre moves; a frequency outside the receiver's range is refused and
//       counted; every other command (rate, gain, bias tee ...) is counted and ignored: the radio's own settings do not change
//   R4  a second client is closed at once; network I/Q out on the same radio is refused while the server holds the radio's tap; turned off the port is released and the tap is free
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <thread>

#include "net_test_support.hpp"
#include "wire_c_rig.hpp"

using namespace wirec_test;

namespace {

constexpr double kPi = 3.14159265358979323846;
std::atomic<double> gCentre{100.0e6};
std::atomic<int> gRateCalls{0};

class Tone final : public cascade::source::IqSource {
public:
    bool start() override { running_ = true; return true; }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return 240000.0; }
    bool setSampleRateHz(double) override { ++gRateCalls; return false; }
    double centerFrequencyHz() const override { return gCentre.load(); }
    bool setCenterFrequencyHz(double hz) override { gCentre.store(hz); return true; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        for (std::size_t i = 0; i < n; ++i, ++k_) {
            const double a = 2.0 * kPi * 15000.0 * static_cast<double>(k_) / 240000.0;
            dst[i] = {static_cast<float>(0.3 * std::cos(a)), static_cast<float>(0.3 * std::sin(a))};
        }
        return n;
    }
    const char* name() const override { return "s4b-tone"; }
    const char* lastError() const override { return ""; }

private:
    bool running_ = false;
    std::uint64_t k_ = 0;
};

template <class F>
bool until(Rig& r, F f, int ms = 8000) {
    const auto t0 = nettest::Clock::now();
    while (nettest::msSince(t0) < ms) {
        r.pass(2);
        if (f()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    return f();
}

int freePort() {
    std::string err;
    std::uint32_t ip = 0;
    cascade::net::sock::parseIpv4("127.0.0.1", ip);
    const cascade::net::sock::Handle h = cascade::net::sock::listenTcp(ip, 0, 1, err);
    const int p = h == cascade::net::sock::kInvalid ? 0 : cascade::net::sock::boundPort(h);
    cascade::net::sock::closeHandle(h);
    return p;
}

void sendCmd(nettest::Client& c, std::uint8_t cmd, std::uint32_t param) {
    const std::uint8_t b[5] = {cmd, static_cast<std::uint8_t>(param >> 24), static_cast<std::uint8_t>(param >> 16), static_cast<std::uint8_t>(param >> 8), static_cast<std::uint8_t>(param)};
    c.sendBytes(b, 5);
}

}  // namespace

int main() {
    engine_api_test::hermetic("s4b_rtltcp");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path dir = scratch("s4brtl");
    {
        Rig rig(dir);
        Access::useSource(rig.eng, std::make_unique<Tone>());
        CHECK(rig.status(testing::cmd(FOXAPI_OP_RUN, 0, 1)) == FOXAPI_OK);
        CHECK(rig.status(testing::cmd(FOXAPI_OP_SET_SQUELCH, -120.0)) == FOXAPI_OK);
        CHECK(rig.status(testing::cmd(FOXAPI_OP_SET_CENTRE, 100.0e6)) == FOXAPI_OK);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        rig.pass(4);

        // R1
        CHECK(rig.fact("srv.rtltcp.running") == "0" && rig.fact("srv.rtltcp.port").empty() && rig.fact("srv.rtltcp.scope").empty());
        CHECK(rig.fact("wirec.srv.rtltcp.enabled") == "0" && rig.fact("wirec.srv.rtltcp.all") == "0" && rig.fact("wirec.srv.rtltcp.port") == "1234" && rig.fact("wirec.srv.control.radio") == "1");
        CHECK(rig.status(setting("srv.rtltcp.port", "80")) != FOXAPI_OK);
        CHECK(rig.status(setting("srv.control.radio", "0")) != FOXAPI_OK);
        const int port = freePort();
        CHECK(port >= 1024);
        CHECK(rig.status(setting("srv.rtltcp.port", std::to_string(port))) == FOXAPI_OK);
        rig.pass(3);
        CHECK(rig.fact("srv.rtltcp.running") == "0");

        // R2
        CHECK(rig.status(setting("srv.rtltcp.enabled", "1")) == FOXAPI_OK);
        CHECK(until(rig, [&] { return rig.fact("srv.rtltcp.running") == "1"; }));
        CHECK(rig.fact("srv.rtltcp.port") == std::to_string(port) && rig.fact("srv.rtltcp.scope") == "this computer only");
        CHECK(until(rig, [&] { return rig.fact("srv.rtltcp.problem").empty(); }));
        nettest::Client c;
        CHECK(c.connect(static_cast<std::uint16_t>(port)));
        bool streamed = false;
        for (int i = 0; i < 100 && !streamed; ++i) { rig.pass(2); streamed = c.waitBytes(cascade::net::kRtlTcpHeaderBytes + 16000, 100); }
        CHECK(streamed);
        CHECK(c.got.size() >= cascade::net::kRtlTcpHeaderBytes && std::memcmp(c.got.data(), "RTL0", 4) == 0);
        if (c.got.size() >= cascade::net::kRtlTcpHeaderBytes + 16000) {
            double peak = 0.0, mean = 0.0;
            const std::size_t n = 8000;
            for (std::size_t k = 0; k < n; ++k) {
                const double v = static_cast<double>(c.got[cascade::net::kRtlTcpHeaderBytes + k]) - 127.5;
                peak = std::max(peak, std::fabs(v));
                mean += v;
            }
            mean /= static_cast<double>(n);
            CHECK(std::fabs(peak - 0.3 * 127.5) < 4.0);   // the tone at 0.3 of full scale
            CHECK(std::fabs(mean) < 3.0);                 // zero is 128 (127.5)
        }
        CHECK(until(rig, [&] { return rig.fact("srv.rtltcp.clients") == "1"; }));

        // R3: the client retunes the radio; its centre follows (150 MHz is far outside the 240 kHz it covers)
        sendCmd(c, 0x01, 150000000u);
        CHECK(until(rig, [&] { return rig.fact("srv.rtltcp.tunes") == "1"; }));
        CHECK(until(rig, [&] { return std::fabs(rig.local->state().tunedHz - 150.0e6) < 1.0; }));   // what the engine says it is tuned to
        CHECK(until(rig, [&] { return std::fabs(gCentre.load() - 150.0e6) < 400000.0; }, 3000));   // the stand-in radio's centre moved with it
        // a frequency this receiver cannot tune is refused and counted; the radio stays where it is
        const double before = gCentre.load();
        sendCmd(c, 0x01, 100u);
        CHECK(until(rig, [&] { return rig.fact("srv.rtltcp.tunes_refused") == "1"; }));
        CHECK(gCentre.load() == before);
        // every other command is counted and ignored: nothing of the radio changes
        const int rateBefore = gRateCalls.load();
        sendCmd(c, 0x02, 2400000u);   // sample rate
        sendCmd(c, 0x04, 300u);       // gain
        sendCmd(c, 0x0E, 1u);         // bias tee
        sendCmd(c, 0x77, 5u);         // not a command
        CHECK(until(rig, [&] { return rig.fact("srv.rtltcp.ignored") == "4"; }));
        CHECK(gRateCalls.load() == rateBefore && gCentre.load() == before);
        CHECK(rig.fact("srv.rtltcp.tunes") == "1");

        // R4
        nettest::Client second;
        CHECK(second.connect(static_cast<std::uint16_t>(port)));
        CHECK(second.waitEof(3000));
        CHECK(until(rig, [&] { return rig.fact("srv.rtltcp.refused_busy") == "1"; }));
        CHECK(until(rig, [&] { return rig.fact("srv.rtltcp.clients") == "1"; }));
        std::string msg;
        FoxCommand iq = testing::cmd(FOXAPP_OP_IQ_OUT_START, 0.0, 0, "{\"format\":\"u8\"}");
        iq.target = 1;
        CHECK(rig.status(iq, &msg) == FOXAPI_BUSY && msg.find("rtl_tcp") != std::string::npos);
        c.close();
        CHECK(rig.status(setting("srv.rtltcp.enabled", "0")) == FOXAPI_OK);
        CHECK(until(rig, [&] { return rig.fact("srv.rtltcp.running") == "0"; }));
        nettest::Client gone;
        CHECK(!gone.connect(static_cast<std::uint16_t>(port), 300));
        CHECK(rig.fact("srv.rtltcp.scope").empty());
        // the tap is free again: network I/Q out can have the radio
        const int p2 = freePort();
        FoxCommand iq2 = testing::cmd(FOXAPP_OP_IQ_OUT_START, 0.0, 0, ("{\"format\":\"u8\",\"port\":" + std::to_string(p2) + "}").c_str());
        iq2.target = 1;
        CHECK(rig.status(iq2) == FOXAPI_OK);
        FoxCommand stop = testing::cmd(FOXAPP_OP_IQ_OUT_STOP, 0.0, 0, nullptr);
        stop.target = 1;
        CHECK(rig.status(stop) == FOXAPI_OK);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    return check::finish("test_engine_api_s4b_rtltcp");
}
