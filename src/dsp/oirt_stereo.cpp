// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/oirt_stereo.hpp"

#include <algorithm>
#include <cmath>

namespace cascade::dsp {

namespace {
constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kSubHz = 31250.0;
// K(f) = (1 + j6.4 f)/(5 + j6.4 f), f in kHz  =>  tau = 6.4 / (2 pi 1000) s.
constexpr double kKTau = 6.4 / (kTwoPi * 1000.0);
constexpr double kNominalCarrier = 0.2;   // 2.1.2.4: 20 %
constexpr double kNominalSScale = 0.8;    // 2.1.2.4: M and S 80 %
}  // namespace

OirtStereo::OirtStereo(double compositeRateHz, int decimation) : rate_(compositeRateHz) {
    stereoCapable_ = rate_ >= 100000.0;
    decim_ = std::max(1, decimation);
    while (decim_ > 1 && rate_ / decim_ < 32000.0) { --decim_; }

    w0_ = kTwoPi * kSubHz / rate_;
    cicLen_ = std::max<std::size_t>(1, static_cast<std::size_t>(rate_ / 6250.0));
    const double tu = static_cast<double>(cicLen_) / rate_;
    const double wn = kTwoPi * 15.0, zeta = 0.707;
    kp_ = 2.0 * zeta * wn * tu;
    ki_ = wn * wn * tu * tu;
    freqMax_ = kTwoPi * 40.0 / rate_;
    meanAlpha_ = 1.0 - std::exp(-tu / 0.02);
    lampHoldMax_ = std::max(1, static_cast<int>(0.04 / tu));

    // K^-1(s) = (5 + s tau) / (1 + s tau), bilinear: s = (2/T)(1 - z^-1)/(1 + z^-1).
    const double c = 2.0 * kKTau * rate_;
    const double a0 = 1.0 + c;
    kb0_ = (5.0 + c) / a0;
    kb1_ = (5.0 - c) / a0;
    ka1_ = (1.0 - c) / a0;

    // Windowed-sinc low-pass: passes S sidebands to 15 kHz, rejects the mixed
    // M image, which starts at 16.25 kHz (31.25 - 15).
    std::size_t n = static_cast<std::size_t>(5.5 * rate_ / 1500.0);
    if ((n & 1u) == 0) { ++n; }
    n = std::max<std::size_t>(n, 31);
    taps_.assign(n, 0.0f);
    const double fc = 15625.0 / rate_;
    const double mid = (static_cast<double>(n) - 1.0) / 2.0;
    double sum = 0.0;
    std::vector<double> t(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double x = static_cast<double>(i) - mid;
        const double sinc = (x == 0.0) ? 2.0 * fc : std::sin(kTwoPi * fc * x) / (0.5 * kTwoPi * x);
        const double w = 0.42 - 0.5 * std::cos(kTwoPi * i / (n - 1)) + 0.08 * std::cos(2.0 * kTwoPi * i / (n - 1));
        t[i] = sinc * w;
        sum += t[i];
    }
    for (std::size_t i = 0; i < n; ++i) { taps_[i] = static_cast<float>(t[i] / sum); }
    ringM_.assign(2 * n, 0.0f);
    ringE_.assign(2 * n, 0.0f);

    hpA_ = std::exp(-kTwoPi * 2.0 / outputRate());
    gateStep_ = 1.0 / (0.02 * outputRate());
    setDeemphasisUs(50.0);
    reset();
}

void OirtStereo::setDeemphasisUs(double us) {
    deTauSec_ = (us > 0.0 && std::isfinite(us)) ? us * 1e-6 : 0.0;
    dePole_ = deTauSec_ > 0.0 ? std::exp(-1.0 / (outputRate() * deTauSec_)) : 0.0;
    deL_ = deR_ = 0.0;
}

void OirtStereo::reset() {
    theta_ = 0.0; freq_ = 0.0;
    for (auto& s : ci_) { s.init(cicLen_); }
    for (auto& s : cq_) { s.init(cicLen_); }
    cicCount_ = 0; iMean_ = qMean_ = 0.0; iSlow_ = kNominalCarrier;
    lampHold_ = 0; lamp_ = false; carrierLevel_ = 0.0f;
    kx1_ = ky1_ = 0.0;
    std::fill(ringM_.begin(), ringM_.end(), 0.0f);
    std::fill(ringE_.begin(), ringE_.end(), 0.0f);
    ringPos_ = 0; phaseCount_ = 0;
    hpState_ = 0.0; hpPrev_ = 0.0;
    gate_ = 0.0; deL_ = deR_ = 0.0;
}

std::size_t OirtStereo::process(const float* mpx, std::size_t n, float* left, float* right) {
    const std::size_t taps = taps_.size();
    std::size_t outCount = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double x = mpx[i];
        double e = 0.0;
        if (stereoCapable_) {
            const double c = std::cos(theta_), s = std::sin(theta_);
            const double mi = 2.0 * x * c, mq = -2.0 * x * s;
            e = mi;
            double yi = mi, yq = mq;
            for (int k = 0; k < 3; ++k) { yi = ci_[k].push(yi); yq = cq_[k].push(yq); }
            // 3 cascaded sliding sums, sampled once per cicLen_.
            if (++cicCount_ >= cicLen_) {
                cicCount_ = 0;
                iMean_ += meanAlpha_ * (yi - iMean_);
                qMean_ += meanAlpha_ * (yq - qMean_);
                iSlow_ += 0.1 * meanAlpha_ * (yi - iSlow_);
                const double ref = std::clamp(iSlow_, 0.05, 0.5);
                const double err = std::clamp(yq / ref, -1.5, 1.5);
                theta_ += kp_ * err;
                freq_ = std::clamp(freq_ + ki_ * err / static_cast<double>(cicLen_), -freqMax_, freqMax_);
                const double mag = std::hypot(iMean_, qMean_);
                carrierLevel_ = static_cast<float>(mag / kNominalCarrier);
                const bool cond = mag > 0.3 * kNominalCarrier && iMean_ > 0.0 &&
                                  std::fabs(qMean_) < 0.5 * iMean_;
                lampHold_ = cond ? std::min(lampHold_ + 1, lampHoldMax_) : std::max(lampHold_ - 1, 0);
                if (lampHold_ >= lampHoldMax_) { lamp_ = true; }
                else if (lampHold_ == 0) { lamp_ = false; }
            }
            theta_ += w0_ + freq_;
            theta_ = std::fmod(theta_, kTwoPi);
            if (theta_ < 0.0) { theta_ += kTwoPi; }
            if (restore_) {
                const double y = kb0_ * e + kb1_ * kx1_ - ka1_ * ky1_;
                kx1_ = e; ky1_ = y; e = y;
            }
        }
        // Doubled ring of the last `taps` samples of M and envelope.
        ringM_[ringPos_] = ringM_[ringPos_ + taps] = static_cast<float>(x);
        ringE_[ringPos_] = ringE_[ringPos_ + taps] = static_cast<float>(e);
        const std::size_t start = (ringPos_ + 1) % taps;
        ringPos_ = (ringPos_ + 1) % taps;
        if (++phaseCount_ < decim_) { continue; }
        phaseCount_ = 0;

        double m = 0.0, env = 0.0;
        const float* pm = &ringM_[start];
        const float* pe = &ringE_[start];
        for (std::size_t k = 0; k < taps; ++k) {
            m += static_cast<double>(taps_[k]) * pm[k];
            env += static_cast<double>(taps_[k]) * pe[k];
        }
        double sig = 0.0;
        if (stereoCapable_) {
            // restored envelope = 1 + S_pre; the nominal 1 is removed up front,
            // then a 2 Hz high-pass takes any residual offset.
            const double raw = restore_ ? env - 1.0 : env - kNominalCarrier;
            hpState_ = hpA_ * (hpState_ + raw - hpPrev_);
            hpPrev_ = raw;
            sig = hpState_;
        }
        const double target = (stereoCapable_ && lamp_ && !forceMono_) ? 1.0 : 0.0;
        if (gate_ < target) { gate_ = std::min(target, gate_ + gateStep_); }
        else if (gate_ > target) { gate_ = std::max(target, gate_ - gateStep_); }
        const double scale = 1.0 + gate_ * (1.0 / kNominalSScale - 1.0);
        double l = scale * (m + gate_ * sig);
        double r = scale * (m - gate_ * sig);
        if (dePole_ > 0.0) {
            deL_ = (1.0 - dePole_) * l + dePole_ * deL_;
            deR_ = (1.0 - dePole_) * r + dePole_ * deR_;
            l = deL_; r = deR_;
        }
        left[outCount] = static_cast<float>(l);
        right[outCount] = static_cast<float>(r);
        ++outCount;
    }
    return outCount;
}

}  // namespace cascade::dsp
