// test_engine_api_wireb_rfexplorer.cpp - THE RF EXPLORER AS A SWEEP DEVICE THROUGH THE ENGINE (engine merge 3 registered the radio-bridges lane's source in wire-b's sweep host:
// FOXAPP_OP_SWEEP_OPEN with kind "rfexplorer"; engine/wire_b_sweep.{hpp,cpp}, source/rfexplorer_protocol.{hpp,cpp}). The device is a STAND-IN SerialLink written from the maker's UART
// specification (the same message layout as tests/test_rfexplorer_source.cpp): it answers "#<n>C0" with its set-up and configuration lines, takes "C2-F:<start kHz>,<stop kHz>,<top>,<bottom>",
// and then streams "$S" sweep frames carrying a tone off the grid over noise. No real COM port is opened anywhere in this file (the engine's factory is replaced).
//
//   R1  open: the worker connects, configures the span asked for (whole kHz) and sweeps; the fact counts them, the kind is "rfexplorer", the device's own point count is the line's, the port is in NO fact
//   R2  read: the newest line peaks at the tone, which is 12.345 kHz off any grid point
//   R3  refusals: a baud that is not on the maker's list, a port that is not a COM name, bottom not below top, stop not above start, a key of another kind, an unknown key, a REMOTE session
//   R4  a device that never answers ends the worker with a sentence that names no port; a link that cannot be made is FAILED with the link's own sentence
//   R5  close ends the worker and sends the device its hold command; a second close is NO_CHANGE; a second open while one is open is BUSY
//
// UNVERIFIED on a real RF Explorer.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "engine/engine_wire_b.hpp"
#include "wire1_rig.hpp"

namespace {

namespace tsa = cascade::source::tinysa;
using Bytes = std::vector<std::uint8_t>;

Bytes bytesOf(const std::string& s) { return Bytes(s.begin(), s.end()); }

std::string configText(long startKHz, long stepHz, int top, int bottom, long points) {
    char b[200];
    int n = std::snprintf(b, sizeof b, "%07ld,%07ld,%04d,%04d,%04ld,%d,%03d,%07ld,%07ld,%07ld", startKHz, stepHz, top, bottom, points, 0, 0, 15000L, 2700000L, 112000L);
    n += std::snprintf(b + n, sizeof b - static_cast<std::size_t>(n), ",%05d,%04d,%03d", 100, 0, 0);
    return std::string(b, static_cast<std::size_t>(n));
}

// The stand-in device: bytes in, bytes out, under one lock.
struct Device {
    std::mutex mu;
    Bytes out, inbuf;
    long startKHz = 100000, stepHz = 898000, points = 112;
    int top = -10, bottom = -120;
    bool silent = false, running = true;
    double toneHz = 150012345.0;
    std::vector<std::string> commands;
    std::mt19937 rng{4242};
    std::uint64_t sweepsSent = 0;

    void queue(const Bytes& b) { out.insert(out.end(), b.begin(), b.end()); }
    void announce() {
        char s[64];
        std::snprintf(s, sizeof s, "#C2-M:%03d,%03d,%s\r\n", 5, 255, "01.12");
        queue(bytesOf(s));
        queue(bytesOf("#C2-F:" + configText(startKHz, stepHz, top, bottom, points) + "\r\n"));
    }
    void sweep() {
        std::vector<std::uint8_t> a(static_cast<std::size_t>(points));
        std::normal_distribution<double> nd(0.0, 1.5);
        for (long i = 0; i < points; ++i) {
            const double f = static_cast<double>(startKHz) * 1000.0 + static_cast<double>(stepHz) * static_cast<double>(i);
            double level = -100.0 + nd(rng);
            const double d = (f - toneHz) / static_cast<double>(stepHz);
            level += 60.0 * std::exp(-d * d / 2.0);
            a[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(std::lround(std::clamp(-level * 2.0, 0.0, 255.0)));
        }
        Bytes b{'$', 'S', static_cast<std::uint8_t>(points)};
        b.insert(b.end(), a.begin(), a.end());
        b.push_back(0x0D);
        b.push_back(0x0A);
        queue(b);
        ++sweepsSent;
    }
    void onBytes(const std::uint8_t* d, std::size_t n) {
        std::lock_guard<std::mutex> lk(mu);
        inbuf.insert(inbuf.end(), d, d + n);
        while (inbuf.size() >= 2) {
            if (inbuf[0] != '#') { inbuf.erase(inbuf.begin()); continue; }
            const std::size_t size = inbuf[1];
            if (size < 3 || size > 64) { inbuf.erase(inbuf.begin()); continue; }
            if (inbuf.size() < size) { return; }
            const std::string body(reinterpret_cast<const char*>(&inbuf[2]), size - 2);
            inbuf.erase(inbuf.begin(), inbuf.begin() + static_cast<std::ptrdiff_t>(size));
            commands.push_back(body);
            if (silent) { continue; }
            if (body == "C0") {
                announce();
            } else if (body == "CH") {
                running = false;
            } else if (body.compare(0, 5, "C2-F:") == 0) {
                long s = 0, e = 0;
                int t = 0, b = 0;
                if (std::sscanf(body.c_str() + 5, "%ld,%ld,%d,%d", &s, &e, &t, &b) == 4) {
                    startKHz = s;
                    stepHz = (e - s) * 1000 / (points - 1);
                    top = t;
                    bottom = b;
                    announce();
                }
            }
        }
    }
    Bytes pull(std::size_t cap) {
        std::lock_guard<std::mutex> lk(mu);
        if (out.empty() && running && !silent && sweepsSent < 100000) { sweep(); }
        const std::size_t n = std::min(cap, out.size());
        Bytes r(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n));
        out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n));
        return r;
    }
    bool saw(const std::string& cmd) {
        std::lock_guard<std::mutex> lk(mu);
        return std::find(commands.begin(), commands.end(), cmd) != commands.end();
    }
    std::string configCommand() {
        std::lock_guard<std::mutex> lk(mu);
        for (const std::string& c : commands) { if (c.compare(0, 5, "C2-F:") == 0) { return c; } }
        return std::string();
    }
};

struct Link final : tsa::SerialLink {
    std::shared_ptr<Device> dev;
    explicit Link(std::shared_ptr<Device> d) : dev(std::move(d)) {}
    bool write(const std::uint8_t* d, std::size_t n) override { dev->onBytes(d, n); return true; }
    long read(std::uint8_t* b, std::size_t cap) override {
        const Bytes r = dev->pull(std::min<std::size_t>(cap, 97));
        if (r.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); return 0; }
        std::memcpy(b, r.data(), r.size());
        return static_cast<long>(r.size());
    }
};

testing::Outcome sendText(Rig& r, std::uint32_t op, const std::string& text, testing::Session* who = nullptr) {
    FoxCommand c = testing::cmd(op, 0.0, 0, nullptr);
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

std::string bigFact(Rig& r, const std::string& key) {
    std::string buf(1 << 18, '\0');
    const int32_t n = api()->get_setting(r.s->raw(), ("fact." + key).c_str(), &buf[0], buf.size());
    return n >= 0 ? std::string(buf.c_str()) : std::string("<absent>");
}

json state(Rig& r) {
    const json j = json::parse(bigFact(r, "sweep.state"), nullptr, false);
    return j.is_discarded() ? json() : j;
}

const char* kOpen = "{\"kind\":\"rfexplorer\",\"port\":\"COM7\",\"start_mhz\":149,\"stop_mhz\":151,\"top_dbm\":-20,\"bottom_dbm\":-110}";

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_rfexplorer\n");
    engine_api_test::hermetic("wirebrfe");
    Access::installHooks();
    Rig r;
    cascade::engine::WireBState& w = r.eng.wireBStateForTest();
    std::shared_ptr<Device> dev;
    std::string refuseWith;
    int baudSeen = 0;
    w.rfeFactory = [&](const std::string& port, int baud, std::string& why) -> std::unique_ptr<tsa::SerialLink> {
        (void)port;
        baudSeen = baud;
        if (!refuseWith.empty()) { why = refuseWith; return nullptr; }
        dev = std::make_shared<Device>();
        return std::make_unique<Link>(dev);
    };
    w.sweepFactory = [](const std::string&, std::string& why) -> std::unique_ptr<tsa::SerialLink> { why = "no port in this test"; return nullptr; };

    std::printf("R1: open\n");
    CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, kOpen).status(), FOXAPI_OK);
    CHECK_EQ(baudSeen, 500000);   // the maker's default
    CHECK(r.until([&] { const json j = state(r); return j.is_object() && j["sweeps"].get<std::uint64_t>() >= 6; }, 30000));
    {
        const json j = state(r);
        std::printf("  sweeps %llu, points %zu, heatmap %zu x %zu\n", static_cast<unsigned long long>(j["sweeps"].get<std::uint64_t>()), j["points"].get<std::size_t>(), j["heat_rows"].get<std::size_t>(),
                    j["heat_cols"].get<std::size_t>());
        CHECK(j["open"].get<bool>());
        CHECK_EQ(j["kind"].get<std::string>(), std::string("rfexplorer"));
        CHECK_EQ(j["points"].get<std::size_t>(), static_cast<std::size_t>(112));   // the device's own
        CHECK(j["heat_rows"].get<std::size_t>() >= 1);
        CHECK(bigFact(r, "sweep.state").find("COM7") == std::string::npos);
        CHECK_EQ(dev->configCommand(), std::string("C2-F:0149000,0151000,-020,-110"));   // whole kHz, the display's top and bottom, in the device's zero-padded fields
    }

    std::printf("R2: read\n");
    {
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_READ, "{\"max_points\":112}").status(), FOXAPI_OK);
        const json j = json::parse(bigFact(r, "sweep.read"), nullptr, false);
        CHECK(!j.is_discarded());
        if (!j.is_discarded()) {
            std::size_t best = 0;
            for (std::size_t i = 0; i < j["db"].size(); ++i) { if (j["db"][i].is_number() && (j["db"][best].is_null() || j["db"][i].get<double>() > j["db"][best].get<double>())) { best = i; } }
            const double hz = j["start_hz"].get<double>() + (j["stop_hz"].get<double>() - j["start_hz"].get<double>()) * static_cast<double>(best) / static_cast<double>(j["points"].get<std::size_t>() - 1);
            std::printf("  peak at %.4f MHz (%.1f dB)\n", hz / 1e6, j["db"][best].get<double>());
            CHECK(std::fabs(hz - 150.012345e6) < 0.03e6);   // within a grid step (about 18 kHz) of the tone
            CHECK(j["db"][best].get<double>() > -60.0);
            CHECK(std::fabs(j["start_hz"].get<double>() - 149.0e6) < 1.0 && std::fabs(j["stop_hz"].get<double>() - 151.0e6) < 200.0);
        }
    }

    std::printf("R5: busy, close\n");
    CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, kOpen).status(), FOXAPI_BUSY);
    CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_CLOSE, "").status(), FOXAPI_OK);
    CHECK(r.until([&] { return !state(r)["open"].get<bool>(); }, 10000));
    CHECK(dev->saw("CH"));   // the worker told the analyser to stop its data dump
    CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_CLOSE, "").status(), FOXAPI_NO_CHANGE);

    std::printf("R3: refusals (no link is made for any of them)\n");
    {
        int made = 0;
        w.rfeFactory = [&](const std::string&, int, std::string& why) -> std::unique_ptr<tsa::SerialLink> { ++made; why = "must not be reached"; return nullptr; };
        const auto bad = [&](const char* text, int32_t want, const char* word) {
            const auto o = sendText(r, FOXAPP_OP_SWEEP_OPEN, text);
            CHECK_EQ(o.status(), want);
            if (word != nullptr) { CHECK(std::string(o.result.message).find(word) != std::string::npos); }
        };
        bad("{\"kind\":\"rfexplorer\",\"port\":\"COM7\",\"start_mhz\":149,\"stop_mhz\":151,\"baud\":12345}", FOXAPI_BAD_ARGUMENT, "baud");
        bad("{\"kind\":\"rfexplorer\",\"port\":\"COM7\",\"start_mhz\":149,\"stop_mhz\":151,\"baud\":9600.5}", FOXAPI_BAD_ARGUMENT, "baud");
        bad("{\"kind\":\"rfexplorer\",\"port\":\"LPT1\",\"start_mhz\":149,\"stop_mhz\":151}", FOXAPI_BAD_ARGUMENT, "COM");
        bad("{\"kind\":\"rfexplorer\",\"port\":\"COM999\",\"start_mhz\":149,\"stop_mhz\":151}", FOXAPI_BAD_ARGUMENT, "COM");
        bad("{\"kind\":\"rfexplorer\",\"start_mhz\":149,\"stop_mhz\":151}", FOXAPI_BAD_ARGUMENT, "COM");   // no port typed: nothing opens
        bad("{\"kind\":\"rfexplorer\",\"port\":\"COM7\",\"start_mhz\":151,\"stop_mhz\":149}", FOXAPI_BAD_ARGUMENT, "above");
        bad("{\"kind\":\"rfexplorer\",\"port\":\"COM7\",\"start_mhz\":149.0001,\"stop_mhz\":149.0002}", FOXAPI_BAD_ARGUMENT, "above");   // both round to the same kHz
        bad("{\"kind\":\"rfexplorer\",\"port\":\"COM7\",\"start_mhz\":149,\"stop_mhz\":151,\"top_dbm\":-50,\"bottom_dbm\":-50}", FOXAPI_BAD_ARGUMENT, "below");
        bad("{\"kind\":\"rfexplorer\",\"port\":\"COM7\",\"start_mhz\":149,\"stop_mhz\":151,\"top_dbm\":10}", FOXAPI_BAD_ARGUMENT, "top_dbm");
        bad("{\"kind\":\"rfexplorer\",\"port\":\"COM7\",\"start_mhz\":149,\"stop_mhz\":151,\"points\":50}", FOXAPI_BAD_ARGUMENT, "RF Explorer");
        bad("{\"kind\":\"rfexplorer\",\"port\":\"COM7\",\"start_mhz\":149,\"stop_mhz\":151,\"colour\":1}", FOXAPI_BAD_ARGUMENT, "colour");
        bad("{\"kind\":\"tinysa\",\"port\":\"COM7\",\"start_mhz\":149,\"stop_mhz\":151,\"baud\":9600}", FOXAPI_BAD_ARGUMENT, "RF Explorer");   // another kind's key
        bad("{\"kind\":\"spectran\",\"port\":\"COM7\",\"start_mhz\":149,\"stop_mhz\":151}", FOXAPI_BAD_ARGUMENT, "rfexplorer");
        CHECK_EQ(made, 0);
        testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
        CHECK(remote.ok());
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, kOpen, &remote).status(), FOXAPI_DENIED);
        CHECK_EQ(made, 0);
    }

    std::printf("R4: a silent device, a link that cannot be made\n");
    {
        refuseWith.clear();
        w.rfeFactory = [&](const std::string&, int baud, std::string& why) -> std::unique_ptr<tsa::SerialLink> {
            baudSeen = baud;
            if (!refuseWith.empty()) { why = refuseWith; return nullptr; }
            dev = std::make_shared<Device>();
            dev->silent = true;
            return std::make_unique<Link>(dev);
        };
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"rfexplorer\",\"port\":\"COM8\",\"baud\":115200,\"start_mhz\":149,\"stop_mhz\":151}").status(), FOXAPI_OK);
        CHECK_EQ(baudSeen, 115200);
        CHECK(r.until([&] { const json j = state(r); return j.is_object() && !j["open"].get<bool>() && !j["last_error"].get<std::string>().empty(); }, 60000));
        const json j = state(r);
        std::printf("  last error: %s\n", j["last_error"].get<std::string>().c_str());
        CHECK(j["last_error"].get<std::string>().find("no RF Explorer answered") != std::string::npos);
        CHECK(bigFact(r, "sweep.state").find("COM8") == std::string::npos);
        CHECK_EQ(j["sweeps"].get<std::uint64_t>(), static_cast<std::uint64_t>(0));
        refuseWith = "there is no such port";
        const auto f = sendText(r, FOXAPP_OP_SWEEP_OPEN, kOpen);
        CHECK_EQ(f.status(), FOXAPI_FAILED);
        CHECK(std::string(f.result.message).find("no such port") != std::string::npos);
        refuseWith.clear();
    }
    return check::finish("test_engine_api_wireb_rfexplorer");
}
