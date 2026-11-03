// The Slint host's HOUSEKEEPING (docs/engine-stage4.md 6.36, phase 2 step 2): everything the ImGui window does around its frame loop
// that is not drawing, so that a build of the Slint window can be the application and not a preview of it. The ImGui window does
// these in AppWindow (app_window.cpp, app_window_updates.cpp); the Slint window has no AppWindow, and the work belongs to no widget,
// so it lives here, in one unit that is tested without a window, a network or the user's profile.
//
// WHAT IT DOES, in the order the ImGui window does it (the line numbers are of app_window.cpp as of this phase):
//   start()      run()'s start (the update check ~1405, the sweep armed ~1444, "frame loop starting"), the constructor's half of the
//                restore (the interface's half read from the table, ~652-690), diagnosticsStartup (the crash-upload memory, ~21855)
//   poll()       the frame's polls: the sentinel's heartbeat and activity (~1572), the console's close request (~1538), the two senders
//                (~1702), the crash sweep started on the first look that can answer (~3259), the update check and download (~3636)
//   finish()     the end of run() (~2290-2345): the sentinel's first shutdown step, the sweep stopped and its memory kept
//                (crashUploadFinish), the two senders cancelled, the last half of the settings handed to the engine (flushInterfaceHalf)
//   closed()     what is left after the host has stopped the service (closeWindow, then ~AppWindow): the sentinel's last two phases, and
//                the update check let go (reaped within its grace, abandoned past it)
//
// WHAT IT LEAVES OUT, on purpose (the report of this step names each): the UI thread's freeze watchdog, the frame timer and the window
// state handed to the report writers (their false-positive rules - the display grace, the modal-loop excuse - are the ImGui window's
// presentation and message-pump code, and a watchdog without them would file freezes that are not); the beta tester's usage report and
// link poll; the Microsoft Store's own update flow (the decision is mirrored, the flow is logged as deferred); the team message cards
// (their Seen list is the interface's half of the settings); the report offer after an unclean exit (a line in the log instead of a
// window); and the ImGui window's test flags (--diag-stall, --diag-toggle, --link-tester). The sentinel is NOT left out: this program is
// started under it in a real session, and a host that never set its phase or beat would be filed as ending before its first frame.
//
// TABLE-ONLY FOR THE RECEIVER. This unit reaches the engine through the C API table and nothing else: two facts (fact.telemetry.* and
// fact.config.interface, read with get_setting) and one command (FOXAPP_OP_INTERFACE_CONFIG, with the whole document beside it). The
// core services it drives are the ones the ImGui window drives, through the same classes, with the same parameters.
//
// THE SEAM. Everything with an effect outside the process's memory (the table, the clock, the network, the disk, the sentinel's page,
// the console's flag, the log) is behind DutyWorld. LiveWorld is the real one; the test's is a recorder, so the ORDER of the exit and the
// decisions at the start are tested as sequences of calls without a thread, a socket or a window.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include "foxsdr_api.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <string>
#include <utility>
#include <vector>

#include "core/breadcrumb.hpp"
#include "core/config.hpp"
#include "core/crash_upload.hpp"
#include "core/feature_request.hpp"
#include "core/problem_report.hpp"
#include "core/updater.hpp"

namespace foxsdr::slinthost {

// ---- who this program is ----------------------------------------------------------------------------------------------------
// THE WORD THE USAGE WORKER WOULD CARRY IN A PROGRAM FIELD, if it had one. It does not: the heartbeat is {id, v, beat} and the usage
// record has no field for the program (src/core/telemetry.hpp; both are built inside the engine from versionString() and
// installChannel()), so nothing here can put this word in either. It is used where the host does have a say: the log line below, which
// rides the diagnostics ring into a report's log and the "Copy diagnostics" bundle, and the About text.
inline constexpr const char* kProgramId = "slint";
inline constexpr const char* kProgramExe = "foxsdr-slint";
// "program: slint (foxsdr-slint 0.99.104)".
std::string programLine(const std::string& version);

// ---- the bounded run --------------------------------------------------------------------------------------------------------
// `--frames N` is a test's contract: exactly N frames, then exit 0 (app_smoke matches "rendered 3 frames"). The Slint window's frame
// is the 33 ms tick, so the contract is N ticks: the Nth tick is the last, N = 0 runs none, a negative N is a real session.
class BoundedRun {
public:
    explicit BoundedRun(int frames) : frames_(frames) {}
    bool bounded() const { return frames_ >= 0; }
    int frames() const { return frames_; }
    int ticks() const { return ticks_; }
    // N = 0: the event loop is not entered at all.
    bool skipsLoop() const { return frames_ == 0; }
    // One tick has finished. True when it was the last one the run is allowed.
    bool noteTick() {
        ++ticks_;
        return frames_ >= 0 && ticks_ >= frames_;
    }
    // "foxsdr-slint: bounded run: 30 of 30 ticks, exit 0".
    std::string exitLine(int exitCode) const;

private:
    int frames_;
    int ticks_ = 0;
};

// ---- the settings file's lock -----------------------------------------------------------------------------------------------
// The window application takes the lock BEST EFFORT (windowServiceOptions: ServiceLock::BestEffort), so acquireConfig never refuses it
// and the service keeps its own state private; what the host can say about it is what the lock file carries ("role=<name> pid=<n>",
// core/config_lock.hpp, written by the holder). Held: that file names this process. Elsewhere: it names another one, and the service
// carried on without the lock. Refused is for a host that asked for a Strict lock and was refused (the sentence is the engine's).
enum class LockState { NotTaken, Held, Elsewhere, Unknown, Refused };
struct LockReport {
    LockState state = LockState::NotTaken;
    std::string sentence;   // the refusal, when Refused
};
// The pid in a lock file's text, or 0 when the text is not the shape a holder writes.
unsigned long holderPid(const std::string& lockFileText);
// This process's id, as the lock file names its holder.
unsigned long currentProcessId();
// asked: the host called acquireConfig at all (a real session with a settings file); accepted: what it answered.
LockReport lockReportFor(bool asked, bool accepted, const std::string& refusal, const std::string& holderText, unsigned long ownPid);
// The state in words, for the About text and the log.
std::string lockLine(const LockReport& report);
// The text of the lock file beside `configPath` ("" when there is none or it cannot be read).
std::string readLockHolder(const std::string& configPath);

// ---- the About text ---------------------------------------------------------------------------------------------------------
struct AboutFacts {
    std::string version;
    std::string configPath;       // "" = a run with no settings file
    std::string diagnosticsDir;   // "" = this run does not write diagnostics
    LockReport lock;
};
std::string aboutText(const AboutFacts& facts);

// ---- the update check's decisions -------------------------------------------------------------------------------------------
// WHETHER AND WHOM TO ASK, once, at the start. Run: ask foxsdr.com. OffByChoice: the Updates tick is off, ask nobody. StoreDeferred:
// a Microsoft Store copy asks the Store and never foxsdr.com (core::updateCheckDisposition); the Store's own flow is a later phase, so
// nobody is asked. Hermetic: a bounded run reaches no network, except for the canned answer of FOXSDR_FAKE_UPDATE_MANIFEST, which is
// a file (core/updater.hpp) - the ImGui window's own rule (app_window.cpp ~1405).
enum class UpdatePlan { Run, OffByChoice, StoreDeferred, Hermetic };
UpdatePlan updatePlanFor(bool bounded, bool manifestSeam, bool packaged, bool userEnabled);
const char* updatePlanLine(UpdatePlan plan);

// OFF MEANS OFF for uploading too (core/crash_upload.hpp): the sweep is armed only when the user's Diagnostics switch is on AND this
// run may write diagnostics at all (a bounded run has no folder).
enum class SweepPlan { Armed, OffBySwitch, NoFolder };
SweepPlan sweepPlanFor(bool diagnosticsEnabled, const std::string& diagnosticsDir);
const char* sweepPlanLine(SweepPlan plan);

// ---- the update card --------------------------------------------------------------------------------------------------------
// What the update card of the sections panel shows, as plain data the host's setters consume (ui/main.slint: update-shown,
// update-headline, update-critical, update-notes, update-primary, update-progress, update-note, update-error). The ImGui banner
// (drawUpdateBanner) is the model: its words are the words here.
enum class UpdateStage { Idle, Downloading, Ready, Installing };

struct UpdateView {
    bool enabled = false;            // the Updates tick
    bool dismissed = false;          // "Not now", for this launch
    bool packaged = false;           // a Microsoft Store copy: its own banner, not this card
    bool installerHere = false;      // Windows: there is an installer to download and run
    cascade::core::UpdateInfo info;  // the last successful check's answer
    UpdateStage stage = UpdateStage::Idle;
    float progress = 0.f;            // 0..1 while Downloading
    std::string error;               // a download or installer failure (a failed CHECK is never shown, as in ImGui)
};

struct UpdateCardModel {
    bool shown = false;
    std::string headline;
    bool critical = false;
    std::vector<std::string> notes;  // per release: its version (and "(important)"), then its lines
    std::string primary;             // the key's label, "" = none
    float progress = -1.f;           // 0..1 replaces the key by a bar; -1 = no bar
    std::string note;
    std::string error;
    bool operator==(const UpdateCardModel&) const = default;
};
UpdateCardModel updateCardFor(const UpdateView& view);

// ---- the seam ---------------------------------------------------------------------------------------------------------------
struct DownloadOutcome {
    bool ok = false;
    std::string path;    // the verified installer, when ok
    std::string error;   // PluginRepo's words, verbatim, when not
};

class DutyWorld {
public:
    virtual ~DutyWorld() = default;

    // The engine's table. fact: get_setting (false when the table does not answer, `value` empty). submitInterfaceHalf: the document
    // beside FOXAPP_OP_INTERFACE_CONFIG, true when the engine took it.
    virtual bool fact(const char* key, std::string& value) = 0;
    virtual bool submitInterfaceHalf(const std::string& document) = 0;
    virtual std::uint64_t nowEpoch() = 0;

    // The crash-report sweep (core/crash_upload.hpp): the endpoint, a start (a thread), and the stop that cancels, joins and answers.
    virtual std::string crashEndpoint() = 0;
    virtual void sweepStart(const cascade::core::SweepParams& params) = 0;
    virtual cascade::core::SweepOutcome sweepStop() = 0;

    // The update check, the download and the installer's start (core/updater.hpp). The check and the download are on workers and are
    // polled; the installer is launched on a worker too, because ShellExecute waits for an elevation prompt to be answered.
    virtual bool inPackage() = 0;
    virtual bool manifestSeam() = 0;
    virtual void updateCheckStart(const std::string& version, const std::string& channel) = 0;
    virtual bool updateCheckPoll(cascade::core::UpdateCheckOutcome& outcome) = 0;
    virtual void updateCheckReap() = 0;
    virtual void downloadStart(const cascade::core::UpdateInfo& info) = 0;
    virtual bool downloadPoll(DownloadOutcome& outcome) = 0;   // true once, when it has finished
    virtual float downloadProgress() = 0;
    virtual void downloadCancel() = 0;
    virtual void installerStart(const std::string& path) = 0;
    virtual bool installerPoll(bool& started) = 0;             // true once, when the launch has answered

    // The feature-request and problem-report senders (core/feature_request.hpp, core/problem_report.hpp).
    virtual void sendersPoll(std::uint64_t nowEpoch) = 0;
    virtual void sendersCancel() = 0;

    // The process: the sentinel's page (core/breadcrumb.hpp), the console's close request (core/console_close.hpp), the crash
    // handler's switches (core/crash_handler.hpp).
    virtual void phase(cascade::core::breadcrumb::Phase phase) = 0;
    virtual void beat() = 0;
    virtual void radioOpening(bool on) = 0;
    virtual void sentinelPoll() = 0;
    virtual bool consoleCloseRequested() = 0;
    virtual void applyDiagnosticsSwitches(bool enabled, bool minidump) = 0;

    // Say: `say` is the host's line on stderr ("foxsdr-slint: ..."), paths allowed; `diag` is a line of the diagnostics log, which
    // rides the ring into reports and so carries no path, name or address.
    virtual void say(const std::string& line) = 0;
    virtual void diag(const std::string& line) = 0;
};

// ---- the duties -------------------------------------------------------------------------------------------------------------
// The manifest at foxsdr.com/api/update names a Windows installer whatever platform asks, and nothing else exists to offer: elsewhere the
// card says so and points at the site (app_window_updates.cpp, drawUpdateBanner).
#if defined(_WIN32)
inline constexpr bool kPlatformHasInstaller = true;
#else
inline constexpr bool kPlatformHasInstaller = false;
#endif

struct DutyStart {
    std::string configPath;        // BootResult::configPath ("" = no settings file)
    bool configLoaded = false;     // EngineService::Restored::loaded
    std::string diagnosticsDir;    // diagCrashDir() when boot let this run write, else ""
    std::string version;           // cascade::versionString()
    int frames = -1;               // BootResult::frames
    bool lastRunUnclean = false;   // EngineService::Restored::lastRunUnclean
    LockReport lock;               // what the lock step came to
    bool installerHere = kPlatformHasInstaller;
};

struct TickInput {
    bool deviceOpening = false;    // FOXAPI_RX_DEVICE_OPENING in the state just read
};

// THE SENTINEL'S POLL runs every N ticks: the ImGui window looks every 300 frames (~5 s at 60 Hz); the tick is 33 ms.
inline constexpr int kSentinelPollEveryTicks = 150;

class HostDuties {
public:
    // `requestClose` ends the event loop (the installer has been started, or the console asked for the close).
    HostDuties(DutyWorld& world, std::function<void()> requestClose);

    // Call after the engine has started and before the window is made. Leaves the sentinel at "creating the window".
    void start(const DutyStart& start);
    // Call when the window is shown: the sentinel's "awaiting the first frame"; the tick's beats take it to "running".
    void windowUp();
    // Call from every tick.
    void poll(const TickInput& in);
    // True once after the card changed; the host then applies card() to the window's properties.
    bool takeCardChange();
    const UpdateCardModel& card() const { return card_; }
    // The card's two keys.
    void updatePrimaryClicked();
    void updateDismissed();

    // Call when the event loop has returned, before the host stops the service.
    void finish();
    // Call after the host has stopped the service.
    void closed();

    AboutFacts about() const;
    int ticks() const { return ticks_; }
    const cascade::core::UploadPolicyState& crashMemory() const { return crashState_; }
    bool sweepRunning() const { return sweepRunning_; }

private:
    void startUpdateCheck();
    void trySweepStart();
    void pollUpdate();
    void startDownload();
    void startInstaller();
    void refreshCard();
    void handBackSettings();

    DutyWorld& w_;
    std::function<void()> requestClose_;
    DutyStart cfg_;

    // The interface's half as the engine served it, and the crash-upload memory decoded from it.
    cascade::core::AppConfig iface_;
    bool ifaceServed_ = false;
    cascade::core::UploadPolicyState crashAtStart_;
    cascade::core::UploadPolicyState crashState_;

    bool sweepArmed_ = false;
    bool sweepRunning_ = false;

    bool updateAsked_ = false;
    bool updatePending_ = false;
    bool updateDismissed_ = false;
    cascade::core::UpdateInfo update_;
    UpdateStage stage_ = UpdateStage::Idle;
    float progress_ = 0.f;
    std::string readyPath_;
    std::string updateError_;
    UpdateCardModel card_;
    bool cardChanged_ = false;

    int ticks_ = 0;
    bool closeAsked_ = false;
};

// ---- the real world ---------------------------------------------------------------------------------------------------------
class LiveWorld : public DutyWorld {   // not final: the test overrides the sweep and keeps the real table
public:
    // `api` and `session` are the host's table and its LOCAL session (VIEW | TUNE | SETTINGS); either may be null in a test, and
    // every call that needs the table then answers "not served".
    LiveWorld(const FoxEngineApi* api, FoxSession* session);
    ~LiveWorld() override;
    LiveWorld(const LiveWorld&) = delete;
    LiveWorld& operator=(const LiveWorld&) = delete;

    // The senders the companion pages of a later phase send through; HostDuties polls and cancels them.
    cascade::core::FeatureRequestSender& featureSender() { return feature_; }
    cascade::core::ProblemReportSendFlow& problemSender() { return problem_; }

    // TEST SEAM: what starts the downloaded installer. The default is the shell's "open" on Windows (nothing elsewhere); a test sets its
    // own, because the real one would start an installer.
    using InstallerLauncher = std::function<bool(const std::string& path)>;
    void setInstallerLauncher(InstallerLauncher launcher) { launcher_ = std::move(launcher); }

    bool fact(const char* key, std::string& value) override;
    bool submitInterfaceHalf(const std::string& document) override;
    std::uint64_t nowEpoch() override;
    std::string crashEndpoint() override;
    void sweepStart(const cascade::core::SweepParams& params) override;
    cascade::core::SweepOutcome sweepStop() override;
    bool inPackage() override;
    bool manifestSeam() override;
    void updateCheckStart(const std::string& version, const std::string& channel) override;
    bool updateCheckPoll(cascade::core::UpdateCheckOutcome& outcome) override;
    void updateCheckReap() override;
    void downloadStart(const cascade::core::UpdateInfo& info) override;
    bool downloadPoll(DownloadOutcome& outcome) override;
    float downloadProgress() override;
    void downloadCancel() override;
    void installerStart(const std::string& path) override;
    bool installerPoll(bool& started) override;
    void sendersPoll(std::uint64_t nowEpoch) override;
    void sendersCancel() override;
    void phase(cascade::core::breadcrumb::Phase phase) override;
    void beat() override;
    void radioOpening(bool on) override;
    void sentinelPoll() override;
    bool consoleCloseRequested() override;
    void applyDiagnosticsSwitches(bool enabled, bool minidump) override;
    void say(const std::string& line) override;
    void diag(const std::string& line) override;

private:
    const FoxEngineApi* api_;
    FoxSession* session_;
    cascade::core::CrashUploader crash_;
    cascade::core::UpdateCheckTask check_;
    std::future<bool> download_;
    std::atomic<float> progress_{0.f};
    std::atomic<bool> cancel_{false};
    std::string resultPath_;
    std::string resultError_;
    std::future<bool> installer_;
    InstallerLauncher launcher_;
    cascade::core::FeatureRequestSender feature_;
    cascade::core::ProblemReportSendFlow problem_;
};

}  // namespace foxsdr::slinthost
