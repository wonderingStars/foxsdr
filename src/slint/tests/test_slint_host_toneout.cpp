// The new window's test tone on Linux (src/slint/lanek_audio.cpp, the ALSA ToneOut; docs/multi-radio-records/linux-local.md section 11). The sound goes to an ALSA device this test
// defines itself (ALSA_CONFIG_PATH names a configuration with one `file` plugin over a `null` sink, FOXSDR_ALSA_DEVICE names it), so no sound card, sound server or speaker is ever touched;
// what the program wrote is then read back as raw 16-bit stereo and measured: its pitch, its level, its pan, and silence when the parameters are not audible. A device that does not exist
// must fail with a reason and leave the object harmless. On Windows the device is the waveOut mapper, which no test opens: the test says so and passes.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "test_check.hpp"

#include <cstdio>

#if defined(__linux__)
#include "lanek_audio.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using foxsdr::slintlanek::ToneOut;
namespace acc = cascade::core::access;

namespace {

fs::path g_dir;

std::vector<std::int16_t> readSamples(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream o;
    o << f.rdbuf();
    const std::string s = o.str();
    std::vector<std::int16_t> v(s.size() / 2);
    if (!v.empty()) { std::memcpy(v.data(), s.data(), v.size() * 2); }
    return v;
}

struct Measure {
    std::size_t frames = 0;
    double hzLeft = 0, peakLeft = 0, peakRight = 0;
};

Measure measure(const std::vector<std::int16_t>& s, double rate) {
    Measure m;
    m.frames = s.size() / 2;
    const std::size_t skip = 2000;   // the 5 ms fade-in and the first blocks
    if (m.frames <= skip + 2000) { return m; }
    int crossings = 0;
    for (std::size_t i = skip + 1; i < m.frames; ++i) {
        const std::int16_t a = s[2 * (i - 1)], b = s[2 * i];
        if (a < 0 && b >= 0) { ++crossings; }
        m.peakLeft = std::max(m.peakLeft, std::fabs(static_cast<double>(b)));
        m.peakRight = std::max(m.peakRight, std::fabs(static_cast<double>(s[2 * i + 1])));
    }
    m.hzLeft = crossings * rate / static_cast<double>(m.frames - skip);
    return m;
}

void useDevice(const std::string& name, const fs::path& rawOut) {
    const fs::path conf = g_dir / ("alsa-" + name + ".conf");
    std::ofstream(conf) << "pcm." << name << "null { type null }\n"
                        << "pcm." << name << " { type file slave.pcm \"" << name << "null\" file \"" << rawOut.string() << "\" format \"raw\" }\n";
    ::setenv("ALSA_CONFIG_PATH", conf.c_str(), 1);
    ::setenv("FOXSDR_ALSA_DEVICE", name.c_str(), 1);
}

void play(const acc::ToneParams& p, const std::string& name, Measure& out, ToneOut** keep = nullptr) {
    const fs::path raw = g_dir / (name + ".raw");
    useDevice(name, raw);
    ToneOut t;
    CHECK(!t.running());
    CHECK(t.start());
    CHECK(t.running());
    CHECK(t.start());            // a second start is the same open device
    for (int i = 0; i < 40; ++i) { t.feed(p); }
    t.stop();
    CHECK(!t.running());
    t.stop();                    // and a second stop is harmless
    (void)keep;
    out = measure(readSamples(raw), 22050.0);
}

void tones() {
    acc::ToneParams p;
    p.audible = true;
    p.freqHz = 1000.0f;
    p.amplitude = 0.3f;
    p.pan = 0.0f;
    Measure m;
    play(p, "centre", m);
    CHECK(m.frames > 20000);                                   // 40 feeds of up to 4 blocks of 1102 frames
    CHECK(std::fabs(m.hzLeft - 1000.0) < 30.0);                // the pitch asked for
    const double expect = 0.3 * std::cos(0.78539816339) * 32000.0;   // amplitude x the centre pan's gain x full scale
    CHECK(std::fabs(m.peakLeft - expect) < 0.06 * expect);
    CHECK(std::fabs(m.peakLeft - m.peakRight) < 0.03 * expect);   // centre: both ears equal

    p.pan = 1.0f;
    play(p, "right", m);
    CHECK(m.peakRight > 0.28 * 32000.0 && m.peakLeft < 0.02 * 32000.0);   // hard right: the left channel is silent

    p.pan = -1.0f;
    p.freqHz = 440.0f;
    play(p, "left", m);
    CHECK(m.peakLeft > 0.28 * 32000.0 && m.peakRight < 0.02 * 32000.0);
    CHECK(std::fabs(m.hzLeft - 440.0) < 15.0);

    p.audible = false;
    play(p, "silent", m);
    CHECK(m.frames > 20000 && m.peakLeft == 0.0 && m.peakRight == 0.0);   // nothing audible: written, and silent
}

void noDevice() {
    ::setenv("FOXSDR_ALSA_DEVICE", "foxsdr_no_such_pcm_device", 1);
    const fs::path conf = g_dir / "alsa-empty.conf";
    std::ofstream(conf) << "pcm.somethingelse { type null }\n";
    ::setenv("ALSA_CONFIG_PATH", conf.c_str(), 1);
    ToneOut t;
    CHECK(!t.start());
    CHECK(!t.running());
    CHECK(t.lastError().find("no sound output could be opened") != std::string::npos);
    // what the window counts (lanek_publish.cpp countToneFailure -> noteSoundFail): the host API and a reason, never the text
    CHECK(t.lastFailure().api == ToneOut::Api::Alsa);
    CHECK(t.lastFailure().reason == ToneOut::Reason::NoDevice);
    acc::ToneParams p;
    p.audible = true;
    p.freqHz = 500.0f;
    p.amplitude = 0.3f;
    t.feed(p);                  // nothing open: no effect, no fault
    t.stop();
    CHECK(!t.running());
}

// A write that the device refuses (the file plugin's file is /dev/full: every write fails with ENOSPC): the tone stops itself, says why, and the window can tell the person.
void writeFails() {
    useDevice("full", "/dev/full");
    ToneOut t;
    CHECK(t.start());
    acc::ToneParams p;
    p.audible = true;
    p.freqHz = 700.0f;
    p.amplitude = 0.3f;
    for (int i = 0; i < 40 && t.running(); ++i) { t.feed(p); }
    CHECK(!t.running());
    CHECK(t.lastError().find("the sound output failed") != std::string::npos);
    CHECK(t.lastFailure().api == ToneOut::Api::Alsa);
    CHECK(t.lastFailure().reason == ToneOut::Reason::Host);   // a stream that died while it played
    t.feed(p);                  // after the failure: nothing open, no effect
    CHECK(!t.running());
}

}  // namespace

int main() {
    g_dir = fs::temp_directory_path() / ("foxsdr-toneout-" + std::to_string(static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count())));
    fs::create_directories(g_dir);
    tones();
    noDevice();
    writeFails();
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    return testSummary("test_slint_host_toneout");
}
#else
int main() {
    std::printf("the Windows tone output is the waveOut mapper; no test opens it\n");
    return testSummary("test_slint_host_toneout");
}
#endif
