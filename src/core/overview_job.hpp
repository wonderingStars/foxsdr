// core/overview_job.hpp - SCREEN LANE S3, G3-14b (docs/multi-radio-records/screen-s3-recplay.md): the WHOLE-FILE WATERFALL. One picture of a whole I/Q recording - time down the page, frequency across -
// made on its own thread with a progress figure and a cancel, never on the drawing thread.
//
// HOW. The file is cut into `rows` equal pieces of time. For each piece up to four evenly spread blocks of `fft` samples are read (a seek and a read each), windowed (Hann) and transformed,
// and their power spectra are averaged into the piece's row; the row is shown from -rate/2 to +rate/2, the columns joined by their strongest bin when the transform is wider than the picture
// (at most 1024 columns). The picture is therefore a SAMPLED view of the file (a piece is represented by up to four blocks), which is what makes a recording of any length cost seconds, not
// minutes; a signal shorter than a quarter of a piece can fall between the blocks. Brightness runs from the file's noise floor (the median) to its peak.
//
// Any sample format the readers know (source/iq_readers.hpp): WAV in every width, RF64, SigMF, and a raw file whose format and rate the person states.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace cascade::core::overview {

struct Request {
    std::string path;
    std::string rawFormat;       // "" = the file says what it is; "cu8", "cs16", "cf32" for a raw file
    double rawRateHz = 0.0;
    double rawCentreHz = 0.0;
    int rows = 600;              // 50 .. 2000
    int fft = 1024;              // 512 .. 4096, a power of two
};

struct Image {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;     // width * height * 4
    double rateHz = 0.0, centreHz = 0.0, durationS = 0.0;
    double floorDb = 0.0, peakDb = 0.0;
};

struct Result {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    Image image;
    double elapsedS = 0.0;
};

// Runs on the calling thread; `cancel` is looked at between rows and `progress` (0..1) is stored after each. False with `err` for a file that cannot be read or is shorter than one block.
bool compute(const Request& req, const std::atomic<bool>& cancel, std::atomic<double>& progress, Image& out, std::string& err, bool& cancelled);

class Job {
public:
    Job() = default;
    ~Job();
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;
    // False while one is running. Never blocks.
    bool start(const Request& req);
    void cancel();
    bool running() const { return running_.load(); }
    double progress() const { return progress_.load(); }
    // True once, when a finished run's result is waiting.
    bool take(Result& out);

private:
    void reap();
    std::thread th_;
    std::atomic<bool> running_{false}, cancel_{false}, ready_{false};
    std::atomic<double> progress_{0.0};
    Result result_;
};

}  // namespace cascade::core::overview
