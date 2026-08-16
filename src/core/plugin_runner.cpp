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

std::string rateSentence(const std::string& name, double want, double have) {
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "\"%s\" needs %.0f Hz audio and the receiver is producing %.0f Hz, so it "
                  "is not being fed.",
                  name.c_str(), want, have);
    return std::string(buf);
}

}  // namespace

PluginRunner::~PluginRunner() { clear(); }

void PluginRunner::rebuild(const std::vector<LoadedPlugin>& plugins, double audioRateHz) {
    std::lock_guard<std::mutex> lock(mutex_);
    destroyLocked();
    audioRateHz_ = audioRateHz;
    pollBuf_.resize(kPollBufBytes);

    for (const LoadedPlugin& lp : plugins) {
        if (!lp.loaded) { continue; }

        // Only audio text decoders for now. An IQ or image decoder is loaded
        // and listed, but this runner has no stream to give it yet, and
        // saying so is better than leaving the user to wonder.
        if (lp.decoder == nullptr) {
            DecoderStatus st;
            st.plugin = lp.name;
            st.reason = DecoderIdleReason::NoAudioTable;
            st.detail = "\"" + lp.name +
                        "\" decodes raw I/Q, which this build does not yet route to plugins.";
            status_.push_back(std::move(st));
            continue;
        }

        // requiredRateHz == 0 means "any rate"; anything else must match what
        // the pipeline actually delivers. Resampling per decoder is the
        // obvious next step, but feeding a 48 kHz decoder 44.1 kHz audio and
        // hoping is how a bit clock drifts, so a mismatch idles it loudly.
        const double want = static_cast<double>(lp.decoder->requiredRateHz);
        if (want != 0.0 && want != audioRateHz) {
            DecoderStatus st;
            st.plugin = lp.name;
            st.reason = DecoderIdleReason::RateMismatch;
            st.wantRateHz = want;
            st.detail = rateSentence(lp.name, want, audioRateHz);
            status_.push_back(std::move(st));
            continue;
        }

        void* h = lp.decoder->create(want != 0.0 ? static_cast<uint32_t>(want)
                                                 : static_cast<uint32_t>(audioRateHz));
        if (h == nullptr) {
            DecoderStatus st;
            st.plugin = lp.name;
            st.reason = DecoderIdleReason::CreateFailed;
            st.detail = "\"" + lp.name + "\" failed to start (its create() returned nothing).";
            status_.push_back(std::move(st));
            continue;
        }

        Instance inst;
        inst.api = lp.decoder;
        inst.handle = h;
        inst.name = lp.name;
        instances_.push_back(std::move(inst));

        DecoderStatus st;
        st.plugin = lp.name;
        st.reason = DecoderIdleReason::Running;
        st.detail = "\"" + lp.name + "\" is decoding.";
        status_.push_back(std::move(st));
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
    status_.clear();
}

void PluginRunner::processAudio(const float* mono, std::size_t frames) {
    if (mono == nullptr || frames == 0) { return; }
    std::lock_guard<std::mutex> lock(mutex_);
    if (instances_.empty()) { return; }
    for (Instance& i : instances_) {
        i.api->process(i.handle, mono, frames);
    }
    pollLocked();
}

void PluginRunner::pollLocked() {
    for (Instance& i : instances_) {
        for (;;) {
            const int32_t n =
                i.api->poll_text(i.handle, pollBuf_.data(), pollBuf_.size());
            if (n <= 0) { break; }  // 0 = nothing pending, <0 = failed for good
            const std::size_t got =
                std::min(static_cast<std::size_t>(n), pollBuf_.size());
            i.partial.append(pollBuf_.data(), got);

            // '\n' separates lines and no NUL is written, so split here and
            // keep any tail for the next poll rather than emitting a
            // half-line the user would see flicker and change.
            std::size_t start = 0;
            for (;;) {
                const std::size_t nl = i.partial.find('\n', start);
                if (nl == std::string::npos) { break; }
                if (nl > start) {
                    pending_.push_back({i.name, i.partial.substr(start, nl - start)});
                }
                start = nl + 1;
            }
            i.partial.erase(0, start);

            // A decoder that never emits a newline must not grow this without
            // bound. Flush what it has as one line and carry on.
            if (i.partial.size() > 4096) {
                pending_.push_back({i.name, i.partial});
                i.partial.clear();
            }
            while (pending_.size() > kMaxPendingLines) { pending_.pop_front(); }
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

std::size_t PluginRunner::activeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return instances_.size();
}

}  // namespace cascade::core
