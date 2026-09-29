// sdrplay_probe.hpp - the SDRplay DIAGNOSTIC PROBE: one run, one plain-text
// file, everything we need to know about how a stranger's RSP and SDRplay API
// actually behave.
//
// WHY THIS EXISTS. There is no SDRplay hardware on the desk this driver was
// written on, and every SDRplay fault FoxSDR has ever fixed was diagnosed from
// the few lines a bug report happened to carry. The 0.99.46 RSPdx-R2 report is
// the case in point: whether the service would have answered the abandoned LNA
// change, and after how long, was the one fact that decided the fix - and it
// was not in the log, because the driver never waits long enough to find out.
// The probe does. An owner of any RSP runs it once (SYSTEM > Diagnostics >
// "Run SDRplay diagnostic", or `cascade --sdrplay-probe <file>`) and sends us
// one complete file.
//
// WHAT IT IS NOT: the receiver. It drives the SDRplay API table directly
// (sdrplay_api_decl.hpp), not through SdrPlaySource, because the receiver's
// whole job is to give up quickly and the probe's is the opposite: every call
// is timed, entry and exit, and waited for up to kProbeCallLimit before it is
// recorded as still inside. It is run in a process of its own by the GUI (a
// hung vendor call there cannot take the GUI's SDRplay session with it), and
// its own thread never blocks for longer than a short slice, so a cancel is
// always answered promptly.
//
// WHAT IT NEVER WRITES: a serial number (only a hash of it), a user path (the
// report goes through core::scrubUploadPath and core::maskAccountNames), or
// anything received. And it NEVER switches the bias tee on unless the caller
// passed biasTeeOn - which the GUI sets only after a separate confirmation
// and the command line only with its own explicit switch - because the bias
// tee puts power on the antenna socket.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "source/sdrplay_api_decl.hpp"

namespace cascade::source {

// Bumped whenever the sequence or the file format changes, so a report can be
// read by the rules it was written under.
inline constexpr const char* kSdrPlayProbeVersion = "1";

// How long any one vendor call is waited for before it is recorded as HUNG
// and the probe stops making calls. Thirty seconds: six times the longest a
// field log has shown the service take to answer at all (0.96.2, ~5 s).
inline constexpr std::chrono::milliseconds kProbeCallLimit{30000};

// A call slower than this is SLOW - the receiver's own kControlWait, so the
// report says directly which calls the receiver would have given up on.
inline constexpr std::chrono::milliseconds kProbeSlowCall{1000};

// Streamed at each sample rate and IF mode.
inline constexpr std::chrono::milliseconds kProbeStreamPerRate{3000};

// Streamed after each frequency, LNA, antenna and bias-tee change.
inline constexpr std::chrono::milliseconds kProbeStreamPerStep{300};

enum class ProbeStatus { Pass, Slow, Hung, Error, NotRun, Skipped };

const char* probeStatusName(ProbeStatus s);

struct SdrPlayProbeOptions {
    // THE BIAS TEE IS SWITCHED ON ONLY WHEN THIS IS TRUE. Off by default and
    // off at the end of every run whatever happens.
    bool biasTeeOn = false;
    std::chrono::milliseconds callLimit = kProbeCallLimit;
    std::chrono::milliseconds slowCall = kProbeSlowCall;
    std::chrono::milliseconds streamPerRate = kProbeStreamPerRate;
    std::chrono::milliseconds streamPerStep = kProbeStreamPerStep;
    // Set from another thread to stop early: the call in progress is left to
    // its worker and no further call is made.
    const std::atomic<bool>* cancel = nullptr;
    // One short line per step as it starts, for a progress display.
    std::function<void(const std::string&)> progress;
    // What goes in the header. Filled by the caller so the engine has no
    // opinion about where the version or the OS description come from.
    std::string foxsdrVersion;
    std::string osDescription;
};

struct SdrPlayProbeStep {
    int number = 0;
    std::string name;
    ProbeStatus status = ProbeStatus::NotRun;
    std::string detail;  // the worst thing seen, for the summary line
};

struct SdrPlayProbeResult {
    std::string report;                  // the whole file, already scrubbed
    std::vector<SdrPlayProbeStep> steps; // 1..8, in order
    bool hung = false;                   // a call was still inside at the limit
    bool cancelled = false;
    int callsMade = 0;
};

// Runs the whole sequence through `api`. Never throws; a table that is not
// resolved produces a report that says so. Blocks the calling thread for the
// length of the run (a minute or two on real hardware) - call it from a
// thread of its own.
SdrPlayProbeResult runSdrPlayProbe(const sdrplay_abi::Api& api, const SdrPlayProbeOptions& opt);

// The hash the report prints instead of a serial: FNV-1a 64, sixteen hex
// digits. Stable across builds and machines, so two reports from one radio
// can be matched without either carrying the serial.
std::string sdrPlayProbeSerialHash(const std::string& serial);

// The command line's whole job: run the probe against the process's own
// SDRplay API and write the file. Returns the process exit code - 0 when the
// report was written and nothing was HUNG or ERROR, 1 when it was written
// with a HUNG or ERROR in it, 2 when it could not be written.
int runSdrPlayProbeToFile(const std::string& outPath, bool biasTeeOn);

// THE COMMAND LINE THE GUI STARTS THE PROBE WITH - this executable again,
// `--sdrplay-probe "<outPath>"`, plus `--sdrplay-probe-bias-tee` only when
// the separate bias-tee confirmation was given. Pure, so the quoting and the
// bias switch can be tested without starting anything. Both paths are
// double-quoted (a profile directory may have a space in it).
std::string sdrPlayProbeCommandLine(const std::string& exePath, const std::string& outPath,
                                    bool biasTeeOn);

// THE GUI'S HANDLE ON A RUNNING PROBE. A process of its own, so a vendor call
// that hangs there cannot take the GUI's own SDRplay session with it, and so
// nothing the probe does is ever on the GUI thread. The GUI polls running()
// once a frame; NOTHING HERE EVER WAITS OR KILLS - closing FoxSDR while a
// probe runs leaves it to finish on its own (its own limits end it) and write
// its file, which is the shutdown bound: zero.
class SdrPlayProbeChild {
public:
    SdrPlayProbeChild() = default;
    ~SdrPlayProbeChild();
    SdrPlayProbeChild(const SdrPlayProbeChild&) = delete;
    SdrPlayProbeChild& operator=(const SdrPlayProbeChild&) = delete;

    // Starts `cascade --sdrplay-probe <outPath>`. False, with `error` filled,
    // when it could not be started (or on a platform with no child process -
    // Android).
    bool start(const std::string& outPath, bool biasTeeOn, std::string& error);
    // True while the child is still running. Never blocks.
    bool running();
    // The child's exit code once running() has returned false; -1 before.
    int exitCode() const { return exitCode_; }

private:
#if defined(_WIN32)
    void* process_ = nullptr;
#else
    long pid_ = -1;
#endif
    int exitCode_ = -1;
};

}  // namespace cascade::source
