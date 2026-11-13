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
    // wire-a begin: true when open() took the centre from the file's own header (the RIFF auxi chunk SpectraVue / HDSDR / SDRangel write), not from the nominal frequency
    bool centerFromHeader() const { return centerFromHeader_; }
    // wire-a end
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

    // SEEK, A-B LOOP AND POSITION (engine serial lane A, row P15). Any thread may call these; they only record the request (atomics, no file I/O) and read() applies it
    // on the source thread BEFORE it produces anything, so the first block delivered after seekFrame() returned starts at the new frame. A loop wraps inside one read():
    // frame b-1 is followed by frame a, no gap and no repeat; a loop end moved below the play position, or a seek past b, continues from a. With no loop the file wraps
    // from its last frame to its first as it always did. totalFrames() is zero before a successful open().
    std::uint64_t totalFrames() const { return frameCount_; }
    // The frame read() will deliver next (a seek is reflected at once, before the reader has applied it).
    std::uint64_t positionFrames() const { return position_.load(std::memory_order_acquire); }
    // False (nothing changed) when no file is open or `frame` is not below totalFrames().
    bool seekFrame(std::uint64_t frame);
    // False (nothing changed) unless a < b <= totalFrames().
    bool setLoopFrames(std::uint64_t a, std::uint64_t b);
    void clearLoop();
    bool loopOn() const { return loopOn_.load(std::memory_order_acquire); }
    std::uint64_t loopStartFrame() const { return loopA_.load(std::memory_order_acquire); }
    std::uint64_t loopEndFrame() const { return loopB_.load(std::memory_order_acquire); }
    // The recording's real start, ms since 1970 UTC, and where it came from: 1 the date and time in the file name (YYYYMMDD_HHMMSS or YYYY-MM-DD_HH-MM-SS, read as the
    // computer's local time, which is how FoxSDR names its takes), 2 the file's last-write time minus its length (a recording ends when its file was last written),
    // 0 unknown (then 0 ms). Worked out once at open().
    std::int64_t startUnixMs() const { return startUnixMs_; }
    int startSource() const { return startSource_; }

    // wire-a second pass begin: PLAYBACK TRANSPORT (speed 0.25x to 8x, pause, step). Any thread may call these; read() applies them on the source thread.
    // SPEED. Two policies, chosen by setSpeedBlockSkip. VARISPEED (default): every delivered frame is the linear interpolation of the file at position p0 + k*s, so time runs s times
    // faster and every frequency in the recording is multiplied by s (a tape); there is no anti-alias filter, so above s = 1 only the lowest 1/s of the band is clean. BLOCK SKIP: grains of
    // 2048 consecutive frames are delivered at normal speed and the position jumps by 2048*(s-1) frames between grains (forward for s > 1, back for s < 1, which plays part of the last grain
    // again), so frequencies are unchanged but time is chopped, with a click at each join. At speed exactly 1 neither policy touches a frame: read() is the unchanged path.
    // PAUSE AND STEP. While paused, read() returns 0 UNLESS a step is pending: stepBlocks(n) (paused only, n != 0, |n| <= 4096) then makes read() deliver exactly n * kStepBlockFrames frames at the
    // current speed and return 0 again; a negative n first moves the position back |n| blocks (clamped to the first frame) and delivers one block. The pipeline's free-running loop paces by its own clock and writes
    // nothing for a 0, so a paused file simply leaves the ring dry (the spectrum stays on its last frame, the audio goes quiet): this is the ONE case in which the "a free-running source never returns 0"
    // rule below does not hold. wantsRead() says whether a read would deliver.
    static constexpr double kMinPlaySpeed = 0.25;
    static constexpr double kMaxPlaySpeed = 8.0;
    static constexpr std::size_t kStepBlockFrames = 4096;
    static constexpr long long kMaxStepBlocks = 4096;
    // False (nothing changed) for NaN, infinity or a value outside 0.25 .. 8.
    bool setPlaySpeed(double s);
    double playSpeed() const { return speed_.load(std::memory_order_acquire); }
    void setSpeedBlockSkip(bool on) { blockSkip_.store(on, std::memory_order_release); }
    bool speedBlockSkip() const { return blockSkip_.load(std::memory_order_acquire); }
    void pausePlayback() { paused_.store(true, std::memory_order_release); }
    void resumePlayback() {
        paused_.store(false, std::memory_order_release);
        stepFrames_.store(0, std::memory_order_release);   // a step left over from the pause must not fire later
    }
    bool playbackPaused() const { return paused_.load(std::memory_order_acquire); }
    // False (nothing changed) while playing, for n == 0, for |n| over kMaxStepBlocks and with no file open.
    bool stepBlocks(long long n);
    long long stepFramesPending() const { return stepFrames_.load(std::memory_order_acquire); }
    // True when read() has something to deliver: playing, or paused with a step pending. A caller that would rather not poll a held file asks this first.
    bool wantsRead() const { return !paused_.load(std::memory_order_acquire) || stepFrames_.load(std::memory_order_acquire) > 0; }
    // wire-a second pass end

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
    bool centerFromHeader_ = false;   // wire-a
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
    // P15: the published position, the pending seek (-1 none) and the loop. Written by any thread, applied by read().
    std::atomic<std::uint64_t> position_{0};
    std::atomic<std::int64_t> seekRequest_{-1};
    std::atomic<bool> loopOn_{false};
    std::atomic<std::uint64_t> loopA_{0};
    std::atomic<std::uint64_t> loopB_{0};
    std::int64_t startUnixMs_ = 0;
    int startSource_ = 0;
    void seekTo(std::uint64_t frame);   // the source thread's (or start()'s): moves the stream and nextFrame_
    // wire-a second pass begin: transport state. The atomics are written by any thread; the rest belongs to the source thread (start() resets it while the thread is parked).
    std::atomic<double> speed_{1.0};
    std::atomic<bool> blockSkip_{false};
    std::atomic<bool> paused_{false};
    std::atomic<long long> stepFrames_{0};
    std::vector<std::complex<float>> win_;   // varispeed: source frames from the integer position floor(frac_)
    double frac_ = 0.0;
    std::size_t grainLeft_ = 2048;           // block skip: frames of the current grain still to deliver
    double skipFrac_ = 0.0;
    std::size_t readNormal(std::complex<float>* dst, std::size_t n);   // the unchanged read (seek, loop, seam)
    std::size_t readVaried(std::complex<float>* dst, std::size_t n, double s);
    std::size_t readSkipping(std::complex<float>* dst, std::size_t n, double s);
    void jumpBy(long long frames);
    // wire-a second pass end
};

}  // namespace cascade::source
