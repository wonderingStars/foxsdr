// screen_first.hpp - the first screen: the built-in signal generator, through
// the real spectrum estimator, drawn on the phone.
//
// WHAT THIS IS FOR. It is the proof that the shell works end to end, and it is
// deliberately built out of the SAME pieces the desktop uses rather than a
// mock: source::SigGenSource is the hardware-free IqSource the desktop's
// Pipeline runs when no radio is attached, and dsp::SpectrumEstimator is the
// estimator that feeds the desktop's spectrum well. If a trace appears on the
// phone, then the DSP compiles and runs correctly for this ABI, the worker
// threading works, the EGL context is alive, ImGui is drawing, and the theme
// and fonts loaded - and each of those is a thing the next slice would
// otherwise have to debug through the whole AppWindow at once.
//
// IT IS NOT THE PRODUCT'S SPECTRUM VIEW. gui/spectrum_view.cpp is 900 lines of
// instrument face - dB ladder, band plan overlay, VFO handles, peak readout,
// context menus - and it belongs to the AppWindow slice. This draws a trace, a
// grid and a caption, because that is what "the shell runs" needs to show.
//
// THREADING, and it mirrors Pipeline's split. A worker thread pulls blocks
// from the generator, runs the estimator and publishes the dB bins under a
// mutex; the GUI thread copies the newest set once a frame. The GUI thread
// never touches the generator or the estimator, exactly as on the desktop.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_PLATFORM_ANDROID_SCREEN_FIRST_HPP
#define CASCADE_PLATFORM_ANDROID_SCREEN_FIRST_HPP

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "dsp/spectrum.hpp"
#include "imgui.h"
#include "source/siggen_source.hpp"

namespace cascade::platform::android {

// What the screen reports about the machine it is running on. Gathered by the
// frame loop, which is the only thing that knows any of it.
struct ShellStatus {
    int surfaceWidth = 0;
    int surfaceHeight = 0;
    int densityDpi = 160;
    float densityScale = 1.0f;
    const char* glVersion = "(unknown)";
    const char* glRenderer = "(unknown)";
    const char* abi = "(unknown)";
    float frameMs = 0.0f;
    const char* appVersion = "(unknown)";
};

class FirstScreen {
public:
    FirstScreen();
    ~FirstScreen();

    FirstScreen(const FirstScreen&) = delete;
    FirstScreen& operator=(const FirstScreen&) = delete;

    // Starts and stops the DSP worker. The frame loop stops it when the
    // activity loses focus: a phone in a pocket has no reason to be running an
    // FFT every 500 microseconds, and this is the single largest thing the
    // application can stop doing to leave the battery alone.
    void setRunning(bool running);
    bool running() const { return run_.load(std::memory_order_relaxed); }

    // One frame. Call between ImGui::NewFrame and ImGui::Render.
    void draw(const ShellStatus& status);

private:
    void workerBody();

    // 2.048 MS/s: a rate a real dongle actually produces, so the numbers on
    // screen are the ones a receiver would show rather than a round invention.
    static constexpr double kSampleRateHz = 2.048e6;
    static constexpr std::size_t kFftSize = 1024;

    source::SigGenSource gen_{kSampleRateHz};
    dsp::SpectrumEstimator estimator_{kFftSize, dsp::WindowType::BlackmanHarris};

    std::thread worker_;
    std::atomic<bool> run_{false};

    std::mutex binsMutex_;
    std::vector<float> bins_;             // guarded: fftshifted dB, kFftSize long
    std::uint64_t seq_ = 0;               // guarded: strictly increasing
    std::atomic<std::uint64_t> blocks_{0};  // total blocks the worker has run

    // THE ONE INTERACTIVE THING ON THE SCREEN, and it earns its place twice
    // over: it is the only way to tell from a screenshot that touch reached
    // ImGui at all, and because muting a tone changes the trace, it proves the
    // whole chain - finger, backend, ImGui, generator, estimator, draw - in one
    // gesture rather than only the near end of it.
    //
    // Configuring a slot while the worker's read() runs is the benign
    // concurrency SigGen documents and the desktop Pipeline already relies on:
    // the tone's NCO owns its own phase and a retune is click-free.
    struct ToneSlot {
        double freqHz = 0.0;
        float amplitudeDb = 0.0f;
        bool on = true;
    };
    std::array<ToneSlot, 4> toneSlots_{};

    // GUI-thread only.
    std::vector<float> drawBins_;
    std::vector<ImVec2> points_;
};

}  // namespace cascade::platform::android

#endif  // CASCADE_PLATFORM_ANDROID_SCREEN_FIRST_HPP
