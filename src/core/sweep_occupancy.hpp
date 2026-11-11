// sweep_occupancy.hpp - the OCCUPANCY AND DUTY-CYCLE MODEL: for each channel of a
// plan, the percent of the observed time its level was above a noise-floor-relative
// threshold; the same by hour of the day; CSV export. Host-side model only (the
// Survey Engine plugin has its own presence fraction; this is the model any
// plugin or the main spectrum can reuse through a hand-off).
//
// INPUT: successive SweepLine passes (levels in dB, timeS = Unix seconds, UTC).
//
// A CHANNEL PLAN is start / stop / step: channel i covers [start + i*step,
// start + (i+1)*step), the last one clipped to stop; at most kOccMaxChannels.
//
// ONE SWEEP -> ONE LEVEL PER CHANNEL: the highest dB among the line's points that
// fall in the channel; a channel with no usable point in that sweep is NOT
// OBSERVED then (it adds nothing to either side of its percentage).
//
// THE THRESHOLD is noise-floor relative, per sweep, so a gain step or a drifting
// floor moves it with the data:
//   floor      = the `floorPercentile` quantile (default 0.2) of that sweep's
//                observed channel levels (index floor(p*(n-1)) of the sorted
//                levels) - the quiet channels; needs minChannelsForPercentile
//                (default 8) observed channels, else the sweep is refused; OR
//                an absolute `fixedFloorDb` when fixedFloor is set;
//   threshold  = floor + marginDb (default 10 dB);
//   occupied   = level strictly above the threshold.
// LIMIT: a percentile floor assumes fewer than (1 - p) of the channels are busy
// at once. A band that is busy everywhere reads as a high floor; use the fixed
// floor there.
//
// PERCENT TIME is time-weighted, not a count of sweeps: a sweep's state holds from
// its time to the next sweep's time, capped at maxGapS (default 60 s) so a pause in
// the capture is not counted as observed; the last sweep holds for the previous
// interval (or nominalDwellS if it is the only one) once finish() is called. The
// time belongs to the hour of the day (local = UTC + utcOffsetMinutes) in which
// the sweep STARTED. Times must strictly increase.
//
// EXPORT (CSV, LF, '.' decimals, first line names the file and version):
//   channelsCsv():     #foxsdr-occupancy,channels,1
//                      channel,start_hz,stop_hz,observed_s,occupied_s,percent
//   hoursCsv():        #foxsdr-occupancy,hours,1
//                      hour,observed_s,occupied_s,percent          (24 rows, 0..23, all channels pooled)
//   channelHoursCsv(): #foxsdr-occupancy,channel-hours,1
//                      channel,hour,observed_s,occupied_s,percent  (rows with observed time only)
// percent is empty where nothing was observed.
//
// Nothing personal is held; memory only.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/sweep_line.hpp"

namespace cascade::core {

inline constexpr std::size_t kOccMaxChannels = 16384;

struct ChannelPlan {
    double startHz = 0.0;
    double stopHz = 0.0;
    double stepHz = 0.0;
    std::size_t count = 0;   // derived by makeChannelPlan
    double channelStartHz(std::size_t i) const { return startHz + stepHz * static_cast<double>(i); }
    double channelStopHz(std::size_t i) const;
};

// false and *why for a plan that is empty, reversed, non-finite, a step below
// 1 Hz, or over kOccMaxChannels.
bool makeChannelPlan(double startHz, double stopHz, double stepHz, ChannelPlan& out, std::string* why = nullptr);

struct OccupancyConfig {
    ChannelPlan plan;
    double marginDb = 10.0;
    double floorPercentile = 0.2;        // 0 .. 0.5
    bool fixedFloor = false;
    double fixedFloorDb = -100.0;
    std::size_t minChannelsForPercentile = 8;
    double maxGapS = 60.0;
    double nominalDwellS = 1.0;
    int utcOffsetMinutes = 0;            // -840 .. 840
};

class OccupancyModel {
public:
    bool configure(const OccupancyConfig& cfg, std::string* why = nullptr);
    bool addSweep(const SweepLine& line, std::string* why = nullptr);
    // Credit the last sweep; safe to call more than once. Call before reading results.
    void finish();

    std::size_t sweepsAccepted() const { return sweeps_; }
    std::size_t channels() const { return cfg_.plan.count; }
    double observedS(std::size_t ch) const;
    double occupiedS(std::size_t ch) const;
    // NaN when nothing was observed (or ch is out of range).
    double percent(std::size_t ch) const;
    double hourPercent(int hour) const;                   // all channels pooled
    double hourPercent(std::size_t ch, int hour) const;
    // The threshold the last accepted sweep used, dB.
    double lastThresholdDb() const { return lastThreshold_; }

    std::string channelsCsv() const;
    std::string hoursCsv() const;
    std::string channelHoursCsv() const;

private:
    void credit(double dt);
    int hourOf(double timeS) const;
    OccupancyConfig cfg_;
    bool configured_ = false;
    std::size_t sweeps_ = 0;
    std::vector<double> obs_, occ_;           // per channel
    std::vector<double> hobs_, hocc_;         // per channel * 24
    std::vector<std::uint8_t> pending_;       // 0 not observed, 1 idle, 2 occupied
    bool havePending_ = false;
    double pendingTime_ = 0.0;
    double lastDt_ = 0.0;
    double lastThreshold_ = 0.0;
    std::vector<double> levelScratch_;
};

}  // namespace cascade::core
