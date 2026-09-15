// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "core/plugin_runner.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/diag_log.hpp"

namespace cascade::core {
namespace {

// Big enough that a decoder emitting a burst of lines empties in one poll,
// small enough to sit on the DSP thread's working set. The ABI guarantees the
// host passes at least 256.
constexpr std::size_t kPollBufBytes = 8192;

std::string rateSentence(const std::string& name, double want, double have,
                         const char* stream) {
    char buf[320];
    std::snprintf(buf, sizeof buf,
                  "\"%s\" needs %.0f Hz %s and the receiver is producing %.0f Hz, so it "
                  "is not being fed.",
                  name.c_str(), want, stream, have);
    return std::string(buf);
}

std::string rateWord(double hz) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f Hz", hz);
    return std::string(buf);
}

// The Running sentence for a decoder that is fed through a resampler. It
// names both rates, because "is decoding the tuned audio" would hide the one
// fact that separates this instance from a plain one when a decode goes
// wrong: the samples it sees are not the samples the pipeline made.
std::string resampleSentence(const std::string& name, double have, double want) {
    return "\"" + name + "\" is decoding the tuned audio, resampled from " + rateWord(have) +
           " to " + rateWord(want) + ".";
}

// Builds the resampler from the pipeline's audio rate to the decoder's.
// Rates are integers by the ABI (Hz), so the ratio is exact and the
// resampler reduces it by the gcd itself: 48000 -> 22050 becomes 147/320.
// The scratch buffer is sized for a generous first block up front, so the
// audio thread does not allocate on its first call either.
void setUpResample(PluginRunner::AudioResample& r, double haveHz, double wantHz) {
    r.rateHz = wantHz;
    r.resampler = std::make_unique<cascade::dsp::RationalResampler>(
        static_cast<unsigned>(wantHz), static_cast<unsigned>(haveHz));
    r.out.resize(r.resampler->maxOut(8192));
}

// Resamples one pipeline block into r.out and returns how many samples are
// there. Grows the buffer only when a block larger than any before arrives.
std::size_t resampleBlock(PluginRunner::AudioResample& r, const float* in, std::size_t n) {
    const std::size_t need = r.resampler->maxOut(n);
    if (r.out.size() < need) { r.out.resize(need); }
    return r.resampler->process(in, n, r.out.data(), r.out.size());
}

// Said in the same voice as the create() failure above, and deliberately
// says what the user can do about it: the two things that build a fresh
// instance are stopping and starting the module, and scanning again.
std::string failureSentence(const std::string& name) {
    return "\"" + name +
           "\" has failed and stopped decoding (it reported a permanent error), so it "
           "is being fed nothing further. Stop and start it, or scan again, to try it "
           "once more.";
}

}  // namespace

PluginRunner::~PluginRunner() { clear(); }

void PluginRunner::setStopped(std::vector<std::string> keys) {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_.set(std::move(keys));
}

bool PluginRunner::isStopped(const std::string& pluginKey) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopped_.contains(pluginKey);
}

void PluginRunner::rebuild(const std::vector<LoadedPlugin>& plugins, double audioRateHz,
                           double iqRateHz, double centreHz) {
    std::lock_guard<std::mutex> lock(mutex_);
    destroyLocked();
    audioRateHz_ = audioRateHz;
    iqRateHz_ = iqRateHz;
    centreHz_ = centreHz;
    pollBuf_.resize(kPollBufBytes);

    for (const LoadedPlugin& lp : plugins) {
        if (!lp.loaded) { continue; }
        // STOPPED BY THE USER: no instance, and therefore no samples, no text
        // and no picture. Skipped BEFORE any create() and before any status
        // line, because a stopped plugin is not idle for a reason the user
        // needs explaining - the row they stopped it from says so already, and
        // an orange "not being fed" warning for a deliberate choice would
        // train them to ignore the ones that matter.
        if (stopped_.contains(lp)) { continue; }

        // STAMPED ON EVERY LINE THIS PLUGIN PRODUCES, at the end of the loop
        // body rather than at each of the eight places a DecoderStatus is
        // built. One assignment cannot be forgotten by the ninth; eight can,
        // and the one that was forgotten would be a plugin isFeeding could
        // never see.
        const std::string key = pluginKey(lp);
        const std::size_t statusBegin = status_.size();

        // A plugin may declare both. Each declared capability gets its own
        // instance and its own stream, which is why this is two independent
        // blocks rather than an else-if.
        bool started = false;

        // THE HANDLE CASCADE_CAP_AUDIO_OUT RIDES ON. That table has no
        // create() of its own - the sound a decoder makes is the same object
        // as the decode - so it borrows the first instance this plugin
        // produces, in the fixed order the ABI states: decoder, then I/Q
        // decoder, then image decoder. Taken here, once, rather than at three
        // call sites, so the order cannot drift from what the header promises.
        void* audioHandle = nullptr;

        if (lp.decoder != nullptr) {
            // requiredRateHz == 0 means "any rate"; anything else is the rate
            // the decoder is BUILT around, and the host resamples the pipeline's
            // audio to it (plugin_abi.h promises exactly that). Feeding a
            // 48 kHz decoder 44.1 kHz audio and hoping is how a bit clock
            // drifts; feeding it 44.1 kHz audio resampled to 48 kHz is how a
            // decoder written for one clock runs on any receiver.
            const double want = static_cast<double>(lp.decoder->requiredRateHz);
            const bool resample = want != 0.0 && want != audioRateHz;
            {
                void* h = lp.decoder->create(want != 0.0 ? static_cast<uint32_t>(want)
                                                         : static_cast<uint32_t>(audioRateHz));
                if (h == nullptr) {
                    DecoderStatus st;
                    st.plugin = lp.name;
                    st.reason = DecoderIdleReason::CreateFailed;
                    st.stream = DecoderStream::Audio;
                    st.detail =
                        "\"" + lp.name + "\" failed to start (its create() returned nothing).";
                    status_.push_back(std::move(st));
                } else {
                    Instance inst;
                    inst.api = lp.decoder;
                    inst.handle = h;
                    inst.name = lp.name;
                    if (resample) { setUpResample(inst.resample, audioRateHz, want); }
                    // The row pushed immediately below, taken BEFORE the push
                    // so the instance can rewrite its own reason later if the
                    // decoder gives up mid-run.
                    inst.statusIndex = status_.size();
                    if (audioHandle == nullptr) { audioHandle = h; }
                    instances_.push_back(std::move(inst));
                    DecoderStatus st;
                    st.plugin = lp.name;
                    st.reason = DecoderIdleReason::Running;
                    st.stream = DecoderStream::Audio;
                    st.detail = resample ? resampleSentence(lp.name, audioRateHz, want)
                                         : "\"" + lp.name + "\" is decoding the tuned audio.";
                    status_.push_back(std::move(st));
                    started = true;
                }
            }
        }

        if (lp.iqDecoder != nullptr) {
            const double want = lp.iqDecoder->requiredRateHz;
            if (want != 0.0 && want != iqRateHz) {
                DecoderStatus st;
                st.plugin = lp.name;
                st.reason = DecoderIdleReason::RateMismatch;
                st.stream = DecoderStream::Iq;
                st.wantRateHz = want;
                st.detail = rateSentence(lp.name, want, iqRateHz, "raw I/Q");
                status_.push_back(std::move(st));
            } else {
                void* h = lp.iqDecoder->create(want != 0.0 ? want : iqRateHz, centreHz);
                if (h == nullptr) {
                    DecoderStatus st;
                    st.plugin = lp.name;
                    st.reason = DecoderIdleReason::CreateFailed;
                    st.stream = DecoderStream::Iq;
                    st.detail =
                        "\"" + lp.name + "\" failed to start (its create() returned nothing).";
                    status_.push_back(std::move(st));
                } else {
                    IqInstance inst;
                    inst.api = lp.iqDecoder;
                    inst.handle = h;
                    inst.name = lp.name;
                    inst.statusIndex = status_.size();
                    if (audioHandle == nullptr) { audioHandle = h; }
                    iqInstances_.push_back(std::move(inst));
                    DecoderStatus st;
                    st.plugin = lp.name;
                    st.reason = DecoderIdleReason::Running;
                    st.stream = DecoderStream::Iq;
                    st.detail = "\"" + lp.name +
                                "\" is decoding the raw receiver band (it ignores the VFO).";
                    status_.push_back(std::move(st));
                    started = true;
                }
            }
        }

        if (lp.imageDecoder != nullptr) {
            // WHICH STREAM is the plugin's own declaration, and it decides
            // everything else here: an SSTV decoder asks for demodulated audio
            // and an LRPT decoder for complex baseband, and the rate it is
            // matched against has to be the rate of the stream it asked for.
            const bool isIq = lp.imageDecoder->inputKind == CASCADE_INPUT_IQ;
            const double have = isIq ? iqRateHz : audioRateHz;
            const double want = lp.imageDecoder->requiredRateHz;
            // Audio is resampled to what the decoder asked for, as for the
            // text decoders above; raw I/Q is not - the band is what the
            // device produces, and only the device can change it.
            const bool resample = !isIq && want != 0.0 && want != have;
            DecoderStatus st;
            st.plugin = lp.name;
            st.output = DecoderOutput::Image;
            st.stream = isIq ? DecoderStream::Iq : DecoderStream::Audio;
            if (isIq && want != 0.0 && want != have) {
                st.reason = DecoderIdleReason::RateMismatch;
                st.wantRateHz = want;
                st.detail = rateSentence(lp.name, want, have, isIq ? "raw I/Q" : "audio");
                status_.push_back(std::move(st));
            } else {
                // centerHz is 0 for an audio-input decoder, per the ABI: the
                // audio it receives has already been tuned and demodulated, so
                // an RF frequency would be a number it could only misuse.
                void* h = lp.imageDecoder->create(want != 0.0 ? want : have,
                                                  isIq ? centreHz : 0.0);
                if (h == nullptr) {
                    st.reason = DecoderIdleReason::CreateFailed;
                    st.detail =
                        "\"" + lp.name + "\" failed to start (its create() returned nothing).";
                    status_.push_back(std::move(st));
                } else {
                    ImageInstance inst;
                    inst.api = lp.imageDecoder;
                    inst.handle = h;
                    inst.name = lp.name;
                    inst.inputKind = isIq ? CASCADE_INPUT_IQ : CASCADE_INPUT_AUDIO;
                    if (resample) { setUpResample(inst.resample, have, want); }
                    inst.statusIndex = status_.size();
                    if (audioHandle == nullptr) { audioHandle = h; }
                    imageInstances_.push_back(std::move(inst));
                    if (isIq) { ++iqImageCount_; } else { ++audioImageCount_; }
                    st.reason = DecoderIdleReason::Running;
                    st.detail = "\"" + lp.name + "\" is building an image from the " +
                                (isIq ? std::string("raw receiver band (it ignores the VFO).")
                                      : resample ? "tuned audio, resampled from " +
                                                       rateWord(have) + " to " +
                                                       rateWord(want) + "."
                                                 : std::string("tuned audio."));
                    status_.push_back(std::move(st));
                    started = true;
                }
            }
        }

        // THE SPEAKERS. Registered last, because it needs an instance to ride
        // on and the three blocks above are what produce one. A plugin that
        // declares the capability but got no instance (its create() failed,
        // or its decoder wants a rate this receiver is not producing) is
        // simply not registered: it has nothing to play from, and the status
        // row its decoder already pushed says why in the words the user needs.
        if (lp.audioOut != nullptr && audioHandle != nullptr) {
            AudioInstance a;
            a.api = lp.audioOut;
            a.handle = audioHandle;
            a.name = lp.name;
            a.key = key;
            a.rateHz = lp.audioOut->sampleRateHz;
            a.channels = lp.audioOut->channels;
            // The plugin's rate is validated non-zero and in range at load
            // time, so this ratio is always meaningful. Built once here, not
            // on the first block: the audio thread must not construct a
            // resampler (it allocates, and it computes a filter).
            if (static_cast<double>(a.rateHz) != audioRateHz && audioRateHz > 0.0) {
                a.rsL = std::make_unique<cascade::dsp::RationalResampler>(
                    static_cast<unsigned>(audioRateHz), a.rateHz);
                a.rsR = std::make_unique<cascade::dsp::RationalResampler>(
                    static_cast<unsigned>(audioRateHz), a.rateHz);
            }
            // Reserved, never resized down: every append below stays inside
            // this capacity, so the steady-state path allocates nothing.
            a.fifoL.reserve(kAudioFifoFrames);
            a.fifoR.reserve(kAudioFifoFrames);
            a.pullBuf.reserve(kAudioFifoFrames * 2u);
            a.inL.reserve(kAudioFifoFrames);
            a.inR.reserve(kAudioFifoFrames);
            audioInstances_.push_back(std::move(a));
        }

        // No table this runner drives: say so rather than leaving the plugin
        // looking loaded-and-working.
        if (!started && lp.decoder == nullptr && lp.iqDecoder == nullptr &&
            lp.imageDecoder == nullptr) {
            DecoderStatus st;
            st.plugin = lp.name;
            st.reason = DecoderIdleReason::NoAudioTable;
            st.stream = DecoderStream::None;
            st.detail = "\"" + lp.name + "\" provides no decoder this build can drive.";
            status_.push_back(std::move(st));
        }

        for (std::size_t i = statusBegin; i < status_.size(); ++i) {
            status_[i].key = key;
        }
    }
}

bool PluginRunner::isFeeding(const std::string& pluginKey) const {
    // An empty key matches nothing, the same rule PluginStopSet applies: a
    // record with no path yields "", and one path-less plugin must not answer
    // for every other.
    if (pluginKey.empty()) { return false; }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const DecoderStatus& s : status_) {
        if (s.key == pluginKey && s.reason == DecoderIdleReason::Running) { return true; }
    }
    return false;
}

void PluginRunner::processIq(const float* interleaved, std::size_t frames) {
    if (interleaved == nullptr || frames == 0) { return; }
    std::lock_guard<std::mutex> lock(mutex_);
    if (iqInstances_.empty() && iqImageCount_ == 0) { return; }
    // Same two rules as processAudio: a permanently failed instance is fed
    // nothing, and the count only moves when something was actually fed.
    bool fedAny = false;
    for (IqInstance& i : iqInstances_) {
        if (i.failed) { continue; }
        i.api->process(i.handle, interleaved, frames);
        fedAny = true;
    }
    for (ImageInstance& i : imageInstances_) {
        if (i.failed || i.inputKind != CASCADE_INPUT_IQ) { continue; }
        i.api->process(i.handle, interleaved, frames);
        fedAny = true;
    }
    if (fedAny) { iqFed_ += frames; }
    pollIqLocked();
    pollImageTextLocked(CASCADE_INPUT_IQ);
}

void PluginRunner::retune(double centreHz) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (centreHz == centreHz_) { return; }
    centreHz_ = centreHz;
    for (IqInstance& i : iqInstances_) {
        // retune is OPTIONAL in the ABI - a decoder working on the envelope
        // usually does not care - so it is null-checked at the call site,
        // which is the contract the header states.
        if (i.api->retune != nullptr) { i.api->retune(i.handle, centreHz); }
    }
    for (ImageInstance& i : imageInstances_) {
        // Only the I/Q ones. An audio-input image decoder was created with
        // centerHz 0 by the ABI's own rule, so handing it a real RF frequency
        // now would contradict what it was told at create().
        if (i.inputKind == CASCADE_INPUT_IQ && i.api->retune != nullptr) {
            i.api->retune(i.handle, centreHz);
        }
    }
}

void PluginRunner::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    destroyLocked();
}

void PluginRunner::destroyLocked() {
    // FIRST, BEFORE ANY destroy(). An AudioInstance holds no handle of its
    // own: it borrows a decoder instance's. Dropping these after the loops
    // below would leave a borrowed pointer to a destroyed instance in the
    // vector, and the very next block on the audio thread would pull through
    // it. The events and counters go with them - they describe a set of
    // instances that no longer exists.
    audioInstances_.clear();
    playing_ = kNoAudio;
    audioEventCount_ = 0;
    audioGaps_ = 0;
    audioGapFrames_ = 0;
    audioGapsReported_ = 0;
    audioGapFramesReported_ = 0;

    for (Instance& i : instances_) {
        if (i.api != nullptr && i.handle != nullptr) { i.api->destroy(i.handle); }
    }
    instances_.clear();
    for (IqInstance& i : iqInstances_) {
        if (i.api != nullptr && i.handle != nullptr) { i.api->destroy(i.handle); }
    }
    iqInstances_.clear();
    for (ImageInstance& i : imageInstances_) {
        if (i.api != nullptr && i.handle != nullptr) { i.api->destroy(i.handle); }
    }
    imageInstances_.clear();
    audioImageCount_ = 0;
    iqImageCount_ = 0;
    status_.clear();
    audioFed_ = 0;
    iqFed_ = 0;
}

void PluginRunner::processAudio(const float* mono, std::size_t frames) {
    if (mono == nullptr || frames == 0) { return; }
    std::lock_guard<std::mutex> lock(mutex_);
    if (instances_.empty() && audioImageCount_ == 0) { return; }
    // NOTHING IS HANDED TO AN INSTANCE THAT HAS FAILED PERMANENTLY, and the
    // frame count is only raised if something actually took the samples. The
    // count exists to separate "it was never fed" from "it was fed and found
    // nothing", so a number that climbed while every decoder was dead would
    // answer that question with the wrong one of the two.
    bool fedAny = false;
    for (Instance& i : instances_) {
        if (i.failed) { continue; }
        if (i.resample.resampler) {
            const std::size_t n = resampleBlock(i.resample, mono, frames);
            if (n != 0) { i.api->process(i.handle, i.resample.out.data(), n); }
        } else {
            i.api->process(i.handle, mono, frames);
        }
        fedAny = true;
    }
    for (ImageInstance& i : imageInstances_) {
        if (i.failed || i.inputKind != CASCADE_INPUT_AUDIO) { continue; }
        if (i.resample.resampler) {
            const std::size_t n = resampleBlock(i.resample, mono, frames);
            if (n != 0) { i.api->process(i.handle, i.resample.out.data(), n); }
        } else {
            i.api->process(i.handle, mono, frames);
        }
        fedAny = true;
    }
    if (fedAny) { audioFed_ += frames; }
    pollLocked();
    pollImageTextLocked(CASCADE_INPUT_AUDIO);
}

// ---------------------------------------------------------------------------
// The plugin audio path
// ---------------------------------------------------------------------------

void PluginRunner::noteAudioEventLocked(AudioEvent::Kind kind, const AudioInstance& a,
                                        const AudioInstance* holder) {
    // DROPPED RATHER THAN QUEUED WITHOUT BOUND. This array is written by the
    // audio thread and emptied by the GUI thread; a GUI that has stopped
    // draining (minimised, modal) must not make the audio path grow anything.
    // Eight transitions between two drains is already a plugin flapping.
    if (audioEventCount_ >= kMaxAudioEvents) { return; }
    AudioEvent& e = audioEvents_[audioEventCount_++];
    e = AudioEvent{};
    e.kind = kind;
    std::snprintf(e.name, sizeof e.name, "%s", a.name.c_str());
    if (holder != nullptr) {
        std::snprintf(e.holder, sizeof e.holder, "%s", holder->name.c_str());
    }
    e.rateHz = a.rateHz;
    e.channels = a.channels;
    e.sinkRateHz = static_cast<std::uint32_t>(audioRateHz_);
}

void PluginRunner::pullAudioLocked(AudioInstance& a, float* left, float* right,
                                   std::size_t frames) {
    // TOP UP FIRST, THEN SERVE. The plugin's clock is not the sink's, so a
    // resampler hands back a variable number of samples per call while the
    // block below needs an exact count; the FIFO is what absorbs the
    // difference. Only the shortfall is asked for, so the buffer never runs
    // ahead and the plugin's own latency is not added to.
    const std::size_t have = a.fifoL.size() - a.fifoHead;
    if (have < frames && !a.stopped) {
        const std::size_t shortfall = frames - have;
        std::size_t needIn = shortfall;
        if (a.rsL) {
            // Ceiling plus two: a polyphase resampler's output count per block
            // depends on its stream phase, so asking for the exact ratio can
            // come up one sample short and charge a gap for arithmetic.
            needIn = static_cast<std::size_t>(
                         std::ceil(static_cast<double>(shortfall) *
                                   static_cast<double>(a.rateHz) / audioRateHz_)) +
                     2u;
        }
        if (a.pullBuf.size() < needIn * a.channels) { a.pullBuf.resize(needIn * a.channels); }
        const std::int32_t got = a.api->pull(a.handle, a.pullBuf.data(), needIn);
        if (got < 0) {
            // The ABI's "stopped producing for good": never pulled again, and
            // whatever is still in the FIFO is played out below rather than
            // cut off mid-word.
            a.stopped = true;
        } else if (got > 0) {
            std::size_t n = static_cast<std::size_t>(got);
            if (n > needIn) { n = needIn; }  // a plugin that overstated itself
            if (a.inL.size() < n) {
                a.inL.resize(n);
                a.inR.resize(n);
            }
            if (a.channels == 2u) {
                for (std::size_t i = 0; i < n; ++i) {
                    a.inL[i] = a.pullBuf[2 * i];
                    a.inR[i] = a.pullBuf[2 * i + 1];
                }
            } else {
                // MONO GOES TO BOTH EARS, not to one. Halving it into a centre
                // image would make every mono service quieter than every
                // stereo one, which is the sort of difference a user blames on
                // the plugin.
                for (std::size_t i = 0; i < n; ++i) {
                    a.inL[i] = a.pullBuf[i];
                    a.inR[i] = a.pullBuf[i];
                }
            }

            // Compact before appending: the head is consumed audio, and an
            // index that only ever grows would walk the buffer off its
            // reserve. The tail being moved is always less than one block.
            if (a.fifoHead != 0) {
                a.fifoL.erase(a.fifoL.begin(),
                              a.fifoL.begin() + static_cast<std::ptrdiff_t>(a.fifoHead));
                a.fifoR.erase(a.fifoR.begin(),
                              a.fifoR.begin() + static_cast<std::ptrdiff_t>(a.fifoHead));
                a.fifoHead = 0;
            }
            const std::size_t base = a.fifoL.size();
            const std::size_t room = a.rsL ? a.rsL->maxOut(n) : n;
            a.fifoL.resize(base + room);
            a.fifoR.resize(base + room);
            std::size_t produced = n;
            if (a.rsL) {
                const std::size_t kL =
                    a.rsL->process(a.inL.data(), n, a.fifoL.data() + base, room);
                const std::size_t kR =
                    a.rsR->process(a.inR.data(), n, a.fifoR.data() + base, room);
                produced = std::min(kL, kR);
            } else {
                std::memcpy(a.fifoL.data() + base, a.inL.data(), n * sizeof(float));
                std::memcpy(a.fifoR.data() + base, a.inR.data(), n * sizeof(float));
            }
            a.fifoL.resize(base + produced);
            a.fifoR.resize(base + produced);
        }
    }

    const std::size_t avail = a.fifoL.size() - a.fifoHead;
    const std::size_t take = std::min(avail, frames);
    if (take != 0) {
        std::memcpy(left, a.fifoL.data() + a.fifoHead, take * sizeof(float));
        std::memcpy(right, a.fifoR.data() + a.fifoHead, take * sizeof(float));
        a.fifoHead += take;
    }
    if (take < frames) {
        // A GAP, NOT AN END. The plugin is still the thing playing; it simply
        // had nothing this block. Silence for the shortfall and a counter, so
        // "the DAB audio keeps breaking up" is a number somebody can read
        // rather than a description.
        std::fill(left + take, left + frames, 0.0f);
        std::fill(right + take, right + frames, 0.0f);
        ++audioGaps_;
        audioGapFrames_ += frames - take;
    }
    if (a.fifoHead == a.fifoL.size()) {
        a.fifoL.clear();
        a.fifoR.clear();
        a.fifoHead = 0;
    }
}

bool PluginRunner::pullPluginAudio(float* left, float* right, std::size_t frames) {
    if (left == nullptr || right == nullptr || frames == 0) { return false; }
    std::lock_guard<std::mutex> lock(mutex_);
    if (audioInstances_.empty()) { return false; }

    // EVERY INSTANCE IS ASKED EVERY BLOCK, including ones not being pulled
    // from. That is how a second plugin gets the speakers after the first lets
    // go - and active() is defined as a read of state the plugin has already
    // decided, so asking costs nothing.
    std::size_t want = kNoAudio;
    for (std::size_t i = 0; i < audioInstances_.size(); ++i) {
        AudioInstance& a = audioInstances_[i];
        if (a.stopped) { continue; }
        if (a.api->active(a.handle) == 0) {
            a.contentionLogged = false;
            continue;
        }
        if (want == kNoAudio) {
            want = i;
            continue;
        }
        // FIRST WINS, in load order, and the loser is told once. Deciding it
        // by load order rather than by whichever asked most recently is what
        // keeps the speaker from being handed back and forth every block when
        // two decoders are both running.
        if (!a.contentionLogged) {
            a.contentionLogged = true;
            noteAudioEventLocked(AudioEvent::Kind::Contended, a, &audioInstances_[want]);
        }
    }

    if (want != playing_) {
        if (playing_ != kNoAudio) {
            noteAudioEventLocked(AudioEvent::Kind::Stopped, audioInstances_[playing_], nullptr);
        }
        if (want != kNoAudio) {
            // Start from empty: whatever is in the FIFO is from the last time
            // this plugin played, and playing it now would put a fragment of
            // an old programme in front of the new one.
            AudioInstance& a = audioInstances_[want];
            a.fifoL.clear();
            a.fifoR.clear();
            a.fifoHead = 0;
            if (a.rsL) {
                a.rsL->reset();
                a.rsR->reset();
            }
            noteAudioEventLocked(AudioEvent::Kind::Started, a, nullptr);
        }
        playing_ = want;
    }

    if (playing_ == kNoAudio) { return false; }
    AudioInstance& a = audioInstances_[playing_];
    // A plugin that gave up with an empty FIFO hands the speakers back in this
    // same block rather than after one of silence: there is nothing of its to
    // play out, so there is nothing to wait for.
    if (a.stopped && a.fifoL.size() == a.fifoHead) {
        noteAudioEventLocked(AudioEvent::Kind::Stopped, a, nullptr);
        playing_ = kNoAudio;
        return false;
    }
    pullAudioLocked(a, left, right, frames);
    return true;
}

std::string PluginRunner::playingPlugin() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (playing_ == kNoAudio) { return std::string(); }
    return audioInstances_[playing_].name;
}

std::string PluginRunner::playingPluginKey() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (playing_ == kNoAudio) { return std::string(); }
    return audioInstances_[playing_].key;
}

std::uint64_t PluginRunner::audioGaps() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return audioGaps_;
}

std::uint64_t PluginRunner::audioGapFrames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return audioGapFrames_;
}

void PluginRunner::pollAudioDiagnostics() {
    // COPIED OUT UNDER THE LOCK, WRITTEN OUT WITHOUT IT. diagLogf takes a
    // mutex and writes a file; holding this lock across that would put the
    // audio thread behind a disk write at exactly the moment it is asking for
    // the next block.
    std::array<AudioEvent, kMaxAudioEvents> events{};
    std::size_t count = 0;
    std::uint64_t gaps = 0;
    std::uint64_t gapFrames = 0;
    double sinkRate = 0.0;
    std::string playing;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events = audioEvents_;
        count = audioEventCount_;
        audioEventCount_ = 0;
        gaps = audioGaps_;
        gapFrames = audioGapFrames_;
        sinkRate = audioRateHz_;
        if (playing_ != kNoAudio) { playing = audioInstances_[playing_].name; }
    }

    for (std::size_t i = 0; i < count; ++i) {
        const AudioEvent& e = events[i];
        const char* chans = e.channels == 2u ? "stereo" : "mono";
        switch (e.kind) {
            case AudioEvent::Kind::Started:
                if (e.rateHz == e.sinkRateHz) {
                    diagLogf("audio: %s is playing (%u Hz %s, no resampling needed)", e.name,
                             static_cast<unsigned>(e.rateHz), chans);
                } else {
                    diagLogf("audio: %s is playing (%u Hz %s, resampled to %u)", e.name,
                             static_cast<unsigned>(e.rateHz), chans,
                             static_cast<unsigned>(e.sinkRateHz));
                }
                break;
            case AudioEvent::Kind::Stopped:
                diagLogf("audio: %s stopped; demodulated audio returns", e.name);
                break;
            case AudioEvent::Kind::Contended:
                diagLogf("audio: %s also wants the speakers; %s has them and keeps them",
                         e.name, e.holder);
                break;
        }
    }

    // The gap digest, once a minute and silent unless something actually
    // broke up - the sibling of the sink's own starvation line, and the same
    // "quiet unless it has news" posture. The clock starts at the first call
    // rather than at construction, so a runner nobody drains never reports a
    // minute that did not happen.
    const auto now = std::chrono::steady_clock::now();
    if (audioDigestAt_.time_since_epoch().count() == 0) {
        audioDigestAt_ = now;
        audioGapsReported_ = gaps;
        audioGapFramesReported_ = gapFrames;
        return;
    }
    if (now - audioDigestAt_ < std::chrono::seconds(60)) { return; }
    if (gaps > audioGapsReported_ && sinkRate > 0.0) {
        const double silentMs =
            1000.0 * static_cast<double>(gapFrames - audioGapFramesReported_) / sinkRate;
        diagLogf("audio: plugin audio (%s) came up short in %llu blocks in the last minute "
                 "(%.0f ms of silence)",
                 playing.empty() ? "none playing now" : playing.c_str(),
                 static_cast<unsigned long long>(gaps - audioGapsReported_), silentMs);
    }
    audioGapsReported_ = gaps;
    audioGapFramesReported_ = gapFrames;
    audioDigestAt_ = now;
}

void PluginRunner::absorbLocked(const std::string& name, std::string& partial,
                                const char* data, std::size_t bytes) {
    partial.append(data, bytes);

    // '\n' separates lines and no NUL is written, so split here and keep any
    // tail for the next poll rather than emitting a half-line the user would
    // see flicker and change.
    std::size_t start = 0;
    for (;;) {
        const std::size_t nl = partial.find('\n', start);
        if (nl == std::string::npos) { break; }
        if (nl > start) { pending_.push_back({name, partial.substr(start, nl - start)}); }
        start = nl + 1;
    }
    partial.erase(0, start);

    // A decoder that never emits a newline must not grow this without bound.
    // Flush what it has as one line and carry on.
    if (partial.size() > 4096) {
        pending_.push_back({name, partial});
        partial.clear();
    }
    while (pending_.size() > kMaxPendingLines) { pending_.pop_front(); }
}

void PluginRunner::failLocked(std::size_t statusIndex, const std::string& name) {
    const std::string sentence = failureSentence(name);

    // The row this instance was created with. The index cannot go stale -
    // status_ is only ever emptied together with the instance vectors - but it
    // is bounds-checked anyway, because the cost of the check is nothing and
    // the cost of being wrong is a failure sentence written into a different
    // module's row, blaming a plugin that is working perfectly well.
    if (statusIndex < status_.size()) {
        status_[statusIndex].reason = DecoderIdleReason::PollFailed;
        status_[statusIndex].detail = sentence;
    }

    // AND SAID OUT LOUD, ONCE, in the decoder log - the same place the
    // decoder's own text goes, for the same reason image decoders report
    // their progress there: a failure that only tints a chip on a panel is a
    // failure nobody reads. This allocates on the real-time path, which is the
    // trade absorbLocked already makes for every decoded line, and here it is
    // paid exactly once per instance rather than once per line.
    pending_.push_back({name, sentence});
    while (pending_.size() > kMaxPendingLines) { pending_.pop_front(); }
}

void PluginRunner::pollLocked() {
    for (Instance& i : instances_) {
        // A DECODER THAT HAS FAILED PERMANENTLY IS NEVER ASKED AGAIN. The
        // negative return below is the ABI saying so, and this loop used to
        // read it as "nothing pending" - which left the runner polling and
        // feeding a dead instance for the rest of the session while every
        // panel went on describing it as decoding.
        if (i.failed) { continue; }
        for (;;) {
            const int32_t n =
                i.api->poll_text(i.handle, pollBuf_.data(), pollBuf_.size());
            if (n < 0) {  // failed for good
                i.failed = true;
                failLocked(i.statusIndex, i.name);
                break;
            }
            if (n == 0) { break; }  // nothing pending, the ordinary case
            const std::size_t got =
                std::min(static_cast<std::size_t>(n), pollBuf_.size());
            absorbLocked(i.name, i.partial, pollBuf_.data(), got);
        }
    }
}

void PluginRunner::pollIqLocked() {
    for (IqInstance& i : iqInstances_) {
        // Same rule as pollLocked: negative is permanent failure, not silence.
        if (i.failed) { continue; }
        for (;;) {
            const int32_t n =
                i.api->poll_text(i.handle, pollBuf_.data(), pollBuf_.size());
            if (n < 0) {
                i.failed = true;
                failLocked(i.statusIndex, i.name);
                break;
            }
            if (n == 0) { break; }
            const std::size_t got =
                std::min(static_cast<std::size_t>(n), pollBuf_.size());
            absorbLocked(i.name, i.partial, pollBuf_.data(), got);
        }
    }
}

void PluginRunner::pollImageTextLocked(std::uint32_t inputKind) {
    for (ImageInstance& i : imageInstances_) {
        // Same rule again, and it stops the pictures as well as the text: the
        // flag is one per instance because a permanent failure is a property
        // of the decoder, not of the call that happened to report it.
        if (i.failed || i.inputKind != inputKind) { continue; }
        for (;;) {
            const int32_t n =
                i.api->poll_text(i.handle, pollBuf_.data(), pollBuf_.size());
            if (n < 0) {
                i.failed = true;
                failLocked(i.statusIndex, i.name);
                break;
            }
            if (n == 0) { break; }
            const std::size_t got =
                std::min(static_cast<std::size_t>(n), pollBuf_.size());
            absorbLocked(i.name, i.partial, pollBuf_.data(), got);
        }
    }
}

void PluginRunner::pollImages(std::vector<HostImage>& out) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Re-seed `out` whenever the instance set changed under it - a rescan, a
    // source switch. Comparing the NAMES rather than just the count is what
    // makes a swap of one plugin for another impossible to miss: the count
    // alone would leave the new decoder's picture labelled with the old
    // plugin's name.
    bool shapeChanged = out.size() != imageInstances_.size();
    for (std::size_t i = 0; !shapeChanged && i < imageInstances_.size(); ++i) {
        if (out[i].plugin != imageInstances_[i].name) { shapeChanged = true; }
    }
    if (shapeChanged) {
        out.assign(imageInstances_.size(), HostImage{});
        for (std::size_t i = 0; i < imageInstances_.size(); ++i) {
            out[i].plugin = imageInstances_[i].name;
        }
    }

    for (std::size_t i = 0; i < imageInstances_.size(); ++i) {
        ImageInstance& ii = imageInstances_[i];
        // A decoder that has failed permanently is not polled again. Its entry
        // in `out` is left exactly as it stands, because half a weather image
        // is still the last thing that decoder saw and throwing it away would
        // punish the user for the plugin's fault.
        if (ii.failed) { continue; }
        CascadeImage img{};
        img.structSize = static_cast<std::uint32_t>(sizeof(CascadeImage));
        const std::int32_t got = ii.api->poll_image(ii.handle, &img);
        if (got < 0) {
            // NEGATIVE IS NOT "NO PICTURE YET". The ABI defines it as the
            // decoder having failed permanently, and this line used to fold it
            // in with 0 - so the row said WAIT for ever, nothing reached the
            // log, and the runner went on handing samples to a decoder that
            // had given up. No borrow is taken on a negative return, so there
            // is nothing to release here.
            ii.failed = true;
            failLocked(ii.statusIndex, ii.name);
            continue;
        }
        if (got == 0) { continue; }  // nothing new; the last picture stands

        // Validate before believing any of it. These are third-party numbers
        // and they are about to size an allocation and bound a read.
        const bool sane =
            img.pixels != nullptr && img.width > 0 && img.height > 0 &&
            img.width <= CASCADE_IMAGE_MAX_DIM && img.height <= CASCADE_IMAGE_MAX_DIM &&
            (img.format == CASCADE_IMAGE_GRAY8 || img.format == CASCADE_IMAGE_RGB24);
        const std::size_t bpp = img.format == CASCADE_IMAGE_RGB24 ? 3u : 1u;
        const std::size_t rowBytes = static_cast<std::size_t>(img.width) * bpp;
        // A stride SHORTER than a row would make the host read rows that
        // overlap and, at the last row, past the end of the plugin buffer.
        const bool strideOk = sane && img.stride >= rowBytes;
        const bool sizeOk =
            strideOk && static_cast<std::size_t>(img.width) * img.height <= kMaxImagePixels;

        if (sizeOk) {
            HostImage& hi = out[i];
            hi.width = img.width;
            hi.height = img.height;
            hi.format = img.format;
            hi.complete = img.complete != 0;
            hi.sequence = img.sequence;
            // Copied ROW BY ROW into a tightly packed buffer: the plugin may
            // pad its rows for alignment, and everything downstream (the
            // texture upload, the file writer) is simpler if the host copy
            // never does.
            hi.pixels.resize(rowBytes * img.height);
            for (std::uint32_t y = 0; y < img.height; ++y) {
                std::memcpy(hi.pixels.data() + static_cast<std::size_t>(y) * rowBytes,
                            img.pixels + static_cast<std::size_t>(y) * img.stride, rowBytes);
            }
            ++hi.revision;
        }
        // Released either way, and immediately: the borrow is over the moment
        // the copy is taken, so a plugin is never waiting on the GUI to finish
        // looking at a picture.
        ii.api->release_image(ii.handle, &img);
    }
}

std::vector<DecodedLine> PluginRunner::drainText() {
    // The audio path's diagnostics ride out on the call the GUI already makes
    // every frame, so a takeover line appears in the log without a single new
    // call site. Taken BEFORE the lock below, because it takes the lock
    // itself.
    pollAudioDiagnostics();
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DecodedLine> out(pending_.begin(), pending_.end());
    pending_.clear();
    return out;
}

std::vector<DecoderStatus> PluginRunner::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

std::size_t PluginRunner::audioFramesFed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return audioFed_;
}

std::size_t PluginRunner::iqFramesFed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return iqFed_;
}

std::size_t PluginRunner::activeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // INSTANCES STILL BEING FED, which is what the header promises. One that
    // reported permanent failure is kept only so its handle can be destroyed
    // in the right order; counting it would keep "DECODING" on the waterfall
    // and keep the decoder output button on the rail for a decoder that has
    // stopped, which is the same lie in a second place.
    std::size_t n = 0;
    for (const Instance& i : instances_) {
        if (!i.failed) { ++n; }
    }
    for (const IqInstance& i : iqInstances_) {
        if (!i.failed) { ++n; }
    }
    for (const ImageInstance& i : imageInstances_) {
        if (!i.failed) { ++n; }
    }
    return n;
}

}  // namespace cascade::core
