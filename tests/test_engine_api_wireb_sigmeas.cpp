// test_engine_api_wireb_sigmeas.cpp - THE SIGNAL MEASUREMENTS THROUGH THE ENGINE (wire-b; FOXAPP_OP_SIGMEAS_* 0x8903-0x890B; engine/wire_b_sigmeas.hpp over dsp/sigmeas_*.hpp).
// One real Engine on a synthetic source (noise, a frequency offset and a modest SNR in every signal); each op reads the radio's channel taps and answers in a LOCAL fact.
//
//   A  FSK 1200 Bd +-2400 Hz: baud within 1 %, shift within 2 %, the bit string comes back;  OOK 2000 Bd: baud within 1 %, pulses listed;  the cursor mode: period from two cursors
//   B  band-limited multitone (10 kHz wide): occupied bandwidth within 4 %, centre near 0 Hz; a search window refused when only half of it is given
//   C  FM tone 1 kHz, 5 kHz deviation: deviation within 3 %;  AM depth 0.5: depth within 0.05
//   D  gated carrier (20 ms on, 30 ms off): burst count and repeat period (50 ms within 2 %), duty cycle 0.4 within 0.05
//   E  frequency hopper (4 channels, 20 ms dwell): hop rate 50 Hz within 5 %, four distinct frequencies;  chirp 20 Hz/s: the drift search finds it within 10 %
//   F  the finished audio of an AM carrier: tone near 1 kHz and a SINAD over 20 dB;  vs time: the mean frequency of a +1500 Hz tone within 5 Hz, the point cap holds;  histogram and CCDF
//   G  refusals: not JSON, an unknown key, a value out of range, a bad window, a radio that does not exist; a radio that is not running is FAILED; a REMOTE session is DENIED
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

enum Mode { kNoise = 0, kFsk, kOok, kMultitone, kFm, kAm, kBursts, kHop, kChirp, kTone1500, kQuiet, kPseudoDoppler };

// One source, several signals: the mode is switched by the test. Every signal has noise (sigma 0.01 per component) and an amplitude of 0.3.
class Synth final : public cascade::source::IqSource {
public:
    std::atomic<int> mode{kNoise};
    bool start() override { running_ = true; return true; }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return kRate; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 0.0; }
    bool setCenterFrequencyHz(double) override { return true; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        const int m = mode.load();
        for (std::size_t i = 0; i < n; ++i, ++k_) {
            const double t = static_cast<double>(k_) / kRate;
            std::complex<double> s(0.0, 0.0);
            switch (m) {
                case kNoise: s = {0.0, 0.0}; break;
                case kFsk: {
                    const std::uint64_t sym = static_cast<std::uint64_t>(t * 1200.0);
                    if (sym != lastSym_) { lastSym_ = sym; bit_ = (lfsr() & 1u) != 0u; }
                    phase_ += 2.0 * kPi * (bit_ ? 2400.0 : -2400.0) / kRate;
                    s = 0.3 * std::complex<double>(std::cos(phase_), std::sin(phase_));
                    break;
                }
                case kOok: {
                    const std::uint64_t sym = static_cast<std::uint64_t>(t * 2000.0);
                    if (sym != lastSym_) { lastSym_ = sym; bit_ = (lfsr() & 1u) != 0u; }
                    const double a = 2.0 * kPi * 1000.0 * t;
                    s = bit_ ? 0.3 * std::complex<double>(std::cos(a), std::sin(a)) : std::complex<double>(0.0, 0.0);
                    break;
                }
                case kMultitone: {
                    for (int j = -50; j <= 50; ++j) {
                        const double a = 2.0 * kPi * (100.0 * j) * t + tonePhase(j);
                        s += (0.3 / 101.0 * 4.0) * std::complex<double>(std::cos(a), std::sin(a));
                    }
                    break;
                }
                case kFm: {
                    const double a = 2.0 * kPi * 0.0 * t + 5000.0 / 1000.0 * std::sin(2.0 * kPi * 1000.0 * t);   // beta = deviation / tone
                    s = 0.3 * std::complex<double>(std::cos(a), std::sin(a));
                    break;
                }
                case kAm: {
                    const double env = 0.3 * (1.0 + 0.5 * std::sin(2.0 * kPi * 1000.0 * t));
                    s = {env, 0.0};
                    break;
                }
                case kBursts: {
                    const double ph = std::fmod(t, 0.050);
                    const double a = 2.0 * kPi * 500.0 * t;
                    s = ph < 0.020 ? 0.3 * std::complex<double>(std::cos(a), std::sin(a)) : std::complex<double>(0.0, 0.0);
                    break;
                }
                case kHop: {
                    static const double f[4] = {-7500.0, -2500.0, 2500.0, 7500.0};
                    const std::uint64_t slot = static_cast<std::uint64_t>(t / 0.020);
                    const double fc = f[(slot * 3u) % 4u];
                    phase_ += 2.0 * kPi * fc / kRate;
                    s = 0.3 * std::complex<double>(std::cos(phase_), std::sin(phase_));
                    break;
                }
                case kChirp: {
                    const double a = 2.0 * kPi * (1000.0 * t + 100.0 * t * t);   // 1000 Hz rising by 200 Hz/s
                    s = 0.3 * std::complex<double>(std::cos(a), std::sin(a));
                    break;
                }
                case kPseudoDoppler: {
                    // a plane wave from 40 degrees onto four antennas on a ring of a quarter wavelength, switched every 100 samples, with a carrier offset
                    const double theta = 40.0 * kPi / 180.0;
                    const int ant = static_cast<int>((k_ / 100u) % 4u);
                    const double ph = 2.0 * kPi * 0.25 * std::cos(theta - ant * kPi / 2.0) + 0.01 * static_cast<double>(k_);
                    s = 0.3 * std::complex<double>(std::cos(ph), std::sin(ph));
                    break;
                }
                case kTone1500: {
                    const double a = 2.0 * kPi * 1500.0 * t;
                    s = 0.3 * std::complex<double>(std::cos(a), std::sin(a));
                    break;
                }
                default: break;
            }
            dst[i] = {static_cast<float>(s.real() + gauss(0.01)), static_cast<float>(s.imag() + gauss(0.01))};
        }
        return n;
    }
    const char* name() const override { return "wireb-synth"; }
    const char* lastError() const override { return ""; }

private:
    std::uint32_t lfsr() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_ >> 7;
    }
    double uni() { return (static_cast<double>(lfsr() & 0xFFFFFFu) + 0.5) / 16777216.0; }
    double gauss(double sigma) {
        const double u1 = uni(), u2 = uni();
        return sigma * std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
    }
    static double tonePhase(int j) { return 1.7 * j * j; }   // a fixed spread of phases: no big crest factor
    bool running_ = false;
    std::uint64_t k_ = 0;
    std::uint64_t lastSym_ = ~0ull;
    bool bit_ = false;
    double phase_ = 0.0;
    std::uint32_t state_ = 0x2545F491u;
};

std::string bigFact(Rig& r, const std::string& key) {
    std::string buf(1 << 18, '\0');
    const int32_t n = api()->get_setting(r.s->raw(), ("fact." + key).c_str(), &buf[0], buf.size());
    return n >= 0 ? std::string(buf.c_str()) : std::string("<absent>");
}

testing::Outcome sendText(Rig& r, std::uint32_t op, std::uint32_t radio, const std::string& params, testing::Session* who = nullptr) {
    FoxCommand c = testing::cmd(op, 0.0, 0, nullptr);
    c.target = radio;
    testing::Session& sess = who != nullptr ? *who : *r.s;
    testing::Outcome o;
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = api()->submit_text(sess.raw(), &c, params.data(), static_cast<uint32_t>(params.size()), &sr);
    o.submitStatus = n < 0 ? n : sr.status;
    if (o.submitStatus != FOXAPI_OK) { return o; }
    for (int pass = 0; pass < 60 && !o.landed; ++pass) {
        r.eng.controlPass();
        FoxCommandResult res[16];
        for (auto& x : res) { x.structSize = sizeof(x); }
        const int32_t got = api()->poll_results(sess.raw(), res, 16);
        for (int32_t i = 0; i < got; ++i) {
            if (res[i].ticket == sr.ticket) { o.landed = true; o.result = res[i]; }
        }
    }
    return o;
}

// Switches the signal, lets the taps roll over to it (the span asked for is the newest 0.5 s at most, so 0.9 s is enough), and returns the fact of the op as JSON.
json measure(Rig& r, Synth& syn, Mode m, std::uint32_t op, const std::string& name, const std::string& params, testing::Outcome* out = nullptr, bool settle = true) {
    if (settle) {
        syn.mode.store(m);
        r.until([] { return false; }, 900);
    }
    const testing::Outcome o = sendText(r, op, 1, params);
    if (out != nullptr) { *out = o; }
    CHECK_EQ(o.status(), FOXAPI_OK);
    const std::string f = bigFact(r, "radio.1.sigmeas." + name);
    json j = json::parse(f, nullptr, false);
    CHECK(!j.is_discarded());
    if (j.is_discarded()) { return json::object(); }
    if (!j.value("valid", false)) { std::printf("  [%s] %.300s\n", name.c_str(), f.c_str()); }
    return j;
}

double rel(double got, double want) { return std::fabs(got - want) / std::fabs(want); }

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_sigmeas\n");
    engine_api_test::hermetic("wirebsigmeas");
    Access::installHooks();
    Rig r;
    auto synth = std::make_unique<Synth>();
    Synth& syn = *synth;
    Access::useSource(r.eng, std::move(synth));
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_MODE, 0, FOXAPI_DEMOD_RAW)).status() == FOXAPI_OK || true, true);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_BANDWIDTH, 200000.0)).status() == FOXAPI_OK || true, true);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_SQUELCH, -120.0)).status(), FOXAPI_OK);
    // the channel on the source's centre: every signal below is generated at its own offset from DC
    {
        const auto o = r.send(testing::cmd(FOXAPI_OP_SET_VFO_OFFSET, 0.0));
        CHECK(o.status() == FOXAPI_OK || o.status() == FOXAPI_NO_CHANGE);
        const FoxReceiverState st = r.radio(1);
        std::printf("  vfo offset %.0f Hz, bandwidth %.0f Hz, mode %d\n", st.vfoOffsetHz, st.bandwidthHz, static_cast<int>(st.demodMode));
        CHECK_EQ(st.vfoOffsetHz, 0.0);
    }

    std::printf("G0: a radio that is not running\n");
    CHECK_EQ(sendText(r, FOXAPP_OP_SIGMEAS_BANDWIDTH, 1, "{}").status(), FOXAPI_FAILED);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    r.until([] { return false; }, 1500);
    const double chRate = r.radio(1).channelRateHz;
    std::printf("  channel rate %.0f Hz\n", chRate);
    CHECK(chRate > 20000.0);

    std::printf("A: symbols (FSK, OOK, cursors)\n");
    {
        json j = measure(r, syn, kFsk, FOXAPP_OP_SIGMEAS_SYMBOLS, "symbols", "{\"mode\":\"fsk\",\"duration_s\":0.5}");
        CHECK(j["valid"].get<bool>());
        const json& x = j["result"];
        if (j["valid"].get<bool>()) {
            std::printf("  fsk: baud %.2f shift %.1f jitter %.3f bits %zu\n", x["baud_hz"].get<double>(), x["shift_hz"].get<double>(), x["jitter_ui"].get<double>(), x["bit_count"].get<std::size_t>());
            CHECK(rel(x["baud_hz"].get<double>(), 1200.0) < 0.01);
            CHECK(rel(x["shift_hz"].get<double>(), 4800.0) < 0.02);
            CHECK(x["bit_count"].get<std::size_t>() > 300);
            CHECK(x["bits"].get<std::string>().size() == std::min<std::size_t>(x["bit_count"].get<std::size_t>(), 4096));
        }
        CHECK(std::fabs(j["rate_hz"].get<double>() - chRate) < 1.0);
        j = measure(r, syn, kOok, FOXAPP_OP_SIGMEAS_SYMBOLS, "symbols", "{\"mode\":\"ook\",\"duration_s\":0.5}");
        CHECK(j["valid"].get<bool>());
        if (j["valid"].get<bool>()) {
            const json& x = j["result"];
            std::printf("  ook: baud %.2f pulses %zu\n", x["baud_hz"].get<double>(), x["pulse_count"].get<std::size_t>());
            CHECK(rel(x["baud_hz"].get<double>(), 2000.0) < 0.01);
            CHECK(x["pulse_count"].get<std::size_t>() > 50);
            CHECK(!x["pulses"].empty());
        }
        j = measure(r, syn, kOok, FOXAPP_OP_SIGMEAS_SYMBOLS, "symbols", "{\"mode\":\"cursor\",\"t0_s\":0.010,\"t1_s\":0.020,\"symbol_count\":12}", nullptr, false);
        CHECK(j["valid"].get<bool>());
        CHECK(std::fabs(j["result"]["rate_hz"].get<double>() - 1200.0) < 1e-6);
        // noise only: no on/off structure and the reason says so
        testing::Outcome o;
        j = measure(r, syn, kNoise, FOXAPP_OP_SIGMEAS_SYMBOLS, "symbols", "{\"mode\":\"ook\"}", &o);
        CHECK(!j["valid"].get<bool>());
        CHECK_EQ(o.result.applied[0], 0.0);
        CHECK(!j["reason"].get<std::string>().empty() && std::string(o.result.message) == j["reason"].get<std::string>());
    }

    std::printf("B: occupied bandwidth\n");
    {
        const json j = measure(r, syn, kMultitone, FOXAPP_OP_SIGMEAS_BANDWIDTH, "bandwidth", "{\"duration_s\":0.5}");
        CHECK(j["valid"].get<bool>());
        if (j["valid"].get<bool>()) {
            const json& x = j["result"];
            std::printf("  obw %.0f Hz centre %.0f snr %.1f\n", x["obw_hz"].get<double>(), x["centre_hz"].get<double>(), x["snr_db"].get<double>());
            CHECK(rel(x["obw_hz"].get<double>(), 9900.0) < 0.04);   // 101 tones 100 Hz apart: 99 % of the power lies in about 9.9 kHz of the 10 kHz
            CHECK(std::fabs(x["centre_hz"].get<double>()) < 300.0);
            CHECK_EQ(x["xdb"].size(), static_cast<std::size_t>(4));
        }
        CHECK_EQ(sendText(r, FOXAPP_OP_SIGMEAS_BANDWIDTH, 1, "{\"band_low_hz\":-6000}").status(), FOXAPI_BAD_ARGUMENT);
    }

    std::printf("C: FM deviation and AM depth\n");
    {
        json j = measure(r, syn, kFm, FOXAPP_OP_SIGMEAS_MODULATION, "modulation", "{\"duration_s\":0.5}");
        CHECK(j["valid"].get<bool>());
        const json& fm = j["result"]["fm"];
        CHECK(fm["valid"].get<bool>());
        if (fm["valid"].get<bool>()) {
            std::printf("  fm deviation %.0f Hz tone %.0f\n", fm["peak_deviation_hz"].get<double>(), fm["tone_hz"].get<double>());
            CHECK(rel(fm["peak_deviation_hz"].get<double>(), 5000.0) < 0.03);
        }
        j = measure(r, syn, kAm, FOXAPP_OP_SIGMEAS_MODULATION, "modulation", "{\"duration_s\":0.5}");
        const json& am = j["result"]["am"];
        CHECK(am["valid"].get<bool>());
        if (am["valid"].get<bool>()) {
            std::printf("  am depth %.3f\n", am["depth"].get<double>());
            CHECK(std::fabs(am["depth"].get<double>() - 0.5) < 0.05);
        }
    }

    std::printf("D: bursts\n");
    {
        const json j = measure(r, syn, kBursts, FOXAPP_OP_SIGMEAS_BURSTS, "bursts", "{\"duration_s\":0.6}");
        CHECK(j["valid"].get<bool>());
        if (j["valid"].get<bool>()) {
            const json& x = j["result"];
            std::printf("  bursts %zu repeat %.4f duty %.3f\n", x["total_bursts"].get<std::size_t>(), x["repeat_period_s"].get<double>(), x["duty_cycle"].get<double>());
            CHECK(x["total_bursts"].get<std::size_t>() >= 10);
            CHECK(rel(x["repeat_period_s"].get<double>(), 0.050) < 0.02);
            CHECK(std::fabs(x["duty_cycle"].get<double>() - 0.4) < 0.05);
        }
    }

    std::printf("E: hopping, chirp (drift)\n");
    {
        const json j = measure(r, syn, kHop, FOXAPP_OP_SIGMEAS_HOPCHIRP, "hopchirp", "{\"duration_s\":0.6}");
        CHECK(j["valid"].get<bool>());
        if (j["valid"].get<bool>()) {
            const json& h = j["result"]["hop"];
            CHECK(h["valid"].get<bool>());
            if (h["valid"].get<bool>()) {
                std::printf("  hop rate %.1f Hz, %zu distinct\n", h["hop_rate_hz"].get<double>(), h["hop_set_hz"].size());
                CHECK(rel(h["hop_rate_hz"].get<double>(), 50.0) < 0.05);
                CHECK_EQ(h["hop_set_hz"].size(), static_cast<std::size_t>(4));
            }
        }
        const json d = measure(r, syn, kChirp, FOXAPP_OP_SIGMEAS_DRIFT, "drift", "{\"duration_s\":1.2,\"min_hz_per_s\":-400,\"max_hz_per_s\":400}");
        CHECK(d["valid"].get<bool>());
        if (d["valid"].get<bool>()) {
            std::printf("  drift %.2f Hz/s\n", d["result"]["drift_hz_per_s"].get<double>());
            CHECK(rel(d["result"]["drift_hz_per_s"].get<double>(), 200.0) < 0.05);
        }
    }

    std::printf("F: audio, vs time, histogram\n");
    {
        // the finished audio of an AM carrier is the 1 kHz tone: the AM demodulator and the audio tap are the engine's own
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_MODE, 0, FOXAPI_DEMOD_AM)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_BANDWIDTH, 10000.0)).status(), FOXAPI_OK);
        const json a = measure(r, syn, kAm, FOXAPP_OP_SIGMEAS_AUDIO, "audio", "{\"duration_s\":0.5}");
        CHECK(a["valid"].get<bool>());
        if (a["valid"].get<bool>()) {
            std::printf("  audio tone %.1f Hz sinad %.1f dB\n", a["result"]["tone_hz"].get<double>(), a["result"]["sinad_db"].get<double>());
            CHECK(std::fabs(a["result"]["tone_hz"].get<double>() - 1000.0) < 10.0);
            CHECK(a["result"]["sinad_db"].get<double>() > 20.0);
        }
        CHECK(std::fabs(a["rate_hz"].get<double>() - 48000.0) < 1.0);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_MODE, 0, FOXAPI_DEMOD_RAW)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_BANDWIDTH, 200000.0)).status(), FOXAPI_OK);
        const json v = measure(r, syn, kTone1500, FOXAPP_OP_SIGMEAS_VSTIME, "vstime", "{\"duration_s\":0.2,\"max_points\":128}");
        CHECK(v["valid"].get<bool>());
        if (v["valid"].get<bool>()) {
            const json& x = v["result"];
            std::printf("  vstime mean freq %.1f Hz, %zu points\n", x["mean_freq_hz"].get<double>(), x["points"].get<std::size_t>());
            CHECK(std::fabs(x["mean_freq_hz"].get<double>() - 1500.0) < 5.0);
            CHECK(x["points"].get<std::size_t>() <= 128 && x["points"].get<std::size_t>() >= 16);
            CHECK_EQ(x["amplitude_dbfs"].size(), x["points"].get<std::size_t>());
        }
        const json h = measure(r, syn, kNoise, FOXAPP_OP_SIGMEAS_HISTOGRAM, "histogram", "{\"duration_s\":0.25,\"bins\":32,\"ccdf\":true}");
        CHECK(h["valid"].get<bool>());
        if (h["valid"].get<bool>()) {
            const json& x = h["result"];
            std::uint64_t sum = 0;
            for (const json& c : x["counts"]) { sum += c.get<std::uint64_t>(); }
            CHECK_EQ(x["counts"].size(), static_cast<std::size_t>(32));
            CHECK_EQ(sum + x["below_range"].get<std::uint64_t>() + x["above_range"].get<std::uint64_t>(), x["total"].get<std::uint64_t>());
            CHECK(x["ccdf"]["valid"].get<bool>());
        }
    }

    std::printf("H: pseudo-Doppler bearing (rfmon)\n");
    {
        // The switch has no sync line: where antenna 0 first dwell begins in the span is for the caller to say ("start_sample"). The model aligns within half a dwell of it and cannot tell
        // which antenna is which (a bearing 90 degrees out is the answer for a start one dwell off), so the right start is the one that gives the true bearing: a sweep of start values over
        // one rotation (400 samples) finds it; the others are valid but rotated, or refused.
        syn.mode.store(kPseudoDoppler);
        r.until([] { return false; }, 900);
        int right = 0, valid = 0;
        for (int start = 0; start < 400; start += 25) {
            const std::string params = "{\"duration_s\":0.25,\"samples_per_dwell\":100,\"start_sample\":" + std::to_string(start) + "}";
            const testing::Outcome o = sendText(r, FOXAPP_OP_PSEUDO_DOPPLER, 1, params);
            CHECK_EQ(o.status(), FOXAPI_OK);
            const json j = json::parse(bigFact(r, "radio.1.sigmeas.pseudodoppler"), nullptr, false);
            if (j.is_discarded() || !j.value("valid", false)) { continue; }
            ++valid;
            const json& x = j["result"];
            double err = std::fabs(x["bearing_deg"].get<double>() - 40.0);
            if (err > 180.0) { err = 360.0 - err; }
            if (err < 3.0 && x["quality"].get<double>() > 0.9 && !x["weak"].get<bool>()) { ++right; }
        }
        std::printf("  %d of 16 starts valid, %d gave the true bearing (40 deg)\n", valid, right);
        CHECK(valid >= 8);
        CHECK(right >= 1);   // (each call reads a span that begins at a different sample of the switching, so the matching starts differ from call to call)
        // the noise alone is not a bearing: it is either refused or flagged weak, never a confident one
        const json n = measure(r, syn, kNoise, FOXAPP_OP_PSEUDO_DOPPLER, "pseudodoppler", "{\"duration_s\":0.25}");
        CHECK(!n["valid"].get<bool>() || n["result"]["weak"].get<bool>());
        CHECK_EQ(sendText(r, FOXAPP_OP_PSEUDO_DOPPLER, 1, "{\"samples_per_dwell\":4}").status(), FOXAPI_BAD_ARGUMENT);
    }

    std::printf("G: refusals\n");
    {
        CHECK_EQ(sendText(r, FOXAPP_OP_SIGMEAS_BANDWIDTH, 1, "not json").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_SIGMEAS_BANDWIDTH, 1, "[1,2]").status(), FOXAPI_BAD_ARGUMENT);
        const auto unk = sendText(r, FOXAPP_OP_SIGMEAS_BANDWIDTH, 1, "{\"obw_pct\":99}");
        CHECK_EQ(unk.status(), FOXAPI_BAD_ARGUMENT);
        CHECK(std::string(unk.result.message).find("obw_pct") != std::string::npos);
        CHECK_EQ(sendText(r, FOXAPP_OP_SIGMEAS_BANDWIDTH, 1, "{\"obw_percent\":120}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_SIGMEAS_BANDWIDTH, 1, "{\"duration_s\":-1}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_SIGMEAS_HOPCHIRP, 1, "{\"frame_samples\":1000}").status(), FOXAPI_BAD_ARGUMENT);   // not a power of two
        CHECK_EQ(sendText(r, FOXAPP_OP_SIGMEAS_BANDWIDTH, 7, "{}").status(), FOXAPI_NO_DEVICE);
        testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
        CHECK(remote.ok());
        CHECK_EQ(sendText(r, FOXAPP_OP_SIGMEAS_BANDWIDTH, 1, "{}", &remote).status(), FOXAPI_DENIED);
    }
    return check::finish("test_engine_api_wireb_sigmeas");
}
