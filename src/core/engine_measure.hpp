// engine_measure.hpp - the measurement driver behind tools/measure_engine.ps1:
// the numbers the engine extraction is gated on, produced by the application
// itself rather than re-derived by hand.
//
// WHY THIS EXISTS. The engine is to be moved out of the window in stages, and
// the promise is that nothing gets slower. That is only a promise if the
// numbers are taken the same way before and after every stage, by a script,
// on the same machine - see docs/ENGINE-EXTRACTION.md section 3 in the
// foxsdr-api repository. Three of the measures can only be taken from INSIDE
// the process, because they are about the engine's own clocks:
//
//   run      the receiver running at a fixed rate off the built-in signal
//            generator for a fixed time; reports the process CPU and the
//            ring's dropped samples over the window after the warm-up, and
//            the working set at the end; also how many decoder instances
//            were being fed and how many frames they were handed, and how
//            often the VFO offset moved (the interface-busy run's proof that
//            its scripted slider drag landed). (Frame times come from the
//            frame log, gui/frame_log.hpp, which runs beside this.)
//   rates    a ladder of signal-generator rates, one window each, reporting
//            Pipeline::ringDroppedSamples() over each window. The highest
//            rate with zero drops is the sustained-rate figure.
//   latency  tune-to-audio: a tone at +100 kHz, the VFO moved off it and
//            back on through the SAME call the spectrum's click-to-tune
//            makes, and the audio sample clock read from the command to the
//            first block in audioTap() whose tone power crosses half its
//            steady value.
//
// ENGINE SIDE ON PURPOSE. It needs a Pipeline and nothing of the window: the
// three things only the interface can do (start the receiver the way its key
// does, select a mode the way its button does, tune the way a click does) are
// handed in as hooks, so the same driver can later be pointed at an engine
// driven through the API instead.
//
// BOUNDED RUNS ONLY, and inert unless FOXSDR_MEASURE is set: AppWindow asks
// fromEnvironment() once, in a --frames run, and holds nothing otherwise, so
// an ordinary session pays one null test per frame.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cascade::source {
class IqSource;
}

namespace cascade::core {

class Pipeline;

// --- the pure half: what a test can pin ---------------------------------------
namespace measure {

// Mean-square amplitude of the component of x at freqHz (a single-bin DFT, the
// Goertzel recurrence). A full-scale sine at exactly freqHz over a whole
// number of cycles reads 0.5, as its mean square does; 0 for n == 0.
double tonePower(const float* x, std::size_t n, double freqHz, double rateHz);

// The frequency in [loHz, hiHz], stepped by stepHz, at which tonePower is
// greatest. Used once, to find where the demodulator actually put the tone,
// rather than assuming how a mode places its carrier.
double dominantFrequencyHz(const float* x, std::size_t n, double rateHz, double loHz,
                           double hiHz, double stepHz);

// THE CROSSING SCAN. The audio tap is a window of the newest `n` samples,
// the last of which has absolute index `endIndex` - 1 (absolute = counted by
// Pipeline::audioSamplesProduced()). Blocks of `block` samples are laid from
// `commandIndex` (the sample clock when the retune was submitted); `nextBlock`
// is the absolute start of the first block not yet judged, advanced past every
// block judged here. Returns the absolute END index of the first block whose
// tonePower is >= threshold, or -1 if none of the complete blocks in the
// window crosses. `missed` is set when a block that should have been judged
// has already left the window (the reader fell more than the tap's length
// behind), which makes that retune's figure untrustworthy.
std::int64_t scanForCrossing(const float* tap, std::size_t n, std::uint64_t endIndex,
                             std::uint64_t& nextBlock, std::size_t block, double freqHz,
                             double rateHz, double threshold, bool& missed);

}  // namespace measure

// What only the interface can do, as the interface does it.
struct MeasureHooks {
    std::function<void()> startReceiver;               // the START key's own path
    std::function<void(const char* mode)> setMode;     // "WFM", "USB", ... as the buttons
    std::function<void(double offsetHz)> tuneVfoHz;    // click-to-tune, offset from centre
    // A source swap as the interface makes one: the pipeline's setSource
    // followed by whatever the interface does after every swap (the app's
    // followInputRate: DSP chain, frequency axis, decoders). Without it the
    // driver swaps and makes only the DSP chain follow.
    std::function<void(std::unique_ptr<cascade::source::IqSource>)> installSource;
};

class EngineMeasure {
public:
    enum class Mode { Run, Rates, Latency };

    // Reads FOXSDR_MEASURE (run | rates | latency) and its companions; null
    // when it is unset or names nothing this build knows (the reason is
    // printed, so a misspelt mode is not a silent ordinary run).
    //   FOXSDR_MEASURE_OUT      where the result is written (required)
    //   FOXSDR_MEASURE_SECONDS  run: total length, warm-up included (65)
    //   FOXSDR_MEASURE_WARMUP   seconds discarded at the start of each window (5)
    //   FOXSDR_MEASURE_RATE     run/latency: signal generator rate, Hz (2048000)
    //   FOXSDR_MEASURE_RATES    rates: comma-separated ladder, Hz, ascending
    //   FOXSDR_MEASURE_WINDOW   rates: seconds measured per rate (60)
    //   FOXSDR_MEASURE_RETUNES  latency: retunes per run (50)
    static std::unique_ptr<EngineMeasure> fromEnvironment();

    // Once a frame, on the thread that owns the pipeline. False when the
    // measurement has finished and written its result: the caller closes.
    bool tick(Pipeline& p, const MeasureHooks& h);

    Mode mode() const { return mode_; }

private:
    struct RateStep {
        double requestedHz = 0.0;
        double inputRateHz = 0.0;
        std::uint64_t dropped = 0;
        std::uint64_t audioSamples = 0;
        double seconds = 0.0;
    };
    struct Retune {
        double latencyMs = -1.0;     // sample clock, command to crossing block end
        double wallMs = -1.0;        // steady clock, command to the frame that saw it
        double awayRatio = 0.0;      // off-tone power / steady, just before the command
        bool missed = false;
    };

    EngineMeasure() = default;
    double now() const;  // seconds since the first tick
    void installGenerator(Pipeline& p, const MeasureHooks& h, double rateHz, bool latencyTone);
    bool tickRun(Pipeline& p, const MeasureHooks& h);
    bool tickRates(Pipeline& p, const MeasureHooks& h);
    bool tickLatency(Pipeline& p, const MeasureHooks& h);
    std::size_t readTap(Pipeline& p, std::uint64_t& endIndex);
    void finish(Pipeline& p);

    Mode mode_ = Mode::Run;
    std::string outPath_;
    double seconds_ = 65.0;
    double warmup_ = 5.0;
    double rateHz_ = 2048000.0;
    std::vector<double> ladder_;
    double window_ = 60.0;
    int retunes_ = 50;

    bool started_ = false;
    std::int64_t t0Ns_ = 0;
    int phase_ = 0;
    double phaseAt_ = 0.0;
    // run
    double cpu0_ = 0.0, cpu1_ = 0.0, runWindow_ = 0.0;
    double cyc0_ = -1.0, cyc1_ = -1.0;  // processCycles(); -1 = none
    std::uint64_t drop0_ = 0, drop1_ = 0, audio0_ = 0, audio1_ = 0;
    std::uint64_t workingSet_ = 0, peakWorkingSet_ = 0;
    // THE DECODERS THE FIGURE WAS TAKEN BESIDE. Section 3 measures frame time
    // "with three decoders open"; a build whose decoders were never created,
    // or created and never fed, does less work and would read faster. So the
    // result says, from the plugin runner itself, how many instances were
    // being fed at the end of the window and how many frames they were handed
    // across it (measure_engine.ps1 refuses a run that says none).
    void readDecoders(Pipeline& p, std::size_t& active, std::uint64_t& audioFed,
                      std::uint64_t& iqFed, std::string* statusJson) const;
    std::size_t decodersActive_ = 0;
    std::uint64_t decAudio0_ = 0, decAudio1_ = 0, decIq0_ = 0, decIq1_ = 0;
    std::string decoderStatusJson_;
    // THE INTERFACE-BUSY RUN'S PROOF. Frames in the window, and how many of
    // them found the VFO offset changed since the frame before: a scripted
    // drag of the VFO slider (FOXSDR_INPUT_SCRIPT) moves it every frame, and
    // a run whose script missed the slider reads as an idle one.
    std::uint64_t ticks_ = 0, vfoChanges_ = 0;
    double lastVfoHz_ = 0.0;
    // rates
    std::size_t step_ = 0;
    std::vector<RateStep> steps_;
    // latency
    std::vector<float> tap_;
    double toneHz_ = 0.0;
    double steady_ = 0.0;
    double onOffsetHz_ = 99000.0;
    double awayOffsetHz_ = -200000.0;
    std::uint64_t commandIndex_ = 0;
    std::uint64_t nextBlock_ = 0;
    std::int64_t commandNs_ = 0;
    double awayRatio_ = 0.0;
    bool detected_ = false;
    // Ring drops over the retunes (from the first retune to the result): the
    // latency figure assumes a chain that is keeping up.
    bool latDropArmed_ = false;
    std::uint64_t latDrop0_ = 0;
    std::vector<Retune> results_;
    std::string error_;
};

}  // namespace cascade::core
