// THE AUDIO BUFFER THAT DEEPENS ITSELF, THROUGH THE REAL AppWindow (0.99.73).
//
// tests/test_audio_lead.cpp holds the policy as a table (sink::nextAudioLead),
// tests/test_audio_out.cpp the lead as the sink's callback honours it and
// tests/test_drift_matcher.cpp the matcher that steers to it. What only the
// application can show is the WIRING of the once-a-minute poll that closes a
// minute: that a minute of starved callbacks reaches the sink's lead, the log,
// the AUDIO card and Sinks rail sentence, the config and the health count - and
// that a minute that should change nothing changes nothing.
//
// THE FIELD REPORT (12CF, a Store user on 0.99.64, RSP1A at 2.048 MS/s, thirty
// plugins): 110 to 126 starved audio callbacks every minute at a fixed 120 ms
// lead, and a bigger buffer was the one thing that could help.
//
// HOW THE CONDITION IS MADE. The starved callbacks are REAL ones: the window's
// own sink, its stream closed so no device thread races the test for the ring,
// is primed to its lead and then asked for more than it holds, which is exactly
// what a callback that arrives before the producer does - one starved callback
// per round, counted by AudioOut::underruns() as always. Nothing is faked in the
// sink; what the test controls is only how many rounds a "minute" holds, by
// closing the minute itself (AppWindow::closeAudioMinute takes no clock) rather
// than waiting sixty seconds. That the frame loop's poll calls it, once a
// minute, is read off the source.
//
// Hermetic like test_diag_audio_app: no config file, the per-user directories in
// a scratch folder, telemetry pointed at a black hole.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/config.hpp"
#include "core/diag_log.hpp"
#include "core/health_events.hpp"
#include "core/patch_audio.hpp"
#include "core/telemetry.hpp"
#include "gui/app_window.hpp"
#include "sink/audio_out.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
namespace health = cascade::core::health;
using health::Counts;
using health::HealthLedger;

namespace {

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

fs::path g_scratch;

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() / ("foxsdr_audio_lead_app_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

std::string u8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::uint32_t countOf(const Counts& c, const std::string& token) {
    const auto it = c.find(token);
    return it == c.end() ? 0u : it->second;
}

// `text` without its comments: `//` to the end of the line and `/* ... */`. Good enough
// for the needles this test looks for, none of which sits after a string holding a `//`.
std::string stripComments(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text.compare(i, 2, "//") == 0) {
            while (i < text.size() && text[i] != '\n') { ++i; }
        } else if (text.compare(i, 2, "/*") == 0) {
            const std::size_t close = text.find("*/", i + 2);
            i = close == std::string::npos ? text.size() : close + 2;
            out += ' ';
        } else {
            out += text[i++];
        }
    }
    return out;
}

// How many lines of the log contain `text`.
int logLinesWith(const std::string& text) {
    int n = 0;
    for (const std::string& line : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (line.find(text) != std::string::npos) { ++n; }
    }
    return n;
}

// A speaker on the patch page that is a sound device: its own buffer, its own
// count of starved callbacks, set by hand.
class FakeSpeaker final : public cascade::core::patch::AudioDest {
public:
    void write(const float*, std::size_t) override {}
    std::string describe() const override { return "fake speaker"; }
    std::string error() const override { return std::string(); }
    std::uint64_t starvedCallbacks() const override { return starved; }
    int leadMs() const override { return lead; }
    void setLeadMs(int ms) override { lead = ms; }
    std::uint64_t starved = 0;
    int lead = 120;
};

// A file: the defaults, no buffer of its own.
class FakeFile final : public cascade::core::patch::AudioDest {
public:
    void write(const float*, std::size_t) override {}
    std::string describe() const override { return "fake file"; }
    std::string error() const override { return std::string(); }
};

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    static cascade::sink::AudioOut& audio(AppWindow& a) { return a.pipeline_.audio(); }
    static void closeMinute(AppWindow& a) { a.closeAudioMinute(a.pipeline_.audio().ringFrames()); }
    static void setBuffer(AppWindow& a, int ms) { a.setAudioBufferMs(ms); }
    static void startReceiver(AppWindow& a) { a.pipeline_.start(); }
    static void tune(AppWindow& a, double hz) { a.tuneAbsoluteHz(hz); }
    static double tunedHz(AppWindow& a) { return a.currentAbsoluteHz(); }
    static cascade::core::PluginUse use(AppWindow& a, const cascade::core::LoadedPlugin& p,
                                         const std::string& playingKey = {}) {
        return a.pluginUseOf(p, playingKey, 0);
    }
    static void lifecycle(AppWindow& a) { a.updatePluginLifecycle(); }
    static int leadNow(AppWindow& a) { return a.audioLeadNowMs(); }
    static std::string notice(AppWindow& a) { return a.audioLeadNotice(); }
    // The status the browser is served (/api/status): built by the window's own snapshot.
    static cascade::net::RadioStatus status(AppWindow& a) {
        a.publishWebSnapshot();
        return a.webStatus_;
    }
    static cascade::core::AppConfig config(AppWindow& a) { return a.currentConfig(); }
    static void restore(AppWindow& a, const cascade::core::AppConfig& c) { a.applyConfig(c); }
    static void addSpeaker(AppWindow& a, cascade::core::patch::NodeId id,
                           std::shared_ptr<cascade::core::patch::AudioDest> d) {
        a.patchDests_.emplace_back(id, std::move(d));
    }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

// ONE STARVED CALLBACK, the way the device makes it: prime the sink to its
// lead, drain exactly that, and ask for more than is left. Needs the stream
// closed (see the file header) so that nothing else reads the ring.
void starveOnce(cascade::sink::AudioOut& out) {
    const std::size_t chan = static_cast<std::size_t>(out.channels() == 2 ? 2 : 1);
    const std::size_t lead = out.leadFrames();
    std::vector<float> pad(lead * chan, 0.0f);
    CHECK(out.write(pad.data(), pad.size()) == pad.size());
    std::vector<float> dst(lead * chan);
    CHECK(cascade::sink::AudioOut::pullBlock(&out, dst.data(), lead) == pad.size());   // primed, empty
    const std::uint64_t before = out.underruns();
    std::vector<float> more(480 * chan);
    cascade::sink::AudioOut::pullBlock(&out, more.data(), 480);                        // nothing there
    CHECK(out.underruns() == before + 1);
    CHECK(!out.primed());
}

void starve(cascade::sink::AudioOut& out, int times) {
    for (int i = 0; i < times; ++i) { starveOnce(out); }
}

const char* kRaisedTo240By3 =
    "Audio buffer raised to 240 ms: this computer fell behind 3 times in the last minute.";

}  // namespace

int main() {
    isolate();

    // A manual tune must give an AUTO audio-output decoder an independent wake
    // signal before it has an instance to report as the playing plugin.
    {
        cascade::gui::AppWindow app;
        cascade::core::LoadedPlugin p;
        p.path = "audio-probe.dll";
        CascadeAudioOutApi audioApi{};
        p.audioOut = &audioApi;
        CHECK(!Access::use(app, p).playingAudio);
        Access::startReceiver(app);
        Access::tune(app, Access::tunedHz(app) + 1000.0);
        CHECK(Access::use(app, p).playingAudio);
        Access::lifecycle(app);  // consume the one-shot tune signal
        CHECK(!Access::use(app, p).playingAudio);
        CHECK(Access::use(app, p, cascade::core::pluginKey(p)).playingAudio);
    }

    // The ledger the window counts into, armed with a file of its own, so that
    // "the next usage record" can be read the way the next start-up reads it.
    const std::string installId = cascade::core::newInstallId();
    const fs::path ledgerDir = g_scratch / "ledger";
    std::error_code ec;
    fs::create_directories(ledgerDir, ec);
    const std::string ledgerFile = HealthLedger::pathIn(u8(ledgerDir));
    auto ledger = health::globalLedger();
    ledger->reset();
    ledger->arm(ledgerFile, installId, false);

    {
        cascade::gui::AppWindow app;
        cascade::sink::AudioOut& out = Access::audio(app);
        out.close();   // no device thread may read the ring while the test makes callbacks

        // --- 1. A NEW WINDOW: 120 ms, AUTOMATIC, nothing to say --------------------
        CHECK(Access::leadNow(app) == 120);
        CHECK(out.leadFrames() == 5760u);
        CHECK(out.targetFrames() == 7680u);
        CHECK(Access::notice(app).empty());
        CHECK(Access::config(app).audioBufferMs == 0);
        CHECK(Access::status(app).audioLeadMs == 120);        // /api/status says so too

        // --- 2. TWO STARVED CALLBACKS IN A MINUTE: nothing happens -------------------
        starve(out, 2);
        Access::closeMinute(app);
        CHECK(Access::leadNow(app) == 120);
        CHECK(Access::notice(app).empty());
        CHECK(logLinesWith("audio: buffer raised") == 0);
        CHECK(countOf(ledger->counts(), "recovered.audiolead") == 0);
        // (the digest line that was always written is still written)
        CHECK(logLinesWith("audio: 2 starved callbacks in the last minute") == 1);

        // --- 3. THREE: THE BUFFER DEEPENS TO 240 ms ----------------------------------
        starve(out, 3);
        Access::closeMinute(app);
        CHECK(Access::leadNow(app) == 240);
        CHECK(out.leadFrames() == 11520u);
        CHECK(out.targetFrames() == 11520u + 1920u);          // the matcher steers to 280 ms
        // THE SENTENCE the AUDIO card and the Sinks rail print, from one place.
        std::printf("notice: \"%s\"\n", Access::notice(app).c_str());
        CHECK(Access::notice(app) == kRaisedTo240By3);
        // THE LOG LINE, once.
        CHECK(logLinesWith("audio: buffer raised to 240 ms after 3 starved callbacks in a minute") == 1);
        // THE HEALTH COUNT, once a session, in the ledger.
        CHECK(countOf(ledger->counts(), "recovered.audiolead") == 1);
        // THE CONFIG: AUTOMATIC stays AUTOMATIC. A raise is the session's, not a choice
        // the person made - saving 240 would turn a machine that fell behind once into a
        // machine that is told it asked for 240 ms for good.
        CHECK(Access::config(app).audioBufferMs == 0);
        // ...BUT THE DEPTH IT REACHED IS REMEMBERED, under a key of its own, for the next launch (a
        // computer that fell behind this often is slow every time).
        CHECK(Access::config(app).audioBufferAutoMs == 240);
        CHECK(Access::status(app).audioLeadMs == 240);        // the browser's number moved with it

        // --- 4. THE NEXT BAD MINUTE: 480 ms; the sentence is the latest ---------------
        starve(out, 5);
        Access::closeMinute(app);
        CHECK(Access::leadNow(app) == 480);
        CHECK(Access::notice(app) ==
              "Audio buffer raised to 480 ms: this computer fell behind 5 times in the last minute.");
        CHECK(logLinesWith("audio: buffer raised to 480 ms after 5 starved callbacks in a minute") == 1);
        CHECK(countOf(ledger->counts(), "recovered.audiolead") == 1);   // once a session
        CHECK(Access::config(app).audioBufferAutoMs == 480);            // the latest depth is the one kept

        // --- 5. A QUIET MINUTE DOES NOTHING: never down ---------------------------------
        Access::closeMinute(app);
        Access::closeMinute(app);
        CHECK(Access::leadNow(app) == 480);
        CHECK(Access::notice(app) ==
              "Audio buffer raised to 480 ms: this computer fell behind 5 times in the last minute.");

        // --- 6. THE CEILING --------------------------------------------------------------
        starve(out, 4);
        Access::closeMinute(app);
        CHECK(Access::leadNow(app) == 960);
        CHECK(out.leadFrames() == 46080u);
        CHECK(logLinesWith("audio: buffer raised to 960 ms after 4 starved callbacks in a minute") == 1);
        starve(out, 50);
        Access::closeMinute(app);
        CHECK(Access::leadNow(app) == 960);                       // never above 960 ms
        CHECK(logLinesWith("audio: buffer raised to") == 3);      // and no fourth line
        CHECK(Access::config(app).audioBufferMs == 0);
        CHECK(Access::config(app).audioBufferAutoMs == 960);      // the ceiling is what is kept

        // --- 7. THE PERSON SETS IT: a FIXED buffer is the lead at once, never steps -----------
        Access::setBuffer(app, 240);
        CHECK(Access::leadNow(app) == 240);
        CHECK(out.leadFrames() == 11520u);
        CHECK(Access::config(app).audioBufferMs == 240);          // the config value
        // A FIXED VALUE LEAVES THE REMEMBERED DEPTH ALONE (it is kept for the file and ignored while
        // the fixed value is in force) ...
        CHECK(Access::config(app).audioBufferAutoMs == 960);
        CHECK(Access::notice(app).empty());                       // the notice was about automatic mode
        const int linesBefore = logLinesWith("audio: buffer raised to");
        starve(out, 30);
        Access::closeMinute(app);
        CHECK(Access::leadNow(app) == 240);                       // a fixed value never steps
        CHECK(Access::notice(app).empty());
        CHECK(logLinesWith("audio: buffer raised to") == linesBefore);
        // 120 is a fixed setting too, and does not climb.
        Access::setBuffer(app, 120);
        CHECK(Access::leadNow(app) == 120);
        CHECK(Access::config(app).audioBufferMs == 120);
        starve(out, 30);
        Access::closeMinute(app);
        CHECK(Access::leadNow(app) == 120);

        // --- 8. BACK TO AUTOMATIC starts again from 120 ms and may deepen again -----------------
        Access::setBuffer(app, 960);
        CHECK(Access::leadNow(app) == 960);
        Access::setBuffer(app, 0);
        CHECK(Access::leadNow(app) == 120);
        CHECK(Access::config(app).audioBufferMs == 0);
        // ...AND CHOOSING AUTOMATIC BY HAND FORGETS THE REMEMBERED DEPTH: the person asked for a fresh
        // start, and a depth an earlier raise had left in the file must not outlive that.
        CHECK(Access::config(app).audioBufferAutoMs == 0);
        CHECK(Access::notice(app).empty());
        starve(out, 3);
        Access::closeMinute(app);
        CHECK(Access::leadNow(app) == 240);
        CHECK(Access::notice(app) == kRaisedTo240By3);
        CHECK(Access::config(app).audioBufferAutoMs == 240);      // and it remembers again from there

        // --- 9. THE PATCH PAGE'S SPEAKERS get the same, each on its own count ----------------------
        {
            auto speaker = std::make_shared<FakeSpeaker>();
            auto file = std::make_shared<FakeFile>();
            Access::setBuffer(app, 0);                            // automatic, 120 ms
            Access::addSpeaker(app, 7, speaker);
            Access::addSpeaker(app, 8, file);
            Access::closeMinute(app);                             // the baseline minute: nothing starved
            CHECK(speaker->lead == 120);
            speaker->starved += 2;                                // two in the next minute: nothing
            Access::closeMinute(app);
            CHECK(speaker->lead == 120);
            speaker->starved += 4;                                // four: one rung
            Access::closeMinute(app);
            CHECK(speaker->lead == 240);
            CHECK(logLinesWith("audio: buffer raised to 240 ms after 4 starved callbacks in a minute "
                               "(patch speaker)") == 1);
            Access::closeMinute(app);                             // quiet: stays
            CHECK(speaker->lead == 240);
            // A fixed setting is the speaker's too, and a file has no buffer to move.
            Access::setBuffer(app, 480);
            CHECK(speaker->lead == 480);
            speaker->starved += 50;
            Access::closeMinute(app);
            CHECK(speaker->lead == 480);
            Access::setBuffer(app, 0);
            CHECK(speaker->lead == 120);
            CHECK(file->leadMs() == 0);
            CHECK(file->starvedCallbacks() == 0u);
        }
    }

    // --- 10. THE SAVED SETTING comes back: a fixed one is applied at start-up -----------------------
    // (A second window, after the first is gone: one at a time, as the application has it.)
    {
        cascade::core::AppConfig cfg;
        cfg.audioBufferMs = 480;
        cascade::gui::AppWindow second;
        Access::audio(second).close();
        Access::restore(second, cfg);
        CHECK(Access::leadNow(second) == 480);
        CHECK(Access::config(second).audioBufferMs == 480);
        // A file that names AUTOMATIC puts the 120 ms start back, once...
        cascade::core::AppConfig automatic;
        Access::restore(second, automatic);                       // 480 -> automatic: back to 120
        CHECK(Access::leadNow(second) == 120);
        starve(Access::audio(second), 3);
        Access::closeMinute(second);
        CHECK(Access::leadNow(second) == 240);
        // ...and a restore that changes nothing changes nothing: automatic -> automatic
        // does not undo what the session had to do.
        Access::restore(second, automatic);
        CHECK(Access::leadNow(second) == 240);
    }

    // --- 10b. THE DEPTH AUTOMATIC REACHED COMES BACK (audioBufferAutoMs, 0.99.73 follow-up) ---------
    //
    // A computer that fell behind is slow at every launch; an AUTOMATIC session starts at the depth the
    // last one had to reach instead of paying the same bad minutes again. (One window at a time.)
    {
        cascade::core::AppConfig remembered;
        remembered.audioBufferAutoMs = 480;
        cascade::gui::AppWindow third;
        Access::audio(third).close();
        CHECK(Access::leadNow(third) == 120);                      // before the file is applied: the default
        const int raisedLinesBefore = logLinesWith("audio: buffer raised");
        Access::restore(third, remembered);
        // AUTOMATIC STARTS AT THE REMEMBERED DEPTH: the lead, and the matcher's target with it.
        CHECK(Access::leadNow(third) == 480);
        CHECK(Access::audio(third).leadFrames() == 23040u);
        CHECK(Access::audio(third).targetFrames() == 23040u + 1920u);
        CHECK(Access::config(third).audioBufferMs == 0);           // still AUTOMATIC: nothing was chosen
        CHECK(Access::config(third).audioBufferAutoMs == 480);     // and the memory is carried forward
        // A restored depth is not a raise: no sentence on the card, no log line, no health count.
        CHECK(Access::notice(third).empty());
        CHECK(logLinesWith("audio: buffer raised") == raisedLinesBefore);
        CHECK(Access::status(third).audioLeadMs == 480);
        // From there it can still deepen, and what it keeps is the new depth.
        starve(Access::audio(third), 3);
        Access::closeMinute(third);
        CHECK(Access::leadNow(third) == 960);
        CHECK(Access::config(third).audioBufferAutoMs == 960);
        // A restore that names a SHALLOWER depth than the session already has leaves the session alone.
        Access::restore(third, remembered);
        CHECK(Access::leadNow(third) == 960);
        CHECK(Access::config(third).audioBufferAutoMs == 960);
    }
    {
        // A FIXED SETTING IGNORES IT, and keeps it for the file.
        cascade::core::AppConfig fixedAndRemembered;
        fixedAndRemembered.audioBufferMs = 240;
        fixedAndRemembered.audioBufferAutoMs = 960;
        cascade::gui::AppWindow fixed;
        Access::audio(fixed).close();
        Access::restore(fixed, fixedAndRemembered);
        CHECK(Access::leadNow(fixed) == 240);
        CHECK(Access::config(fixed).audioBufferMs == 240);
        CHECK(Access::config(fixed).audioBufferAutoMs == 960);
        starve(Access::audio(fixed), 30);
        Access::closeMinute(fixed);
        CHECK(Access::leadNow(fixed) == 240);                      // a fixed value never steps
        CHECK(Access::config(fixed).audioBufferAutoMs == 960);     // and the memory is not overwritten
    }
    {
        // NEVER LOWER THAN 120, NEVER ABOVE 960: a value the file should not hold is not applied.
        // (The loader repairs these to 0 before they get here; the window does not rely on it.)
        for (const int bad : {1, 100, 300, 5000, -480}) {
            cascade::core::AppConfig junk;
            junk.audioBufferAutoMs = bad;
            cascade::gui::AppWindow w;
            Access::audio(w).close();
            Access::restore(w, junk);
            CHECK(Access::leadNow(w) == 120);
            CHECK(Access::config(w).audioBufferAutoMs == 0);
        }
        cascade::core::AppConfig floorOnly;
        floorOnly.audioBufferAutoMs = 120;
        cascade::gui::AppWindow w2;
        Access::audio(w2).close();
        Access::restore(w2, floorOnly);
        CHECK(Access::leadNow(w2) == 120);                         // 120 is the start already
        CHECK(Access::config(w2).audioBufferAutoMs == 120);
    }
    {
        // CHOOSING AUTOMATIC BY HAND resets it to 120 and FORGETS the memory - the person's own choice.
        cascade::core::AppConfig remembered;
        remembered.audioBufferAutoMs = 480;
        cascade::gui::AppWindow hand;
        Access::audio(hand).close();
        Access::restore(hand, remembered);
        CHECK(Access::leadNow(hand) == 480);
        Access::setBuffer(hand, 240);                              // a fixed value: remembered but ignored
        CHECK(Access::leadNow(hand) == 240);
        CHECK(Access::config(hand).audioBufferAutoMs == 480);
        Access::setBuffer(hand, 0);                                // back to AUTOMATIC, by hand
        CHECK(Access::leadNow(hand) == 120);
        CHECK(Access::config(hand).audioBufferAutoMs == 0);
    }

    // A shorter setting must shed audio queued under the old 960 ms lead. The
    // stream is closed so the test can drive the same callback code directly;
    // the setting thread must leave the ring for that callback to consume.
    {
        cascade::gui::AppWindow app;
        cascade::sink::AudioOut& out = Access::audio(app);
        out.close();
        Access::setBuffer(app, 960);
        const std::size_t channels = static_cast<std::size_t>(out.channels());
        std::vector<float> oldAudio(cascade::sink::audioLeadFrames(960) * channels, 0.25f);
        CHECK(out.write(oldAudio.data(), oldAudio.size()) == oldAudio.size());
        CHECK(out.ringFrames() >= oldAudio.size() / channels);
        Access::setBuffer(app, 120);
        CHECK(Access::leadNow(app) == 120);
        CHECK(out.ringFrames() >= oldAudio.size() / channels);  // queued until the callback
        std::vector<float> heard(480 * channels);
        CHECK(cascade::sink::AudioOut::pullBlock(&out, heard.data(), 480) == heard.size());
        CHECK(out.ringFrames() <= out.targetFrames());
        CHECK(out.primed());  // the retained 120 ms lead is ready to play
    }

    // --- 11. THE NEXT USAGE RECORD carries it ------------------------------------------------------
    // This session counted `recovered.audiolead` into the ledger's FILE, where the next
    // start-up finds it as an earlier session's and puts it in the record it sends.
    ledger->flush();
    Counts onDisk;
    CHECK(HealthLedger::parseFileText(readFile(ledgerFile), installId, onDisk));
    std::printf("health: the ledger's file holds %s\n", health::encode(onDisk).c_str());
    CHECK(countOf(onDisk, "recovered.audiolead") == 1);
    {
        auto next = std::make_shared<HealthLedger>();
        next->arm(ledgerFile, installId, true);                   // the next launch
        CHECK(countOf(next->priorCounts(), "recovered.audiolead") == 1);
        const std::string pending = "{\"id\":\"" + installId + "\",\"v\":\"0.99.73\"}";
        std::string json;
        std::uint64_t carriedStalls = 0;
        Counts carried;
        cascade::core::StallLedger stalls;
        CHECK(cascade::core::prepareStartupRecord("", pending, stalls, *next, json, carriedStalls,
                                                  carried));
        const std::string text = nlohmann::json::parse(json)["health"].get<std::string>();
        std::printf("health: the next usage record carries \"%s\"\n", text.c_str());
        CHECK(text.find("recovered.audiolead=1") != std::string::npos);
        CHECK(health::validToken("recovered.audiolead"));
    }

    // --- 12. THE FRAME LOOP'S POLL CLOSES A MINUTE once a minute, and the card and the rail
    // print the one sentence. Read off the source: drawing needs a window.
    {
        // CODE, NOT COMMENTS: a call commented out must not satisfy a scan for the call.
        const std::string src =
            stripComments(readFile(fs::path(CASCADE_SOURCE_DIR) / "src" / "gui" / "app_window.cpp"));
        const std::size_t poll = src.find("void AppWindow::pollAudioHealth()");
        const std::size_t end = src.find("void AppWindow::closeAudioMinute(", poll);
        CHECK(poll != std::string::npos && end != std::string::npos && end > poll);
        const std::string body = src.substr(poll, end - poll);
        const std::size_t minute = body.find("now - lastAudioLogSec_ >= 60.0");
        const std::size_t close = body.find("closeAudioMinute(", minute);
        CHECK(minute != std::string::npos && close != std::string::npos);
        // ...inside that block, before the next statement of the poll (the 1 Hz probe).
        CHECK(close < body.find("now - lastAudioProbeSec_ < 1.0"));
        // The card and the rail both ask for the sentence.
        std::size_t uses = 0;
        for (std::size_t at = 0; (at = src.find("audioLeadNotice()", at)) != std::string::npos; at += 10) {
            ++uses;
        }
        CHECK(uses >= 3);   // the definition, the AUDIO card and the Sinks rail
        const std::size_t card = src.find("card(tr(\"AUDIO - UNDERRUNS\")");
        CHECK(card != std::string::npos);
        CHECK(src.rfind("audioLeadNotice()", card) != std::string::npos && card - src.rfind("audioLeadNotice()", card) < 600);
        // The Sinks rail: the sentence, and the box itself - its label, its five
        // choices, and the call that makes a pick the buffer. (Drawing needs a window; the
        // bounded run of the real binary draws every bank once.)
        const std::size_t rail = src.find("void AppWindow::drawSinksSection()");
        const std::size_t railEnd = src.find("void AppWindow::drawTrailWidthControl(", rail);
        CHECK(rail != std::string::npos && railEnd != std::string::npos && railEnd > rail);
        const std::string section = src.substr(rail, railEnd - rail);
        CHECK(section.find("audioLeadNotice()") != std::string::npos);
        CHECK(section.find("trId(\"Audio buffer\")") != std::string::npos);
        CHECK(section.find("tr(\"AUTOMATIC\")") != std::string::npos);
        CHECK(section.find("{0, 120, 240, 480, 960}") != std::string::npos);
        CHECK(section.find("setAudioBufferMs(choice)") != std::string::npos);

        // A patch speaker made from the patch page starts with the receiver's buffer.
        const std::string patch = stripComments(
            readFile(fs::path(CASCADE_SOURCE_DIR) / "src" / "gui" / "app_window_patch_radios.cpp"));
        std::size_t made = 0;
        for (std::size_t at = 0; (at = patch.find("makeDeviceDest(", at)) != std::string::npos; at += 10) {
            const std::size_t eol = patch.find(';', at);
            CHECK(eol != std::string::npos &&
                  patch.substr(at, eol - at).find("audioLeadNowMs()") != std::string::npos);
            ++made;
        }
        CHECK(made == 2);   // the default output and a named one
    }

    // --- 13. A SPEAKER MADE WITH A BUFFER HAS IT (needs an output; skipped without one) ------------
    {
        std::string err;
        std::shared_ptr<cascade::core::patch::AudioDest> d =
            cascade::core::patch::makeDeviceDest(std::string(), err, 480);
        if (!d) {
            std::printf("SKIP a patch speaker made with a 480 ms buffer: no output opened (%s)\n", err.c_str());
            ++g_checksSkipped;
        } else {
            CHECK(d->leadMs() == 480);
            d->setLeadMs(960);
            CHECK(d->leadMs() == 960);
            d->setLeadMs(1);                       // clamped to the 120 ms floor, as the sink's is
            CHECK(d->leadMs() == 120);
            auto plain = cascade::core::patch::makeDeviceDest(std::string(), err);
            if (plain) { CHECK(plain->leadMs() == 120); }   // no buffer named: the default
        }
    }

    std::error_code gone;
    fs::remove_all(g_scratch, gone);
    return testSummary("test_audio_lead_app");
}
