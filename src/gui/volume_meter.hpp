// volume_meter.hpp - the needle that shows what is coming out of the speakers.
//
// WHAT IT REPLACED, AND WHY. The right-hand meter on the tuning bar read FRAME
// TIME: the GUI's own draw time against a 16.7 ms budget. It was an honest
// measurement of a real thing and it was the wrong thing to give a permanent
// meter to - on a receiver, the quantity worth watching at a glance is the
// AUDIO, not the renderer. The owner asked for the swap (2026-09-21). Frame
// time is still measured and still reported; it just does not own a meter.
//
// WHAT IT SHOWS: the level of the FINISHED audio - the pipeline's scopeAudio
// tap, which is taken below the plugin audio replacement and below the hard
// mute, so a decoder playing through the host's audio capability moves this
// needle and the hiss it replaced does not - multiplied by the volume control
// and forced to zero by mute. A meter that ignored the volume knob would sit
// half way up a silent set, which is the one reading nobody can argue with
// and everybody would misread.
//
// THE SCALE IS dB, NOT AMPLITUDE. A linear needle spends nine tenths of its
// arc on the top 20 dB and reads as "nothing" for ordinary speech; kFloorDb
// (-60) to full scale is the range a meter on a radio is expected to cover.
//
// THE BALLISTICS ARE ASYMMETRIC, which is what makes it readable: a meter that
// followed the signal exactly would be a blur at syllable rate. It rises
// instantly, so a transient is never under-read, and falls with a time
// constant - the shape every peak-programme meter has used since they were
// mechanical.
//
// PURE, so tests/test_volume_meter.cpp can drive all of it without audio, a
// device or an ImGui context.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_VOLUME_METER_HPP
#define CASCADE_GUI_VOLUME_METER_HPP

#include <cmath>
#include <cstddef>
#include <cstdio>

namespace cascade::gui {

// The bottom of the scale. Below this the needle sits on the stop.
inline constexpr float kVolumeFloorDb = -60.0f;

// How long the needle takes to fall by 1/e. Slow enough to read a syllable,
// fast enough that a level change does not look stuck.
inline constexpr float kVolumeFallS = 0.35f;

// Peak magnitude of a block of mono audio. Peak rather than RMS because this
// is a headroom instrument: what a user needs from it is "am I clipping" and
// "is there anything there", and RMS answers neither at a glance.
inline float audioPeak(const float* samples, std::size_t n) {
    if (samples == nullptr || n == 0) { return 0.0f; }
    float peak = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        const float a = std::fabs(samples[i]);
        if (a > peak) { peak = a; }
    }
    // NaN in, zero out: a needle driven by a NaN vanishes, which reads as a
    // broken meter rather than as a broken signal.
    return (peak == peak) ? peak : 0.0f;
}

// What the speakers actually get: the tap is taken before the volume control,
// and the hard mute is applied below it.
inline float audibleAmplitude(float tapPeak, float volume, bool muted) {
    if (muted) { return 0.0f; }
    if (!(tapPeak > 0.0f)) { return 0.0f; }  // NaN-safe
    if (!(volume > 0.0f)) { return 0.0f; }
    return tapPeak * volume;
}

// Amplitude (1.0 = full scale) to needle position, 0..1 over kVolumeFloorDb.
inline float meterFraction(float amplitude) {
    if (!(amplitude > 0.0f)) { return 0.0f; }  // silence, and NaN-safe
    const float db = 20.0f * std::log10(amplitude);
    if (db <= kVolumeFloorDb) { return 0.0f; }
    if (db >= 0.0f) { return 1.0f; }  // and anything above full scale pins
    return (db - kVolumeFloorDb) / (0.0f - kVolumeFloorDb);
}

// Rise instantly, fall with kVolumeFallS. `dt` is the frame time in seconds;
// a non-finite or negative dt leaves the needle where it is rather than
// teleporting it, because the one place that happens is the first frame.
inline float meterBallistics(float previous, float target, float dt) {
    if (!(previous >= 0.0f)) { previous = 0.0f; }
    if (target >= previous) { return target; }
    if (!(dt > 0.0f) || !(dt < 1.0f)) { return previous; }
    const float k = std::exp(-dt / kVolumeFallS);
    return target + (previous - target) * k;
}

// The dB reading under the face. `amplitude` is the audible amplitude.
// Silence prints the floor rather than "-inf", which is what a meter's scale
// says and what a user reads as "nothing".
inline void formatVolumeText(char* out, std::size_t cap, float amplitude, bool have) {
    if (out == nullptr || cap == 0) { return; }
    if (!have) {
        std::snprintf(out, cap, "--");
        return;
    }
    if (!(amplitude > 0.0f)) {
        std::snprintf(out, cap, "%.0f dB", static_cast<double>(kVolumeFloorDb));
        return;
    }
    const float db = 20.0f * std::log10(amplitude);
    if (db <= kVolumeFloorDb) {
        std::snprintf(out, cap, "%.0f dB", static_cast<double>(kVolumeFloorDb));
        return;
    }
    std::snprintf(out, cap, "%.1f dB", static_cast<double>(db));
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_VOLUME_METER_HPP
