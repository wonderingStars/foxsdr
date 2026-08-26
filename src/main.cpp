// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// Thin entry point: parse the command line, hand off to the GUI shell.
// `--frames N` renders exactly N frames then exits 0 — the bounded-run
// contract the app_smoke ctest entry relies on. `--selftest` runs the
// pipeline headless (no window, no GL, no audio device) and checks that the
// demo signal's peak lands on the right FFT bin AND that the audio chain
// demodulates a CW carrier to its 700 Hz sidetone. No flag: run until the
// window is closed.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

#include <string>

#include "core/config.hpp"
#include "core/pipeline.hpp"
#include "core/updater.hpp"
#include "core/version.hpp"
#include "dsp/demod.hpp"
#include "dsp/rds.hpp"
#include "dsp/vfo.hpp"
#include "core/recorder.hpp"
#include "gui/app_window.hpp"
#include "source/iq_file_source.hpp"
#include "source/soapy_enum_proc.hpp"
#include "source/soapy_source.hpp"

#include "core/crash_handler.hpp"
#include "core/diag_log.hpp"
#include "core/diag_report.hpp"

// ---------------------------------------------------------------------------
// Fatal-fault capture.
//
// This used to be forty lines of SEH filter inline in this file (kept from the
// P6a crash investigation, 2026-08-15, where one stderr line naming
// libusb-1.0.dll inside SoapyUHD was the whole diagnosis). That line still
// exists - it is now written by core/crash_handler.cpp, along with a report an
// engineer who was not there can act on. See core/crash_handler.hpp for which
// four registrations are made and why one filter was never enough.
// ---------------------------------------------------------------------------
namespace {

// WHEN DIAGNOSTICS TOUCH THE DISK, and it is deliberately narrow.
//
// A plain `cascade` with no arguments is a real session on a real user's
// machine: it logs, it captures, it offers a report. EVERY flagged invocation
// is a tool or a test - app_smoke, app_selftest, app_version, --update-check,
// the bench checks - and those must leave nothing behind on the machine that
// ran them, exactly as the config already stays hermetic under --frames.
//
// FOXSDR_DIAG_DIR overrides both halves: it redirects the whole tree into a
// caller-owned directory AND turns capture on, which is how
// tests/test_diag_hang.cpp exercises the real application without going
// anywhere near the user's %LOCALAPPDATA%\FoxSDR.
bool diagnosticsOnDisk(int argc) {
    const char* over = std::getenv("FOXSDR_DIAG_DIR");
    if (over != nullptr && *over != '\0') { return true; }
    return argc == 1;
}

// The config THIS run will use, resolved before the argument parse so the
// user's stored switch can be honoured before anything touches the disk. Same
// rules as the config wiring at the bottom of main(), and deliberately so: a
// bare `cascade` uses the real per-user config; a bounded --frames run uses
// CASCADE_CONFIG_TEST when it is set and no config at all otherwise. Empty
// means "this run has no stored preference".
std::string startupConfigPath(int argc, char** argv) {
    bool bounded = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0) { bounded = true; }
    }
    if (bounded) {
        const char* hook = std::getenv("CASCADE_CONFIG_TEST");
        return (hook != nullptr && *hook != '\0') ? std::string(hook) : std::string();
    }
    if (argc == 1) { return cascade::core::ConfigStore::defaultPath(); }
    return std::string();
}

// DOES THE USER WANT THIS AT ALL, read from the config BEFORE a directory is
// created or a handler is armed.
//
// This is the second half of "off means off", and the half that was wrong.
// diagnosticsOnDisk() above answers "is this run allowed to write" - a real
// session rather than a tool. It does NOT answer "did the user ask for it",
// and reading that only once AppWindow::run() is reached is far too late: by
// then the log file exists, the crashes directory exists, and the crash
// handler has been armed across the config load, the GL context and the
// LoadLibrary of every third-party plugin - which is the most fault-prone part
// of start-up and precisely where a user who opted out would get a report.
//
// Reproduced on the shipped application before this was written: a config
// saying "diagnosticsEnabled": false produced a 14-line foxsdr.log and a
// crashes directory. Held there now by tests/test_diagnostics.cpp, which
// launches the real binary with that config and requires the tree not to exist.
//
// The cost is one small JSON read at the top of main() that AppWindow will do
// again a moment later. That is the correct trade: the alternative is arming
// the disk before consent is known.
bool storedDiagnosticsEnabled(int argc, char** argv) {
    const std::string path = startupConfigPath(argc, argv);
    if (path.empty()) { return cascade::core::AppConfig{}.diagnosticsEnabled; }
    cascade::core::AppConfig cfg;
    std::string err;
    // Missing or corrupt file: ConfigStore::load leaves the defaults in place,
    // which is on. A user who has never chosen has not opted out.
    cascade::core::ConfigStore::load(path, cfg, err);
    return cfg.diagnosticsEnabled;
}

}  // namespace

namespace {

// End-to-end DSP check with zero GUI involvement, so it also runs on CI
// boxes and SSH sessions with no display. Mirrors the exact pipeline + demo
// signal AppWindow constructs; if the two drift apart, the selftest guards
// the DSP path only, which is its job.
int runSelftest() {
    cascade::core::Pipeline::Config cfg;
    cfg.sampleRateHz = 2'000'000.0;
    cfg.fftSize = 1024;
    cfg.averagingAlpha = 0.5f;
    cfg.audioEnabled = false;  // the audio chain still runs; no device needed

    cascade::core::Pipeline pipeline(cfg);
    cascade::source::SigGen& gen = pipeline.sigGen();
    gen.setTone(0, 300000.0, -30.0f);   // the dominant tone the check targets
    gen.setTone(1, -500000.0, -45.0f);  // present but 15 dB down — must NOT win
    gen.setNoiseFloorDb(-90.0f);
    pipeline.start();

    // Wait for at least 5 published frames so the EMA has settled past its
    // first-frame prime. Generous 10 s deadline: the pipeline needs only
    // ~5 * 0.5 ms of samples, but Windows scheduling under load is spiky.
    cascade::core::SpectrumFrame frame;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        pipeline.getLatestFrame(frame);
        if (frame.seq >= 5) { break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (frame.seq < 5) {
        pipeline.stop();
        std::printf("selftest FAIL only %llu frames within 10 s\n",
                    static_cast<unsigned long long>(frame.seq));
        return 1;
    }

    std::size_t peak = 0;
    for (std::size_t i = 1; i < frame.dbBins.size(); ++i) {
        if (frame.dbBins[i] > frame.dbBins[peak]) { peak = i; }
    }
    const int peakBin = static_cast<int>(peak);

    // Expected bin, computed from the config rather than hard-coded: the
    // spectrum is fftshifted (DC at fftSize/2), and +300 kHz at 2 MS/s with
    // 1024 bins sits round(300000 / 2000000 * 1024) = 154 bins above DC.
    // +/-2 bins of slack covers window spreading of the off-grid tone.
    const int expected =
        static_cast<int>(cfg.fftSize) / 2 +
        static_cast<int>(std::lround(300000.0 / cfg.sampleRateHz *
                                     static_cast<double>(cfg.fftSize)));
    if (peakBin < expected - 2 || peakBin > expected + 2) {
        pipeline.stop();
        std::printf("selftest FAIL peak_bin=%d expected=%d +/-2\n", peakBin, expected);
        return 1;
    }

    // --- Audio-chain check (P3) ---------------------------------------------
    // Tune the VFO onto demo tone 0 and demodulate it as CW: the carrier at
    // the VFO center must beat at the 700 Hz sidetone. The chain is tapped
    // BEFORE AudioOut (Pipeline::audioTap / audioSamplesProduced), so this
    // verifies VFO -> demod -> AGC -> squelch -> resampler with no device.
    pipeline.setDemodMode(cascade::dsp::DemodMode::CW);
    pipeline.setVfoOffsetHz(300000.0);

    // Let >= 24000 post-switch samples (0.5 s at 48 kHz) flow so the squelch
    // has opened and the tap window holds pure steady-state CW audio.
    const std::uint64_t base = pipeline.audioSamplesProduced();
    std::uint64_t produced = 0;
    const auto audioDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < audioDeadline) {
        produced = pipeline.audioSamplesProduced() - base;
        if (produced >= 24000) { break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (produced < 24000) {
        pipeline.stop();
        std::printf("selftest FAIL only %llu audio samples within 10 s\n",
                    static_cast<unsigned long long>(produced));
        return 1;
    }

    std::vector<float> window(4096);
    const std::size_t got = pipeline.audioTap(window.data(), window.size());
    pipeline.stop();
    if (got < window.size()) {
        std::printf("selftest FAIL audio tap returned %llu of %llu samples\n",
                    static_cast<unsigned long long>(got),
                    static_cast<unsigned long long>(window.size()));
        return 1;
    }

    // Dominant DFT frequency over the captured window: direct DFT (no
    // dependency on the FFT wrapper — an independent reference, per the
    // testing protocol), rectangular window, bins 1..N/2 so DC never wins.
    const std::size_t nWin = window.size();
    std::size_t bestBin = 1;
    double bestPow = -1.0;
    for (std::size_t k = 1; k <= nWin / 2; ++k) {
        double re = 0.0;
        double im = 0.0;
        const double w = -2.0 * 3.14159265358979323846 *
                         static_cast<double>(k) / static_cast<double>(nWin);
        for (std::size_t i = 0; i < nWin; ++i) {
            const double angle = w * static_cast<double>(i);
            const double x = static_cast<double>(window[i]);
            re += x * std::cos(angle);
            im += x * std::sin(angle);
        }
        const double p = re * re + im * im;
        if (p > bestPow) {
            bestPow = p;
            bestBin = k;
        }
    }
    const double audioHz = static_cast<double>(bestBin) *
                           cascade::core::Pipeline::kAudioRateHz /
                           static_cast<double>(nWin);

    // CW sidetone is 700 Hz (dsp/demod.cpp kCwToneHz). +/-40 Hz covers the
    // 48000/4096 = 11.7 Hz bin quantization with margin.
    if (std::fabs(audioHz - 700.0) > 40.0) {
        std::printf("selftest FAIL audio_hz=%.1f expected 700 +/-40\n", audioHz);
        return 1;
    }
    std::printf("selftest PASS peak_bin=%d audio_hz=%.1f\n", peakBin, audioHz);
    return 0;
}

// Bench diagnostic (hidden flag --soapy-check, deliberately absent from the
// usage string and never a ctest entry — it needs real hardware): enumerate
// SoapySDR, open the first RADIO device, set 2 MS/s / 100 MHz, and stream for
// two seconds through a headless Pipeline. PASS requires >= 20 spectrum
// frames and a rate readback within 1% of 2 MS/s. A machine with no device
// (or no vendor modules) FAILs cleanly with a reason — that is the expected
// answer off the bench, not a crash. The 30 s first-frame deadline covers a
// cold USB device's firmware/FPGA load, which the open above already blocked
// through once (UHD loads inside Device::make).
//
// "First RADIO device": SoapyAudio (driver=audio) advertises every sound
// card as a SoapySDR device, so on a bench with that module installed the
// literal first enumeration row is a Realtek codec, not the SDR. Rows whose
// kwargs carry driver=audio are skipped — a sound card cannot satisfy a
// 2 MS/s RF check and its constant presence would make "no device attached"
// undetectable.
int runSoapyCheck() {
    const auto devices = cascade::source::SoapySource::enumerate();
    const cascade::source::SoapyDeviceInfo* pick = nullptr;
    for (const auto& d : devices) {
        if (d.args.find("driver=audio") == std::string::npos) {
            pick = &d;
            break;
        }
    }
    if (pick == nullptr) {
        std::printf("soapy-check FAIL no SoapySDR radio devices enumerated\n");
        // WHERE IT LOOKED, because "no devices" on its own is the message that
        // hid a total failure of the module search path for several releases:
        // it reads identically whether nothing is plugged in, nothing is
        // installed, or the application is looking in a directory that cannot
        // exist. These three lines are what turn a support conversation into a
        // one-command answer.
        for (const auto& v : cascade::source::SoapySource::vendorInstalls()) {
            std::printf("soapy-check vendor: %s at %s\n", v.name.c_str(), v.root.c_str());
        }
        for (const auto& p : cascade::source::SoapySource::moduleSearchPaths()) {
            std::printf("soapy-check search: %s\n", p.c_str());
        }
        const auto modules = cascade::source::SoapySource::loadedModules();
        for (const auto& m : modules) {
            std::printf("soapy-check module: %s\n", m.c_str());
        }
        if (modules.empty()) {
            std::printf("soapy-check modules: none loaded - install PothosSDR or radioconda\n");
        }
        return 1;
    }
    std::fprintf(stderr, "cascade: soapy-check device: %s (%s)\n",
                 pick->label.c_str(), pick->args.c_str());

    auto src = std::make_unique<cascade::source::SoapySource>();
    if (!src->open(pick->args)) {
        std::printf("soapy-check FAIL open: %s\n", src->lastError());
        return 1;
    }
    if (!src->setSampleRateHz(2'000'000.0)) {
        std::printf("soapy-check FAIL set rate: %s\n", src->lastError());
        return 1;
    }
    if (!src->setCenterFrequencyHz(100'000'000.0)) {
        std::printf("soapy-check FAIL tune: %s\n", src->lastError());
        return 1;
    }
    const double actualRate = src->sampleRateHz();  // device readback, not echo

    cascade::core::Pipeline::Config cfg;
    cfg.sampleRateHz = 2'000'000.0;
    cfg.fftSize = 1024;
    cfg.averagingAlpha = 0.5f;
    cfg.audioEnabled = false;  // bench diagnostic: no audio device wanted
    cascade::core::Pipeline pipeline(cfg);
    pipeline.setSource(std::move(src));
    pipeline.start();

    // Wait (bounded) for the stream to produce its first frame, then measure
    // the frame count over exactly two seconds of steady streaming.
    cascade::core::SpectrumFrame frame;
    const auto firstDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < firstDeadline) {
        pipeline.getLatestFrame(frame);
        if (frame.seq >= 1) { break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (frame.seq < 1) {
        pipeline.stop();
        std::printf("soapy-check FAIL no spectrum frames within 30 s (%s)\n",
                    pipeline.activeSource().lastError());
        return 1;
    }
    const std::uint64_t seqAtStart = frame.seq;
    std::this_thread::sleep_for(std::chrono::seconds(2));
    pipeline.getLatestFrame(frame);
    const std::uint64_t frames = frame.seq - seqAtStart;
    pipeline.stop();

    if (frames < 20) {
        std::printf("soapy-check FAIL only %llu frames in 2 s (>= 20 required)\n",
                    static_cast<unsigned long long>(frames));
        return 1;
    }
    if (std::fabs(actualRate - 2'000'000.0) > 0.01 * 2'000'000.0) {
        std::printf("soapy-check FAIL rate readback %.0f not within 1%% of 2000000\n",
                    actualRate);
        return 1;
    }
    std::printf("soapy-check PASS frames=%llu rate=%.0f\n",
                static_cast<unsigned long long>(frames), actualRate);
    return 0;
}

// Bench diagnostic (hidden flag --tone-check): drive the audio output
// directly with an audible 1 kHz sine for two seconds. This is the bisect for
// "no sound" — it bypasses the whole radio chain (source, VFO, demod, AGC,
// squelch) and answers one question: does this machine's speakers path work
// at all? Silence here means the device/volume/routing; sound here means the
// audio stack is fine and the silence is upstream (nothing tuned, or a mode
// that is legitimately silent on the signal being fed to it).
int runToneCheck() {
    cascade::sink::AudioOut audio;
    constexpr double kRate = 48000.0;
    if (!audio.open(-1, kRate)) {
        std::printf("tone-check FAIL could not open the default output device\n");
        return 1;
    }
    audio.setVolume(0.5f);

    // 1 kHz at half scale, written slightly ahead of real time so the ring
    // never starves; 2 s is long enough to be unmistakable.
    constexpr int kBlock = 480;  // 10 ms
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    std::vector<float> block(kBlock);
    double phase = 0.0;
    const double step = kTwoPi * 1000.0 / kRate;
    // Keep the ring as full as it will take rather than writing in lockstep
    // with playback: a bounded ring back-pressures naturally (write() returns
    // what it accepted), so this both pre-fills before the stream starts
    // pulling and never starves it afterwards. Pacing by sleep instead would
    // sit permanently one block from empty and log underruns that say nothing
    // about the machine's audio.
    constexpr std::uint64_t kTotal = 96000;  // 2 s at 48 kHz
    std::uint64_t written = 0;
    while (written < kTotal) {
        for (int i = 0; i < kBlock; ++i) {
            block[static_cast<std::size_t>(i)] = 0.5f * static_cast<float>(std::sin(phase));
            phase += step;
            if (phase > kTwoPi) { phase -= kTwoPi; }
        }
        const std::size_t took = audio.write(block.data(), block.size());
        written += took;
        if (took < block.size()) {
            // Ring full: rewind the phase for the samples it refused so the
            // sine stays continuous, then wait for the callback to drain.
            phase -= step * static_cast<double>(block.size() - took);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    // Let the ring play out: 2 s of audio has been handed over, but the
    // callback is still consuming the tail.
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // drain
    const std::uint64_t under = audio.underruns();
    audio.close();
    std::printf("tone-check PASS wrote=%llu underruns=%llu (did you hear a 1 kHz tone?)\n",
                static_cast<unsigned long long>(written),
                static_cast<unsigned long long>(under));
    return 0;
}

// Bench diagnostic (hidden flag --rds-check <MHz>): the ONLY test that can
// validate the RDS offset words and CRC polynomial. The unit tests encode with
// the same constants they decode with, so a self-consistent error in them
// passes every check and still fails off air. Recovering a real station's
// name from a real broadcast is what closes that gap.
//
// Chain built by hand rather than reusing Pipeline's: RDS rides a 57 kHz
// subcarrier on the FM composite, and the audio path's de-emphasis would
// crush it (a 50 us one-pole is ~40 dB down at 57 kHz). So: Vfo -> WFM
// discriminator with de-emphasis explicitly OFF -> RdsDecoder.
// Diagnostic support for the above (added during the off-air RDS debug,
// 2026-08-15). The B200 is USB-exclusive and a hardware run costs 45 s, so the
// bench can CAPTURE the demodulated composite it feeds the decoder
// (CASCADE_RDS_MPX=<path>, raw little-endian float32 at 250 kHz) and REPLAY it
// later (CASCADE_RDS_REPLAY=<path>) with no radio attached. Replaying a real
// broadcast turns a hardware-in-the-loop debug into an offline one that runs in
// a second, and the capture doubles as a permanent off-air regression fixture.
int runRdsReplay(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("rds-check FAIL cannot open replay file %s\n", path);
        return 1;
    }
    constexpr double kChanRate = 250'000.0;
    cascade::dsp::RdsDecoder rds(kChanRate);
    std::vector<float> block(65536);
    std::uint64_t fed = 0;
    while (in) {
        in.read(reinterpret_cast<char*>(block.data()),
                static_cast<std::streamsize>(block.size() * sizeof(float)));
        const std::size_t got =
            static_cast<std::size_t>(in.gcount()) / sizeof(float);
        if (got == 0) { break; }
        rds.process(block.data(), got);
        fed += got;
    }
    const cascade::dsp::RdsState st = rds.state();
    std::printf("rds-replay %s synced=%d pi=%04X piValid=%d ps=\"%s\" psValid=%d "
                "pty=%u tp=%d ta=%d groups=%u blockErrors=%u fed=%llu (%.1f s)\n",
                (st.piValid && st.psValid) ? "PASS" : "FAIL", rds.synced() ? 1 : 0,
                st.pi, st.piValid ? 1 : 0, st.ps.c_str(), st.psValid ? 1 : 0,
                static_cast<unsigned>(st.pty), st.tp ? 1 : 0, st.ta ? 1 : 0,
                st.groupsDecoded, st.blockErrors,
                static_cast<unsigned long long>(fed),
                static_cast<double>(fed) / kChanRate);
    if (!st.radioText.empty()) {
        std::printf("rds-replay radiotext=\"%s\"\n", st.radioText.c_str());
    }
    return (st.piValid && st.psValid) ? 0 : 1;
}

int runRdsCheck(double centerHz) {
    if (const char* replay = std::getenv("CASCADE_RDS_REPLAY")) {
        return runRdsReplay(replay);
    }
    if (!cascade::source::SoapySource::runtimeAvailable()) {
        std::printf("rds-check FAIL SoapySDR runtime not available\n");
        return 1;
    }
    auto devs = cascade::source::SoapySource::enumerate();
    std::string args;
    for (const auto& d : devs) {
        if (d.args.find("driver=audio") == std::string::npos) { args = d.args; break; }
    }
    if (args.empty()) {
        std::printf("rds-check FAIL no SoapySDR radio devices enumerated\n");
        return 1;
    }

    cascade::source::SoapySource src;
    if (!src.open(args)) {
        std::printf("rds-check FAIL open: %s\n", src.lastError());
        return 1;
    }
    constexpr double kRate = 2'000'000.0;
    src.setSampleRateHz(kRate);
    src.setCenterFrequencyHz(centerHz);
    // Gain matters: a B200 opens near 0 dB, and RDS at -20 dB relative to the
    // main carrier is the first thing to vanish in the noise. The GUI pushes a
    // default too; this path has no UI to do it.
    src.setAutoGain(false);
    // CASCADE_RDS_GAIN overrides the 50 dB default (diagnostic: RDS sits at the
    // top of the FM baseband where demodulator noise is worst, so it is the
    // first thing lost to a poor carrier-to-noise ratio — gain is the cheapest
    // variable to sweep when it fails to appear).
    double gainDb = 50.0;
    if (const char* g = std::getenv("CASCADE_RDS_GAIN")) {
        const double v = std::strtod(g, nullptr);
        if (v > 0.0) { gainDb = v; }
    }
    for (const std::string& g : src.listGainNames()) {
        const bool ok = src.setGainDb(g, gainDb);
        std::printf("rds-check gain %s := %.1f dB (%s)\n", g.c_str(), gainDb,
                    ok ? "accepted" : "REFUSED");
    }
    if (!src.start()) {
        std::printf("rds-check FAIL start: %s\n", src.lastError());
        return 1;
    }

    // Decimate 2 MS/s -> 250 kHz; 200 kHz of channel comfortably passes the
    // 53 kHz composite plus the 57 kHz RDS subcarrier.
    constexpr unsigned kDecim = 8;
    const double chanRate = kRate / kDecim;
    cascade::dsp::Vfo vfo(kRate, kDecim, 200000.0);
    vfo.setOffsetHz(0.0);
    cascade::dsp::Demodulator demod(chanRate);
    demod.setMode(cascade::dsp::DemodMode::WFM);
    demod.setDeemphasisUs(0.0);  // MUST be off: see the comment above
    cascade::dsp::RdsDecoder rds(chanRate);

    std::vector<std::complex<float>> raw(65536);
    std::vector<std::complex<float>> chan(65536);
    std::vector<float> mpx(65536);

    // Optional capture of the exact composite handed to the decoder.
    std::ofstream capture;
    if (const char* capPath = std::getenv("CASCADE_RDS_MPX")) {
        capture.open(capPath, std::ios::binary | std::ios::trunc);
        if (!capture) { std::printf("rds-check WARN cannot write %s\n", capPath); }
    }
    // Optional capture of the RAW IQ ahead of the channel filter, so the whole
    // chain (channel bandwidth included) can be re-run offline. Capped and
    // terminating, because 2 MS/s of complex float is 16 MB per second.
    std::ofstream iqCapture;
    std::uint64_t iqSamplesLeft = 0;
    if (const char* iqPath = std::getenv("CASCADE_RDS_IQ")) {
        iqCapture.open(iqPath, std::ios::binary | std::ios::trunc);
        if (!iqCapture) { std::printf("rds-check WARN cannot write %s\n", iqPath); }
        const char* secs = std::getenv("CASCADE_RDS_IQ_SECS");
        const double s = secs ? std::strtod(secs, nullptr) : 10.0;
        iqSamplesLeft = static_cast<std::uint64_t>((s > 0.0 ? s : 10.0) * kRate);
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
    std::uint64_t fedSamples = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const std::size_t got = src.read(raw.data(), raw.size());
        if (got == 0) { continue; }  // self-paced timeout: retry per contract
        if (iqCapture && iqSamplesLeft > 0) {
            const std::size_t take = static_cast<std::size_t>(
                std::min<std::uint64_t>(iqSamplesLeft, got));
            iqCapture.write(reinterpret_cast<const char*>(raw.data()),
                            static_cast<std::streamsize>(
                                take * sizeof(std::complex<float>)));
            iqSamplesLeft -= take;
            if (iqSamplesLeft == 0) {
                iqCapture.close();
                std::printf("rds-check IQ capture complete\n");
                break;
            }
        }
        const std::size_t m = vfo.process(raw.data(), got, chan.data(), chan.size());
        if (m == 0) { continue; }
        demod.process(chan.data(), m, mpx.data());
        if (capture) {
            capture.write(reinterpret_cast<const char*>(mpx.data()),
                          static_cast<std::streamsize>(m * sizeof(float)));
        }
        rds.process(mpx.data(), m);
        fedSamples += m;
        const cascade::dsp::RdsState st = rds.state();
        // CASCADE_RDS_FULL keeps running past the first good PS so the block
        // error RATE and the complete RadioText can be observed, rather than
        // just "it locked once".
        static const bool kRunFull = std::getenv("CASCADE_RDS_FULL") != nullptr;
        if (st.piValid && st.psValid && !kRunFull) {
            src.stop();
            std::printf("rds-check PASS pi=%04X ps=\"%s\" pty=%u groups=%u blockErrors=%u\n",
                        st.pi, st.ps.c_str(), static_cast<unsigned>(st.pty),
                        st.groupsDecoded, st.blockErrors);
            if (!st.radioText.empty()) {
                std::printf("rds-check radiotext=\"%s\"\n", st.radioText.c_str());
            }
            return 0;
        }
    }
    const cascade::dsp::RdsState st = rds.state();

    // Diagnose WHERE it failed rather than just reporting that it did. Measure
    // the composite the decoder was actually fed: a healthy WFM composite has
    // a 19 kHz pilot and, on an RDS station, energy at 57 kHz. No pilot means
    // the failure is upstream (mistuned, too little gain, wrong demod) and the
    // RDS decoder never had a chance; pilot but no 57 kHz means the station
    // simply carries no RDS; both present but no sync points at the decoder.
    const std::size_t got = src.read(raw.data(), raw.size());
    const std::size_t m = (got > 0) ? vfo.process(raw.data(), got, chan.data(), chan.size()) : 0;
    if (m > 0) { demod.process(chan.data(), m, mpx.data()); }
    src.stop();

    auto toneDb = [&](double hz) {
        if (m < 4096) { return -200.0; }
        const std::size_t n = 4096;
        double re = 0.0, im = 0.0, tot = 0.0;
        const double w = -2.0 * 3.14159265358979323846 * hz / chanRate;
        for (std::size_t i = 0; i < n; ++i) {
            const double x = static_cast<double>(mpx[i]);
            re += x * std::cos(w * static_cast<double>(i));
            im += x * std::sin(w * static_cast<double>(i));
            tot += x * x;
        }
        const double binP = (re * re + im * im) / (static_cast<double>(n) * n);
        const double avgP = tot / static_cast<double>(n);
        return (avgP > 0.0) ? 10.0 * std::log10(binP / avgP + 1e-30) : -200.0;
    };

    if (st.piValid && st.psValid) {
        std::printf("rds-check PASS(full) pi=%04X ps=\"%s\" pty=%u tp=%d ta=%d "
                    "groups=%u blockErrors=%u (%.2f%% of blocks) synced=%d\n",
                    st.pi, st.ps.c_str(), static_cast<unsigned>(st.pty),
                    st.tp ? 1 : 0, st.ta ? 1 : 0, st.groupsDecoded, st.blockErrors,
                    st.groupsDecoded ? 100.0 * st.blockErrors /
                                           (4.0 * st.groupsDecoded)
                                     : 0.0,
                    rds.synced() ? 1 : 0);
        std::printf("rds-check radiotext=\"%s\"\n", st.radioText.c_str());
        return 0;
    }
    std::printf("rds-check FAIL no PS within 45 s (synced=%d pi=%04X piValid=%d "
                "groups=%u blockErrors=%u fed=%llu)\n",
                rds.synced() ? 1 : 0, st.pi, st.piValid ? 1 : 0, st.groupsDecoded,
                st.blockErrors, static_cast<unsigned long long>(fedSamples));
    std::printf("rds-check composite: pilot19k=%.1f dBc  rds57k=%.1f dBc  "
                "audio1k=%.1f dBc (relative to total composite power)\n",
                toneDb(19000.0), toneDb(57000.0), toneDb(1000.0));
    return 1;
}

// --- Recorder end-to-end check (hidden flag --record-check) -------------------

// Little-endian field decodes for the WAV probe below (mirrors the recorder's
// own explicit-byte encoder, so the probe is endian- and alignment-safe).
std::uint32_t leU32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint16_t leU16(const unsigned char* p) {
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(p[0]) |
                                      (static_cast<std::uint32_t>(p[1]) << 8));
}

// Minimal chunk-walking WAV probe: true iff the file is RIFF/WAVE carrying a
// fmt chunk and a data chunk, reporting rate/channels/bits and the data byte
// length. Deliberately independent of IqFileSource, which by design rejects
// the AUDIO recording's mono int16 layout (it only accepts 2-channel IQ).
bool probeWav(const std::string& path, std::uint32_t* rate,
              std::uint16_t* channels, std::uint16_t* bits,
              std::uint64_t* dataBytes) {
    std::ifstream f(path, std::ios::binary);
    unsigned char riff[12];
    if (!f.read(reinterpret_cast<char*>(riff), 12)) { return false; }
    if (std::memcmp(riff, "RIFF", 4) != 0 ||
        std::memcmp(riff + 8, "WAVE", 4) != 0) {
        return false;
    }
    bool haveFmt = false;
    bool haveData = false;
    unsigned char ch[8];
    while (f.read(reinterpret_cast<char*>(ch), 8)) {
        const std::uint32_t size = leU32(ch + 4);
        std::uint32_t skip = size + (size & 1u);  // chunks are word-aligned
        if (std::memcmp(ch, "fmt ", 4) == 0 && size >= 16) {
            unsigned char fmt[16];
            if (!f.read(reinterpret_cast<char*>(fmt), 16)) { return false; }
            *channels = leU16(fmt + 2);
            *rate = leU32(fmt + 4);
            *bits = leU16(fmt + 14);
            haveFmt = true;
            skip -= 16;
        } else if (std::memcmp(ch, "data", 4) == 0) {
            *dataBytes = size;
            haveData = true;
        }
        f.seekg(static_cast<std::streamoff>(skip), std::ios::cur);
    }
    return haveFmt && haveData;
}

// Bench diagnostic (hidden flag --record-check, deliberately absent from the
// usage string like --soapy-check, and never a ctest entry — it sleeps a
// real-time second, which is bench-check budget, not unit-test budget):
// proves the recorder wiring end to end with ZERO GUI involvement. A
// headless pipeline (generator source, no audio device) runs both recorder
// taps simultaneously for ~1 s of wall time; the IQ take is then re-read
// through IqFileSource — the actual replay path, closing the record->replay
// loop — checking rate readback 2000000 and a sample count within 10% of
// rate x window (2,000,000 samples for the 1 s window; the slack covers
// source pacing plus ring/block latency at the tap edges). The audio take
// must parse as a mono 16-bit 48000 Hz WAV. The takes land in a fresh
// unique directory under %TEMP% and are left there for inspection.
int runRecordCheck() {
    namespace fs = std::filesystem;

    const char* tmp = std::getenv("TEMP");
    const fs::path base =
        (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
    const fs::path dir =
        base / (std::string("cascade-record-check-") +
                std::to_string(static_cast<unsigned long long>(
                    std::chrono::system_clock::now().time_since_epoch().count())));

    cascade::core::Pipeline::Config cfg;
    cfg.sampleRateHz = 2'000'000.0;
    cfg.fftSize = 1024;
    cfg.averagingAlpha = 0.5f;
    cfg.audioEnabled = false;  // headless: no device; the chain still runs
    cascade::core::Pipeline pipeline(cfg);
    // Real content (demo tone over a noise floor): the checks below assert
    // counts and headers, but recording live nonzero floats exercises the
    // same encode path a bench take would.
    pipeline.sigGen().setTone(0, 300000.0, -30.0f);
    pipeline.sigGen().setNoiseFloorDb(-90.0f);
    pipeline.start();

    cascade::core::Recorder iqRec;
    cascade::core::Recorder audioRec;
    std::string err;
    if (!iqRec.start(cascade::core::RecordKind::BasebandIq, dir.string(),
                     cfg.sampleRateHz, err)) {
        pipeline.stop();
        std::printf("record-check FAIL iq start: %s\n", err.c_str());
        return 1;
    }
    if (!audioRec.start(cascade::core::RecordKind::Audio, dir.string(),
                        cascade::core::Pipeline::kAudioRateHz, err)) {
        iqRec.stop();
        pipeline.stop();
        std::printf("record-check FAIL audio start: %s\n", err.c_str());
        return 1;
    }

    // Both taps at once (the spec's simultaneous-recording contract). The
    // window is measured install -> uninstall so the pacing comparison
    // divides by what the taps actually saw, not by the nominal sleep.
    const auto t0 = std::chrono::steady_clock::now();
    pipeline.setIqRecorder(&iqRec);
    pipeline.setAudioRecorder(&audioRec);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    pipeline.setIqRecorder(nullptr);
    pipeline.setAudioRecorder(nullptr);
    const double windowS =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();
    // Uninstall-then-stop: the documented Recorder toggle order.
    iqRec.stop();
    audioRec.stop();
    pipeline.stop();

    const std::uint64_t iqSamples = iqRec.samplesWritten();
    const std::uint64_t audioSamples = audioRec.samplesWritten();

    // Locate the two takes by prefix — the directory is fresh per run, so
    // exactly one iq_* and one audio_* file can exist.
    std::string iqPath;
    std::string audioPath;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("iq_", 0) == 0) { iqPath = e.path().string(); }
        if (name.rfind("audio_", 0) == 0) { audioPath = e.path().string(); }
    }
    if (ec || iqPath.empty() || audioPath.empty()) {
        std::printf("record-check FAIL takes not found in %s\n",
                    dir.string().c_str());
        return 1;
    }

    // IQ readback through the REAL replay path.
    cascade::source::IqFileSource reader;
    if (!reader.open(iqPath)) {
        std::printf("record-check FAIL iq reopen: %s\n", reader.lastError());
        return 1;
    }
    if (reader.sampleRateHz() != 2'000'000.0) {
        std::printf("record-check FAIL iq rate readback %.0f != 2000000\n",
                    reader.sampleRateHz());
        return 1;
    }
    // Stream a block back to prove the data chunk actually replays (read()
    // returning short would mean a husk with a lying header).
    std::vector<std::complex<float>> replay(4096);
    reader.start();
    const std::size_t got = reader.read(replay.data(), replay.size());
    reader.stop();
    if (got != replay.size()) {
        std::printf("record-check FAIL iq replay read %llu of %llu\n",
                    static_cast<unsigned long long>(got),
                    static_cast<unsigned long long>(replay.size()));
        return 1;
    }

    // Length: the file's own frame count (fixed 44-byte header + 8 bytes per
    // IQ frame) must match the recorder's accepted-frame counter exactly,
    // and land within the 10% pacing tolerance of rate x window.
    const std::uintmax_t fileBytes = fs::file_size(iqPath, ec);
    if (ec || fileBytes < 44u) {
        std::printf("record-check FAIL iq file unreadable/truncated\n");
        return 1;
    }
    const std::uint64_t fileFrames = (fileBytes - 44u) / 8u;
    if (fileFrames != iqSamples) {
        std::printf("record-check FAIL iq file frames %llu != counter %llu\n",
                    static_cast<unsigned long long>(fileFrames),
                    static_cast<unsigned long long>(iqSamples));
        return 1;
    }
    const double expect = cfg.sampleRateHz * windowS;
    if (std::fabs(static_cast<double>(iqSamples) - expect) > 0.10 * expect) {
        std::printf(
            "record-check FAIL iq_samples=%llu outside 10%% of %.0f "
            "(window %.3f s)\n",
            static_cast<unsigned long long>(iqSamples), expect, windowS);
        return 1;
    }

    // Audio take: must parse as the recorder's documented layout at 48 kHz.
    std::uint32_t aRate = 0;
    std::uint16_t aChannels = 0;
    std::uint16_t aBits = 0;
    std::uint64_t aDataBytes = 0;
    if (!probeWav(audioPath, &aRate, &aChannels, &aBits, &aDataBytes)) {
        std::printf("record-check FAIL audio WAV does not parse\n");
        return 1;
    }
    if (aRate != 48000u || aChannels != 1u || aBits != 16u) {
        std::printf(
            "record-check FAIL audio header rate=%lu channels=%u bits=%u "
            "(want 48000/1/16)\n",
            static_cast<unsigned long>(aRate), aChannels, aBits);
        return 1;
    }
    if (aDataBytes != 2u * audioSamples) {
        std::printf("record-check FAIL audio data bytes %llu != 2 x %llu\n",
                    static_cast<unsigned long long>(aDataBytes),
                    static_cast<unsigned long long>(audioSamples));
        return 1;
    }

    std::printf("record-check PASS iq_samples=%llu audio_samples=%llu\n",
                static_cast<unsigned long long>(iqSamples),
                static_cast<unsigned long long>(audioSamples));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // THE ENUMERATION HELPER, decided before anything else in this function.
    //
    // `cascade --enumerate-json` is not a user-facing mode: it is the child
    // process AppWindow's device scan spawns so that a vendor driver faulting
    // mid-probe kills a throwaway process instead of the session. See
    // source/soapy_enum_proc.hpp for why nothing short of a process boundary
    // contains that fault.
    //
    // It returns HERE, above the config read, because a helper must touch
    // nothing else: no config file, no window, no telemetry marker. It does
    // NOT inherit the parent's diagnostics environment either - FOXSDR_DIAG_DIR
    // is set in some sessions and would otherwise arm capture on nothing more
    // than an inherited variable.
    //
    // The ONE thing it accepts is the parent's crash directory, passed
    // explicitly and only when the parent's own capture is armed. Without it
    // the walk - the most crash-prone path in this product - would run in a
    // process with no handler at all, turning a symbolised fault stack into an
    // exit code; see CRASH CAPTURE IN THE CHILD in source/soapy_enum_proc.hpp.
    //
    // The accepted forms are enumerated exhaustively, and argv[1] must be the
    // flag itself, so no combination of arguments to a real session can turn
    // it into a helper.
    constexpr const char* kCrashDirFlag = "--crash-dir=";
    if (argc >= 2 && std::strcmp(argv[1], "--enumerate-json") == 0) {
        const char* crashDir = nullptr;
        if (argc == 3 && std::strncmp(argv[2], kCrashDirFlag, std::strlen(kCrashDirFlag)) == 0) {
            crashDir = argv[2] + std::strlen(kCrashDirFlag);
        } else if (argc != 2) {
            std::fprintf(stderr, "cascade: --enumerate-json takes at most one %s argument\n",
                         kCrashDirFlag);
            return 2;
        }
        return cascade::source::runEnumerateHelper(crashDir);
    }

    // FIRST, before anything that could fault has had the chance. The four
    // registrations are made unconditionally so the stderr attribution is
    // always available; only the on-disk half is conditional.
    //
    // TWO SEPARATE QUESTIONS, and conflating them was a bug. `mayWrite` is
    // "is this a real session rather than a tool"; `wanted` is "did the user
    // ask for diagnostics", read from the config right here rather than a
    // constructor and a window later. The directory is still handed over when
    // the run may write, so turning diagnostics on in Settings mid-session has
    // somewhere to go - but nothing is created until it is switched on.
    const bool mayWrite = diagnosticsOnDisk(argc);
    cascade::core::CrashHandlerConfig crashCfg;
    if (mayWrite) { crashCfg.crashDir = cascade::core::diagCrashDir(); }
    // REGISTRATIONS FIRST, ON-DISK CAPTURE SECOND, and in that order for two
    // reasons. The switch cannot be known without reading the config, and
    // reading the config is itself something that can fault - so the filters go
    // on now, writing nothing, and the read below happens under them. Nothing
    // reaches the disk until the line after it.
    crashCfg.enabled = false;
    cascade::core::installCrashHandlers(crashCfg);

    const bool wanted = mayWrite && storedDiagnosticsEnabled(argc, argv);
    // Creates the crashes directory if and only if `wanted`; see
    // crash_handler.cpp. The minidump switch stays off until AppWindow::run()
    // applies the user's - a dump is process memory and is never armed on a
    // guess.
    cascade::core::setCrashCaptureEnabled(wanted, false);
    cascade::core::DiagLog::instance().configure(
        wanted ? cascade::core::diagLogDir() : std::string(), wanted);
    cascade::core::diagLogf("FoxSDR %s (%s) starting", cascade::versionString(),
                            cascade::gitCommit());

    int frames = -1;  // negative: run until the window is closed
    int diagStallMs = 0;
    int diagToggle = 0;
    bool selftest = false;
    bool soapyCheck = false;
    bool recordCheck = false;
    bool toneCheck = false;
    double rdsCheckMhz = 0.0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "cascade: --frames requires an integer argument\n");
                return 1;
            }
            char* end = nullptr;
            const long value = std::strtol(argv[i + 1], &end, 10);
            // Reject trailing junk and negatives; cap so the int cast below
            // cannot overflow on LP64-style long values.
            if (end == argv[i + 1] || *end != '\0' || value < 0 || value > 1000000L) {
                std::fprintf(stderr, "cascade: invalid --frames value '%s'\n", argv[i + 1]);
                return 1;
            }
            frames = static_cast<int>(value);
            ++i;  // consumed the value argument
        } else if (std::strcmp(argv[i], "--update-check") == 0) {
            // Support tool, and the way this path is verified without driving
            // the GUI: ask the real endpoint what this build should be told.
            // Kept out of the usage string for the same reason as the other
            // diagnostics - it makes a network call.
            cascade::core::UpdateInfo info;
            std::string err;
            if (!cascade::core::checkForUpdate(cascade::core::updateEndpoint(),
                                               cascade::versionString(), "", info, err)) {
                std::printf("update-check FAIL %s\n", err.c_str());
                return 1;
            }
            std::printf("update-check running=%s latest=%s newer=%d critical=%d\n",
                        cascade::versionString(), info.version.c_str(), info.newer ? 1 : 0,
                        info.critical ? 1 : 0);
            if (info.newer) {
                std::printf("update-check url=%s\n", info.url.c_str());
                std::printf("update-check sha256=%s\n", info.sha256.c_str());
                // "--update-check download" runs the same download-and-verify
                // the button runs, without installing anything. It exists so
                // that path can be exercised on a machine where actually
                // installing over the top would be unwelcome, and so a support
                // conversation can establish whether a download verifies
                // before anyone reinstalls.
                if (i + 1 < argc && std::strcmp(argv[i + 1], "download") == 0) {
                    std::string path;
                    std::string derr;
                    if (!cascade::core::downloadUpdate(info, path, derr)) {
                        std::printf("update-check DOWNLOAD FAIL %s\n", derr.c_str());
                        return 1;
                    }
                    std::printf("update-check downloaded and verified: %s\n", path.c_str());
                }
                for (const cascade::core::ReleaseNote& n : info.notes) {
                    std::printf("update-check note %s (%s)%s\n", n.version.c_str(),
                                n.date.c_str(), n.critical ? " CRITICAL" : "");
                    for (const std::string& line : n.notes) {
                        std::printf("    - %s\n", line.c_str());
                    }
                }
            }
            return 0;
        } else if (std::strcmp(argv[i], "--version") == 0) {
            // One line, on stdout, and nothing else: this is what the release
            // and nightly scripts read back to check that the binary agrees
            // with the version its installer is about to be named after. A
            // nightly whose binary called itself by the release version would
            // produce bug reports naming a build that does not exist.
            std::printf("%s %s\n", cascade::appName(), cascade::versionString());
            return 0;
        } else if (std::strcmp(argv[i], "--selftest") == 0) {
            selftest = true;
        } else if (std::strcmp(argv[i], "--soapy-check") == 0) {
            // Hidden bench diagnostic (see runSoapyCheck): not in the usage
            // string on purpose — it requires attached hardware, so
            // advertising it in CI-facing help would only invite red herrings.
            soapyCheck = true;
        } else if (std::strcmp(argv[i], "--record-check") == 0) {
            // Hidden bench diagnostic (see runRecordCheck): same policy as
            // --soapy-check — kept out of the usage string because its
            // ~1 s real-time sleep and disk writes make it a bench tool,
            // not part of the CI-facing CLI surface.
            recordCheck = true;
        } else if (std::strcmp(argv[i], "--rds-check") == 0) {
            // Hidden bench diagnostic (see runRdsCheck): needs a real radio
            // tuned to a real RDS broadcast, so never a ctest entry.
            if (i + 1 >= argc) {
                std::fprintf(stderr, "cascade: --rds-check requires a frequency in MHz\n");
                return 1;
            }
            char* end = nullptr;
            rdsCheckMhz = std::strtod(argv[i + 1], &end);
            if (end == argv[i + 1] || !(rdsCheckMhz > 0.0)) {
                std::fprintf(stderr, "cascade: invalid --rds-check frequency '%s'\n",
                             argv[i + 1]);
                return 1;
            }
            ++i;
        } else if (std::strcmp(argv[i], "--diag-stall") == 0) {
            // Hidden diagnostic: wedge the frame loop once, N ms into the run,
            // so the SHIPPED hang threshold can be proved against the REAL
            // frame loop rather than against a unit test of the watchdog
            // class. Kept out of the usage string because deliberately
            // freezing the application is not a user-facing feature.
            if (i + 1 >= argc) {
                std::fprintf(stderr, "cascade: --diag-stall requires a duration in ms\n");
                return 1;
            }
            char* end = nullptr;
            const long value = std::strtol(argv[i + 1], &end, 10);
            if (end == argv[i + 1] || *end != '\0' || value <= 0 || value > 600000L) {
                std::fprintf(stderr, "cascade: invalid --diag-stall value '%s'\n", argv[i + 1]);
                return 1;
            }
            diagStallMs = static_cast<int>(value);
            ++i;
        } else if (std::strcmp(argv[i], "--diag-toggle") == 0) {
            // Hidden diagnostic: flip Settings > Diagnostics mid-session, on
            // frame 30, through the same function the checkbox calls. The
            // switch's mid-session behaviour is a privacy promise made in
            // three documents, and it was broken for the watchdog precisely
            // because nothing could exercise the checkbox from a test. Kept
            // out of the usage string for the same reason --diag-stall is.
            if (i + 1 >= argc) {
                std::fprintf(stderr, "cascade: --diag-toggle requires 'on' or 'off'\n");
                return 1;
            }
            if (std::strcmp(argv[i + 1], "on") == 0) {
                diagToggle = 1;
            } else if (std::strcmp(argv[i + 1], "off") == 0) {
                diagToggle = -1;
            } else {
                std::fprintf(stderr, "cascade: invalid --diag-toggle value '%s'\n", argv[i + 1]);
                return 1;
            }
            ++i;
        } else if (std::strcmp(argv[i], "--tone-check") == 0) {
            // Hidden bench diagnostic (see runToneCheck): plays audible sound,
            // so it is a human-in-the-loop tool, never a ctest entry.
            toneCheck = true;
        } else {
            std::fprintf(stderr,
                         "cascade: unknown argument '%s' (usage: cascade [--frames N] "
                         "[--selftest] [--version])\n",
                         argv[i]);
            return 1;
        }
    }

    // Headless mode wins over --frames: a selftest that opened a window would
    // defeat its purpose (running where no display exists). Same for the
    // hardware bench check, which is likewise headless.
    if (selftest) { return runSelftest(); }
    if (soapyCheck) { return runSoapyCheck(); }
    if (recordCheck) { return runRecordCheck(); }
    if (toneCheck) { return runToneCheck(); }
    if (rdsCheckMhz > 0.0) { return runRdsCheck(rdsCheckMhz * 1.0e6); }

    // Config persistence wiring. Normal interactive runs load/save the
    // user's config at ConfigStore::defaultPath(). Bounded --frames runs
    // stay hermetic (no load, no save — the CI contract) UNLESS the
    // CASCADE_CONFIG_TEST env var supplies an explicit path: that is the
    // documented test hook that forces a config load+save round trip against
    // a caller-owned file and switches on AppWindow's one-line
    // "config applied: ..." diagnostic. It is honored ONLY with --frames, so
    // no interactive run can be silently redirected by a stray env var.
    std::string configPath;
    bool announceConfig = false;
    if (frames >= 0) {
        const char* hook = std::getenv("CASCADE_CONFIG_TEST");
        if (hook != nullptr && *hook != '\0') {
            configPath = hook;
            announceConfig = true;
        }
    } else {
        configPath = cascade::core::ConfigStore::defaultPath();
    }

    cascade::gui::AppWindow app(configPath, announceConfig);
    // Empty unless this run is ALLOWED to write - which is not the same as
    // whether the user wants it. run() gates the writing on the stored switch;
    // handing over the directory regardless is what lets the Settings toggle
    // work in a session that started with diagnostics off. The watchdog still
    // runs and still logs a recovered stall to the ring either way, but it puts
    // no file on a machine that only asked for a smoke test.
    app.setDiagnosticsDir(mayWrite ? cascade::core::diagCrashDir() : std::string());
    app.setDiagStallMs(diagStallMs);
    app.setDiagToggle(diagToggle);
    return app.run(frames);
}
