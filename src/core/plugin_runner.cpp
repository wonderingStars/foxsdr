// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "core/plugin_runner.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/diag_log.hpp"
#include "core/plugin_calls.hpp"
#include "core/plugin_marker.hpp"
#include "core/utf8_text.hpp"  // utf8Floor: audioPathSnapshot cuts a name on a character
#include "core/plugin_run.hpp"

namespace cascade::core {
namespace {

// Big enough that a decoder emitting a burst of lines empties in one poll,
// small enough to sit on the DSP thread's working set. The ABI guarantees the
// host passes at least 256.
constexpr std::size_t kPollBufBytes = 8192;

// THE MOST poll_text() CALLS ONE DECODER GETS PER BLOCK (128 KiB of text at
// kPollBufBytes a call). The drain loops run on the real-time DSP thread with
// mutex_ held, and they used to loop until the decoder answered 0 - so one
// decoder whose poll_text kept answering a positive count (a read cursor that
// never advances) spun the DSP thread for ever and froze the audio, every
// other decoder and every GUI reader of this runner behind it. Whatever is
// still pending after the budget is simply read on the next block: nothing is
// lost, a well-behaved decoder never comes near the limit, and a broken one
// costs a bounded slice of each block instead of the whole thread. Same idea
// as the patch runner's pollText (8 tries of 1 KiB).
constexpr int kMaxPollsPerBlock = 16;

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

// What the row says when the plugin was SWITCHED OFF (core/plugin_fault.hpp) rather than reporting a permanent error itself: it faulted,
// threw or stopped answering, the host contained it, and nothing of it is called again this session.
std::string quarantineSentence(const std::string& name, const fault::Slot* slot) {
    const fault::Why why = fault::quarantineWhy(slot);
    const char* what = why == fault::Why::Wedge       ? "stopped answering"
                       : why == fault::Why::Exception ? "threw an error"
                                                      : "faulted";
    return "\"" + name + "\" " + what + " in " + fault::entryWord(fault::quarantineEntry(slot)) +
           " and was switched off for this session, so it is being fed nothing further. RE-ENABLE it in Fitted modules to try it again.";
}

// The host clock an epoch is stamped with (CascadeStreamInfo::epochStartUnixMs).
std::int64_t nowUnixMs() {
    return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count());
}

// One instance's destroy(), as a guarded call: the marker says `unload` for the length of it, a plugin that faults in it is switched off
// (its other instances are then not destroyed at all, and are leaked with their module), and an instance of a plugin that was already
// switched off is simply dropped - nothing of it is called again.
template <class Api>
void destroyOne(fault::Slot* slot, const std::string& key, const Api* api, void* handle) {
    if (api == nullptr || handle == nullptr) { return; }
    marker::CallPhase inUnload(key, marker::Phase::Unload);
    calls::destroy(slot, api, handle);
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

void PluginRunner::setDormant(std::vector<std::string> keys) {
    std::lock_guard<std::mutex> lock(mutex_);
    dormant_.set(std::move(keys));
}

bool PluginRunner::isDormant(const std::string& pluginKey) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dormant_.contains(pluginKey);
}

bool PluginRunner::hasInstances(const std::string& pluginKey) const {
    if (pluginKey.empty()) { return false; }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Instance& i : instances_) {
        if (i.key == pluginKey) { return true; }
    }
    for (const IqInstance& i : iqInstances_) {
        if (i.key == pluginKey) { return true; }
    }
    for (const ImageInstance& i : imageInstances_) {
        if (i.key == pluginKey) { return true; }
    }
    for (const ProcessorInstance& p : processors_) {
        if (p.key == pluginKey) { return true; }
    }
    return false;
}

void PluginRunner::rebuild(const std::vector<LoadedPlugin>& plugins, double audioRateHz,
                           double iqRateHz, double centreHz) {
    std::unique_lock<std::mutex> lock(mutex_);
    // Drops the lock while the OLD instances' destroy() runs and retakes it
    // before returning - see its declaration. Everything below therefore
    // continues under the lock exactly as it did before.
    //
    // KNOWN AND DELIBERATE: the create() calls below still run with the lock
    // held, so a plugin that asked the host to retune from inside its create()
    // would re-enter the same mutex. No shipped plugin does, and the cure -
    // building the whole instance set into locals and swapping it in - would
    // give up the atomicity of a rebuild for a case that has never happened.
    // If one ever does, this is where it is fixed, the same way as destroy.
    destroyInstances(lock);
    audioRateHz_ = audioRateHz;
    iqRateHz_ = iqRateHz;
    centreHz_ = centreHz;
    // The caller read this centre from the source just now, so it is newer than any
    // retune() that was waiting for the lock.
    pendingCentre_.store(false, std::memory_order_relaxed);
    pollBuf_.resize(kPollBufBytes);
    // A NEW EPOCH BEFORE ANY create(), so a decoder that reads the stream
    // clock from inside its create() sees the stream it is about to be fed.
    if (clock_) { clock_->beginEpoch(iqRateHz, audioRateHz, nowUnixMs()); }
    processorNonFinite_ = 0;
    chainBuf_.reserve(2u * kChainReserveFrames);

    for (const LoadedPlugin& lp : plugins) {
        if (!lp.loaded) { continue; }
        // STOPPED BY THE USER: no instance, and therefore no samples, no text
        // and no picture. Skipped BEFORE any create() and before any status
        // line, because a stopped plugin is not idle for a reason the user
        // needs explaining - the row they stopped it from says so already, and
        // an orange "not being fed" warning for a deliberate choice would
        // train them to ignore the ones that matter.
        //
        // AND A DORMANT ONE (0.99.73): AUTO and not wanted, so it is given no
        // instance either, and for the same reason no status line - it is not
        // idle for a fault, and the Fitted modules window says IDLE against it
        // from the run state, not from a row it does not have.
        if (stopped_.contains(lp) || dormant_.contains(lp)) { continue; }
        buildPluginLocked(lp);
    }
}

bool PluginRunner::startPlugin(const LoadedPlugin& lp) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!lp.loaded) { return false; }
    // A stop is a decision; only the owner's setStopped() reverses it.
    if (stopped_.contains(lp)) { return false; }
    const std::string key = pluginKey(lp);
    // Awake from here on, so a later rebuild() builds it too.
    {
        const std::string id = pluginRunId(key);
        std::vector<std::string> rest;
        for (const std::string& k : dormant_.keys()) {
            if (k != id) { rest.push_back(k); }
        }
        dormant_.set(std::move(rest));
    }
    // The rates are the last rebuild()'s: until one has run there is nothing to
    // create an instance against.
    if (!(audioRateHz_ > 0.0) && !(iqRateHz_ > 0.0)) { return false; }
    // Already running: a second set of instances would feed the plugin twice.
    for (const Instance& i : instances_) {
        if (i.key == key) { return false; }
    }
    for (const IqInstance& i : iqInstances_) {
        if (i.key == key) { return false; }
    }
    for (const ImageInstance& i : imageInstances_) {
        if (i.key == key) { return false; }
    }
    for (const ProcessorInstance& p : processors_) {
        if (p.key == key) { return false; }
    }
    if (pollBuf_.size() < kPollBufBytes) { pollBuf_.resize(kPollBufBytes); }
    chainBuf_.reserve(2u * kChainReserveFrames);
    // A tune that arrived while the lock was busy is the receiver's real centre:
    // the instance must be created against it, not against the one before.
    applyPendingCentreLocked();
    // The create() calls run with the lock held, as rebuild()'s do (see there).
    buildPluginLocked(lp);
    return true;
}

void PluginRunner::stopPlugin(const std::string& pluginKey) {
    std::unique_lock<std::mutex> lock(mutex_);
    // Down until startPlugin(): a rebuild() in between must not bring it back.
    std::vector<std::string> keys = dormant_.keys();
    keys.push_back(pluginKey);
    dormant_.set(std::move(keys));
    removePluginLocked(pluginKey, lock);
}

void PluginRunner::buildPluginLocked(const LoadedPlugin& lp) {
    // STAMPED ON EVERY LINE THIS PLUGIN PRODUCES, at the end of the loop
    // body rather than at each of the eight places a DecoderStatus is
    // built. One assignment cannot be forgotten by the ninth; eight can,
    // and the one that was forgotten would be a plugin isFeeding could
    // never see.
    const std::string key = pluginKey(lp);
    fault::Slot* const slot = fault::slotFor(key);
    // A plugin that was switched off this session gets no instance, whoever asks (a rebuild, a wake): the engine's pass has already put
    // it down, and the module is only still mapped because nothing may unmap it.
    if (fault::isQuarantined(slot)) { return; }
    const fault::Origin origin = faultOriginOf(lp.origin);
    // THE CRASH-LOOP MARKER (core/plugin_marker.hpp): this plugin is inside create() from here to the end of this function.
    const marker::Phase phaseBefore = marker::phaseOf(key);
    marker::set(key, marker::Phase::Create, lp.version, origin);
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
        const bool resample = want != 0.0 && want != audioRateHz_;
        {
            void* h = calls::create(slot, lp.decoder,
                                    want != 0.0 ? static_cast<uint32_t>(want) : static_cast<uint32_t>(audioRateHz_));
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
                inst.slot = slot;
                inst.name = lp.name;
                inst.key = key;
                if (resample) { setUpResample(inst.resample, audioRateHz_, want); }
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
                st.detail = resample ? resampleSentence(lp.name, audioRateHz_, want)
                                     : "\"" + lp.name + "\" is decoding the tuned audio.";
                status_.push_back(std::move(st));
                started = true;
            }
        }
    }

    if (lp.iqDecoder != nullptr) {
        const double want = lp.iqDecoder->requiredRateHz;
        if (want != 0.0 && want != iqRateHz_) {
            DecoderStatus st;
            st.plugin = lp.name;
            st.reason = DecoderIdleReason::RateMismatch;
            st.stream = DecoderStream::Iq;
            st.wantRateHz = want;
            st.detail = rateSentence(lp.name, want, iqRateHz_, "raw I/Q");
            status_.push_back(std::move(st));
        } else {
            void* h = calls::create(slot, lp.iqDecoder, want != 0.0 ? want : iqRateHz_, centreHz_);
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
                inst.slot = slot;
                inst.name = lp.name;
                inst.key = key;
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
        const double have = isIq ? iqRateHz_ : audioRateHz_;
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
            void* h = calls::create(slot, lp.imageDecoder, want != 0.0 ? want : have, isIq ? centreHz_ : 0.0);
            if (h == nullptr) {
                st.reason = DecoderIdleReason::CreateFailed;
                st.detail =
                    "\"" + lp.name + "\" failed to start (its create() returned nothing).";
                status_.push_back(std::move(st));
            } else {
                ImageInstance inst;
                inst.api = lp.imageDecoder;
                inst.input = lp.imageInput;
                inst.handle = h;
                inst.slot = slot;
                inst.name = lp.name;
                inst.key = key;
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
        a.slot = slot;
        a.name = lp.name;
        a.key = key;
        a.rateHz = lp.audioOut->sampleRateHz;
        a.channels = lp.audioOut->channels;
        // The plugin's rate is validated non-zero and in range at load
        // time, so this ratio is always meaningful. Built once here, not
        // on the first block: the audio thread must not construct a
        // resampler (it allocates, and it computes a filter).
        if (static_cast<double>(a.rateHz) != audioRateHz_ && audioRateHz_ > 0.0) {
            a.rsL = std::make_unique<cascade::dsp::RationalResampler>(
                static_cast<unsigned>(audioRateHz_), a.rateHz);
            a.rsR = std::make_unique<cascade::dsp::RationalResampler>(
                static_cast<unsigned>(audioRateHz_), a.rateHz);
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

    // THE PLUGIN'S TRANSMIT AUDIO (host API level 2). Rides on the same borrowed
    // handle the audio-out table does, and is registered under the same rule: a
    // plugin whose decoder did not start has nothing to pull from, and the status
    // row its decoder already pushed says why. Registering it asks nothing of the
    // plugin and keys nothing: pollTxAudio only ever calls active() until the host
    // has keyed the transmitter for it.
    if (lp.txAudio != nullptr && audioHandle != nullptr) {
        TxInstance t;
        t.api = lp.txAudio;
        t.handle = audioHandle;
        t.slot = slot;
        t.name = lp.name;
        t.key = key;
        // Reserved here, on the control thread: the real-time pull never allocates.
        if (txPullBuf_.size() < TxAudioLink::kMaxPullFrames) {
            txPullBuf_.resize(TxAudioLink::kMaxPullFrames);
        }
        txInstances_.push_back(std::move(t));
    }

    // AN IN-CHAIN AUDIO PROCESSOR (host API level 1). Its own instance,
    // not riding on a decoder's the way the audio-out table does: it is
    // fed the audio the user hears, not a decoder's stream, and a plugin
    // that only processes has no decoder to ride on. Stereo at the audio
    // rate, because that is the shape of the chain where it runs (see
    // processAudioChain and the ABI's CascadeAudioProcessorApi).
    if (lp.audioProcessor != nullptr && audioRateHz_ > 0.0) {
        void* h = calls::create(slot, lp.audioProcessor, static_cast<uint32_t>(audioRateHz_), 2u);
        if (h != nullptr) {
            ProcessorInstance p;
            p.api = lp.audioProcessor;
            p.handle = h;
            p.slot = slot;
            p.name = lp.name;
            p.key = key;
            p.title = lp.audioProcessor->title != nullptr ? lp.audioProcessor->title
                                                          : lp.name;
            processors_.push_back(std::move(p));
            // A ROW LIKE ANY DECODER'S, so the module reads FED on its
            // plate while its processor runs (isFeeding answers from these
            // rows) instead of "takes no signal" - which a module rewriting
            // the audio the user hears plainly does not deserve.
            DecoderStatus st;
            st.plugin = lp.name;
            st.reason = DecoderIdleReason::Running;
            st.stream = DecoderStream::Audio;
            st.detail = "\"" + lp.name + "\" is processing the audio you hear.";
            status_.push_back(std::move(st));
        } else {
            DecoderStatus st;
            st.plugin = lp.name;
            st.reason = DecoderIdleReason::CreateFailed;
            st.stream = DecoderStream::None;
            st.detail = "\"" + lp.name +
                        "\" failed to start its audio processor (its create() returned "
                        "nothing), so the audio is not being processed.";
            status_.push_back(std::move(st));
        }
    }

    // No table this runner drives: say so rather than leaving the plugin
    // looking loaded-and-working. A plugin that ONLY processes audio is
    // driven all the same - it is simply not a decoder.
    if (!started && lp.audioProcessor == nullptr && lp.decoder == nullptr &&
        lp.iqDecoder == nullptr && lp.imageDecoder == nullptr) {
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
    statusGen_.fetch_add(1, std::memory_order_release);
    // Out of create(): an instance exists and has not yet been fed (`first-pass`; the engine's pass promotes it to `running` once it has
    // survived a few seconds), or none was made and the plugin is not in play at all.
    marker::set(key, (started || !processors_.empty()) ? marker::Phase::FirstPass : phaseBefore, lp.version, origin);
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
    // A RETUNE THAT FOUND THE LOCK BUSY lands here, between two blocks and before
    // this one's samples are handed out, so a decoder never sees half a block at
    // each centre. Before the early return below: the centre has to move even when
    // nothing is running, or the next instance would be created against the old one.
    // One atomic load when there is nothing to do.
    if (pendingCentre_.load(std::memory_order_acquire)) { applyPendingCentreLocked(); }
    // Counted whether or not anything takes these samples: the clock times
    // the STREAM, and every instance created this epoch is handed every block
    // from here on, so the count is also what each of them has received.
    if (clock_) { clock_->addIq(frames); }
    if (iqInstances_.empty() && iqImageCount_ == 0) { return; }
    // Same two rules as processAudio: a permanently failed instance is fed
    // nothing, and the count only moves when something was actually fed.
    bool fedAny = false;
    for (IqInstance& i : iqInstances_) {
        if (i.failed || fault::isQuarantined(i.slot)) { continue; }
        i.api->process(i.handle, interleaved, frames);
        fedAny = true;
    }
    for (ImageInstance& i : imageInstances_) {
        if (i.failed || i.inputKind != CASCADE_INPUT_IQ || fault::isQuarantined(i.slot)) { continue; }
        i.api->process(i.handle, interleaved, frames);
        fedAny = true;
    }
    if (fedAny) { iqFed_ += frames; }
    pollIqLocked();
    pollImageTextLocked(CASCADE_INPUT_IQ);
}

void PluginRunner::retune(double centreHz) {
    // TRY, never wait: see the declaration. A destroy()-time re-entry arrives here
    // with the lock dropped and takes whichever branch the DSP thread leaves it.
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        pendingCentreHz_.store(centreHz, std::memory_order_relaxed);
        pendingCentre_.store(true, std::memory_order_release);
        return;
    }
    // The lock is ours, so the DSP thread is not between its flag test and its apply:
    // a centre left pending by an EARLIER call is older than this one and must never
    // be applied after it.
    pendingCentre_.store(false, std::memory_order_relaxed);
    retuneLocked(centreHz);
}

void PluginRunner::applyPendingCentreLocked() {
    if (!pendingCentre_.exchange(false, std::memory_order_acq_rel)) { return; }
    retuneLocked(pendingCentreHz_.load(std::memory_order_relaxed));
}

void PluginRunner::retuneLocked(double centreHz) {
    if (centreHz == centreHz_) { return; }
    centreHz_ = centreHz;
    for (IqInstance& i : iqInstances_) {
        if (fault::isQuarantined(i.slot)) { continue; }
        // retune is OPTIONAL in the ABI - a decoder working on the envelope
        // usually does not care - so it is null-checked at the call site,
        // which is the contract the header states.
        if (i.api->retune != nullptr) { i.api->retune(i.handle, centreHz); }
    }
    for (ImageInstance& i : imageInstances_) {
        // Only the I/Q ones. An audio-input image decoder was created with
        // centerHz 0 by the ABI's own rule, so handing it a real RF frequency
        // now would contradict what it was told at create().
        if (i.inputKind == CASCADE_INPUT_IQ && i.api->retune != nullptr && !fault::isQuarantined(i.slot)) {
            i.api->retune(i.handle, centreHz);
        }
    }
}

void PluginRunner::clear() {
    std::unique_lock<std::mutex> lock(mutex_);
    destroyInstances(lock);
    // THE STREAM ENDS HERE. A new epoch with no rates says so: a plugin still
    // reading the clock (a patch-page instance, a worker thread) sees the
    // epoch move and both rates at zero rather than a count frozen mid-run.
    if (clock_) { clock_->beginEpoch(0.0, 0.0, nowUnixMs()); }
}

void PluginRunner::setStreamClock(std::shared_ptr<StreamClock> clock) {
    std::lock_guard<std::mutex> lock(mutex_);
    clock_ = std::move(clock);
}

void PluginRunner::processAudioChain(float* left, float* right, std::size_t frames) {
    if (left == nullptr || right == nullptr || frames == 0) { return; }
    std::lock_guard<std::mutex> lock(mutex_);
    if (processors_.empty()) { return; }
    // Reserved at rebuild; only a block larger than any before grows it.
    if (chainBuf_.size() < 2u * frames) { chainBuf_.resize(2u * frames); }
    float* ilv = chainBuf_.data();
    for (std::size_t i = 0; i < frames; ++i) {
        ilv[2u * i] = left[i];
        ilv[2u * i + 1u] = right[i];
    }
    for (ProcessorInstance& p : processors_) {
        if (fault::isQuarantined(p.slot)) { continue; }   // a switched-off processor passes the audio through untouched
        p.api->process(p.handle, ilv, frames);
        // AFTER EACH ONE, so the next processor is never handed a NaN either:
        // a processor that divides by its input would turn one into a block.
        for (std::size_t i = 0; i < 2u * frames; ++i) {
            if (!std::isfinite(ilv[i])) {
                ilv[i] = 0.0f;
                ++processorNonFinite_;
            }
        }
    }
    for (std::size_t i = 0; i < frames; ++i) {
        left[i] = ilv[2u * i];
        right[i] = ilv[2u * i + 1u];
    }
}

std::vector<std::string> PluginRunner::processorTitles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const ProcessorInstance& p : processors_) {
        out.push_back(p.title + " (" + p.name + ")");
    }
    return out;
}

std::uint64_t PluginRunner::processorNonFinite() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return processorNonFinite_;
}

void PluginRunner::destroyInstances(std::unique_lock<std::mutex>& lock) {
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
    // THE TRANSMIT CLAIMS GO FIRST TOO, for the same reason, and what the GUI has
    // been told about the holder goes with them: with no instance nobody wants the key.
    foldTxAskedLocked();
    txInstances_.clear();
    txHolder_ = kNoTx;
    if (txLink_) { txLink_->publishNone(TxAudioLink::nowMs()); }

    // MOVED OUT, THEN DESTROYED WITH THE LOCK DROPPED. See the declaration in
    // the header for why: a plugin's destroy() may ask the host for the time
    // or for a retune, and the tune service comes straight back into
    // PluginRunner::retune() and this same mutex_. The runner is left holding
    // nothing before the lock goes, so anything that takes it in the gap - the
    // DSP thread, or a host service re-entered by the very destroy() running
    // below - sees an empty runner, which is the truth.
    std::vector<Instance> deadAudio;
    std::vector<IqInstance> deadIq;
    std::vector<ImageInstance> deadImage;
    std::vector<ProcessorInstance> deadProc;
    deadProc.swap(processors_);
    deadAudio.swap(instances_);
    deadIq.swap(iqInstances_);
    deadImage.swap(imageInstances_);
    audioImageCount_ = 0;
    iqImageCount_ = 0;
    status_.clear();
    statusGen_.fetch_add(1, std::memory_order_release);
    audioFed_ = 0;
    iqFed_ = 0;

    lock.unlock();
    // THE ORDER IS THE ABI'S: a handle is destroyed exactly once, after the
    // last call of anything else on it. An instance that failed permanently
    // kept its handle for precisely this moment.
    for (Instance& i : deadAudio) { destroyOne(i.slot, i.key, i.api, i.handle); }
    for (IqInstance& i : deadIq) { destroyOne(i.slot, i.key, i.api, i.handle); }
    for (ImageInstance& i : deadImage) { destroyOne(i.slot, i.key, i.api, i.handle); }
    for (ProcessorInstance& p : deadProc) { destroyOne(p.slot, p.key, p.api, p.handle); }
    // The moved-out vectors are released here, with the lock still down: a
    // resampler's buffers are a free() and belong outside it too.
    deadAudio.clear();
    deadIq.clear();
    deadImage.clear();
    deadProc.clear();
    lock.lock();
}

namespace {

// Moves every element of `live` whose key is `key` into `dead`, keeping the rest in order.
template <typename T>
void moveOutKey(std::vector<T>& live, const std::string& key, std::vector<T>& dead) {
    std::vector<T> keep;
    keep.reserve(live.size());
    for (T& t : live) {
        if (t.key == key) {
            dead.push_back(std::move(t));
        } else {
            keep.push_back(std::move(t));
        }
    }
    live = std::move(keep);
}

}  // namespace

void PluginRunner::removePluginLocked(const std::string& key,
                                      std::unique_lock<std::mutex>& lock) {
    if (key.empty()) { return; }

    // THE SPEAKERS FIRST, as destroyInstances does it: an AudioInstance borrows
    // a decoder instance's handle, so it has to go before the instance does. If
    // this plugin was the one playing, that is said (the log line of the
    // takeover transition) and the demodulated audio returns; otherwise the
    // playing index is re-pointed at the same instance in the shorter vector.
    {
        std::vector<AudioInstance> keep;
        keep.reserve(audioInstances_.size());
        std::size_t newPlaying = kNoAudio;
        for (std::size_t i = 0; i < audioInstances_.size(); ++i) {
            if (audioInstances_[i].key == key) {
                if (i == playing_) {
                    noteAudioEventLocked(AudioEvent::Kind::Stopped, audioInstances_[i], nullptr);
                }
                continue;
            }
            if (i == playing_) { newPlaying = keep.size(); }
            keep.push_back(std::move(audioInstances_[i]));
        }
        audioInstances_ = std::move(keep);
        playing_ = newPlaying;
    }
    // ...AND ITS TRANSMIT CLAIM, with the holder re-pointed (or dropped) the same way.
    {
        foldTxAskedLocked();
        std::vector<TxInstance> keep;
        keep.reserve(txInstances_.size());
        std::size_t newHolder = kNoTx;
        for (std::size_t i = 0; i < txInstances_.size(); ++i) {
            if (txInstances_[i].key == key) { continue; }
            if (i == txHolder_) { newHolder = keep.size(); }
            keep.push_back(std::move(txInstances_[i]));
        }
        const bool lostHolder = txHolder_ != kNoTx && newHolder == kNoTx;
        txInstances_ = std::move(keep);
        txHolder_ = newHolder;
        if (lostHolder && txLink_) { txLink_->publishNone(TxAudioLink::nowMs()); }
    }

    // THIS PLUGIN'S INSTANCES OUT, everyone else's left exactly as they are.
    std::vector<Instance> deadAudio;
    std::vector<IqInstance> deadIq;
    std::vector<ImageInstance> deadImage;
    std::vector<ProcessorInstance> deadProc;
    moveOutKey(instances_, key, deadAudio);
    moveOutKey(iqInstances_, key, deadIq);
    moveOutKey(imageInstances_, key, deadImage);
    moveOutKey(processors_, key, deadProc);

    // ITS STATUS ROWS OUT, and the survivors' row indices follow: an instance
    // reports through the index it was created with (see Instance::statusIndex),
    // and the rows after the removed ones have moved up.
    {
        constexpr std::size_t kGone = static_cast<std::size_t>(-1);
        std::vector<std::size_t> moved(status_.size(), kGone);
        std::vector<DecoderStatus> kept;
        kept.reserve(status_.size());
        for (std::size_t i = 0; i < status_.size(); ++i) {
            if (status_[i].key == key) { continue; }
            moved[i] = kept.size();
            kept.push_back(std::move(status_[i]));
        }
        status_ = std::move(kept);
        const auto follow = [&moved, kGone](std::size_t idx) {
            return idx < moved.size() && moved[idx] != kGone ? moved[idx] : idx;
        };
        for (Instance& i : instances_) { i.statusIndex = follow(i.statusIndex); }
        for (IqInstance& i : iqInstances_) { i.statusIndex = follow(i.statusIndex); }
        for (ImageInstance& i : imageInstances_) { i.statusIndex = follow(i.statusIndex); }
    }
    audioImageCount_ = 0;
    iqImageCount_ = 0;
    for (const ImageInstance& i : imageInstances_) {
        if (i.inputKind == CASCADE_INPUT_IQ) {
            ++iqImageCount_;
        } else {
            ++audioImageCount_;
        }
    }

    // DESTROYED WITH THE LOCK DROPPED, for the reason destroyInstances gives: a
    // plugin's destroy() may call the host back, and the host's tune service
    // comes straight back into retune() and this mutex.
    lock.unlock();
    for (Instance& i : deadAudio) { destroyOne(i.slot, i.key, i.api, i.handle); }
    for (IqInstance& i : deadIq) { destroyOne(i.slot, i.key, i.api, i.handle); }
    for (ImageInstance& i : deadImage) { destroyOne(i.slot, i.key, i.api, i.handle); }
    for (ProcessorInstance& p : deadProc) { destroyOne(p.slot, p.key, p.api, p.handle); }
    deadAudio.clear();
    deadIq.clear();
    deadImage.clear();
    deadProc.clear();
    lock.lock();
}

void PluginRunner::processAudio(const float* mono, std::size_t frames) {
    if (mono == nullptr || frames == 0) { return; }
    std::lock_guard<std::mutex> lock(mutex_);
    if (clock_) { clock_->addAudio(frames); }  // see processIq
    if (instances_.empty() && audioImageCount_ == 0) { return; }
    // NOTHING IS HANDED TO AN INSTANCE THAT HAS FAILED PERMANENTLY, and the
    // frame count is only raised if something actually took the samples. The
    // count exists to separate "it was never fed" from "it was fed and found
    // nothing", so a number that climbed while every decoder was dead would
    // answer that question with the wrong one of the two.
    bool fedAny = false;
    for (Instance& i : instances_) {
        if (i.failed || fault::isQuarantined(i.slot)) { continue; }
        if (i.resample.resampler) {
            const std::size_t n = resampleBlock(i.resample, mono, frames);
            if (n != 0) { i.api->process(i.handle, i.resample.out.data(), n); }
        } else {
            i.api->process(i.handle, mono, frames);
        }
        fedAny = true;
    }
    for (ImageInstance& i : imageInstances_) {
        if (i.failed || i.inputKind != CASCADE_INPUT_AUDIO || fault::isQuarantined(i.slot)) { continue; }
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
    if (fault::isQuarantined(a.slot)) { a.stopped = true; }
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
        if (!a.stopped && fault::isQuarantined(a.slot)) { a.stopped = true; }   // switched off: it stops producing, as the ABI's negative pull
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

bool PluginRunner::audioPathSnapshot(std::uint64_t& gaps, std::uint64_t& gapFrames, char* playing,
                                     std::size_t cap) const {
    bool any = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        gaps = audioGaps_;
        gapFrames = audioGapFrames_;
        any = playing_ != kNoAudio;
        if (playing != nullptr && cap > 0) {
            playing[0] = '\0';
            if (any) {
                // A copy of bytes, no formatting and no allocation: the cut
                // onto a whole character is made after the lock is let go.
                const std::string& n = audioInstances_[playing_].name;
                const std::size_t k = std::min(n.size(), cap - 1);
                std::memcpy(playing, n.data(), k);
                playing[k] = '\0';
            }
        }
    }
    if (playing != nullptr && cap > 0) {
        const std::size_t len = std::strlen(playing);
        playing[utf8Floor(playing, len)] = '\0';
    }
    return any;
}

void PluginRunner::setPlayingForTest(const std::string& name, std::uint64_t gaps, std::uint64_t gapFrames) {
    std::lock_guard<std::mutex> lock(mutex_);
    audioGaps_ = gaps;
    audioGapFrames_ = gapFrames;
    audioInstances_.clear();
    playing_ = kNoAudio;
    if (name.empty()) { return; }
    AudioInstance a;
    a.name = name;
    a.key = name;
    a.stopped = true;
    audioInstances_.push_back(std::move(a));
    playing_ = 0;
}

// ---------------------------------------------------------------------------
// The plugin transmit path
// ---------------------------------------------------------------------------

void PluginRunner::setTxLink(std::shared_ptr<TxAudioLink> link) {
    std::lock_guard<std::mutex> lock(mutex_);
    txLink_ = std::move(link);
}

void PluginRunner::setTxGranted(std::vector<std::string> keys) {
    std::lock_guard<std::mutex> lock(mutex_);
    txGranted_ = std::move(keys);
    // A GRANT TAKEN BACK TAKES THE HOLDER WITH IT AT ONCE, not at the next audio block: the
    // GUI thread reads who holds the transmitter every frame, and a holder it was told about
    // a block ago must not outlive the permission it was holding by.
    if (txHolder_ != kNoTx && txHolder_ < txInstances_.size() &&
        std::find(txGranted_.begin(), txGranted_.end(), txInstances_[txHolder_].key) ==
            txGranted_.end()) {
        txHolder_ = kNoTx;
        if (txLink_) { txLink_->publishNone(TxAudioLink::nowMs()); }
    }
}

void PluginRunner::setTxKeyed(bool keyed) {
    // UNDER THE LOCK, which is the point. pollTxAudio holds it for the whole of a block
    // including the pull(), so once this returns false no pull is in flight and none
    // will start: the caller can then empty the ring knowing nothing is still arriving.
    std::lock_guard<std::mutex> lock(mutex_);
    if (txLink_) { txLink_->setKeyed(keyed); }
}

void PluginRunner::pollTxAudio() {
    std::lock_guard<std::mutex> lock(mutex_);
    pollTxAudioLocked();
}

void PluginRunner::pollTxAudioLocked() {
    if (txInstances_.empty() || !txLink_) { return; }
    const std::int64_t now = TxAudioLink::nowMs();
    const auto granted = [this](const TxInstance& t) {
        return std::find(txGranted_.begin(), txGranted_.end(), t.key) != txGranted_.end();
    };

    // EVERY INSTANCE IS ASKED active() EVERY BLOCK, granted or not, keyed or not. That
    // is how a plugin comes to have ASKED (and so to be offered the grant), and how a
    // second plugin gets the transmitter after the first lets go. active() is defined
    // as a read of state the plugin has already decided, so asking costs nothing and
    // puts nothing on the air.
    for (TxInstance& t : txInstances_) {
        // A plugin that was switched off stops here, as if its pull() had said "stopped for good": it never holds the transmitter again
        // this session, and the key it held goes up with it (the GUI thread unkeys when the link says nobody wants it).
        if (!t.stopped && fault::isQuarantined(t.slot)) { t.stopped = true; }
        if (t.stopped) {
            t.active = false;
            continue;
        }
        t.active = t.api->active(t.handle) != 0;
        if (t.active) { t.asked = true; }
    }

    // WHO HOLDS THE TRANSMITTER. Decided afresh while the key is open - the first
    // plugin in load order that is granted and wants it, so two asking at once is
    // settled by load order and not by which block the question was asked in - and
    // STICKY while it is closed on one: a second plugin that starts wanting the key in
    // the middle of a transmission waits for the next one.
    const bool keyed = txLink_->keyed();
    if (!keyed) {
        txHolder_ = kNoTx;
        for (std::size_t i = 0; i < txInstances_.size(); ++i) {
            const TxInstance& t = txInstances_[i];
            if (!t.stopped && t.active && granted(t)) {
                txHolder_ = i;
                break;
            }
        }
    } else if (txHolder_ != kNoTx && txInstances_[txHolder_].stopped) {
        // A holder that has stopped for good stops holding at once. (One whose grant is taken
        // back is dropped by setTxGranted, under this same lock, before any block can see it.)
        txHolder_ = kNoTx;
    }

    if (txHolder_ == kNoTx) {
        txLink_->publishNone(now);
        return;
    }
    TxInstance& h = txInstances_[txHolder_];
    txLink_->publishActive(h.active, now);

    // PULL ONLY WHILE KEYED FOR THIS PLUGIN AND IT WANTS THE KEY. Being the holder IS having
    // the grant (a holder is chosen among the granted, and dropped the moment the grant goes);
    // being keyed is the host's decision, and wanting the key is the plugin's. With active()
    // zero the host stops asking and lets the ring play out.
    if (!keyed || !h.active) { return; }
    const std::size_t fill = txLink_->backlog();
    if (fill >= TxAudioLink::kLeadFrames) { return; }
    const std::size_t ask = std::min(TxAudioLink::kLeadFrames - fill, TxAudioLink::kMaxPullFrames);
    if (txPullBuf_.size() < ask) { return; }  // not reserved: never grow on this thread
    const std::int32_t got = h.api->pull(h.handle, txPullBuf_.data(), ask);
    if (got < 0) {
        // The ABI's "stopped for good": never pulled or asked again, and nothing queued
        // is worth sending. The link says nobody wants the key; the GUI thread unkeys.
        h.stopped = true;
        h.active = false;
        txHolder_ = kNoTx;
        txLink_->publishNone(now);
        return;
    }
    if (got > 0) {
        std::size_t n = static_cast<std::size_t>(got);
        if (n > ask) { n = ask; }  // a plugin that overstated itself
        txLink_->write(txPullBuf_.data(), n);
    }
}

void PluginRunner::foldTxAskedLocked() {
    for (const TxInstance& t : txInstances_) {
        if (!t.asked) { continue; }
        if (std::find(txAsked_.begin(), txAsked_.end(), t.key) == txAsked_.end()) {
            txAsked_.push_back(t.key);
        }
    }
}

PluginRunner::TxPluginState PluginRunner::txState(const std::string& pluginKey) const {
    TxPluginState s;
    if (pluginKey.empty()) { return s; }
    std::lock_guard<std::mutex> lock(mutex_);
    s.asked = std::find(txAsked_.begin(), txAsked_.end(), pluginKey) != txAsked_.end();
    for (std::size_t i = 0; i < txInstances_.size(); ++i) {
        const TxInstance& t = txInstances_[i];
        if (t.key != pluginKey) { continue; }
        s.declared = true;
        s.name = t.name;
        s.asked = s.asked || t.asked;
        s.stopped = t.stopped;
        s.holder = (i == txHolder_);
        const bool isGranted =
            std::find(txGranted_.begin(), txGranted_.end(), t.key) != txGranted_.end();
        s.waiting = isGranted && t.active && !s.holder && txHolder_ != kNoTx;
        break;
    }
    return s;
}

std::uint32_t PluginRunner::txBits(const std::string& pluginKey) const {
    if (pluginKey.empty()) { return 0u; }
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint32_t bits = 0u;
    if (std::find(txAsked_.begin(), txAsked_.end(), pluginKey) != txAsked_.end()) { bits |= kTxAsked; }
    for (std::size_t i = 0; i < txInstances_.size(); ++i) {
        const TxInstance& t = txInstances_[i];
        if (t.key != pluginKey) { continue; }
        bits |= kTxDeclared;
        if (t.asked) { bits |= kTxAsked; }
        if (t.stopped) { bits |= kTxStopped; }
        const bool holder = (i == txHolder_);
        if (holder) { bits |= kTxHolder; }
        const bool isGranted = std::find(txGranted_.begin(), txGranted_.end(), t.key) != txGranted_.end();
        if (isGranted && t.active && !holder && txHolder_ != kNoTx) { bits |= kTxWaiting; }
        break;
    }
    return bits;
}

bool PluginRunner::txHolderKeyInto(std::string& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (txHolder_ == kNoTx) {
        out.clear();
        return false;
    }
    out.assign(txInstances_[txHolder_].key);
    return true;
}

std::vector<std::string> PluginRunner::txAskers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out = txAsked_;
    for (const TxInstance& t : txInstances_) {
        if (t.asked && std::find(out.begin(), out.end(), t.key) == out.end()) {
            out.push_back(t.key);
        }
    }
    return out;
}

std::string PluginRunner::txHolderName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return txHolder_ == kNoTx ? std::string() : txInstances_[txHolder_].name;
}

std::string PluginRunner::txHolderKey() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return txHolder_ == kNoTx ? std::string() : txInstances_[txHolder_].key;
}

std::string PluginRunner::txFirstGrantedName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const TxInstance& t : txInstances_) {
        if (std::find(txGranted_.begin(), txGranted_.end(), t.key) != txGranted_.end()) {
            return t.name;
        }
    }
    return std::string();
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

void PluginRunner::failLocked(std::size_t statusIndex, const std::string& name, const fault::Slot* slot) {
    const std::string sentence = (slot != nullptr && fault::isQuarantined(slot)) ? quarantineSentence(name, slot) : failureSentence(name);

    // The row this instance was created with. The index cannot go stale -
    // status_ is only ever emptied together with the instance vectors - but it
    // is bounds-checked anyway, because the cost of the check is nothing and
    // the cost of being wrong is a failure sentence written into a different
    // module's row, blaming a plugin that is working perfectly well.
    if (statusIndex < status_.size()) {
        status_[statusIndex].reason = DecoderIdleReason::PollFailed;
        status_[statusIndex].detail = sentence;
        statusGen_.fetch_add(1, std::memory_order_release);
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
        // Bounded - see kMaxPollsPerBlock. The rest waits for the next block.
        for (int k = 0; k < kMaxPollsPerBlock; ++k) {
            const int32_t n = calls::pollText(i.slot, i.api, i.handle, pollBuf_.data(), pollBuf_.size());
            if (n < 0) {  // failed for good (or switched off: the guard answers the same)
                i.failed = true;
                failLocked(i.statusIndex, i.name, i.slot);
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
        // And the same bound (kMaxPollsPerBlock).
        for (int k = 0; k < kMaxPollsPerBlock; ++k) {
            const int32_t n = calls::pollText(i.slot, i.api, i.handle, pollBuf_.data(), pollBuf_.size());
            if (n < 0) {
                i.failed = true;
                failLocked(i.statusIndex, i.name, i.slot);
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
        // And the same bound (kMaxPollsPerBlock).
        for (int k = 0; k < kMaxPollsPerBlock; ++k) {
            const int32_t n = calls::pollText(i.slot, i.api, i.handle, pollBuf_.data(), pollBuf_.size());
            if (n < 0) {
                i.failed = true;
                failLocked(i.statusIndex, i.name, i.slot);
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
    // source switch, a plugin woken or put to sleep. Comparing the NAMES rather
    // than just the count is what makes a swap of one plugin for another
    // impossible to miss: the count alone would leave the new decoder's picture
    // labelled with the old plugin's name.
    //
    // AN ENTRY THAT SURVIVES THE CHANGE KEEPS ITS PICTURE (0.99.73). Re-seeding
    // every slot from empty was harmless while the set changed only wholesale,
    // but a plugin woken or put to sleep changes it under every OTHER plugin's
    // picture, and a decoder offers a picture only when it changes: the one
    // already received would never be offered again, and a window showing it
    // would go blank. So the entries are matched by name and carried over; only
    // a name with no entry yet starts empty, and a plugin that has gone loses
    // its entry (its picture is part of the instance that made it).
    bool shapeChanged = out.size() != imageInstances_.size();
    for (std::size_t i = 0; !shapeChanged && i < imageInstances_.size(); ++i) {
        if (out[i].plugin != imageInstances_[i].name) { shapeChanged = true; }
    }
    if (shapeChanged) {
        std::vector<HostImage> next(imageInstances_.size());
        std::vector<bool> taken(out.size(), false);
        for (std::size_t i = 0; i < imageInstances_.size(); ++i) {
            for (std::size_t k = 0; k < out.size(); ++k) {
                if (!taken[k] && out[k].plugin == imageInstances_[i].name) {
                    taken[k] = true;
                    next[i] = std::move(out[k]);
                    break;
                }
            }
            next[i].plugin = imageInstances_[i].name;
        }
        out = std::move(next);
    }

    for (std::size_t i = 0; i < imageInstances_.size(); ++i) {
        ImageInstance& ii = imageInstances_[i];
        out[i].interactive = ii.input != nullptr && !ii.failed;
        // A decoder that has failed permanently is not polled again. Its entry
        // in `out` is left exactly as it stands, because half a weather image
        // is still the last thing that decoder saw and throwing it away would
        // punish the user for the plugin's fault.
        if (ii.failed) { continue; }
        CascadeImage img{};
        img.structSize = static_cast<std::uint32_t>(sizeof(CascadeImage));
        const std::int32_t got = calls::pollImage(ii.slot, ii.api, ii.handle, &img);
        if (got < 0) {
            // NEGATIVE IS NOT "NO PICTURE YET". The ABI defines it as the
            // decoder having failed permanently, and this line used to fold it
            // in with 0 - so the row said WAIT for ever, nothing reached the
            // log, and the runner went on handing samples to a decoder that
            // had given up. No borrow is taken on a negative return, so there
            // is nothing to release here.
            ii.failed = true;
            out[i].interactive = false;
            failLocked(ii.statusIndex, ii.name, ii.slot);
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
            // Copied ROW BY ROW into a tightly packed buffer: the plugin may
            // pad its rows for alignment, and everything downstream (the
            // texture upload, the file writer) is simpler if the host copy
            // never does. THE READ IS GUARDED (plugin containment P2,
            // fault::copyRowsFromPlugin): the numbers above are checked, but a
            // pointer cannot be - one that is wild, or readable for less than
            // width x height says, is the plugin's fault, not the process's.
            std::vector<std::uint8_t> packed(rowBytes * img.height);
            if (fault::copyRowsFromPlugin(ii.slot, fault::Entry::PollImage, packed.data(), rowBytes, img.pixels, img.stride,
                                          img.height)) {
                ii.width = img.width;
                ii.height = img.height;
                HostImage& hi = out[i];
                hi.width = img.width;
                hi.height = img.height;
                hi.format = img.format;
                hi.complete = img.complete != 0;
                hi.sequence = img.sequence;
                hi.pixels = std::move(packed);
                ++hi.revision;
            } else {
                // The plugin lent memory it did not have: switched off by the read, so its instance is finished here as a failed poll is.
                ii.failed = true;
                out[i].interactive = false;
                failLocked(ii.statusIndex, ii.name, ii.slot);
            }
        } else {
            fault::noteMisuse(ii.slot, fault::Misuse::BadReturn);   // a picture whose numbers cannot be believed
        }
        // Released either way, and immediately: the borrow is over the moment
        // the copy is taken, so a plugin is never waiting on the GUI to finish
        // looking at a picture.
        calls::releaseImage(ii.slot, ii.api, ii.handle, &img);
    }
}

bool PluginRunner::imagePress(const std::string& plugin, std::uint32_t x, std::uint32_t y) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const ImageInstance& image : imageInstances_) {
        if (image.name == plugin && !image.failed && image.input != nullptr &&
            x < image.width && y < image.height) {
            calls::press(image.slot, image.input, x, y);
            return true;
        }
    }
    return false;
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

void PluginRunner::feedSnapshot(std::size_t& active, std::vector<std::string>& feedingKeys) const {
    std::lock_guard<std::mutex> lock(mutex_);
    // activeCount()'s count, inline (it takes the same lock).
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
    active = n;
    // isFeeding()'s answer for every key at once: a key is feeding when any
    // status line with it reads Running; an empty key never is.
    std::size_t k = 0;
    for (const DecoderStatus& s : status_) {
        if (s.key.empty() || s.reason != DecoderIdleReason::Running) { continue; }
        bool seen = false;
        for (std::size_t j = 0; j < k && !seen; ++j) { seen = feedingKeys[j] == s.key; }
        if (seen) { continue; }
        if (k < feedingKeys.size()) {
            feedingKeys[k].assign(s.key);
        } else {
            feedingKeys.push_back(s.key);
        }
        ++k;
    }
    feedingKeys.resize(k);
}

}  // namespace cascade::core
