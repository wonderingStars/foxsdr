// lanek_publish.hpp - SCREEN LANE K's PUBLISHER (docs/multi-radio-records/screen-k.md): the LaneK and LaneKMatrix globals of the Slint window filled and their act(id, text) answered, over the pure rules
// of lanek_logic.hpp and the models of the engine lanes sysmodels, access, rfmon, dspmodels and sigmeas.
//
// Sections it fills (SYSTEM bank, after About): Settings and backup, Performance, Accessibility, Community, Spectrum tools; the Display body's foot; the generic sheet (the first-run setup,
// the confirmations); the Matrix sheet. Nothing here writes the settings file under a running engine: an import, a reset, a profile switch and an undo are PENDING operations that the
// host carries out after the engine has made its last save (applyPendingAtExit).
//
// A BOUNDED RUN starts no program, opens no URL and plays no sound (the constructor's `bounded`).
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
#include "lanek_logic.hpp"
#include "main.h"
#include "host_logic.hpp"
#include "plugin_kit.hpp"

namespace foxsdr::slintlanek {

// What the Matrix needs to know of one open radio (the host builds it from its RadioView).
struct MatrixInput {
    int slot = 0;               // 0-based
    bool running = false;
    double tunedHz = 0.0;
    std::uint32_t demod = 0;    // FOXAPI_DEMOD_*
    double signalDb = -200.0;
    double sMeter = 0.0;        // 0..1
    bool muted = false;
};

class LaneKPublisher {
public:
    struct Env {
        MainWindow* ui = nullptr;
        const FoxEngineApi* api = nullptr;
        FoxSession* session = nullptr;
        std::string settingsDir;                  // the folder of the settings file ("" = nothing is kept)
        std::string dataDir;                      // %LOCALAPPDATA%/FoxSDR
        std::string exportsDir;                   // where exports go
        std::string version;
        bool bounded = false;
        bool firstRun = false;                    // a new profile: the setup opens at the first tick
        std::function<void(const FoxCommand&, std::uint32_t radio)> submit;
        std::function<double()> nowMs;            // a steady clock
        std::function<std::int64_t()> unixNow;
        std::function<int()> selectedRadio;       // 1-based
        std::function<int()> soloRadio;           // the radio heard alone, 0 = nobody
        std::function<std::vector<MatrixInput>()> matrix;
        std::function<Skin()> baseSkin;           // the skin of the theme in force, before lane K's palette and effects
        std::function<void()> quit;               // ends the event loop
    };
    explicit LaneKPublisher(Env env);
    ~LaneKPublisher();
    LaneKPublisher(const LaneKPublisher&) = delete;
    LaneKPublisher& operator=(const LaneKPublisher&) = delete;

    void wire();                                  // registers LaneK.act and LaneKMatrix.act (once, after the window is created)
    void tick(const FoxReceiverState& state, long tickNo);
    void flush();                                 // the record, if it changed

    // What the waterfall pacer and the skin should do now (reduced effects): the host reads these.
    bool reducedEffects() const;
    int waterfallRowsPerSecondCap() const;   // 0 = no cap
    void restyle();                               // the base skin with the palette and the effects over it

    // THE SPECTRUM TOOLS (the host's trace painter calls these; all of them are no-ops with everything off)
    struct TraceSource { const float* data = nullptr; std::size_t n = 0; };
    // The bins the trace is painted from: the averaged frame (Display > Averaging) reduced to `columns` points by the chosen detector (Spectrum tools). `full` is the whole frame, [b0, b0+subN) the zoomed part.
    TraceSource traceSource(int slot, const float* full, std::size_t fullN, std::size_t b0, std::size_t subN, double nowS, std::uint64_t key, std::size_t columns);
    // A new frame of a radio arrived (the selected one feeds the mask, the cursors' levels and the RBW readout).
    void frameSeen(int slot, bool selected, const float* full, std::size_t n, double centreHz, double spanHz, double sampleRateHz);
    // Draws the limit lines and the cursors over a trace picture showing [loHz, hiHz] x [dbMin, dbMax].
    void paintOver(foxsdr::slinthost::Rgba* trace, std::uint32_t w, std::uint32_t h, double loHz, double hiHz, double dbMin, double dbMax);
    const foxsdr::slinthost::Rgba* gradientTable() const;   // 256 entries, or nullptr while the user's own gradient is off
    bool takeGradientChanged();                              // true once after the gradient or its switch changed (the host clears the waterfall)
    bool placeCursorAt(double hz);                           // the spectrum's click while "place the next cursor" is on: true = the click was used

    // The community entries of the App menu: "" = hidden.
    std::string forumUrl() const;
    std::string discordUrl() const;
    void openCommunity(bool discord);
    bool overlayOpen() const;
    void closeOverlays();

    // THE END OF THE RUN, after the engine has stopped: the automatic backup, then the pending file work. `detail` says what happened ("" = nothing was pending).
    std::string finishAtExit();

    // `state lk.<what>` for the drive scripts
    std::string readState(const std::string& what, std::string& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace foxsdr::slintlanek
