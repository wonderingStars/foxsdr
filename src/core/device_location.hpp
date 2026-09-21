// device_location.hpp - the receiver's position from the DEVICE's own
// location service, rather than from a GPS receiver on a serial port.
//
// THE REQUEST THIS EXISTS FOR (owner, 2026-09-21): "can you look at using the
// android tablets GPS in the software". A tablet has a GNSS chip in it and an
// operating system that already knows how to talk to it. What FoxSDR offered
// on that tablet instead was core/gps_reader.hpp's control - a box asking for
// a port name, with "COM3" as its hint, and a baud rate beside it. There is no
// serial port on a tablet and no puck to plug into it, so the one surface that
// could have set the receiver position on the platform that actually knows it
// was the one surface that could not work.
//
// WHAT THIS IS, AND WHAT IT DELIBERATELY IS NOT. It is the same feature as the
// GPS reader, with the same shape and the same promises: ask once, take ONE
// position, hand it to AppWindow::applyReceiverPosition, stop. It is not a
// tracker and it does not follow the device around. The antenna does not move
// while you are listening to it, and a location request left running is a
// GNSS chip left powered on somebody's battery.
//
// WHY IT IS NOT THE GPS READER WITH A DIFFERENT SOURCE. GpsReader's ByteSource
// seam would take NMEA from anywhere, and Android can even produce NMEA
// (OnNmeaMessageListener). It was still the wrong way round: that listener
// only delivers while a location request is already running, so it costs the
// whole permission-and-request sequence anyway and then adds a text protocol
// in the middle of it. The platform hands over a position with an accuracy in
// metres and the provider that produced it; re-encoding that as sentences to
// parse them back would lose the accuracy figure, which is the one number that
// tells a user whether the fix is worth taking.
//
// THE TWO PROVIDERS, AND WHY THE DIFFERENCE IS SHOWN. Android will answer from
// the satellites (GPS_PROVIDER) or from the network - wi-fi and cell (a fused
// or NETWORK_PROVIDER fix). A network fix arrives in a second indoors and can
// be a kilometre out; a satellite fix needs sky and is good to a few metres.
// For a receiver position, a kilometre matters: it is the difference between a
// satellite pass predicted correctly and one that is minutes off. So both are
// accepted - an unusable position is worse than a rough one - and the status
// line always says WHICH, with the accuracy, so the user can decide whether to
// go outside and press it again.
//
// PRIVACY - THE SAME RULE AS THE SERIAL READER, AND IT IS NOT NEGOTIABLE.
// Diagnostic log lines ride inside crash reports that are uploaded, and
// PRIVACY.md promises that a position is never sent. So nothing in this file
// or its platform half logs a latitude, a longitude or an altitude - not at
// debug level, not truncated, not "just the first digits". Accuracy in metres,
// satellite counts, provider names, elapsed time and error text are the whole
// vocabulary. tests/test_device_location.cpp feeds a fix at a distinctive
// position and then searches the entire log ring for those digits; a line that
// carried any of them fails the suite.
//
// THREADS. The platform's answer arrives on whatever thread the platform
// chooses (on Android, a JNI call from the main looper). Everything it writes
// goes under `mutex_`, and the GUI reads one consistent copy through status().
// The fix is taken exactly ONCE, through takeFix(), so a frame loop polling at
// 60 Hz cannot re-apply a position sixty times a second and reset the coverage
// map each time - the same contract GpsReader::takeFix has, for the same
// reason.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_DEVICE_LOCATION_HPP
#define CASCADE_CORE_DEVICE_LOCATION_HPP

#include <functional>
#include <mutex>
#include <string>

namespace cascade::core {

// True on a platform that HAS a location service of its own. Android today.
// A constexpr function rather than an #ifdef at the call site so the call site
// reads the same everywhere, exactly as background_exit.hpp does it.
inline constexpr bool platformHasDeviceLocation() {
#if defined(__ANDROID__)
    return true;
#else
    return false;
#endif
}

class DeviceLocation {
public:
    // How long to wait before giving up. A network fix lands in a second or
    // two; a cold GNSS chip indoors may never fix at all, which is what the
    // timeout is for. 60 s matches the serial reader's, and the user can stop
    // at any moment.
    static constexpr double kDefaultTimeoutS = 60.0;

    enum class State {
        Idle,         // never started, or stopped without a fix
        Unavailable,  // no location service on this build or this device
        Denied,       // the user said no to the permission, or it is blocked
        Listening,    // asked, waiting for a position
        Fixed,        // a position arrived; the request is already cancelled
        TimedOut,     // the wait passed with nothing acceptable
        Failed,       // the platform refused the request; see error
    };

    // Which of the two the platform answered from. Shown, never guessed at:
    // the difference is metres against kilometres.
    enum class Provider { Unknown, Satellites, Network };

    struct Fix {
        double latDeg = 0.0;
        double lonDeg = 0.0;
        // The platform's own estimate, in metres, of the radius it is 68%
        // confident the true position lies within. Negative when the platform
        // did not supply one, which some network fixes do not.
        double accuracyM = -1.0;
        Provider provider = Provider::Unknown;
    };

    struct Status {
        State state = State::Idle;
        double elapsedS = 0.0;
        // Satellites used in the last fix the platform reported, or -1 before
        // it has said. Only a satellite fix has one.
        int satellites = -1;
        std::string error;  // Failed/Denied: why, as a complete predicate.
        Fix fix;            // meaningful only when state == Fixed
    };

    // WHAT THE PLATFORM HALF MUST DO. start asks for a position and returns
    // false with `error` filled if it cannot even ask (no service, no class,
    // no activity); stop cancels an outstanding request and must be safe to
    // call when there is none. Injected rather than called directly so the
    // whole state machine above is exercised by tests on a machine with no
    // location service in sight - the same seam GpsReader::setOpenerForTest
    // provides, for the same reason.
    struct Platform {
        std::function<bool(double timeoutS, std::string& error)> start;
        std::function<void()> stop;
    };

    DeviceLocation();
    ~DeviceLocation();
    DeviceLocation(const DeviceLocation&) = delete;
    DeviceLocation& operator=(const DeviceLocation&) = delete;

    // Replaces the platform half. An empty Platform restores the real one
    // (which is a no-op answering Unavailable off Android). Takes effect on
    // the next start().
    void setPlatform(Platform platform);

    // Asks the platform for a position. Returns at once: a refusal surfaces
    // as State::Failed or State::Unavailable with `error` set, never as a
    // return value, so the GUI has ONE place to look. Starting while a
    // request is outstanding cancels that one first.
    void start(double timeoutS = kDefaultTimeoutS);

    // Cancels an outstanding request. The state afterwards is whatever was
    // reached: a fix that landed first is KEPT (so a stop pressed in the same
    // frame a fix arrives does not throw the position away), anything else
    // returns to Idle. Safe when nothing is running, and from the destructor.
    void stop();

    // True while a request is outstanding - what the GUI shows its Stop key
    // against.
    bool listening() const;

    // One consistent copy of everything the GUI draws.
    Status status() const;

    // Takes the fix ONCE. False when there is none to take or one has already
    // been taken. See the header note on why this is not a getter.
    bool takeFix(Fix& out);

    // Forgets a terminal state so the control reads clean again - pressed
    // when the user changes something, exactly as the serial reader's is.
    void clearResult();

    // --- What the platform half calls when the answer arrives --------------
    //
    // All four are safe to call from any thread and at any time, including
    // after a stop() (a late fix from a cancelled request is dropped rather
    // than applied, which is why they take the generation the request was
    // started with... except that the platform does not know it, so instead
    // they are simply ignored unless a request is outstanding).

    // A position. Rejected - and the wait continues - unless it is finite, on
    // the globe and not exactly (0,0): the same acceptance rule the serial
    // reader applies, for the same reason (a device that honestly reports the
    // origin has not fixed).
    void onFix(double latDeg, double lonDeg, double accuracyM, Provider provider);

    // The platform could not do it. `reason` is shown as given, so it must
    // read as a complete predicate and must never contain a coordinate.
    void onFailed(const std::string& reason);

    // The permission was refused, or is blocked by policy.
    void onDenied(const std::string& reason);

    // Satellites used in the current fix attempt, as the platform counts
    // them. Advisory: it moves the status line, never the acceptance.
    void onSatellites(int used);

    // Called by the GUI once a frame while a request is outstanding, with the
    // frame clock, so the elapsed figure and the timeout are driven by the
    // same clock the rest of the interface uses and no thread has to sleep.
    void tick(double nowS);

private:
    // Marks the outstanding request cancelled and hands back the platform's
    // stop function for the CALLER to run once mutex_ is released. See the
    // locking rule at the top of device_location.cpp: no platform function is
    // ever called with the lock held, because the platform's stop can deliver
    // a final callback on the calling thread and that callback takes mutex_.
    // Returns an empty function when there was nothing outstanding.
    std::function<void()> takeCancelLocked();

    mutable std::mutex mutex_;
    Platform platform_;
    Status status_;
    bool outstanding_ = false;  // a platform request is live
    bool fixTaken_ = true;      // nothing to take until one arrives
    double startedAtS_ = 0.0;
    double timeoutS_ = kDefaultTimeoutS;
    bool haveClock_ = false;
};

// The one-line status the GUI draws, and the only place this wording lives.
//
// It NEVER composes a coordinate - see the privacy note at the top of this
// file. What it does say, on a fix, is the provider and the accuracy, because
// "position set" alone cannot tell a user that the position they just took is
// two kilometres from where they are standing.
std::string deviceLocationStatusLine(const DeviceLocation::Status& s);

// The platform half, implemented per platform. Off Android this answers
// Unavailable and touches nothing.
DeviceLocation::Platform makePlatformLocation(DeviceLocation& target);

// Binds the Java half and remembers the activity, exactly as
// usb::androidUsbInit does, and for the same reason: until RegisterNatives has
// run, every call Java makes into native code is an UnsatisfiedLinkError. A
// no-op off Android. Safe to call when there is no VM (a test binary).
void androidLocationInit(void* javaVm, void* activityObject);

}  // namespace cascade::core

#endif  // CASCADE_CORE_DEVICE_LOCATION_HPP
