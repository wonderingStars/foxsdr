// converter_view.hpp - the radio as the rest of the application sees it when
// an up- or down-converter sits in front of it: an IqSource that speaks AIR
// frequencies and tells the radio underneath the converted ones.
//
// THE TRANSLATION LAYER. core/freq_converter.hpp holds the arithmetic; this is
// where it is applied, and there are exactly two owners of one:
//   - core::Pipeline, whose activeSource() IS this view over whatever source
//     feeds the receiver (so the counter, the spectrum axis, click-to-tune, the
//     band plan, bookmarks, the scanner, presets, the plugin API, the web
//     remote, CAT and the saved centre all read and write air without knowing
//     a converter exists), and
//   - core::patch::PatchRadio, for the patch page's own radios.
// Everything else in the application goes through one of those two.
//
// WHAT IT CHANGES, and all it changes:
//   centerFrequencyHz()      the radio's readback, as the air frequency
//   setCenterFrequencyHz(a)  tells the radio radioFromAir(a) - or refuses,
//                            with a reason in lastError(), when the converter
//                            cannot deliver `a` at all (the radio would have to
//                            tune to 0 Hz or below)
//   read()                   the samples, mirrored when the converter inverts
// Every other call is the radio's own, unchanged: the rate, the name, start,
// stop, faults.
//
// THREADING. The same contract as IqSource: GUI/control thread for the
// setters and readbacks. mirrors() is the one thing a source thread may ask,
// and it is an atomic for that reason - the pipeline's source thread reads
// the RAW source (see Pipeline::sourceThreadBody for why it must never read
// through a view that a swap can re-point) and asks mirrors() whether to
// conjugate what it read.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <complex>
#include <cstddef>
#include <string>

#include "core/freq_converter.hpp"
#include "source/iq_source.hpp"

namespace cascade::source {

class ConverterView final : public IqSource {
public:
    // The source underneath. Non-owning; the owner re-binds on every swap.
    void bind(IqSource* inner) {
        inner_ = inner;
        error_.clear();
    }
    IqSource* inner() const { return inner_; }

    void setConverter(const cascade::core::ConverterSetting& s) {
        conv_ = s;
        mirror_.store(cascade::core::converterMirrors(s), std::memory_order_relaxed);
        error_.clear();
    }
    const cascade::core::ConverterSetting& converter() const { return conv_; }
    bool mirrors() const { return mirror_.load(std::memory_order_relaxed); }

    bool start() override { return inner_ != nullptr && inner_->start(); }
    void stop() override {
        if (inner_ != nullptr) { inner_->stop(); }
    }
    bool running() const override { return inner_ != nullptr && inner_->running(); }
    bool selfPaced() const override { return inner_ != nullptr && inner_->selfPaced(); }
    double sampleRateHz() const override {
        return inner_ != nullptr ? inner_->sampleRateHz() : 0.0;
    }
    bool setSampleRateHz(double hz) override {
        return inner_ != nullptr && inner_->setSampleRateHz(hz);
    }

    double centerFrequencyHz() const override {
        if (inner_ == nullptr) { return 0.0; }
        return cascade::core::airFromRadio(conv_, inner_->centerFrequencyHz());
    }

    bool setCenterFrequencyHz(double airHz) override {
        if (inner_ == nullptr) { return false; }
        if (!cascade::core::airReachable(conv_, airHz)) {
            // The radio is not asked at all: the frequency it would need is
            // 0 Hz or below, which no radio tunes. The application states the
            // reason in the user's terms (AppWindow::noteTuneRefused); this is
            // the source's own line for the log and the web remote.
            error_ = "the converter cannot reach that frequency (the radio would have to "
                     "tune to 0 Hz or below)";
            return false;
        }
        error_.clear();
        return inner_->setCenterFrequencyHz(cascade::core::radioFromAir(conv_, airHz));
    }

    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        if (inner_ == nullptr) { return 0; }
        const std::size_t got = inner_->read(dst, n);
        if (mirrors()) { cascade::core::conjugateInPlace(dst, got); }
        return got;
    }

    bool faulted() const override { return inner_ != nullptr && inner_->faulted(); }
    const char* name() const override { return inner_ != nullptr ? inner_->name() : ""; }
    const char* lastError() const override {
        if (!error_.empty()) { return error_.c_str(); }
        return inner_ != nullptr ? inner_->lastError() : "";
    }

private:
    IqSource* inner_ = nullptr;
    cascade::core::ConverterSetting conv_{};
    std::atomic<bool> mirror_{false};
    std::string error_;
};

}  // namespace cascade::source
