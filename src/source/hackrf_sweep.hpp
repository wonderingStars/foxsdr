// hackrf_sweep.hpp - HackRF sweep mode: the INIT_SWEEP request, the sweep block stream, and a spectrum assembler
// that turns it into SweepLine records (src/core/sweep_line.hpp). Also HackRfSweeper, which drives a HackRF
// One in sweep mode over the WinUSB transport (no libhackrf, no libusb).
//
// SOURCE AND LICENCE (opened 2026-11-12; the licence question is answered FILE BY FILE, because the repository
// as a whole is GPL-2.0, its firmware and tools are GPL, and none of those was opened):
//   * https://github.com/greatscottgadgets/hackrf , host/libhackrf/src/hackrf.c and host/libhackrf/src/hackrf.h
//     (commit 3a63b29953a3b763f428821e02110def9fb1cade). The HEADER OF EACH of the two files, opened and read,
//     is the BSD 3-clause licence ("Copyright (c) 2012-2026 Great Scott Gadgets, (c) 2012 Jared Boone, (c) 2013
//     Benjamin Vernoux ... Redistribution and use in source and binary forms, with or without modification, are
//     permitted ... Neither the name of Great Scott Gadgets nor the names of its contributors may be used to
//     endorse or promote products derived from this software without specific prior written permission"), and
//     neither names another origin. Taken as facts: HACKRF_VENDOR_REQUEST_INIT_SWEEP = 26; the request's layout
//     (hackrf_init_sweep); TRANSCEIVER_MODE_RX_SWEEP = 5 and its USB API requirements (init 0x0102, start 0x0104);
//     the block format ("0x7F - 0x7F - uint64_t frequency (LSBFIRST, in Hz)" ahead of the samples); BYTES_PER_BLOCK
//     16384, SAMPLES_PER_BLOCK 8192, MAX_SWEEP_RANGES 10; the sweep styles LINEAR / INTERLEAVED.
//   * hackrf_sweep.c (the command-line tool) and the firmware are GPL: NOT opened. hackrf.h names the tool as the
//     example for parsing the header; it was not followed. What the tool does with a block (which FFT bins it
//     keeps, how INTERLEAVED is combined) is therefore NOT known here: this file builds the LINEAR style only
//     and makes its own, documented choices (see SweepAssembler).
//
// WHAT IS INFERRED, NOT STATED (all UNVERIFIED ON A HACKRF): that the 10-byte header lies INSIDE the num_bytes
// block, replacing its first five samples (the blocks must tile the fixed-size USB transfer, which hackrf.c
// takes as 262144 bytes = 16 blocks of 16384); that the header frequency is the centre of the block's samples
// (hackrf.h calls it "the tuned frequency"); the samples are signed 8-bit I, Q as in normal receive; that the
// firmware tunes start + offset, then adds step_width at every step.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/sweep_line.hpp"
#include "usb/usb_device.hpp"

namespace cascade::source::hackrf_sweep {

constexpr std::uint8_t kRequestInitSweep = 26;
constexpr std::uint16_t kModeRxSweep = 5;
constexpr std::size_t kBytesPerBlock = 16384;
constexpr std::size_t kSamplesPerBlock = 8192;
constexpr std::size_t kHeaderBytes = 10;
constexpr int kMaxRanges = 10;
constexpr std::uint16_t kUsbApiInitSweep = 0x0102, kUsbApiStartRxSweep = 0x0104;  // informational: not readable through UsbDevice

enum class Style : std::uint8_t { Linear = 0, Interleaved = 1 };

struct Plan {
    std::vector<std::pair<std::uint16_t, std::uint16_t>> rangesMHz;  // start, stop, whole MHz
    std::uint32_t numBytes = static_cast<std::uint32_t>(kBytesPerBlock);
    std::uint32_t stepWidthHz = 10000000;
    std::uint32_t offsetHz = 5000000;  // the first block's centre is start + offset: half a step puts [start, start+step] in it
    Style style = Style::Linear;
};

// Empty when the plan can be sent; otherwise the sentence. Checks (all from hackrf_init_sweep): 1 to 10 ranges,
// each stop above its start, numBytes a multiple of 16384 and at least 16384 (at most 4 MiB here), step width at
// least 1 Hz (and at least 1 kHz, a choice of this file), style 0 or 1 (this driver builds LINEAR only and says so).
std::string validate(const Plan& p);

struct InitRequest {
    std::uint16_t value = 0;  // num_bytes & 0xFFFF
    std::uint16_t index = 0;  // num_bytes >> 16
    std::vector<std::uint8_t> data;  // step LE32, offset LE32, style, then start/stop LE16 pairs
};
bool buildInitRequest(const Plan& p, InitRequest& out, std::string& error);

// ---- the block stream ---------------------------------------------------------------------------------------------------
struct Block {
    std::uint64_t frequencyHz = 0;          // the header: the centre of this block's samples
    std::vector<std::complex<float>> iq;   // (blockBytes - 10) / 2 samples, full scale 1.0
};

// Cuts the bulk byte stream into blocks of `blockBytes`. A block whose first two bytes are not 7F 7F, or whose
// frequency is outside [minHz, maxHz] when those are set, is counted bad; the splitter then scans byte by byte for
// the next position that has both a header and a plausible frequency (the sample data can contain 7F 7F by
// itself, so the frequency check is what makes a resync believable) and counts a resync.
class BlockSplitter {
public:
    explicit BlockSplitter(std::size_t blockBytes = kBytesPerBlock, std::uint64_t minHz = 0, std::uint64_t maxHz = 0)
        : blockBytes_(blockBytes), minHz_(minHz), maxHz_(maxHz) {}
    void feed(const std::uint8_t* d, std::size_t n, std::vector<Block>& out);
    void reset() { buf_.clear(); }
    std::uint64_t goodBlocks() const { return good_; }
    std::uint64_t badBlocks() const { return bad_; }
    std::uint64_t resyncs() const { return resyncs_; }

private:
    bool plausible(const std::uint8_t* p) const;
    std::size_t blockBytes_;
    std::uint64_t minHz_, maxHz_;
    std::vector<std::uint8_t> buf_;
    std::uint64_t good_ = 0, bad_ = 0, resyncs_ = 0;
};

// ---- the spectrum ------------------------------------------------------------------------------------------------------------
struct SpectrumConfig {
    double sampleRateHz = 20.0e6;
    std::size_t fftSize = 1024;   // a power of two, 32..65536; blocks are averaged (Welch, Hann, 50 % none)
    double stepHz = 10.0e6;       // the part of each block kept: centre +- step/2 (LINEAR tiles the range with it)
    bool removeDcSpike = true;    // the centre bins are replaced by their neighbours' mean: HackRF's DC spike
};

// One block's power in dB (relative to full scale, per bin) for the bins within centre +- stepHz/2, ascending
// frequency. `binHz` = sampleRate / fftSize. False with `error` when the config or block cannot be used.
bool blockSpectrum(const Block& b, const SpectrumConfig& c, core::SweepLine& line, std::string& error);

// Puts blocks into one line per range and completes a pass when the centre frequency stops rising (the sweep
// wrapped). Bins not covered stay NaN. A range line has one point per bin, on the grid of the range's start.
class SweepAssembler {
public:
    SweepAssembler(const Plan& plan, const SpectrumConfig& cfg);
    // Appends the lines of any pass this block completed (one per range, in plan order).
    void add(const Block& b, double timeS, std::vector<core::SweepLine>& out);
    std::uint64_t passes() const { return passes_; }
    std::uint64_t unusedBlocks() const { return unused_; }

private:
    void flush(double timeS, std::vector<core::SweepLine>& out);
    Plan plan_;
    SpectrumConfig cfg_;
    std::vector<core::SweepLine> acc_;  // per range
    std::uint64_t lastHz_ = 0;
    bool any_ = false;
    std::uint64_t passes_ = 0, unused_ = 0;
};

// ---- the driver ------------------------------------------------------------------------------------------------------------------
struct SweepConfig {
    Plan plan;
    SpectrumConfig spectrum;
    double lnaDb = 16.0;   // 0..40 in 8 dB steps (rounded down as the receiver driver does)
    double vgaDb = 16.0;   // 0..62 in 2 dB steps
    bool ampOn = false;
};

class HackRfSweeper {
public:
    using UsbOpenFn = std::function<std::unique_ptr<cascade::usb::UsbDevice>(const std::string& path, std::string& error)>;
    HackRfSweeper() = default;
    ~HackRfSweeper();
    HackRfSweeper(const HackRfSweeper&) = delete;
    HackRfSweeper& operator=(const HackRfSweeper&) = delete;

    // TESTS ONLY: the devices open() will enumerate and what opens one.
    void setTransportForTest(std::vector<cascade::usb::UsbDeviceInfo> devices, UsbOpenFn opener);

    // args: serial=<s> or index=<n> (first HackRF by default).
    bool open(const std::string& args);
    void close();
    bool isOpen() const;
    // Programs the sample rate (with its filter), gains, amplifier and the sweep (INIT_SWEEP). Not while streaming.
    bool configure(const SweepConfig& c);
    // Sweep mode on, bulk ring begun. Needs configure() first.
    bool start();
    // Reads for at most timeoutMs and appends completed passes to `out`. False (with lastError) when the pipe failed.
    bool poll(std::vector<core::SweepLine>& out, unsigned timeoutMs, double timeS);
    void stop();
    bool streaming() const;
    const std::string& lastError() const { return error_; }
    std::uint64_t goodBlocks() const;
    std::uint64_t badBlocks() const;
    std::uint64_t resyncs() const;
    std::uint64_t passes() const;

private:
    bool controlOut(std::uint8_t request, std::uint16_t value, std::uint16_t index, const std::uint8_t* d, std::size_t n, const char* what);
    bool controlInByte(std::uint8_t request, std::uint16_t value, std::uint16_t index, std::uint8_t& reply, const char* what);
    mutable std::mutex mu_;
    std::vector<cascade::usb::UsbDeviceInfo> fakeDevices_;
    UsbOpenFn fakeOpener_;
    bool useFake_ = false;
    std::unique_ptr<cascade::usb::UsbDevice> dev_;
    bool configured_ = false, streaming_ = false, ringBegun_ = false;
    SweepConfig cfg_;
    std::unique_ptr<BlockSplitter> splitter_;
    std::unique_ptr<SweepAssembler> assembler_;
    std::vector<std::uint8_t> buf_;
    std::string error_;
};

}  // namespace cascade::source::hackrf_sweep
