// test_engine_api_wireb_iqout.cpp - NETWORK I/Q OUT THROUGH THE ENGINE (wire-b; FOXAPP_OP_IQ_OUT_START 0x8924, FOXAPP_OP_IQ_OUT_STOP 0x8925; Pipeline::setIqSink over net/iq_out.hpp), on a real Engine with
// a +15 kHz tone source at 240 kS/s centred on 100 MHz. The client is a plain loopback socket in this test.
//
//   I1  OFF by default: no output fact, nothing listens, STOP is NO_CHANGE
//   I2  TCP, 16-bit, with the header: the greeting is the 28-byte FXIQ header (format, rate, centre), then the samples: the tone is 9830 +- 5 % in amplitude and turns 0.3927 rad a sample; the fact counts
//       samples in and the accepted client; a second start is BUSY
//   I3  UDP with the header: a datagram starts with the header and its sequence counts up
//   I4  STOP ends the stream (the client reads end of stream), the fact goes, STOP again is NO_CHANGE; removing a radio that sends stops its output
//   I5  refusals: a bind key (there is none: the output binds 127.0.0.1 only), another destination, a bad format, UDP without a port, a REMOTE session
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "net/iq_out.hpp"
#include "wire1_rig.hpp"

namespace {

constexpr double kPi = 3.14159265358979323846;

#ifdef _WIN32
using Sk = SOCKET;
constexpr Sk kBad = INVALID_SOCKET;
void closeSk(Sk s) { ::closesocket(s); }
void startNetOnce() { static WSADATA d; static bool done = (::WSAStartup(MAKEWORD(2, 2), &d) == 0); (void)done; }
#else
using Sk = int;
constexpr Sk kBad = -1;
void closeSk(Sk s) { ::close(s); }
void startNetOnce() {}
#endif

class Tone final : public cascade::source::IqSource {
public:
    bool start() override { running_ = true; return true; }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return 240000.0; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return center_; }
    bool setCenterFrequencyHz(double hz) override { center_ = hz; return true; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        for (std::size_t i = 0; i < n; ++i, ++k_) {
            const double a = 2.0 * kPi * 15000.0 * static_cast<double>(k_) / 240000.0;
            dst[i] = {static_cast<float>(0.3 * std::cos(a)), static_cast<float>(0.3 * std::sin(a))};
        }
        return n;
    }
    const char* name() const override { return "wireb-tone"; }
    const char* lastError() const override { return ""; }

private:
    bool running_ = false;
    double center_ = 100.0e6;
    std::uint64_t k_ = 0;
};

testing::Outcome sendText(Rig& r, std::uint32_t op, std::uint32_t radio, const std::string& text, testing::Session* who = nullptr) {
    FoxCommand c = testing::cmd(op, 0.0, 0, nullptr);
    c.target = radio;
    testing::Session& sess = who != nullptr ? *who : *r.s;
    testing::Outcome o;
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = text.empty() ? api()->submit(sess.raw(), &c, 1, &sr) : api()->submit_text(sess.raw(), &c, text.data(), static_cast<uint32_t>(text.size()), &sr);
    o.submitStatus = n < 0 ? n : sr.status;
    if (o.submitStatus != FOXAPI_OK) { return o; }
    for (int pass = 0; pass < 60 && !o.landed; ++pass) {
        r.eng.controlPass();
        FoxCommandResult res[16];
        for (auto& x : res) { x.structSize = sizeof(x); }
        const int32_t got = api()->poll_results(sess.raw(), res, 16);
        for (int32_t i = 0; i < got; ++i) { if (res[i].ticket == sr.ticket) { o.landed = true; o.result = res[i]; } }
    }
    return o;
}

json iqFact(Rig& r, int radio = 1) {
    const std::string f = r.fact("radio." + std::to_string(radio) + ".iqout");
    const json j = json::parse(f, nullptr, false);
    return j.is_discarded() ? json() : j;
}

Sk connectTcp(std::uint16_t port) {
    startNetOnce();
    const Sk s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kBad) { return kBad; }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { closeSk(s); return kBad; }
#ifdef _WIN32
    DWORD tmo = 3000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
#else
    timeval tv{3, 0};
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    return s;
}

// Reads exactly n bytes (or fewer on end of stream / timeout); returns the count.
std::size_t readN(Sk s, std::uint8_t* dst, std::size_t n) {
    std::size_t got = 0;
    while (got < n) {
        const int k = ::recv(s, reinterpret_cast<char*>(dst + got), static_cast<int>(n - got), 0);
        if (k <= 0) { break; }
        got += static_cast<std::size_t>(k);
    }
    return got;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_iqout\n");
    engine_api_test::hermetic("wirebiqout");
    Access::installHooks();
    Rig r;
    Access::useSource(r.eng, std::make_unique<Tone>());
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_SQUELCH, -120.0)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_CENTRE, 100.0e6)).status(), FOXAPI_OK);
    r.until([] { return false; }, 1500);

    std::printf("I1: off by default\n");
    CHECK(iqFact(r).is_null());
    CHECK_EQ(sendText(r, FOXAPP_OP_IQ_OUT_STOP, 1, "").status(), FOXAPI_NO_CHANGE);

    std::printf("I2: TCP, 16-bit, header\n");
    std::uint16_t port = 0;
    {
        const auto o = sendText(r, FOXAPP_OP_IQ_OUT_START, 1, "{\"format\":\"s16\",\"header\":true}");
        CHECK_EQ(o.status(), FOXAPI_OK);
        port = static_cast<std::uint16_t>(o.result.applied[0]);
        CHECK(port != 0);
        CHECK_EQ(sendText(r, FOXAPP_OP_IQ_OUT_START, 1, "{}").status(), FOXAPI_BUSY);
        const Sk s = connectTcp(port);
        CHECK(s != kBad);
        if (s != kBad) {
            std::uint8_t hdr[cascade::net::kIqHeaderBytes];
            CHECK_EQ(readN(s, hdr, sizeof(hdr)), sizeof(hdr));
            cascade::net::IqHeader h;
            CHECK(cascade::net::iqReadHeader(hdr, sizeof(hdr), h));
            std::printf("  header: format %d, rate %u, centre %.0f\n", static_cast<int>(h.format), h.sampleRateHz, h.centreHz);
            CHECK(h.format == cascade::net::IqFormat::S16);
            CHECK_EQ(h.sampleRateHz, 240000u);
            CHECK_EQ(h.centreHz, 100.0e6);
            std::vector<std::uint8_t> raw(4 * 4096);
            CHECK_EQ(readN(s, raw.data(), raw.size()), raw.size());
            const std::int16_t* v = reinterpret_cast<const std::int16_t*>(raw.data());
            double amp = 0.0, step = 0.0;
            const std::size_t n = raw.size() / 4;
            for (std::size_t i = 0; i < n; ++i) { amp += std::hypot(static_cast<double>(v[2 * i]), static_cast<double>(v[2 * i + 1])); }
            amp /= static_cast<double>(n);
            for (std::size_t i = 1; i < n; ++i) {
                const std::complex<double> a(v[2 * i - 2], v[2 * i - 1]), b(v[2 * i], v[2 * i + 1]);
                step += std::arg(b * std::conj(a));
            }
            step /= static_cast<double>(n - 1);
            std::printf("  amplitude %.0f (wanted about 9830), step %.4f rad (wanted 0.3927)\n", amp, step);
            CHECK(std::fabs(amp - 0.3 * 32767.0) < 0.05 * 0.3 * 32767.0);
            CHECK(std::fabs(step - 2.0 * kPi * 15000.0 / 240000.0) < 0.01);
            CHECK(r.until([&] { const json f = iqFact(r); return f.is_object() && f["samples_in"].get<std::uint64_t>() > 0 && f["clients_accepted"].get<std::uint64_t>() >= 1; }, 15000));
            const json j = iqFact(r);
            CHECK(j.is_object());
            if (j.is_object()) {
                CHECK(j["running"].get<bool>() && !j["udp"].get<bool>());
                CHECK_EQ(j["port"].get<int>(), static_cast<int>(port));
                CHECK_EQ(j["format"].get<std::string>(), std::string("s16"));
                CHECK(j["samples_in"].get<std::uint64_t>() > 0);
                CHECK(j["clients_accepted"].get<std::uint64_t>() >= 1);
            }
            // I4 (stop): the client reads the end of the stream
            CHECK_EQ(sendText(r, FOXAPP_OP_IQ_OUT_STOP, 1, "").status(), FOXAPI_OK);
            std::uint8_t sink[65536];
            std::size_t total = 0;
            int k = 1;
            for (int i = 0; i < 200 && k > 0; ++i) { k = ::recv(s, reinterpret_cast<char*>(sink), sizeof(sink), 0); if (k > 0) { total += static_cast<std::size_t>(k); } }
            CHECK(k <= 0);   // end of stream (or reset), not a timeout that was never reached
            closeSk(s);
            CHECK(iqFact(r).is_null());
            CHECK_EQ(sendText(r, FOXAPP_OP_IQ_OUT_STOP, 1, "").status(), FOXAPI_NO_CHANGE);
            (void)total;
        }
    }

    std::printf("I3: UDP with the header\n");
    {
        startNetOnce();
        const Sk u = ::socket(AF_INET, SOCK_DGRAM, 0);
        CHECK(u != kBad);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = 0;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(::bind(u, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
        socklen_t len = sizeof(a);
        ::getsockname(u, reinterpret_cast<sockaddr*>(&a), &len);
        const unsigned up = ntohs(a.sin_port);
#ifdef _WIN32
        DWORD tmo = 3000;
        ::setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
#else
        timeval tv{3, 0};
        ::setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        CHECK_EQ(sendText(r, FOXAPP_OP_IQ_OUT_START, 1, "{\"udp\":true,\"port\":" + std::to_string(up) + ",\"format\":\"f32\",\"header\":true}").status(), FOXAPI_OK);
        std::uint8_t pkt[65536];
        std::uint32_t lastSeq = 0;
        int got = 0;
        for (int i = 0; i < 6; ++i) {
            const int n = ::recv(u, reinterpret_cast<char*>(pkt), sizeof(pkt), 0);
            if (n < static_cast<int>(cascade::net::kIqHeaderBytes)) { continue; }
            cascade::net::IqHeader h;
            if (!cascade::net::iqReadHeader(pkt, static_cast<std::size_t>(n), h)) { continue; }
            CHECK(h.format == cascade::net::IqFormat::F32);
            if (got > 0) { CHECK(h.sequence > lastSeq); }
            lastSeq = h.sequence;
            ++got;
        }
        std::printf("  %d datagram(s) with a header, last sequence %u\n", got, lastSeq);
        CHECK(got >= 3);
        CHECK(iqFact(r).is_object() && iqFact(r)["udp"].get<bool>());
        CHECK_EQ(sendText(r, FOXAPP_OP_IQ_OUT_STOP, 1, "").status(), FOXAPI_OK);
        closeSk(u);
    }

    std::printf("I4b: removing a radio that sends stops its output\n");
    {
        CHECK_EQ(r.add(2, "generator", true).status(), FOXAPI_OK);
        CHECK_EQ(sendText(r, FOXAPP_OP_IQ_OUT_START, 2, "{}").status(), FOXAPI_OK);
        CHECK(iqFact(r, 2).is_object());
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_REMOVE, 0.0, 2)).status(), FOXAPI_OK);
        r.pump(5);
        CHECK(iqFact(r, 2).is_null());
        // the engine goes on: the first radio can send again
        CHECK_EQ(sendText(r, FOXAPP_OP_IQ_OUT_START, 1, "{}").status(), FOXAPI_OK);   // (stopped by engine shutdown at the end: nothing is left running)
    }

    std::printf("I5: refusals\n");
    {
        CHECK_EQ(sendText(r, FOXAPP_OP_IQ_OUT_STOP, 1, "").status(), FOXAPI_OK);
        const auto bind = sendText(r, FOXAPP_OP_IQ_OUT_START, 1, "{\"bind\":\"0.0.0.0\"}");
        CHECK_EQ(bind.status(), FOXAPI_BAD_ARGUMENT);
        CHECK(std::string(bind.result.message).find("bind") != std::string::npos);
        CHECK_EQ(sendText(r, FOXAPP_OP_IQ_OUT_START, 1, "{\"udp\":true,\"port\":9,\"dest\":\"192.168.1.5\"}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_IQ_OUT_START, 1, "{\"format\":\"u16\"}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_IQ_OUT_START, 1, "{\"udp\":true}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_IQ_OUT_START, 1, "{\"port\":70000}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_IQ_OUT_START, 7, "{}").status(), FOXAPI_NO_DEVICE);
        testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
        CHECK(remote.ok());
        CHECK_EQ(sendText(r, FOXAPP_OP_IQ_OUT_START, 1, "{}", &remote).status(), FOXAPI_DENIED);
        CHECK(iqFact(r).is_null());
    }
    return check::finish("test_engine_api_wireb_iqout");
}
