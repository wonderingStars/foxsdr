// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/iq_file_source.hpp"
#include "source/file_start_time.hpp"
#include "source/wav_meta.hpp"   // wire-a

#include <algorithm>   // wire-a
#include <bit>
#include <cmath>
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

// NaN and the infinities become true silence; every finite value, -0 and
// subnormals included, passes through untouched.
float sanitize(float v) {
    return std::isfinite(v) ? v : 0.0f;
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
    faulted_.store(false, std::memory_order_release);
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

    // Positioned reads keep the chunk walk independent of stream state, and
    // they go through readHeaderRaw for the same reason read()'s chunk loop
    // goes through readRaw: it is the seam where the disk can be made to fail
    // or block on demand, and open() runs on the GUI thread where the blocking
    // work is the thing being measured. Behaviourally identical to the inline
    // read it replaces — the default body IS that read.
    auto readAt = [this](std::uint64_t off, unsigned char* buf,
                         std::size_t len) -> bool {
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(off));
        return readHeaderRaw(buf, len) == len;
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
    //
    // BOUNDED, because this loop runs on the GUI thread and its trip count was
    // set entirely by the file's own contents. A chunk that declares size 0
    // advances `pos` by exactly 8, so a file of zero-size chunks costs one
    // seek+read per 8 bytes: over a gigabyte-scale capture that is 134 million
    // blocking reads before Open returns, with the window frozen throughout —
    // and it needs no hostile author, a truncated or partially-overwritten
    // recording produces the same run of zeros. The cap is the same trade
    // read()'s chunking makes: bound the work, then say plainly what was not
    // finished. It cannot reject a real file — a WAV with more than 256 chunks
    // before both fmt and data does not occur outside a fuzzer.
    //
    // What this does NOT fix, stated because it is the reason the loop was
    // looked at: ONE read against a dead SMB share still parks for that
    // filesystem's own timeout, and nothing in user space can shorten it.
    // open() cannot honour an abort flag the way read() does either — read()'s
    // flag is set by a different thread, whereas open() runs ON the thread
    // that would have to set it. Bounding the COUNT is the part that is
    // actually in this code's gift.
    std::uint64_t pos = 12;
    std::size_t walked = 0;
    for (; walked < kMaxHeaderChunks && pos + 8 <= fileSize; ++walked) {
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
        // Nothing after this point can change the outcome: both finders are
        // one-shot (`&& !haveFmt`, `&& !haveData`), so every later chunk is
        // read and discarded. Stopping here is outcome-preserving and takes
        // the trailing-metadata reads of a broadcast WAV off the GUI thread.
        if (haveFmt && haveData) {
            break;
        }
    }
    if (walked == kMaxHeaderChunks && !(haveFmt && haveData)) {
        // Named separately from "missing fmt chunk": the difference between a
        // file that lacks a chunk and one whose chunk table was not walked to
        // the end is the whole reason a user would look at the message.
        return fail("gave up walking RIFF chunks after " +
                    std::to_string(kMaxHeaderChunks) +
                    " of them without finding both fmt and data (corrupt file?)");
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
    position_.store(0, std::memory_order_release);
    seekRequest_.store(-1, std::memory_order_release);
    loopOn_.store(false, std::memory_order_release);
    loopA_.store(0, std::memory_order_release);
    loopB_.store(0, std::memory_order_release);
    bytesPerFrame_ = bpf;
    ioBuf_.assign(kIoFrames * bpf, 0);
    {
        const auto when = cascade::source::recordingStart(wavPath, static_cast<double>(frames) / static_cast<double>(rateHz));
        startUnixMs_ = when.first;
        startSource_ = when.second;
    }
    const std::size_t sep = wavPath.find_last_of("/\\");
    name_ = "IQ file: " +
            (sep == std::string::npos ? wavPath : wavPath.substr(sep + 1));
    // wire-a begin: the centre the recorder wrote into the file's own header (source/wav_meta.hpp) is the file's centre; a file without one keeps the nominal
    {
        const cascade::source::wavmeta::WavMeta meta = cascade::source::wavmeta::readWavMeta(std::filesystem::path(wavPath));
        centerFromHeader_ = meta.ok && meta.centerKnown;
        if (centerFromHeader_) { centerFrequencyHz_ = meta.centerHz; }
    }
    // wire-a end
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
    // (a seek made while stopped is honoured: the position bar is a promise)
    const std::int64_t pending = seekRequest_.exchange(-1, std::memory_order_acq_rel);
    seekTo(pending >= 0 && static_cast<std::uint64_t>(pending) < frameCount_ ? static_cast<std::uint64_t>(pending) : 0u);
    win_.clear(); frac_ = 0.0; grainLeft_ = 2048; skipFrac_ = 0.0;   // wire-a: the transport starts from the top as well
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
        // passthrough with no aliasing or alignment hazards — except that a
        // float32 WAV can encode NaN and both infinities, and no stage of the
        // DSP chain defends its sample path against them. One such sample
        // latches the AGC gain, the squelch/S-meter EMA and the noise
        // reducer's spectrum, which is heard as the receiver going silent
        // while the spectrum display stays alive. This decoder is the entry
        // point, so it is where a corrupt or crafted capture is turned back
        // into silence. The int16 branch above needs no equivalent: every
        // int16 scaled by 2^-15 is finite by construction.
        for (std::size_t i = 0; i < frames; ++i) {
            const unsigned char* p = raw + i * 8;
            dst[i] = std::complex<float>(sanitize(std::bit_cast<float>(u32le(p))),
                                         sanitize(std::bit_cast<float>(u32le(p + 4))));
        }
    }
}

void IqFileSource::seekTo(std::uint64_t frame) {
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(dataOffset_ + frame * bytesPerFrame_));
    nextFrame_ = frame;
    position_.store(frame, std::memory_order_release);
}

bool IqFileSource::seekFrame(std::uint64_t frame) {
    if (!opened_ || frame >= frameCount_) { return false; }
    seekRequest_.store(static_cast<std::int64_t>(frame), std::memory_order_release);
    position_.store(frame, std::memory_order_release);   // the bar jumps the moment it is clicked
    return true;
}

bool IqFileSource::setLoopFrames(std::uint64_t a, std::uint64_t b) {
    if (!opened_ || !(a < b) || b > frameCount_) { return false; }
    loopA_.store(a, std::memory_order_release);
    loopB_.store(b, std::memory_order_release);
    loopOn_.store(true, std::memory_order_release);
    return true;
}

void IqFileSource::clearLoop() { loopOn_.store(false, std::memory_order_release); }

std::size_t IqFileSource::readRaw(unsigned char* dst, std::size_t bytes) {
    file_.read(reinterpret_cast<char*>(dst),
               static_cast<std::streamsize>(bytes));
    return static_cast<std::size_t>(file_.gcount());
}

std::size_t IqFileSource::readHeaderRaw(unsigned char* dst, std::size_t bytes) {
    // QUALIFIED on purpose: the header path must reach the ifstream itself,
    // not whatever a subclass did to the streaming seam. See the header.
    return IqFileSource::readRaw(dst, bytes);
}

// wire-a second pass begin: the transport around the unchanged read (see the header). At speed 1 and not paused this is exactly readNormal.
bool IqFileSource::setPlaySpeed(double s) {
    if (!std::isfinite(s) || s < kMinPlaySpeed || s > kMaxPlaySpeed) { return false; }
    speed_.store(s, std::memory_order_release);
    return true;
}

bool IqFileSource::stepBlocks(long long n) {
    if (!opened_ || !paused_.load(std::memory_order_acquire) || n == 0 || n > kMaxStepBlocks || n < -kMaxStepBlocks) { return false; }
    if (n > 0) {
        stepFrames_.fetch_add(n * static_cast<long long>(kStepBlockFrames), std::memory_order_acq_rel);
        return true;
    }
    const std::uint64_t back = static_cast<std::uint64_t>(-n) * kStepBlockFrames;
    const std::uint64_t cur = position_.load(std::memory_order_acquire);
    seekFrame(cur > back ? cur - back : 0);
    stepFrames_.fetch_add(static_cast<long long>(kStepBlockFrames), std::memory_order_acq_rel);
    return true;
}

std::size_t IqFileSource::read(std::complex<float>* dst, std::size_t n) {
    if (!running_.load(std::memory_order_acquire) || dst == nullptr || n == 0) { return 0; }
    const bool paused = paused_.load(std::memory_order_acquire);
    if (paused) {
        const long long pend = stepFrames_.load(std::memory_order_acquire);
        if (pend <= 0) { return 0; }
        n = std::min<std::size_t>(n, static_cast<std::size_t>(pend));
    }
    const double s = speed_.load(std::memory_order_acquire);
    std::size_t got;
    if (s == 1.0) {
        if (!win_.empty()) { win_.clear(); frac_ = 0.0; }
        got = readNormal(dst, n);
    } else if (blockSkip_.load(std::memory_order_acquire)) {
        got = readSkipping(dst, n, s);
    } else {
        got = readVaried(dst, n, s);
    }
    if (paused && got > 0) { stepFrames_.fetch_sub(static_cast<long long>(got), std::memory_order_acq_rel); }
    return got;
}

std::size_t IqFileSource::readVaried(std::complex<float>* dst, std::size_t n, double s) {
    if (seekRequest_.load(std::memory_order_acquire) >= 0) { win_.clear(); frac_ = 0.0; }   // a seek is about to move the stream: what was held is from the old place
    const std::size_t need = static_cast<std::size_t>(std::floor(frac_ + static_cast<double>(n - 1) * s)) + 2;
    while (win_.size() < need) {
        const std::size_t want = std::min<std::size_t>(need - win_.size(), kIoFrames * 4);
        const std::size_t old = win_.size();
        win_.resize(old + want);
        const std::size_t got = readNormal(win_.data() + old, want);
        win_.resize(old + got);
        if (got < want) { break; }   // stopped
    }
    std::size_t out = 0;
    for (; out < n; ++out) {
        const std::size_t i = static_cast<std::size_t>(frac_);
        if (i + 1 >= win_.size()) { break; }
        const float t = static_cast<float>(frac_ - static_cast<double>(i));
        dst[out] = win_[i] * (1.0f - t) + win_[i + 1] * t;
        frac_ += s;
    }
    const std::size_t drop = std::min<std::size_t>(static_cast<std::size_t>(frac_), win_.size());
    if (drop > 0) {
        win_.erase(win_.begin(), win_.begin() + static_cast<std::ptrdiff_t>(drop));
        frac_ -= static_cast<double>(drop);
    }
    return out;
}

void IqFileSource::jumpBy(long long frames) {
    const bool looping = loopOn_.load(std::memory_order_acquire);
    const std::uint64_t a = loopA_.load(std::memory_order_acquire);
    const std::uint64_t b = loopB_.load(std::memory_order_acquire);
    const bool loopOk = looping && a < b && b <= frameCount_;
    const std::uint64_t start = loopOk ? a : 0u;
    const std::uint64_t end = loopOk ? b : frameCount_;
    if (end <= start) { return; }
    std::uint64_t pos = nextFrame_;
    if (pos < start || pos > end) { pos = start; }
    if (frames >= 0) {
        pos = start + (pos - start + static_cast<std::uint64_t>(frames)) % (end - start);
    } else {
        const std::uint64_t back = static_cast<std::uint64_t>(-frames);
        pos = pos > start + back ? pos - back : start;
    }
    seekTo(pos);
}

std::size_t IqFileSource::readSkipping(std::complex<float>* dst, std::size_t n, double s) {
    constexpr std::size_t kGrain = 2048;
    if (seekRequest_.load(std::memory_order_acquire) >= 0) { grainLeft_ = kGrain; skipFrac_ = 0.0; }   // the seek starts a fresh grain
    std::size_t done = 0;
    while (done < n) {
        if (grainLeft_ == 0) {
            const double jump = static_cast<double>(kGrain) * (s - 1.0) + skipFrac_;
            const double whole = std::floor(jump);
            skipFrac_ = jump - whole;
            jumpBy(static_cast<long long>(whole));
            grainLeft_ = kGrain;
        }
        const std::size_t got = readNormal(dst + done, std::min(n - done, grainLeft_));
        if (got == 0) { break; }
        grainLeft_ -= got;
        done += got;
    }
    return done;
}

std::size_t IqFileSource::readNormal(std::complex<float>* dst, std::size_t n) {
    if (!running_.load(std::memory_order_acquire) || dst == nullptr || n == 0) {
        // Not an error, so lastError_ is deliberately untouched: the
        // pipeline may poll a stopped source and must not see error spam.
        return 0;
    }
    std::size_t delivered = 0;
    while (delivered < n) {
        // Abort point between chunks. A large request is served as a sequence
        // of bounded disk reads, and on a healthy local file the whole loop
        // costs microseconds — but the backing file can be an SMB share that
        // just went away or a USB disk that is failing, where ONE read parks
        // in the filesystem for its own timeout. The pipeline's stop() and
        // setSource() both clear their run flag, call this source's stop(),
        // then JOIN the source thread, so every further read this loop
        // chooses to issue after that stop() is time the GUI spends frozen.
        // Re-checking here bounds that to the single read already in flight
        // instead of ceil(n / chunk) of them.
        //
        // The read in flight is deliberately NOT cancelled. The Windows
        // mechanism for that is CancelSynchronousIo against the reading
        // thread, which this class cannot use honestly: it never learns that
        // thread's handle (the Pipeline owns the thread and only ever calls
        // read() on it), the cancellation would have to be raced against the
        // thread leaving read() or it lands on unrelated I/O, and it leaves
        // the ifstream in an implementation-defined state mid-buffer. A
        // chunk-bounded stall with the stream intact is the conservative
        // trade; the alternative buys one chunk of latency for a Win32-only
        // cancellation protocol and an undefined stream.
        //
        // Returning short here is not the "free-running sources never return
        // 0" case: the source is stopped, which is exactly the state whose
        // documented answer is 0 (see the guard above). lastError_ stays
        // untouched — a stop is not an I/O failure.
        if (!running_.load(std::memory_order_acquire)) {
            return delivered;
        }
        // A seek asked for since the last chunk is applied before anything is produced from the old place.
        const std::int64_t pending = seekRequest_.exchange(-1, std::memory_order_acq_rel);
        if (pending >= 0 && static_cast<std::uint64_t>(pending) < frameCount_) { seekTo(static_cast<std::uint64_t>(pending)); }
        // The end of what plays: the loop's end while a loop is set (and it lies inside the file), else the end of the file.
        const bool looping = loopOn_.load(std::memory_order_acquire);
        const std::uint64_t loopA = loopA_.load(std::memory_order_acquire);
        const std::uint64_t loopB = loopB_.load(std::memory_order_acquire);
        const bool loopOk = looping && loopA < loopB && loopB <= frameCount_;
        const std::uint64_t endFrame = loopOk ? loopB : frameCount_;
        if (nextFrame_ >= endFrame) {
            // The loop seam: jump straight back (to the loop's start, else to frame 0) so the first frame follows the last
            // with no gap and no repeats. clear() first - the previous read may have parked the stream on EOF
            // when the data chunk runs to the end of the file.
            seekTo(loopOk ? loopA : 0u);
        }
        std::size_t want = n - delivered;
        const std::uint64_t left = endFrame - nextFrame_;
        if (static_cast<std::uint64_t>(want) > left) {
            want = static_cast<std::size_t>(left);
        }
        if (want > kIoFrames) {
            want = kIoFrames;
        }
        const std::size_t wantBytes = want * bytesPerFrame_;
        if (readRaw(ioBuf_.data(), wantBytes) != wantBytes) {
            // open() validated that the data chunk fits the file, so a short
            // read can only mean the file changed underneath us or the disk
            // failed. Zero-fill and still return n: a free-running source
            // returning 0 would read as "retry" to the pacing loop.
            lastError_ = "I/O error while reading '" + name_ +
                         "' (file truncated or removed during playback?)";
            // The flag the pipeline's source thread actually polls - see
            // faulted() in the header for why the message alone was not
            // enough.
            faulted_.store(true, std::memory_order_release);
            for (; delivered < n; ++delivered) {
                dst[delivered] = std::complex<float>(0.0f, 0.0f);
            }
            return n;
        }
        convertFrames(ioBuf_.data(), dst + delivered, want);
        nextFrame_ += want;
        if (seekRequest_.load(std::memory_order_acquire) < 0) { position_.store(nextFrame_, std::memory_order_release); }   // (a seek asked for meanwhile owns the bar)
        delivered += want;
    }
    return delivered;
}

}  // namespace cascade::source
