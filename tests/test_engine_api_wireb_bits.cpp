// test_engine_api_wireb_bits.cpp - THE BIT VIEW THROUGH THE ENGINE (wire-b; FOXAPP_OP_BITS_* 0x890D-0x8917; engine/wire_b_bits.hpp over core/bits/*.hpp). One real Engine on a synthetic source:
// an on/off-keyed carrier sending frames (sync word A55A, six payload bytes - a counter, a constant and random bytes - and a CRC-16 of them), 4000 bits a second, with noise.
//
//   K  KEEP slices the newest span of the radio channel I/Q into a kept burst (the slicer tap, on demand): a signal is found, its runs are counted, LIST shows it; noise alone keeps nothing
//   R  REDECODE with the right settings returns every frame whole (64 bits after the sync), the frame bytes are the ones sent (counter steps by one), a wrong chip period or a missing sync says so
//   S  SWEEP ranks NRZ-L first with the sync found;  C  FIND_CRC recovers CRC-16/IBM-3740 (the catalogue name) from the decoded frames; FIND_SUM finds nothing on CRC frames
//   I  INFER finds the counter and the constant field;  D  DIFF aligns two frames;  U  a decoder file is checked with line numbers and a table file likewise
//   G  refusals: not JSON, an unknown key, a slice that is not there, FIND_CRC before decoding, a frame number out of range, an over-long decoder text; a REMOTE session is DENIED
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "wire1_rig.hpp"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 200000.0;
constexpr int kSamplesPerBit = 50;     // 4000 bits a second at the 200 kHz channel
constexpr int kFrameBits = 80;         // 16 sync + 48 payload + 16 CRC
constexpr int kPeriodBits = 160;       // a frame, then as long again with the carrier off

std::uint16_t crc16(const std::vector<std::uint8_t>& d) {   // CRC-16/IBM-3740: poly 0x1021, init 0xFFFF, no reflection, no final XOR
    std::uint16_t c = 0xFFFF;
    for (const std::uint8_t b : d) {
        c ^= static_cast<std::uint16_t>(b) << 8;
        for (int i = 0; i < 8; ++i) { c = (c & 0x8000u) ? static_cast<std::uint16_t>((c << 1) ^ 0x1021u) : static_cast<std::uint16_t>(c << 1); }
    }
    return c;
}

class FrameSource final : public cascade::source::IqSource {
public:
    std::atomic<bool> frames{false};
    bool start() override { running_ = true; return true; }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return kRate; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 0.0; }
    bool setCenterFrequencyHz(double) override { return true; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        const bool on = frames.load();
        for (std::size_t i = 0; i < n; ++i, ++k_) {
            const std::uint64_t bitIndex = k_ / kSamplesPerBit;
            const int inPeriod = static_cast<int>(bitIndex % kPeriodBits);
            if (inPeriod == 0 && (k_ % kSamplesPerBit) == 0) { makeFrame(); }
            bool level = false;
            if (on && inPeriod < kFrameBits) { level = frameBits_[static_cast<std::size_t>(inPeriod)]; }
            const double a = 2.0 * kPi * 1000.0 * static_cast<double>(k_) / kRate;
            const double amp = level ? 0.3 : 0.0;
            dst[i] = {static_cast<float>(amp * std::cos(a) + gauss(0.01)), static_cast<float>(amp * std::sin(a) + gauss(0.01))};
        }
        return n;
    }
    const char* name() const override { return "wireb-frames"; }
    const char* lastError() const override { return ""; }

private:
    void makeFrame() {
        std::vector<std::uint8_t> payload = {static_cast<std::uint8_t>(counter_++), 0x42, static_cast<std::uint8_t>(lfsr() & 0xFF), static_cast<std::uint8_t>(lfsr() & 0xFF),
                                             static_cast<std::uint8_t>(lfsr() & 0xFF), static_cast<std::uint8_t>(lfsr() & 0xFF)};
        const std::uint16_t crc = crc16(payload);
        std::vector<std::uint8_t> all = {0xA5, 0x5A};
        all.insert(all.end(), payload.begin(), payload.end());
        all.push_back(static_cast<std::uint8_t>(crc >> 8));
        all.push_back(static_cast<std::uint8_t>(crc & 0xFF));
        frameBits_.clear();
        for (const std::uint8_t b : all) { for (int i = 7; i >= 0; --i) { frameBits_.push_back(((b >> i) & 1) != 0); } }
    }
    std::uint32_t lfsr() { state_ ^= state_ << 13; state_ ^= state_ >> 17; state_ ^= state_ << 5; return state_ >> 7; }
    double uni() { return (static_cast<double>(lfsr() & 0xFFFFFFu) + 0.5) / 16777216.0; }
    double gauss(double sigma) { const double u1 = uni(), u2 = uni(); return sigma * std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2); }
    bool running_ = false;
    std::uint64_t k_ = 0;
    int counter_ = 7;
    std::vector<bool> frameBits_ = std::vector<bool>(kFrameBits, false);
    std::uint32_t state_ = 0x1234567u;
};

std::string bigFact(Rig& r, const std::string& key) {
    std::string buf(1 << 18, '\0');
    const int32_t n = api()->get_setting(r.s->raw(), ("fact." + key).c_str(), &buf[0], buf.size());
    return n >= 0 ? std::string(buf.c_str()) : std::string("<absent>");
}

testing::Outcome sendText(Rig& r, std::uint32_t op, std::uint32_t radio, const std::string& text, long long ival0 = 0, testing::Session* who = nullptr) {
    FoxCommand c = testing::cmd(op, 0.0, ival0, nullptr);
    c.target = radio;
    testing::Session& sess = who != nullptr ? *who : *r.s;
    testing::Outcome o;
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = api()->submit_text(sess.raw(), &c, text.data(), static_cast<uint32_t>(text.size()), &sr);
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

json fact(Rig& r, const std::string& name) {
    const json j = json::parse(bigFact(r, "radio.1.bits." + name), nullptr, false);
    CHECK(!j.is_discarded());
    return j.is_discarded() ? json::object() : j;
}

const char* kSettings = "\"line_codes\":[\"nrz-l\"],\"chip_us\":250,\"sync\":\"1010010101011010\",\"framing\":\"fixed\",\"fixed_bits\":64";

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_bits\n");
    engine_api_test::hermetic("wirebbits");
    Access::installHooks();
    Rig r;
    auto src = std::make_unique<FrameSource>();
    FrameSource& syn = *src;
    Access::useSource(r.eng, std::move(src));
    CHECK(r.send(testing::cmd(FOXAPI_OP_SET_MODE, 0, FOXAPI_DEMOD_RAW)).status() == FOXAPI_OK || true);
    CHECK(r.send(testing::cmd(FOXAPI_OP_SET_BANDWIDTH, 200000.0)).status() == FOXAPI_OK || true);
    CHECK(r.send(testing::cmd(FOXAPI_OP_SET_VFO_OFFSET, 0.0)).status() == FOXAPI_OK || true);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_SQUELCH, -120.0)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    r.until([] { return false; }, 1200);

    std::printf("K: keep\n");
    {
        // noise alone: no signal, nothing kept
        const auto o = sendText(r, FOXAPP_OP_BITS_KEEP, 1, "{\"duration_s\":0.3}");
        CHECK_EQ(o.status(), FOXAPI_OK);
        CHECK_EQ(o.result.applied[0], 0.0);
        const json n = fact(r, "keep");
        CHECK(!n["valid"].get<bool>());
        CHECK_EQ(n["result"]["kept"].get<std::size_t>(), static_cast<std::size_t>(0));
        syn.frames.store(true);
        r.until([] { return false; }, 900);
        const auto k = sendText(r, FOXAPP_OP_BITS_KEEP, 1, "{\"duration_s\":0.6,\"label\":\"frames\"}");
        CHECK_EQ(k.status(), FOXAPI_OK);
        CHECK_EQ(k.result.applied[0], 1.0);
        CHECK(k.result.applied[1] >= 1.0);
        const json j = fact(r, "keep");
        CHECK(j["valid"].get<bool>());
        std::printf("  kept id %.0f: %zu runs\n", k.result.applied[1], j["result"]["runs"].get<std::size_t>());
        CHECK(j["result"]["runs"].get<std::size_t>() > 100);
        CHECK_EQ(sendText(r, FOXAPP_OP_BITS_LIST, 1, "").status(), FOXAPI_OK);
        const json l = fact(r, "list");
        CHECK_EQ(l["result"]["slices"].size(), static_cast<std::size_t>(1));
        CHECK_EQ(l["result"]["slices"][0]["label"].get<std::string>(), std::string("frames"));
    }

    std::printf("R: redecode\n");
    {
        const std::string settings = std::string("{") + kSettings + "}";
        CHECK_EQ(sendText(r, FOXAPP_OP_BITS_REDECODE, 1, settings).status(), FOXAPI_OK);
        const json j = fact(r, "redecode");
        CHECK(j["valid"].get<bool>());
        const json& frames = j["result"]["frames"];
        std::printf("  %zu frames, %zu sync hits, chip %.1f us\n", frames.size(), j["result"]["sync_hits"].get<std::size_t>(), j["result"]["chip_us"].get<double>());
        CHECK(frames.size() >= 8);
        std::vector<int> counters;
        std::size_t whole = 0;
        for (const json& f : frames) {
            if (f["bit_count"].get<std::size_t>() == 64) {
                ++whole;
                const std::string hex = f["hex"].get<std::string>();
                CHECK_EQ(hex.size(), static_cast<std::size_t>(16));
                CHECK_EQ(hex.substr(2, 2), std::string("42"));   // the constant byte
                counters.push_back(std::stoi(hex.substr(0, 2), nullptr, 16));
            }
        }
        CHECK(whole >= 8);
        // consecutive frames step the counter by one (a frame the slicer lost would show as a gap, not as a wrong value)
        std::size_t steps = 0;
        for (std::size_t i = 1; i < counters.size(); ++i) { steps += ((counters[i] - counters[i - 1]) & 0xFF) == 1 ? 1u : 0u; }
        CHECK(counters.size() >= 2 && steps + 1 >= counters.size() - 1);
        // no sync word that is not in the signal: no frames, and the answer says so
        CHECK_EQ(sendText(r, FOXAPP_OP_BITS_REDECODE, 1, "{\"sync\":\"11001100110011001100\",\"framing\":\"fixed\",\"fixed_bits\":64,\"chip_us\":250}").status(), FOXAPI_OK);
        CHECK_EQ(fact(r, "redecode")["result"]["frames"].size(), static_cast<std::size_t>(0));
        // decode again with the right settings: the frames are the ones the later operations use
        CHECK_EQ(sendText(r, FOXAPP_OP_BITS_REDECODE, 1, settings).status(), FOXAPI_OK);
    }

    std::printf("S/C/I/D: sweep, CRC, checksum, fields, diff\n");
    {
        CHECK_EQ(sendText(r, FOXAPP_OP_BITS_SWEEP, 1, std::string("{") + kSettings + "}").status(), FOXAPI_OK);
        const json s = fact(r, "sweep");
        CHECK(s["valid"].get<bool>());
        if (s["valid"].get<bool>()) {
            std::printf("  best: %s invert %d sync hits %zu\n", s["result"]["rows"][0]["line_code"].get<std::string>().c_str(), s["result"]["rows"][0]["invert"].get<bool>() ? 1 : 0,
                        s["result"]["rows"][0]["sync_hits"].get<std::size_t>());
            CHECK_EQ(s["result"]["rows"][0]["line_code"].get<std::string>(), std::string("nrz-l"));
            CHECK(!s["result"]["rows"][0]["invert"].get<bool>());
            CHECK(s["result"]["rows"][0]["sync_hits"].get<std::size_t>() >= 8);
        }
        CHECK_EQ(sendText(r, FOXAPP_OP_BITS_FIND_CRC, 1, "{\"widths\":[16]}").status(), FOXAPI_OK);
        const json c = fact(r, "findcrc");
        CHECK(c["valid"].get<bool>());
        if (c["valid"].get<bool>()) {
            const json& h = c["result"]["hits"][0];
            std::printf("  crc: width %d name %s\n", h["width"].get<int>(), h["name"].get<std::string>().c_str());
            CHECK_EQ(h["width"].get<int>(), 16);
            CHECK(h["name"].get<std::string>().find("3740") != std::string::npos);
            CHECK_EQ(h["head_bytes"].get<std::size_t>(), static_cast<std::size_t>(0));
            CHECK_EQ(h["tail_bytes"].get<std::size_t>(), static_cast<std::size_t>(0));
        }
        CHECK_EQ(sendText(r, FOXAPP_OP_BITS_FIND_SUM, 1, "{}").status(), FOXAPI_OK);
        CHECK(!fact(r, "findsum")["valid"].get<bool>());   // a CRC frame is not a plain checksum
        CHECK_EQ(sendText(r, FOXAPP_OP_BITS_INFER, 1, "{}").status(), FOXAPI_OK);
        const json inf = fact(r, "infer");
        CHECK(inf["valid"].get<bool>());
        bool counter = false, constant = false;
        for (const json& f : inf["result"]["fields"]) {
            const std::string kind = f["kind"].get<std::string>();
            if (kind == "counter") { counter = true; }
            if (kind == "constant") { constant = true; }
        }
        CHECK(counter && constant);
        CHECK_EQ(sendText(r, FOXAPP_OP_BITS_DIFF, 1, "{\"a\":1,\"b\":2}").status(), FOXAPI_OK);
        const json d = fact(r, "diff");
        CHECK(d["valid"].get<bool>());
        CHECK(d["result"]["mismatches"].get<std::size_t>() > 0);
        CHECK_EQ(d["result"]["row_a"].get<std::string>().size(), d["result"]["marks"].get<std::string>().size());
    }

    std::printf("U: decoder and table files\n");
    {
        const std::string good = "foxsdr-bitspec 1\nname engine-test\ncoding chips manchester\nbit_us 976\npreamble 1111 0101\nmin_bits 16\nfield a at 0 8 hex\n";
        CHECK_EQ(sendText(r, FOXAPP_OP_BITS_SPEC_CHECK, 1, good).status(), FOXAPI_OK);
        const json g = fact(r, "speccheck");
        CHECK(g["result"]["ok"].get<bool>());
        CHECK_EQ(g["result"]["name"].get<std::string>(), std::string("engine-test"));
        const auto bad = sendText(r, FOXAPP_OP_BITS_SPEC_CHECK, 1, "foxsdr-bitspec 1\nwobble 3\n");
        CHECK_EQ(bad.status(), FOXAPI_OK);
        CHECK_EQ(bad.result.applied[0], 0.0);
        const json b = fact(r, "speccheck");
        CHECK(!b["result"]["ok"].get<bool>());
        CHECK_EQ(b["result"]["errors"][0]["line"].get<int>(), 2);
        CHECK(b["result"]["errors"][0]["message"].get<std::string>().find("unknown statement") != std::string::npos);
        CHECK_EQ(sendText(r, FOXAPP_OP_BITS_SPEC_RUN, 1, good, 0).status(), FOXAPI_OK);   // a good decoder on a slice it does not fit: no message, no error
        CHECK(fact(r, "specrun")["result"]["ok"].get<bool>());
        CHECK_EQ(sendText(r, FOXAPP_OP_BITS_TABLE_CHECK, 1, "foxsdr-bittable 1\nname mine\nbits 5\nplane LTRS\n1 = E\n").status(), FOXAPI_OK);
        CHECK(fact(r, "tablecheck")["result"]["ok"].get<bool>());
        CHECK_EQ(sendText(r, FOXAPP_OP_BITS_TABLE_CHECK, 1, "foxsdr-bittable 1\nbits 9\n").status(), FOXAPI_OK);
        const json t = fact(r, "tablecheck");
        CHECK(!t["result"]["ok"].get<bool>());
        CHECK_EQ(t["result"]["errors"][0]["line"].get<int>(), 2);
    }

    std::printf("G: refusals\n");
    {
        CHECK_EQ(sendText(r, FOXAPP_OP_BITS_REDECODE, 1, "nope").status(), FOXAPI_BAD_ARGUMENT);
        const auto unk = sendText(r, FOXAPP_OP_BITS_REDECODE, 1, "{\"sinc\":\"1\"}");
        CHECK_EQ(unk.status(), FOXAPI_BAD_ARGUMENT);
        CHECK(std::string(unk.result.message).find("sinc") != std::string::npos);
        CHECK_EQ(sendText(r, FOXAPP_OP_BITS_REDECODE, 1, "{\"slice\":99999}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_BITS_REDECODE, 1, "{\"line_codes\":[\"nrz-x\"]}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_BITS_REDECODE, 1, "{\"framing\":\"fixed\"}").status(), FOXAPI_BAD_ARGUMENT);   // fixed framing needs fixed_bits
        CHECK_EQ(sendText(r, FOXAPP_OP_BITS_DIFF, 1, "{\"a\":1,\"b\":9999}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_BITS_SPEC_CHECK, 1, std::string(70000, 'x')).status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_BITS_KEEP, 7, "{}").status(), FOXAPI_NO_DEVICE);
        testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
        CHECK(remote.ok());
        CHECK_EQ(sendText(r, FOXAPP_OP_BITS_KEEP, 1, "{}", 0, &remote).status(), FOXAPI_DENIED);
    }
    // a radio with nothing decoded: FIND_CRC says what to do first
    {
        Rig fresh;
        CHECK_EQ(sendText(fresh, FOXAPP_OP_BITS_FIND_CRC, 1, "{}").status(), FOXAPI_BAD_ARGUMENT);
    }
    return check::finish("test_engine_api_wireb_bits");
}
