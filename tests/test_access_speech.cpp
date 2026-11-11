// Tests for core/access_speech.hpp and access_speech_sapi.*: spoken state.
//
// Part 1: phrases, S-units, the announcement policy (on demand, on change, the
// rate limit, the settle rule, no stale values) against the recording double.
// Part 2 (Windows): the real SAPI voice, with its output attached to a MEMORY
// stream only. The test checks that the output IS the memory stream, that audio
// bytes come back, that they are not silence and that longer text takes longer.
// NOTHING IS PLAYED: the class refuses to speak unless the memory output is
// confirmed, and a source scan below pins that no audio-device API is referenced.
// Nobody listens to the result and no screen reader is run: UNVERIFIED as speech
// a person understands.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/access_speech.hpp"
#include "core/access_speech_sapi.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "test_check.hpp"

namespace {

using namespace cascade::core::access;

NumberSymbols en() { return NumberSymbols{}; }
NumberSymbols de() { NumberSymbols s; s.name = "de-DE"; s.decimal = ","; s.group = "."; return s; }
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

void frequencyPhrases() {
    CHECK(frequencyPhrase(7.074e6, en()) == "7.074 megahertz");
    CHECK(frequencyPhrase(7.074e6, de()) == "7,074 megahertz");
    CHECK(frequencyPhrase(145.5e6, en()) == "145.5 megahertz");
    CHECK(frequencyPhrase(7.0e6, en()) == "7 megahertz");
    CHECK(frequencyPhrase(10.0e6, en()) == "10 megahertz");
    CHECK(frequencyPhrase(7.074125e6, en()) == "7.074125 megahertz");   // down to 1 Hz
    CHECK(frequencyPhrase(1296.2e6, en()) == "1.2962 gigahertz");
    CHECK(frequencyPhrase(198.0e3, en()) == "198 kilohertz");
    CHECK(frequencyPhrase(1.0e3, en()) == "1 kilohertz");
    CHECK(frequencyPhrase(999.0, en()) == "999 hertz");
    CHECK(frequencyPhrase(0.0, en()) == "0 hertz");
    CHECK(frequencyPhrase(kNaN, en()).empty());
    CHECK(frequencyPhrase(-5.0, en()).empty());
    CHECK(frequencyPhrase(INFINITY, en()).empty());
    // A translated table is used.
    PhraseTable t; t.megahertz = "{n} Megahertz";
    CHECK(frequencyPhrase(7.074e6, de(), t) == "7,074 Megahertz");
}

void modePhrases() {
    CHECK(modePhrase("USB") == "upper sideband");
    CHECK(modePhrase("lsb") == "lower sideband");
    CHECK(modePhrase("NFM") == "narrow F M");
    CHECK(modePhrase("CW") == "C W");
    CHECK(modePhrase("FT8") == "FT8");        // not in the table: as written
    CHECK(modePhrase("").empty());
}

void sUnits() {
    // HF: S9 is -73 dBm, 6 dB per unit.
    CHECK(sUnitsFromDbm(-73.0, 7e6).s == 9 && sUnitsFromDbm(-73.0, 7e6).overS9Db == 0);
    CHECK(sUnitsFromDbm(-79.0, 7e6).s == 8);
    CHECK(sUnitsFromDbm(-97.0, 7e6).s == 5);
    CHECK(sUnitsFromDbm(-121.0, 7e6).s == 1);          // the table's S1
    CHECK(sUnitsFromDbm(-140.0, 7e6).s == 0);          // clamped, never negative
    CHECK(sUnitsFromDbm(-63.0, 7e6).s == 9 && sUnitsFromDbm(-63.0, 7e6).overS9Db == 10);
    CHECK(sUnitsFromDbm(-53.0, 7e6).overS9Db == 20);
    CHECK(sUnitsFromDbm(-70.0, 7e6).overS9Db == 0);    // 3 dB over S9 is still plain S9
    CHECK(sUnitsFromDbm(-20.0, 7e6).overS9Db == 50);
    // VHF/UHF: S9 is -93 dBm.
    CHECK(sUnitsFromDbm(-93.0, 145e6).s == 9);
    CHECK(sUnitsFromDbm(-99.0, 145e6).s == 8);
    CHECK(sUnitsFromDbm(-73.0, 145e6).overS9Db == 20);
    CHECK(sUnitsFromDbm(-73.0, 29.999e6).s == 9);
    CHECK(sUnitsFromDbm(-73.0, 30e6).overS9Db == 20);
    CHECK(sUnitsFromDbm(-73.0, kNaN).s == 9);          // unknown frequency: HF reference

    CHECK(levelPhrase(-79.0, kNaN, 7e6, LevelStyle::SUnits, en()) == "S 8");
    CHECK(levelPhrase(-53.0, kNaN, 7e6, LevelStyle::SUnits, en()) == "S 9 plus 20 decibels");
    CHECK(levelPhrase(-82.0, kNaN, 7e6, LevelStyle::Decibels, en()) == "minus 82 decibel milliwatts");
    CHECK(levelPhrase(kNaN, -63.0, 7e6, LevelStyle::SUnits, en()) == "level minus 63 decibels");   // uncalibrated: never S-units
    CHECK(levelPhrase(kNaN, -63.0, 7e6, LevelStyle::Decibels, en()) == "level minus 63 decibels");
    CHECK(levelPhrase(kNaN, 6.0, 7e6, LevelStyle::Decibels, en()) == "level 6 decibels");
    CHECK(levelPhrase(kNaN, -0.2, 7e6, LevelStyle::Decibels, en()) == "level 0 decibels");           // no "minus 0"
    CHECK(levelPhrase(kNaN, kNaN, 7e6, LevelStyle::SUnits, en()).empty());
}

SpokenState state(double hz, const char* mode, double dbm, bool squelchOpen = true, bool hasSq = false) {
    SpokenState s; s.freqHz = hz; s.mode = mode; s.levelDbm = dbm; s.hasSquelch = hasSq; s.squelchOpen = squelchOpen;
    return s;
}

void onDemand() {
    AnnouncePolicy p;     // defaults: on demand only
    Announcer a(p, en());
    const Utterance u = a.onDemand(state(7.074e6, "USB", -79.0), 1000.0);
    CHECK(u.text == "7.074 megahertz, upper sideband, S 8");
    CHECK(u.interrupt);
    // Parts can be switched off.
    p.speakMode = false; p.speakLevel = false;
    Announcer b(p, en());
    CHECK(b.onDemand(state(7.074e6, "USB", -79.0), 0.0).text == "7.074 megahertz");
    p.speakFrequency = false; p.speakMode = true;
    Announcer c(p, en());
    CHECK(c.onDemand(state(7.074e6, "USB", -79.0), 0.0).text == "upper sideband");
    // Nothing known: says so, never silence.
    Announcer d(AnnouncePolicy{}, en());
    CHECK(d.onDemand(SpokenState{}, 0.0).text == "no signal");
    // German numbers.
    Announcer g(AnnouncePolicy{}, de());
    CHECK(g.onDemand(state(7.074e6, "AM", kNaN), 0.0).text == "7,074 megahertz, A M");
    // On demand ignores the interval entirely.
    CHECK(!a.onDemand(state(7.074e6, "USB", -79.0), 1000.5).text.empty());
    // Squelch closed: "no signal" in place of the level.
    SpokenState closed = state(7.074e6, "USB", -100.0, false, true);
    CHECK(a.onDemand(closed, 5000.0).text == "7.074 megahertz, upper sideband, no signal");
}

void onChangeOff() {
    Announcer a(AnnouncePolicy{}, en());          // onChange = false
    CHECK(!a.update(state(7.0e6, "USB", -80.0), 0.0).has_value());
    CHECK(!a.update(state(14.0e6, "LSB", -50.0), 99999.0).has_value());
}

void onChangeRules() {
    AnnouncePolicy p; p.onChange = true;
    Announcer a(p, en());
    // Baseline: silent.
    CHECK(!a.update(state(7.074e6, "USB", -79.0), 0.0).has_value());
    CHECK(!a.update(state(7.074e6, "USB", -79.0), 5000.0).has_value());     // unchanged: silent
    // A tiny move (below minFreqChangeHz) is not announced.
    CHECK(!a.update(state(7.074e6 + 50.0, "USB", -79.0), 10000.0).has_value());
    // The dial turns through many steps: ONE announcement, of the final value,
    // only after it has rested for the settle time.
    double now = 20000.0;
    int utterances = 0;
    std::string last;
    for (int i = 1; i <= 50; ++i) {
        now += 100.0;                                                         // 100 ms per click
        const auto u = a.update(state(7.074e6 + 5000.0 * i, "USB", -79.0), now);
        if (u) { ++utterances; last = u->text; }
    }
    CHECK(utterances == 0);                                                    // still moving: silent
    for (int i = 0; i < 20; ++i) {
        now += 100.0;
        const auto u = a.update(state(7.074e6 + 5000.0 * 50, "USB", -79.0), now);
        if (u) { ++utterances; last = u->text; }
    }
    CHECK(utterances == 1);
    CHECK(last == "7.324 megahertz, upper sideband");                         // final value, with the mode as context
    // Nothing more once spoken.
    CHECK(!a.update(state(7.324e6, "USB", -79.0), now + 60000.0).has_value());

    // A mode change alone is spoken alone, at once.
    const auto m = a.update(state(7.324e6, "LSB", -79.0), now + 61000.0);
    CHECK(m.has_value() && m->text == "lower sideband");

    // A level change below one S-unit is not spoken; at or above it is.
    CHECK(!a.update(state(7.324e6, "LSB", -75.0), now + 70000.0).has_value());
    const auto l = a.update(state(7.324e6, "LSB", -67.0), now + 71000.0);     // 12 dB up from the spoken -79
    CHECK(l.has_value() && l->text == "S 9 plus 10 decibels");
    // And it measures from the last SPOKEN value, not the last seen: creeping up
    // 1 dB a poll eventually crosses.
    double dbm = -67.0;
    int spokenCreep = 0;
    for (int i = 0; i < 40; ++i) {
        dbm -= 1.0;
        if (a.update(state(7.324e6, "LSB", dbm), now + 80000.0 + 4000.0 * i)) { ++spokenCreep; }
    }
    CHECK(spokenCreep >= 5 && spokenCreep <= 7);                              // 40 dB / 6 dB per announcement
}

void rateLimitAndFreshness() {
    AnnouncePolicy p; p.onChange = true; p.minIntervalMs = 3000.0;
    Announcer a(p, en());
    a.update(state(7.0e6, "USB", -79.0), 0.0);                                   // baseline: not an utterance
    const auto first = a.update(state(7.0e6, "LSB", -79.0), 1000.0);              // first change: spoken at once
    CHECK(first.has_value() && first->text == "lower sideband");
    // Within 3000 ms of that utterance a further change is held...
    CHECK(!a.update(state(7.0e6, "AM", -79.0), 1500.0).has_value());
    CHECK(!a.update(state(7.0e6, "AM", -79.0), 3900.0).has_value());
    // ...and while it waits the mode changes AGAIN: what is spoken is the CURRENT mode.
    const auto u = a.update(state(7.0e6, "CW", -79.0), 4100.0);
    CHECK(u.has_value() && u->text == "C W");
    // Two utterances never closer than the interval.
    CHECK(!a.update(state(7.0e6, "USB", -79.0), 5000.0).has_value());
    CHECK(a.update(state(7.0e6, "USB", -79.0), 7200.0).has_value());
    // A frequency and a mode together: one utterance after the settle, and the mode
    // does not jump the queue ahead of an unsettled dial.
    Announcer b(p, en());
    b.update(state(7.0e6, "USB", -79.0), 0.0);
    CHECK(!b.update(state(14.2e6, "LSB", -79.0), 5000.0).has_value());          // dial just moved
    CHECK(!b.update(state(14.2e6, "LSB", -79.0), 5500.0).has_value());          // not settled (800 ms)
    const auto both = b.update(state(14.2e6, "LSB", -79.0), 5900.0);
    CHECK(both.has_value() && both->text == "14.2 megahertz, lower sideband");
}

void squelchAndUnknowns() {
    AnnouncePolicy p; p.onChange = true; p.minIntervalMs = 0.0;
    Announcer a(p, en());
    a.update(state(7.0e6, "USB", -79.0, true, true), 0.0);
    // Squelch closing speaks "no signal" once, and the level is not read while closed.
    const auto closed = a.update(state(7.0e6, "USB", -120.0, false, true), 1000.0);
    CHECK(closed.has_value() && closed->text == "no signal");
    CHECK(!a.update(state(7.0e6, "USB", -90.0, false, true), 2000.0).has_value());
    CHECK(!a.update(state(7.0e6, "USB", -60.0, false, true), 3000.0).has_value());
    // Opening speaks the level.
    const auto open = a.update(state(7.0e6, "USB", -60.0, true, true), 4000.0);
    CHECK(open.has_value() && open->text == "S 9 plus 10 decibels");
    // Unknown frequency/level/mode never produce words.
    Announcer b(p, en());
    b.update(SpokenState{}, 0.0);
    CHECK(!b.update(SpokenState{}, 1000.0).has_value());
    SpokenState onlyMode; onlyMode.mode = "AM";
    const auto m = b.update(onlyMode, 2000.0);
    CHECK(m.has_value() && m->text == "A M");
    // Level style switch and the uncalibrated path.
    AnnouncePolicy q = p; q.levelStyle = LevelStyle::Decibels;
    Announcer c(q, en());
    c.update(state(7.0e6, "USB", -79.0), 0.0);
    const auto d = c.update(state(7.0e6, "USB", -61.0), 1000.0);
    CHECK(d.has_value() && d->text == "minus 61 decibel milliwatts");
    SpokenState rel; rel.freqHz = 7.0e6; rel.mode = "USB"; rel.levelRelDb = -63.0;
    Announcer e(p, en());
    e.update(rel, 0.0);
    rel.levelRelDb = -40.0;
    const auto f = e.update(rel, 1000.0);
    CHECK(f.has_value() && f->text == "level minus 40 decibels");
}

void announceOnStartAndSpeakers() {
    AnnouncePolicy p; p.onChange = true; p.announceOnStart = true;
    Announcer a(p, en());
    RecordingSpeaker sp;
    CHECK(speakIfDue(a, sp, state(7.074e6, "USB", -79.0), 0.0));
    CHECK(sp.spoken.size() == 1 && sp.spoken[0].text == "7.074 megahertz, upper sideband, S 8");
    CHECK(!speakIfDue(a, sp, state(7.074e6, "USB", -79.0), 100.0));
    CHECK(sp.spoken.size() == 1);
    // speakNow always speaks.
    CHECK(speakNow(a, sp, state(7.074e6, "USB", -79.0), 200.0));
    CHECK(sp.spoken.size() == 2);
    // The speaker saying no is reported, not swallowed.
    sp.available = false;
    CHECK(!speakNow(a, sp, state(7.074e6, "USB", -79.0), 300.0));
    CHECK(sp.spoken.size() == 2);
    p.interrupt = false;
    Announcer b(p, en());
    RecordingSpeaker sp2;
    speakNow(b, sp2, state(7.0e6, "AM", kNaN), 0.0);
    CHECK(sp2.spoken.size() == 1 && !sp2.spoken[0].interrupt);
}

std::string readFile(const std::string& rel) {
    std::string here = __FILE__;
    std::size_t cut = here.find_last_of("/\\");
    std::string root = here.substr(0, cut);
    cut = root.find_last_of("/\\");
    root = root.substr(0, cut);
    std::ifstream in(root + "/" + rel, std::ios::binary);
    std::ostringstream ss; ss << in.rdbuf();
    return ss.str();
}

void sapiSourceHasNoAudioDevice() {
    const std::string src = readFile("src/core/access_speech_sapi.cpp");
    CHECK(!src.empty());
    for (const char* banned : {"PlaySound", "waveOut", "SpMMAudioOut", "SPDFID_Text", "MMSYSTEM", "mciSend", "CLSID_SpMMAudioOut", "IAudioClient", "XAudio"}) {
        CHECK(src.find(banned) == std::string::npos);
    }
    // The guard that keeps the engine from being reached without the memory output.
    CHECK(src.find("if (!impl_ || !impl_->memoryOutput) { return false; }") != std::string::npos);
    // SetOutput is called with the memory stream, never a null/default output.
    CHECK(src.find("SetOutput(spStream.p, TRUE)") != std::string::npos);
    CHECK(src.find("SetOutput(nullptr") == std::string::npos);
}

#ifdef _WIN32
double rms16(const std::vector<std::uint8_t>& pcm) {
    const std::size_t n = pcm.size() / 2;
    if (n == 0) { return 0.0; }
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::int16_t v = static_cast<std::int16_t>(pcm[2 * i] | (pcm[2 * i + 1] << 8));
        acc += static_cast<double>(v) * v;
    }
    return std::sqrt(acc / static_cast<double>(n));
}

void sapiMemoryVoice() {
    SapiMemorySpeaker sp;
    std::printf("  SAPI: available=%d outputIsMemory=%d voice='%s' error='%s'\n", sp.available() ? 1 : 0,
                sp.outputIsMemory() ? 1 : 0, sp.voiceName().c_str(), sp.lastError().c_str());
    CHECK(sp.available());
    CHECK(sp.outputIsMemory());
    if (!sp.available()) { return; }
    CHECK(sp.name() == "sapi-memory");
    CHECK(!sp.voiceName().empty());
    const PcmFormat f = sp.format();
    CHECK(f.sampleRate == 22050 && f.channels == 1 && f.bitsPerSample == 16);

    // Empty text is refused without touching the engine.
    CHECK(!sp.speak("", true));
    CHECK(sp.pcm().empty());

    const std::string shortText = "S 8";
    const std::string longText = "7.074 megahertz, upper sideband, S 9 plus 20 decibels";
    CHECK(sp.speak(shortText, true));
    const std::vector<std::uint8_t> a = sp.pcm();
    sp.clearPcm();
    CHECK(sp.pcm().empty());
    CHECK(sp.speak(longText, true));
    const std::vector<std::uint8_t> b = sp.pcm();
    const double secA = static_cast<double>(a.size()) / 2.0 / f.sampleRate;
    const double secB = static_cast<double>(b.size()) / 2.0 / f.sampleRate;
    std::printf("  rendered '%s' -> %zu bytes (%.2f s, rms %.0f); '%s' -> %zu bytes (%.2f s, rms %.0f)\n",
                shortText.c_str(), a.size(), secA, rms16(a), longText.c_str(), b.size(), secB, rms16(b));
    CHECK(a.size() > 2000);                       // audio came back at all
    CHECK(b.size() > a.size() * 2);                // the longer sentence is substantially longer
    CHECK(secB > 1.5 && secB < 15.0);              // a plausible length for that sentence
    CHECK(rms16(a) > 200.0 && rms16(b) > 200.0);   // not silence
    // The bytes are 16-bit mono PCM: even length, and not a RIFF container.
    CHECK(a.size() % 2 == 0 && b.size() % 2 == 0);
    CHECK(!(b.size() > 4 && b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F'));

    // Appending: a second utterance adds to the capture.
    const std::size_t before = sp.pcm().size();
    CHECK(sp.speak(shortText, false));
    CHECK(sp.pcm().size() > before);
    sp.stop();   // harmless when idle

    // The whole chain: announcer -> real voice -> memory.
    AnnouncePolicy p;
    Announcer ann(p, en());
    sp.clearPcm();
    CHECK(speakNow(ann, sp, state(14.2e6, "USB", -63.0), 0.0));
    CHECK(sp.pcm().size() > 20000);
    CHECK(sp.timeoutMs() == 20000);
}
#endif

}  // namespace

int main() {
    frequencyPhrases();
    modePhrases();
    sUnits();
    onDemand();
    onChangeOff();
    onChangeRules();
    rateLimitAndFreshness();
    squelchAndUnknowns();
    announceOnStartAndSpeakers();
    sapiSourceHasNoAudioDevice();
#ifdef _WIN32
    sapiMemoryVoice();
#else
    SKIP_LINUX("SAPI is Windows only");
#endif
    return testSummary("test_access_speech");
}
