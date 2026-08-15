// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/iq_file_source.hpp"

#include <bit>
#include <cstring>

namespace cascade::source {

namespace {

// Disk-read granularity in frames. 4096 frames is 16-32 KiB per read —
// small enough that the "never load the whole file" streaming contract holds
// for any capture size, large enough that per-read overhead is negligible
// against the pipeline's block cadence.
constexpr std::size_t kIoFrames = 4096;

// WAV is little-endian by definition; decode explicitly from bytes instead of
// type-punning the buffer, so the parser is correct on any host endianness
// and free of alignment assumptions.
std::uint16_t u16le(const unsigned char* p) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                      static_cast<std::uint16_t>(p[1] << 8));
}

std::uint32_t u32le(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::int16_t i16le(const unsigned char* p) {
    // uint16 -> int16 is well-defined two's-complement modulo in C++20.
    return static_cast<std::int16_t>(u16le(p));
}

}  // namespace

bool IqFileSource::fail(std::string msg) {
    lastError_ = std::move(msg);
    // A half-parsed source must not be startable: leave every playback field
    // in the "never opened" state so start()/read() stay inert.
    opened_ = false;
    format_ = SampleFormat::none;
    sampleRateHz_ = 0.0;
    frameCount_ = 0;
    nextFrame_ = 0;
    dataOffset_ = 0;
    bytesPerFrame_ = 0;
    if (file_.is_open()) {
        file_.close();
    }
    return false;
}

bool IqFileSource::open(const std::string& wavPath) {
    // Replacing the file mid-playback would tear the source thread's view;
    // stop first (the caller must already have parked read(), per the
    // threading contract — this just makes the state transition explicit).
    stop();
    opened_ = false;
    format_ = SampleFormat::none;
    lastError_.clear();
    name_ = "IQ file";
    if (file_.is_open()) {
        file_.close();
    }
    file_.clear();
    file_.open(wavPath, std::ios::binary);
    if (!file_) {
        return fail("cannot open file: " + wavPath);
    }

    file_.seekg(0, std::ios::end);
    const std::uint64_t fileSize = static_cast<std::uint64_t>(
        static_cast<std::streamoff>(file_.tellg()));

    // Positioned reads keep the chunk walk independent of stream state.
    auto readAt = [this](std::uint64_t off, unsigned char* buf,
                         std::size_t len) -> bool {
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(off));
        file_.read(reinterpret_cast<char*>(buf),
                   static_cast<std::streamsize>(len));
        return static_cast<std::size_t>(file_.gcount()) == len;
    };

    unsigned char hdr[12];
    if (fileSize < 12 || !readAt(0, hdr, 12) ||
        std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
        return fail("not a RIFF/WAVE file: " + wavPath);
    }
    // The RIFF size field itself is deliberately ignored: real-world writers
    // routinely leave it stale, and the chunk walk below is bounded by the
    // actual file size, which is the ground truth.

    bool haveFmt = false;
    bool haveData = false;
    std::uint16_t formatTag = 0;
    std::uint16_t channels = 0;
    std::uint32_t rateHz = 0;
    std::uint16_t bits = 0;
    std::uint64_t dataOff = 0;
    std::uint64_t dataSize = 0;

    // Walk every chunk: unknown ones (LIST, JUNK, bext, ...) are skipped by
    // size. RIFF chunks are word-aligned, so an odd size carries one pad
    // byte that belongs to the chunk, not to the next header.
    std::uint64_t pos = 12;
    while (pos + 8 <= fileSize) {
        unsigned char ch[8];
        if (!readAt(pos, ch, 8)) {
            break;
        }
        const std::uint32_t size = u32le(ch + 4);
        const std::uint64_t body = pos + 8;
        if (std::memcmp(ch, "fmt ", 4) == 0 && !haveFmt) {
            // 16 bytes is the classic PCM fmt block; smaller cannot even
            // hold the fields we need to identify the format.
            if (size < 16) {
                return fail("fmt chunk too small (" + std::to_string(size) +
                            " bytes, need 16)");
            }
            unsigned char f[16];
            if (!readAt(body, f, 16)) {
                return fail("fmt chunk extends past end of file");
            }
            formatTag = u16le(f);
            channels = u16le(f + 2);
            rateHz = u32le(f + 4);
            bits = u16le(f + 14);
            haveFmt = true;
        } else if (std::memcmp(ch, "data", 4) == 0 && !haveData) {
            dataOff = body;
            dataSize = size;
            haveData = true;
        }
        pos = body + size + (size & 1u);
    }

    if (!haveFmt) {
        return fail("missing fmt chunk");
    }
    // Format tag before channel count: an exotic codec's channel field may
    // not mean what PCM's does, so reject the codec first.
    if (formatTag != 1 && formatTag != 3) {
        return fail("unsupported format tag " + std::to_string(formatTag) +
                    " (only PCM=1 and IEEE float=3; ADPCM/extensible are "
                    "not supported)");
    }
    if (channels != 2) {
        return fail("IQ playback needs exactly 2 channels (ch0=I, ch1=Q); "
                    "file has " + std::to_string(channels));
    }
    if (formatTag == 1 && bits != 16) {
        return fail("unsupported PCM bit depth " + std::to_string(bits) +
                    "-bit (only 16-bit PCM)");
    }
    if (formatTag == 3 && bits != 32) {
        return fail("unsupported IEEE float bit depth " + std::to_string(bits) +
                    "-bit (only 32-bit float)");
    }
    if (rateHz == 0) {
        return fail("fmt chunk declares a zero sample rate");
    }
    if (!haveData) {
        return fail("missing data chunk");
    }

    const std::size_t bpf = static_cast<std::size_t>(channels) * (bits / 8u);
    if (dataOff + dataSize > fileSize) {
        return fail("truncated data chunk: header declares " +
                    std::to_string(dataSize) + " bytes but only " +
                    std::to_string(fileSize - dataOff) + " remain in the file");
    }
    // Complete frames only; a trailing partial frame is unplayable and is
    // excluded so the loop seam stays frame-aligned (see header contract).
    const std::uint64_t frames = dataSize / bpf;
    if (frames == 0) {
        return fail("data chunk holds no complete sample frames");
    }

    format_ = (formatTag == 1) ? SampleFormat::pcm16 : SampleFormat::float32;
    sampleRateHz_ = static_cast<double>(rateHz);
    dataOffset_ = dataOff;
    frameCount_ = frames;
    nextFrame_ = 0;
    bytesPerFrame_ = bpf;
    ioBuf_.assign(kIoFrames * bpf, 0);
    const std::size_t sep = wavPath.find_last_of("/\\");
    name_ = "IQ file: " +
            (sep == std::string::npos ? wavPath : wavPath.substr(sep + 1));
    opened_ = true;
    return true;
}

bool IqFileSource::start() {
    if (!opened_) {
        lastError_ = "start() before a successful open()";
        return false;
    }
    if (running_.load(std::memory_order_acquire)) {
        return true;  // idempotent: a running source keeps its position
    }
    // Rewind so every start() replays deterministically from frame 0 —
    // reproducibility is the whole point of a file source.
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(dataOffset_));
    nextFrame_ = 0;
    running_.store(true, std::memory_order_release);
    return true;
}

void IqFileSource::stop() {
    running_.store(false, std::memory_order_release);
}

bool IqFileSource::running() const {
    return running_.load(std::memory_order_acquire);
}

bool IqFileSource::setSampleRateHz(double hz) {
    (void)hz;  // the recording fixes the rate; the request is refused as-is
    return false;
}

bool IqFileSource::setCenterFrequencyHz(double hz) {
    centerFrequencyHz_ = hz;
    return true;
}

void IqFileSource::convertFrames(const unsigned char* raw,
                                 std::complex<float>* dst, std::size_t frames) {
    if (format_ == SampleFormat::pcm16) {
        // 1/32768 (not 1/32767): a power-of-two divisor makes every int16
        // exactly representable after scaling, and maps the full code range
        // onto [-1, 1) — the documented convention.
        constexpr float kScale = 1.0f / 32768.0f;
        for (std::size_t i = 0; i < frames; ++i) {
            const unsigned char* p = raw + i * 4;
            dst[i] = std::complex<float>(
                static_cast<float>(i16le(p)) * kScale,
                static_cast<float>(i16le(p + 2)) * kScale);
        }
    } else {
        // bit_cast from the assembled little-endian word: bit-exact
        // passthrough with no aliasing or alignment hazards.
        for (std::size_t i = 0; i < frames; ++i) {
            const unsigned char* p = raw + i * 8;
            dst[i] = std::complex<float>(std::bit_cast<float>(u32le(p)),
                                         std::bit_cast<float>(u32le(p + 4)));
        }
    }
}

std::size_t IqFileSource::read(std::complex<float>* dst, std::size_t n) {
    if (!running_.load(std::memory_order_acquire) || dst == nullptr || n == 0) {
        // Not an error, so lastError_ is deliberately untouched: the
        // pipeline may poll a stopped source and must not see error spam.
        return 0;
    }
    std::size_t delivered = 0;
    while (delivered < n) {
        if (nextFrame_ >= frameCount_) {
            // The loop seam: jump straight back to frame 0 so the first
            // frame follows the last with no gap and no repeats. clear()
            // first — the previous read may have parked the stream on EOF
            // when the data chunk runs to the end of the file.
            file_.clear();
            file_.seekg(static_cast<std::streamoff>(dataOffset_));
            nextFrame_ = 0;
        }
        std::size_t want = n - delivered;
        const std::uint64_t left = frameCount_ - nextFrame_;
        if (static_cast<std::uint64_t>(want) > left) {
            want = static_cast<std::size_t>(left);
        }
        if (want > kIoFrames) {
            want = kIoFrames;
        }
        const std::size_t wantBytes = want * bytesPerFrame_;
        file_.read(reinterpret_cast<char*>(ioBuf_.data()),
                   static_cast<std::streamsize>(wantBytes));
        if (static_cast<std::size_t>(file_.gcount()) != wantBytes) {
            // open() validated that the data chunk fits the file, so a short
            // read can only mean the file changed underneath us or the disk
            // failed. Zero-fill and still return n: a free-running source
            // returning 0 would read as "retry" to the pacing loop.
            lastError_ = "I/O error while reading '" + name_ +
                         "' (file truncated or removed during playback?)";
            for (; delivered < n; ++delivered) {
                dst[delivered] = std::complex<float>(0.0f, 0.0f);
            }
            return n;
        }
        convertFrames(ioBuf_.data(), dst + delivered, want);
        nextFrame_ += want;
        delivered += want;
    }
    return delivered;
}

}  // namespace cascade::source
