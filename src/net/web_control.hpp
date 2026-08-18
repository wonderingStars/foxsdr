// Control requests from the browser (P11): the parse-and-validate step that
// turns a JSON body into something the application may act on.
//
// WHY THIS IS PURE AND SEPARATE. Applying a control touches the receiver, and
// several of the things a browser wants to change are documented GUI-THREAD-
// ONLY (the source's centre frequency in particular). So the HTTP handler never
// applies anything: it validates here, queues the result, and answers 202; the
// GUI thread drains the queue and applies it on the next frame. That split is
// what lets every rejection be tested without a socket or a radio, and it is
// the same shape the rest of net/ already uses — decide in a pure function,
// act somewhere that is allowed to.
//
// THE RULES, each covered in tests/test_web_control.cpp:
//
//  1. THE BODY MUST BE A JSON OBJECT, and every key in it must be one this
//     version understands. An UNKNOWN KEY IS AN ERROR, not something to
//     ignore: a browser sending "centreHz" or "freqHz" has a bug, and a
//     control API that silently accepts a request it did not carry out is
//     worse than one that refuses. (This is the opposite of the config
//     store's rule, deliberately — a config file must survive being written
//     by a future version, whereas a control request is a live instruction
//     from a client we are talking to right now.)
//
//  2. EVERY VALUE IS RANGE-CHECKED, and NaN and infinity are refused
//     everywhere a number is accepted. A NaN centre frequency reaching the
//     source setter is not a wrong tune, it is a wrong tune that no later
//     clamp can repair.
//
//  3. MODE NAMES COME FROM dsp::modeFromName, exact and case-sensitive, so
//     the browser's vocabulary is the same one the config file and the GUI
//     use rather than a third private list.
//
//  4. AN EMPTY OBJECT IS REFUSED. "{}" is a client that meant to say
//     something and did not; answering 202 to it would report success for a
//     request that changed nothing.
//
// Absent fields mean "leave this alone" — which is why every member is
// optional rather than carrying a sentinel that some valid value could collide
// with.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <optional>
#include <string>

#include "dsp/demod.hpp"

namespace cascade::net {

// Accepted ranges. Deliberately generous where the application clamps again
// against the live sample rate (offset, bandwidth) and tight where nothing
// downstream would catch a nonsense value.
inline constexpr double kMinCenterHz = 0.0;
inline constexpr double kMaxCenterHz = 6.0e9;   // beyond any SDR this targets
inline constexpr double kMaxVfoOffsetHz = 1.0e8;
inline constexpr double kMinBandwidthHz = 100.0;
inline constexpr double kMaxBandwidthHz = 1.0e7;
inline constexpr double kMinSquelchDb = -200.0;
inline constexpr double kMaxSquelchDb = 20.0;
// Display range. The ends are the desktop sliders' own limits; the rule that
// dbMin must stay below dbMax is enforced by the application, which owns the
// same minimum-span rule the window uses.
inline constexpr double kMinDisplayDb = -200.0;
inline constexpr double kMaxDisplayDb = 40.0;
// The notch's audible span inside the 48 kHz sink's Nyquist, and the biquad's
// useful Q range — the same bounds the config store sanitizes to.
inline constexpr double kMinNotchHz = 10.0;
inline constexpr double kMaxNotchHz = 20000.0;
inline constexpr double kMinNotchQ = 0.1;
inline constexpr double kMaxNotchQ = 1000.0;

struct ControlRequest {
    std::optional<bool> running;                    // start / stop the receiver
    std::optional<double> centerHz;                 // source centre frequency
    std::optional<double> vfoOffsetHz;              // VFO offset from centre
    std::optional<cascade::dsp::DemodMode> mode;
    std::optional<double> bandwidthHz;
    std::optional<double> squelchDb;
    std::optional<double> volume;                   // 0..1

    // Display range shared by the spectrum axis and the waterfall colormap.
    std::optional<double> dbMin;
    std::optional<double> dbMax;

    // Audio processing, matching the desktop's "Radio" and "Audio filters".
    std::optional<int> deemphasisIndex;             // 0 = 50 us, 1 = 75 us, 2 = off
    std::optional<bool> nrEnabled;
    std::optional<double> nrStrength;               // 0..1
    std::optional<bool> notchEnabled;
    std::optional<double> notchFreqHz;
    std::optional<double> notchQ;
    std::optional<bool> autoNotch;
    std::optional<bool> stereoEnabled;

    // --- Source ---------------------------------------------------------
    // "siggen" or "soapy". NOTE what is deliberately absent: there is no way
    // for a browser to name an IQ FILE. Letting a network client open an
    // arbitrary path on the machine running the receiver is a real capability
    // — a file-read primitive dressed as a source selector — and it is not
    // one to add without the owner deciding they want it. The desktop window
    // keeps its file picker.
    std::optional<std::string> sourceKind;
    // Which SoapySDR device to open, as the kwargs string the scan reported.
    // Matched against the scanned list rather than passed to the driver
    // verbatim, so a browser cannot hand arbitrary kwargs to a vendor module.
    std::optional<std::string> soapyArgs;
    std::optional<std::string> antenna;
    std::optional<double> sampleRateHz;
    // One named gain stage, set as a pair — both or neither.
    std::optional<std::string> gainName;
    std::optional<double> gainDb;
    std::optional<bool> agc;
    // Ask the application to enumerate SoapySDR devices. A REQUEST, not a
    // result: enumeration loads vendor modules and walks the USB bus, so it
    // happens on the application's own thread and the outcome appears in the
    // next status poll.
    std::optional<bool> scanDevices;

    // --- Recorder -------------------------------------------------------
    // The recording DIRECTORY is not settable from here, for the same reason
    // an IQ file is not selectable: it would let a network client choose
    // where the application writes on the host. Takes land in the directory
    // the application already uses.
    std::optional<bool> recordIq;
    std::optional<bool> recordAudio;

    // --- Bookmarks ------------------------------------------------------
    // Add the CURRENT frequency under this name.
    std::optional<std::string> bookmarkAdd;
    std::optional<int> bookmarkTune;     // index into the published list
    std::optional<int> bookmarkRemove;   // index into the published list

    // --- Scanner --------------------------------------------------------
    std::optional<bool> scannerActive;
    std::optional<double> scanStartHz;
    std::optional<double> scanStopHz;
    std::optional<double> scanStepHz;

    // --- Plugins ---------------------------------------------------------
    // Fetch the catalogue. A REQUEST: this is the only thing in the product
    // that contacts the catalogue origin, and it still happens only because
    // someone asked — the browser is simply another way of asking.
    std::optional<bool> pluginFetch;
    // Install the catalogue entry with this id. `acknowledgeNotice` is the
    // browser's equivalent of ticking the legal-notice box, and the install
    // gate refuses without it exactly as the desktop's does — the SAME
    // predicate decides both, so the two can never disagree about what is
    // installable.
    std::optional<std::string> pluginInstall;
    std::optional<bool> acknowledgeNotice;
    // Remove an installed plugin by FILE NAME, as the status reports it.
    std::optional<std::string> pluginRemove;
    // Grant or revoke one plugin's permission to move the receiver. Both
    // fields or neither.
    std::optional<std::string> pluginTuneName;
    std::optional<bool> pluginTuneAllowed;
    // Apply one of a plugin's declared presets: tune there, set the mode,
    // bandwidth and device rate it asks for, and open what it contributes.
    // Both fields or neither — an index with no plugin names nothing.
    std::optional<std::string> pluginPresetName;
    std::optional<int> pluginPresetIndex;

    bool empty() const {
        return !running && !centerHz && !vfoOffsetHz && !mode && !bandwidthHz &&
               !squelchDb && !volume && !dbMin && !dbMax && !deemphasisIndex &&
               !nrEnabled && !nrStrength && !notchEnabled && !notchFreqHz &&
               !notchQ && !autoNotch && !stereoEnabled && !sourceKind &&
               !soapyArgs && !antenna && !sampleRateHz && !gainName && !gainDb &&
               !agc && !scanDevices && !recordIq && !recordAudio &&
               !bookmarkAdd && !bookmarkTune && !bookmarkRemove &&
               !scannerActive && !scanStartHz && !scanStopHz && !scanStepHz &&
               !pluginFetch && !pluginInstall && !pluginRemove &&
               !pluginTuneName && !pluginTuneAllowed && !pluginPresetName &&
               !pluginPresetIndex;
    }
};

// Parses and validates one control body. Returns false with `error` set to a
// sentence suitable for returning to the client verbatim; `out` is left empty
// on any failure, so a caller that ignores the return value cannot act on a
// half-parsed request.
bool parseControlRequest(const std::string& body, ControlRequest& out,
                         std::string& error);

}  // namespace cascade::net
