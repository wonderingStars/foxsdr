// foxsdr-slint - THE SLINT WINDOW (spike): the Workbench main screen as a client of the C API table.
//
// What this proves (docs/engine-stage4.md, the Slint spike):
//   1. Slint builds into this tree from the prebuilt SDK (no Rust), beside the engine, with the ImGui window untouched.
//   2. A window that is ONLY a table client: it owns an EngineService exactly as src/main.cpp does, opens a LOCAL session,
//      drains read_state / read_spectrum once per tick into Slint properties, and turns every key into a submit().
//   3. The spectrum and waterfall are Image wells painted from SharedPixelBuffers - the handover's "surfaces fed by a
//      renderer" - which is also the path the five 3D screens take.
//
// THE REAL SETTINGS FILE IS NEVER TOUCHED. The spike's config path comes from FOXSDR_SLINT_CONFIG or defaults to a file
// beside the executable, and a resolved path that is the user's own %APPDATA%\foxsdr\config.json is refused outright
// unless FOXSDR_SLINT_REAL=1 (nothing here sets it). The launch script also redirects APPDATA/LOCALAPPDATA/USERPROFILE
// to a scratch profile, as the GUI test gate does.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <slint.h>

#include "foxsdr_api.h"
#include "core/version.hpp"
#include "engine/engine_service.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/window_service_options.hpp"

#include "main.h"  // generated from ui/main.slint by slint_target_sources

namespace {

std::string lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

// The spike's settings file: never the product's. See the header comment.
std::string spikeConfigPath(const char* argv0) {
    if (const char* p = std::getenv("FOXSDR_SLINT_CONFIG"); p != nullptr && *p != '\0') { return p; }
    std::filesystem::path exe = std::filesystem::absolute(argv0 != nullptr ? argv0 : ".");
    return (exe.parent_path() / "slint-spike-config.json").string();
}

bool isTheRealConfig(const std::string& path) {
    const char* appdata = std::getenv("APPDATA");
    if (appdata == nullptr) { return false; }
    std::error_code ec;
    const auto real = std::filesystem::weakly_canonical(std::filesystem::path(appdata) / "foxsdr" / "config.json", ec);
    const auto mine = std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
    return lower(real.string()) == lower(mine.string());
}

// --- the waterfall palette, the sheet's strength key: #0a1430 -> #1b3a6a -> #2e86c1 -> #f5a524 -> #ffffff ---
slint::Rgba8Pixel heat(float t) {
    struct Stop { float at; std::uint8_t r, g, b; };
    static const std::array<Stop, 5> stops{{{0.00f, 0x0a, 0x14, 0x30}, {0.30f, 0x1b, 0x3a, 0x6a}, {0.55f, 0x2e, 0x86, 0xc1},
                                            {0.80f, 0xf5, 0xa5, 0x24}, {1.00f, 0xff, 0xff, 0xff}}};
    t = std::clamp(t, 0.0f, 1.0f);
    for (std::size_t i = 1; i < stops.size(); ++i) {
        if (t <= stops[i].at) {
            const float f = (t - stops[i - 1].at) / (stops[i].at - stops[i - 1].at);
            const auto mix = [f](std::uint8_t a, std::uint8_t b) { return static_cast<std::uint8_t>(a + (b - a) * f); };
            return {mix(stops[i - 1].r, stops[i].r), mix(stops[i - 1].g, stops[i].g), mix(stops[i - 1].b, stops[i].b), 255};
        }
    }
    return {255, 255, 255, 255};
}

constexpr std::uint32_t kSpecW = 1024, kSpecH = 340;   // the spectrum well's painted resolution (image-fit: fill scales it)
constexpr std::uint32_t kWfW = 1024, kWfH = 238;       // the waterfall picture: one row per published frame

struct Wells {
    slint::SharedPixelBuffer<slint::Rgba8Pixel> waterfall{kWfW, kWfH};
    std::vector<float> peak;  // peak hold, one per painted column
    Wells() {
        for (auto& p : waterfall) { p = {0x06, 0x07, 0x0a, 255}; }
    }

    // The spectrum well: graticule, the live trace (#7ad7ff) and the peak-hold trace (#ff9f43) over the sheet's #0d0f12.
    slint::Image paintSpectrum(const std::vector<float>& bins, double dbMin, double dbMax) {
        slint::SharedPixelBuffer<slint::Rgba8Pixel> buf(kSpecW, kSpecH);
        for (auto& p : buf) { p = {0x0d, 0x0f, 0x12, 255}; }
        auto* px = buf.begin();
        const auto put = [&](int x, int y, slint::Rgba8Pixel c) {
            if (x >= 0 && y >= 0 && x < static_cast<int>(kSpecW) && y < static_cast<int>(kSpecH)) { px[y * kSpecW + x] = c; }
        };
        // graticule every 10% (the sheet's #1e2229 lines)
        for (std::uint32_t gy = 0; gy < 10; ++gy) { const int y = static_cast<int>(gy * kSpecH / 10); for (std::uint32_t x = 0; x < kSpecW; ++x) { put(static_cast<int>(x), y, {0x1e, 0x22, 0x29, 255}); } }
        for (std::uint32_t gx = 0; gx < 10; ++gx) { const int x = static_cast<int>(gx * kSpecW / 10); for (std::uint32_t y = 0; y < kSpecH; ++y) { put(x, static_cast<int>(y), {0x1e, 0x22, 0x29, 255}); } }
        if (bins.empty()) { return slint::Image(buf); }
        if (peak.size() != kSpecW) { peak.assign(kSpecW, -200.0f); }
        const double range = std::max(1.0, dbMax - dbMin);
        auto yOf = [&](float db) { return static_cast<int>((1.0 - std::clamp((db - dbMin) / range, 0.0, 1.0)) * (kSpecH - 1)); };
        int lastLive = -1, lastPeak = -1;
        for (std::uint32_t x = 0; x < kSpecW; ++x) {
            const std::size_t b0 = static_cast<std::size_t>(x) * bins.size() / kSpecW;
            const std::size_t b1 = std::max(b0 + 1, static_cast<std::size_t>(x + 1) * bins.size() / kSpecW);
            float v = -300.0f;
            for (std::size_t b = b0; b < b1 && b < bins.size(); ++b) { v = std::max(v, bins[b]); }
            peak[x] = std::max(peak[x] - 0.15f, v);  // the peak decays slowly, as the sheet's trace implies
            const int yl = yOf(v), yp = yOf(peak[x]);
            // vertical join to the previous column so the traces are continuous lines
            if (lastPeak >= 0) { for (int y = std::min(yp, lastPeak); y <= std::max(yp, lastPeak); ++y) { put(static_cast<int>(x), y, {0xff, 0x9f, 0x43, 255}); } }
            if (lastLive >= 0) { for (int y = std::min(yl, lastLive); y <= std::max(yl, lastLive); ++y) { put(static_cast<int>(x), y, {0x7a, 0xd7, 0xff, 255}); } }
            put(static_cast<int>(x), yp, {0xff, 0x9f, 0x43, 255});
            put(static_cast<int>(x), yl, {0x7a, 0xd7, 0xff, 255});
            lastLive = yl; lastPeak = yp;
        }
        return slint::Image(buf);
    }

    // The waterfall: scroll the picture down one row and paint the new frame along the top.
    slint::Image scrollWaterfall(const std::vector<float>& bins, double dbMin, double dbMax) {
        auto* px = waterfall.begin();
        std::memmove(px + kWfW, px, sizeof(slint::Rgba8Pixel) * kWfW * (kWfH - 1));
        const double range = std::max(1.0, dbMax - dbMin);
        for (std::uint32_t x = 0; x < kWfW; ++x) {
            float v = -300.0f;
            if (!bins.empty()) {
                const std::size_t b0 = static_cast<std::size_t>(x) * bins.size() / kWfW;
                const std::size_t b1 = std::max(b0 + 1, static_cast<std::size_t>(x + 1) * bins.size() / kWfW);
                for (std::size_t b = b0; b < b1 && b < bins.size(); ++b) { v = std::max(v, bins[b]); }
            }
            px[x] = heat(static_cast<float>((v - dbMin) / range));
        }
        // a copy: the Image takes the buffer as it is now, the next scroll edits our own
        return slint::Image(slint::SharedPixelBuffer<slint::Rgba8Pixel>(kWfW, kWfH, waterfall.begin()));
    }
};

const char* modeName(std::uint32_t demod) {
    switch (demod) {
        case FOXAPI_DEMOD_NFM: return "NFM"; case FOXAPI_DEMOD_WFM: return "WFM"; case FOXAPI_DEMOD_AM: return "AM";
        case FOXAPI_DEMOD_DSB: return "DSB"; case FOXAPI_DEMOD_USB: return "USB"; case FOXAPI_DEMOD_CW: return "CW";
        case FOXAPI_DEMOD_LSB: return "LSB"; case FOXAPI_DEMOD_RAW: return "RAW"; default: return "?";
    }
}

std::string fmt(const char* f, double v) { char b[64]; std::snprintf(b, sizeof(b), f, v); return b; }
std::string fmtMHz(double hz) { return fmt("%.6f MHz", hz / 1e6); }

// The counter's ten digits of a frequency in Hz: GHz, MHz x3, kHz x3, Hz x3 (the sheet's "0 . 1 4 5 . 5 0 0 . 0 0 0").
std::vector<int> digitsOf(double hz) {
    long long n = std::llround(std::max(0.0, hz));
    std::vector<int> d(10, 0);
    for (int i = 9; i >= 0; --i) { d[static_cast<std::size_t>(i)] = static_cast<int>(n % 10); n /= 10; }
    return d;
}
int leadingZeros(const std::vector<int>& d) { int n = 0; for (int v : d) { if (v != 0) { break; } ++n; } return std::min(n, 9); }
double stepOfDigit(int index) { double s = 1.0; for (int i = 9; i > index; --i) { s *= 10.0; } return s; }

}  // namespace

int main(int argc, char** argv) {
    using namespace cascade::engine;

    const std::string configPath = spikeConfigPath(argc > 0 ? argv[0] : nullptr);
    if (isTheRealConfig(configPath) && std::getenv("FOXSDR_SLINT_REAL") == nullptr) {
        std::fprintf(stderr, "foxsdr-slint: refusing to run on the product's own settings file (%s)\n", configPath.c_str());
        return 2;
    }
    std::fprintf(stderr, "foxsdr-slint: settings %s\n", configPath.c_str());

    // THE ENGINE'S SERVICE, as src/main.cpp holds it: built first, destroyed last; the window is its client.
    WindowServiceRun run;
    run.configPath = configPath;
    run.frames = -1;
    EngineService service(windowServiceOptions(run));
    const EngineService::Restored restored = service.restore();
    (void)restored;

    const FoxEngineApi* api = engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR);
    FoxEngine* handle = service.engine().apiHandle();
    if (api == nullptr || handle == nullptr) { std::fprintf(stderr, "foxsdr-slint: no API table\n"); return 1; }

    FoxSessionParams p{};
    p.structSize = sizeof(p);
    p.flags = FOXAPI_SESSION_LOCAL | FOXAPI_SESSION_FRAME;   // FRAME: our heartbeat kicks the free-running pass, as the window's does
    p.grants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS;
    p.clientName = "FoxSDR Slint interface (spike)";
    p.clientVersion = cascade::versionString();
    FoxSession* session = nullptr;
    if (api->open_session(handle, &p, &session) != FOXAPI_OK || session == nullptr) {
        std::fprintf(stderr, "foxsdr-slint: open_session failed\n");
        return 1;
    }

    std::string why;
    if (!service.start(why)) { std::fprintf(stderr, "foxsdr-slint: %s\n", why.c_str()); return 1; }

    auto ui = MainWindow::create();
    ui->set_version(slint::SharedString(cascade::versionString()));

    // ---- commands: every key becomes one submit() ----
    // submit() answers at once with a ticket per command (or NO_CHANGE for one merged into a pending twin, or BUSY);
    // what the engine then DID with it - including a refusal - is a FoxCommandResult drained by poll_results in the tick.
    const auto submit = [&](FoxCommand c) {
        c.structSize = sizeof(c);
        FoxSubmitResult r{}; r.structSize = sizeof(r);
        const int32_t rc = api->submit(session, &c, 1, &r);
        if (rc < 0) { std::fprintf(stderr, "foxsdr-slint: submit op 0x%04x refused by the call: %d\n", c.op, rc); }
        else if (r.status == FOXAPI_BUSY) { std::fprintf(stderr, "foxsdr-slint: submit op 0x%04x: the session's queue is full\n", c.op); }
    };
    FoxReceiverState state{}; state.structSize = sizeof(state);
    std::string gainStage; double gainMin = 0, gainMax = 1;
    bool gainAuto = false;

    ui->on_stop_clicked([&] { FoxCommand c{}; c.op = FOXAPI_OP_RUN; c.ival[0] = (state.flags & FOXAPI_RX_RUNNING) ? 0 : 1; submit(c); });
    ui->on_nudge([&](int digit, int dir) { FoxCommand c{}; c.op = FOXAPI_OP_STEP_TUNE; c.ival[0] = dir; c.num[0] = stepOfDigit(digit); submit(c); });
    ui->on_mode_selected([&](int i) { FoxCommand c{}; c.op = FOXAPI_OP_SET_MODE; c.ival[0] = i + 1; submit(c); });  // NFM=1 .. RAW=8
    ui->on_squelch_changed([&](float v) { FoxCommand c{}; c.op = FOXAPI_OP_SET_SQUELCH; c.num[0] = -140.0 + 140.0 * v; submit(c); });  // the sheet's -85 dB at 29%
    ui->on_squelch_auto_clicked([&] { FoxCommand c{}; c.op = FOXAPI_OP_SET_SQUELCH; c.num[0] = -200.0; submit(c); });  // "always open" is the table's auto
    ui->on_volume_changed([&](float v) { FoxCommand c{}; c.op = FOXAPI_OP_SET_VOLUME; c.num[0] = v; submit(c); });
    ui->on_gain_changed([&](float v) {
        if (gainStage.empty()) { return; }
        FoxCommand c{}; c.op = FOXAPI_OP_SET_GAIN; std::snprintf(c.text, sizeof(c.text), "%s", gainStage.c_str());
        c.num[0] = gainMin + (gainMax - gainMin) * v; submit(c);
    });
    ui->on_gain_auto_clicked([&] { FoxCommand c{}; c.op = FOXAPI_OP_SET_DEVICE_AGC; c.ival[0] = gainAuto ? 0 : 1; submit(c); });
    ui->on_bias_tee_clicked([&] { FoxCommand c{}; c.op = FOXAPI_OP_SET_BIAS_TEE; c.ival[0] = (state.flags & FOXAPI_RX_BIAS_TEE_ON) ? 0 : 1; submit(c); });
    ui->on_bank_selected([&](int i) { ui->set_bank_index(i); });
    ui->on_view_selected([&](int i) { ui->set_view_index(i); });

    // ---- the tick: heartbeat, read_state, read_spectrum, poll_results -> properties ----
    Wells wells;
    std::vector<float> bins(8192);
    std::uint64_t sinceSeq = 0;
    std::uint64_t lastListSeq = ~0ull;
    slint::Timer tick;
    tick.start(slint::TimerMode::Repeated, std::chrono::milliseconds(33), [&] {
        (void)api->heartbeat(session);
        if (api->read_state(session, &state) < 0) { return; }

        // lamps + strip
        ui->set_running((state.flags & FOXAPI_RX_RUNNING) != 0);
        ui->set_decoding((state.flags & FOXAPI_RX_DECODER_ACTIVE) != 0);
        ui->set_muted((state.flags & FOXAPI_RX_MUTED) != 0);
        ui->set_failed((state.flags & FOXAPI_RX_FAULTED) != 0);
        ui->set_tuned_text(slint::SharedString(fmtMHz(state.tunedHz)));
        const auto d = digitsOf(state.tunedHz);
        ui->set_digits(std::make_shared<slint::VectorModel<int>>(d));
        ui->set_leading_zeros(leadingZeros(d));
        ui->set_mode_index(static_cast<int>(state.demodMode) - 1);
        ui->set_mode_hint(slint::SharedString("Passband " + fmt("%.1f kHz", state.bandwidthHz / 1e3) + " · more in Signal › Radio"));
        ui->set_squelch_auto(state.squelchDb <= -199.0);
        ui->set_squelch_norm(static_cast<float>(std::clamp((state.squelchDb + 140.0) / 140.0, 0.0, 1.0)));
        ui->set_squelch_text(slint::SharedString(state.squelchDb <= -199.0 ? std::string("open") : fmt("%.0f dB", state.squelchDb)));
        ui->set_volume_norm(static_cast<float>(state.volume));
        ui->set_volume_text(slint::SharedString(fmt("%.2f", state.volume)));
        ui->set_level_norm(static_cast<float>(std::clamp((state.audioLevelDb + 60.0) / 60.0, 0.0, 1.0)));
        ui->set_level_text(slint::SharedString(fmt("%.1f dB", state.audioLevelDb)));
        ui->set_bias_tee_on((state.flags & FOXAPI_RX_BIAS_TEE_ON) != 0);
        ui->set_bias_hint(slint::SharedString((state.flags & FOXAPI_RX_BIAS_TEE_PRESENT) ? "4.5 V on the cable · asks first" : "No bias tee on this radio"));
        gainAuto = (state.flags & FOXAPI_RX_DEVICE_AGC) != 0;
        ui->set_gain_auto(gainAuto);
        ui->set_source_line(slint::SharedString(state.deviceName));
        ui->set_rate_line(slint::SharedString(fmt("%.3f MS/s", state.sampleRateHz / 1e6) + " · ch " + fmt("%.1f kHz", state.channelRateHz / 1e3) + " · underruns " + std::to_string(state.audioUnderruns)));
        ui->set_waterfall_foot_right(slint::SharedString(std::string(modeName(state.demodMode)) + " · decoders " + std::to_string(state.decodersRunning) + "/" + std::to_string(state.decodersFitted)));

        // the gain stage: the first listed stage's name and range (FOXAPI_LIST_GAINS: value[0..3] = min, max, step, current)
        if (state.listSeq != lastListSeq) {
            lastListSeq = state.listSeq;
            FoxListItem it{}; it.structSize = sizeof(it);
            if (api->read_list(session, FOXAPI_LIST_GAINS, 0, &it) > 0) {
                gainStage = it.name; gainMin = it.value[0]; gainMax = it.value[1];
                ui->set_gain_hint(slint::SharedString(std::string(it.name) + " stage · " + state.deviceName));
            } else {
                // the inventory's deck rule: readout "N/A" with no gain stage (the dial is dimmed there; here the slider just idles)
                gainStage.clear(); ui->set_gain_hint(slint::SharedString("no gain stage")); ui->set_gain_text(slint::SharedString("N/A")); ui->set_gain_norm(0.0f);
            }
        }
        if (!gainStage.empty()) {
            FoxListItem it{}; it.structSize = sizeof(it);
            if (api->read_list(session, FOXAPI_LIST_GAINS, 0, &it) > 0) {
                const double range = std::max(1e-9, gainMax - gainMin);
                ui->set_gain_norm(static_cast<float>(std::clamp((it.value[3] - gainMin) / range, 0.0, 1.0)));
                ui->set_gain_text(slint::SharedString(fmt("%.0f ", it.value[3]) + it.detail));
            }
        }

        // status bar
        std::vector<StatusCard> cards;
        cards.push_back({slint::SharedString("RECEIVER"), slint::SharedString((state.flags & FOXAPI_RX_RUNNING) ? "RUNNING" : "STOPPED"),
                         slint::SharedString(std::string(state.deviceName) + " · " + fmt("%.3f MS/s", state.sampleRateHz / 1e6)), false, false,
                         (state.flags & FOXAPI_RX_RUNNING) ? LampState::Green : LampState::Off, true});
        cards.push_back({slint::SharedString("DECODERS"), slint::SharedString(std::to_string(state.decodersRunning) + " running"),
                         slint::SharedString("of " + std::to_string(state.decodersFitted) + " fitted"), state.decodersRunning > 0, false, LampState::Off, false});
        cards.push_back({slint::SharedString("SINK"), slint::SharedString((state.flags & FOXAPI_RX_SINK_OPEN) ? "OPEN" : "closed"),
                         slint::SharedString(state.sinkName), (state.flags & FOXAPI_RX_SINK_OPEN) != 0, false, LampState::Off, false});
        cards.push_back({slint::SharedString("WEB"), slint::SharedString((state.flags & FOXAPI_RX_WEB_LISTENING) ? "listening" : "off"), slint::SharedString(""), false, false, LampState::Off, false});
        cards.push_back({slint::SharedString("UNDERRUNS"), slint::SharedString(std::to_string(state.audioUnderruns)), slint::SharedString(""), false, state.audioUnderruns > 0, LampState::Off, false});
        if (state.faultMessage[0] != '\0') { cards.push_back({slint::SharedString("FAULT"), slint::SharedString(state.faultMessage), slint::SharedString(""), false, true, LampState::Red, true}); }
        ui->set_status(std::make_shared<slint::VectorModel<StatusCard>>(std::move(cards)));

        // the spectrum: only when the engine published a newer frame
        FoxSpectrumInfo info{}; info.structSize = sizeof(info);
        const int32_t rc = api->read_spectrum(session, sinceSeq, &info, bins.data(), static_cast<uint32_t>(bins.size()));
        if (rc == FOXAPI_OK && info.copied > 0) {
            sinceSeq = info.seq;
            std::vector<float> frame(bins.begin(), bins.begin() + info.copied);
            ui->set_spectrum_image(wells.paintSpectrum(frame, state.dbMin, state.dbMax));
            ui->set_waterfall_image(wells.scrollWaterfall(frame, state.dbMin, state.dbMax));
            ui->set_span_text(slint::SharedString("SPAN " + fmt("%.3f MHz", info.spanHz / 1e6)));
            ui->set_spectrum_caption(slint::SharedString("Spectrum · " + std::to_string(info.binCount) + " bin"));
            const auto peakIt = std::max_element(frame.begin(), frame.end());
            if (peakIt != frame.end()) { ui->set_peak_text(slint::SharedString(fmt("%.1f dBFS", *peakIt))); ui->set_peak_age(slint::SharedString("now")); }
        }

        // answers: log anything the engine refused (the spike has no refusal UI yet)
        FoxCommandResult r[8]; for (auto& x : r) { x.structSize = sizeof(x); }
        const int32_t got = api->poll_results(session, r, 8);
        for (int32_t i = 0; i < got; ++i) {
            if (r[i].status != FOXAPI_OK && r[i].status != FOXAPI_NO_CHANGE) { std::fprintf(stderr, "foxsdr-slint: op 0x%04x answered %d: %s\n", r[i].op, r[i].status, r[i].message); }
        }
    });

    ui->run();

    tick.stop();
    api->close_session(session);
    service.stop();
    return 0;
}
