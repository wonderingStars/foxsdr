// WAV recorder: captures the live stream to disk from the DSP thread.
//
//   RecordKind::BasebandIq -> 2-channel IEEE float32 WAV (ch0 = I, ch1 = Q).
//       Samples are written bit-exact — no clamping, no rescaling — so a
//       recording read back through source/iq_file_source reproduces the
//       original stream float-for-float. That reader/writer symmetry is the
//       point: record here, replay through the file source, run the same DSP.
//   RecordKind::Audio -> 1-channel PCM int16 WAV. Samples are clamped to
//       [-1, 1] first, then quantized as round(v * 32768) saturated to the
//       int16 range — the exact inverse of IqFileSource's i/32768 read
//       convention, so a round trip lands within 1 LSB (the worst case is
//       +1.0, which the int16 grid cannot represent; it saturates to 32767).
//
// Hot path: writeIq()/writeAudio() run on the CALLER'S thread (the DSP
// thread). They encode into a small staging block (4096 frames, 32 KiB for
// IQ) and hand it to buffered fwrite — no locks, no worker thread. Why that
// suffices: the worst-case ingest is baseband IQ at the app's default 2 Msps
// = 16 MB/s. stdio is given a 256 KiB buffer via setvbuf, so the stream
// spills to the OS about 64 times a second as sequential 256 KiB writes; a
// sequential write of that size lands in the page cache in tens of
// microseconds, orders of magnitude inside the pipeline's per-block budget.
// A dedicated writer thread would add a ring and a failure mode without
// buying anything at this rate.
//
// Crash contract: start() writes AND flushes a complete 44-byte RIFF header
// declaring a ZERO-length data chunk before the first sample byte. stop()
// seeks back and patches the two size fields (RIFF size at byte 4, data size
// at byte 40). If the process dies mid-record only that patch is lost: the
// flushed header is already on disk, so the orphan parses as a valid
// zero-sample WAV in any chunk-walking reader — a recoverable husk, never an
// unreadable file (the OS still persists whatever sample bytes stdio had
// flushed by then; a tool that ignores the stale data size can salvage them).
//
// Filenames: <prefix>_<YYYYMMDD>_<HHMMSS>_<rate>Hz.wav with prefix "iq" or
// "audio" and the LOCAL time at start(), e.g.
// "iq_20260815_143059_2000000Hz.wav". makeFilename() is a pure function of
// its arguments so the format is pinned by test. Restarting into the same
// directory within the same local second reuses the name and truncates the
// earlier take — accepted, because a record toggle cannot meaningfully cycle
// inside one second.
//
// Limits: the data chunk is capped so the 32-bit RIFF size field stays
// valid (just under 4 GiB). Once full — or after the first short fwrite
// (disk full/removed) — further samples are dropped silently; the counters
// only ever report bytes stdio actually accepted, so the header patched by
// stop() never claims samples the disk refused.
//
// Threading: writeIq()/writeAudio() from one thread only, and start()/stop()
// must not overlap an in-flight write (the caller parks the DSP thread
// across record toggles — the same serialization contract IqFileSource
// documents for its read()). recording(), kind() and both counters are
// atomic and may be polled from any thread (e.g. the GUI status bar).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace cascade::core {

enum class RecordKind { BasebandIq, Audio };

class Recorder {
public:
    Recorder();
    ~Recorder();  // stop(): a Recorder leaving scope finalizes its file

    // The FILE* and its buffer are single-owner by nature.
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Opens <directory>/<makeFilename(...)> for the current local time,
    // creating missing directories. Writes + flushes the zero-length header
    // (see crash contract above). False with a reason in `error` if already
    // recording, the rate is unrepresentable in a WAV header, the directory
    // cannot be created, or the file cannot be opened/written.
    bool start(RecordKind kind, const std::string& directory,
               double sampleRateHz, std::string& error);

    // No-op unless recording the matching kind (wrong-kind and pre-start
    // calls are silently ignored so the DSP loop needs no conditionals).
    void writeIq(const std::complex<float>* s, std::size_t n);
    void writeAudio(const float* s, std::size_t n);

    // Patches the RIFF/data sizes and closes the file. Idempotent; safe
    // without a prior start(). Counters keep their final values until the
    // next start() so the GUI can show "recorded N samples" after the fact.
    void stop();

    bool recording() const;
    RecordKind kind() const;

    // Sample frames / data-chunk payload bytes accepted so far. bytesWritten
    // excludes the 44-byte header: it is the amount of signal captured, which
    // is the number a recording UI wants.
    std::uint64_t samplesWritten() const;
    std::uint64_t bytesWritten() const;

    // True once the disk refused a write during this recording (full, removed,
    // or gone read-only) - the recording is still "on" in the sense that
    // stop() has not been called, but nothing more is reaching the file. Read
    // by the status column so a frozen MB counter is explained rather than
    // left for the user to notice. Cleared by the next start().
    bool writeFailed() const { return writeFailed_.load(std::memory_order_acquire); }

    // Pure and testable: "iq_20260815_143059_2000000Hz.wav",
    // "audio_20260815_143059_48000Hz.wav". The rate is rounded to the
    // nearest integer Hz (WAV itself stores an integer rate anyway).
    static std::string makeFilename(RecordKind kind, double sampleRateHz,
                                    std::tm localTime);

private:
    // fwrite the first `bytes` of stage_ with truthful accounting: counters
    // advance only by what stdio accepted, and the first short write latches
    // writeFailed_ so a dead disk is not hammered once per block forever.
    void flushStage(std::size_t bytes, std::size_t bytesPerFrame);

    std::FILE* file_ = nullptr;
    std::atomic<RecordKind> kind_{RecordKind::BasebandIq};
    std::atomic<bool> recording_{false};
    std::atomic<std::uint64_t> frames_{0};
    std::atomic<std::uint64_t> dataBytes_{0};
    // Atomic because the DSP thread latches it and the GUI thread reads it
    // through writeFailed() to say so on the RECORDER card.
    std::atomic<bool> writeFailed_{false};
    std::vector<char> fileBuf_;          // setvbuf storage; must outlive file_
    std::vector<unsigned char> stage_;   // little-endian staging block
};

}  // namespace cascade::core
