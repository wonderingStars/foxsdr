// lanes2_publish.hpp - SCREEN LANE S2's PUBLISHER (docs/multi-radio-records/screen-s2.md): the LaneS2 global of the Slint window filled and its act(id, text) answered, over the pure rules of lanes2_logic.hpp.
//
// Five row lists and one picture, mounted by two panels in main.slint (SYSTEM, Spectrum tools and SYSTEM, Tuning and memories): the spectrum's FFT size, window and frame rate; the IF zoom with its
// picture; the waterfall's autosave and the power and SNR log; centre tuning, VFO A / B and band stacking; the several VFOs of the selected radio. Every control READS the engine's own state for what it
// shows (the receiver's state record and its facts, read again about twice a second and at once after a change made here) and sends the engine's own operation; what the person typed and has not sent is
// kept as a draft and is never saved. The host's own little jobs (the autosave of a picture, the folder chooser, the zoom frame) come in through the Env so a test can stand in for them.
// Nothing here keys a transmitter or opens a radio; nothing writes to the disk until the person switches that job on.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <slint.h>

#include "foxsdr_api.h"
#include "main.h"

namespace foxsdr::slints2 {

// What one submit came to.
struct SubmitOutcome {
    std::uint64_t ticket = 0;      // 0 = the engine never took it
    int status = FOXAPI_OK;        // what submit answered
    std::string message;           // the host's own words when the call refused it
};

// What the host's picture writer did with a request to save the waterfall.
enum class PictureStart { Started = 0, NoSpectrum, Busy };
struct PictureDone { bool ok = false; std::string path, error; };

class LaneS2Publisher {
public:
    struct Env {
        MainWindow* ui = nullptr;
        const FoxEngineApi* api = nullptr;
        FoxSession* session = nullptr;
        bool bounded = false;
        std::string configDir;                    // the per-user folder: the band stack's file lives there
        // One command; `radio` is the radio's number (1-based) it is aimed at, 0 = the selected radio. `longText` is the command's text when it is longer than the struct's slot.
        std::function<SubmitOutcome(const FoxCommand&, std::uint32_t radio, const std::string* longText)> submit;
        std::function<int()> selectedRadio;       // 1-based
        // The newest zoom frame of a radio (Engine::zoomFrame): fills `bins` (dB, FFT-shifted) and the picture's span; false while there is none.
        std::function<bool(std::uint32_t radio, std::vector<float>& bins, double& binHz, double& spanHz, double& usableSpanHz)> zoomFrame;
        // The waterfall of the selected radio as a PNG file at `path` + ".png" (the host captures the picture and a disk job writes it, off the frame); `pollPicture` returns the finished one.
        std::function<PictureStart(const std::string& path)> startPicture;
        std::function<bool(PictureDone&)> pollPicture;
        // The folder chooser behind the interface: the folder the person picked, "" when they cancelled. The real one is a Windows dialog; a test stands in.
        std::function<std::string()> chooseFolder;
        std::function<double()> steadyMs;         // a clock in ms for the autosave's interval (a test moves it)
        std::function<std::int64_t()> utcMs;      // wall time in ms since 1970 for a file name
    };
    explicit LaneS2Publisher(Env env);
    ~LaneS2Publisher();
    LaneS2Publisher(const LaneS2Publisher&) = delete;
    LaneS2Publisher& operator=(const LaneS2Publisher&) = delete;

    void wire();                                                          // registers LaneS2.act (once, after the window is created)
    void tick(const FoxReceiverState& selectedState, long tickNo);         // refreshes the rows whose facts moved (a few times a second) and runs the autosave
    void onAnswer(const FoxCommandResult& r);                              // the engine's answer to an op this publisher sent (others are ignored)
    // One frame of a radio's spectrum (called for every radio the window draws): feeds the power and SNR log while it is on. Cheap when it is off.
    void frameSeen(int radio, const float* bins, std::size_t n, double centreHz, double spanHz, double tunedHz, double bandwidthHz);
    // `state s2.<what>` for the drive scripts
    std::string readState(const std::string& what, std::string& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace foxsdr::slints2
