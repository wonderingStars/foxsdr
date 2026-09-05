// IQ file playback source: loops a 2-channel RIFF/WAVE recording (ch0 = I,
// ch1 = Q) forever, streaming from disk through a small fixed buffer so a
// multi-gigabyte capture costs the same RAM as a tiny one.
//
// Accepted sample formats, chosen because they are what SDR recorders emit:
//   - PCM 16-bit    scaled to [-1, 1) by 1/32768 (so -32768 maps to exactly
//                   -1.0 and +32767 to 32767/32768; the divisor is a power of
//                   two, making the int16 -> float mapping bit-reproducible)
//   - IEEE float32  passed through bit-exact, no clamping or rescaling
// Everything else (mono, 8/24-bit, ADPCM, extensible, float64, ...) is
// rejected by open() with a specific lastError() message, because silently
// guessing at an unsupported layout would render garbage spectra that look
// like a DSP bug instead of a file problem.
//
// Loop-seam contract: read() delivers the file's complete frames in order and
// wraps from the last frame straight to the first — no inserted zeros, no
// repeated frame, no dropped tail. A trailing partial frame (data bytes not a
// multiple of the frame size) is ignored entirely: it can never be delivered,
// so the seam stays sample-aligned on every pass.
//
// Pacing: this is a free-running source (selfPaced() == false). read() always
// fills the full request immediately; the Pipeline paces consumption with its
// real-time clock, exactly as it already does for SigGen.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "source/iq_source.hpp"

namespace cascade::source {

class IqFileSource : public IqSource {
public:
    IqFileSource() = default;

    // Parses the WAV header and validates the format; does NOT start
    // playback. False on any failure, with the reason in lastError().
    // Reopening replaces the previous file and stops playback first.
    bool open(const std::string& wavPath);

    // start() rewinds to the first frame, so every start() is a
    // deterministic replay from the top (pause/resume is the caller's job:
    // it simply stops calling read()). Idempotent while running. Fails if
    // open() has not succeeded.
    bool start() override;
    void stop() override;  // idempotent; keeps the file open for restart
    bool running() const override;

    // Free-running: samples are synthesized (read) on demand, the caller
    // provides the clock.
    bool selfPaced() const override { return false; }

    double sampleRateHz() const override { return sampleRateHz_; }
    // Always false: the rate is a property of the recording, not a knob.
    bool setSampleRateHz(double hz) override;

    // Nominal only — a file has no tuner. Stored and reported so the
    // frequency display stays coherent.
    double centerFrequencyHz() const override { return centerFrequencyHz_; }
    bool setCenterFrequencyHz(double hz) override;

    // Fills dst with exactly n samples while running (looping seamlessly at
    // EOF); returns 0 when not running (including before open()), without
    // touching lastError() — polling a stopped source is not an error.
    // If the file fails underneath us mid-read (deleted, device error), the
    // remainder is zero-filled and lastError() set: a free-running source
    // must never return 0 for n > 0, or the pipeline's pacing loop would
    // misread it as a transient and spin.
    //
    // Abortable: a large request is served as a sequence of bounded disk
    // reads and stop() is re-checked between them, so a stop() from the
    // control thread truncates the request — read() returns however many
    // samples it had already decoded (possibly 0) instead of finishing the
    // fill. This is what keeps the Pipeline's stop()/setSource() join bounded
    // to ONE disk read when the backing file lives on a dead SMB share or a
    // failing USB device; on a healthy local file it is unobservable. The
    // truncated return is the "stopped" answer, not an error: lastError()
    // is left untouched.
    //
    // Threading: read() only from the pipeline's source thread; open()/
    // start() must not overlap an in-flight read() (the Pipeline serializes
    // this by parking the source thread across restarts).
    std::size_t read(std::complex<float>* dst, std::size_t n) override;

    // "IQ file: <basename>" after a successful open, "IQ file" before.
    const char* name() const override { return name_.c_str(); }

    const char* lastError() const override { return lastError_.c_str(); }

    // TRUE ONCE A READ HAS FAILED MID-PLAYBACK, until the next open(). read()
    // zero-fills after an I/O error and keeps returning n, deliberately (a
    // free-running source that returns 0 reads as "retry" to the pacing
    // loop) - which meant the pipeline saw a perfectly healthy source
    // delivering silence for ever: the spectrum kept scrolling, every lamp
    // stayed green, and the only record was a lastError() nothing polled
    // during playback. The pipeline's source thread polls THIS, so a file
    // that vanished or was cut short under the receiver is a fault with a
    // message on the panel rather than a quiet radio. Atomic because the
    // source thread sets it and the GUI thread reads it.
    bool faulted() const override { return faulted_.load(std::memory_order_acquire); }

    // How many RIFF chunk headers ONE open() may walk. See the loop's comment:
    // open() runs on the GUI thread and the walk's trip count was set by the
    // file's own contents, so a run of zero-size chunks bought one blocking
    // read per 8 bytes of file. Far above anything a real recording contains.
    static constexpr std::size_t kMaxHeaderChunks = 256;

protected:
    // The one raw-disk seam: reads up to `bytes` bytes from the current file
    // position into dst and returns how many actually arrived (short == the
    // file failed underneath us). Virtual for exactly one reason — a test
    // cannot make a real disk block on demand, and the stop() responsiveness
    // read() owes the pipeline is only observable while a read is IN FLIGHT.
    // Production always runs the ifstream body; the indirect call is paid once
    // per 16-32 KiB chunk, which is noise against the syscall it wraps.
    virtual std::size_t readRaw(unsigned char* dst, std::size_t bytes);

    // The same seam for open()'s RIFF header walk (it seeks, then calls this),
    // so the count of blocking reads that walk costs is observable by the same
    // means — which is what the kMaxHeaderChunks bound is measured against.
    //
    // A SECOND virtual and not a reuse of readRaw, which is a real distinction
    // and not tidiness. A readRaw fake is written against the STREAMING path,
    // where "the first read" means the first read of a playback request — the
    // suite's blocking fake parks in exactly that call to reproduce a stalled
    // disk. Feeding open()'s header reads through the same function silently
    // moves that first call from read() into open(), and a fake that meant to
    // stall playback stalls the file being opened instead. Splitting them
    // keeps each seam's contract its own; the default body below is the same
    // ifstream read, reached NON-virtually so an override of one never
    // reroutes the other.
    virtual std::size_t readHeaderRaw(unsigned char* dst, std::size_t bytes);

private:
    enum class SampleFormat { none, pcm16, float32 };

    // Central failure path so every parse error both records its reason and
    // leaves the source in a consistent not-opened state.
    bool fail(std::string msg);

    void convertFrames(const unsigned char* raw, std::complex<float>* dst,
                       std::size_t frames);

    std::ifstream file_;
    std::string name_ = "IQ file";
    std::string lastError_;
    SampleFormat format_ = SampleFormat::none;
    double sampleRateHz_ = 0.0;
    double centerFrequencyHz_ = 0.0;
    std::uint64_t dataOffset_ = 0;    // file byte offset of the first frame
    std::uint64_t frameCount_ = 0;    // complete frames in the data chunk
    std::uint64_t nextFrame_ = 0;     // next frame index read() will deliver
    std::size_t bytesPerFrame_ = 0;   // 4 (pcm16) or 8 (float32)
    bool opened_ = false;
    // Atomic because stop() arrives from the control thread while read()
    // polls it from the source thread; everything else is caller-serialized
    // per the threading note on read().
    std::atomic<bool> running_{false};
    // See faulted(): set by read() on the source thread, read by anyone.
    std::atomic<bool> faulted_{false};
    std::vector<unsigned char> ioBuf_;  // small streaming buffer, never the file
};

}  // namespace cascade::source
