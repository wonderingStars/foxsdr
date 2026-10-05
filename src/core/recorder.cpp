// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/recorder.hpp"

#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>

#include "core/unique_file.hpp"

namespace fs = std::filesystem;

namespace cascade::core {

namespace {

// stdio buffer size — the number the header's hot-path budget is computed
// from: 16 MB/s worst case / 256 KiB = ~64 OS spills per second.
constexpr std::size_t kFileBufBytes = 256u * 1024u;

// Staging block in frames. 4096 IQ frames = 32 KiB of encoded bytes: big
// enough that the per-fwrite overhead is noise, small enough to live in a
// member buffer that costs nothing when idle.
constexpr std::size_t kStageFrames = 4096;

// The RIFF size field is 32-bit and equals dataBytes + 36 for this fixed
// 44-byte layout, so the data chunk must stop just short of 4 GiB.
constexpr std::uint64_t kMaxDataBytes = 0xFFFFFFFFull - 36u;

// WAV is little-endian by definition; encode explicitly from bytes (the
// mirror of iq_file_source's decoder) so the writer is correct on any host
// endianness and free of alignment assumptions.
void putU16(unsigned char* p, std::uint16_t x) {
    p[0] = static_cast<unsigned char>(x & 0xFFu);
    p[1] = static_cast<unsigned char>((x >> 8) & 0xFFu);
}

void putU32(unsigned char* p, std::uint32_t x) {
    p[0] = static_cast<unsigned char>(x & 0xFFu);
    p[1] = static_cast<unsigned char>((x >> 8) & 0xFFu);
    p[2] = static_cast<unsigned char>((x >> 16) & 0xFFu);
    p[3] = static_cast<unsigned char>((x >> 24) & 0xFFu);
}

}  // namespace

Recorder::Recorder() : maxDataBytes_(kMaxDataBytes), opener_(&Recorder::openFile) {}

Recorder::~Recorder() {
    stop();
}

std::string Recorder::makeFilename(RecordKind kind, double sampleRateHz,
                                   std::tm localTime) {
    // %04d keeps pre-2000 or post-9999 tm_year values readable rather than
    // truncated; the rate is rounded because WAV stores integer Hz anyway.
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s_%04d%02d%02d_%02d%02d%02d_%lldHz.wav",
                  kind == RecordKind::BasebandIq ? "iq" : "audio",
                  localTime.tm_year + 1900, localTime.tm_mon + 1,
                  localTime.tm_mday, localTime.tm_hour, localTime.tm_min,
                  localTime.tm_sec, std::llround(sampleRateHz));
    return buf;
}

bool Recorder::start(RecordKind kind, const std::string& directory,
                     double sampleRateHz, std::string& error) {
    return start(kind, directory, sampleRateHz, error, std::string{});
}

std::string Recorder::path() const { return path_; }

bool Recorder::start(RecordKind kind, const std::string& directory, double sampleRateHz,
                     std::string& error, const std::string& namePrefix) {
    // THE THREE STEPS IN A ROW, ON THIS THREAD: the open in the middle waits
    // for the disk, so a caller that cannot wait does not come through here
    // (see "Slow disks" in the header).
    OpenRequest req;
    if (!prepare(kind, directory, sampleRateHz, namePrefix, req, error)) { return false; }
    // Set before the open, as it always was, so a refused open still leaves
    // path() naming the file it tried for.
    path_ = req.path;
    OpenedFile opened;
    if (!opener_(req, opened, error)) { return false; }
    return begin(std::move(opened), error);
}

bool Recorder::prepare(RecordKind kind, const std::string& directory, double sampleRateHz,
                       const std::string& namePrefix, OpenRequest& out,
                       std::string& error) const {
    error.clear();
    if (recording_.load(std::memory_order_acquire)) {
        // Refusing beats implicitly finalizing the current take: an implicit
        // stop would silently destroy a recording on a double-click.
        error = "recorder: already recording — stop the current file first";
        return false;
    }

    // The WAV fmt chunk stores the rate (and the derived byte rate) as
    // uint32; a rate they cannot hold would write a header that lies.
    const std::uint16_t channels = (kind == RecordKind::BasebandIq) ? 2 : 1;
    const std::uint16_t bits = (kind == RecordKind::BasebandIq) ? 32 : 16;
    const std::uint16_t blockAlign =
        static_cast<std::uint16_t>(channels * (bits / 8u));
    if (!(sampleRateHz >= 1.0) || sampleRateHz > 4294967295.0) {
        error = "recorder: sample rate " + std::to_string(sampleRateHz) +
                " Hz is not representable in a WAV header";
        return false;
    }
    const std::uint32_t rate =
        static_cast<std::uint32_t>(std::llround(sampleRateHz));
    const std::uint64_t byteRate =
        static_cast<std::uint64_t>(rate) * blockAlign;
    if (byteRate > 0xFFFFFFFFull) {
        error = "recorder: byte rate overflows the WAV header at " +
                std::to_string(rate) + " Hz";
        return false;
    }

    // An empty directory means "here": a blank GUI field should record next
    // to the executable, not fail with a confusing filesystem error.
    const fs::path dir = directory.empty() ? fs::path(".") : fs::path(directory);

    std::tm tmv{};
    const std::time_t now = std::time(nullptr);
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    std::string fileName = makeFilename(kind, sampleRateHz, tmv);
    if (!namePrefix.empty()) {
        char stamp[32];
        std::snprintf(stamp, sizeof stamp, "_%04d%02d%02d_%02d%02d%02d.wav",
                      tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                      tmv.tm_min, tmv.tm_sec);
        fileName = namePrefix + stamp;
    }

    out.kind = kind;
    out.directory = dir.string();
    out.path = (dir / fileName).string();

    // The zero-length header, flushed by the opener as soon as the file
    // exists: this is the crash contract from the class comment. Everything
    // before the data payload is fixed-size, so stop() can patch by absolute
    // offset.
    unsigned char* h = out.header.data();
    std::memcpy(h, "RIFF", 4);
    putU32(h + 4, 36);  // RIFF size for an empty data chunk
    std::memcpy(h + 8, "WAVE", 4);
    std::memcpy(h + 12, "fmt ", 4);
    putU32(h + 16, 16);  // classic 16-byte fmt block
    putU16(h + 20, (kind == RecordKind::BasebandIq) ? 3u : 1u);  // float/PCM
    putU16(h + 22, channels);
    putU32(h + 24, rate);
    putU32(h + 28, static_cast<std::uint32_t>(byteRate));
    putU16(h + 32, blockAlign);
    putU16(h + 34, bits);
    std::memcpy(h + 36, "data", 4);
    putU32(h + 40, 0);
    return true;
}

bool Recorder::openFile(const OpenRequest& req, OpenedFile& out, std::string& error) {
    // EVERYTHING IN HERE WAITS ON THE DISK, and nothing in here may touch a
    // Recorder: it runs on a worker that can be abandoned at quit.
    const fs::path dir(req.directory);
    std::error_code ec;
    fs::create_directories(dir, ec);
    // create_directories is a silent no-op on an existing directory but
    // reports when a plain FILE squats on the path — check both ways.
    if (ec || !fs::is_directory(dir)) {
        error = "recorder: cannot create directory \"" + dir.string() +
                "\": " + (ec ? ec.message() : "path exists and is not a directory");
        return false;
    }

    // THE NAME IS DECIDED HERE, ON THE WORKER, AND THE FILE IS CREATED EXCLUSIVELY (0.99.65,
    // core/unique_file.hpp). req.path is the name the take WANTS (the second it was asked for);
    // when something is there already - the take just finished, one still being finalised, another
    // writer of this process, a stranger's file - the take goes to "name-2.wav", "name-3.wav" ...
    // and never opens over a file. opened.path is the name actually used, which is what begin()
    // keeps and everything that shows a take's name reads. Past kMaxUniqueNames the create fails
    // in the words a refused create always had.
    std::string usedPath;
    bool exhausted = false;
    std::FILE* f = createUnique(req.path, usedPath, exhausted);
    if (f == nullptr) {
        (void)exhausted;  // taken to the end, or refused outright: the same words
        error = "recorder: cannot create \"" + req.path + "\"";
        return false;
    }
    OpenedFile opened;
    opened.kind = req.kind;
    opened.path = usedPath;
    opened.file.reset(f);  // from here a failure closes the file by itself
    // setvbuf must precede the first I/O on the stream. Failure (it cannot
    // realistically fail with valid arguments) just leaves stdio's default
    // buffer — slower flush cadence, still correct — so no check.
    opened.buffer.resize(kFileBufBytes);
    std::setvbuf(f, opened.buffer.data(), _IOFBF, kFileBufBytes);

    if (std::fwrite(req.header.data(), 1, req.header.size(), f) != req.header.size() ||
        std::fflush(f) != 0) {
        error = "recorder: writing the WAV header to \"" + usedPath + "\" failed";
        return false;
    }
    out = std::move(opened);
    return true;
}

bool Recorder::begin(OpenedFile&& opened, std::string& error) {
    error.clear();
    if (recording_.load(std::memory_order_acquire)) {
        error = "recorder: already recording — stop the current file first";
        return false;
    }
    if (!opened.file) {
        error = "recorder: no file was opened";
        return false;
    }
    // The previous take's file is closed (recording_ is false), so its stdio
    // buffer is free to be replaced by the one this stream was set up with.
    fileBuf_ = std::move(opened.buffer);
    stage_.assign(kStageFrames * 8u, 0);  // sized for the larger (IQ) frame
    writeFailed_.store(false, std::memory_order_relaxed);
    sizeLimit_.store(false, std::memory_order_relaxed);
    frames_.store(0, std::memory_order_relaxed);
    dataBytes_.store(0, std::memory_order_relaxed);
    kind_.store(opened.kind, std::memory_order_relaxed);
    file_ = opened.file.release();
    path_ = opened.path;
    recording_.store(true, std::memory_order_release);
    return true;
}

void Recorder::flushStage(std::size_t bytes, std::size_t bytesPerFrame) {
    const std::size_t w = std::fwrite(stage_.data(), 1, bytes, file_);
    // Count only accepted bytes: stop()'s header patch must never claim
    // samples the disk refused. A short write (full/removed disk) latches
    // writeFailed_ so the DSP thread stops paying for a dead stream.
    dataBytes_.fetch_add(w, std::memory_order_relaxed);
    frames_.fetch_add(w / bytesPerFrame, std::memory_order_relaxed);
    if (w != bytes) {
        writeFailed_.store(true, std::memory_order_release);
    }
}

void Recorder::writeIq(const std::complex<float>* s, std::size_t n) {
    if (!recording_.load(std::memory_order_acquire) ||
        kind_.load(std::memory_order_relaxed) != RecordKind::BasebandIq ||
        s == nullptr || n == 0 || writeFailed_.load(std::memory_order_acquire)) {
        return;
    }
    std::size_t done = 0;
    while (done < n && !writeFailed_.load(std::memory_order_relaxed)) {
        // 4 GiB WAV ceiling: `room` is the frames the data chunk can still
        // hold, and none left is the END of the take, not a reason to drop
        // this block. It used to set take = 0 and return — so the file stayed
        // open with the zero-length header start() flushed, recording() went
        // on saying true, and every sample from there on vanished without a
        // word. Truncating the last block to `room` still holds: what fits is
        // written, and the chunk going full closes the file on the spot,
        // either on the next pass of this loop or in the check after it.
        const std::uint64_t room =
            (maxDataBytes_ - dataBytes_.load(std::memory_order_relaxed)) / 8u;
        if (room == 0) {
            endAtSizeLimit();
            return;
        }
        std::size_t take = n - done;
        if (take > kStageFrames) {
            take = kStageFrames;
        }
        if (take > room) {
            take = static_cast<std::size_t>(room);
        }
        unsigned char* p = stage_.data();
        for (std::size_t i = 0; i < take; ++i) {
            // bit_cast + explicit LE bytes: the exact mirror of the file
            // source's decoder, which is what makes the round trip bit-exact.
            putU32(p + 8 * i,
                   std::bit_cast<std::uint32_t>(s[done + i].real()));
            putU32(p + 8 * i + 4,
                   std::bit_cast<std::uint32_t>(s[done + i].imag()));
        }
        flushStage(take * 8u, 8u);
        done += take;
    }
    // A block that lands exactly ON the ceiling leaves the loop with the
    // chunk full and nothing left to write, so end the take here rather than
    // wait for a next call to notice: the user may stop the pipeline, or tune
    // away, and that call may never come. A latched writeFailed_ means the
    // loop stopped for the OTHER reason — the disk refused a write, which can
    // leave the chunk within a frame of full without it being what happened —
    // and that case stays a stall for stop() to finalize.
    if (!writeFailed_.load(std::memory_order_relaxed) &&
        (maxDataBytes_ - dataBytes_.load(std::memory_order_relaxed)) / 8u == 0) {
        endAtSizeLimit();
    }
}

void Recorder::writeAudio(const float* s, std::size_t n) {
    if (!recording_.load(std::memory_order_acquire) ||
        kind_.load(std::memory_order_relaxed) != RecordKind::Audio ||
        s == nullptr || n == 0 || writeFailed_.load(std::memory_order_acquire)) {
        return;
    }
    std::size_t done = 0;
    while (done < n && !writeFailed_.load(std::memory_order_relaxed)) {
        // The ceiling ends the take here too — see writeIq. An audio take is
        // 96 kB/s, so this is a WAV nobody will reach in a sitting; it shares
        // the path anyway because "the file quietly stopped growing" is not a
        // behaviour worth keeping for the rare case either.
        const std::uint64_t room =
            (maxDataBytes_ - dataBytes_.load(std::memory_order_relaxed)) / 2u;
        if (room == 0) {
            endAtSizeLimit();
            return;
        }
        std::size_t take = n - done;
        if (take > kStageFrames) {
            take = kStageFrames;
        }
        if (take > room) {
            take = static_cast<std::size_t>(room);
        }
        unsigned char* p = stage_.data();
        for (std::size_t i = 0; i < take; ++i) {
            float v = s[done + i];
            if (!(v == v)) {
                v = 0.0f;  // a NaN from a demod glitch must not write garbage
            } else if (v > 1.0f) {
                v = 1.0f;
            } else if (v < -1.0f) {
                v = -1.0f;
            }
            // round(v * 32768) in double: float->double and the power-of-two
            // multiply are both exact, so the quantizer is deterministic.
            // +1.0 lands on 32768, which int16 cannot hold -> saturate.
            long q = std::lround(static_cast<double>(v) * 32768.0);
            if (q > 32767) {
                q = 32767;
            }
            putU16(p + 2 * i,
                   static_cast<std::uint16_t>(static_cast<std::int16_t>(q)));
        }
        flushStage(take * 2u, 2u);
        done += take;
    }
    // See writeIq: a block that lands exactly on the ceiling ends the take
    // here rather than leaving a full file open for a call that may not come,
    // and a refused write is the other reason to be here, not this one.
    if (!writeFailed_.load(std::memory_order_relaxed) &&
        (maxDataBytes_ - dataBytes_.load(std::memory_order_relaxed)) / 2u == 0) {
        endAtSizeLimit();
    }
}

void Recorder::endAtSizeLimit() {
    // THE DATA CHUNK IS FULL, AND THAT IS THE END OF THE TAKE — said out
    // loud, because the alternative was saying nothing at all. At the app's
    // default 2 Msps an IQ take fills it in about four and a half minutes,
    // and until now that moment produced no file change, no state change and
    // no message: the panel and the status card went on showing REC with the
    // elapsed clock climbing over a recording that had already stopped.
    //
    // Ending it means the three things stop() means. The header is patched
    // with the bytes actually written, so the file on disk is a complete,
    // playable WAV of everything that fitted rather than the zero-length husk
    // start() flushed. recording() goes false, so nothing goes on claiming a
    // take that is over. And the fault flags latch: writeFailed_ because its
    // question is "is anything still reaching the file", whose answer is no,
    // and sizeLimit_ because a UI needs to know the answer is "it is full and
    // safely closed" rather than "the disk refused it".
    sizeLimit_.store(true, std::memory_order_relaxed);
    // recording_ BEFORE writeFailed_: a reader that samples recording() and
    // then writeFailed() would otherwise have a wide window to catch the pair
    // as "recording, and the disk refused it" — a sentence about a fault that
    // did not happen. This order shrinks that window to the gap between the
    // reader's own two loads.
    recording_.store(false, std::memory_order_release);
    writeFailed_.store(true, std::memory_order_release);
    finalizeFile();
}

namespace {
std::atomic<Recorder::FinishHook> g_finishHook{nullptr};
}  // namespace

void Recorder::setFinishHookForTest(FinishHook hook) { g_finishHook.store(hook); }

bool Recorder::detachFile(FinishRequest& out) {
    if (file_ == nullptr) {
        return false;  // idempotent, and safe without a prior start()
    }
    // The buffer BEFORE the file in `out` (declaration order makes it die after
    // it), and both moved out of the recorder, so a following begin() finds an
    // empty fileBuf_ rather than the storage a finishing stream still uses.
    out.buffer = std::move(fileBuf_);
    fileBuf_.clear();
    out.file.reset(file_);
    file_ = nullptr;
    out.dataBytes = dataBytes_.load(std::memory_order_relaxed);
    return true;
}

bool Recorder::finishFile(FinishRequest&& req) {
    // EVERYTHING FROM HERE WAITS ON THE DISK, and nothing in here may touch a
    // Recorder: it runs on a worker that can be abandoned at quit.
    FinishRequest owned = std::move(req);
    if (!owned.file) { return true; }  // nothing to finish
    std::FILE* f = owned.file.get();
    if (const FinishHook hook = g_finishHook.load()) { hook(f); }
    const std::uint64_t data = owned.dataBytes;
    unsigned char sz[4];
    bool ok = true;
    // Flush the tail of the sample stream before seeking: fseek on an
    // update stream requires it anyway, and it puts every accepted byte on
    // disk before the header starts claiming them.
    if (std::fflush(f) != 0) { ok = false; }
    // Best effort from here: if the patch itself fails (device vanished),
    // closing still leaves the parseable zero-length header from start().
    if (std::fseek(f, 4, SEEK_SET) == 0) {
        putU32(sz, static_cast<std::uint32_t>(36u + data));
        if (std::fwrite(sz, 1, 4, f) != 4) { ok = false; }
    } else {
        ok = false;
    }
    if (std::fseek(f, 40, SEEK_SET) == 0) {
        putU32(sz, static_cast<std::uint32_t>(data));
        if (std::fwrite(sz, 1, 4, f) != 4) { ok = false; }
    } else {
        ok = false;
    }
    // fclose flushes the two patches: its answer is the last word on whether they
    // reached the file. Released from the owner so it is closed exactly once.
    std::FILE* raw = owned.file.release();
    if (std::fclose(raw) != 0) { ok = false; }
    return ok;
}

void Recorder::finalizeFile() {
    FinishRequest req;
    if (detachFile(req)) { (void)finishFile(std::move(req)); }
}

bool Recorder::stopForFinish(FinishRequest& out) {
    recording_.store(false, std::memory_order_release);
    // False when the take already ended at the size limit and closed its own
    // file, so the Stop button after that still works and still leaves the
    // counters alone.
    return detachFile(out);
}

void Recorder::stop() {
    recording_.store(false, std::memory_order_release);
    // A no-op when the take already ended at the size limit and closed its
    // own file, so the Stop button after that still works and still leaves
    // the counters alone.
    finalizeFile();
    // Counters deliberately keep their final values until the next start().
}

void Recorder::setMaxDataBytesForTest(std::uint64_t bytes) {
    maxDataBytes_ = bytes;
}

bool Recorder::recording() const {
    return recording_.load(std::memory_order_acquire);
}

RecordKind Recorder::kind() const {
    return kind_.load(std::memory_order_relaxed);
}

std::uint64_t Recorder::samplesWritten() const {
    return frames_.load(std::memory_order_relaxed);
}

std::uint64_t Recorder::bytesWritten() const {
    return dataBytes_.load(std::memory_order_relaxed);
}

}  // namespace cascade::core
