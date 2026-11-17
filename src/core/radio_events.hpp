// radio_events.hpp - what a DRIVER says about a radio after it opened: the radio that went away, the stream that stalls, the setting the radio refuses, the
// vendor call that failed and the service that restarted (radio reports, app side, part 2; docs/multi-radio-records/radio-reports-app-2.md).
//
// WHY THIS IS ONE SMALL HEADER. The owner's rule is "I want to know every fail of every radio, not an accumulation". Part 1 filed the failure of every OPEN; what
// happens after it opened (an unplug, a stalled stream, a refused gain, a swallowed vendor exception) was only a log line. There are about two hundred such places in
// the drivers, so each driver makes ONE call to one of the helpers below at the place it already notices the fault; nothing here is a hand-written message per site.
//
// WHICH RADIO. A driver does not know its radio's model and serial (that is the engine's knowledge, read from the device arguments). The engine REGISTERS the
// source object it opened (registerSource) and a driver passes `this`; a call from a source nobody registered falls back to the one open radio of that driver. A call
// that finds no open radio is dropped and counted (unattributedCount): the failure of an OPEN is a radio_fail, and a made-up radio would be a phantom on the site.
// FOLDING: a refusal repeated within five seconds, or a vendor fault within ten, is counted in the radio's own `dropped` (the field every sent event of the radio
// carries) and not sent again.
//
// NEVER SENT. Every text goes through the reporter's own scrub (serial, paths, addresses and frequencies out); a refused CONTROL is named from a fixed word list and
// its value is never carried, a tuned frequency is only ever a BAND name. Calls are cheap when nothing is configured (a hermetic or bounded run) and may be made from
// any thread; they take the reporter's own locks and nothing of the driver's: call them AFTER leaving the driver's own mutex.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_RADIO_EVENTS_HPP
#define CASCADE_CORE_RADIO_EVENTS_HPP

#include <cstdint>
#include <string>

#include "core/radio_report.hpp"

namespace cascade::core::radioreport {

// ---------------------------------------------------------------------------
// Which radio: the engine's side
// ---------------------------------------------------------------------------
// The engine says "this source object is this radio" when a radio opens, and forgets it when the radio closes. `openedAtSteadyMs` is when it opened (for `openSec`).
void registerSource(const void* source, const RadioIdentity& radio);
void forgetRadio(const std::string& radioKey);
// True once when a driver already said this radio closed (`noteDeviceFault`), so that the engine's own close event for the same ending is not a second one.
bool takeCloseReported(const std::string& radioKey);
// The cause word of a radio's ending: "unplugged" when the radio is gone from the bus (its own sentence says so), "fault" when the driver gave up on it (`deviceDead`),
// "error" for a failure that is the application's or a driver exception, and "" when the text says nothing the app can use (the engine then keeps its own word).
std::string closeCauseFor(bool deviceDead, const std::string& text);

// ---------------------------------------------------------------------------
// A setting the radio refuses (G3)
// ---------------------------------------------------------------------------
// The CONTROL, from a fixed word list; never the value. Frequency has no `refused` word in the contract (CONTRACT ISSUE 9 of part 2): it is said in the reason and
// as a band.
enum class Control { Frequency, Rate, Bandwidth, Gain, AutoGain, Ppm, BiasTee, Antenna, DirectSampling, Decimation, Other };
const char* controlWord(Control c);        // "frequency" "rate" "bandwidth" "gain" "gain" "ppm" "bias" "antenna" "direct" "decimation" "other"
const char* controlRefusedWord(Control c); // the contract's `refused` word, or "" when the contract has none for it
// The control a driver's own description names ("retune", "IF gain change", "AGC set point change", "bias tee change"): for the drivers that apply many controls through
// one function and say which in a sentence.
Control controlFromText(const std::string& what);

// WHY a setting is being applied, for the event's `why` ("open", "user", "auto", "restore"): a scope the engine sets around the code that applies a setting. The default
// on a thread with no scope is "user" (the control path of a person's command).
class ControlWhyScope {
public:
    explicit ControlWhyScope(const char* why);
    ~ControlWhyScope();
    ControlWhyScope(const ControlWhyScope&) = delete;
    ControlWhyScope& operator=(const ControlWhyScope&) = delete;
private:
    const char* previous_;
};

// The radio refused a setting. `reason` is the driver's own sentence (scrubbed here). `userRange` is true when the refusal is the radio saying the VALUE the user
// asked for is outside what it can do (a typed value out of range): it is still reported, with its own class so it can be told from a fault of the radio.
void noteControlRefused(const void* source, const char* driverKey, Control control, const std::string& reason, bool userRange = false, const char* why = nullptr);
// The same, handing the text back: a driver's `setError(text)` becomes `setError(noteControlRefusedText(this, "rtlsdr", Control::Gain, text))`, one wrap at the place
// it already says the refusal. Safe to nest in any expression; it never throws.
std::string noteControlRefusedText(const void* source, const char* driverKey, Control control, std::string text, bool userRange = false, const char* why = nullptr);

// THE ENGINE'S SIDE OF A REFUSAL (mustfix-b, G3). The engine applies a setting through a driver and learns from its return value that the radio refused it; the
// driver may already have said so itself (every wired driver does, at the place it notices). A report from both would be two events for one refusal, a report from
// neither would leave a refusal nobody was told about (a driver that never learned to say it, or a branch of one that sets its error text and returns). The watch
// settles it with one rule: it counts the refusals RAISED ON THIS THREAD since it was made (the driver call is synchronous, so a driver's own report is on the same
// thread), and `noteRefusalIfSilent` reports only when there were none.
std::uint64_t refusalSequence();   // calls of noteControlRefused made on the calling thread so far (admitted or folded)
// The same rule for a caller that cannot hold a watch across two functions: the sequence number taken before the driver call, and the report if it has not moved.
void noteRefusalIfSilentSince(std::uint64_t sequenceBefore, const void* source, const char* driverKey, Control control, const std::string& text, bool userRange = false);
class RefusalWatch {
public:
    RefusalWatch();
    // The setting came back refused with this sentence. Files it as a refusal unless a driver already did, on this thread, since the watch was made. Safe to call
    // when it does nothing; never throws.
    void noteRefusalIfSilent(const void* source, const char* driverKey, Control control, const std::string& text, bool userRange = false);
    // The same, for a call site that has the outcome as a flag and wants no branch of its own: reports only when `refused` is true.
    void noteRefusalIf(bool refused, const void* source, const char* driverKey, Control control, const std::string& text, bool userRange = false);
private:
    std::uint64_t start_;
};

// ---------------------------------------------------------------------------
// A radio that failed after it opened (G1)
// ---------------------------------------------------------------------------
// The driver latched the radio dead (a transfer failed, the device went away). ONE `radio_close` event with its cause (`unplugged`, `fault` or `error`, from the
// sentence), urgent, at the place the driver detects it. `what` is the operation that failed ("reading samples"), `detail` the driver's own error text.
void noteDeviceFault(const void* source, const char* driverKey, const std::string& what, const std::string& detail);

// ---------------------------------------------------------------------------
// Stream health (G2)
// ---------------------------------------------------------------------------
struct StreamTrouble {
    double windowSec = 0.0;            // the driver's health window
    std::uint64_t samples = 0;         // samples read in the window
    std::uint64_t errors = 0;          // reads that failed
    std::uint64_t overflows = 0;       // blocks the ring could not take
    std::uint64_t timeouts = 0;        // reads that came back with nothing in time
    long long longestGapMs = 0;        // the longest wait between two blocks of samples
};
// A window with errors or a stall (a gap of a second or more): one `radio_stream` event with `usbErrors`, `stalls` and the window's figures. The reporter's own bound
// (first and last of a run of them, the rest counted in `dropped`) keeps a loop from flooding.
void noteStreamTrouble(const void* source, const char* driverKey, const StreamTrouble& t);

// ---------------------------------------------------------------------------
// A vendor call or a service that failed (G8, G9, S8, G5)
// ---------------------------------------------------------------------------
// `what` is a word of the contract's recovery vocabulary (^[a-z0-9]{3,24}$): "vendorcall", "pllretry", "release", "uninit", "servicerestart". `absorbed` is true when the
// application went on after the fault (phase `ok`) and false when it gave up (phase `gave-up`, urgent).
void noteVendorCallFault(const void* source, const char* driverKey, const char* what, const std::string& reason, bool absorbed = true);

// A service the radios depend on (the SDRplay API service): not tied to one source object; the open radio of that driver, when there is one.
void noteServiceFault(const char* driverKey, const char* what, const std::string& reason, bool absorbed);

// ---------------------------------------------------------------------------
// A radio that is not one of the engine's registered sources (mustfix-b)
// ---------------------------------------------------------------------------
// The HackRF sweeper, the one-click driver installer: code that uses or prepares a physical radio without the engine having opened it as the receiver. The radio is
// the one `driverKey` and `args` name (the engine's own reading: the model from the driver, a serial only where the arguments carry one). A radio that did not
// start for such a use is a radio_fail like any other, sent with the Diagnostics switch on or off; a radio that stopped is a radio_close. `trigger` is a word of the
// contract (2.1); anything else is filed as "other".
void fileSideRadioFailure(const char* driverKey, const std::string& args, const std::string& reason, const char* trigger = "other");
void noteSideRadioStopped(const char* driverKey, const std::string& args, const std::string& what, const std::string& detail);

// How many driver events had no open radio to be about (dropped: an open that fails is a radio_fail, a service state with no radio open is information), and how many
// calls threw inside the helper (counted and swallowed: a report must never take a driver's fault path down). For the tests and the record.
std::uint64_t unattributedCount();
std::uint64_t lostCount();

}  // namespace cascade::core::radioreport

#endif  // CASCADE_CORE_RADIO_EVENTS_HPP
