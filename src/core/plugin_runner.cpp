// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "core/plugin_runner.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

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

        if (lp.decoder != nullptr) {
            // requiredRateHz == 0 means "any rate"; anything else must match
            // what the pipeline actually delivers. Feeding a 48 kHz decoder
            // 44.1 kHz audio and hoping is how a bit clock drifts, so a
            // mismatch idles it loudly. Per-decoder resampling would retire
            // this case.
            const double want = static_cast<double>(lp.decoder->requiredRateHz);
            if (want != 0.0 && want != audioRateHz) {
                DecoderStatus st;
                st.plugin = lp.name;
                st.reason = DecoderIdleReason::RateMismatch;
                st.stream = DecoderStream::Audio;
                st.wantRateHz = want;
                st.detail = rateSentence(lp.name, want, audioRateHz, "audio");
                status_.push_back(std::move(st));
            } else {
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
                    // The row pushed immediately below, taken BEFORE the push
                    // so the instance can rewrite its own reason later if the
                    // decoder gives up mid-run.
                    inst.statusIndex = status_.size();
                    instances_.push_back(std::move(inst));
                    DecoderStatus st;
                    st.plugin = lp.name;
                    st.reason = DecoderIdleReason::Running;
                    st.stream = DecoderStream::Audio;
                    st.detail = "\"" + lp.name + "\" is decoding the tuned audio.";
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
            DecoderStatus st;
            st.plugin = lp.name;
            st.output = DecoderOutput::Image;
            st.stream = isIq ? DecoderStream::Iq : DecoderStream::Audio;
            if (want != 0.0 && want != have) {
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
                    inst.statusIndex = status_.size();
                    imageInstances_.push_back(std::move(inst));
                    if (isIq) { ++iqImageCount_; } else { ++audioImageCount_; }
                    st.reason = DecoderIdleReason::Running;
                    st.detail = "\"" + lp.name + "\" is building an image from the " +
                                (isIq ? "raw receiver band (it ignores the VFO)."
                                      : "tuned audio.");
                    status_.push_back(std::move(st));
                    started = true;
                }
            }
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
        i.api->process(i.handle, mono, frames);
        fedAny = true;
    }
    for (ImageInstance& i : imageInstances_) {
        if (i.failed || i.inputKind != CASCADE_INPUT_AUDIO) { continue; }
        i.api->process(i.handle, mono, frames);
        fedAny = true;
    }
    if (fedAny) { audioFed_ += frames; }
    pollLocked();
    pollImageTextLocked(CASCADE_INPUT_AUDIO);
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
