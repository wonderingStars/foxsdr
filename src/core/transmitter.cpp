// The transmit thread and the rules about when it may run. See
// transmitter.hpp for the hard rule this file is built around and for why the
// rate matching is two stages.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/transmitter.hpp"

#include "core/diag_log.hpp"
#include "core/i18n.hpp"  // FOX_TR_NOOP: the input names are drawn through trId()

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace cascade::core {

namespace {

struct InputRow {
    TxInput input;
    const char* name;
};

constexpr InputRow kInputs[kTxInputCount] = {
    {TxInput::Microphone, FOX_TR_NOOP("MIC")},
    {TxInput::Tone, FOX_TR_NOOP("TONE")},
};

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

const char* txInputName(TxInput in) {
    const int i = static_cast<int>(in);
    if (i < 0 || i >= kTxInputCount) { return kInputs[0].name; }
    return kInputs[i].name;
}

TxInput txInputFromIndex(int index) {
    // OUT OF RANGE LANDS ON THE TONE, not on the microphone. A hand-edited
    // config with a number this build does not know must not point a
    // transmitter at a room.
    if (index < 0 || index >= kTxInputCount) { return TxInput::Tone; }
    return static_cast<TxInput>(index);
}

bool txInputFromName(const char* name, TxInput& out) {
    if (name == nullptr) { return false; }
    for (const InputRow& row : kInputs) {
        if (std::strcmp(row.name, name) == 0) {
            out = row.input;
            return true;
        }
    }
    return false;
}

// --- the rate matcher --------------------------------------------------------

void TxInterpolator::configure(double audioRateHz, double sinkRateHz) {
    if (!(audioRateHz > 0.0) || !(sinkRateHz > 0.0)) { return; }
    audioRateHz_ = audioRateHz;
    sinkRateHz_ = sinkRateHz;
    // A sink slower than the audio is not a transmit path this product has,
    // and pretending otherwise would decimate the modulation without an
    // anti-alias filter. Stage 1 becomes a pass-through and stage 2 does
    // whatever it can; the caller is the one that knows a rate like that is
    // a mistake.
    const double ratio = sinkRateHz_ / audioRateHz_;
    interp_ = static_cast<unsigned>(std::max(1.0, std::floor(ratio + 0.5)));
    upI_.reset(new dsp::RationalResampler(interp_, 1));
    upQ_.reset(new dsp::RationalResampler(interp_, 1));
    // Input samples of stage 1's output per output sample of stage 2. Within
    // about half a percent of 1 by construction, because interp_ is the
    // ROUNDED ratio.
    step_ = (audioRateHz_ * static_cast<double>(interp_)) / sinkRateHz_;
    reset();
}

void TxInterpolator::reset() {
    if (upI_) { upI_->reset(); }
    if (upQ_) { upQ_->reset(); }
    phase_ = 0.0;
    last_ = std::complex<float>(0.0f, 0.0f);
    primed_ = false;
}

std::size_t TxInterpolator::maxOut(std::size_t nIn) const {
    const std::size_t stage1 = nIn * static_cast<std::size_t>(interp_) + 1;
    if (!(step_ > 0.0)) { return stage1; }
    // One spare beyond the arithmetic: the fractional phase can be anywhere
    // inside a sample when a block starts, so one more output than the
    // average can fall inside it.
    return static_cast<std::size_t>(static_cast<double>(stage1) / step_) + 2;
}

std::size_t TxInterpolator::process(const std::complex<float>* in, std::size_t nIn,
                                    std::complex<float>* out, std::size_t outCap) {
    if (in == nullptr || out == nullptr || nIn == 0 || outCap == 0) { return 0; }
    if (!upI_ || !upQ_) { configure(audioRateHz_, sinkRateHz_); }

    inI_.resize(nIn);
    inQ_.resize(nIn);
    for (std::size_t i = 0; i < nIn; ++i) {
        inI_[i] = in[i].real();
        inQ_[i] = in[i].imag();
    }
    const std::size_t cap = upI_->maxOut(nIn);
    midI_.resize(cap);
    midQ_.resize(cap);
    const std::size_t m = upI_->process(inI_.data(), nIn, midI_.data(), cap);
    const std::size_t mq = upQ_->process(inQ_.data(), nIn, midQ_.data(), cap);
    // The two arms are the same filter fed the same number of samples, so
    // they produce the same count. Taking the smaller costs nothing and
    // means a future change to one of them cannot slide I against Q, which
    // is the entire content of a sideband.
    const std::size_t have = std::min(m, mq);

    std::size_t made = 0;
    for (std::size_t k = 0; k < have && made < outCap; ++k) {
        const std::complex<float> s(midI_[k], midQ_[k]);
        if (!primed_) {
            last_ = s;
            primed_ = true;
            continue;
        }
        while (phase_ < 1.0 && made < outCap) {
            const float t = static_cast<float>(phase_);
            out[made++] = std::complex<float>(last_.real() + (s.real() - last_.real()) * t,
                                              last_.imag() + (s.imag() - last_.imag()) * t);
            phase_ += step_;
        }
        phase_ -= 1.0;
        last_ = s;
    }
    return made;
}

// --- the transmitter ---------------------------------------------------------

Transmitter::Transmitter() {
    tone_.setSampleRateHz(48000.0);
    tone_.setFrequencyHz(dsp::kToneDefaultHz);
    tone_.setLevel(dsp::kToneDefaultLevel);
    modulator_.setSampleRateHz(48000.0);
}

Transmitter::~Transmitter() { stop(); }

void Transmitter::setSink(std::unique_ptr<source::IqSink> sink) {
    // UNKEYED FIRST, ALWAYS. Swapping a radio out from under a live
    // transmission would leave the old one keyed with nothing feeding it and
    // the new one inheriting a key it was never given.
    stop();
    std::lock_guard<std::mutex> lk(stateMutex_);
    sink_ = std::move(sink);
}

bool Transmitter::haveSink() const {
    std::lock_guard<std::mutex> lk(stateMutex_);
    return sink_ != nullptr;
}

source::IqSink* Transmitter::sink() {
    std::lock_guard<std::mutex> lk(stateMutex_);
    return sink_.get();
}

const source::IqSink* Transmitter::sink() const {
    std::lock_guard<std::mutex> lk(stateMutex_);
    return sink_.get();
}

void Transmitter::setMode(dsp::TxMode m) {
    std::lock_guard<std::mutex> lk(stateMutex_);
    modulator_.setMode(m);
}

dsp::TxMode Transmitter::mode() const {
    std::lock_guard<std::mutex> lk(stateMutex_);
    return modulator_.mode();
}

void Transmitter::setInput(TxInput in) {
    std::lock_guard<std::mutex> lk(stateMutex_);
    if (in == input_) { return; }
    input_ = in;
    // Whatever the microphone had queued belongs to the moment before the
    // operator chose it. drain() again at key-down catches the rest.
    audioIn_.drain();
}

TxInput Transmitter::input() const {
    std::lock_guard<std::mutex> lk(stateMutex_);
    return input_;
}

void Transmitter::setPowerDb(double db) {
    std::lock_guard<std::mutex> lk(stateMutex_);
    if (sink_ != nullptr) { sink_->setGainDb(db); }
}

double Transmitter::powerDb() const {
    std::lock_guard<std::mutex> lk(stateMutex_);
    return sink_ != nullptr ? sink_->gainDb() : 0.0;
}

bool Transmitter::setFrequencyHz(double hz) {
    std::lock_guard<std::mutex> lk(stateMutex_);
    if (sink_ == nullptr) { return false; }
    if (sink_->setCenterFrequencyHz(hz)) { return true; }
    setError(sink_->lastError());
    return false;
}

double Transmitter::frequencyHz() const {
    std::lock_guard<std::mutex> lk(stateMutex_);
    return sink_ != nullptr ? sink_->centerFrequencyHz() : 0.0;
}

void Transmitter::setToneHz(double hz) {
    std::lock_guard<std::mutex> lk(stateMutex_);
    tone_.setFrequencyHz(hz);
}

double Transmitter::toneHz() const {
    std::lock_guard<std::mutex> lk(stateMutex_);
    return tone_.frequencyHz();
}

// --- the key -----------------------------------------------------------------

void Transmitter::setPttHeld(bool held) { pttHeld_.store(held, std::memory_order_relaxed); }

bool Transmitter::pttHeld() const { return pttHeld_.load(std::memory_order_relaxed); }

void Transmitter::setLatched(bool on) {
    if (on == latched_.load(std::memory_order_relaxed)) { return; }
    latched_.store(on, std::memory_order_relaxed);
    if (on) {
        std::lock_guard<std::mutex> lk(stateMutex_);
        latchedAt_ = std::chrono::steady_clock::now();
    }
}

bool Transmitter::latched() const { return latched_.load(std::memory_order_relaxed); }

void Transmitter::keyRemote() {
    // THE STAMP IS WRITTEN FIRST, and unconditionally. Everything below is
    // logging; the only thing that keeps the key closed is this timestamp
    // being recent, so a re-assertion must extend the hold even if every
    // other line here were removed.
    remoteKeyedAtMs_.store(nowMs(), std::memory_order_relaxed);
    if (!remoteKeyed_.exchange(true, std::memory_order_relaxed)) {
        diagLogf("tx: keyed by the web remote");
    }
}

void Transmitter::releaseRemote(const char* why) {
    if (!remoteKeyed_.exchange(false, std::memory_order_relaxed)) { return; }
    diagLogf("tx: remote key released (%s)", why != nullptr ? why : "no reason given");
}

bool Transmitter::remoteKeyed() const { return remoteKeyed_.load(std::memory_order_relaxed); }

std::int64_t Transmitter::remoteHoldRemainingMs() const {
    if (!remoteKeyed_.load(std::memory_order_relaxed)) { return 0; }
    const std::int64_t left = kRemotePttHoldMs.count() -
                              (nowMs() - remoteKeyedAtMs_.load(std::memory_order_relaxed));
    return left > 0 ? left : 0;
}

void Transmitter::setLatchTimeoutForTest(std::chrono::milliseconds t) {
    std::lock_guard<std::mutex> lk(stateMutex_);
    latchTimeout_ = t;
}

bool Transmitter::transmitting() const {
    return transmitting_.load(std::memory_order_relaxed);
}

std::uint64_t Transmitter::blocksSent() const { return blocks_.load(std::memory_order_relaxed); }
std::uint64_t Transmitter::blocksShort() const {
    return shortBlocks_.load(std::memory_order_relaxed);
}

float Transmitter::takeInputPeak() { return audioIn_.takePeak(); }

void Transmitter::setError(std::string msg) {
    std::lock_guard<std::mutex> lk(errorMutex_);
    lastError_ = std::move(msg);
}

std::string Transmitter::lastError() const {
    std::lock_guard<std::mutex> lk(errorMutex_);
    return lastError_;
}

std::string Transmitter::lastAutoUnkeyReason() const {
    std::lock_guard<std::mutex> lk(errorMutex_);
    return autoUnkeyReason_;
}

void Transmitter::tick() {
    // THE LIVENESS STAMP, and it is the first thing this function does. Every
    // other decision below can be skipped without a radio being left keyed;
    // this one cannot, because it is what the TX thread watches.
    lastTickMs_.store(nowMs(), std::memory_order_relaxed);

    // THE LATCH'S OWN DEADLINE.
    if (latched_.load(std::memory_order_relaxed)) {
        bool expired = false;
        long long limitMs = 0;
        {
            std::lock_guard<std::mutex> lk(stateMutex_);
            expired = (std::chrono::steady_clock::now() - latchedAt_) >= latchTimeout_;
            limitMs = static_cast<long long>(latchTimeout_.count());
        }
        if (expired) {
            latched_.store(false, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lk(errorMutex_);
                char note[96];
                // In seconds where the product's own minute lives, in
                // milliseconds where a test has shortened it - "after 0
                // seconds" is a sentence nobody can act on.
                if (limitMs >= 1000) {
                    std::snprintf(note, sizeof(note),
                                  "the latch released itself after %lld seconds",
                                  limitMs / 1000);
                } else {
                    std::snprintf(note, sizeof(note),
                                  "the latch released itself after %lld ms", limitMs);
                }
                autoUnkeyReason_ = note;
            }
            diagWarnf("tx: the transmit latch released itself after %lld ms", limitMs);
        }
    }

    // THE REMOTE KEY'S OWN DEADLINE, and it is the same shape as the latch's
    // above for the same reason: the thing holding this key is at the far end
    // of a network and may simply stop existing. One assertion is worth
    // kRemotePttHoldMs and no more.
    if (remoteKeyed_.load(std::memory_order_relaxed)) {
        const std::int64_t age =
            nowMs() - remoteKeyedAtMs_.load(std::memory_order_relaxed);
        if (age >= kRemotePttHoldMs.count()) {
            remoteKeyed_.store(false, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lk(errorMutex_);
                autoUnkeyReason_ = "the web remote stopped asking, so the key was released";
            }
            diagWarnf("tx: remote key released (the hold expired after %lld ms)",
                      static_cast<long long>(age));
        }
    }

    const bool want = pttHeld_.load(std::memory_order_relaxed) ||
                      latched_.load(std::memory_order_relaxed) ||
                      remoteKeyed_.load(std::memory_order_relaxed);
    const bool have = transmitting_.load(std::memory_order_relaxed);

    if (want && !have) {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (!keyDownLocked()) {
            // A key that could not be honoured must not leave a latch closed
            // and a panel lamp lit, so the request is dropped as well - and
            // the remote's with it, or a browser that asked once would have
            // the refusal retried on every frame for two seconds.
            latched_.store(false, std::memory_order_relaxed);
            pttHeld_.store(false, std::memory_order_relaxed);
            releaseRemote("the radio would not key");
        }
        return;
    }
    if (!want && have) {
        stopThread();
        std::lock_guard<std::mutex> lk(stateMutex_);
        keyUpLocked(nullptr);
        return;
    }
    if (have && !run_.load(std::memory_order_relaxed)) {
        // The TX thread let go on its own - a fault, or the dead-man's
        // handle. It has already silenced the radio; this is the bookkeeping
        // and the panel catching up with it.
        stopThread();
        std::lock_guard<std::mutex> lk(stateMutex_);
        keyUpLocked("the transmitter stopped on its own");
        latched_.store(false, std::memory_order_relaxed);
        pttHeld_.store(false, std::memory_order_relaxed);
        // AND THE REMOTE'S, which is the one an operator at the far end
        // cannot see has happened. A fault or the dead-man's handle must not
        // leave a browser re-asserting into a transmitter that has already
        // let go; the next assertion then keys a radio that just faulted.
        releaseRemote("the transmitter stopped on its own");
    }
}

bool Transmitter::keyDownLocked() {
    if (sink_ == nullptr) {
        setError("there is no transmitter open");
        return false;
    }
    if (sink_->faulted()) {
        setError(sink_->lastError());
        return false;
    }
    if (!sink_->start()) {
        setError(sink_->lastError());
        diagWarnf("tx: refused to key - %s", sink_->lastError());
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(errorMutex_);
        autoUnkeyReason_.clear();
    }
    // EVERY HISTORY CLEARED AT KEY-DOWN. A modulator carrying the tail of the
    // last transmission, a microphone ring holding what was said before the
    // key went down, an interpolator half way through somebody else's word -
    // each of them would be transmitted, and the last one is the one nobody
    // would forgive.
    modulator_.reset();
    modulator_.setSampleRateHz(48000.0);
    tone_.setSampleRateHz(48000.0);
    audioIn_.drain();
    interp_.configure(48000.0, sink_->sampleRateHz());
    modulator_.setKeyed(true);
    blocks_.store(0, std::memory_order_relaxed);
    shortBlocks_.store(0, std::memory_order_relaxed);
    lastTickMs_.store(nowMs(), std::memory_order_relaxed);

    transmitting_.store(true, std::memory_order_relaxed);
    startThread();
    // The mode and the rate, never the frequency: what somebody transmits on
    // is as private as what they listen to (PRIVACY.md).
    diagLogf("tx: keyed - %s, %.0f S/s", dsp::txModeName(modulator_.mode()),
             sink_->sampleRateHz());
    return true;
}

void Transmitter::keyUpLocked(const char* reason) {
    transmitting_.store(false, std::memory_order_relaxed);
    modulator_.setKeyed(false);
    if (sink_ != nullptr) {
        // Idempotent, and usually a no-op: the TX thread silences the radio
        // on its way out. This is the second attempt, for the case where the
        // thread had to be abandoned.
        sink_->stop();
    }
    if (reason != nullptr) {
        // DOES NOT OVERWRITE A REASON ALREADY GIVEN. When the TX thread lets
        // go on its own it has already recorded WHY - the radio faulted, or
        // the window stopped responding - and tick() then calls this with its
        // own generic sentence. Writing that over the specific one replaces
        // the cause with the observation, which is how both of those turned
        // into "the transmitter stopped on its own" on the panel and in
        // tests/test_transmitter.cpp before this guard.
        std::lock_guard<std::mutex> lk(errorMutex_);
        if (autoUnkeyReason_.empty()) { autoUnkeyReason_ = reason; }
    }
    diagLogf("tx: unkeyed after %llu block(s)%s%s",
             static_cast<unsigned long long>(blocks_.load(std::memory_order_relaxed)),
             reason != nullptr ? " - " : "", reason != nullptr ? reason : "");
}

void Transmitter::startThread() {
    {
        std::lock_guard<std::mutex> lk(waitMutex_);
        exited_ = false;
    }
    run_.store(true, std::memory_order_relaxed);
    thread_ = std::thread(&Transmitter::threadBody, this);
}

void Transmitter::stopThread() {
    run_.store(false, std::memory_order_relaxed);
    waitCv_.notify_all();
    if (!thread_.joinable()) { return; }
    // A BOUNDED JOIN, and stateMutex_ IS NOT HELD ACROSS IT. The TX thread's
    // last act takes that mutex to silence the radio, so a join underneath it
    // would be a deadlock - and a deadlock here is a keyed transmitter and a
    // window that will not close.
    bool exited = false;
    {
        std::unique_lock<std::mutex> lk(waitMutex_);
        exited = waitCv_.wait_for(lk, kThreadJoinWait, [this] { return exited_; });
    }
    if (!exited) {
        // NOT ABANDONED, AND THAT IS DELIBERATE - this is the one bounded
        // wait in the product that is followed by a real join, so it is worth
        // saying why it is safe here when it is not safe for a driver's
        // reader thread. Everything the TX thread can be inside is ALREADY
        // bounded: one sink write (IqSink::kWriteWait on the Pluto) or the
        // sink's own stop (PlutoTx::kWriterJoinWait), both of which return on
        // their own. So the thread always comes back; this wait is what turns
        // "it took longer than it should have" into a line in the log rather
        // than into silence, and the join below then costs whatever is left
        // of the sink's own bound. The shutdown budget charges both numbers,
        // which is an upper bound on the pair and not their sum in practice.
        diagWarnf("tx: the transmit thread did not return within %lld ms; waiting for the "
                  "radio's own bound",
                  static_cast<long long>(kThreadJoinWait.count()));
    }
    thread_.join();
}

void Transmitter::stop() {
    const bool was = transmitting_.load(std::memory_order_relaxed);
    pttHeld_.store(false, std::memory_order_relaxed);
    latched_.store(false, std::memory_order_relaxed);
    // setSink() calls this, so swapping or removing a radio opens the remote
    // key too - a browser's assertion must not be inherited by the next board
    // any more than a latch is.
    releaseRemote("the transmitter was shut down");
    stopThread();
    std::lock_guard<std::mutex> lk(stateMutex_);
    if (was) { keyUpLocked("the transmitter was shut down"); }
    transmitting_.store(false, std::memory_order_relaxed);
    if (sink_ != nullptr) { sink_->stop(); }
}

void Transmitter::threadBody() {
    constexpr double kAudioRate = 48000.0;
    std::vector<float> audio(kAudioBlock);
    std::vector<std::complex<float>> base(kAudioBlock);
    std::vector<std::complex<float>> rf;

    const auto blockPeriod = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(static_cast<double>(kAudioBlock) / kAudioRate));
    auto next = std::chrono::steady_clock::now();

    while (run_.load(std::memory_order_relaxed)) {
        next += blockPeriod;

        // --- THE DEAD-MAN'S HANDLE ------------------------------------------
        // The GUI thread stamps lastTickMs_ every frame. If it has stopped,
        // the thing that would normally release the key has stopped too, so
        // this thread releases it instead. See the hard rule in the header.
        const std::int64_t age = nowMs() - lastTickMs_.load(std::memory_order_relaxed);
        if (age > kKeyAliveWait.count()) {
            {
                std::lock_guard<std::mutex> lk(errorMutex_);
                autoUnkeyReason_ = "the window stopped responding, so the key was released";
            }
            diagWarnf("tx: no frame in %lld ms - releasing the key", static_cast<long long>(age));
            break;
        }

        bool tailDone = false;
        std::size_t made = 0;
        {
            std::lock_guard<std::mutex> lk(stateMutex_);
            if (sink_ == nullptr) { break; }

            // --- the audio --------------------------------------------------
            if (input_ == TxInput::Tone) {
                tone_.generate(audio.data(), kAudioBlock);
            } else {
                const std::size_t got = audioIn_.read(audio.data(), kAudioBlock);
                if (got < kAudioBlock) {
                    // Padded rather than waited for. A microphone that is
                    // momentarily behind should cost a few milliseconds of
                    // silence on the air, not a thread that stops noticing
                    // the key has been released.
                    std::fill(audio.begin() + static_cast<std::ptrdiff_t>(got), audio.end(),
                              0.0f);
                }
            }

            modulator_.process(audio.data(), kAudioBlock, base.data());
            // THE TAIL. Key-up does not stop this thread: it lowers the
            // modulator's envelope, and the thread keeps feeding the radio
            // until the ramp has reached zero. Cutting at the key instead
            // would put the step back that the ramp exists to remove.
            tailDone = modulator_.idle();

            rf.resize(interp_.maxOut(kAudioBlock));
            made = interp_.process(base.data(), kAudioBlock, rf.data(), rf.size());
            if (made > 0) {
                const std::size_t took = sink_->write(rf.data(), made);
                blocks_.fetch_add(1, std::memory_order_relaxed);
                if (took < made) { shortBlocks_.fetch_add(1, std::memory_order_relaxed); }
            }
            if (sink_->faulted()) {
                setError(sink_->lastError());
                {
                    std::lock_guard<std::mutex> elk(errorMutex_);
                    autoUnkeyReason_ = "the radio faulted, so the key was released";
                }
                diagWarnf("tx: fault - %s", sink_->lastError());
                break;
            }
        }

        if (tailDone) { break; }

        std::unique_lock<std::mutex> lk(waitMutex_);
        waitCv_.wait_until(lk, next, [this] { return !run_.load(std::memory_order_relaxed); });
    }

    // THE LAST ACT: SILENCE THE RADIO, from this thread. Not from whoever
    // joins us - the whole point of the dead-man's handle is that there may
    // be nobody left to join us.
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (sink_ != nullptr) { sink_->stop(); }
    }
    // transmitting_ IS NOT CLEARED HERE, and the first draft of this file
    // cleared it - which quietly broke the recovery path. tick() notices a
    // thread that let go on its own by seeing transmitting_ still set while
    // run_ has gone; a thread that cleared the flag itself would look exactly
    // like a transmitter that had never been keyed, so nothing would ever
    // join it and the panel would never say why the key opened.

    {
        std::lock_guard<std::mutex> lk(waitMutex_);
        run_.store(false, std::memory_order_relaxed);
        exited_ = true;
    }
    waitCv_.notify_all();
}

}  // namespace cascade::core
