// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "core/plugin_runner.hpp"

#include <algorithm>
#include <cstdio>

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

}  // namespace

PluginRunner::~PluginRunner() { clear(); }

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

        // Neither table, or nothing this runner drives: say so rather than
        // leaving the plugin looking loaded-and-working.
        if (!started && lp.decoder == nullptr && lp.iqDecoder == nullptr) {
            DecoderStatus st;
            st.plugin = lp.name;
            st.reason = DecoderIdleReason::NoAudioTable;
            st.stream = DecoderStream::None;
            st.detail = "\"" + lp.name +
                        "\" provides no decoder this build can drive (image decoders are "
                        "not routed yet).";
            status_.push_back(std::move(st));
        }
    }
}

void PluginRunner::processIq(const float* interleaved, std::size_t frames) {
    if (interleaved == nullptr || frames == 0) { return; }
    std::lock_guard<std::mutex> lock(mutex_);
    if (iqInstances_.empty()) { return; }
    iqFed_ += frames;
    for (IqInstance& i : iqInstances_) {
        i.api->process(i.handle, interleaved, frames);
    }
    pollIqLocked();
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
    status_.clear();
    audioFed_ = 0;
    iqFed_ = 0;
}

void PluginRunner::processAudio(const float* mono, std::size_t frames) {
    if (mono == nullptr || frames == 0) { return; }
    std::lock_guard<std::mutex> lock(mutex_);
    if (instances_.empty()) { return; }
    audioFed_ += frames;
    for (Instance& i : instances_) {
        i.api->process(i.handle, mono, frames);
    }
    pollLocked();
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

void PluginRunner::pollLocked() {
    for (Instance& i : instances_) {
        for (;;) {
            const int32_t n =
                i.api->poll_text(i.handle, pollBuf_.data(), pollBuf_.size());
            if (n <= 0) { break; }  // 0 = nothing pending, <0 = failed for good
            const std::size_t got =
                std::min(static_cast<std::size_t>(n), pollBuf_.size());
            absorbLocked(i.name, i.partial, pollBuf_.data(), got);
        }
    }
}

void PluginRunner::pollIqLocked() {
    for (IqInstance& i : iqInstances_) {
        for (;;) {
            const int32_t n =
                i.api->poll_text(i.handle, pollBuf_.data(), pollBuf_.size());
            if (n <= 0) { break; }
            const std::size_t got =
                std::min(static_cast<std::size_t>(n), pollBuf_.size());
            absorbLocked(i.name, i.partial, pollBuf_.data(), got);
        }
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
    return instances_.size() + iqInstances_.size();
}

}  // namespace cascade::core
