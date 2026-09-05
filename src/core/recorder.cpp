// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/recorder.hpp"

#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>

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

Recorder::Recorder() = default;

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
    std::error_code ec;
    fs::create_directories(dir, ec);
    // create_directories is a silent no-op on an existing directory but
    // reports when a plain FILE squats on the path — check both ways.
    if (ec || !fs::is_directory(dir)) {
        error = "recorder: cannot create directory \"" + dir.string() +
                "\": " + (ec ? ec.message() : "path exists and is not a directory");
        return false;
    }

    std::tm tmv{};
    const std::time_t now = std::time(nullptr);
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    const fs::path path = dir / makeFilename(kind, sampleRateHz, tmv);

    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    if (f == nullptr) {
        error = "recorder: cannot create \"" + path.string() + "\"";
        return false;
    }
    // setvbuf must precede the first I/O on the stream. Failure (it cannot
    // realistically fail with valid arguments) just leaves stdio's default
    // buffer — slower flush cadence, still correct — so no check.
    if (fileBuf_.size() != kFileBufBytes) {
        fileBuf_.resize(kFileBufBytes);
    }
    std::setvbuf(f, fileBuf_.data(), _IOFBF, kFileBufBytes);

    // The zero-length header, flushed immediately: this is the crash
    // contract from the class comment. Everything before the data payload
    // is fixed-size, so stop() can patch by absolute offset.
    unsigned char h[44];
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
    if (std::fwrite(h, 1, sizeof h, f) != sizeof h || std::fflush(f) != 0) {
        std::fclose(f);
        error = "recorder: writing the WAV header to \"" + path.string() +
                "\" failed";
        return false;
    }

    stage_.assign(kStageFrames * 8u, 0);  // sized for the larger (IQ) frame
    writeFailed_.store(false, std::memory_order_relaxed);
    frames_.store(0, std::memory_order_relaxed);
    dataBytes_.store(0, std::memory_order_relaxed);
    kind_.store(kind, std::memory_order_relaxed);
    file_ = f;
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
        std::size_t take = n - done;
        if (take > kStageFrames) {
            take = kStageFrames;
        }
        // 4 GiB WAV ceiling: truncate rather than corrupt the size fields.
        const std::uint64_t room =
            (kMaxDataBytes - dataBytes_.load(std::memory_order_relaxed)) / 8u;
        if (take > room) {
            take = static_cast<std::size_t>(room);
            if (take == 0) {
                return;
            }
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
}

void Recorder::writeAudio(const float* s, std::size_t n) {
    if (!recording_.load(std::memory_order_acquire) ||
        kind_.load(std::memory_order_relaxed) != RecordKind::Audio ||
        s == nullptr || n == 0 || writeFailed_.load(std::memory_order_acquire)) {
        return;
    }
    std::size_t done = 0;
    while (done < n && !writeFailed_.load(std::memory_order_relaxed)) {
        std::size_t take = n - done;
        if (take > kStageFrames) {
            take = kStageFrames;
        }
        const std::uint64_t room =
            (kMaxDataBytes - dataBytes_.load(std::memory_order_relaxed)) / 2u;
        if (take > room) {
            take = static_cast<std::size_t>(room);
            if (take == 0) {
                return;
            }
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
}

void Recorder::stop() {
    recording_.store(false, std::memory_order_release);
    if (file_ == nullptr) {
        return;  // idempotent, and safe without a prior start()
    }
    std::FILE* f = file_;
    file_ = nullptr;
    const std::uint64_t data = dataBytes_.load(std::memory_order_relaxed);
    unsigned char sz[4];
    // Flush the tail of the sample stream before seeking: fseek on an
    // update stream requires it anyway, and it puts every accepted byte on
    // disk before the header starts claiming them.
    std::fflush(f);
    // Best effort from here: if the patch itself fails (device vanished),
    // closing still leaves the parseable zero-length header from start().
    if (std::fseek(f, 4, SEEK_SET) == 0) {
        putU32(sz, static_cast<std::uint32_t>(36u + data));
        std::fwrite(sz, 1, 4, f);
    }
    if (std::fseek(f, 40, SEEK_SET) == 0) {
        putU32(sz, static_cast<std::uint32_t>(data));
        std::fwrite(sz, 1, 4, f);
    }
    std::fclose(f);
    // Counters deliberately keep their final values until the next start().
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
