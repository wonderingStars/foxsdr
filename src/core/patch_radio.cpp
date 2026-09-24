// patch_radio.cpp - see patch_radio.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/patch_radio.hpp"

#include <algorithm>
#include <chrono>
#include <complex>

#include "core/diag_log.hpp"
#include "core/plugin_api.hpp"
#include "dsp/spectrum.hpp"

namespace cascade::core::patch {

namespace {
constexpr std::size_t kFftSize = 2048;
constexpr double kSpectrumEverySec = 0.05;
constexpr double kChunkSec = 0.010;
}  // namespace

struct PatchRadio::Shared {
    std::unique_ptr<cascade::source::IqSource> src;
    Runner runner;
    std::atomic<bool> run{false};
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> blocks{0};

    mutable std::mutex specMutex;
    std::vector<float> specDb;
    std::uint64_t specSeq = 0;

    mutable std::mutex faultMutex;
    std::string fault;

    void setFault(const std::string& why) {
        std::lock_guard<std::mutex> lock(faultMutex);
        if (fault.empty()) { fault = why; }
    }
};

namespace {

void readerBody(const std::shared_ptr<PatchRadio::Shared>& shp);

}  // namespace

PatchRadio::PatchRadio(NodeId node, std::unique_ptr<cascade::source::IqSource> source,
                       std::string label)
    : node_(node), label_(std::move(label)), sh_(std::make_shared<Shared>()) {
    sh_->src = std::move(source);
}

PatchRadio::~PatchRadio() { stop(); }

bool PatchRadio::start(std::string& error) {
    error.clear();
    if (started_) { return true; }
    if (!sh_->src) {
        error = "no source";
        return false;
    }
    if (!sh_->src->start()) {
        const char* why = sh_->src->lastError();
        error = (why != nullptr && *why != '\0') ? why : "the device would not start streaming";
        return false;
    }
    sh_->run.store(true, std::memory_order_release);
    sh_->done.store(false, std::memory_order_release);
    std::shared_ptr<Shared> shp = sh_;
    thread_ = std::thread([shp] { readerBody(shp); });
    started_ = true;
    return true;
}

void PatchRadio::stop() {
    if (!started_) { return; }
    started_ = false;
    sh_->run.store(false, std::memory_order_release);
    // The source's own stop() is what gets a device's blocking read out.
    if (sh_->src) { sh_->src->stop(); }
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kStopWaitMs);
    while (!sh_->done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (sh_->done.load(std::memory_order_acquire)) {
        thread_.join();
    } else {
        // ABANDONED, not waited on: the thread holds its own reference to the
        // source and the runner, so when the driver finally lets it go it
        // finds live memory, sees run == false and returns.
        cascade::core::diagWarnf(
            "patch: radio '%s' did not stop within %d ms - its reader is left to finish "
            "on its own",
            label_.c_str(), kStopWaitMs);
        // BUT ITS DECODERS ARE NOT LEFT WITH IT. The old Shared owns the
        // runner, and the runner owns every decoder plugin handle this radio
        // runs. Once sh_ is swapped below, nothing else can reach that runner
        // - the app's plugin-unload flush walks the radios it still holds, and
        // this one has just been dropped - so without this the handles died
        // whenever the driver let the thread go: destroy() on the reader
        // thread, which the ABI forbids, and after a rescan or at exit into a
        // plugin module already unmapped. flushNow() destroys them HERE, on
        // the control thread, now. Its wait is for the reader to leave
        // runner.process(), and the reader is not in there: it is parked in
        // the driver's read(), outside the runner, which is why it was
        // abandoned. When it does come back it finds an empty runner and
        // run == false, and leaves without touching plugin code. (Were the
        // reader instead stuck inside a plugin's own process() call, this
        // waits for that call, exactly as the unload path's flushNow() on the
        // same runner always has: destroying a handle under its own running
        // call would be the crash this exists to prevent.)
        sh_->runner.flushNow();
        thread_.detach();
        // A fresh Shared for this object, so nothing it does from here on can
        // touch what the abandoned thread still holds.
        sh_ = std::make_shared<Shared>();
    }
}

bool PatchRadio::running() const {
    return started_ && !sh_->done.load(std::memory_order_acquire);
}

double PatchRadio::rateHz() const { return sh_->src ? sh_->src->sampleRateHz() : 0.0; }

double PatchRadio::centreHz() const { return sh_->src ? sh_->src->centerFrequencyHz() : 0.0; }

bool PatchRadio::setCentreHz(double hz) {
    return sh_->src ? sh_->src->setCenterFrequencyHz(hz) : false;
}

Runner& PatchRadio::runner() { return sh_->runner; }

bool PatchRadio::spectrum(std::vector<float>& db, std::uint64_t& seq) const {
    std::lock_guard<std::mutex> lock(sh_->specMutex);
    if (sh_->specSeq == seq || sh_->specDb.empty()) { return false; }
    db = sh_->specDb;
    seq = sh_->specSeq;
    return true;
}

std::string PatchRadio::fault() const {
    std::lock_guard<std::mutex> lock(sh_->faultMutex);
    return sh_->fault;
}

std::uint64_t PatchRadio::blocksRead() const {
    return sh_->blocks.load(std::memory_order_relaxed);
}

namespace {

void readerBody(const std::shared_ptr<PatchRadio::Shared>& shp) {
    // A patch radio's reader runs its decoders' process() calls, so it is a
    // real-time thread for the plugin API exactly as the pipeline's DSP
    // thread is (see core/plugin_api.hpp, RealtimeThreadScope).
    const cascade::core::RealtimeThreadScope realtime;
    PatchRadio::Shared& sh = *shp;
    cascade::source::IqSource& src = *sh.src;
    using clock = std::chrono::steady_clock;

    double rate = src.sampleRateHz();
    if (!(rate > 0.0)) { rate = 2.0e6; }
    std::size_t chunk = static_cast<std::size_t>(rate * kChunkSec + 0.5);
    if (chunk < 1) { chunk = 1; }
    std::vector<std::complex<float>> buf(chunk);

    cascade::dsp::SpectrumEstimator est(kFftSize, cascade::dsp::WindowType::BlackmanHarris);
    est.setAlpha(0.35f);
    std::vector<std::complex<float>> fftIn(kFftSize);
    std::size_t fftFill = 0;
    std::vector<float> db(kFftSize);
    auto lastSpectrum = clock::now();

    const bool paced = src.selfPaced();
    const auto t0 = clock::now();
    std::uint64_t produced = 0;

    while (sh.run.load(std::memory_order_acquire)) {
        if (!paced) {
            // The generator has no clock of its own: hold it to real time.
            const auto due =
                t0 + std::chrono::duration_cast<clock::duration>(
                         std::chrono::duration<double>(static_cast<double>(produced) / rate));
            std::this_thread::sleep_until(due);
        }
        const std::size_t got = src.read(buf.data(), chunk);
        if (!sh.run.load(std::memory_order_acquire)) { break; }
        if (src.faulted()) {
            const char* why = src.lastError();
            sh.setFault((why != nullptr && *why != '\0') ? why : "the device stopped");
            break;
        }
        if (got == 0) { continue; }
        produced += got;
        sh.blocks.fetch_add(1, std::memory_order_relaxed);

        sh.runner.process(buf.data(), got);

        // The spectrum: the newest kFftSize samples, about twenty times a second.
        const std::size_t take = std::min(got, kFftSize - fftFill);
        std::copy(buf.data() + (got - take), buf.data() + got, fftIn.begin() + fftFill);
        fftFill += take;
        if (fftFill >= kFftSize) {
            fftFill = 0;
            const auto now = clock::now();
            if (std::chrono::duration<double>(now - lastSpectrum).count() >= kSpectrumEverySec) {
                lastSpectrum = now;
                est.process(fftIn.data(), db.data());
                std::lock_guard<std::mutex> lock(sh.specMutex);
                sh.specDb = db;
                ++sh.specSeq;
            }
        }
    }
    sh.done.store(true, std::memory_order_release);
}

}  // namespace

}  // namespace cascade::core::patch
