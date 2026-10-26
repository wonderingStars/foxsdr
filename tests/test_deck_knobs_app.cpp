/*
 * THE TOP BAR'S GAIN, SQUELCH AND VOLUME KNOBS, AND THE TWO AUTO KEYS UNDER THE
 * FIRST TWO, IN THE REAL APPLICATION (0.99.74 feature batch). The tester, 2026-10-12:
 * "Add a gain knob, just like the Volume knob at the top of the screen with a switch
 * for Auto Gain" and "Add a Squelch knob, like the gain knob above but with a button
 * for AUTO ... I would put these knobs in the order of gain, squelch and volume just
 * as that is the order the signal goes through the system."
 *
 * The geometry is pinned in tests/test_tune_control.cpp and the noise-floor arithmetic
 * in tests/test_noise_floor_tracker.cpp; this runs the application itself (AppWindow::run,
 * in a child of this very test, so that a FAKE RADIO can be put behind the window the way
 * tests/test_autogain_app.cpp does - cascade.exe has no way to be handed one) and checks,
 * from the UI census and from what the fake radio was told:
 *
 *   parts      the four new parts are drawn at 1600 x 1000 and at the bar a fresh install
 *              opens with (1280 x 720), in the order GAIN, SQUELCH, VOLUME, each AUTO key
 *              under its own knob, all three dials one size, none on another deck part,
 *              and both meters STILL on the bar at 1280 x 720;
 *   gain auto  a press of "deck:gainauto" switches the fake radio's automatic gain ON (the
 *              radio's own setAutoGain call, and the remembered per-radio switch in the saved
 *              config), a second press OFF; on a radio WITHOUT automatic gain the key is
 *              drawn and a press asks the radio for nothing;
 *   gain knob  a twist of "deck:gain" sets the radio's first stage, paced (never more than one
 *              call a 50 ms slot of the run's OWN clock, read back from its frame times: a slow
 *              machine's frames are each a slot of their own) with the last call the value the
 *              hand ended on; under automatic gain the knob is dead and the radio is not asked;
 *   squelch    a twist of "deck:squelch" changes the saved squelchDb; a press of
 *              "deck:squelchauto" saves squelchAuto true; a twist AFTER that saves it false
 *              (a hand on the knob is a manual choice); and with AUTOMATIC on the threshold
 *              follows the measured noise: the fake radio puts out white noise at two levels
 *              20 dB apart and the saved threshold lands about 20 dB apart;
 *   carrier    (follow-up) a carrier that holds the squelch gate open for longer than the
 *              tracker's window does not teach AUTOMATIC the floor: the threshold stays where
 *              the quiet noise put it (-53.5), not dragged up toward the carrier (-24);
 *   airband    (follow-up, the tester: "I really like the Airband tool and find myself
 *              adjusting the Squelch often when doing so") while the Airband monitor LISTENS,
 *              a twist of "deck:squelch" sets the MONITOR's squelch (airbandSquelchDb_, read
 *              back from the child at the end of the run) and leaves the receiver's alone;
 *              AUTOMATIC applies the monitor's "Above the noise" rule (two noise levels 20 dB
 *              apart give two thresholds about 20 dB apart, the receiver's threshold untouched);
 *              the dial, the Airband section's own slider and its Above the noise key each
 *              turn AUTOMATIC off; and once the monitor is stopped the dial is the receiver's
 *              again.
 *
 * The presses and twists are the application's own input script, aimed from the census.
 * Isolated like every other test that starts the application.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/bias_tee_memory.hpp"
#include "core/config.hpp"
#include "core/patch_devices.hpp"
#include "gui/app_window.hpp"
#include "source/device_source.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

const std::string kRtlA = "serial=0000000A";

// --- the fake radio (the child's) ----------------------------------------------------------
//
// Every call the application makes that this test is about is appended to a text file the
// parent reads: "agc 0|1" for setAutoGain, "gain <db>" for setGainDb. The child is another
// process, so a file is the only witness there is.
struct FakeEnv {
    bool hasAgc = true;
    std::string logPath;
    double noiseDb = -60.0;  // total power of the white noise read() puts out
    // A BURST: between burstFromS and burstToS after the first read the noise is burstDb
    // instead (a carrier that holds the channel; burstToS <= burstFromS is none).
    double burstFromS = 0.0;
    double burstToS = 0.0;
    double burstDb = -60.0;
};
FakeEnv g_fake;
std::mutex g_logMutex;

void logLine(const std::string& line) {
    if (g_fake.logPath.empty()) { return; }
    std::lock_guard<std::mutex> lk(g_logMutex);
    std::ofstream f(g_fake.logPath, std::ios::app);
    f << line << '\n';
}

class FakeRadio final : public cascade::source::DeviceSource {
public:
    bool start() override {
        abort_ = false;
        return true;
    }
    void stop() override { abort_ = true; }
    bool running() const override { return !abort_; }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return rate_; }
    bool setSampleRateHz(double hz) override {
        rate_ = hz;
        return true;
    }
    double centerFrequencyHz() const override { return centre_; }
    bool setCenterFrequencyHz(double hz) override {
        centre_ = hz;
        return true;
    }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        // Paced at the sample rate, so the DSP sees a radio and not a firehose.
        std::this_thread::sleep_for(
            std::chrono::microseconds(static_cast<long long>(1.0e6 * static_cast<double>(n) / rate_)));
        if (abort_) { return 0; }
        static const auto t0 = std::chrono::steady_clock::now();
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const bool burst = g_fake.burstToS > g_fake.burstFromS && now >= g_fake.burstFromS &&
                           now < g_fake.burstToS;
        const double level = burst ? g_fake.burstDb : g_fake.noiseDb;
        const float sigma = static_cast<float>(std::sqrt(std::pow(10.0, level / 10.0) * 0.5));
        std::normal_distribution<float> dist(0.0f, sigma);
        for (std::size_t i = 0; i < n; ++i) { dst[i] = {dist(rng_), dist(rng_)}; }
        return n;
    }
    const char* name() const override { return "Fake radio"; }
    const char* lastError() const override { return err_.c_str(); }

    const char* driverKey() const override { return "rtlsdr"; }
    bool open(const std::string&) override {
        open_ = true;
        return true;
    }
    void closeDevice() override { open_ = false; }
    bool isOpen() const override { return open_; }
    std::vector<cascade::source::GainInfo> gains() const override {
        return {{"TUNER", 0.0, 50.0, 0.1}};
    }
    bool setGainDb(const std::string&, double db) override {
        logLine("gain " + std::to_string(db));
        gain_ = std::round(std::min(std::max(db, 0.0), 50.0));  // the radio quantises
        return true;
    }
    double gainDb(const std::string&) const override { return gain_; }
    bool autoGainSupported() const override { return g_fake.hasAgc; }
    bool setAutoGain(bool on) override {
        logLine(std::string("agc ") + (on ? "1" : "0"));
        if (!g_fake.hasAgc) { return false; }
        agc_ = on;
        return true;
    }
    bool autoGain() const override { return agc_; }
    std::vector<std::string> antennas() const override { return {}; }
    bool setAntenna(const std::string&) override { return false; }
    std::string antenna() const override { return {}; }
    std::vector<double> supportedSampleRatesHz() const override { return {2.4e6}; }
    bool frequencyRangeHz(double& lo, double& hi) const override {
        lo = 24.0e6;
        hi = 1766.0e6;
        return true;
    }
    bool deviceDead() const override { return false; }
    std::string faultedWhile() const override { return {}; }

private:
    std::atomic<bool> abort_{true};
    double rate_ = 2.4e6;
    double centre_ = 100.0e6;
    bool open_ = false;
    bool agc_ = false;
    double gain_ = 10.0;
    std::string err_;
    std::mt19937 rng_{12345u};
};

std::unique_ptr<cascade::source::DeviceSource> makeFake(const std::string&) {
    return std::make_unique<FakeRadio>();
}

std::vector<cascade::source::NativeDeviceInfo> fakeScan() {
    return {{"rtlsdr", "Fake RTL2832U", kRtlA}};
}

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    static void installHooks() {
        AppWindow::testHooks_.makeDevice = &makeFake;
        AppWindow::testHooks_.nativeScan = &fakeScan;
    }
    // What the run ended with, for the file the parent reads: the receiver's squelch threshold,
    // the Airband monitor's, and AUTOMATIC (one flag for both).
    static float squelchDb(const AppWindow& a) { return a.squelchDb_; }
    static float airbandSquelchDb(const AppWindow& a) { return a.airbandSquelchDb_; }
    static bool squelchAuto(const AppWindow& a) { return a.squelchAuto_; }
};

}  // namespace cascade::gui

namespace {

// The child: this process IS the application for one bounded run, with the fake radio
// behind the window and FOXSDR_SOURCE_DEVICE naming it (the capture hook that opens a
// native device by its patch key once the window is up).
int childMain(const std::string& radio, int frames) {
    g_fake.hasAgc = (radio == "agc");
    if (const char* p = std::getenv("FAKE_RADIO_LOG")) { g_fake.logPath = p; }
    if (const char* n = std::getenv("FAKE_NOISE_DB")) { g_fake.noiseDb = std::atof(n); }
    // "from,to,db": a stretch of louder noise, in seconds from the first read.
    if (const char* b = std::getenv("FAKE_BURST"); b != nullptr && b[0] != '\0') {
        std::sscanf(b, "%lf,%lf,%lf", &g_fake.burstFromS, &g_fake.burstToS, &g_fake.burstDb);
    }
    // "none" is the application with no radio open at all (the generator): the knob must be
    // inert, not crash, and ask nobody for anything.
    if (radio != "none") {
        const std::string key = cascade::core::patch::makeDeviceKey("rtlsdr", kRtlA);
#if defined(_WIN32)
        ::SetEnvironmentVariableA("FOXSDR_SOURCE_DEVICE", key.c_str());
        _putenv_s("FOXSDR_SOURCE_DEVICE", key.c_str());
#else
        ::setenv("FOXSDR_SOURCE_DEVICE", key.c_str(), 1);
#endif
    }
    cascade::gui::AppWindowTestAccess::installHooks();
    const char* cfg = std::getenv("CASCADE_CONFIG_TEST");
    cascade::gui::AppWindow app(cfg != nullptr ? cfg : "", true);
    const int rc = app.run(frames);
    // What the run ended with, in the file the parent reads (the thresholds are not in the
    // saved config except the receiver's, and the Airband monitor's is not saved at all).
    if (const char* sf = std::getenv("FAKE_STATE_FILE"); sf != nullptr && sf[0] != '\0') {
        using Access = cascade::gui::AppWindowTestAccess;
        std::ofstream f(sf, std::ios::trunc);
        f << "squelch " << Access::squelchDb(app) << "\nairband " << Access::airbandSquelchDb(app)
          << "\nauto " << (Access::squelchAuto(app) ? 1 : 0) << "\n";
    }
    std::fflush(stdout);
    return rc;
}

// --- the parent --------------------------------------------------------------------------

int pid() {
#if defined(_WIN32)
    return static_cast<int>(::GetCurrentProcessId());
#else
    return static_cast<int>(::getpid());
#endif
}

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(name, value.empty() ? nullptr : value.c_str());
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) {
        ::unsetenv(name);
    } else {
        ::setenv(name, value.c_str(), 1);
    }
#endif
}

std::string selfPath(const char* argv0) {
#if defined(_WIN32)
    char buf[MAX_PATH * 2] = {};
    const DWORD n = ::GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof(buf)));
    if (n > 0 && n < sizeof(buf)) { return std::string(buf, n); }
#endif
    return argv0;
}

std::string run(const std::string& cmd) {
    std::string out;
#if defined(_WIN32)
    FILE* p = _popen(("\"" + cmd + "\"").c_str(), "r");
#else
    FILE* p = popen(cmd.c_str(), "r");
#endif
    char buf[512];
    while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { out += buf; }
#if defined(_WIN32)
    if (p != nullptr) { _pclose(p); }
#else
    if (p != nullptr) { pclose(p); }
#endif
    return out;
}

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float cx() const { return (x0 + x1) * 0.5f; }
    float cy() const { return (y0 + y1) * 0.5f; }
    float w() const { return x1 - x0; }
    float h() const { return y1 - y0; }
};

bool overlaps(const Rect& a, const Rect& b) {
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

struct Result {
    bool ok = false;
    std::set<std::string> items;
    std::map<std::string, Rect> rects;
    cascade::core::AppConfig saved;
    std::vector<std::string> log;  // the fake radio's calls, in order
    std::map<std::string, double> state;  // squelch / airband / auto, as the run ended
    // Frame number -> the application's own clock in seconds (ImGui::GetTime, the one the dial's
    // pace is read against), from FOXSDR_SCRIPT_TRACE; filled only for a run asked to trace.
    std::map<long, double> frameT;
    double st(const char* k) const {
        const auto it = state.find(k);
        return it == state.end() ? std::numeric_limits<double>::quiet_NaN() : it->second;
    }
};

constexpr int kFrames = 100;
fs::path g_dir;
std::string g_self;

bool has(const Result& r, const char* rect) { return r.rects.count(rect) == 1; }

// Two ticked AM rows 600 kHz apart (the Airband test's own list): the monitor tunes the one
// centre that holds both, 120.000 MHz, and plays them each behind its own squelch.
const char* kTwoRows =
    "{\n"
    "  \"schemaVersion\": 1,\n"
    "  \"bookmarks\": [\n"
    "    { \"name\": \"Low\", \"freqHz\": 119700000.0, \"mode\": \"AM\", \"bandwidthHz\": 10000.0,\n"
    "      \"scan\": true },\n"
    "    { \"name\": \"High\", \"freqHz\": 120300000.0, \"mode\": \"AM\", \"bandwidthHz\": 10000.0,\n"
    "      \"scan\": true }\n"
    "  ]\n"
    "}\n";

// What a run may add to the plain one: the Airband section's list and rail bank, and a burst.
struct Extra {
    bool airband = false;     // the two ticked rows above, the TOOLS bank with the Airband section
    std::string burst;        // "from,to,db" for the fake radio (seconds from its first read)
    bool trace = false;       // keep the run's frame times (needs a script), in Result::frameT
};

// `radio` is "agc" or "noagc"; `script` may be empty; `extraConfig` is JSON members to append.
Result once(const std::string& tag, const std::string& radio, const std::string& script,
            const std::string& size, const std::string& extraConfig, int frames = kFrames,
            double noiseDb = -60.0, bool startReceiver = false, const Extra& ex = Extra{}) {
    Result r;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path census = g_dir / (tag + ".census");
    const fs::path log = g_dir / (tag + ".fakelog");
    const fs::path state = g_dir / (tag + ".state");
    const fs::path trace = g_dir / (tag + ".trace");
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, "
             "\"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", "
             "\"mainView\": \"receiver\", \"volume\": 0.0"
          << (ex.airband ? ", \"railBank\": 2, \"bandPlanOverlay\": false" : "") << extraConfig
          << " }\n";
    }
    if (ex.airband) {
        // The frequency list the app reads from its (scratch) per-user folder.
        const fs::path bm = g_dir / "appdata" / "foxsdr" / "bookmarks.json";
        std::error_code mec;
        fs::create_directories(bm.parent_path(), mec);
        std::ofstream f(bm, std::ios::binary | std::ios::trunc);
        f << kTwoRows;
    }
    std::error_code ec;
    fs::remove(log, ec);
    fs::remove(state, ec);
    fs::remove(trace, ec);
    // One line a frame, "frame N t=<seconds>", appended by the child (a scripted run only).
    setEnv("FOXSDR_SCRIPT_TRACE", ex.trace ? trace.string() : std::string());
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_WINDOW_SIZE", size);
    setEnv("FAKE_RADIO_LOG", log.string());
    setEnv("FAKE_STATE_FILE", state.string());
    setEnv("FAKE_BURST", ex.burst);
    // The Airband section opened (a row's open state is ImGui's own storage, so only this
    // verification hook can) and the rail held on its bank, as tests/test_airband_app.cpp does.
    setEnv("FOXSDR_OPEN_SECTIONS", ex.airband ? "airband" : "");
    setEnv("FOXSDR_CENSUS_HOLD_BANK", ex.airband ? "1" : "");
    setEnv("FAKE_NOISE_DB", std::to_string(noiseDb));
    // The receiver stays stopped in a bounded run unless this is set (the decoder-test hook);
    // only the tracking run needs the DSP to be running at all.
    setEnv("CASCADE_DECODE_TEST", startReceiver ? "1" : "");
    if (script.empty()) {
        setEnv("FOXSDR_INPUT_SCRIPT", "");
    } else {
        const fs::path sp = g_dir / (tag + ".script");
        std::ofstream f(sp);
        f << script;
        f.close();
        setEnv("FOXSDR_INPUT_SCRIPT", sp.string());
    }
    const std::string out = run("\"" + g_self + "\" --child " + radio + " " +
                                std::to_string(frames) + " 2>&1");
    const bool rendered =
        out.find("rendered " + std::to_string(frames) + " frames") != std::string::npos;
    const bool written = out.find("ui census written") != std::string::npos;
    const bool scripted = script.empty() || out.find("script steps run") != std::string::npos;
    if (!rendered || !written || !scripted) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", tag.c_str(), out.c_str());
        return r;
    }
    {
        std::ifstream in(census);
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("item ", 0) == 0) { r.items.insert(line.substr(5)); }
            if (line.rfind("rect ", 0) != 0) { continue; }
            std::istringstream ss(line.substr(5));
            std::string name;
            Rect rc;
            ss >> name >> rc.x0 >> rc.y0 >> rc.x1 >> rc.y1;
            r.rects[name] = rc;
        }
    }
    {
        std::ifstream in(log);
        std::string line;
        while (std::getline(in, line)) { r.log.push_back(line); }
    }
    {
        std::ifstream in(state);
        std::string k;
        double v = 0.0;
        while (in >> k >> v) { r.state[k] = v; }
    }
    if (ex.trace) {
        std::ifstream in(trace);
        std::string line;
        while (std::getline(in, line)) {
            long frame = 0;
            double t = 0.0;
            if (std::sscanf(line.c_str(), "frame %ld t=%lf", &frame, &t) == 2) { r.frameT[frame] = t; }
        }
    }
    std::string err;
    if (!cascade::core::ConfigStore::load(cfg.string(), r.saved, err)) {
        std::printf("  %s: the saved config did not load: %s\n", tag.c_str(), err.c_str());
        return r;
    }
    r.ok = has(r, "deck:bar");
    if (!r.ok) { std::printf("  %s: no deck rectangle\n", tag.c_str()); }
    return r;
}

// A press: the pointer to the part's centre, down, up, and then away from it.
std::string press(const Rect& k, int frame) {
    char buf[256];
    std::snprintf(buf, sizeof buf, "%d screen %.0f %.0f\n%d down\n%d up\n%d screen %.0f %.0f\n",
                  frame, k.cx(), k.cy(), frame + 2, frame + 4, frame + 6, k.cx(),
                  k.cy() + 300.0f);
    return buf;
}

// A TWIST of a dial, clockwise through `quarters` quarter turns of the pointer about its
// centre (10 px out): top, right, bottom ... The dial reads the pointer's ANGLE about its
// centre, so a quarter turn is 90 degrees of its 270-degree travel.
std::string twist(const Rect& k, int frame, int quarters) {
    const float r = 10.0f;
    const float dx[4] = {0.0f, r, 0.0f, -r};
    const float dy[4] = {-r, 0.0f, r, 0.0f};
    std::string s;
    char buf[128];
    std::snprintf(buf, sizeof buf, "%d screen %.0f %.0f\n%d down\n", frame, k.cx() + dx[0],
                  k.cy() + dy[0], frame + 1);
    s += buf;
    for (int q = 1; q <= quarters; ++q) {
        std::snprintf(buf, sizeof buf, "%d screen %.0f %.0f\n", frame + 1 + 2 * q,
                      k.cx() + dx[q % 4], k.cy() + dy[q % 4]);
        s += buf;
    }
    std::snprintf(buf, sizeof buf, "%d up\n%d screen %.0f %.0f\n", frame + 3 + 2 * quarters,
                  frame + 5 + 2 * quarters, k.cx(), k.cy() + 300.0f);
    s += buf;
    return s;
}

// A SLOW TWIST: the pointer walks `steps` frames round the dial's centre at 18 px, `degPerStep`
// degrees a frame from straight up, one script step per frame - the gesture a hand makes, where
// twist() jumps a quarter turn in one frame. Every one of those frames moves the dial, so a radio
// asked once a frame would be asked `steps` times.
std::string slowTwist(const Rect& k, int frame, int steps, float degPerStep) {
    const float r = 18.0f;
    const float pi = 3.14159265f;
    std::string s;
    char buf[128];
    std::snprintf(buf, sizeof buf, "%d screen %.0f %.0f\n%d down\n", frame, k.cx(), k.cy() - r,
                  frame + 1);
    s += buf;
    for (int i = 1; i <= steps; ++i) {
        const float a = static_cast<float>(i) * degPerStep * pi / 180.0f;
        std::snprintf(buf, sizeof buf, "%d screen %.0f %.0f\n", frame + 1 + i,
                      k.cx() + r * std::sin(a), k.cy() - r * std::cos(a));
        s += buf;
    }
    std::snprintf(buf, sizeof buf, "%d up\n%d screen %.0f %.0f\n", frame + 2 + steps,
                  frame + 4 + steps, k.cx(), k.cy() + 300.0f);
    s += buf;
    return s;
}

// A pause of real time, so the open radio has been adopted before the first gesture.
std::string settle() { return "3 sleep 150\n"; }

// The gain values the fake was asked for, in order.
std::vector<double> gainCalls(const Result& r) {
    std::vector<double> v;
    for (const std::string& l : r.log) {
        if (l.rfind("gain ", 0) == 0) { v.push_back(std::atof(l.c_str() + 5)); }
    }
    return v;
}

std::vector<std::string> agcCalls(const Result& r) {
    std::vector<std::string> v;
    for (const std::string& l : r.log) {
        if (l.rfind("agc ", 0) == 0) { v.push_back(l); }
    }
    return v;
}

// WHAT A SCRIPTED GESTURE TOOK, from the run's own frame times (Result::frameT): the clock a
// dial's pace is read against, so a count of radio calls can be judged against it and not
// against a number of frames, which are a different length on every machine.
struct Timing {
    bool ok = false;             // every frame from `first` to `last` is in the trace
    double spanMs = 0.0;         // frame `first` to frame `last`
    double frameMs = 0.0;        // the mean frame inside it
    double shortestGapMs = 0.0;  // the shortest frame-to-frame gap over frames gapFrom..gapTo
};

Timing timingOf(const Result& r, long first, long last, long gapFrom, long gapTo) {
    Timing t;
    for (long f = std::min(first, gapFrom - 1); f <= std::max(last, gapTo); ++f) {
        if (r.frameT.count(f) == 0) { return t; }
    }
    t.spanMs = (r.frameT.at(last) - r.frameT.at(first)) * 1000.0;
    t.frameMs = t.spanMs / static_cast<double>(last - first);
    t.shortestGapMs = std::numeric_limits<double>::infinity();
    for (long f = gapFrom; f <= gapTo; ++f) {
        t.shortestGapMs = std::min(t.shortestGapMs, (r.frameT.at(f) - r.frameT.at(f - 1)) * 1000.0);
    }
    t.ok = true;
    return t;
}

int memoryOf(const cascade::core::AppConfig& c) {
    const auto it = c.autoGainByRadio.find(cascade::core::biasTeeRadioKey("rtlsdr", kRtlA));
    return it == c.autoGainByRadio.end() ? -1 : (it->second ? 1 : 0);
}

// --- the checks ------------------------------------------------------------------------------

void partsAt(const std::string& size, bool metersMustShow) {
    std::printf("  parts at %s\n", size.c_str());
    const Result r = once("parts-" + size, "agc", "", size, ", \"squelchDb\": -60");
    CHECK(r.ok);
    for (const char* part : {"deck:gain", "deck:squelch", "deck:volume", "deck:gainauto",
                             "deck:squelchauto"}) {
        if (r.items.count(part) != 1) { std::printf("    %s is not drawn\n", part); }
        CHECK(r.items.count(part) == 1);
        CHECK(has(r, part));
    }
    if (!has(r, "deck:gain") || !has(r, "deck:squelch") || !has(r, "deck:volume") ||
        !has(r, "deck:gainauto") || !has(r, "deck:squelchauto")) {
        return;
    }
    const Rect g = r.rects.at("deck:gain");
    const Rect s = r.rects.at("deck:squelch");
    const Rect v = r.rects.at("deck:volume");
    const Rect ga = r.rects.at("deck:gainauto");
    const Rect sa = r.rects.at("deck:squelchauto");
    const Rect bar = r.rects.at("deck:bar");
    std::printf("    gain %.0f..%.0f, squelch %.0f..%.0f, volume %.0f..%.0f (bar %.0f..%.0f)\n",
                g.x0, g.x1, s.x0, s.x1, v.x0, v.x1, bar.x0, bar.x1);
    // THE ORDER THE SIGNAL GOES THROUGH THE SYSTEM, left to right.
    CHECK(g.cx() < s.cx() && s.cx() < v.cx());
    // ONE SIZE, and the same row.
    CHECK_NEAR(g.w(), s.w(), 0.5);
    CHECK_NEAR(s.w(), v.w(), 0.5);
    CHECK_NEAR(g.cy(), s.cy(), 0.5);
    CHECK_NEAR(s.cy(), v.cy(), 0.5);
    // EQUALLY SPACED.
    CHECK_NEAR(s.cx() - g.cx(), v.cx() - s.cx(), 0.5);
    // EACH AUTO KEY IS UNDER ITS OWN KNOB, on the bar, clear of the foot rail.
    CHECK_NEAR(ga.cx(), g.cx(), 1.0);
    CHECK_NEAR(sa.cx(), s.cx(), 1.0);
    CHECK(ga.y0 > g.y1 && sa.y0 > s.y1);
    CHECK(ga.y1 <= bar.y1 - 3.0f && sa.y1 <= bar.y1 - 3.0f);
    CHECK(ga.x0 >= bar.x0 && sa.x1 <= bar.x1);
    // ...and none of the five stands on another deck part.
    for (const char* other : {"deck:stop", "deck:counter", "deck:meter.rate", "deck:meter.volume"}) {
        if (!has(r, other)) { continue; }
        for (const Rect* mine : {&g, &s, &v, &ga, &sa}) { CHECK(!overlaps(*mine, r.rects.at(other))); }
    }
    for (const Rect* a : {&g, &s, &v, &ga, &sa}) {
        for (const Rect* b : {&g, &s, &v, &ga, &sa}) {
            if (a != b) { CHECK(!overlaps(*a, *b)); }
        }
    }
    // THE METERS ARE STILL ON THE BAR at the width a fresh install opens with.
    if (metersMustShow) {
        CHECK(has(r, "deck:meter.rate"));
        CHECK(has(r, "deck:meter.volume"));
        if (has(r, "deck:meter.rate")) { CHECK(r.rects.at("deck:meter.rate").x0 >= v.x1); }
    }
}

void gainAuto(const Rect& key, const Rect& knob) {
    std::printf("  the GAIN AUTO key, on a radio with automatic gain\n");
    // One press: the radio's probe switched it off at open, the press switches it on.
    const Result one = once("agc-one", "agc", settle() + press(key, 30), "1600x1000", "");
    CHECK(one.ok);
    CHECK(has(one, "deck:gain") && one.items.count("deck:gain:noradio") == 0);  // a radio is open
    {
        const std::vector<std::string> calls = agcCalls(one);
        CHECK(calls.size() == 2 && calls[0] == "agc 0" && calls[1] == "agc 1");
        if (calls.size() != 2) {
            std::printf("    the radio was told:");
            for (const std::string& c : calls) { std::printf(" [%s]", c.c_str()); }
            std::printf("\n");
        }
        CHECK(memoryOf(one.saved) == 1);  // remembered for this radio
    }
    // A second press: off again.
    const Result two =
        once("agc-two", "agc", settle() + press(key, 30) + press(key, 50), "1600x1000", "");
    CHECK(two.ok);
    {
        const std::vector<std::string> calls = agcCalls(two);
        CHECK(calls.size() == 3 && calls[2] == "agc 0");
        CHECK(memoryOf(two.saved) == 0);
    }
    // UNDER AUTOMATIC GAIN THE GAIN KNOB IS DEAD: the radio is not asked for a gain.
    const Result dead = once("agc-dead", "agc",
                             settle() + press(key, 30) + twist(knob, 50, 2), "1600x1000", "");
    CHECK(dead.ok);
    CHECK(agcCalls(dead).size() == 2 && agcCalls(dead)[1] == "agc 1");
    CHECK(gainCalls(dead).empty());
}

void gainNoAuto(const Rect& key) {
    std::printf("  the GAIN AUTO key, on a radio WITHOUT automatic gain\n");
    const Result none = once("noagc", "noagc", settle() + press(key, 30), "1600x1000", "");
    CHECK(none.ok);
    CHECK(has(none, "deck:gainauto"));  // the key is present, however dead
    // A radio IS open here (it just has no automatic gain): the dial must not claim otherwise.
    CHECK(none.items.count("deck:gain:noradio") == 0);
    CHECK(agcCalls(none).empty());      // and a press asked the radio for nothing
    CHECK(memoryOf(none.saved) == -1);  // and was remembered as nothing
}

void gainNoRadio(const Rect& knob, const Rect& key) {
    std::printf("  the GAIN dial and its AUTO key with no radio open\n");
    // Pressed and twisted: the application finishes the run, the radio (there is none) is asked
    // for nothing, and nothing was remembered for any radio.
    const Result r =
        once("noradio", "none", settle() + press(key, 30) + twist(knob, 50, 2), "1600x1000", "");
    CHECK(r.ok);
    CHECK(has(r, "deck:gain") && has(r, "deck:gainauto"));
    // THE DIAL SAYS WHY IT IS DEAD (0.99.92): drawn with no device behind it, it notes
    // that, and its tooltip then names the missing radio instead of leaving "N/A" and
    // a line about what the dial is for. (The 0.99.91 field report: "the Gain button
    // no longer works" - the saved radio had been busy at start-up and nothing said so.)
    CHECK(r.items.count("deck:gain:noradio") == 1);
    CHECK(r.log.empty());
    CHECK(r.saved.autoGainByRadio.empty());
}

void gainKnob(const Rect& knob) {
    std::printf("  the GAIN knob\n");
    // Two quarter turns clockwise = 180 degrees = two thirds of the 50 dB travel, from 10 dB.
    const Result r = once("gain-twist", "agc", settle() + twist(knob, 30, 2), "1600x1000", "");
    CHECK(r.ok);
    const std::vector<double> g = gainCalls(r);
    std::printf("    the radio was set to:");
    for (const double d : g) { std::printf(" %.1f", d); }
    std::printf("\n");
    CHECK(!g.empty());
    // PACED: a gesture of a dozen frames is not a call a frame.
    CHECK(g.size() <= 6);
    if (!g.empty()) {
        // The last one is where the hand ended: 10 + 50 * 180/270 = 43.3.
        CHECK_NEAR(g.back(), 10.0 + 50.0 * 180.0 / 270.0, 3.0);
        for (std::size_t i = 1; i < g.size(); ++i) { CHECK(g[i] >= g[i - 1] - 1.0e-6); }
    }
    // The dial is a gain control only: it did not touch the automatic-gain switch (the
    // open's probe is the one call).
    CHECK(agcCalls(r).size() == 1 && agcCalls(r)[0] == "agc 0");

    // A HAND'S GESTURE: thirty-four frames, five degrees each, 170 degrees in all. The dial moves
    // on every one of them, and the radio is asked far fewer times (at most one in 50 ms while
    // the hand is on it, and once more when it lets go) - with the last request the value the
    // hand ended on: 10 + 50 * 170/270 = 41.5 dB. A dial that asked every frame would make
    // thirty-four calls.
    //
    // "FEWER" IS A FACT ABOUT THE CLOCK, NOT ABOUT FRAMES (CI run 38288445561, 2026-10-12). The
    // pace is 50 ms of the application's own time, so what a 34-frame gesture costs depends on how
    // long a frame is. This desktop draws one in about 8 to 15 ms: the gesture lasts under half a
    // second and the radio is asked 6 times. Both GitHub Linux runners (x64 and arm64) asked it 34
    // times, the dial doing exactly what it is meant to - 34 calls in 34 frames means every frame
    // was a fresh 50 ms slot, frames of 50 ms or more - so a constant (12) failed there. The run
    // therefore keeps its own frame times (FOXSDR_SCRIPT_TRACE: ImGui's clock, the one the dial's
    // pace is read against) and the count is judged against them:
    //   - never more than one call per 50 ms slot of the time the hand was on the dial, plus the
    //     release's own call and a slot of slack: ceil(duration / 50 ms) + 2;
    //   - and, where the run shows the pace could bite, fewer than one call a frame. A call a
    //     frame needs EVERY gap between the frames that move the dial to be a full slot (the last
    //     frame's call may be the release's, which is not paced), so one shorter gap, with the
    //     count still under the frame count, is the pace at work. Where every gap is a full slot
    //     or more nothing can be proved either way (a dial that asked every frame looks the
    //     same), and the run says so instead of passing silently.
    constexpr int kAt = 30;       // the script frame the gesture starts at
    constexpr int kSteps = 34;    // frames that move the dial
    constexpr double kSlotMs = 50.0;  // kDeckGainPaceS
    constexpr long kLanded = kAt + 1;               // button down on the dial
    constexpr long kFirstMove = kAt + 2;            // the first frame that turns it
    constexpr long kLastMove = kAt + 1 + kSteps;    // the last
    constexpr long kReleased = kAt + 2 + kSteps;    // button up: the final value goes out
    Extra traced;
    traced.trace = true;
    const Result slow = once("gain-slow", "agc", settle() + slowTwist(knob, kAt, kSteps, 5.0f),
                             "1600x1000", "", kFrames, -60.0, false, traced);
    CHECK(slow.ok);
    const std::vector<double> sg = gainCalls(slow);
    std::printf("    a thirty-four-frame twist asked the radio %zu time(s), the last for %.1f\n",
                sg.size(), sg.empty() ? 0.0 : sg.back());
    CHECK(sg.size() >= 1);
    {
        const Timing tm = timingOf(slow, kLanded, kReleased, kFirstMove + 1, kLastMove - 1);
        if (!tm.ok) { std::printf("    the run's frame times do not cover the gesture\n"); }
        CHECK(tm.ok);
        if (tm.ok) {
            const double allowed = std::ceil(tm.spanMs / kSlotMs) + 2.0;
            std::printf("    the hand was on the dial for %.0f ms over %ld frames (%.1f ms a frame, "
                        "the shortest gap %.1f ms): a paced dial is asked at most %.0f times\n",
                        tm.spanMs, kReleased - kLanded, tm.frameMs, tm.shortestGapMs, allowed);
            CHECK(static_cast<double>(sg.size()) <= allowed);
            if (tm.shortestGapMs < kSlotMs - 1.0) {
                CHECK(sg.size() < static_cast<std::size_t>(kSteps));
            } else {
                std::printf("    frames here are as long as the pace (%.0f ms or more a gap): this run "
                            "cannot tell a paced dial from one asked every frame, so only the count "
                            "against the run's own clock is judged\n",
                            kSlotMs);
            }
        }
    }
    if (!sg.empty()) {
        CHECK_NEAR(sg.back(), 10.0 + 50.0 * 170.0 / 270.0, 4.0);
        for (std::size_t i = 1; i < sg.size(); ++i) { CHECK(sg[i] >= sg[i - 1] - 1.0e-6); }
    }
}

void squelchKnobAndKey(const Rect& knob, const Rect& key) {
    std::printf("  the SQUELCH knob and its AUTO key\n");
    // From -100 dB: a half turn clockwise (180 of 270 degrees) is two thirds of the 120 dB
    // travel, -100 + 80 = -20.
    const Result t = once("sq-twist", "agc", settle() + twist(knob, 30, 2), "1600x1000",
                          ", \"squelchDb\": -100");
    CHECK(t.ok);
    std::printf("    squelchDb after a half turn from -100: %.1f\n", static_cast<double>(t.saved.squelchDb));
    CHECK_NEAR(t.saved.squelchDb, -20.0, 3.0);
    CHECK(!t.saved.squelchAuto);
    // AUTO on by a press, saved.
    const Result a = once("sq-auto", "agc", settle() + press(key, 30), "1600x1000",
                          ", \"squelchDb\": -100");
    CHECK(a.ok);
    CHECK(a.saved.squelchAuto);
    // ...and off again by a second press.
    const Result b = once("sq-auto2", "agc", settle() + press(key, 30) + press(key, 50), "1600x1000",
                          ", \"squelchDb\": -100");
    CHECK(b.ok);
    CHECK(!b.saved.squelchAuto);
    // A HAND ON THE KNOB IS A MANUAL CHOICE: AUTO on, then a twist: AUTO off, threshold moved.
    const Result h = once("sq-auto-twist", "agc",
                          settle() + press(key, 30) + twist(knob, 50, 2), "1600x1000",
                          ", \"squelchDb\": -100");
    CHECK(h.ok);
    CHECK(!h.saved.squelchAuto);
    CHECK_NEAR(h.saved.squelchDb, -20.0, 3.0);
    // And a config that STARTS with AUTO on brings it back on.
    const Result s = once("sq-auto-saved", "agc", "", "1600x1000",
                          ", \"squelchDb\": -100, \"squelchAuto\": true");
    CHECK(s.ok);
    CHECK(s.saved.squelchAuto);
}

// AUTOMATIC FOLLOWS THE MEASURED NOISE. The fake radio's white noise at two levels, 20 dB
// apart; the receiver runs (the decoder-test hook starts it); the saved threshold, which
// starts at -100 in both, lands about 20 dB apart and well above -100.
void squelchFollowsNoise() {
    std::printf("  AUTOMATIC squelch follows the measured noise floor\n");
    std::string waits;
    for (int f = 10; f < 190; ++f) { waits += std::to_string(f) + " sleep 16\n"; }
    const std::string cfg = ", \"squelchDb\": -100, \"squelchAuto\": true";
    const Result lo = once("noise-lo", "agc", waits, "1600x1000", cfg, 200, -50.0, true);
    const Result hi = once("noise-hi", "agc", waits, "1600x1000", cfg, 200, -30.0, true);
    CHECK(lo.ok && hi.ok);
    std::printf("    threshold with the noise at -50 dB: %.1f, at -30 dB: %.1f\n",
                static_cast<double>(lo.saved.squelchDb), static_cast<double>(hi.saved.squelchDb));
    CHECK(lo.saved.squelchAuto && hi.saved.squelchAuto);
    CHECK(lo.saved.squelchDb > -100.0f + 5.0f);
    CHECK_NEAR(static_cast<double>(hi.saved.squelchDb) - static_cast<double>(lo.saved.squelchDb),
               20.0, 4.0);
}

// A CARRIER NEVER TEACHES THE FLOOR (the owner's follow-up): the radio's noise is -50 dB for the
// first four seconds, so AUTOMATIC learns the floor, and then a louder stretch (-20 dB, 30 dB up:
// over the threshold, so the gate opens) holds the channel for the rest of the run - longer than
// the ten-second window by the time the run ends. The threshold must still be where the quiet
// noise put it (-53.5: 8 dB over the 12 dB-reduced channel level), not dragged up to the carrier.
// The run ends inside the carrier on purpose: when it stops the quiet blocks come back and a
// floor that HAD been taught would quietly come back down with them.
void carrierDoesNotTeach() {
    std::printf("  a carrier that holds the gate open does not teach AUTOMATIC the floor\n");
    std::string waits;
    for (int f = 10; f < 170; ++f) { waits += std::to_string(f) + " sleep 100\n"; }
    Extra ex;
    ex.burst = "4,60,-20";
    const Result r = once("carrier", "agc", waits, "1600x1000",
                          ", \"squelchDb\": -100, \"squelchAuto\": true", 175, -50.0, true, ex);
    CHECK(r.ok);
    std::printf("    threshold after a carrier held for ~13 s: %.1f dB (quiet noise gave -53.5)\n",
                r.st("squelch"));
    CHECK(r.st("auto") == 1.0);
    CHECK_NEAR(r.st("squelch"), -53.5, 4.0);
    CHECK(r.st("squelch") < -45.0);
}

// --- THE SQUELCH DIAL AND AUTO KEY WHILE THE AIRBAND MONITOR LISTENS ---------------------------------
//
// The tester: "I really like the Airband tool and find myself adjusting the Squelch often when
// doing so." The monitor has its own squelch (airbandSquelchDb_, -110..0, default -55) and its
// own "Above the noise" rule, and while it listens the receiver's VFO is not demodulated, so a
// dial that set the receiver's threshold would change nothing the tester hears. Now it acts on
// the monitor's, the receiver's stays where it was, and AUTO is the monitor's rule (the median
// channel level plus 8 dB) re-applied while it is lit.

// The scripted scroll that brings the Airband section into view in the TOOLS bank (the Airband
// test's own, kScroll there).
const char* const kScroll = "30 screen 200 600\n32 wheel -30\n";

// `n` sleeps of `ms`, three frames apart from `first`: real time held inside a bounded run.
std::string holds(int first, int n, int ms) {
    std::string s;
    for (int i = 0; i < n; ++i) {
        s += std::to_string(first + 3 * i) + " sleep " + std::to_string(ms) + "\n";
    }
    return s;
}

// The radio adopted before the scroll and the clicks.
std::string settleLong() { return "3 sleep 150\n20 sleep 300\n"; }

// A run in the Airband layout at 1600 x 1000.
Result airbandRun(const std::string& tag, const std::string& script, int frames,
                  const std::string& extraConfig, double noiseDb = -50.0) {
    Extra ex;
    ex.airband = true;
    return once(tag, "agc", settleLong() + kScroll + script, "1600x1000", extraConfig, frames,
                noiseDb, false, ex);
}

void airbandRouting() {
    std::printf("  the SQUELCH dial and its AUTO key while the Airband monitor listens\n");
    // LAYOUT: the section in view; LISTEN's place idle, and the controls' places while it
    // listens (the status lines come in above the row list and move them).
    const Result idle = airbandRun("ab-idle", "", 60, "");
    CHECK(idle.ok);
    CHECK(has(idle, "airband:listen"));
    CHECK(has(idle, "deck:squelch") && has(idle, "deck:squelchauto"));
    if (!idle.ok || !has(idle, "airband:listen") || !has(idle, "deck:squelch") ||
        !has(idle, "deck:squelchauto")) {
        return;
    }
    const Rect listen = idle.rects.at("airband:listen");
    const Rect dial = idle.rects.at("deck:squelch");
    const Rect key = idle.rects.at("deck:squelchauto");
    const std::string listenNow = press(listen, 40);

    // (1) A TWIST sets the MONITOR's squelch and leaves the receiver's alone. -55 dB is half
    // way along -110..0; a quarter turn (90 of 270 degrees) is a third of the span: -18.3.
    const Result t = airbandRun(
        "ab-twist", listenNow + holds(50, 12, 100) + twist(dial, 90, 1) + holds(110, 8, 100), 140,
        ", \"squelchDb\": -100");
    CHECK(t.ok);
    CHECK(t.items.count("airband:listening") == 1);
    std::printf("    a quarter turn while listening: Airband squelch %.1f, receiver squelch %.1f, "
                "AUTO %.0f\n",
                t.st("airband"), t.st("squelch"), t.st("auto"));
    CHECK_NEAR(t.st("airband"), -110.0 + 110.0 * (0.5 + 90.0 / 270.0), 3.0);
    CHECK_NEAR(t.st("squelch"), -100.0, 0.01);  // the receiver's threshold was not touched
    CHECK(t.st("auto") == 0.0);
    CHECK(t.saved.squelchDb == -100.0f);

    // (2) AUTO WHILE LISTENING IS THE MONITOR'S "ABOVE THE NOISE": white noise at two levels 20
    // dB apart puts the Airband squelch about 20 dB apart, away from its -55 default, and the
    // receiver's threshold is not moved by it (it is not listening to anything).
    const std::string autoCfg = ", \"squelchDb\": -100, \"squelchAuto\": true";
    const Result lo =
        airbandRun("ab-auto-lo", listenNow + holds(50, 30, 100), 150, autoCfg, -50.0);
    const Result hi =
        airbandRun("ab-auto-hi", listenNow + holds(50, 30, 100), 150, autoCfg, -30.0);
    CHECK(lo.ok && hi.ok);
    std::printf("    AUTO with the noise at -50 dB: Airband squelch %.1f, at -30 dB: %.1f "
                "(receiver %.1f)\n",
                lo.st("airband"), hi.st("airband"), lo.st("squelch"));
    CHECK(lo.items.count("airband:listening") == 1);
    CHECK(lo.st("auto") == 1.0 && hi.st("auto") == 1.0);
    CHECK(std::fabs(lo.st("airband") - (-55.0)) > 3.0);   // not the default: it was set
    CHECK(lo.st("airband") > -110.0 && lo.st("airband") < 0.0);
    CHECK_NEAR(hi.st("airband") - lo.st("airband"), 20.0, 4.0);
    CHECK_NEAR(lo.st("squelch"), -100.0, 0.01);
    CHECK_NEAR(hi.st("squelch"), -100.0, 0.01);
    const double aboveNoise = lo.st("airband");

    // (3) A HAND ON THE DIAL TURNS AUTO OFF, and the threshold stays where the hand put it
    // (a third of the span, 36.7 dB, from where AUTO had it).
    const Result h = airbandRun(
        "ab-auto-twist", listenNow + holds(50, 24, 100) + twist(dial, 125, 1) + holds(145, 12, 100),
        190, autoCfg, -50.0);
    CHECK(h.ok);
    std::printf("    AUTO then a quarter turn: Airband squelch %.1f, AUTO %.0f\n", h.st("airband"),
                h.st("auto"));
    CHECK(h.st("auto") == 0.0);
    CHECK_NEAR(h.st("airband"), std::min(0.0, aboveNoise + 110.0 / 3.0), 6.0);

    // THE SECTION'S OWN CONTROLS, where they stand while it listens (the status lines come in
    // above the row list and move them), from a run that listens to the end.
    const Result lis = airbandRun("ab-layout", listenNow + holds(50, 10, 100), 100, "");
    CHECK(lis.ok);
    CHECK(lis.items.count("airband:listening") == 1);
    CHECK(has(lis, "airband:squelch"));
    CHECK(has(lis, "airband:abovenoise"));
    CHECK(has(lis, "airband:stop"));
    if (!lis.ok || !has(lis, "airband:squelch") || !has(lis, "airband:abovenoise") ||
        !has(lis, "airband:stop")) {
        return;
    }
    const Rect slider = lis.rects.at("airband:squelch");
    const Rect above = lis.rects.at("airband:abovenoise");
    const std::string stopNow = press(lis.rects.at("airband:stop"), 120);

    // (4) THE AIRBAND SECTION'S OWN SQUELCH SLIDER turns AUTO off: a click on its middle sets
    // -55, ten dB or more from where AUTO had it.
    const Result s = airbandRun(
        "ab-auto-slider", listenNow + holds(50, 24, 100) + press(slider, 125) + holds(135, 12, 100),
        180, autoCfg, -50.0);
    CHECK(s.ok);
    std::printf("    AUTO then a click on the Airband slider: squelch %.1f, AUTO %.0f\n",
                s.st("airband"), s.st("auto"));
    CHECK(s.st("auto") == 0.0);
    CHECK_NEAR(s.st("airband"), -55.0, 6.0);

    // (5) ...and so does its "Above the noise" key (the same rule, pressed once).
    const Result a = airbandRun(
        "ab-auto-above", listenNow + holds(50, 24, 100) + press(above, 125) + holds(135, 12, 100),
        180, autoCfg, -50.0);
    CHECK(a.ok);
    std::printf("    AUTO then Above the noise: squelch %.1f, AUTO %.0f\n", a.st("airband"),
                a.st("auto"));
    CHECK(a.st("auto") == 0.0);
    CHECK_NEAR(a.st("airband"), aboveNoise, 4.0);

    // (6) STOPPED, THE DIAL IS THE RECEIVER'S AGAIN: a half turn from -100 saves -20 and the
    // monitor's own squelch is where it was (-55, the default).
    const Result st = airbandRun(
        "ab-stopped", listenNow + holds(50, 12, 100) + stopNow + holds(130, 4, 100) +
                          twist(dial, 150, 2) + holds(170, 6, 100),
        200, ", \"squelchDb\": -100");
    CHECK(st.ok);
    std::printf("    stopped, then a half turn: receiver squelch %.1f, Airband squelch %.1f\n",
                st.st("squelch"), st.st("airband"));
    CHECK_NEAR(st.st("squelch"), -20.0, 3.0);
    CHECK_NEAR(st.st("airband"), -55.0, 0.01);
    CHECK(st.st("auto") == 0.0);
    (void)key;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && std::strcmp(argv[1], "--child") == 0) {
        return childMain(argv[2], std::atoi(argv[3]));
    }
    // UNBUFFERED, so that a run stopped by the timeout still says how far it got.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_deck_knobs_app\n");
    g_self = selfPath(argv[0]);
    g_dir = fs::temp_directory_path() / ("cascade-deck-knobs-" + std::to_string(pid()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir / "appdata", ec);
    fs::create_directories(g_dir / "local", ec);
    fs::create_directories(g_dir / "diag", ec);
    setEnv("APPDATA", (g_dir / "appdata").string());
    setEnv("LOCALAPPDATA", (g_dir / "local").string());
    setEnv("XDG_CONFIG_HOME", (g_dir / "appdata").string());
    setEnv("XDG_DATA_HOME", (g_dir / "local").string());
    setEnv("XDG_STATE_HOME", (g_dir / "local").string());
    setEnv("FOXSDR_DIAG_DIR", (g_dir / "diag").string());
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL",
                            "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }
    setEnv("FOXSDR_SINGLE_VIEWPORT", "1");
    setEnv("FOXSDR_PATCH_START", "");

    partsAt("1600x1000", true);
    partsAt("1280x720", true);

    // The gestures are aimed from the census of a plain run at 1600 x 1000.
    const Result base = once("base", "agc", "", "1600x1000", "");
    CHECK(base.ok);
    if (base.ok && has(base, "deck:gain") && has(base, "deck:gainauto") && has(base, "deck:squelch") &&
        has(base, "deck:squelchauto")) {
        gainAuto(base.rects.at("deck:gainauto"), base.rects.at("deck:gain"));
        gainNoAuto(base.rects.at("deck:gainauto"));
        gainNoRadio(base.rects.at("deck:gain"), base.rects.at("deck:gainauto"));
        gainKnob(base.rects.at("deck:gain"));
        squelchKnobAndKey(base.rects.at("deck:squelch"), base.rects.at("deck:squelchauto"));
    } else {
        std::printf("  the gain and squelch parts are not on the deck: no gesture can be aimed\n");
        CHECK(has(base, "deck:gain"));
    }
    squelchFollowsNoise();
    carrierDoesNotTeach();
    airbandRouting();

    const int rc = testSummary("test_deck_knobs_app");
    if (rc == 0) { fs::remove_all(g_dir, ec); }
    return rc;
}
