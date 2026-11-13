// test_engine_api_wireb_sweep.cpp - THE SWEEP DEVICE HOST THROUGH THE ENGINE (wire-b; FOXAPP_OP_SWEEP_OPEN/CLOSE/CONFIG/READ 0x8920-0x8923; engine/wire_b_sweep.{hpp,cpp} over source/tinysa_protocol.hpp,
// core/sweep_stitch.hpp, core/sweep_occupancy.hpp). The device is a STAND-IN SerialLink that answers the tinySA scanraw command with a frame (a tone at 100 MHz over a noise floor): no test opens a COM port.
//
//   S1  SWEEP_OPEN: the worker sweeps (the fact counts them), the heatmap fills, the port name is in NO fact
//   S2  SWEEP_READ: the newest line thinned to the points asked for; its peak is at the tone; the click-to-tune answer for the middle of the picture is the middle of the range
//   S3  SWEEP_CONFIG occupancy: the channel holding the tone reads occupied almost all the time, the others almost never; a bad plan is refused with the model's words
//   S4  a second open is BUSY, close ends the worker, closing again is NO_CHANGE
//   S5  a device that stops answering ends the worker with a sentence and the engine goes on; a link that cannot be made is FAILED with a sentence
//   S6  refusals: bad kind, a port that is not a COM name, stop under start, points 1, unknown key, no sweep yet; a REMOTE session is DENIED
//
// UNVERIFIED on a real tinySA.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine/engine_wire_b.hpp"
#include "wire1_rig.hpp"

namespace {

namespace tsa = cascade::source::tinysa;

// A tinySA that answers a "scanraw <start> <stop> <points> <option>" with echo bytes, then '{' (x LSB MSB)*points '}'. After `dieAfter` sweeps it stops answering (read -> 0) or fails (-1).
class FakeTinySa final : public tsa::SerialLink {
public:
    std::atomic<int> sweeps{0};
    std::atomic<int> dieAfter{-1};
    std::atomic<int> dieMode{0};   // 0 silence, 1 link failure
    bool write(const std::uint8_t* data, std::size_t n) override {
        const std::string cmd(reinterpret_cast<const char*>(data), n);
        unsigned long long a = 0, b = 0;
        unsigned long long pts = 0;
        if (std::sscanf(cmd.c_str(), "scanraw %llu %llu %llu", &a, &b, &pts) != 3 || pts < 2) { return false; }
        if (dieAfter.load() >= 0 && sweeps.load() >= dieAfter.load()) { return true; }   // nothing is queued: the next reads time out
        ++sweeps;
        std::string frame = cmd;   // the device echoes the command first
        frame.push_back('{');
        for (unsigned long long i = 0; i < pts; ++i) {
            const double f = static_cast<double>(a) + (static_cast<double>(b - a) * static_cast<double>(i)) / static_cast<double>(pts - 1);
            const double db = std::fabs(f - 100.0e6) < 150000.0 ? -40.0 : -95.0 + 2.0 * std::sin(0.37 * static_cast<double>(i));
            const std::uint16_t raw = static_cast<std::uint16_t>(std::lround((db + 128.0) * 32.0));
            frame.push_back('x');
            frame.push_back(static_cast<char>(raw & 0xFFu));
            frame.push_back(static_cast<char>(raw >> 8));
        }
        frame.push_back('}');
        frame += "\r\nch> ";
        std::lock_guard<std::mutex> lk(m_);
        for (const char ch : frame) { q_.push_back(static_cast<std::uint8_t>(ch)); }
        return true;
    }
    long read(std::uint8_t* buf, std::size_t cap) override {
        std::lock_guard<std::mutex> lk(m_);
        if (q_.empty()) {
            if (dieAfter.load() >= 0 && sweeps.load() >= dieAfter.load() && dieMode.load() == 1) { return -1; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return 0;
        }
        std::size_t n = 0;
        while (n < cap && n < 512 && !q_.empty()) { buf[n++] = q_.front(); q_.pop_front(); }
        return static_cast<long>(n);
    }

private:
    std::mutex m_;
    std::deque<std::uint8_t> q_;
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

const char* kOpen = "{\"kind\":\"tinysa\",\"port\":\"COM7\",\"start_mhz\":88,\"stop_mhz\":108,\"points\":450,\"interval_ms\":5}";

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_sweep\n");
    engine_api_test::hermetic("wirebsweep");
    Access::installHooks();
    Rig r;
    cascade::engine::WireBState& w = r.eng.wireBStateForTest();
    FakeTinySa* fake = nullptr;
    std::string refuseWith;
    w.sweepFactory = [&](const std::string& port, std::string& why) -> std::unique_ptr<tsa::SerialLink> {
        (void)port;
        if (!refuseWith.empty()) { why = refuseWith; return nullptr; }
        auto f = std::make_unique<FakeTinySa>();
        fake = f.get();
        return f;
    };

    std::printf("S6a: nothing open yet\n");
    CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_READ, "{}").status(), FOXAPI_FAILED);
    CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_CLOSE, "").status(), FOXAPI_NO_CHANGE);

    std::printf("S1: open\n");
    CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, kOpen).status(), FOXAPI_OK);
    CHECK(r.until([&] { const json j = state(r); return j.is_object() && j["sweeps"].get<std::uint64_t>() >= 8; }, 20000));
    {
        const json j = state(r);
        std::printf("  sweeps %llu, heatmap %zu rows x %zu cols\n", static_cast<unsigned long long>(j["sweeps"].get<std::uint64_t>()), j["heat_rows"].get<std::size_t>(), j["heat_cols"].get<std::size_t>());
        CHECK(j["open"].get<bool>());
        CHECK_EQ(j["kind"].get<std::string>(), std::string("tinysa"));
        CHECK_EQ(j["heat_cols"].get<std::size_t>(), static_cast<std::size_t>(1024));
        CHECK(j["heat_rows"].get<std::size_t>() >= 1);
        CHECK(bigFact(r, "sweep.state").find("COM7") == std::string::npos);   // the port name is in no fact
    }

    std::printf("S2: read\n");
    {
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_READ, "{\"max_points\":100,\"x\":0.5}").status(), FOXAPI_OK);
        const json j = json::parse(bigFact(r, "sweep.read"), nullptr, false);
        CHECK(!j.is_discarded());
        if (!j.is_discarded()) {
            CHECK(j["points"].get<std::size_t>() <= 100 && j["points"].get<std::size_t>() >= 50);
            std::size_t best = 0;
            for (std::size_t i = 0; i < j["db"].size(); ++i) { if (j["db"][i].is_number() && (j["db"][best].is_null() || j["db"][i].get<double>() > j["db"][best].get<double>())) { best = i; } }
            const double hz = j["start_hz"].get<double>() + (j["stop_hz"].get<double>() - j["start_hz"].get<double>()) * static_cast<double>(best) / static_cast<double>(j["points"].get<std::size_t>() - 1);
            std::printf("  peak at %.2f MHz (%.1f dB), click at the middle: %.3f MHz\n", hz / 1e6, j["db"][best].get<double>(), j["click_hz"].get<double>() / 1e6);
            CHECK(std::fabs(hz - 100.0e6) < 0.5e6);
            CHECK(j["db"][best].get<double>() > -45.0);
            CHECK(std::fabs(j["click_hz"].get<double>() - 98.0e6) < 0.1e6);
            CHECK(std::fabs(j["start_hz"].get<double>() - 88.0e6) < 1.0 && std::fabs(j["stop_hz"].get<double>() - 108.0e6) < 1.0);
        }
        CHECK(bigFact(r, "sweep.read").find("COM7") == std::string::npos);
    }

    std::printf("S3: occupancy\n");
    {
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_CONFIG, "{\"occupancy\":{\"start_mhz\":88,\"stop_mhz\":108,\"step_khz\":1000,\"margin_db\":10}}").status(), FOXAPI_OK);
        CHECK(r.until([&] { return state(r)["occ_sweeps"].get<std::size_t>() >= 10; }, 20000));
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_READ, "{\"occupancy\":true}").status(), FOXAPI_OK);
        const json j = json::parse(bigFact(r, "sweep.read"), nullptr, false);
        const std::string csv = j.is_discarded() ? std::string() : j["occupancy_csv"].get<std::string>();
        CHECK(!csv.empty());
        // the channel table: channel,start_hz,stop_hz,observed_s,occupied_s,percent - the channel that holds 100 MHz is occupied nearly all the time it was observed, the rest almost never
        {
            std::size_t pos = csv.find("percent\n");
            std::size_t tone = 0, quiet = 0, rows = 0;
            if (pos != std::string::npos) {
                pos += 8;
                while (pos < csv.size()) {
                    const std::size_t eol = csv.find('\n', pos);
                    const std::string line = csv.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
                    pos = eol == std::string::npos ? csv.size() : eol + 1;
                    double start = 0.0, stop = 0.0, pct = -1.0;
                    int ch = 0;
                    double obs = 0.0, occ = 0.0;
                    if (std::sscanf(line.c_str(), "%d,%lf,%lf,%lf,%lf,%lf", &ch, &start, &stop, &obs, &occ, &pct) != 6) { continue; }
                    ++rows;
                    if (start <= 100.0e6 && 100.0e6 < stop) { if (pct > 80.0) { ++tone; } } else if (pct < 20.0) { ++quiet; }
                }
            }
            std::printf("  %zu channel rows: the tone channel occupied %zu, quiet channels %zu\n", rows, tone, quiet);
            CHECK_EQ(rows, static_cast<std::size_t>(20));
            CHECK_EQ(tone, static_cast<std::size_t>(1));
            CHECK(quiet >= 17);   // (the tone has a 150 kHz skirt, so the channel just under 100 MHz may count as occupied too)
        }
        std::printf("  occupancy csv: %zu bytes, %zu channels\n", csv.size(), state(r)["occ_channels"].get<std::size_t>());
        CHECK_EQ(state(r)["occ_channels"].get<std::size_t>(), static_cast<std::size_t>(20));
        const auto bad = sendText(r, FOXAPP_OP_SWEEP_CONFIG, "{\"occupancy\":{\"start_mhz\":108,\"stop_mhz\":88,\"step_khz\":1000}}");
        CHECK_EQ(bad.status(), FOXAPI_BAD_ARGUMENT);
        CHECK(std::string(bad.result.message).size() > 3);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_CONFIG, "{\"heatmap\":{\"cols\":256,\"rows\":64,\"freq_mode\":\"mean\"}}").status(), FOXAPI_OK);
        CHECK(r.until([&] { return state(r)["heat_cols"].get<std::size_t>() == 256; }, 10000));
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_CONFIG, "{\"heatmap\":{\"freq_mode\":\"median\"}}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_CONFIG, "{\"occupancy\":null}").status(), FOXAPI_OK);
    }

    std::printf("S4: busy, close\n");
    CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, kOpen).status(), FOXAPI_BUSY);
    CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_CLOSE, "").status(), FOXAPI_OK);
    CHECK(r.until([&] { return !state(r)["open"].get<bool>(); }, 10000));
    CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_CLOSE, "").status(), FOXAPI_NO_CHANGE);

    std::printf("S5: a device that stops answering, a link that cannot be made\n");
    {
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"tinysa_ultra\",\"port\":\"COM8\",\"start_mhz\":1,\"stop_mhz\":30,\"points\":100}").status(), FOXAPI_OK);
        CHECK(r.until([&] { return fake != nullptr && fake->sweeps.load() >= 3; }, 20000));
        fake->dieAfter.store(fake->sweeps.load() + 1);
        fake->dieMode.store(0);
        CHECK(r.until([&] { const json j = state(r); return j.is_object() && !j["open"].get<bool>() && !j["last_error"].get<std::string>().empty(); }, 30000));
        const json j = state(r);
        std::printf("  last error: %s\n", j["last_error"].get<std::string>().c_str());
        CHECK(j["last_error"].get<std::string>().find("stopped answering") != std::string::npos);
        CHECK(bigFact(r, "sweep.state").find("COM8") == std::string::npos);
        refuseWith = "the sweep device's port could not be opened (is it plugged in, and free?)";
        const auto f = sendText(r, FOXAPP_OP_SWEEP_OPEN, kOpen);
        CHECK_EQ(f.status(), FOXAPI_FAILED);
        CHECK(std::string(f.result.message).find("plugged in") != std::string::npos);
        refuseWith.clear();
    }

    std::printf("S6: refusals\n");
    {
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"rfexplorer\",\"port\":\"COM3\",\"start_mhz\":1,\"stop_mhz\":2}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"tinysa\",\"port\":\"LPT1\",\"start_mhz\":1,\"stop_mhz\":2}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"tinysa\",\"port\":\"COM999\",\"start_mhz\":1,\"stop_mhz\":2}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"tinysa\",\"port\":\"COM3\",\"start_mhz\":5,\"stop_mhz\":2}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"tinysa\",\"port\":\"COM3\",\"start_mhz\":1,\"stop_mhz\":2,\"points\":1}").status(), FOXAPI_BAD_ARGUMENT);
        const auto unk = sendText(r, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"tinysa\",\"port\":\"COM3\",\"start_mhz\":1,\"stop_mhz\":2,\"baud\":9600}");
        CHECK_EQ(unk.status(), FOXAPI_BAD_ARGUMENT);
        CHECK(std::string(unk.result.message).find("baud") != std::string::npos);
        testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
        CHECK(remote.ok());
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, kOpen, &remote).status(), FOXAPI_DENIED);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_CLOSE, "", &remote).status(), FOXAPI_DENIED);
    }
    return check::finish("test_engine_api_wireb_sweep");
}
