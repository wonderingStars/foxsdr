// SPDX-License-Identifier: MIT
//
// Thin entry point: parse the command line, hand off to the GUI shell.
// `--frames N` renders exactly N frames then exits 0 — the bounded-run
// contract the app_smoke ctest entry relies on. `--selftest` runs the
// pipeline headless (no window, no GL, no audio device) and checks that the
// demo signal's peak lands on the right FFT bin AND that the audio chain
// demodulates a CW carrier to its 700 Hz sidetone. No flag: run until the
// window is closed.
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
#include "core/recorder.hpp"
#include "gui/app_window.hpp"
#include "source/iq_file_source.hpp"
#include "source/soapy_source.hpp"

// ---------------------------------------------------------------------------
// Minimal fatal-crash attribution (kept from the P6a crash investigation,
// 2026-08-15). One line to stderr naming the exception code and the faulting
// address as module+offset — this is exactly how the intermittent 0xC0000005
// inside libusb-1.0.dll (SoapyUHD USB discovery) was pinned, and it costs
// nothing at runtime. Vendor SDR modules are runtime-loaded third-party code;
// when one faults in the field, this line is the difference between a report
// that says "crashed" and one that names the module.
// ---------------------------------------------------------------------------
#ifdef _WIN32
#include <windows.h>

#include <psapi.h>

#include <cinttypes>
#pragma comment(lib, "psapi.lib")

namespace {

// Resolve an address to "module.dll+0xOFFSET" and report on stderr. Stack
// buffer + WriteFile (not fprintf): the filter may run on a corrupted heap,
// so the report path must not allocate.
LONG WINAPI crashSehFilter(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    char modName[MAX_PATH] = "?";
    std::uintptr_t offset = 0;
    HMODULE mods[1024];
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        const std::size_t n = needed / sizeof(HMODULE);
        for (std::size_t i = 0; i < n && i < 1024; ++i) {
            MODULEINFO mi{};
            if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi,
                                      sizeof(mi))) {
                continue;
            }
            const auto base = reinterpret_cast<std::uintptr_t>(mi.lpBaseOfDll);
            const auto a = reinterpret_cast<std::uintptr_t>(addr);
            if (a >= base && a < base + mi.SizeOfImage) {
                GetModuleFileNameA(mods[i], modName, sizeof(modName));
                offset = a - base;
                break;
            }
        }
    }
    char buf[MAX_PATH + 128];
    const int len = std::snprintf(
        buf, sizeof(buf), "cascade: fatal exception 0x%08lX at %s+0x%" PRIxPTR "\n",
        static_cast<unsigned long>(code), modName, offset);
    if (len > 0) {
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, static_cast<DWORD>(len),
                  &written, nullptr);
    }
    return EXCEPTION_CONTINUE_SEARCH;  // die with the original exit code
}

void installCrashReporter() {
    SetUnhandledExceptionFilter(&crashSehFilter);
}

}  // namespace
#else
namespace {
void installCrashReporter() {}
}  // namespace
#endif

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
    installCrashReporter();
    int frames = -1;  // negative: run until the window is closed
    bool selftest = false;
    bool soapyCheck = false;
    bool recordCheck = false;
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
        } else {
            std::fprintf(stderr,
                         "cascade: unknown argument '%s' (usage: cascade [--frames N] [--selftest])\n",
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
    return app.run(frames);
}
