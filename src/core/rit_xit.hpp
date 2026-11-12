// RIT / XIT: the receive and transmit clarifier - an offset from the VFO that moves what you HEAR (RIT) or what you would SEND (XIT) without moving the VFO.
//
// RIT shifts the receive frequency by rit hertz while the VFO, and so the dial and the other station's idea of where you are, stay where they are; XIT does
// the same for the transmit frequency. Each has its own on/off and its own offset, within +-maxHz (default 9999 Hz, a value the operator can change up to
// 20000). This is the model: the offsets, their limits, the arithmetic, and the positions to DRAW on the spectrum. It does not tune a radio and it does not
// transmit: txHz() is a number for the display and for the CAT answer, nothing here reaches a transmit path.
//
// rxHz(vfoHz)  = vfoHz + (RIT on ? rit : 0)       txHz(vfoHz) = vfoHz + (XIT on ? xit : 0)
// marks(vfoHz) = where the RIT and XIT markers belong on the spectrum (the receive marker is drawn at rxHz when RIT is on, the transmit marker at txHz when
// XIT is on), so the operator sees the clarifier as a shift of a line, not as a number alone.
// setRit / setXit / stepRit / stepXit CLAMP to the limit and say whether they clamped. clearOnRetuneHz (default 0 = never): a VFO move larger than that clears
// both offsets, the usual behaviour of a clarifier that should not follow you to another station; the move is reported with onVfoMoved().
// catMaxRit() / catMaxXit() are what the CAT answer should report instead of 0 (src/net/cat_protocol.cpp:192-193 reports 0 today because no clarifier existed).
// formatOffset(hz) gives "+350 Hz", "-1.25 kHz", "0 Hz".
//
// Offsets are never saved: a clarifier that comes back set after a restart would put the receiver off the station it was on.
//
// THREADS: one thread. Pure.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace cascade::core::tuning {

struct ClarifierMarks {
    bool showRx = false;
    double rxHz = 0.0;
    bool showTx = false;
    double txHz = 0.0;
};

class RitXit {
public:
    static constexpr double kMaxLimitHz = 20000.0;

    // false (limit unchanged) for a limit outside 1..20000 Hz or not finite. A smaller limit re-clamps the offsets.
    bool setMaxHz(double maxHz) {
        if (!std::isfinite(maxHz) || maxHz < 1.0 || maxHz > kMaxLimitHz) { return false; }
        maxHz_ = maxHz;
        rit_ = clamp(rit_);
        xit_ = clamp(xit_);
        return true;
    }
    double maxHz() const { return maxHz_; }
    int catMaxRit() const { return static_cast<int>(maxHz_); }
    int catMaxXit() const { return static_cast<int>(maxHz_); }

    void setRitOn(bool on) { ritOn_ = on; }
    void setXitOn(bool on) { xitOn_ = on; }
    bool ritOn() const { return ritOn_; }
    bool xitOn() const { return xitOn_; }
    double rit() const { return rit_; }
    double xit() const { return xit_; }

    // Returns true when the value was taken as given, false when it was clamped or refused (NaN is refused and changes nothing).
    bool setRit(double hz) { return set(rit_, hz); }
    bool setXit(double hz) { return set(xit_, hz); }
    bool stepRit(double deltaHz) { return std::isfinite(deltaHz) ? set(rit_, rit_ + deltaHz) : false; }
    bool stepXit(double deltaHz) { return std::isfinite(deltaHz) ? set(xit_, xit_ + deltaHz) : false; }
    void clearRit() { rit_ = 0.0; }
    void clearXit() { xit_ = 0.0; }
    void clear() { rit_ = xit_ = 0.0; }

    double rxHz(double vfoHz) const { return vfoHz + (ritOn_ ? rit_ : 0.0); }
    double txHz(double vfoHz) const { return vfoHz + (xitOn_ ? xit_ : 0.0); }
    // The shift to give the receive oscillator (what rxHz adds to the VFO), 0 when RIT is off.
    double receiveShiftHz() const { return ritOn_ ? rit_ : 0.0; }

    ClarifierMarks marks(double vfoHz) const {
        ClarifierMarks m;
        m.showRx = ritOn_ && rit_ != 0.0;
        m.rxHz = rxHz(vfoHz);
        m.showTx = xitOn_ && xit_ != 0.0;
        m.txHz = txHz(vfoHz);
        return m;
    }

    void setClearOnRetuneHz(double hz) { clearOnRetuneHz_ = (std::isfinite(hz) && hz > 0.0) ? hz : 0.0; }
    double clearOnRetuneHz() const { return clearOnRetuneHz_; }
    // The VFO moved from oldHz to newHz: clears both offsets when the move is larger than the limit. Returns whether it cleared.
    bool onVfoMoved(double oldHz, double newHz) {
        if (clearOnRetuneHz_ <= 0.0 || !std::isfinite(oldHz) || !std::isfinite(newHz)) { return false; }
        if (std::fabs(newHz - oldHz) <= clearOnRetuneHz_) { return false; }
        const bool had = rit_ != 0.0 || xit_ != 0.0;
        clear();
        return had;
    }

    static std::string formatOffset(double hz) {
        if (!std::isfinite(hz)) { return "0 Hz"; }
        const double r = std::round(hz);
        char buf[32];
        if (std::fabs(r) < 1000.0) {
            std::snprintf(buf, sizeof buf, "%s%d Hz", r > 0 ? "+" : "", static_cast<int>(r));
        } else {
            std::snprintf(buf, sizeof buf, "%s%.2f kHz", r > 0 ? "+" : "", r / 1000.0);
        }
        return buf;
    }

private:
    double clamp(double hz) const { return std::max(-maxHz_, std::min(maxHz_, hz)); }
    bool set(double& dst, double hz) {
        if (!std::isfinite(hz)) { return false; }
        const double c = clamp(hz);
        dst = c;
        return c == hz;
    }
    double maxHz_ = 9999.0;
    double rit_ = 0.0, xit_ = 0.0;
    bool ritOn_ = false, xitOn_ = false;
    double clearOnRetuneHz_ = 0.0;
};

}  // namespace cascade::core::tuning
