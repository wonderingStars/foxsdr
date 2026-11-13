// wire_b_sweep.hpp - the SWEEP DEVICE host (wire-b; docs/multi-radio-records/wire-b.md): a spectrum-analyser style source that produces SWEEP LINES (core::SweepLine), not I/Q, so it does not go
// through IqSource and the radios. One worker thread asks the device for a sweep over and over, and feeds every line to a stitched heatmap (core/sweep_stitch.hpp) and, when configured, an occupancy
// model (core/sweep_occupancy.hpp).
//
// FIRST DEVICE: the tinySA / tinySA Ultra (source/tinysa_protocol.hpp) over a serial link. The link is made by a FACTORY (the engine's default opens a Windows COM port by a validated name; the
// tests give a stand-in that speaks the frame: no test ever opens a COM port). THE PORT NAME IS NEVER LOGGED OR PUT IN A FACT (health words say "the sweep device", never COM3).
// OFF until SWEEP_OPEN; a refusal or a lost device is a sentence and the engine goes on. UNVERIFIED on a real tinySA (the record lists what its wiki does not say).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/sweep_line.hpp"
#include "core/sweep_occupancy.hpp"
#include "core/sweep_stitch.hpp"
#include "source/hackrf_sweep.hpp"
#include "source/rfexplorer_protocol.hpp"
#include "source/rfexplorer_serial_win.hpp"
#include "source/tinysa_protocol.hpp"

namespace cascade::engine::wireb {

using SerialLinkFactory = std::function<std::unique_ptr<cascade::source::tinysa::SerialLink>(const std::string& port, std::string& why)>;

// The factory the engine uses unless a test gives another: a Windows COM port opened by its validated name (false and a sentence on any other build or an invalid name).
SerialLinkFactory defaultSerialLinkFactory();

// A HackRF in sweep mode (source/hackrf_sweep.hpp, the oss-radios lane): made by a factory (the default opens the first HackRF on USB; the tests give a stand-in USB transport). One range, LINEAR style.
using HackRfFactory = std::function<std::unique_ptr<cascade::source::hackrf_sweep::HackRfSweeper>(const std::string& args, std::string& why)>;
HackRfFactory defaultHackRfFactory();

// An RF Explorer spectrum analyser (source/rfexplorer_protocol.hpp, the radio-bridges lane) over a serial link made by a factory (the default opens the COM port by a validated name and baud; the
// tests give a stand-in link). The worker does the connect and configure, so starting never blocks on the device; a refusal arrives as the status sentence and the worker ends.
using RfeLinkFactory = std::function<std::unique_ptr<cascade::source::tinysa::SerialLink>(const std::string& port, int baud, std::string& why)>;
RfeLinkFactory defaultRfeLinkFactory();

struct SweepStatus {
    bool open = false;
    std::string kind;
    std::uint64_t sweeps = 0;        // lines delivered
    std::uint64_t refused = 0;       // sweeps the device refused or sent damaged (skipped)
    std::string lastError;           // "" when none (no port name in it)
    std::size_t points = 0;
    double startHz = 0.0, stopHz = 0.0;
    std::size_t heatRows = 0, heatCols = 0;
    std::size_t occChannels = 0;
    std::size_t occSweeps = 0;
};

class SweepHost {
public:
    SweepHost() = default;
    ~SweepHost() { stop(); }
    SweepHost(const SweepHost&) = delete;
    SweepHost& operator=(const SweepHost&) = delete;

    // Opens the link and starts the worker. False with `why` when the link cannot be made. Never blocks on the device.
    bool start(const SerialLinkFactory& factory, const std::string& kind, const std::string& port, cascade::source::tinysa::Model model, double startHz, double stopHz, std::size_t points,
               int intervalMs, std::string& why);
    // The HackRF kind: the sweeper is opened by the factory with `args` ("serial=..." or "index=n", "" = the first), configured with `cfg` and started; its passes feed the same heatmap and occupancy.
    bool startHackRf(const HackRfFactory& factory, const std::string& args, const cascade::source::hackrf_sweep::SweepConfig& cfg, std::string& why);
    // The RF Explorer kind: one span in Hz (whole kHz), the top and bottom of the device's display in dBm (the device scales its amplitude byte by them). The link is opened here; the worker connects,
    // configures and then takes one complete sweep after another. False with `why` when the link cannot be made or the baud is not one of the page's list.
    bool startRfe(const RfeLinkFactory& factory, const std::string& port, int baud, double startHz, double stopHz, int topDbm, int bottomDbm, std::string& why);
    void stop();
    bool running() const { return running_.load(); }
    // A run that is not a device (second pass, the stitched sweep of an ordinary radio, wire_b_stitch.hpp): no worker thread; the host is "open" (so SWEEP_OPEN is BUSY and SWEEP_CLOSE ends it) while the
    // engine's control pass feeds finished lines through ingestLine(). The heat map takes its range from the first line. endExternal() ends the run; its sentence ("" = ended normally) is the last error.
    bool beginExternal(const std::string& kind, double startHz, double stopHz, std::string& why);
    void ingestLine(const cascade::core::SweepLine& line);
    void endExternal(const std::string& sentence);

    SweepStatus status() const;
    // Heatmap and occupancy configuration (while running or not); the heatmap restarts.
    bool configureHeatmap(const cascade::core::HeatmapConfig& cfg, std::string& why);
    bool configureOccupancy(const cascade::core::OccupancyConfig& cfg, std::string& why);
    void clearOccupancy();
    // The newest line, thinned to at most `maxPoints` (max of each group), and the occupancy table as CSV.
    bool lastLine(std::size_t maxPoints, cascade::core::SweepLine& out) const;
    std::string occupancyCsv();
    // The newest heatmap row's frequency at a horizontal position 0..1 (the click-to-tune answer).
    bool heatmapFrequencyAt(double x01, double& hz) const;

private:
    void loop();
    void loopHackRf();
    void loopRfe();
    int rfeTopDbm_ = -10, rfeBottomDbm_ = -120;
    std::unique_ptr<cascade::source::hackrf_sweep::HackRfSweeper> hackrf_;
    mutable std::mutex m_;
    std::thread th_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopFlag_{false};
    std::unique_ptr<cascade::source::tinysa::SerialLink> link_;
    cascade::source::tinysa::SweepRequest req_;
    int intervalMs_ = 0;
    std::string kind_;
    std::uint64_t sweeps_ = 0, refused_ = 0;
    std::string lastError_;
    cascade::core::SweepLine last_;
    bool haveLast_ = false;
    cascade::core::SweepHeatmap heat_;
    bool heatReady_ = false;
    bool external_ = false;
    // the heat map's own settings (columns, rows, modes), kept across opens: SWEEP_CONFIG works with no device open (second pass)
    cascade::core::HeatmapConfig heatPrefs_;
    cascade::core::OccupancyModel occ_;
    bool occReady_ = false;
};

}  // namespace cascade::engine::wireb
