// report_send_guard.hpp - THE RULE THAT NO TEST, DRIVE OR CI RUN SENDS A REPORT OFF THIS COMPUTER (send-on lane, 2026-11-17; record
// docs/multi-radio-records/send-on.md).
//
// With radioreport::kRadioReportsLiveSend true, every interactive run of the real program arms the sender (Engine::configureHeartbeat). A test or a
// drive of foxsdr-slint.exe / cascade.exe is an interactive run of the real program, so without this rule every ctest and every hidden-desktop drive on
// this machine, and every CI job, would post to https://telemetry.foxsdr.com. A `synthetic: 1` marker is not enough: the Worker would drop the message,
// but it would already have left. So the rule sits at the one place a report's bytes are handed to the network, `makeRealTransport`, and the usage
// heartbeat's real transport (TelemetryReporter::send), and answers, for a destination and the profile folder the run keeps its queue in:
//
//   allowed         the destination is on this computer (127.0.0.1, localhost, ::1): a stand-in server a test started, which is the point of such a test; or
//                   this is a real user's run.
//   refused (why)   the destination is anywhere else AND the run is not a real user's:
//     "env"         FOXSDR_NO_OUTSIDE_REPORTS is set (ctest sets it on every test; the drive scripts and CI set it)
//     "synthetic"   core::syntheticRun(): FOXSDR_SYNTHETIC, CI or GITHUB_ACTIONS is set
//     "diag-dir"    FOXSDR_DIAG_DIR is set (the diagnostics tree is a test tree: the same interlock crash_upload.cpp has had since the crash path)
//     "redirected"  the profile folder is not the one the operating system names for this user: APPDATA (Windows) or HOME (elsewhere) was pointed
//                   somewhere else, as every drive and hermetic test does. Portable mode (a foxsdr.portable marker beside the program) and the Store
//                   package are the two places a real user's profile is legitimately elsewhere, and are not refused for it.
//
// THE FIRST THREE are markers somebody has to remember to set; THE FOURTH is not: it is how this computer says "this is not the user's profile", so a
// test or a drive that forgets every marker is still stopped, because it has redirected its profile to keep the real config.json safe.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_REPORT_SEND_GUARD_HPP
#define CASCADE_CORE_REPORT_SEND_GUARD_HPP

#include <string>
#include <vector>

namespace cascade::core::radioreport {

// True when the URL's host is on this computer (127.0.0.1, localhost, ::1 / [::1]).
bool isLoopbackUrl(const std::string& url);

// "" when a message may be sent to `url` by a run whose queue lives in `profileDir` (the `radio-reports` folder or the folder above it); otherwise the
// short reason word above. Reads the environment on every call (a test changes it).
std::string outsideSendRefusal(const std::string& url, const std::string& profileDir);

// The decision itself, with the folders the operating system names for this user's profile passed in (the tests give it each situation). Only usable (absolute) names count; with none the
// answer is "allowed": the guard exists to stop tests and drives and must never silence a real user whose system call failed or answered something odd.
std::string outsideSendRefusalWithRoots(const std::string& url, const std::string& profileDir, const std::vector<std::string>& osRoots);

// The same for the run's default profile (the folder ConfigStore::defaultPath() names): for the usage heartbeat, which has no queue of its own.
std::string outsideSendRefusal(const std::string& url);

}  // namespace cascade::core::radioreport

#endif  // CASCADE_CORE_REPORT_SEND_GUARD_HPP
