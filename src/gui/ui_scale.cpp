// ui_scale.cpp - see the header for the model. Pure arithmetic; the only
// global state is the two inputs (choice, monitor DPI) and their product.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/ui_scale.hpp"

#include <cmath>
#include <cstdlib>

namespace cascade::gui::uiscale {

namespace {
std::string gChoice = kAuto;
unsigned gMonitorDpi = 96;
float gFactor = 1.0f;
bool gChanged = false;

// Recomputes gFactor from the two inputs and marks the change, if any. The
// one place either setter writes gFactor, so the two can never disagree about
// which value is "current".
void recompute() {
    const float f = effectiveScale(gChoice, gMonitorDpi);
    if (f != gFactor) {
        gFactor = f;
        gChanged = true;
    }
}
}  // namespace

int nearestStep(int rawPercent) {
    int best = kSteps[0];
    int bestDiff = std::abs(rawPercent - kSteps[0]);
    for (int i = 1; i < kStepCount; ++i) {
        const int diff = std::abs(rawPercent - kSteps[i]);
        if (diff < bestDiff) {
            best = kSteps[i];
            bestDiff = diff;
        }
    }
    return best;
}

int dpiToPercent(unsigned dpi) {
    if (dpi == 0) { dpi = 96; }
    return static_cast<int>(std::lround(100.0 * static_cast<double>(dpi) / 96.0));
}

std::string normalizeChoice(const std::string& raw) {
    if (raw == kAuto) { return raw; }
    for (int i = 0; i < kStepCount; ++i) {
        if (raw == std::to_string(kSteps[i])) { return raw; }
    }
    return kAuto;
}

float effectiveScale(const std::string& choice, unsigned monitorDpi) {
    const std::string c = normalizeChoice(choice);
    if (c == kAuto) {
        if (monitorDpi == 0) { monitorDpi = 96; }
        // CONTINUOUS, not snapped to a step: a monitor at, say, 110% (some
        // laptops offer odd percentages) is followed exactly, and a 96 dpi
        // monitor is exactly 1.0f — 96.0f/96.0f is bit-exact in IEEE-754,
        // never 0.999999-something, which is what the S=1 guarantee rests on.
        return static_cast<float>(monitorDpi) / 96.0f;
    }
    // c is normalised, so this is always one of kSteps' own decimal spellings
    // and std::stoi cannot throw here.
    return static_cast<float>(std::stoi(c)) / 100.0f;
}

void setChoice(const std::string& choice) {
    const std::string c = normalizeChoice(choice);
    if (c != gChoice) {
        gChoice = c;
        recompute();
    }
}

void setMonitorDpi(unsigned dpi) {
    if (dpi == 0) { dpi = 96; }
    if (dpi != gMonitorDpi) {
        gMonitorDpi = dpi;
        recompute();
    }
}

float factor() { return gFactor; }
const std::string& choice() { return gChoice; }
unsigned monitorDpi() { return gMonitorDpi; }

bool consumeChanged() {
    const bool c = gChanged;
    gChanged = false;
    return c;
}

float px(float v) { return v * gFactor; }

}  // namespace cascade::gui::uiscale
