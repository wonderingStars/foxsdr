// lanex_bench.hpp - LANE X, row G5-19: the DSP benchmark as a control (docs/multi-radio-records/lane-x.md). Pure: no Slint, no engine; the timing itself is core/dsp_benchmark.hpp, handed in as a function.
//
// The job runs its items one after another on ONE worker thread (the drawing thread never waits for a timing loop), reports how many items are done, can be cancelled between items, and writes no
// setting and no file. The result lines say plain numbers: how many million samples a second the block handled and how many times faster than real time that is.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace foxsdr::slintlanex {

struct BenchItem {
    std::string group;          // "FFT", "Channel", "Demod", "Resample"
    std::string name;           // "forward 65536", "NFM", ...
    bool ok = false;
    std::string error;
    double samplesPerSec = 0.0;
    double referenceRateHz = 0.0;
    double realTimeMultiple = 0.0;
};

enum class BenchState { Idle, Running, Done, Cancelled };

class BenchJob {
public:
    // run(i) times item i and never throws (a block that fails answers ok = false); count is the number of items.
    BenchJob(std::function<BenchItem(std::size_t)> run, std::size_t count);
    ~BenchJob();   // cancels and joins: a window closing in the middle of a run does not wait for the rest of the items
    BenchJob(const BenchJob&) = delete;
    BenchJob& operator=(const BenchJob&) = delete;

    bool start();      // false while a run is in progress; a finished or cancelled run is cleared and begun again
    void cancel();     // the run stops after the item it is timing; its results so far stay
    BenchState state() const { return state_.load(); }
    std::size_t done() const { return done_.load(); }
    std::size_t total() const { return count_; }
    std::vector<BenchItem> results() const;   // a copy of the items finished so far
    bool running() const { return state() == BenchState::Running; }

private:
    void work();
    std::function<BenchItem(std::size_t)> run_;
    std::size_t count_;
    std::atomic<BenchState> state_{BenchState::Idle};
    std::atomic<std::size_t> done_{0};
    std::atomic<bool> cancel_{false};
    mutable std::mutex mu_;
    std::vector<BenchItem> results_;
    std::thread th_;
};

// How a number is written: value and decimals in, text out. The window hands in the program's own number format (the Windows decimal sign when the person asked for it); the default is the plain point.
using NumberWriter = std::function<std::string(double value, int decimals)>;
NumberWriter plainNumbers();
// "24.1 million samples a second, 10.0 times real time" for a finished item, "failed: <why>" for one that could not run.
std::string itemText(const BenchItem& it, const NumberWriter& num);
// "5 of 21" for the progress line.
std::string progressText(std::size_t done, std::size_t total);
// The one-line summary of a finished run: how many items, how many failed, the slowest multiple and its name; "" for no items.
std::string summaryText(const std::vector<BenchItem>& items, const NumberWriter& num);

}  // namespace foxsdr::slintlanex
