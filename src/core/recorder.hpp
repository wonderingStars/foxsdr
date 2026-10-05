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
// valid (just under 4 GiB) — about four and a half minutes of the app's
// default 2 Msps IQ, so it is a length real takes reach rather than an
// exotic one. Reaching it ENDS the take: the header is patched with the
// bytes actually written, the file is closed, recording() goes false and
// sizeLimitReached() latches. It used to drop that block and every block
// after it and tell nobody, so the panel kept its REC lamp and its climbing
// clock over a file that had stopped growing minutes earlier and still
// declared zero samples on disk. A short fwrite (disk full/removed) is the
// other way a take stops taking: that one leaves the file open for stop()
// to finalize, and only latches writeFailed(). Either way the counters
// report bytes stdio actually accepted, so the patched header never claims
// samples the disk refused.
//
// Threading: writeIq()/writeAudio() from one thread only, and start()/stop()
// must not overlap an in-flight write (the caller parks the DSP thread
// across record toggles — the same serialization contract IqFileSource
// documents for its read()). That contract is what lets the size-limit end
// finalize the file from inside a write, on the writing thread: no other
// thread can be in start()/stop() at that moment, and the parking the caller
// already does to satisfy the contract publishes the closed file_ to it.
// recording(), kind(), both counters and both fault flags are atomic and may
// be polled from any thread (e.g. the GUI status bar).
//
// Slow disks: start() WAITS FOR THE DISK, and a caller that must not wait
// (the GUI thread) takes it apart. A freeze report from 0.99.58 (Windows 11,
// 151 s into the session) had the GUI thread inside Recorder::start, inside
// the C runtime's file open, for more than the hang watchdog's five seconds:
// the Record button's handler created a file in a folder that was slow to
// answer (a synchronised or network folder, a sleeping drive, a scanner
// holding the path), and the window did not draw until it did. What waits is
// exactly create_directories, fopen and the header's write and flush; the
// rest of start() is arithmetic. So the three are separate steps:
//
//   prepare()   no I/O, no state: validates the rate, builds the file name and
//               the header bytes, refuses a recorder already recording. Cheap.
//   an opener   THE BLOCKING PART, and the only one. Runs on any thread, reads
//               its OpenRequest and touches nothing else (see "Opener").
//   begin()     no I/O: attaches the opened file and arms the recorder. Cheap.
//
// start() is the three in a row on the caller's thread, which is what every
// caller that can afford to wait still does (the patch page's speaker files,
// the --record-check bench). gui::RecordStart runs the middle step on a worker
// and the other two on the GUI thread, so the window keeps drawing until the
// file is there.
//
// THE ORDER CONTRACT THAT MATTERS SURVIVES THE SPLIT: the DSP thread reaches a
// recorder only through Pipeline::set*Recorder, and Pipeline's own header says
// start() comes FIRST and the tap SECOND. With the open on a worker that reads
// "begin() returned true, THEN set*Recorder()" - the worker never sees the
// Recorder, only the OpenRequest it was handed, so nothing can be written to a
// take that has not been armed, and begin() never overlaps a write because no
// write can be in flight against a recorder the pipeline has not been given.
//

// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <atomic>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <utility>
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
    // cannot be created, or the file cannot be opened/written. BLOCKS for as
    // long as the disk takes (see "Slow disks"): not for the GUI thread, which
    // goes through gui::RecordStart.
    bool start(RecordKind kind, const std::string& directory,
               double sampleRateHz, std::string& error);

    // The same, with the file named "<namePrefix>_<YYYYMMDD>_<HHMMSS>.wav"
    // instead of makeFilename's. Several patch speakers record at once, each
    // to its own file, so each carries its node in the name (0.99.17). An
    // empty prefix is the plain start() above.
    bool start(RecordKind kind, const std::string& directory, double sampleRateHz,
               std::string& error, const std::string& namePrefix);

    // The file the current or most recent take went to; "" before any start.
    std::string path() const;

    // --- start() taken apart (see "Slow disks" above) ------------------------

    // Everything the blocking step needs, by value: the directory to create,
    // the file to create in it and the 44 header bytes to write into it. Built
    // by prepare(); carries no pointer to any Recorder, which is what lets a
    // worker that is still inside the filesystem at quit be abandoned.
    struct OpenRequest {
        RecordKind kind = RecordKind::BasebandIq;
        std::string directory;  // created if missing; "." for "here"
        std::string path;       // directory + file name, the file to create
        std::array<unsigned char, 44> header{};  // zero-length data chunk
    };

    // A closed-over FILE*: fclose by default, replaceable only so a test can
    // count closes. A function pointer rather than a type so that an OpenedFile
    // is the same type whoever made it.
    using FilePtr = std::unique_ptr<std::FILE, void (*)(std::FILE*)>;
    static void closeFile(std::FILE* f) {
        if (f != nullptr) { std::fclose(f); }
    }

    // A take's file, open, its header written and flushed, and not yet armed.
    // MOVE-ONLY AND SELF-CLEANING: one that is dropped (a take stopped while it
    // was still opening, or a worker abandoned at quit) closes its file, so
    // nothing leaks a handle however the take ends. What it leaves on disk is
    // the same zero-sample WAV that Record followed at once by Stop leaves.
    struct OpenedFile {
        // Declared BEFORE `file` so that it is destroyed AFTER it: the stream
        // is closed (flushed) while its setvbuf buffer is still alive.
        std::vector<char> buffer;
        FilePtr file{nullptr, &Recorder::closeFile};
        std::string path;
        RecordKind kind = RecordKind::BasebandIq;

        OpenedFile() = default;
        OpenedFile(OpenedFile&&) = default;
        // Closes what it holds BEFORE taking the other's buffer. The defaulted
        // assignment moves members in declaration order, which would free this
        // stream's setvbuf buffer first and fclose the stream into it after.
        OpenedFile& operator=(OpenedFile&& other) noexcept {
            if (this != &other) {
                file.reset();
                buffer = std::move(other.buffer);
                file = std::move(other.file);
                path = std::move(other.path);
                kind = other.kind;
            }
            return *this;
        }
    };

    // A take's file, taken OFF the recorder to be finished somewhere else
    // (0.99.65). stop() patches the header and closes the file, and BOTH wait for
    // the disk: the same slow answer that froze the Record button on its way in
    // freezes the Stop button on its way out. So stop() is two steps, as start()
    // is three:
    //
    //   stopForFinish()   no I/O: marks the recorder stopped and moves the file,
    //                     its stdio buffer and the byte count into a FinishRequest.
    //   finishFile()      THE BLOCKING PART: flush, patch the two size fields,
    //                     close. Static, reads nothing but its argument, so a
    //                     worker that is abandoned at quit touches no recorder.
    //
    // stop() is still the two in a row, for every caller that can wait (the bench,
    // the tests, a size-limit end, which happens on the writing thread). The
    // GUI hands the request to core::RecordFinisher (core/record_finish.hpp).
    //
    // MOVE-ONLY AND SELF-CLEANING, as OpenedFile is: one that is dropped closes
    // its file, which leaves what a crash leaves - the zero-length header the
    // opener flushed (a husk any chunk-walking reader parses).
    struct FinishRequest {
        // Declared BEFORE `file` so that it is destroyed AFTER it: the stream is
        // closed (flushed) while its setvbuf buffer is still alive.
        std::vector<char> buffer;
        FilePtr file{nullptr, &Recorder::closeFile};
        std::uint64_t dataBytes = 0;  // what the header's two size fields must say

        FinishRequest() = default;
        FinishRequest(FinishRequest&&) = default;
        FinishRequest& operator=(FinishRequest&& other) noexcept {
            if (this != &other) {
                file.reset();
                buffer = std::move(other.buffer);
                file = std::move(other.file);
                dataBytes = other.dataBytes;
            }
            return *this;
        }
    };

    // The blocking step. Called with the request, fills `out` and returns true,
    // or returns false with the reason in `error`. Runs on whatever thread it
    // is given and MUST touch nothing but its arguments.
    using Opener = std::function<bool(const OpenRequest&, OpenedFile&, std::string&)>;

    // Production opener: create_directories, fopen("wb"), setvbuf with the
    // 256 KiB buffer the hot path is budgeted on, then the header written and
    // flushed (the crash contract). Static and stateless on purpose.
    static bool openFile(const OpenRequest& req, OpenedFile& out, std::string& error);

    // Step one. False with the reason in `error` if already recording, or the
    // rate is unrepresentable in a WAV header; otherwise `out` is the request.
    // Does NOT touch the filesystem. The file's timestamp is taken HERE, so a
    // take that waited seconds for its disk is still named for the moment the
    // user asked.
    bool prepare(RecordKind kind, const std::string& directory, double sampleRateHz,
                 const std::string& namePrefix, OpenRequest& out, std::string& error) const;

    // Step three. Arms the recorder on an opened file: counters reset, kind set,
    // recording() true. False (and the file left to the caller, who still owns
    // it) if already recording or `opened` holds no file. Call it only when no
    // write can be in flight (see "Threading"), then install the pipeline tap.
    bool begin(OpenedFile&& opened, std::string& error);

    // How the blocking step is done: openFile unless bound otherwise, and
    // gui::RecordStart copies it to its worker. Replacing it is the seam for
    // slow I/O - a test binds one that sleeps (a slow disk as far as the
    // caller can tell) or fails late. Never called by the application.
    void bindOpener(Opener opener) { opener_ = std::move(opener); }
    Opener opener() const { return opener_; }

    // No-op unless recording the matching kind (wrong-kind and pre-start
    // calls are silently ignored so the DSP loop needs no conditionals).
    void writeIq(const std::complex<float>* s, std::size_t n);
    void writeAudio(const float* s, std::size_t n);

    // Patches the RIFF/data sizes and closes the file. Idempotent; safe
    // without a prior start(), and a no-op on a take that already ended
    // itself at the size limit. Counters keep their final values until the
    // next start() so the GUI can show "recorded N samples" after the fact.
    // BLOCKS for as long as the disk takes (see FinishRequest): not for the GUI
    // thread, which goes through stopForFinish() and core::RecordFinisher.
    void stop();

    // Step one of stop(): recording() goes false and the file, its buffer and the
    // byte count move into `out`. NO DISK. True when there was a file to finish; a
    // no-op returning false when the recorder was idle or the take had already
    // ended itself at the size limit. Call it only when no write can be in flight,
    // exactly as stop() requires. The counters keep their final values.
    bool stopForFinish(FinishRequest& out);

    // Step two: patch the header and close. Returns whether the header patch
    // reached the file (a false return leaves the zero-length header the opener
    // flushed - a husk, not an unreadable file); the file is closed either way.
    // Reads nothing but `req`. Runs the finish hook first (below).
    static bool finishFile(FinishRequest&& req);

    // Test seam (0.99.65): called by finishFile immediately before it touches the
    // file, with the stream it is about to flush, patch and close - so a test can
    // make the finish as slow as a bad disk, or pull the file out from under it
    // (the handle closed, the device gone). A plain function pointer, null in every
    // shipped build, set only by tests.
    using FinishHook = void (*)(std::FILE* file);
    static void setFinishHookForTest(FinishHook hook);

    bool recording() const;
    RecordKind kind() const;

    // Sample frames / data-chunk payload bytes accepted so far. bytesWritten
    // excludes the 44-byte header: it is the amount of signal captured, which
    // is the number a recording UI wants.
    std::uint64_t samplesWritten() const;
    std::uint64_t bytesWritten() const;

    // True once nothing more is reaching the file: the disk refused a write
    // (full, removed, or gone read-only), or the data chunk hit its 4 GiB
    // ceiling. Read by the status column so a frozen MB counter is explained
    // rather than left for the user to notice. Cleared by the next start().
    //
    // The two causes differ in what follows, and a UI with a sentence for
    // each must ask sizeLimitReached() FIRST, because this flag is set for
    // both: a refused write leaves the take "on" until stop() is called,
    // while the ceiling has already ended and finalized it.
    bool writeFailed() const { return writeFailed_.load(std::memory_order_acquire); }

    // True once this take ended by filling the data chunk. Nothing was lost
    // when this is seen: the header is patched, the file is closed and
    // recording() is already false — the recording simply reached the length
    // a 32-bit RIFF size field can describe. Cleared by the next start().
    bool sizeLimitReached() const { return sizeLimit_.load(std::memory_order_acquire); }

    // Pure and testable: "iq_20260815_143059_2000000Hz.wav",
    // "audio_20260815_143059_48000Hz.wav". The rate is rounded to the
    // nearest integer Hz (WAV itself stores an integer rate anyway).
    static std::string makeFilename(RecordKind kind, double sampleRateHz,
                                    std::tm localTime);

    // TEST HOOK. What happens at the data-chunk ceiling cannot be staged the
    // honest way — nothing in ctest is going to write 4 GiB to find out — so
    // this moves the ceiling down to a handful of bytes and the same room
    // arithmetic and the same end-of-take path run over a few frames. Call it
    // before start(); lowering it under a take already in progress underflows
    // the room subtraction. Never called by the application.
    void setMaxDataBytesForTest(std::uint64_t bytes);

private:
    // fwrite the first `bytes` of stage_ with truthful accounting: counters
    // advance only by what stdio accepted, and the first short write latches
    // writeFailed_ so a dead disk is not hammered once per block forever.
    void flushStage(std::size_t bytes, std::size_t bytesPerFrame);

    // Patch the two RIFF size fields from dataBytes_ and close the file: the two
    // steps in a row, on the calling thread. Shared by stop() and
    // endAtSizeLimit() so a take that ends by filling up leaves exactly the file
    // a take the user stopped would have.
    void finalizeFile();
    // The recorder's half of stopForFinish: file_, fileBuf_ and the byte count
    // into `out`. No disk. False when there is no file.
    bool detachFile(FinishRequest& out);

    // The data chunk is full: finalize the file and end the take.
    void endAtSizeLimit();

    std::FILE* file_ = nullptr;
    std::atomic<RecordKind> kind_{RecordKind::BasebandIq};
    std::atomic<bool> recording_{false};
    std::atomic<std::uint64_t> frames_{0};
    std::atomic<std::uint64_t> dataBytes_{0};
    // Atomic because the DSP thread latches it and the GUI thread reads it
    // through writeFailed() to say so on the RECORDER card.
    std::atomic<bool> writeFailed_{false};
    // Same reason, and the discriminator between the two ways a take stops
    // taking — see writeFailed()/sizeLimitReached().
    std::atomic<bool> sizeLimit_{false};
    // The data-chunk ceiling in bytes: kMaxDataBytes (just under 4 GiB) in
    // the product, and only setMaxDataBytesForTest ever moves it. Plain
    // rather than atomic because nothing changes it while a take runs.
    std::uint64_t maxDataBytes_;
    std::vector<char> fileBuf_;          // setvbuf storage; must outlive file_
    std::vector<unsigned char> stage_;   // little-endian staging block
    std::string path_;                   // set by start(), on the caller's thread
    Opener opener_;                      // the blocking step; see bindOpener()
};

}  // namespace cascade::core
