// lanek_publish.cpp - see lanek_publish.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "lanek_publish.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>

#include "core/access_palette.hpp"
#include "core/access_sonify.hpp"
#include "core/access_speech.hpp"
#include "lanek_audio.hpp"
#include "core/gradient_model.hpp"
#include "dsp/sigmeas_cursors.hpp"
#include "dsp/trace_average.hpp"
#include "lanek_spectrum.hpp"
#include "core/i18n.hpp"
#include "core/radio_matrix_model.hpp"
#include "core/sys_effects_policy.hpp"
#include "core/sys_first_run.hpp"
#include "core/sys_resource_meter.hpp"
#include "core/utf8_text.hpp"
#include "host_logic.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace foxsdr::slintlanek {

namespace {
namespace cc = cascade::core;
namespace sys = cascade::core::sys;
namespace acc = cascade::core::access;

using cascade::i18n::tr;
using cc::formatText;

slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }

fs::path pathOf(const std::string& u8) { return fs::path(std::u8string(u8.begin(), u8.end())); }

std::string num(const char* f, double v) {   // one double into one of this unit's own literal formats (never a translated text)
    std::string s;
    cc::formatUtf8(s, f, v);
    return s;
}

KRow toK(const Row& r) {
    KRow k;
    k.kind = r.kind;
    k.id = ss(r.id);
    k.label = ss(r.label);
    k.value = ss(r.value);
    k.note = ss(r.note);
    k.on = r.on;
    k.enabled = r.enabled;
    std::vector<slint::SharedString> opts;
    for (const auto& o : r.options) { opts.push_back(ss(o)); }
    k.options = std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(opts));
    k.pick = r.pick;
    k.level = r.level;
    k.tone = r.tone;
    return k;
}

slint::Color colorOf(const acc::Rgb& c) { return slint::Color::from_rgb_uint8(c.r, c.g, c.b); }

const int kKeepChoices[4] = {5, 10, 20, 50};

enum class SheetKind { None, Wizard, ImportConfirm, SwitchConfirm, ResetConfirm, UndoConfirm, Closing, Gradient, Cursors };

std::string trimmed(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])) != 0) { ++a; }
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])) != 0) { --b; }
    return s.substr(a, b - a);
}

void openUrlNow(const std::string& url) {
#if defined(_WIN32)
    const std::wstring w = std::filesystem::path(std::u8string(url.begin(), url.end())).wstring();
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    (void)url;
#endif
}

bool urlOk(const std::string& u) { return u.rfind("https://", 0) == 0 && u.size() > 8 && u.size() < 512 && u.find_first_of(" \t\r\n\"<>") == std::string::npos; }
}  // namespace

struct LaneKPublisher::Impl {
    Env env;
    Record rec;
    std::unique_ptr<SettingsKeeper> keeper;
    foxsdr::slintkit::LiveTable table;
    fs::path recordPath;
    long lastSaveTick = 0;

    // the models the window shows
    std::shared_ptr<slint::VectorModel<KRow>> mBackup, mPerf, mAccess, mCommunity, mSpectrum, mDisplay, mSheet;
    std::shared_ptr<slint::VectorModel<KTile>> mTiles;
    std::string sigBackup, sigPerf, sigAccess, sigCommunity, sigSpectrum, sigDisplay, sigSheet, sigTiles;

    // settings and backup
    std::string status;               // the sentence the last action left under the section's keys
    int statusTone = kDim;
    std::string importPath;
    std::vector<std::string> bundleNames;      // the exports folder's bundles, newest first
    int profilePick = -1;
    std::string profileName;
    std::string syncFolder;
    SheetKind sheet = SheetKind::None;
    ImportPreview importPreview;
    cc::sys::SwitchPlan switchPlan;
    std::string switchName;
    cc::sys::ResetOptions resetOpts;
    std::string undoStamp;
    Pending pending;

    // the first-run setup
    std::optional<sys::FirstRunWizard> wizard;
    bool wizardAsked = false;

    // performance
    sys::ResourceMeter meter{sys::realResourceSources()};
    sys::EffectsPolicy policy;
    sys::ResourceSnapshot snap;
    bool haveSnap = false;
    double lastSampleMs = -1e9;
    double sampleCostMs = 0.0;
    int sampleCount = 0;
    bool lowEndStarted = false;
    bool reduced = false;
    int reducedRows = 0;

    // palette
    std::string paletteKey = "normal";

    // the Matrix
    cc::HoverListen hover;
    int hoverId = -1;          // 0-based, -1 = none
    int baseline = -1;         // 0-based
    bool matrixWasOpen = false;
    int matrixListen = 0;      // what the sheet last asked the solo call for (1-based, 0 = nobody); "-1" = untouched
    double lastNowS = 0;

    // numbers, speech and the tone
    int numFormat = 0;                 // 0 follow Windows, 1 always a dot
    int digitsPolicy = 0;              // 0 Latin, 1 native, 2 as the language prefers
    acc::NumberSymbols symbols;
    bool symbolsLoaded = false;
    std::unique_ptr<acc::Announcer> announcer;
    std::unique_ptr<acc::Speaker> speaker;
    acc::RecordingSpeaker* recording = nullptr;   // the stand-in a bounded run speaks into (nobody hears it)
    std::string lastSpoken;
    int spokenCount = 0;
    acc::Sonifier sonifier;
    ToneOut tone;
    bool toneOn = false;
    acc::ToneParams toneParams;
    double lastToneMs = 0;
    std::string toneNote;

    // the spectrum tools
    std::array<cascade::dsp::TraceAverager, 8> avg;            // one per radio slot
    int avgMode = 0;                                           // 0 off, 1 linear, 2 rms, 3 log, 4 exponential
    int avgFrames = 16;
    double avgTau = 1.0;
    bool zoomSelect = false;
    int detector = 0;                                          // 0 = the trace as drawn today, 1.. = RbwDetector + 1
    double lastRbwHz = 0.0;
    std::vector<float> detScratch;
    cascade::core::Mask mask;
    bool maskLoaded = false, maskShown = false;
    std::string maskPath, maskNote;
    double maskMargin = 0.0, maskWorstHz = 0.0;
    bool maskHaveResult = false, maskPass = true;
    long maskBreachFrames = 0;
    cascade::core::MaskEvaluator maskEval;
    double evalCentre = -1, evalSpan = -1; std::size_t evalBins = 0;
    // the gradient
    cascade::core::gradient::GradientEditor grad;
    bool gradOn = false;
    std::vector<foxsdr::slinthost::Rgba> gradTable;
    bool gradChanged = false;
    // the cursors
    cascade::sigmeas::CursorSet8 cursors;
    bool cursorsShown = false, cursorPick = false;
    int cursorNext = 0;
    std::array<double, 8> cursorLevelDb{};
    std::string cursorNote;
    int cursorRef = 0;

    // what a screen reader is told about the pictures (ui/theme.slint SrText), refreshed twice a second at most and only when the sentence changed
    std::string srSpectrum, srWaterfall, srCounter;
    std::vector<std::string> srPanes;
    void publishSr() {
        if (env.ui == nullptr) { return; }
        const acc::PhraseTable phrases;
        const double tuned = lastState.tunedHz;
        const std::string freq = acc::frequencyPhrase(tuned, symbols, phrases);
        const std::string mode = acc::modePhrase(foxsdr::slinthost::modeName(lastState.demodMode), phrases);
        std::string spec = formatText(tr("Spectrum, tuned to %s, %s."), freq.c_str(), mode.c_str());
        if (frameN >= 3) {
            const acc::SpectrumPeak pk = acc::findSpectrumPeak(frameCopy.data(), frameN, 6.0f);
            const double hz = frameCentre - frameSpan / 2.0 + static_cast<double>(pk.position) * frameSpan;
            if (pk.found) {
                spec += " " + formatText(tr("Strongest signal %s, %.0f decibels over the noise."), acc::frequencyPhrase(hz, symbols, phrases).c_str(), static_cast<double>(pk.overFloorDb));
            } else {
                spec += std::string(" ") + tr("No signal stands out from the noise.");
            }
            spec += " " + formatText(tr("The span is %s."), acc::frequencyPhrase(frameSpan, symbols, phrases).c_str());
        }
        const std::string wf = formatText(tr("Waterfall of the same span, newest line at the bottom, tuned to %s."), freq.c_str());
        const std::string ctr = freq + ", " + mode;
        auto& g = env.ui->global<SrText>();
        if (spec != srSpectrum) { srSpectrum = spec; g.set_spectrum(ss(spec)); }
        if (wf != srWaterfall) { srWaterfall = wf; g.set_waterfall(ss(wf)); }
        if (ctr != srCounter) { srCounter = ctr; g.set_counter(ss(ctr)); }
        // the panes of a multi-radio window: one sentence per open radio, from its state
        std::vector<std::string> panes(8);
        if (env.matrix) {
            for (const auto& m : env.matrix()) {
                if (m.slot < 0 || m.slot >= 8) { continue; }
                panes[static_cast<std::size_t>(m.slot)] = formatText(tr("Radio %d: tuned to %s, %s."), m.slot + 1, acc::frequencyPhrase(m.tunedHz, symbols, phrases).c_str(), acc::modePhrase(foxsdr::slinthost::modeName(m.demod), phrases).c_str());
            }
        }
        if (panes != srPanes) {
            srPanes = panes;
            std::vector<slint::SharedString> a, b;
            for (const auto& t : panes) { a.push_back(ss(t)); b.push_back(ss(t.empty() ? t : t + " " + tr("Waterfall."))); }
            g.set_pane_spectrum(std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(a)));
            g.set_pane_waterfall(std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(b)));
        }
    }

    bool quitAsked = false;
    std::string lastAction;    // for the drive scripts

    explicit Impl(Env e) : env(std::move(e)), table(env.api, env.session) {}

    // ---------------------------------------------------------------- the record
    void load() {
        if (env.settingsDir.empty()) { return; }
        recordPath = pathOf(env.settingsDir) / "lanek.json";
        std::ifstream f(recordPath, std::ios::binary);
        if (f) {
            std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            rec.loadText(text);
        }
    }
    void save() {
        rec.clean();
        if (recordPath.empty() || env.bounded && std::getenv("FOXSDR_SLINT_LANEK_SAVE") == nullptr) { return; }
        std::error_code ec;
        fs::create_directories(recordPath.parent_path(), ec);
        fs::path tmp = recordPath;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) { return; }
            out << rec.toText();
            if (!out) { return; }
        }
        fs::rename(tmp, recordPath, ec);
    }

    std::string fact(const char* key) { std::string s; if (!table.fact(key, s)) { s.clear(); } return s; }
    void submit(const FoxCommand& c, std::uint32_t radio = 0) { if (env.submit) { env.submit(c, radio); } }
    FoxCommand op(std::uint32_t code) { FoxCommand c{}; c.structSize = sizeof(c); c.op = code; return c; }

    void say(const std::string& s, int tone = kDim) { status = s; statusTone = tone; }

    // ---------------------------------------------------------------- the rows
    std::vector<Row> backupRows() {
        std::vector<Row> v;
        v.push_back(rHeading(tr("Settings file")));
        v.push_back(rKey("export", tr("Export settings"), true, tr("Writes one file with your settings, bookmarks and markers into the exports folder. Passwords, tokens and your anonymous id are never exported.")));
        v.push_back(rKey("backup", tr("Back up settings now")));
        v.push_back(rTick("autobackup", tr("Back up automatically when FoxSDR closes"), rec.getBool("backup.auto", false)));
        const int keep = static_cast<int>(rec.getInt("backup.keep", 10, 1, 50));
        int pick = 1;
        for (int i = 0; i < 4; ++i) { if (kKeepChoices[i] == keep) { pick = i; } }
        v.push_back(rChoice("keep", tr("Backups kept"), {"5", "10", "20", "50"}, pick));
        v.push_back(rHeading(tr("Import")));
        if (!bundleNames.empty()) {
            std::vector<std::string> names = bundleNames;
            if (names.size() > 6) { names.resize(6); }
            int cur = -1;
            for (std::size_t i = 0; i < names.size(); ++i) { if (importPath.size() >= names[i].size() && importPath.compare(importPath.size() - names[i].size(), std::string::npos, names[i]) == 0) { cur = static_cast<int>(i); } }
            v.push_back(rChoice("imp-pick", tr("Files in the exports folder"), names, cur));
        }
        v.push_back(rField("imp-path", tr("File to import"), importPath, tr("The full path of a .foxsdr-bundle file")));
        v.push_back(rKey("import", tr("Import settings..."), !importPath.empty()));
        v.push_back(rNote(tr("An import, a profile switch and a reset take effect when FoxSDR closes: it closes for you, and you start it again.")));
        v.push_back(rHeading(tr("Profiles")));
        const auto names = keeper ? keeper->profileNames() : std::vector<std::string>();
        if (!names.empty()) { v.push_back(rChoice("prof-pick", tr("Profile"), names, profilePick)); }
        v.push_back(rKey("prof-use", tr("Use this profile"), profilePick >= 0 && profilePick < static_cast<int>(names.size())));
        v.push_back(rKey("prof-delete", tr("Delete profile"), profilePick >= 0 && profilePick < static_cast<int>(names.size())));
        v.push_back(rField("prof-name", tr("Name for a new profile"), profileName, tr("1 to 40 letters, digits, spaces, dots, dashes or underscores")));
        v.push_back(rKey("prof-save", tr("Save as profile..."), !profileName.empty()));
        v.push_back(rField("sync-folder", tr("Shared folder for syncing profiles"), syncFolder, tr("A folder both computers can see; empty = off")));
        v.push_back(rKey("sync", tr("Sync profiles now"), !syncFolder.empty()));
        v.push_back(rHeading(tr("Start again")));
        v.push_back(rKey("wizard", tr("Run the first-time setup again")));
        v.push_back(rKey("reset", tr("Reset everything to defaults...")));
        const bool anyUndo = keeper && !keeper->resetStamps().empty();
        v.push_back(rKey("undo-reset", tr("Undo the reset"), anyUndo));
        if (!status.empty()) { v.push_back(rNote(status, statusTone)); }
        return v;
    }

    std::vector<Row> perfRows() {
        std::vector<Row> v;
        v.push_back(rHeading(tr("This computer")));
        const auto pct = [&](const sys::Reading<double>& r) { return r.available ? num("%.0f%%", r.value) : std::string(tr("n/a")); };
        v.push_back(rBar(tr("CPU (this program)"), haveSnap && snap.processCpuPercent.available ? static_cast<float>(snap.processCpuPercent.value / 100.0) : 0.0f, haveSnap ? pct(snap.processCpuPercent) : std::string(tr("n/a"))));
        v.push_back(rReadout(tr("Memory (this program)"), haveSnap && snap.workingSetBytes.available ? num("%.0f MB", static_cast<double>(snap.workingSetBytes.value) / (1024.0 * 1024.0)) : std::string(tr("n/a"))));
        v.push_back(rBar(tr("Memory (the whole computer)"), haveSnap && snap.systemMemoryPercent.available ? static_cast<float>(snap.systemMemoryPercent.value / 100.0) : 0.0f, haveSnap ? pct(snap.systemMemoryPercent) : std::string(tr("n/a"))));
        v.push_back(rBar(tr("Graphics processor"), haveSnap && snap.gpuPercent.available ? static_cast<float>(snap.gpuPercent.value / 100.0) : 0.0f, haveSnap ? pct(snap.gpuPercent) : std::string(tr("n/a"))));
        v.push_back(rHeading(tr("Effects")));
        const int mode = static_cast<int>(rec.getInt("perf.mode", 0, 0, 2));
        v.push_back(rChoice("perf-mode", tr("Reduced effects"), {tr("Automatic"), tr("On"), tr("Off")}, mode));
        v.push_back(rNote(reduced ? tr("Reduced effects are ON: the waterfall makes at most 15 lines a second, shadows and lamp glow are off.") : tr("Reduced effects are off. Automatic turns them on when this program or the computer is under load, or on a small computer."), reduced ? kWarn : kDim));
        return v;
    }

    std::vector<Row> accessRows() {
        std::vector<Row> v;
        v.push_back(rHeading(tr("Colours")));
        std::vector<std::string> labels = {tr("Normal"), tr("Colour-blind safe: green-weak (deuteranopia)"), tr("Colour-blind safe: red-weak (protanopia)"), tr("Colour-blind safe: blue-weak (tritanopia)")};
        int pick = 0;
        const auto& ps = acc::palettes();
        for (int i = 0; i < 4; ++i) { if (paletteKey == ps[static_cast<std::size_t>(i)].key) { pick = i; } }
        v.push_back(rChoice("palette", tr("Colour set"), labels, pick));
        v.push_back(rNote(tr("The high-contrast look is its own tick under Display.")));
        v.push_back(rHeading(tr("Numbers")));
        v.push_back(rChoice("numfmt", tr("Number format"), {tr("Follow the Windows regional settings"), tr("Always use a dot")}, numFormat));
        v.push_back(rChoice("digits", tr("Digits"), {tr("Latin (0-9)"), tr("The language's own digits"), tr("As the language prefers")}, digitsPolicy, numFormat == 0));
        v.push_back(rNote(tr("Units (feet or metres, knots or km/h) are chosen where an altitude or a speed is shown; this window has none yet.")));
        v.push_back(rHeading(tr("Spoken readout")));
        v.push_back(rKey("speak-now", tr("Speak the frequency, mode and level now"), true, tr("Uses the Windows voice on the default sound output")));
        v.push_back(rTick("speech-onchange", tr("Speak changes automatically"), rec.getBool("speech.onchange", false)));
        v.push_back(rTick("speech-start", tr("Speak the first state on start"), rec.getBool("speech.start", false)));
        v.push_back(rTick("speech-freq", tr("Speak the frequency"), rec.getBool("speech.freq", true)));
        v.push_back(rTick("speech-mode", tr("Speak the mode"), rec.getBool("speech.mode", true)));
        v.push_back(rTick("speech-level", tr("Speak the signal level"), rec.getBool("speech.level", true)));
        v.push_back(rChoice("speech-style", tr("Signal level as"), {tr("S-units"), tr("Decibels")}, static_cast<int>(rec.getInt("speech.style", 0, 0, 1))));
        v.push_back(rSlider("speech-interval", tr("Wait between announcements"), static_cast<float>(rec.getInt("speech.interval", 3000, 0, 60000)) / 60000.0f, num("%.1f s", static_cast<double>(rec.getInt("speech.interval", 3000, 0, 60000)) / 1000.0)));
        v.push_back(rSlider("speech-settle", tr("Wait for the dial to settle"), static_cast<float>(rec.getInt("speech.settle", 800, 200, 5000) - 200) / 4800.0f, num("%.1f s", static_cast<double>(rec.getInt("speech.settle", 800, 200, 5000)) / 1000.0)));
        v.push_back(rSlider("speech-minfreq", tr("Smallest frequency change to announce"), static_cast<float>(std::log10(rec.getNum("speech.minfreq", 100.0, 1.0, 100000.0)) / 5.0), num("%.0f Hz", rec.getNum("speech.minfreq", 100.0, 1.0, 100000.0))));
        v.push_back(rSlider("speech-step", tr("Level change to announce"), static_cast<float>((rec.getNum("speech.step", 6.0, 1.0, 20.0) - 1.0) / 19.0), num("%.0f dB", rec.getNum("speech.step", 6.0, 1.0, 20.0))));
        v.push_back(rTick("speech-interrupt", tr("Interrupt the previous announcement"), rec.getBool("speech.interrupt", true)));
        v.push_back(rHeading(tr("Signal tone")));
        v.push_back(rTick("tone-on", tr("Play a tone that follows the signal"), toneOn, true, tr("A pitch that rises with the signal level, on the Windows default output")));
        if (!toneNote.empty()) { v.push_back(rNote(toneNote, kWarn)); }
        v.push_back(rSlider("tone-minhz", tr("Lowest pitch"), static_cast<float>((rec.getNum("sonify.minhz", 220.0, 100.0, 4000.0) - 100.0) / 3900.0), num("%.0f Hz", rec.getNum("sonify.minhz", 220.0, 100.0, 4000.0))));
        v.push_back(rSlider("tone-maxhz", tr("Highest pitch"), static_cast<float>((rec.getNum("sonify.maxhz", 1760.0, 100.0, 4000.0) - 100.0) / 3900.0), num("%.0f Hz", rec.getNum("sonify.maxhz", 1760.0, 100.0, 4000.0))));
        v.push_back(rTick("tone-log", tr("Musical pitch steps (logarithmic)"), rec.getBool("sonify.log", true)));
        v.push_back(rSlider("tone-floor", tr("Level that sounds lowest"), static_cast<float>((rec.getNum("sonify.floor", -120.0, -160.0, 0.0) + 160.0) / 160.0), num("%.0f dB", rec.getNum("sonify.floor", -120.0, -160.0, 0.0))));
        v.push_back(rSlider("tone-ceiling", tr("Level that sounds highest"), static_cast<float>((rec.getNum("sonify.ceiling", -20.0, -160.0, 0.0) + 160.0) / 160.0), num("%.0f dB", rec.getNum("sonify.ceiling", -20.0, -160.0, 0.0))));
        v.push_back(rSlider("tone-volume", tr("Tone volume"), static_cast<float>(rec.getNum("sonify.volume", 0.3, 0.0, 0.5) / 0.5), num("%.0f%%", rec.getNum("sonify.volume", 0.3, 0.0, 0.5) / 0.5 * 100.0)));
        v.push_back(rTick("tone-mutesq", tr("Silent while the squelch is closed"), rec.getBool("sonify.mutesq", true)));
        v.push_back(rHeading(tr("For a screen reader")));
        v.push_back(rNote(tr("The spectrum, the waterfall and the frequency are described to a screen reader: the strongest signal in the span, how many seconds the waterfall holds, the tuned frequency. The description changes at most twice a second.")));
        v.push_back(rNote(tr("Right-to-left languages: the window can draw Arabic and Hebrew text, but no such language is installed yet and the layout is not mirrored."), kWarn));
        return v;
    }

    std::vector<Row> communityRows() {
        std::vector<Row> v;
        const std::string forum = forumUrl(), discord = discordUrl();
        if (forum.empty() && discord.empty()) { v.push_back(rNote(tr("No community links are set up in this copy of FoxSDR."))); }
        if (!forum.empty()) { v.push_back(rKey("community-forum", tr("Forum"), true, forum)); }
        if (!discord.empty()) { v.push_back(rKey("community-discord", tr("Discord"), true, discord)); }
        return v;
    }

    acc::AnnouncePolicy speechPolicy() const {
        acc::AnnouncePolicy a;
        a.onChange = rec.getBool("speech.onchange", false);
        a.announceOnStart = rec.getBool("speech.start", false);
        a.speakFrequency = rec.getBool("speech.freq", true);
        a.speakMode = rec.getBool("speech.mode", true);
        a.speakLevel = rec.getBool("speech.level", true);
        a.levelStyle = rec.getInt("speech.style", 0, 0, 1) == 1 ? acc::LevelStyle::Decibels : acc::LevelStyle::SUnits;
        a.minIntervalMs = static_cast<double>(rec.getInt("speech.interval", 3000, 0, 60000));
        a.settleMs = static_cast<double>(rec.getInt("speech.settle", 800, 200, 5000));
        a.minFreqChangeHz = rec.getNum("speech.minfreq", 100.0, 1.0, 100000.0);
        a.levelStepDb = rec.getNum("speech.step", 6.0, 1.0, 20.0);
        a.interrupt = rec.getBool("speech.interrupt", true);
        return a;
    }
    acc::SonifySettings sonifySettings() const {
        acc::SonifySettings t;
        t.minHz = static_cast<float>(rec.getNum("sonify.minhz", 220.0, 100.0, 4000.0));
        t.maxHz = std::max(t.minHz, static_cast<float>(rec.getNum("sonify.maxhz", 1760.0, 100.0, 4000.0)));
        t.logPitch = rec.getBool("sonify.log", true);
        const float lo = static_cast<float>(rec.getNum("sonify.floor", -120.0, -160.0, 0.0)), hi = static_cast<float>(rec.getNum("sonify.ceiling", -20.0, -160.0, 0.0));
        t.curve.setLinear(lo, hi);
        t.maxAmplitude = static_cast<float>(rec.getNum("sonify.volume", 0.3, 0.0, 0.5));
        t.muteWhenSquelchClosed = rec.getBool("sonify.mutesq", true);
        t.panWidth = static_cast<float>(rec.getNum("sonify.pan", 1.0, 0.0, 1.0));
        return t;
    }
    void applySpeech() { if (announcer) { announcer->setPolicy(speechPolicy()); } }
    void applySonify() { sonifier.setSettings(sonifySettings()); }
    void applyNumbers() {
        numFormat = static_cast<int>(rec.getInt("access.numfmt", 0, 0, 1));
        digitsPolicy = static_cast<int>(rec.getInt("access.digits", 0, 0, 2));
        const bool locale = numFormat == 0 && (!env.bounded || std::getenv("FOXSDR_SLINT_LOCALE") != nullptr);
        if (locale) {
            if (!symbolsLoaded) { symbolsLoaded = acc::loadNumberSymbols("", true, symbols); }
            if (symbolsLoaded) {
                foxsdr::slinthost::setNumberLocale(&symbols, digitsPolicy == 1 ? acc::DigitPolicy::Native : digitsPolicy == 2 ? acc::DigitPolicy::FromLocale : acc::DigitPolicy::Latin);
                if (announcer) { announcer->setSymbols(symbols); }
                return;
            }
        }
        foxsdr::slinthost::setNumberLocale(nullptr);
        if (announcer) { announcer->setSymbols(acc::NumberSymbols{}); }
    }
    acc::SpokenState spokenState() const {
        acc::SpokenState st;
        st.freqHz = lastState.tunedHz;
        st.mode = foxsdr::slinthost::modeName(lastState.demodMode);
        st.levelRelDb = lastState.signalDb;
        st.hasSquelch = lastState.squelchDb > -190.0;
        st.squelchOpen = (lastState.flags & FOXAPI_RX_SQUELCH_OPEN) != 0;
        return st;
    }
    void speakerReady() {
        if (speaker) { return; }
        if (env.bounded) { auto r = std::make_unique<acc::RecordingSpeaker>(); recording = r.get(); speaker = std::move(r); }
        else { speaker = makeDeviceSpeaker(); }
    }

    std::vector<Row> displayRows() {
        std::vector<Row> v;
        v.push_back(rHeading(tr("Trace averaging")));
        v.push_back(rChoice("avg-mode", tr("Averaging"), {tr("Off (the trace as it is)"), tr("Linear (mean of the amplitudes)"), tr("RMS (mean of the powers)"), tr("Log (mean of the dB values)"), tr("Exponential (fading)")}, avgMode));
        if (avgMode >= 1 && avgMode <= 3) { v.push_back(rSlider("avg-frames", tr("Frames"), static_cast<float>(avgFrames - 1) / 127.0f, formatText(tr("%d frames"), avgFrames))); }
        if (avgMode == 4) { v.push_back(rSlider("avg-tau", tr("Time constant"), static_cast<float>(std::log(avgTau / 0.05) / std::log(10.0 / 0.05)), formatText(tr("%.2f s"), avgTau))); }
        v.push_back(rTick("zoom-select", tr("Zoom to a dragged selection"), zoomSelect, true, tr("Drag over the spectrum to choose a span; the view zooms to it. Holding Ctrl while dragging does the same at any time.")));
        v.push_back(rKey("grad-open", tr("Gradient editor..."), true, tr("Make your own waterfall colour scale from coloured stops")));
        return v;
    }

    std::vector<Row> spectrumRows() {
        std::vector<Row> v;
        v.push_back(rHeading(tr("Detector")));
        v.push_back(rChoice("det-mode", tr("Trace detector"), {tr("As drawn (strongest bin of a column)"), tr("Peak"), tr("Negative peak"), tr("RMS (mean power)"), tr("Voltage average"), tr("Log average"), tr("Sample")}, detector));
        v.push_back(rReadout(tr("Resolution bandwidth"), rbwText(lastRbwHz)));
        v.push_back(rNote(tr("The bandwidth is that of the window the spectrum runs (Blackman-Harris). A finer one needs a longer FFT.")));
        v.push_back(rHeading(tr("Limit mask")));
        v.push_back(rField("mask-path", tr("Mask file"), maskPath, tr("A text file whose first line is: foxmask 1, then lines like: upper 88e6 -30 108e6 -30")));
        v.push_back(rKey("mask-example", tr("Write an example mask file"), !env.exportsDir.empty(), tr("Writes a small mask file into the exports folder and fills in its path")));
        v.push_back(rKey("mask-load", tr("Load the mask"), !maskPath.empty()));
        v.push_back(rTick("mask-show", tr("Show the limit lines"), maskShown, maskLoaded));
        if (maskHaveResult) {
            v.push_back(rReadout(tr("Worst margin"), maskPass ? num("%.1f dB", maskMargin) : num("%.1f dB (breach)", maskMargin)));
            v.push_back(rReadout(tr("Frames in breach"), std::to_string(maskBreachFrames)));
        }
        if (!maskNote.empty()) { v.push_back(rNote(maskNote, maskLoaded ? kGood : kErr)); }
        v.push_back(rHeading(tr("Cursors")));
        v.push_back(rKey("cur-open", tr("Eight-cursor tool..."), true, tr("Eight cursors on the spectrum: free, at a fixed spacing, or as the harmonics of a base")));
        v.push_back(rTick("cur-show", tr("Show the cursors"), cursorsShown));
        return v;
    }

    std::string cursorReadoutText(std::size_t i) {
        const auto ro = cursors.readout();
        const auto& r = ro[i];
        std::string t = formatText(tr("offset %+.3f kHz"), r.offsetHz / 1e3);
        if (std::isfinite(r.ratio)) { t += formatText(tr("  ratio %.3f"), r.ratio); }
        if (std::isfinite(cursorLevelDb[i])) { t += formatText(tr("  level %.1f dB"), cursorLevelDb[i]); }
        if (!r.inRange) { t += std::string("  ") + tr("(out of range)"); }
        return t;
    }

    void gradRows(std::vector<Row>& v, std::string& title) {
        title = tr("Gradient editor");
        v.push_back(rKey("sheet-dismiss", tr("Done"), true, "", true));
        v.push_back(rNote(tr("Your own colour scale for the waterfall: coloured stops from the weakest signal (position 0) to the strongest (position 1). Colours are typed as RRGGBB.")));
        v.push_back(rTick("grad-on", tr("Use my gradient for the waterfall"), gradOn));
        v.push_back(rChoice("grad-interp", tr("Blend in"), {tr("sRGB"), tr("Linear light"), tr("Oklab (even brightness steps)")}, static_cast<int>(grad.gradient().interp)));
        const auto& st = grad.gradient().stops;
        for (std::size_t i = 0; i < st.size(); ++i) {
            const bool interior = i > 0 && i + 1 < st.size();
            char hex[16]; std::snprintf(hex, sizeof(hex), "%02X%02X%02X", st[i].colour.r, st[i].colour.g, st[i].colour.b);
            v.push_back(rField("grad-col-" + std::to_string(i), formatText(tr("Stop %d colour (position %.3f)"), static_cast<int>(i + 1), st[i].pos), hex));
            if (interior) {
                v.push_back(rSlider("grad-pos-" + std::to_string(i), formatText(tr("Stop %d position"), static_cast<int>(i + 1)), static_cast<float>(st[i].pos), num("%.3f", st[i].pos)));
                v.push_back(rKey("grad-del-" + std::to_string(i), formatText(tr("Remove stop %d"), static_cast<int>(i + 1))));
            }
        }
        v.push_back(rKey("grad-add", tr("Add a stop in the middle")));
        v.push_back(rKey("grad-reverse", tr("Reverse the scale")));
        v.push_back(rKey("grad-reset", tr("Back to the default scale")));
    }

    void cursorRows(std::vector<Row>& v, std::string& title) {
        title = tr("Eight-cursor tool");
        v.push_back(rKey("sheet-dismiss", tr("Done"), true, "", true));
        const int mode = static_cast<int>(cursors.mode());
        v.push_back(rChoice("cur-mode", tr("Cursors move"), {tr("Each on its own"), tr("At a fixed spacing"), tr("As harmonics of cursor 1")}, mode));
        if (mode == 1) { v.push_back(rField("cur-spacing", tr("Spacing (kHz)"), num("%.3f", cursors.spacingHz() / 1e3))); }
        std::vector<std::string> refs;
        for (int i = 1; i <= 8; ++i) { refs.push_back(formatText(tr("Cursor %d"), i)); }
        v.push_back(rChoice("cur-ref", tr("Offsets and ratios are against"), refs, cursorRef));
        for (std::size_t i = 0; i < 8; ++i) {
            v.push_back(rField("cur-f-" + std::to_string(i), formatText(tr("Cursor %d (MHz)"), static_cast<int>(i + 1)), num("%.6f", cursors.frequency(i) / 1e6)));
            v.push_back(rNote(cursorReadoutText(i)));
        }
        v.push_back(rTick("cur-pick", tr("Click the spectrum to place the next cursor"), cursorPick));
        v.push_back(rKey("cur-tuned", tr("Put cursor 1 on the tuned frequency")));
        v.push_back(rKey("cur-snap", tr("Snap the cursors to the nearest peaks"), mode == 0));
        v.push_back(rTick("cur-show", tr("Show the cursors"), cursorsShown));
        if (!cursorNote.empty()) { v.push_back(rNote(cursorNote)); }
    }

    std::string forumUrl() const { const std::string u = rec.getStr("community.forum", ""); return urlOk(u) ? u : std::string(); }
    std::string discordUrl() const { const std::string u = rec.getStr("community.discord", ""); return urlOk(u) ? u : std::string(); }

    // ---------------------------------------------------------------- the sheet
    std::vector<Row> sheetRows(std::string& title, int& width) {
        std::vector<Row> v;
        width = 460;
        switch (sheet) {
            case SheetKind::None: break;
            case SheetKind::Wizard: wizardRows(v, title); break;
            case SheetKind::ImportConfirm: {
                title = tr("Import settings");
                v.push_back(rNote(formatText(tr("Replace %d file(s)? Your current ones are kept as .before-import copies."), importPreview.replace)));
                v.push_back(rKey("sheet-go", tr("Import and close FoxSDR"), true, "", true));
                v.push_back(rKey("sheet-dismiss", tr("Cancel")));
                if (importPreview.added > 0) { v.push_back(rNote(formatText(tr("%d file(s) are new."), importPreview.added))); }
                if (importPreview.identical > 0) { v.push_back(rNote(formatText(tr("%d file(s) are the same as now."), importPreview.identical))); }
                for (const auto& n : importPreview.res.notes) { v.push_back(rNote(n, kWarn)); }
                v.push_back(rNote(tr("FoxSDR closes to finish the import. Start it again afterwards.")));
                break;
            }
            case SheetKind::SwitchConfirm: {
                title = tr("Use a profile");
                v.push_back(rNote(formatText(tr("Replace %d file(s) with the profile \"%s\"? Your current ones are kept as .before-import copies."), static_cast<int>(switchPlan.plan.items.size()), switchName.c_str())));
                v.push_back(rKey("sheet-go", tr("Switch and close FoxSDR"), true, "", true));
                v.push_back(rKey("sheet-dismiss", tr("Cancel")));
                v.push_back(rNote(tr("This computer's own network switches and passwords are kept as they are.")));
                v.push_back(rNote(tr("FoxSDR closes to switch. Start it again afterwards.")));
                break;
            }
            case SheetKind::ResetConfirm: {
                title = tr("Reset everything to defaults");
                const auto plan = keeper->planReset(resetOpts);
                if (!plan.error.empty()) { v.push_back(rNote(plan.error, kErr)); }
                v.push_back(rTick("sheet-reset-plugins", tr("Also set aside installed plugins"), resetOpts.includePlugins));
                v.push_back(rTick("sheet-reset-logs", tr("Also set aside log files"), resetOpts.includeLogs));
                v.push_back(rKey("sheet-go", tr("Reset and close FoxSDR"), plan.error.empty() && !plan.items.empty(), "", true));
                v.push_back(rKey("sheet-dismiss", tr("Cancel")));
                v.push_back(rNote(tr("These files will be set aside:")));
                for (const auto& it : plan.items) { v.push_back(rNote(it.relative + (it.isFolder ? formatText(tr("  (%d files)"), static_cast<int>(it.files)) : std::string()))); }
                if (plan.items.empty()) { v.push_back(rNote(tr("Nothing to set aside."))); }
                if (plan.leftAlone > 0) { v.push_back(rNote(formatText(tr("%d other file(s) in the folder are left alone."), static_cast<int>(plan.leftAlone)))); }
                v.push_back(rNote(tr("They are moved into a reset folder, not deleted: \"Undo the reset\" brings them back. FoxSDR closes to finish; start it again afterwards.")));
                break;
            }
            case SheetKind::UndoConfirm: {
                title = tr("Undo the reset");
                v.push_back(rNote(tr("The files set aside by the last reset are put back. FoxSDR closes to finish; start it again afterwards.")));
                v.push_back(rKey("sheet-go", tr("Undo and close FoxSDR"), !undoStamp.empty(), "", true));
                v.push_back(rKey("sheet-dismiss", tr("Cancel")));
                break;
            }
            case SheetKind::Gradient: gradRows(v, title); width = 520; break;
            case SheetKind::Cursors: cursorRows(v, title); width = 560; break;
            case SheetKind::Closing: {
                title = tr("FoxSDR is closing");
                v.push_back(rNote(tr("The change is applied as FoxSDR closes. Start FoxSDR again afterwards.")));
                break;
            }
        }
        return v;
    }

    void wizardRows(std::vector<Row>& v, std::string& title) {
        if (!wizard) { wizard.emplace(); }
        auto& w = *wizard;
        title = tr("Welcome to FoxSDR");
        switch (w.step()) {
            case sys::WizardStep::Welcome:
                v.push_back(rNote(tr("A few quick questions to set FoxSDR up for your radio. You can skip them and change everything later.")));
                v.push_back(rKey("wiz-next", tr("Next"), true, "", true));
                v.push_back(rKey("wiz-skip", tr("Skip setup")));
                break;
            case sys::WizardStep::Radio: {
                title = tr("Which radio do you have?");
                std::vector<std::string> opts;
                for (const auto& c : sys::radioChoices()) { opts.push_back(tr(c.label)); }
                v.push_back(rChoice("wiz-radio", tr("Radio"), opts, wizardRadio));
                v.push_back(rKey("wiz-back", tr("Back")));
                v.push_back(rKey("wiz-next", tr("Next"), w.canNext(), "", true));
                v.push_back(rKey("wiz-skip", tr("Skip setup")));
                break;
            }
            case sys::WizardStep::Preset: {
                title = tr("Add some starting stations?");
                std::vector<std::string> opts = {tr("None")};
                for (const auto& p : w.offered()) { opts.push_back(tr(p.title.c_str())); }
                v.push_back(rChoice("wiz-preset", tr("Starting stations"), opts, wizardPreset));
                v.push_back(rKey("wiz-back", tr("Back")));
                v.push_back(rKey("wiz-next", tr("Next"), true, "", true));
                v.push_back(rKey("wiz-skip", tr("Skip setup")));
                break;
            }
            case sys::WizardStep::Privacy:
                title = tr("Anonymous usage reports");
                v.push_back(rNote(tr("FoxSDR can send anonymous usage reports: how often it is used and which features. They never contain what you listen to, your position or anything that names you. You can change this at any time under Usage reporting.")));
                v.push_back(rTick("wiz-telemetry", tr("Send anonymous usage reports"), wizardTelemetry));
                v.push_back(rKey("wiz-back", tr("Back")));
                v.push_back(rKey("wiz-next", tr("Done"), true, "", true));
                v.push_back(rKey("wiz-skip", tr("Skip setup")));
                break;
            case sys::WizardStep::Done: break;
        }
    }
    int wizardRadio = -1;
    int wizardPreset = 0;
    bool wizardTelemetry = true;

    void openWizard() {
        wizard.emplace();
        wizardRadio = -1;
        wizardPreset = 0;
        const std::string t = fact("fact.telemetry.enabled");
        wizardTelemetry = t.empty() ? true : t == "1";
        wizard->chooseTelemetry(wizardTelemetry);
        sheet = SheetKind::Wizard;
    }

    void finishWizard() {
        auto& w = *wizard;
        const sys::WizardOutcome out = w.outcome();
        const std::string settingsDir = env.settingsDir;
        if (out.applied) {
            // the starter bookmarks: tune, set the mode, add (the engine's bookmark op takes the CURRENT frequency and mode); the tuning is put back afterwards
            const double hz0 = lastState.tunedHz;
            const std::uint32_t mode0 = lastState.demodMode;
            int added = 0;
            for (const auto& b : out.bookmarks) {
                FoxCommand f = op(FOXAPI_OP_SET_FREQUENCY); f.num[0] = b.freqHz; submit(f);
                std::uint32_t demod = 0;
                for (int i = 0; i < 8; ++i) { if (b.mode == foxsdr::slinthost::modeName(foxsdr::slinthost::demodOfIndex(i))) { demod = foxsdr::slinthost::demodOfIndex(i); } }
                if (demod != 0) { FoxCommand m = op(FOXAPI_OP_SET_MODE); m.ival[0] = demod; submit(m); }
                FoxCommand a = op(FOXAPI_OP_BOOKMARK_ADD); std::snprintf(a.text, sizeof(a.text), "%s", b.name.c_str()); submit(a);
                ++added;
            }
            if (added > 0 && hz0 > 0) {
                FoxCommand f = op(FOXAPI_OP_SET_FREQUENCY); f.num[0] = hz0; submit(f);
                if (mode0 >= 1 && mode0 <= 8) { FoxCommand m = op(FOXAPI_OP_SET_MODE); m.ival[0] = mode0; submit(m); }
            }
            const std::string now = fact("fact.telemetry.enabled");
            if ((now == "1") != out.telemetryEnabled) { FoxCommand c = op(FOXAPP_OP_TELEMETRY_CONSENT); c.ival[0] = out.telemetryEnabled ? 1 : 0; submit(c); }
            say(added > 0 ? formatText(tr("Setup finished: %d starting station(s) added to your bookmarks."), added) : std::string(tr("Setup finished.")), kGood);
        }
        if (!settingsDir.empty() && !env.bounded) { sys::markFirstRunDone(pathOf(settingsDir)); }
        lastAction = out.applied ? "wizard-done" : "wizard-skipped";
        wizard.reset();
        sheet = SheetKind::None;
    }

    FoxReceiverState lastState{};

    // ---------------------------------------------------------------- actions
    void act(const std::string& id, const std::string& text);
    void actSheet(const std::string& id, const std::string& text);
    void gradAct(const std::string& id, const std::string& text);
    void cursorAct(const std::string& id, const std::string& text);
    bool snapCursors();
    std::vector<float> frameCopy;          // the selected radio's last full frame (the cursor tool's peaks and levels)
    std::size_t frameN = 0;
    double frameCentre = 0, frameSpan = 0;
    void actMatrix(const std::string& id, const std::string& text);
    void refreshBundles() { if (keeper) { bundleNames = keeper->listBundles(keeper->dirs().exports); } }
    void beginPending(Pending p) {
        pending = std::move(p);
        sheet = SheetKind::Closing;
        lastAction = "pending-" + pending.what;
        if (!quitAsked && env.quit && !env.bounded) { quitAsked = true; env.quit(); }   // a bounded run ends at its own frame count and applies the work then (finishAtExit)
    }

    // ---------------------------------------------------------------- performance
    void samplePerf(double nowMs) {
        if (nowMs - lastSampleMs < 1000.0) { return; }
        lastSampleMs = nowMs;
        const auto t0 = std::chrono::steady_clock::now();
        snap = meter.sample();
        haveSnap = true;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        sampleCostMs = sampleCount == 0 ? ms : sampleCostMs * 0.9 + ms * 0.1;
        ++sampleCount;
        if (!lowEndStarted) { policy.startLowEnd(snap); lowEndStarted = true; }
        const int mode = static_cast<int>(rec.getInt("perf.mode", 0, 0, 2));
        policy.setMode(mode == 1 ? sys::EffectsMode::On : mode == 2 ? sys::EffectsMode::Off : sys::EffectsMode::Auto);
        const auto& sw = policy.update(snap);
        if (sw.reduced != reduced) {
            reduced = sw.reduced;
            reducedRows = reduced ? sw.waterfallRowsPerSecond : 0;
            dirtyStyle = true;
        }
    }
    bool dirtyStyle = true;

    // ---------------------------------------------------------------- the style
    void restyle() {
        if (env.ui == nullptr || !env.baseSkin) { return; }
        Skin s = env.baseSkin();
        const bool hc = s.hairline > 1.5f;   // the high-contrast skin: its own colours stand
        if (!hc && paletteKey != "normal") {
            const acc::Palette* pal = nullptr;
            for (const auto& p : acc::palettes()) { if (paletteKey == p.key) { pal = &p; } }
            if (pal != nullptr) {
                const auto role = [&](acc::Role r) { return colorOf(pal->role[static_cast<std::size_t>(r)]); };
                s.fg = role(acc::Role::Fg);
                s.muted = role(acc::Role::Muted);
                s.accent = role(acc::Role::Accent);
                s.focus = role(acc::Role::Focus);
                s.lamp_green = s.lamp_green_cap = s.chip_good_fg = s.meter_led_low = role(acc::Role::StatusOk);
                s.lamp_amber = s.lamp_amber_cap = s.chip_warn_fg = s.meter_led_mid = role(acc::Role::StatusWarn);
                s.lamp_red = s.lamp_red_cap = s.chip_err_fg = s.meter_led_high = role(acc::Role::StatusBad);
                s.lamp_yellow = s.lamp_yellow_cap = role(acc::Role::StatusInfo);
                s.trace_live = role(acc::Role::TraceLive);
                s.trace_peak = role(acc::Role::TracePeak);
                s.trace_average = role(acc::Role::TraceAverage);
                s.radio_1 = role(acc::Role::Radio1);
                s.radio_2 = role(acc::Role::Radio2);
                s.radio_3 = role(acc::Role::Radio3);
                s.radio_4 = role(acc::Role::Radio4);
            }
        }
        if (reduced) { s.shadows = false; s.lamp_glow = false; }
        env.ui->global<Theme>().set_skin(s);
        dirtyStyle = false;
    }
};

// ============================================================ construction ============================================================
LaneKPublisher::LaneKPublisher(Env env) : p_(std::make_unique<Impl>(std::move(env))) {
    Impl& I = *p_;
    I.load();
    if (!I.env.settingsDir.empty()) {
        KeeperDirs d;
        d.settings = pathOf(I.env.settingsDir);
        d.data = pathOf(I.env.dataDir);
        d.exports = pathOf(I.env.exportsDir);
        d.backups = d.settings / "backups";
        d.profiles = d.settings / "profiles";
        I.keeper = std::make_unique<SettingsKeeper>(d);
    }
    I.syncFolder = I.rec.getStr("sync.folder", "");
    I.paletteKey = I.rec.getStr("access.palette", "normal");
    I.mBackup = std::make_shared<slint::VectorModel<KRow>>();
    I.mPerf = std::make_shared<slint::VectorModel<KRow>>();
    I.mAccess = std::make_shared<slint::VectorModel<KRow>>();
    I.mCommunity = std::make_shared<slint::VectorModel<KRow>>();
    I.mSpectrum = std::make_shared<slint::VectorModel<KRow>>();
    I.mDisplay = std::make_shared<slint::VectorModel<KRow>>();
    I.mSheet = std::make_shared<slint::VectorModel<KRow>>();
    I.mTiles = std::make_shared<slint::VectorModel<KTile>>();
    if (I.env.ui != nullptr) {
        auto& g = I.env.ui->global<LaneK>();
        g.set_rows_backup(I.mBackup);
        g.set_rows_perf(I.mPerf);
        g.set_rows_access(I.mAccess);
        g.set_rows_community(I.mCommunity);
        g.set_rows_spectrum(I.mSpectrum);
        g.set_rows_display(I.mDisplay);
        g.set_sheet_rows(I.mSheet);
        I.env.ui->global<LaneKMatrix>().set_tiles(I.mTiles);
    }
    I.refreshBundles();
    I.applyNumbers();
    I.applySonify();
    I.avgMode = static_cast<int>(I.rec.getInt("spec.avg", 0, 0, 4));
    I.avgFrames = static_cast<int>(I.rec.getInt("spec.avgframes", 16, 1, 128));
    I.avgTau = I.rec.getNum("spec.avgtau", 1.0, 0.05, 10.0);
    I.detector = static_cast<int>(I.rec.getInt("spec.detector", 0, 0, 6));
    if (I.env.firstRun || std::getenv("FOXSDR_SLINT_FIRSTRUN") != nullptr) { I.wizardAsked = true; }
    I.hover.configure({});
}

LaneKPublisher::~LaneKPublisher() = default;

void LaneKPublisher::wire() {
    Impl* I = p_.get();
    I->env.ui->global<LaneK>().on_act([I](slint::SharedString id, slint::SharedString text) { I->act(std::string(id), std::string(text)); });
    I->env.ui->global<LaneKMatrix>().on_act([I](slint::SharedString id, slint::SharedString text) { I->actMatrix(std::string(id), std::string(text)); });
}

// ============================================================ actions ============================================================
void LaneKPublisher::Impl::act(const std::string& id, const std::string& text) {
    lastAction = id;
    if (id.rfind("sheet-", 0) == 0 || id.rfind("wiz-", 0) == 0) { actSheet(id, text); return; }
    const int idx = std::atoi(text.c_str());
    const std::string stamp = stampOf(env.unixNow ? env.unixNow() : 0);
    if (id == "export") {
        if (!keeper) { say(tr("There is no settings folder in this run."), kWarn); return; }
        const ExportOut o = keeper->exportTo(keeper->dirs().exports, stamp, env.version);
        say(o.ok ? formatText(tr("Exported to %s. %d private value(s) were left out."), o.path.c_str(), o.stripped) : formatText(tr("The export failed: %s"), o.error.c_str()), o.ok ? kGood : kErr);
        if (o.ok) { importPath = o.path; refreshBundles(); }
    } else if (id == "backup") {
        if (!keeper) { say(tr("There is no settings folder in this run."), kWarn); return; }
        const ExportOut o = keeper->backupNow(stamp, env.version, static_cast<int>(rec.getInt("backup.keep", 10, 1, 50)));
        say(o.ok ? formatText(tr("Backed up to %s."), o.path.c_str()) : formatText(tr("The backup failed: %s"), o.error.c_str()), o.ok ? kGood : kErr);
    } else if (id == "autobackup") {
        rec.setBool("backup.auto", text == "1");
    } else if (id == "keep") {
        rec.setInt("backup.keep", kKeepChoices[std::clamp(idx, 0, 3)]);
    } else if (id == "imp-pick") {
        refreshBundles();
        if (idx >= 0 && idx < static_cast<int>(bundleNames.size()) && keeper) { importPath = (keeper->dirs().exports / pathOf(bundleNames[static_cast<std::size_t>(idx)])).string(); }
    } else if (id == "imp-path") {
        importPath = trimmed(text);
    } else if (id == "import") {
        if (!keeper || importPath.empty()) { return; }
        importPreview = keeper->previewImport(pathOf(importPath));
        if (!importPreview.ok) { say(formatText(tr("That file cannot be imported: %s"), importPreview.error.c_str()), kErr); return; }
        sheet = SheetKind::ImportConfirm;
    } else if (id == "prof-pick") {
        profilePick = idx;
    } else if (id == "prof-name") {
        profileName = trimmed(text);
    } else if (id == "prof-save") {
        if (!keeper) { return; }
        std::string err;
        const bool ok = keeper->saveProfile(profileName, true, env.version, err);
        say(ok ? formatText(tr("Saved the profile \"%s\"."), profileName.c_str()) : formatText(tr("The profile was not saved: %s"), err.c_str()), ok ? kGood : kErr);
        if (ok) { const auto n = keeper->profileNames(); for (std::size_t i = 0; i < n.size(); ++i) { if (n[i] == profileName) { profilePick = static_cast<int>(i); } } }
    } else if (id == "prof-delete") {
        if (!keeper) { return; }
        const auto n = keeper->profileNames();
        if (profilePick < 0 || profilePick >= static_cast<int>(n.size())) { return; }
        std::string err;
        const std::string name = n[static_cast<std::size_t>(profilePick)];
        const bool ok = keeper->deleteProfile(name, err);
        say(ok ? formatText(tr("Deleted the profile \"%s\"."), name.c_str()) : formatText(tr("The profile was not deleted: %s"), err.c_str()), ok ? kGood : kErr);
        profilePick = -1;
    } else if (id == "prof-use") {
        if (!keeper) { return; }
        const auto n = keeper->profileNames();
        if (profilePick < 0 || profilePick >= static_cast<int>(n.size())) { return; }
        switchName = n[static_cast<std::size_t>(profilePick)];
        switchPlan = keeper->planSwitch(switchName);
        if (!switchPlan.error.empty()) { say(formatText(tr("That profile cannot be used: %s"), switchPlan.error.c_str()), kErr); return; }
        sheet = SheetKind::SwitchConfirm;
    } else if (id == "sync-folder") {
        syncFolder = trimmed(text);
        rec.setStr("sync.folder", syncFolder);
    } else if (id == "sync") {
        if (!keeper) { return; }
        auto base = rec.getMap("sync.base");
        const SyncReport r = keeper->sync(pathOf(syncFolder), base);
        if (!r.ok) { say(formatText(tr("The sync did not run: %s"), r.error.c_str()), kErr); return; }
        rec.setMap("sync.base", base);
        say(formatText(tr("Sync: %d sent, %d received, %d unchanged, %d in conflict (left as they are)."), r.pushed, r.pulled, r.same, r.conflicts), r.conflicts > 0 ? kWarn : kGood);
    } else if (id == "wizard") {
        openWizard();
    } else if (id == "reset") {
        if (!keeper) { return; }
        resetOpts = {};
        sheet = SheetKind::ResetConfirm;
    } else if (id == "undo-reset") {
        if (!keeper) { return; }
        const auto st = keeper->resetStamps();
        if (st.empty()) { return; }
        undoStamp = st.front();
        sheet = SheetKind::UndoConfirm;
    } else if (id == "perf-mode") {
        rec.setInt("perf.mode", std::clamp(idx, 0, 2));
        lastSampleMs = -1e9;   // look again at once
    } else if (id == "palette") {
        const auto& ps = acc::palettes();
        paletteKey = ps[static_cast<std::size_t>(std::clamp(idx, 0, 3))].key;
        rec.setStr("access.palette", paletteKey);
        dirtyStyle = true;
    } else if (id == "numfmt") {
        rec.setInt("access.numfmt", std::clamp(idx, 0, 1)); applyNumbers();
    } else if (id == "digits") {
        rec.setInt("access.digits", std::clamp(idx, 0, 2)); applyNumbers();
    } else if (id == "speak-now") {
        speakerReady();
        if (!announcer) { announcer = std::make_unique<acc::Announcer>(speechPolicy(), symbols); }
        if (speaker) {
            const bool ok = acc::speakNow(*announcer, *speaker, spokenState(), env.nowMs ? env.nowMs() : 0.0);
            ++spokenCount;
            if (recording != nullptr && !recording->spoken.empty()) { lastSpoken = recording->spoken.back().text; }
            say(ok ? tr("Spoken.") : tr("Nothing could be spoken: no voice is available."), ok ? kGood : kWarn);
        } else { say(tr("Nothing could be spoken: no voice is available."), kWarn); }
    } else if (id.rfind("speech-", 0) == 0) {
        const double f = std::clamp(std::atof(text.c_str()), 0.0, 1.0);
        if (id == "speech-onchange") { rec.setBool("speech.onchange", text == "1"); if (text == "1") { speakerReady(); } }
        else if (id == "speech-start") { rec.setBool("speech.start", text == "1"); }
        else if (id == "speech-freq") { rec.setBool("speech.freq", text == "1"); }
        else if (id == "speech-mode") { rec.setBool("speech.mode", text == "1"); }
        else if (id == "speech-level") { rec.setBool("speech.level", text == "1"); }
        else if (id == "speech-interrupt") { rec.setBool("speech.interrupt", text == "1"); }
        else if (id == "speech-style") { rec.setInt("speech.style", std::clamp(idx, 0, 1)); }
        else if (id == "speech-interval") { rec.setInt("speech.interval", static_cast<long long>(std::lround(f * 60.0)) * 1000); }
        else if (id == "speech-settle") { rec.setInt("speech.settle", 200 + static_cast<long long>(std::lround(f * 4800.0 / 100.0)) * 100); }
        else if (id == "speech-minfreq") { rec.setNum("speech.minfreq", std::round(std::pow(10.0, f * 5.0))); }
        else if (id == "speech-step") { rec.setNum("speech.step", std::round(1.0 + f * 19.0)); }
        applySpeech();
    } else if (id == "tone-on") {
        toneOn = text == "1";
        toneNote.clear();
        if (toneOn && !env.bounded) { if (!tone.start()) { toneNote = tone.lastError(); toneOn = false; } }
        if (!toneOn) { tone.stop(); sonifier.reset(); }
        lastAction = toneOn ? "tone-on" : "tone-off";
    } else if (id.rfind("tone-", 0) == 0) {
        const double f = std::clamp(std::atof(text.c_str()), 0.0, 1.0);
        if (id == "tone-minhz") { rec.setNum("sonify.minhz", std::round(100.0 + f * 3900.0)); }
        else if (id == "tone-maxhz") { rec.setNum("sonify.maxhz", std::round(100.0 + f * 3900.0)); }
        else if (id == "tone-log") { rec.setBool("sonify.log", text == "1"); }
        else if (id == "tone-floor") { rec.setNum("sonify.floor", std::min(std::round(-160.0 + f * 160.0), rec.getNum("sonify.ceiling", -20.0, -160.0, 0.0) - 1.0)); }
        else if (id == "tone-ceiling") { rec.setNum("sonify.ceiling", std::max(std::round(-160.0 + f * 160.0), rec.getNum("sonify.floor", -120.0, -160.0, 0.0) + 1.0)); }
        else if (id == "tone-volume") { rec.setNum("sonify.volume", std::round(f * 50.0) / 100.0); }
        else if (id == "tone-mutesq") { rec.setBool("sonify.mutesq", text == "1"); }
        applySonify();
    } else if (id == "avg-mode") {
        avgMode = std::clamp(idx, 0, 4); rec.setInt("spec.avg", avgMode);
        for (auto& a : avg) { a.setMode(static_cast<cascade::dsp::AvgMode>(avgMode)); a.setFrameCount(static_cast<std::size_t>(avgFrames)); a.setTimeConstantS(avgTau); }
    } else if (id == "avg-frames") {
        avgFrames = 1 + static_cast<int>(std::lround(std::clamp(std::atof(text.c_str()), 0.0, 1.0) * 127.0)); rec.setInt("spec.avgframes", avgFrames);
        for (auto& a : avg) { a.setFrameCount(static_cast<std::size_t>(avgFrames)); }
    } else if (id == "avg-tau") {
        avgTau = 0.05 * std::pow(10.0 / 0.05, std::clamp(std::atof(text.c_str()), 0.0, 1.0)); rec.setNum("spec.avgtau", avgTau);
        for (auto& a : avg) { a.setTimeConstantS(avgTau); }
    } else if (id == "zoom-select") {
        zoomSelect = text == "1";
        if (env.ui != nullptr) { env.ui->global<SpectrumEvents>().set_select_mode(zoomSelect); }
    } else if (id == "grad-open") {
        sheet = SheetKind::Gradient;
    } else if (id.rfind("grad-", 0) == 0) {
        gradAct(id, text);
    } else if (id == "det-mode") {
        detector = std::clamp(idx, 0, 6); rec.setInt("spec.detector", detector);
    } else if (id == "mask-path") {
        maskPath = trimmed(text);
    } else if (id == "mask-example") {
        const fs::path f = pathOf(env.exportsDir) / "example-mask.txt";
        std::error_code ec;
        fs::create_directories(f.parent_path(), ec);
        std::ofstream o(f, std::ios::binary | std::ios::trunc);
        o << "foxmask 1\n# an example: nothing above -20 dB across the FM broadcast band, nothing below -110 dB\nupper 87.5e6 -20 108e6 -20\nlower 87.5e6 -110 108e6 -110\n";
        o.flush();
        if (o) { maskPath = f.string(); maskNote = tr("An example mask was written; load it, then edit the file to taste."); } else { maskNote = tr("The example mask could not be written."); }
    } else if (id == "mask-load") {
        std::ifstream f(pathOf(maskPath), std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string err;
        if (!f || body.empty()) { maskLoaded = false; maskNote = tr("The mask file cannot be read."); }
        else if (!parseMaskText(body, mask, err)) { maskLoaded = false; maskNote = formatText(tr("That is not a usable mask: %s"), err.c_str()); }
        else { maskLoaded = true; maskShown = true; maskNote = formatText(tr("Loaded: %d line(s)."), static_cast<int>(mask.segments.size())); evalCentre = -1; maskBreachFrames = 0; }
    } else if (id == "mask-show") {
        maskShown = text == "1" && maskLoaded;
    } else if (id == "cur-open") {
        sheet = SheetKind::Cursors;
    } else if (id == "cur-show") {
        cursorsShown = text == "1";
    } else if (id.rfind("cur-", 0) == 0) {
        cursorAct(id, text);
    } else if (id == "community-forum") {
        if (!env.bounded) { openUrlNow(forumUrl()); }
        lastAction = "community-forum";
    } else if (id == "community-discord") {
        if (!env.bounded) { openUrlNow(discordUrl()); }
        lastAction = "community-discord";
    }
}

void LaneKPublisher::Impl::gradAct(const std::string& id, const std::string& text) {
    namespace gr = cascade::core::gradient;
    const double v = std::atof(text.c_str());
    if (id == "grad-on") { gradOn = text == "1"; }
    else if (id == "grad-interp") { grad.setInterp(static_cast<gr::Interp>(std::clamp(std::atoi(text.c_str()), 0, 2))); }
    else if (id == "grad-add") { grad.addStop(0.5, gr::Rgb8{128, 128, 128}); }
    else if (id == "grad-reverse") { grad.reverse(); }
    else if (id == "grad-reset") { grad.reset(); }
    else if (id.rfind("grad-col-", 0) == 0) {
        const std::size_t i = static_cast<std::size_t>(std::atoi(id.c_str() + 9));
        std::string h = trimmed(text);
        if (!h.empty() && h[0] == '#') { h.erase(0, 1); }
        if (h.size() == 6 && h.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
            const unsigned long x = std::strtoul(h.c_str(), nullptr, 16);
            grad.setColour(i, gr::Rgb8{static_cast<std::uint8_t>((x >> 16) & 255), static_cast<std::uint8_t>((x >> 8) & 255), static_cast<std::uint8_t>(x & 255)});
        }
    } else if (id.rfind("grad-pos-", 0) == 0) {
        grad.moveStop(static_cast<std::size_t>(std::atoi(id.c_str() + 9)), v);
    } else if (id.rfind("grad-del-", 0) == 0) {
        grad.removeStop(static_cast<std::size_t>(std::atoi(id.c_str() + 9)));
    }
    const auto table = gr::toTable(grad.gradient());
    gradTable.clear();
    for (const auto& c : table) { gradTable.push_back(foxsdr::slinthost::Rgba{c.r, c.g, c.b, 255}); }
    gradChanged = true;
}

void LaneKPublisher::Impl::cursorAct(const std::string& id, const std::string& text) {
    const double v = std::atof(text.c_str());
    if (id == "cur-mode") { cursors.setMode(static_cast<cascade::sigmeas::CursorMode>(std::clamp(std::atoi(text.c_str()), 0, 2))); }
    else if (id == "cur-spacing") { cursors.setSpacing(v * 1e3); }
    else if (id == "cur-ref") { cursorRef = std::clamp(std::atoi(text.c_str()), 0, 7); cursors.setReference(static_cast<std::size_t>(cursorRef)); }
    else if (id == "cur-pick") { cursorPick = text == "1"; cursorNext = 0; }
    else if (id == "cur-tuned") { cursors.setCursor(0, lastState.tunedHz); cursorsShown = true; }
    else if (id == "cur-snap") { cursorNote = snapCursors() ? std::string(tr("Snapped.")) : std::string(tr("Nothing to snap to yet.")); }
    else if (id.rfind("cur-f-", 0) == 0) { cursors.setCursor(static_cast<std::size_t>(std::atoi(id.c_str() + 6)), v * 1e6); cursorsShown = true; }
}

bool LaneKPublisher::Impl::snapCursors() {
    if (frameN < 4) { return false; }
    bool any = false;
    for (std::size_t i = 0; i < 8; ++i) {
        const double f = cursors.frequency(i);
        const double binHz = frameSpan / static_cast<double>(frameN);
        const long c = static_cast<long>(std::floor((f - (frameCentre - frameSpan / 2.0)) / binHz));
        if (c < 0 || c >= static_cast<long>(frameN)) { continue; }
        const long a = std::max(0L, c - 20), b = std::min(static_cast<long>(frameN) - 1, c + 20);
        long best = c;
        for (long k = a; k <= b; ++k) { if (frameCopy[static_cast<std::size_t>(k)] > frameCopy[static_cast<std::size_t>(best)]) { best = k; } }
        cursors.setCursor(i, frameCentre - frameSpan / 2.0 + (static_cast<double>(best) + 0.5) * binHz);
        any = true;
    }
    return any;
}

void LaneKPublisher::Impl::actSheet(const std::string& id, const std::string& text) {
    const int idx = std::atoi(text.c_str());
    const std::string stamp = stampOf(env.unixNow ? env.unixNow() : 0);
    if (id == "sheet-dismiss") {
        if (sheet == SheetKind::Wizard && wizard) { wizard->skip(); finishWizard(); return; }
        if (sheet != SheetKind::Closing) { sheet = SheetKind::None; }
        return;
    }
    if (id == "sheet-reset-plugins") { resetOpts.includePlugins = text == "1"; return; }
    if (id == "sheet-reset-logs") { resetOpts.includeLogs = text == "1"; return; }
    if (id == "sheet-go") {
        Pending p;
        p.stamp = stamp;
        if (sheet == SheetKind::ImportConfirm) {
            p.op = Op::Import; p.token = importPreview.plan.confirmToken; p.staged = importPreview.res; p.what = "import";
        } else if (sheet == SheetKind::SwitchConfirm) {
            p.op = Op::Switch; p.token = switchPlan.plan.confirmToken; p.sw = switchPlan; p.profile = switchName; p.what = "switch";
        } else if (sheet == SheetKind::ResetConfirm) {
            const auto plan = keeper->planReset(resetOpts);
            if (!plan.error.empty() || plan.items.empty()) { return; }
            p.op = Op::Reset; p.token = plan.confirmToken; p.reset = resetOpts; p.what = "reset";
        } else if (sheet == SheetKind::UndoConfirm) {
            p.op = Op::UndoReset; p.undoStamp = undoStamp; p.what = "undo";
        } else { return; }
        beginPending(std::move(p));
        return;
    }
    // the wizard
    if (!wizard) { return; }
    auto& w = *wizard;
    if (id == "wiz-skip") { w.skip(); finishWizard(); return; }
    if (id == "wiz-back") { w.back(); return; }
    if (id == "wiz-radio") {
        wizardRadio = idx;
        const auto& cs = sys::radioChoices();
        if (idx >= 0 && idx < static_cast<int>(cs.size())) { w.chooseRadio(cs[static_cast<std::size_t>(idx)].family); wizardPreset = 0; w.choosePreset(""); }
        return;
    }
    if (id == "wiz-preset") {
        wizardPreset = idx;
        const auto offered = w.offered();
        w.choosePreset(idx >= 1 && idx <= static_cast<int>(offered.size()) ? offered[static_cast<std::size_t>(idx - 1)].id : std::string());
        return;
    }
    if (id == "wiz-telemetry") { wizardTelemetry = text == "1"; w.chooseTelemetry(wizardTelemetry); return; }
    if (id == "wiz-next") {
        if (w.step() == sys::WizardStep::Privacy) { w.chooseTelemetry(wizardTelemetry); }
        w.next();
        if (w.finished()) { finishWizard(); }
    }
}

void LaneKPublisher::Impl::actMatrix(const std::string& id, const std::string& text) {
    const int radio = std::atoi(text.c_str());   // 1-based, 0 = none
    lastAction = "matrix-" + id;
    if (id == "hover") { hoverId = radio - 1; }
    else if (id == "leave") { if (hoverId == radio - 1 || radio == 0) { hoverId = -1; } }
    else if (id == "listen") {
        // a click keeps that radio: it becomes the baseline the hover returns to (a second click on the kept radio lets it go)
        const int already = baseline;
        baseline = (already == radio - 1) ? -1 : radio - 1;
        hover.setBaseline(baseline);
        FoxCommand c = op(FOXAPI_OP_RADIO_SOLO); c.ival[0] = baseline + 1; submit(c);
        matrixListen = baseline + 1;
    } else if (id == "select") {
        FoxCommand c = op(FOXAPI_OP_RADIO_FOCUS); c.ival[0] = radio; submit(c);
    }
}

// ============================================================ the tick ============================================================
void LaneKPublisher::tick(const FoxReceiverState& state, long tickNo) {
    Impl& I = *p_;
    I.lastState = state;
    if (I.env.ui == nullptr) { return; }
    const double nowMs = I.env.nowMs ? I.env.nowMs() : 0.0;
    I.lastNowS = nowMs / 1000.0;
    auto& g = I.env.ui->global<LaneK>();

    if (I.wizardAsked && I.sheet == SheetKind::None && tickNo > 5) { I.wizardAsked = false; I.openWizard(); }

    // performance: once a second, and only when somebody can use it (a mode other than Off needs the policy; the readouts need it too)
    I.samplePerf(nowMs);
    if (I.dirtyStyle) { I.restyle(); }

    // the Matrix: the table, and the hover-to-listen policy
    auto& m = I.env.ui->global<LaneKMatrix>();
    const bool open = m.get_open();
    if (open) {
        if (!I.matrixWasOpen) {
            const int solo = I.env.soloRadio ? I.env.soloRadio() : 0;
            I.baseline = solo - 1;
            I.hover.setBaseline(I.baseline);
            I.hover.reset();
            I.hoverId = -1;
        }
        const auto inputs = I.env.matrix ? I.env.matrix() : std::vector<MatrixInput>();
        cc::RadioMatrix matrix;
        std::uint32_t mutedMask = 0;
        std::vector<KTile> tiles;
        const int heard = I.hover.hovering() >= 0 ? I.hover.hovering() : I.baseline;
        for (const auto& in : inputs) {
            if (in.slot < 0 || in.slot >= cc::kMatrixMaxRadios) { continue; }
            cc::RadioSummary r;
            r.id = in.slot;
            r.state = in.running ? cc::MatrixRadioState::Running : cc::MatrixRadioState::Closed;
            r.tunedHz = in.tunedHz;
            r.modeCode = static_cast<int>(in.demod);
            r.levelDb = static_cast<float>(in.signalDb);
            r.muted = in.muted;
            r.listening = in.slot == heard;
            matrix.update(r);
            if (in.muted) { mutedMask |= 1u << in.slot; }
        }
        for (const auto& r : matrix.rows()) {
            KTile t;
            t.radio = r.id + 1;
            t.name = ss(formatText(tr("Radio %d"), r.id + 1));
            t.freq = ss(r.tunedHz > 0 ? num("%.4f MHz", r.tunedHz / 1e6) : std::string("-"));
            t.mode = ss(foxsdr::slinthost::modeName(static_cast<std::uint32_t>(r.modeCode)));
            const auto it = std::find_if(inputs.begin(), inputs.end(), [&](const MatrixInput& x) { return x.slot == r.id; });
            t.level = it != inputs.end() ? static_cast<float>(it->sMeter) : 0.0f;
            t.level_text = ss(num("%.0f dB", r.levelDb));
            t.state = ss(r.muted ? std::string(tr("muted")) : std::string(cc::matrixRadioStateText(r.state) == std::string("running") ? tr("running") : tr("stopped")));
            t.heard = r.listening;
            t.follows = ss("");
            tiles.push_back(std::move(t));
        }
        const std::string sig = std::to_string(tiles.size()) + [&] { std::string s; for (const auto& t : tiles) { s += std::string(t.freq) + std::string(t.mode) + std::string(t.level_text) + std::string(t.state) + (t.heard ? "H" : "-") + num("%.2f", t.level); } return s; }();
        if (sig != I.sigTiles) {
            I.sigTiles = sig;
            while (I.mTiles->row_count() > 0) { I.mTiles->erase(0); }
            for (auto& t : tiles) { I.mTiles->push_back(t); }
        }
        m.set_note(ss(tiles.size() < 2 ? std::string(tr("Open a second radio to compare receivers here.")) : std::string(tr("Point at a radio to hear it. Click to keep it; click again to let it go. Double-click selects it."))));
        const cc::HoverDecision d = I.hover.update(I.hoverId, mutedMask, I.lastNowS);
        if (d.changed) {
            FoxCommand c = I.op(FOXAPI_OP_RADIO_SOLO);
            c.ival[0] = d.listenId >= 0 ? d.listenId + 1 : 0;
            I.submit(c);
            I.matrixListen = static_cast<int>(c.ival[0]);
        }
    } else if (I.matrixWasOpen) {
        // the sheet closed: the sound goes back to what it was before the pointer moved over it
        I.hover.reset();
        I.hoverId = -1;
        FoxCommand c = I.op(FOXAPI_OP_RADIO_SOLO);
        c.ival[0] = I.baseline + 1;
        I.submit(c);
        I.matrixListen = I.baseline + 1;
    }
    I.matrixWasOpen = open;

    // the rows: rebuilt only when their signature moved; the performance rows move every second
    const auto publish = [&](std::shared_ptr<slint::VectorModel<KRow>>& model, std::string& sig, const std::vector<Row>& rows) {
        const std::string s = signatureOf(rows);
        if (s == sig) { return; }
        sig = s;
        std::vector<KRow> v;
        for (const auto& r : rows) { v.push_back(toK(r)); }
        while (model->row_count() > 0) { model->erase(0); }
        for (auto& k : v) { model->push_back(std::move(k)); }
    };
    if (tickNo % 3 == 0 || tickNo < 4) {
        I.refreshBundles();
        publish(I.mBackup, I.sigBackup, I.backupRows());
        publish(I.mPerf, I.sigPerf, I.perfRows());
        publish(I.mAccess, I.sigAccess, I.accessRows());
        publish(I.mCommunity, I.sigCommunity, I.communityRows());
        publish(I.mDisplay, I.sigDisplay, I.displayRows());
        publish(I.mSpectrum, I.sigSpectrum, I.spectrumRows());
    }
    std::string title;
    int width = 460;
    const auto sheetRows = I.sheetRows(title, width);
    publish(I.mSheet, I.sigSheet, sheetRows);
    g.set_sheet_title(ss(title));
    g.set_sheet_width(width);
    g.set_sheet_open(I.sheet != SheetKind::None);

    // spoken changes and the tone (each does nothing unless the user switched it on)
    if (I.rec.getBool("speech.onchange", false)) {
        I.speakerReady();
        if (!I.announcer) { I.announcer = std::make_unique<acc::Announcer>(I.speechPolicy(), I.symbols); }
        if (I.speaker) {
            if (acc::speakIfDue(*I.announcer, *I.speaker, I.spokenState(), nowMs)) {
                ++I.spokenCount;
                if (I.recording != nullptr && !I.recording->spoken.empty()) { I.lastSpoken = I.recording->spoken.back().text; }
            }
        }
    }
    if (I.toneOn) {
        const double dt = I.lastToneMs > 0 ? std::clamp(nowMs - I.lastToneMs, 1.0, 200.0) : 33.0;
        I.lastToneMs = nowMs;
        const double half = std::max(1.0, state.sampleRateHz / 2.0);
        const float offset = static_cast<float>(std::clamp((state.tunedHz - state.centreHz) / half, -1.0, 1.0));
        I.toneParams = I.sonifier.update(static_cast<float>(state.signalDb), (state.flags & FOXAPI_RX_SQUELCH_OPEN) != 0 || state.squelchDb < -190.0, offset, static_cast<float>(dt));
        if (!I.env.bounded) { I.tone.feed(I.toneParams); }
    }
    if (tickNo % 15 == 7) { I.publishSr(); }
    if (I.rec.dirty() && tickNo - I.lastSaveTick > 30) { I.lastSaveTick = tickNo; I.save(); }
}

void LaneKPublisher::flush() { if (p_->rec.dirty()) { p_->save(); } }

std::string LaneKPublisher::forumUrl() const { return p_->forumUrl(); }
std::string LaneKPublisher::discordUrl() const { return p_->discordUrl(); }

void LaneKPublisher::openCommunity(bool discord) {
    Impl& I = *p_;
    const std::string u = discord ? I.discordUrl() : I.forumUrl();
    I.lastAction = discord ? "community-discord" : "community-forum";
    if (!u.empty() && !I.env.bounded) { openUrlNow(u); }
}

LaneKPublisher::TraceSource LaneKPublisher::traceSource(int slot, const float* full, std::size_t fullN, std::size_t b0, std::size_t subN, double nowS, std::uint64_t key, std::size_t columns) {
    Impl& I = *p_;
    TraceSource t;
    t.data = full + b0;
    t.n = subN;
    if (I.avgMode != 0 && slot >= 0 && slot < 8) {
        auto& a = I.avg[static_cast<std::size_t>(slot)];
        if (static_cast<int>(a.mode()) != I.avgMode) { a.setMode(static_cast<cascade::dsp::AvgMode>(I.avgMode)); a.setFrameCount(static_cast<std::size_t>(I.avgFrames)); a.setTimeConstantS(I.avgTau); }
        a.push(full, static_cast<int>(fullN), nowS, key);
        const auto& tr2 = a.trace();
        if (tr2.size() == fullN && b0 + subN <= fullN) { t.data = tr2.data() + b0; }
    }
    if (I.detector > 0 && columns > 0 && subN > 0) {
        I.detScratch.resize(columns);
        detectorColumns(t.data, subN, columns, static_cast<cascade::dsp::RbwDetector>(I.detector - 1), I.detScratch.data());
        t.data = I.detScratch.data();
        t.n = columns;
    }
    return t;
}

void LaneKPublisher::frameSeen(int slot, bool selected, const float* full, std::size_t n, double centreHz, double spanHz, double sampleRateHz) {
    Impl& I = *p_;
    if (!selected || full == nullptr || n < 4) { return; }
    I.frameCopy.assign(full, full + n);
    I.frameN = n; I.frameCentre = centreHz; I.frameSpan = spanHz;
    I.lastRbwHz = nativeRbwHz(sampleRateHz, n);
    for (std::size_t i = 0; i < 8; ++i) { I.cursorLevelDb[i] = I.cursorsShown || I.sheet == SheetKind::Cursors ? cursorLevel(full, n, centreHz, spanHz, I.cursors.frequency(i), spanHz / static_cast<double>(n) * 2.0) : std::numeric_limits<double>::quiet_NaN(); }
    if (I.maskLoaded) {
        if (I.evalCentre != centreHz || I.evalSpan != spanHz || I.evalBins != n) { I.maskEval.prepare(I.mask, centreHz, spanHz, n); I.evalCentre = centreHz; I.evalSpan = spanHz; I.evalBins = n; }
        cascade::core::MaskResult r;
        if (I.maskEval.evaluate(full, n, r) && r.anyLimited) {
            I.maskHaveResult = true; I.maskPass = r.pass; I.maskMargin = r.worstMarginDb; I.maskWorstHz = r.worstFreqHz;
            if (!r.pass) { ++I.maskBreachFrames; }
        }
    }
    (void)slot;
}

void LaneKPublisher::paintOver(foxsdr::slinthost::Rgba* trace, std::uint32_t w, std::uint32_t h, double loHz, double hiHz, double dbMin, double dbMax) {
    Impl& I = *p_;
    if (I.maskLoaded && I.maskShown) { paintMask(trace, w, h, loHz, hiHz, dbMin, dbMax, I.mask); }
    if (I.cursorsShown || I.sheet == SheetKind::Cursors) {
        std::array<double, 8> f{};
        std::array<bool, 8> shown{};
        for (std::size_t i = 0; i < 8; ++i) { f[i] = I.cursors.frequency(i); shown[i] = true; }
        paintCursors(trace, w, h, loHz, hiHz, f, shown);
    }
}

const foxsdr::slinthost::Rgba* LaneKPublisher::gradientTable() const { return (p_->gradOn && p_->gradTable.size() == 256) ? p_->gradTable.data() : nullptr; }

bool LaneKPublisher::takeGradientChanged() { const bool c = p_->gradChanged; p_->gradChanged = false; return c; }

bool LaneKPublisher::placeCursorAt(double hz) {
    Impl& I = *p_;
    if (!I.cursorPick || I.sheet != SheetKind::Cursors) { return false; }
    I.cursors.setCursor(static_cast<std::size_t>(I.cursorNext), hz);
    I.cursorNext = (I.cursorNext + 1) % 8;
    I.cursorsShown = true;
    I.lastAction = "cursor-placed";
    return true;
}

bool LaneKPublisher::overlayOpen() const { return p_->sheet != SheetKind::None || (p_->env.ui != nullptr && p_->env.ui->global<LaneKMatrix>().get_open()); }

void LaneKPublisher::closeOverlays() {
    Impl& I = *p_;
    if (I.sheet == SheetKind::Wizard && I.wizard) { I.wizard->skip(); I.finishWizard(); }
    else if (I.sheet != SheetKind::Closing) { I.sheet = SheetKind::None; }
    if (I.env.ui != nullptr) { I.env.ui->global<LaneKMatrix>().set_open(false); }
}

void LaneKPublisher::restyle() { p_->restyle(); }
bool LaneKPublisher::reducedEffects() const { return p_->reduced; }
int LaneKPublisher::waterfallRowsPerSecondCap() const { return p_->reducedRows; }

std::string LaneKPublisher::finishAtExit() {
    Impl& I = *p_;
    std::string out;
    if (I.rec.dirty()) { I.save(); }
    if (I.keeper && I.rec.getBool("backup.auto", false) && I.pending.op == Op::None) {
        const ExportOut o = I.keeper->backupNow(stampOf(I.env.unixNow ? I.env.unixNow() : 0), I.env.version, static_cast<int>(I.rec.getInt("backup.keep", 10, 1, 50)));
        out += o.ok ? "backup ok; " : "backup failed (" + o.error + "); ";
    }
    if (I.keeper && I.pending.op != Op::None) {
        std::string detail;
        const bool ok = I.keeper->apply(I.pending, detail);
        out += "pending " + I.pending.what + (ok ? " applied" : " FAILED: " + detail);
    }
    return out;
}

std::string LaneKPublisher::readState(const std::string& w, std::string& out) const {
    const Impl& I = *p_;
    if (w == "action") { out = I.lastAction; }
    else if (w == "status") { out = I.status; }
    else if (w == "sheet") { out = std::to_string(static_cast<int>(I.sheet)); }
    else if (w == "pending") { out = std::to_string(static_cast<int>(I.pending.op)); }
    else if (w == "palette") { out = I.paletteKey; }
    else if (w == "reduced") { out = I.reduced ? "1" : "0"; }
    else if (w == "perfmode") { out = std::to_string(I.rec.getInt("perf.mode", 0, 0, 2)); }
    else if (w == "samplems") { out = num("%.3f", I.sampleCostMs); }
    else if (w == "samples") { out = std::to_string(I.sampleCount); }
    else if (w == "cpu") { out = I.haveSnap && I.snap.processCpuPercent.available ? num("%.1f", I.snap.processCpuPercent.value) : "-"; }
    else if (w == "mem") { out = I.haveSnap && I.snap.workingSetBytes.available ? num("%.1f", static_cast<double>(I.snap.workingSetBytes.value) / 1048576.0) : "-"; }
    else if (w == "spoken") { out = I.lastSpoken; }
    else if (w == "spokencount") { out = std::to_string(I.spokenCount); }
    else if (w == "toneon") { out = I.toneOn ? "1" : "0"; }
    else if (w == "tonehz") { out = num("%.1f", I.toneParams.freqHz); }
    else if (w == "toneaudible") { out = I.toneParams.audible ? "1" : "0"; }
    else if (w == "numfmt") { out = std::to_string(I.numFormat); }
    else if (w == "forumurl") { out = I.forumUrl(); }
    else if (w == "discordurl") { out = I.discordUrl(); }
    else if (w == "sr.spectrum") { out = I.srSpectrum; }
    else if (w == "sr.waterfall") { out = I.srWaterfall; }
    else if (w == "sr.counter") { out = I.srCounter; }
    else if (w == "avgmode") { out = std::to_string(I.avgMode); }
    else if (w == "avgframes") { out = std::to_string(I.avgFrames); }
    else if (w == "zoomselect") { out = I.zoomSelect ? "1" : "0"; }
    else if (w == "detector") { out = std::to_string(I.detector); }
    else if (w == "rbw") { out = num("%.3f", I.lastRbwHz); }
    else if (w == "maskloaded") { out = I.maskLoaded ? "1" : "0"; }
    else if (w == "maskpass") { out = I.maskHaveResult ? (I.maskPass ? "1" : "0") : "-"; }
    else if (w == "maskmargin") { out = I.maskHaveResult ? num("%.2f", I.maskMargin) : "-"; }
    else if (w == "maskbreaches") { out = std::to_string(I.maskBreachFrames); }
    else if (w == "gradon") { out = I.gradOn ? "1" : "0"; }
    else if (w == "gradstops") { out = std::to_string(I.grad.size()); }
    else if (w == "gradtop") { out = I.gradTable.size() == 256 ? num("%.0f", I.gradTable[255].r * 65536.0 + I.gradTable[255].g * 256.0 + I.gradTable[255].b) : "-"; }
    else if (w == "cursorshown") { out = I.cursorsShown ? "1" : "0"; }
    else if (w.rfind("cursor.", 0) == 0) { out = num("%.0f", I.cursors.frequency(static_cast<std::size_t>(std::clamp(std::atoi(w.c_str() + 7), 0, 7)))); }
    else if (w == "cursormode") { out = std::to_string(static_cast<int>(I.cursors.mode())); }
    else if (w == "matrixsolo") { out = std::to_string(I.matrixListen); }
    else if (w == "baseline") { out = std::to_string(I.baseline + 1); }
    else if (w == "hovering") { out = std::to_string(I.hover.hovering() + 1); }
    else if (w == "tiles") { out = std::to_string(I.mTiles ? I.mTiles->row_count() : 0); }
    else if (w == "wizardstep") { out = I.wizard ? std::to_string(static_cast<int>(I.wizard->step())) : "-"; }
    else if (w == "importpath") { out = I.importPath; }
    else if (w == "exports") { out = std::to_string(I.bundleNames.size()); }
    else if (w == "profiles") { out = std::to_string(I.keeper ? I.keeper->profileNames().size() : 0); }
    else { return "unknown lk state: " + w; }
    return std::string();
}

}  // namespace foxsdr::slintlanek
