// Implementation of core/device_location.hpp - the portable half. The
// platform half (the JNI call into android/app/src/main/java/com/foxsdr/app/
// Loc.java) is in device_location_android.cpp; everything decided here is
// decided the same way on every platform and is covered by
// tests/test_device_location.cpp on the machine the tests run on.
//
// THE LOCKING RULE, IN ONE PLACE. The platform's stop() can deliver a final
// callback on the calling thread, and those callbacks take mutex_ - so no
// platform function is ever called with mutex_ held. Every path that needs to
// cancel therefore TAKES the stop function out under the lock
// (takeCancelLocked), finishes what it was doing to the status, releases the
// lock, and only then calls it. There is no unlock-in-the-middle anywhere in
// this file; that shape works by luck with a unique_lock and not at all with a
// lock_guard, and the luck runs out the first time a callback throws.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/device_location.hpp"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <utility>

#include "core/diag_log.hpp"
#include "gui/scope_view.hpp"  // receiverPositionAcceptable - ONE rule at every door

namespace cascade::core {

namespace {

// A short formatted string, the same helper gps_reader.cpp uses: everything
// reaching it is a count, an accuracy or an OS message, and it truncates
// rather than trusting.
std::string fmt(const char* f, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, f);
    const int n = std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    if (n < 0) { return std::string(); }
    buf[sizeof buf - 1] = '\0';
    return std::string(buf);
}

const char* providerName(DeviceLocation::Provider p) {
    switch (p) {
        case DeviceLocation::Provider::Satellites:
            return "the satellites";
        case DeviceLocation::Provider::Network:
            return "the network";
        case DeviceLocation::Provider::Unknown:
            break;
    }
    return "the device";
}

}  // namespace

DeviceLocation::DeviceLocation() { platform_ = makePlatformLocation(*this); }

DeviceLocation::~DeviceLocation() { stop(); }

void DeviceLocation::setPlatform(Platform platform) {
    const std::lock_guard<std::mutex> lock(mutex_);
    // An empty start() means "give me the real one back", which is what a
    // test does when it is finished with its fake.
    platform_ = platform.start ? std::move(platform) : makePlatformLocation(*this);
}

std::function<void()> DeviceLocation::takeCancelLocked() {
    if (!outstanding_) { return {}; }
    outstanding_ = false;
    return platform_.stop;
}

void DeviceLocation::start(double timeoutS) {
    std::function<void()> cancelPrevious;
    std::function<bool(double, std::string&)> startFn;
    double t = kDefaultTimeoutS;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        cancelPrevious = takeCancelLocked();
        status_ = Status();
        fixTaken_ = true;
        timeoutS_ = (timeoutS > 0.0) ? timeoutS : kDefaultTimeoutS;
        startedAtS_ = 0.0;
        haveClock_ = false;
        t = timeoutS_;
        startFn = platform_.start;
        if (startFn) {
            status_.state = State::Listening;
            outstanding_ = true;
        } else {
            status_.state = State::Unavailable;
            status_.error = "this build has no location service to ask";
        }
    }
    if (cancelPrevious) { cancelPrevious(); }
    if (!startFn) {
        diagLogf("location: no platform location service on this build");
        return;
    }

    // CALLED WITHOUT THE LOCK, and that matters here more than anywhere: on
    // Android the platform's start can answer IMMEDIATELY with a position the
    // system already had, on this very thread, and that answer arrives
    // through onFix - which takes mutex_.
    std::string error;
    const bool asked = startFn(t, error);
    if (asked) {
        diagLogf("location: asked the device for a position (timeout %.0f s)", t);
        return;
    }
    // A refusal to even ask. Not a timeout, and not a denial unless the
    // platform said so through onDenied, which has its own door.
    std::string said;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        // An immediate fix or denial during startFn wins: it already said
        // something truer than "refused".
        if (!outstanding_) { return; }
        outstanding_ = false;
        status_.state = State::Failed;
        status_.error = error.empty() ? "the location service refused the request" : error;
        said = status_.error;
    }
    diagLogf("location: the request was refused - %s", said.c_str());
}

void DeviceLocation::stop() {
    std::function<void()> cancel;
    bool said = false;
    double elapsed = 0.0;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        cancel = takeCancelLocked();
        // A fix that landed before the stop is KEPT - a Stop pressed in the
        // same frame a position arrives must not throw it away. Anything
        // else returns to Idle: a listen the user cut short has nothing to
        // report.
        if (cancel && status_.state == State::Listening) {
            status_.state = State::Idle;
            status_.error.clear();
            elapsed = status_.elapsedS;
            said = true;
        }
    }
    if (cancel) { cancel(); }
    if (said) { diagLogf("location: the request was stopped after %.0f s", elapsed); }
}

bool DeviceLocation::listening() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return outstanding_;
}

DeviceLocation::Status DeviceLocation::status() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

bool DeviceLocation::takeFix(Fix& out) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (fixTaken_ || status_.state != State::Fixed) { return false; }
    out = status_.fix;
    fixTaken_ = true;
    return true;
}

void DeviceLocation::clearResult() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (outstanding_) { return; }  // a live request is not a result to clear
    status_ = Status();
    fixTaken_ = true;
}

void DeviceLocation::onFix(double latDeg, double lonDeg, double accuracyM, Provider provider) {
    std::function<void()> cancel;
    double accuracy = -1.0;
    double elapsed = 0.0;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!outstanding_) {
            // A late answer from a request already cancelled. Dropped, and
            // said so: a position applied after the user pressed Stop is a
            // position they did not ask for.
            diagLogf("location: a position arrived after the request ended; ignored");
            return;
        }
        // THE SAME ACCEPTANCE RULE AS EVERY OTHER DOOR (gui/scope_view.hpp).
        // A platform that answers the origin has not fixed, and the wait
        // continues rather than setting the receiver to the Gulf of Guinea.
        if (!cascade::gui::receiverPositionAcceptable(latDeg, lonDeg)) {
            diagLogf("location: a position that is not on the globe was ignored");
            return;
        }
        cancel = takeCancelLocked();
        status_.state = State::Fixed;
        status_.fix.latDeg = latDeg;
        status_.fix.lonDeg = lonDeg;
        status_.fix.accuracyM = (std::isfinite(accuracyM) && accuracyM >= 0.0) ? accuracyM : -1.0;
        status_.fix.provider = provider;
        status_.error.clear();
        fixTaken_ = false;
        accuracy = status_.fix.accuracyM;
        elapsed = status_.elapsedS;
    }
    if (cancel) { cancel(); }
    // NO COORDINATE IN THIS LINE, and none in any other in this file.
    if (accuracy >= 0.0) {
        diagLogf("location: a position arrived from %s, accurate to %.0f m, after %.0f s",
                 providerName(provider), accuracy, elapsed);
    } else {
        diagLogf("location: a position arrived from %s after %.0f s (no accuracy given)",
                 providerName(provider), elapsed);
    }
}

void DeviceLocation::onFailed(const std::string& reason) {
    std::function<void()> cancel;
    std::string said;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!outstanding_) { return; }
        cancel = takeCancelLocked();
        status_.state = State::Failed;
        status_.error = reason.empty() ? "the location service stopped answering" : reason;
        said = status_.error;
    }
    if (cancel) { cancel(); }
    diagLogf("location: failed - %s", said.c_str());
}

void DeviceLocation::onDenied(const std::string& reason) {
    std::function<void()> cancel;
    std::string said;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        // A denial is worth recording even when no request is outstanding: it
        // is the answer to the only question the control can ask next time.
        cancel = takeCancelLocked();
        status_.state = State::Denied;
        status_.error = reason.empty() ? "permission to use the location was refused" : reason;
        said = status_.error;
        fixTaken_ = true;
    }
    if (cancel) { cancel(); }
    diagLogf("location: denied - %s", said.c_str());
}

void DeviceLocation::onSatellites(int used) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (used >= 0) { status_.satellites = used; }
}

void DeviceLocation::tick(double nowS) {
    std::function<void()> cancel;
    double elapsed = 0.0;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!outstanding_) { return; }
        if (!haveClock_) {
            haveClock_ = true;
            startedAtS_ = nowS;
        }
        const double since = nowS - startedAtS_;
        // A clock that went backwards (a frame clock reset, a test) must not
        // freeze the timeout, and must not print a negative age either.
        status_.elapsedS = (since > 0.0) ? since : 0.0;
        if (status_.elapsedS < timeoutS_) { return; }
        cancel = takeCancelLocked();
        status_.state = State::TimedOut;
        elapsed = status_.elapsedS;
    }
    if (cancel) { cancel(); }
    diagLogf("location: no position within %.0f s", elapsed);
}

std::string deviceLocationStatusLine(const DeviceLocation::Status& s) {
    using State = DeviceLocation::State;
    switch (s.state) {
        case State::Idle:
            return std::string();
        case State::Unavailable:
            return s.error.empty() ? std::string("there is no location service on this device")
                                   : s.error;
        case State::Denied:
            // Said as the thing the user can act on, not as an error code.
            return s.error + " - you can allow it in Android's own Settings, under this "
                             "application's permissions.";
        case State::Listening: {
            std::string line = "Asking the device for a position";
            if (s.satellites > 0) { line += fmt(" - %d satellites so far", s.satellites); }
            return line + fmt(" (%.0f s)", s.elapsedS);
        }
        case State::Fixed: {
            // THE PROVIDER AND THE ACCURACY, ALWAYS. A network fix can be a
            // kilometre out, and the user is the only one who can decide
            // whether that is good enough for what they are pointing at.
            std::string line = fmt("Position set from %s", providerName(s.fix.provider));
            if (s.fix.accuracyM >= 0.0) {
                if (s.fix.accuracyM >= 1000.0) {
                    line += fmt(", accurate to about %.0f km", s.fix.accuracyM / 1000.0);
                } else {
                    line += fmt(", accurate to about %.0f m", s.fix.accuracyM);
                }
            }
            if (s.fix.provider == DeviceLocation::Provider::Network) {
                line += " - go outdoors and press it again for a satellite fix";
            }
            return line;
        }
        case State::TimedOut:
            return fmt("No position within %.0f s - a tablet indoors often cannot see the "
                       "satellites; try again by a window or outside.",
                       s.elapsedS);
        case State::Failed:
            return s.error;
    }
    return std::string();
}

}  // namespace cascade::core
