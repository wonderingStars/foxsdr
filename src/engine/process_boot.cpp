// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// Process boot, in the order src/main.cpp had it (engine extraction stage 7a:
// a MOVE; the statements and their comments are main()'s, the step functions
// and the kind guards are the only new text). See process_boot.hpp for why this
// is a unit of its own and what the kind decides.
//
// READING ORDER. bootProcess() at the bottom is the sequence; each step above it
// is one stretch of the old main() with its own comments intact. Every step
// below says WHY IT IS WHERE IT IS in a "POSITION" paragraph, because the order
// is the contract and has been broken before (a helper that read the config, a
// handler armed after a fault-prone read, a mutex claimed after the flags were
// parsed). tests/test_process_boot_order.cpp holds the order from this file's text.
#include "engine/process_boot.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>

#include "core/breadcrumb.hpp"
#include "core/config.hpp"
#include "core/console_close.hpp"
#include "core/console_owner.hpp"
#include "core/crash_handler.hpp"
#include "core/diag_log.hpp"
#include "core/diag_report.hpp"
#include "core/exit_record.hpp"
#include "core/sentinel.hpp"
#include "core/stop_signal.hpp"
#include "core/tester_link.hpp"
#include "core/updater.hpp"
#include "core/version.hpp"
#include "net/web_policy.hpp"
#include "source/sdrplay_probe.hpp"
#include "source/sdrplay_service.hpp"
#include "source/soapy_enum_proc.hpp"

// ---------------------------------------------------------------------------
// Fatal-fault capture.
//
// This used to be forty lines of SEH filter inline in main.cpp (kept from the
// P6a crash investigation, 2026-08-15, where one stderr line naming
// libusb-1.0.dll inside SoapyUHD was the whole diagnosis). That line still
// exists - it is now written by core/crash_handler.cpp, along with a report an
// engineer who was not there can act on. See core/crash_handler.hpp for which
// four registrations are made and why one filter was never enough.
// ---------------------------------------------------------------------------
namespace cascade::engine {

namespace {

// WHEN DIAGNOSTICS TOUCH THE DISK, and it is deliberately narrow.
//
// A plain `cascade` with no arguments is a real session on a real user's
// machine: it logs, it captures, it offers a report. EVERY flagged invocation
// is a tool or a test - app_smoke, app_selftest, app_version, --update-check,
// the bench checks - and those must leave nothing behind on the machine that
// ran them, exactly as the config already stays hermetic under --frames.
//
// FOXSDR_DIAG_DIR overrides both halves: it redirects the whole tree into a
// caller-owned directory AND turns capture on, which is how
// tests/test_diag_hang.cpp exercises the real application without going
// anywhere near the user's %LOCALAPPDATA%\FoxSDR.
//
// THE ENGINE (stage 7b) has no "real session" of this kind: a bare
// `foxsdr-engine` is a headless run that stores nothing unless it is given a
// settings file (stage 7c), so it is hermetic whatever its argument count is - the
// one door is the same override, a directory somebody named on purpose.
bool diagnosticsOnDisk(ProcessKind kind, int argc) {
    const char* over = std::getenv("FOXSDR_DIAG_DIR");
    if (over != nullptr && *over != '\0') { return true; }
    if (kind != ProcessKind::Host) { return false; }
    return argc == 1;
}

// THE ENGINE'S SETTINGS FILE, as the command line names it (stage 7c): `--config PATH` is that file,
// `--use-config` the per-user file the window application uses (the survey's decision O3: one shared
// file, guarded by a lock); neither: none - the hermetic default of 7b. Read from argv alone, so the
// stored Diagnostics switch can be honoured before the parse (the Host's reason, startupConfigPath);
// parseFlags is what REFUSES a malformed or contradictory line, so a refused line may be read here
// and is never acted on.
std::string engineConfigFromArgs(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc && argv[i + 1][0] != '\0' &&
            std::strncmp(argv[i + 1], "--", 2) != 0) {
            return std::string(argv[i + 1]);
        }
        if (std::strcmp(argv[i], "--use-config") == 0) { return cascade::core::ConfigStore::defaultPath(); }
    }
    return std::string();
}

// ...and the same answer once the parse has accepted the line: what wireConfig hands on to an Engine.
std::string engineSettingsPath(const BootResult& boot) {
    if (!boot.engineConfigArg.empty()) { return boot.engineConfigArg; }
    if (boot.engineUseConfig) { return cascade::core::ConfigStore::defaultPath(); }
    return std::string();
}

// The config THIS run will use, resolved before the argument parse so the
// user's stored switch can be honoured before anything touches the disk. Same
// rules as the config wiring at the bottom of main(), and deliberately so: a
// bare `cascade` uses the real per-user config; a bounded --frames run uses
// CASCADE_CONFIG_TEST when it is set and no config at all otherwise. Empty
// means "this run has no stored preference".
std::string startupConfigPath(ProcessKind kind, int argc, char** argv) {
    // An Engine reads no config unless it was ASKED to (stage 7b: none at all; 7c:
    // `--config PATH` or `--use-config`): "no stored preference", so the stored
    // switch below answers its default and no file of the user's is opened.
    if (kind != ProcessKind::Host) { return engineConfigFromArgs(argc, argv); }
    bool bounded = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0) { bounded = true; }
    }
    if (bounded) {
        const char* hook = std::getenv("CASCADE_CONFIG_TEST");
        return (hook != nullptr && *hook != '\0') ? std::string(hook) : std::string();
    }
    if (argc == 1) { return cascade::core::ConfigStore::defaultPath(); }
    return std::string();
}

// DOES THE USER WANT THIS AT ALL, read from the config BEFORE a directory is
// created or a handler is armed.
//
// This is the second half of "off means off", and the half that was wrong.
// diagnosticsOnDisk() above answers "is this run allowed to write" - a real
// session rather than a tool. It does NOT answer "did the user ask for it",
// and reading that only once AppWindow::run() is reached is far too late: by
// then the log file exists, the crashes directory exists, and the crash
// handler has been armed across the config load, the GL context and the
// LoadLibrary of every third-party plugin - which is the most fault-prone part
// of start-up and precisely where a user who opted out would get a report.
//
// Reproduced on the shipped application before this was written: a config
// saying "diagnosticsEnabled": false produced a 14-line foxsdr.log and a
// crashes directory. Held there now by tests/test_diagnostics.cpp, which
// launches the real binary with that config and requires the tree not to exist.
//
// The cost is one small JSON read at the top of main() that AppWindow will do
// again a moment later. That is the correct trade: the alternative is arming
// the disk before consent is known.
bool storedDiagnosticsEnabled(ProcessKind kind, int argc, char** argv) {
    const std::string path = startupConfigPath(kind, argc, argv);
    if (path.empty()) { return cascade::core::AppConfig{}.diagnosticsEnabled; }
    cascade::core::AppConfig cfg;
    std::string err;
    // Missing or corrupt file: ConfigStore::load leaves the defaults in place,
    // which is on. A user who has never chosen has not opted out.
    cascade::core::ConfigStore::load(path, cfg, err);
    return cfg.diagnosticsEnabled;
}

// An unrecognised or untrusted foxsdr: link is dropped by an Engine without being
// parsed (the Host has its own copy of this test inline in activateLink, moved
// verbatim): the scheme, case-insensitively.
bool hasFoxsdrScheme(const char* arg) {
    const char* want = "foxsdr:";
    for (int k = 0; want[k] != '\0'; ++k) {
        const char c = arg[k];
        if (c == '\0') { return false; }
        if (c != want[k] && c != want[k] - ('a' - 'A')) { return false; }
    }
    return true;
}

// A step returns nullopt to let the boot go on, or the exit code of a process
// that is finished.
using Step = std::optional<int>;

// ---------------------------------------------------------------------------
// STEP 2 - the helper modes.
//
// POSITION: after the console (every line a helper prints must already know
// where it is printing) and BEFORE anything else, because a helper must touch
// nothing: no crash handler, no config, no diagnostics directory, no instance
// name. A helper is a throwaway child of a session; arming anything in it would
// write reports or claim a name on the session's behalf.
// ---------------------------------------------------------------------------
Step runHelperModes(int argc, char** argv) {
    // THE ENUMERATION HELPER, decided before anything else in this function.
    //
    // `cascade --enumerate-json` is not a user-facing mode: it is the child
    // process AppWindow's device scan spawns so that a vendor driver faulting
    // mid-probe kills a throwaway process instead of the session. See
    // source/soapy_enum_proc.hpp for why nothing short of a process boundary
    // contains that fault.
    //
    // It returns HERE, above the config read, because a helper must touch
    // nothing else: no config file, no window, no telemetry marker. It does
    // NOT inherit the parent's diagnostics environment either - FOXSDR_DIAG_DIR
    // is set in some sessions and would otherwise arm capture on nothing more
    // than an inherited variable.
    //
    // The ONE thing it accepts is the parent's crash directory, passed
    // explicitly and only when the parent's own capture is armed. Without it
    // the walk - the most crash-prone path in this product - would run in a
    // process with no handler at all, turning a symbolised fault stack into an
    // exit code; see CRASH CAPTURE IN THE CHILD in source/soapy_enum_proc.hpp.
    //
    // The accepted forms are enumerated exhaustively, and argv[1] must be the
    // flag itself, so no combination of arguments to a real session can turn
    // it into a helper.
    // THE SDRPLAY DIAGNOSTIC PROBE (0.99.50, source/sdrplay_probe.hpp):
    //
    //     cascade --sdrplay-probe <outfile> [--sdrplay-probe-bias-tee]
    //
    // Returns here for the same reasons the enumeration helper below does -
    // no config, no window, no telemetry marker - and because SYSTEM >
    // Diagnostics runs it as a child process while the GUI is open. The bias
    // tee is switched on during the run ONLY with the second switch: that is
    // the command line's form of the GUI's separate confirmation, since the
    // bias tee puts power on the antenna socket.
    // THE SDRPLAY API SERVICE'S STATE (0.99.55, source/sdrplay_service.hpp):
    //
    //     cascade --sdrplay-service-state
    //
    // Asks the Windows Service Control Manager - read-only, no administrator
    // rights - what the SDRplay API Service is doing, prints it, and returns.
    // Touches nothing else (no config, no window, no telemetry marker), for
    // the probe's reasons above; it exists so support and the tests can see
    // the query's answer on a real machine without opening the application.
    if (argc >= 2 && std::strcmp(argv[1], "--sdrplay-service-state") == 0) {
        if (argc != 2) {
            std::fprintf(stderr, "cascade: --sdrplay-service-state takes no arguments\n");
            return 2;
        }
        const cascade::source::SdrPlayServiceStatus st =
            cascade::source::querySdrPlayServiceFromWindows();
        std::printf("sdrplay-service: %s\n", cascade::source::sdrPlayServiceSummary(st).c_str());
        const char* sentence = cascade::source::sdrPlayServiceSentence(st);
        if (sentence[0] != '\0') { std::printf("advice: %s\n", sentence); }
        std::fflush(stdout);
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "--sdrplay-probe") == 0) {
        if (argc < 3 || argv[2][0] == '\0' || argv[2][0] == '-') {
            std::fprintf(stderr, "cascade: --sdrplay-probe requires an output file\n");
            return 2;
        }
        bool biasTee = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--sdrplay-probe-bias-tee") == 0) {
                biasTee = true;
            } else {
                std::fprintf(stderr,
                             "cascade: --sdrplay-probe takes only an output file and "
                             "--sdrplay-probe-bias-tee\n");
                return 2;
            }
        }
        return cascade::source::runSdrPlayProbeToFile(argv[2], biasTee);
    }

    // THE SENTINEL (0.99.64, core/sentinel.hpp):
    //
    //     cascade --sentinel --app-pid=N --app-handle=H --map-handle=M ...
    //
    // The same executable, started by the application itself, which waits for the
    // application to end and writes the one report of an ending nothing inside it
    // could report. Returns here for the reasons the helpers above do and a few
    // more: no configuration is read or written, no crash handler is installed (it
    // would write reports of its own), no instance mutex is claimed, no window,
    // console, GL or radio is opened. The accepted arguments are enumerated by
    // runSentinelMain, which refuses anything else, and argv[1] must be the flag
    // itself, so no combination of arguments to a real session can turn it into a
    // watcher.
    if (argc >= 2 && std::strcmp(argv[1], "--sentinel") == 0) {
        return cascade::core::runSentinelMain(argc, argv);
    }

    constexpr const char* kCrashDirFlag = "--crash-dir=";
    constexpr const char* kDriverFlag = "--driver=";
    constexpr const char* kListDriversFlag = "--list-drivers";
    constexpr const char* kSkipFlag = "--skip=";
    // Which try the parent says this child is (2026-10-04): only the parent
    // knows, and the child's own crash report - the one with the stack, and now
    // the only one filed for a death it covers - says it on its reason line.
    constexpr const char* kAttemptFlag = "--attempt=";
    if (argc >= 2 && std::strcmp(argv[1], "--enumerate-json") == 0) {
        const char* crashDir = nullptr;
        const char* driver = nullptr;
        const char* skip = nullptr;
        bool listDrivers = false;
        int attempt = 0;
        for (int i = 2; i < argc; ++i) {
            if (std::strncmp(argv[i], kCrashDirFlag, std::strlen(kCrashDirFlag)) == 0) {
                crashDir = argv[i] + std::strlen(kCrashDirFlag);
            } else if (std::strncmp(argv[i], kDriverFlag, std::strlen(kDriverFlag)) == 0) {
                driver = argv[i] + std::strlen(kDriverFlag);
            } else if (std::strncmp(argv[i], kSkipFlag, std::strlen(kSkipFlag)) == 0) {
                skip = argv[i] + std::strlen(kSkipFlag);
            } else if (std::strncmp(argv[i], kAttemptFlag, std::strlen(kAttemptFlag)) == 0) {
                // A number or nothing: anything else leaves it 0, "not told".
                attempt = std::atoi(argv[i] + std::strlen(kAttemptFlag));
                if (attempt < 0) { attempt = 0; }
            } else if (std::strcmp(argv[i], kListDriversFlag) == 0) {
                listDrivers = true;
            } else {
                std::fprintf(stderr,
                             "cascade: --enumerate-json takes only %s, %s, %s, %s and %s\n",
                             kCrashDirFlag, kDriverFlag, kSkipFlag, kAttemptFlag,
                             kListDriversFlag);
                return 2;
            }
        }
        // ENDED, NOT RETURNED: returning from main runs ExitProcess, which
        // detaches every vendor module the walk loaded - and an ASIO driver
        // faulting in its detach killed a child that had already answered
        // (2026-10-01). See endEnumerateHelperProcess.
        cascade::source::endEnumerateHelperProcess(
            cascade::source::runEnumerateHelper(crashDir, driver, listDrivers, skip, attempt));
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// STEP 3 - the crash handlers and the diagnostics switch.
//
// POSITION: after the helpers (they return before this and must not inherit it)
// and BEFORE the config is read for any other purpose, the instance claimed or a
// flag parsed: the filters go on first because everything after this can fault,
// the config read included. Inside the step the order is the order of consent:
// REGISTER the filters with capture OFF, THEN read the stored switch under them,
// THEN turn capture on if the user wants it. The kind decides only two inputs
// (7b): whether this run may write at all, and which config the stored switch is
// read from - an Engine writes nothing and reads no config; the order is the same.
// ---------------------------------------------------------------------------
void armDiagnostics(ProcessKind kind, int argc, char** argv, BootResult& boot) {
    // FIRST, before anything that could fault has had the chance. The four
    // registrations are made unconditionally so the stderr attribution is
    // always available; only the on-disk half is conditional.
    //
    // TWO SEPARATE QUESTIONS, and conflating them was a bug. `mayWrite` is
    // "is this a real session rather than a tool"; `wanted` is "did the user
    // ask for diagnostics", read from the config right here rather than a
    // constructor and a window later. The directory is still handed over when
    // the run may write, so turning diagnostics on in Settings mid-session has
    // somewhere to go - but nothing is created until it is switched on.
    const bool mayWrite = diagnosticsOnDisk(kind, argc);
    cascade::core::CrashHandlerConfig crashCfg;
    if (mayWrite) { crashCfg.crashDir = cascade::core::diagCrashDir(); }
    // REGISTRATIONS FIRST, ON-DISK CAPTURE SECOND, and in that order for two
    // reasons. The switch cannot be known without reading the config, and
    // reading the config is itself something that can fault - so the filters go
    // on now, writing nothing, and the read below happens under them. Nothing
    // reaches the disk until the line after it.
    crashCfg.enabled = false;
    cascade::core::installCrashHandlers(crashCfg);

    const bool wanted = mayWrite && storedDiagnosticsEnabled(kind, argc, argv);
    // Creates the crashes directory if and only if `wanted`; see
    // crash_handler.cpp. The minidump switch stays off until AppWindow::run()
    // applies the user's - a dump is process memory and is never armed on a
    // guess.
    cascade::core::setCrashCaptureEnabled(wanted, false);
    cascade::core::DiagLog::instance().configure(
        wanted ? cascade::core::diagLogDir() : std::string(), wanted);
    cascade::core::diagLogf("FoxSDR %s (%s) starting", cascade::versionString(),
                            cascade::gitCommit());
    boot.mayWrite = mayWrite;
    boot.wanted = wanted;
}

// ---------------------------------------------------------------------------
// STEP 4 - the single-instance claim. HOST ONLY.
//
// POSITION: after the diagnostics are armed (a fault in the claim is captured)
// and BEFORE the argument parse and before any config is loaded for the session
// (PORTAL-LINK-VERDICT.md finding 3, below): the name is claimed as the first
// statement of a REAL run. An Engine never claims `Local\FoxSDR-instance`: that
// name answers "is a window application already on this desktop", it is the
// channel a foxsdr:// link is handed over on, and a headless service holding it
// would turn a tester's link click into a file nobody polls.
// ---------------------------------------------------------------------------
void claimInstance(ProcessKind kind, int argc, char** argv, BootResult& boot) {
    // KIND GATE: an Engine claims nothing; its `primaryInstance` stays false.
    if (kind != ProcessKind::Host) { return; }
    // --- beta-tester portal link activation (core/tester_link.hpp) ---------
    //
    // Checked here, before config is ever read and before the flag loop
    // below - a URL handed to us by the OS's protocol handler (or, on
    // Windows, an unpackaged install's registered shell\open\command) is not
    // a user-facing mode, it is an ACTIVATION, and PORTAL-LINK-VERDICT.md
    // finding 3 is specific about the ordering: the instance mutex must be
    // claimed as the first statement of a run, before config is read,
    // because config is loaded seconds before a listener-based design could
    // ever exist - claiming here means there is no such window at all.
    //
    // THE MUTEX IS CLAIMED UNCONDITIONALLY, for every launch that reaches
    // this point (every headless tool mode - --version, --selftest,
    // --soapy-check, --frames CI runs and the rest - returns within this
    // function well before a real session would ever be mistaken for
    // "already running", and each releases it at exit like any other
    // process-lifetime resource). FoxSDR's multi-instance design is
    // unaffected: this only ANSWERS "is one already running", it never
    // refuses a second instance the right to start.
    //
    // A TEST RUN MAY CLAIM A PRIVATE SCOPE instead (tester_link.hpp,
    // kInstanceScopeEnv): the CASCADE_CONFIG_TEST hook is passed on only for a
    // bounded --frames run - exactly startupConfigPath()'s rule - so a real
    // launch always claims the fixed, shared name whatever the environment says.
    std::string instanceTestHook;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0) {
            const char* hook = std::getenv("CASCADE_CONFIG_TEST");
            if (hook != nullptr) { instanceTestHook = hook; }
        }
    }
    const char* instanceScope = std::getenv(cascade::core::kInstanceScopeEnv);
    const bool primaryInstance = cascade::core::claimPrimaryInstance(
        std::filesystem::path(cascade::core::ConfigStore::defaultPath()).parent_path().string(),
        instanceTestHook, instanceScope != nullptr ? std::string(instanceScope) : std::string());
    boot.primaryInstance = primaryInstance;
}

// ---------------------------------------------------------------------------
// STEP 5 - the beta-tester link activation. HOST ONLY.
//
// POSITION: immediately after the claim, which it needs the answer of, and
// BEFORE the argument parse, which must not see argv[1] when argv[1] is a link
// (the unknown-argument branch would print the whole URL, token included, to
// stderr). An Engine never writes a link-request file and never acts on a link.
// It still must not ECHO one: a foxsdr: argv[1] is skipped by the parse and
// logged by length only, like the Host's untrusted-shape branch.
// ---------------------------------------------------------------------------
Step activateLink(ProcessKind kind, int argc, char** argv, BootResult& boot) {
    // KIND GATE: no claim, no file, no "hand it to the other instance" - and no echo.
    if (kind != ProcessKind::Host) {
        if (argc >= 2 && hasFoxsdrScheme(argv[1])) {
            boot.argv1IsLinkActivation = true;
            cascade::core::diagLogf("foxsdr: link ignored by a process that does not take links (len=%zu)",
                                    std::strlen(argv[1]));
        }
        return std::nullopt;
    }
    const bool primaryInstance = boot.primaryInstance;
    // Set below when argv[1] parsed as a real beta-link activation - the flag
    // loop starting a few lines down must then skip index 1 rather than hand
    // it to strcmp against every flag name, which is what used to happen: a
    // link clicked with FoxSDR already closed fell through to "no instance
    // was running" below, kept argv[1] as the URL, and the flag loop's
    // unknown-argument branch (see the trailing else below) printed the whole
    // URL - the token included - to stderr and exited 1. Neither half of that
    // was acceptable for a credential.
    bool argv1IsLinkActivation = false;
    if (argc >= 2) {
        const std::string linkToken = cascade::core::parseBetaLinkUrl(argv[1]);
        if (!linkToken.empty()) {
            argv1IsLinkActivation = true;
            // NEVER THE TOKEN ITSELF, only its length - matching
            // core::extractTesterToken's own out-of-log discipline for the
            // older portal-token flow.
            cascade::core::diagLogf("tester link received (len=%zu)", linkToken.size());
            const std::string configDir =
                std::filesystem::path(cascade::core::ConfigStore::defaultPath())
                    .parent_path()
                    .string();
            cascade::core::writeLinkRequestFile(configDir, linkToken);
            if (!primaryInstance) {
                // A GUI instance is already running: it claims this file on
                // its own ~1 Hz poll (AppWindow::testerLinkPoll). Nothing
                // else here - no window, no device scan, no crash sweep.
                return 0;
            }
            // No instance was running: fall through and continue as an
            // entirely normal launch, which consumes its own just-written
            // file after ConfigStore::load, through the same poll. argv[1]
            // itself is skipped below (argv1IsLinkActivation) rather than
            // parsed as a flag.
        } else if ([&] {
                       const char* want = "foxsdr:";
                       for (int k = 0; want[k] != '\0'; ++k) {
                           const char c = argv[1][k];
                           if (c == '\0') { return false; }
                           if (c != want[k] && c != want[k] - ('a' - 'A')) { return false; }
                       }
                       return true;
                   }()) {
            // A foxsdr: link in any shape parseBetaLinkUrl does not accept - a
            // browser's trailing slash, an upper-cased scheme, an extra
            // parameter. It is not a link we trust, so nothing is written,
            // but it may still carry a token: it must never reach the
            // unknown-argument branch, which would echo it to stderr and exit.
            // Dropped, logged without its text, and the launch carries on.
            argv1IsLinkActivation = true;
            cascade::core::diagLogf("unrecognised foxsdr: link ignored (len=%zu)",
                                    std::strlen(argv[1]));
        }
    }
    boot.argv1IsLinkActivation = argv1IsLinkActivation;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// STEP 6 - the argument parse.
//
// POSITION: after the claim and the link (argv[1] may already be consumed) and
// BEFORE the tool modes run, because a tool mode is chosen by the whole line: an
// unknown or malformed argument anywhere on it refuses the run before the check
// is started. The modes that need no further parsing (--version, --update-check)
// answer inside the loop and end the process there, as they always did.
// The tool modes and the two answering flags are the same for both kinds; the
// KIND decides only the rest (stage 7b): the Host takes the window's flags
// (--frames, --diag-stall, --diag-toggle, --link-tester) and refuses --seconds,
// as it always refused it; the Engine takes --seconds and refuses the window's,
// each as an unknown argument, with its own name in the message.
// ---------------------------------------------------------------------------
// The value of an Engine provisioning flag (stage 7e): the next argument, which must be there and must not be another flag
// (a forgotten value is not a file called "--seconds"). Null, after saying so, when it is not; `i` is moved onto it otherwise.
const char* provisionValue(int argc, char** argv, int& i, const char* flag, const char* needs) {
    if (i + 1 >= argc || argv[i + 1][0] == '\0' || std::strncmp(argv[i + 1], "--", 2) == 0) {
        std::fprintf(stderr, "foxsdr-engine: %s requires %s\n", flag, needs);
        return nullptr;
    }
    return argv[++i];
}

Step parseFlags(ProcessKind kind, int argc, char** argv, BootResult& boot) {
    const bool host = kind == ProcessKind::Host;
    const bool argv1IsLinkActivation = boot.argv1IsLinkActivation;
    int frames = -1;  // negative: run until the window is closed
    double seconds = -1.0;  // negative: run until stopped (Engine)
    std::string configArg;  // --config PATH (Engine)
    bool useConfig = false;  // --use-config (Engine)
    ProvisionRequest prov;   // --web-* and --cat-* (Engine, stage 7e)
    int diagStallMs = 0;
    int diagToggle = 0;
    bool selftest = false;
    bool soapyCheck = false;
    bool rtlsdrCheck = false;
    bool recordCheck = false;
    bool toneCheck = false;
    bool linkTesterReveal = false;
    double rdsCheckMhz = 0.0;
    // argv[1] was already consumed above as a beta-link activation (written
    // to the link-request file and logged by length only) - it is never a
    // flag, and must never reach strcmp/the unknown-argument branch below,
    // which would otherwise print the whole "foxsdr://beta?t=<token>" URL to
    // stderr and exit 1 for a link clicked while no instance was running.
    for (int i = argv1IsLinkActivation ? 2 : 1; i < argc; ++i) {
        if (host && std::strcmp(argv[i], "--frames") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "cascade: --frames requires an integer argument\n");
                return 1;
            }
            char* end = nullptr;
            const long value = std::strtol(argv[i + 1], &end, 10);
            // Reject trailing junk and negatives; cap so the int cast below
            // cannot overflow on LP64-style long values.
            if (end == argv[i + 1] || *end != '\0' || value < 0 || value > 1000000L) {
                std::fprintf(stderr, "cascade: invalid --frames value '%s'\n", argv[i + 1]);
                return 1;
            }
            frames = static_cast<int>(value);
            ++i;  // consumed the value argument
        } else if (std::strcmp(argv[i], "--update-check") == 0) {
            // Support tool, and the way this path is verified without driving
            // the GUI: ask the real endpoint what this build should be told.
            // Kept out of the usage string for the same reason as the other
            // diagnostics - it makes a network call.
            cascade::core::UpdateInfo info;
            std::string err;
            if (!cascade::core::checkForUpdate(cascade::core::updateEndpoint(),
                                               cascade::versionString(), "", info, err)) {
                std::printf("update-check FAIL %s\n", err.c_str());
                return 1;
            }
            std::printf("update-check running=%s latest=%s newer=%d critical=%d\n",
                        cascade::versionString(), info.version.c_str(), info.newer ? 1 : 0,
                        info.critical ? 1 : 0);
            if (info.newer) {
                std::printf("update-check url=%s\n", info.url.c_str());
                std::printf("update-check sha256=%s\n", info.sha256.c_str());
                // "--update-check download" runs the same download-and-verify
                // the button runs, without installing anything. It exists so
                // that path can be exercised on a machine where actually
                // installing over the top would be unwelcome, and so a support
                // conversation can establish whether a download verifies
                // before anyone reinstalls.
                if (i + 1 < argc && std::strcmp(argv[i + 1], "download") == 0) {
                    std::string path;
                    std::string derr;
                    if (!cascade::core::downloadUpdate(info, path, derr)) {
                        std::printf("update-check DOWNLOAD FAIL %s\n", derr.c_str());
                        return 1;
                    }
                    std::printf("update-check downloaded and verified: %s\n", path.c_str());
                }
                for (const cascade::core::ReleaseNote& n : info.notes) {
                    std::printf("update-check note %s (%s)%s\n", n.version.c_str(),
                                n.date.c_str(), n.critical ? " CRITICAL" : "");
                    for (const std::string& line : n.notes) {
                        std::printf("    - %s\n", line.c_str());
                    }
                }
            }
            return 0;
        } else if (std::strcmp(argv[i], "--version") == 0) {
            // One line, on stdout, and nothing else: this is what the release
            // and nightly scripts read back to check that the binary agrees
            // with the version its installer is about to be named after. A
            // nightly whose binary called itself by the release version would
            // produce bug reports naming a build that does not exist.
            std::printf("%s %s\n", cascade::appName(), cascade::versionString());
            return 0;
        } else if (std::strcmp(argv[i], "--selftest") == 0) {
            selftest = true;
        } else if (std::strcmp(argv[i], "--soapy-check") == 0) {
            // Hidden bench diagnostic (see runSoapyCheck): not in the usage
            // string on purpose — it requires attached hardware, so
            // advertising it in CI-facing help would only invite red herrings.
            soapyCheck = true;
        } else if (std::strcmp(argv[i], "--rtlsdr-check") == 0) {
            // The native counterpart to --soapy-check, and hidden for the
            // same reason: it needs a dongle bound to WinUSB, so advertising
            // it in CI-facing help would only invite red herrings. What it
            // answers is the one question the suite cannot - the register
            // sequences are proven against a fake, and whether THIS machine's
            // dongle enumerates, opens, answers its tuner probe and delivers
            // samples is a question only a run against it can settle. See
            // source/rtlsdr_check.cpp; exit 0 means samples arrived and were
            // neither silent nor saturated.
            rtlsdrCheck = true;
        } else if (std::strcmp(argv[i], "--record-check") == 0) {
            // Hidden bench diagnostic (see runRecordCheck): same policy as
            // --soapy-check — kept out of the usage string because its
            // ~1 s real-time sleep and disk writes make it a bench tool,
            // not part of the CI-facing CLI surface.
            recordCheck = true;
        } else if (std::strcmp(argv[i], "--rds-check") == 0) {
            // Hidden bench diagnostic (see runRdsCheck): needs a real radio
            // tuned to a real RDS broadcast, so never a ctest entry.
            if (i + 1 >= argc) {
                std::fprintf(stderr, "cascade: --rds-check requires a frequency in MHz\n");
                return 1;
            }
            char* end = nullptr;
            rdsCheckMhz = std::strtod(argv[i + 1], &end);
            if (end == argv[i + 1] || !(rdsCheckMhz > 0.0)) {
                std::fprintf(stderr, "cascade: invalid --rds-check frequency '%s'\n",
                             argv[i + 1]);
                return 1;
            }
            ++i;
        } else if (host && std::strcmp(argv[i], "--diag-stall") == 0) {
            // Hidden diagnostic: wedge the frame loop once, N ms into the run,
            // so the SHIPPED hang threshold can be proved against the REAL
            // frame loop rather than against a unit test of the watchdog
            // class. Kept out of the usage string because deliberately
            // freezing the application is not a user-facing feature.
            if (i + 1 >= argc) {
                std::fprintf(stderr, "cascade: --diag-stall requires a duration in ms\n");
                return 1;
            }
            char* end = nullptr;
            const long value = std::strtol(argv[i + 1], &end, 10);
            if (end == argv[i + 1] || *end != '\0' || value <= 0 || value > 600000L) {
                std::fprintf(stderr, "cascade: invalid --diag-stall value '%s'\n", argv[i + 1]);
                return 1;
            }
            diagStallMs = static_cast<int>(value);
            ++i;
        } else if (host && std::strcmp(argv[i], "--diag-toggle") == 0) {
            // Hidden diagnostic: flip Settings > Diagnostics mid-session, on
            // frame 30, through the same function the checkbox calls. The
            // switch's mid-session behaviour is a privacy promise made in
            // three documents, and it was broken for the watchdog precisely
            // because nothing could exercise the checkbox from a test. Kept
            // out of the usage string for the same reason --diag-stall is.
            if (i + 1 >= argc) {
                std::fprintf(stderr, "cascade: --diag-toggle requires 'on' or 'off'\n");
                return 1;
            }
            if (std::strcmp(argv[i + 1], "on") == 0) {
                diagToggle = 1;
            } else if (std::strcmp(argv[i + 1], "off") == 0) {
                diagToggle = -1;
            } else {
                std::fprintf(stderr, "cascade: invalid --diag-toggle value '%s'\n", argv[i + 1]);
                return 1;
            }
            ++i;
        } else if (std::strcmp(argv[i], "--tone-check") == 0) {
            // Hidden bench diagnostic (see runToneCheck): plays audible sound,
            // so it is a human-in-the-loop tool, never a ctest entry.
            toneCheck = true;
        } else if (host && std::strcmp(argv[i], "--link-tester") == 0) {
            // Hidden, session-only reveal of SYSTEM > Beta tester's paste box
            // when no link has been confirmed yet (core/tester_link.hpp,
            // PORTAL-LINK-VERDICT.md §6/finding 8) - for a tester whose
            // browser cannot or will not hand the app a foxsdr:// URL
            // (Linux, or a blocked/dismissed protocol prompt). DELIBERATELY
            // a bare flag, never --link-tester=<token>: a value here would
            // sit in shell history indefinitely, unlike a protocol
            // activation's argv[1], which is OS-delivered and transient.
            linkTesterReveal = true;
        } else if (!host && std::strcmp(argv[i], "--seconds") == 0) {
            // THE ENGINE'S BOUNDED RUN (stage 7b): run the engine this many
            // seconds, stop it in the order a stop has, exit 0. A decimal is
            // accepted (a test wants half a second); negatives, junk, NaN and
            // more than a year are refused, so a typo cannot become "for ever".
            // Without it an Engine runs until it is ended from outside (7d adds
            // the signal that ends it in order).
            if (i + 1 >= argc) {
                std::fprintf(stderr, "foxsdr-engine: --seconds requires a number of seconds\n");
                return 1;
            }
            char* end = nullptr;
            const double value = std::strtod(argv[i + 1], &end);
            if (end == argv[i + 1] || *end != '\0' || !(value >= 0.0) || value > 31536000.0) {
                std::fprintf(stderr, "foxsdr-engine: invalid --seconds value '%s'\n", argv[i + 1]);
                return 1;
            }
            seconds = value;
            ++i;
        } else if (!host && std::strcmp(argv[i], "--config") == 0) {
            // THE ENGINE'S SETTINGS FILE (stage 7c): this run reads, owns and saves that file. A value that
            // is another flag is a forgotten path, not a file called "--seconds".
            if (i + 1 >= argc || argv[i + 1][0] == '\0' || std::strncmp(argv[i + 1], "--", 2) == 0) {
                std::fprintf(stderr, "foxsdr-engine: --config requires the path of a settings file\n");
                return 1;
            }
            if (!configArg.empty()) {
                std::fprintf(stderr, "foxsdr-engine: --config was given twice\n");
                return 1;
            }
            configArg = argv[i + 1];
            ++i;
        } else if (!host && std::strcmp(argv[i], "--use-config") == 0) {
            // The per-user settings file the window application uses: the one shared file, behind a lock.
            useConfig = true;
        } else if (!host && std::strcmp(argv[i], "--web-enable") == 0) {
            // THE ENGINE'S PROVISIONING (stage 7e, engine/provision.hpp): the web server and the CAT listener, set up from
            // the command line because a service has no settings panel. Every value flag below needs the switch it belongs
            // to (checked after the loop, so their order on the line does not matter), and the PASSWORD IS NEVER A VALUE
            // HERE - --web-password-file names a file (or `-`, standard input), the rule of --link-tester above.
            prov.webEnable = true;
        } else if (!host && std::strcmp(argv[i], "--cat-enable") == 0) {
            prov.catEnable = true;
        } else if (!host && std::strcmp(argv[i], "--cat-bind-all") == 0) {
            prov.catBindAll = true;
        } else if (!host && std::strcmp(argv[i], "--web-bind") == 0) {
            const char* v = provisionValue(argc, argv, i, "--web-bind", "an address (an IPv4 address, ::1, :: or localhost)");
            if (v == nullptr) { return 1; }
            if (!prov.webBind.empty()) {
                std::fprintf(stderr, "foxsdr-engine: --web-bind was given twice\n");
                return 1;
            }
            // The policy's own predicate (net/web_policy.hpp): a hostname is not an address, and the line is refused
            // before the engine starts rather than after its plugin scan. (The empty string, which the predicate
            // accepts as "any", is refused above: a missing value is not a wildcard.)
            if (!cascade::net::isAcceptableBindAddress(v)) {
                std::fprintf(stderr,
                             "foxsdr-engine: invalid --web-bind value '%s' (an IPv4 address, ::1, :: or localhost)\n", v);
                return 1;
            }
            prov.webBind = v;
        } else if (!host && std::strcmp(argv[i], "--web-port") == 0) {
            const char* v = provisionValue(argc, argv, i, "--web-port", "a port number");
            if (v == nullptr) { return 1; }
            if (prov.webPort != 0) {
                std::fprintf(stderr, "foxsdr-engine: --web-port was given twice\n");
                return 1;
            }
            char* end = nullptr;
            const long port = std::strtol(v, &end, 10);
            if (end == v || *end != '\0' || port < cascade::net::kMinPort || port > cascade::net::kMaxPort) {
                std::fprintf(stderr, "foxsdr-engine: invalid --web-port value '%s' (a port from %d to %d)\n", v,
                             cascade::net::kMinPort, cascade::net::kMaxPort);
                return 1;
            }
            prov.webPort = static_cast<int>(port);
        } else if (!host && std::strcmp(argv[i], "--web-user") == 0) {
            const char* v = provisionValue(argc, argv, i, "--web-user", "a user name");
            if (v == nullptr) { return 1; }
            if (!prov.webUser.empty()) {
                std::fprintf(stderr, "foxsdr-engine: --web-user was given twice\n");
                return 1;
            }
            prov.webUser = v;
        } else if (!host && std::strcmp(argv[i], "--web-password-file") == 0) {
            // DELIBERATELY the name of a file, never the password: a value here would sit in shell history, in the
            // process list and in a service manager's unit file for as long as any of them lives.
            const char* v = provisionValue(argc, argv, i, "--web-password-file",
                                           "the name of a file holding the password, or - for standard input");
            if (v == nullptr) { return 1; }
            if (!prov.passwordFile.empty()) {
                std::fprintf(stderr, "foxsdr-engine: --web-password-file was given twice\n");
                return 1;
            }
            prov.passwordFile = v;
        } else if (!host) {
            // AN ARGUMENT WITH `=` IN IT IS SHOWN ONLY UP TO THE `=` (stage 7e): `--web-password=hunter2` is somebody
            // who thought the password goes on the command line - the one thing this refusal must not then print, into
            // a terminal, a journal or a pasted bug report. (No flag takes `=`; nothing that was accepted is hidden.)
            std::string shown = argv[i];
            if (const std::size_t eq = shown.find('='); eq != std::string::npos) {
                shown = shown.substr(0, eq) + "=<value not shown>";
            }
            std::fprintf(stderr,
                         "foxsdr-engine: unknown argument '%s' (usage: foxsdr-engine [--seconds N] "
                         "[--config PATH | --use-config] [--selftest] [--version] "
                         "[--web-enable [--web-bind ADDR] [--web-port N] [--web-user NAME] [--web-password-file FILE|-]] "
                         "[--cat-enable [--cat-bind-all]])\n",
                         shown.c_str());
            return 1;
        } else {
            std::fprintf(stderr,
                         "cascade: unknown argument '%s' (usage: cascade [--frames N] "
                         "[--selftest] [--version])\n",
                         argv[i]);
            return 1;
        }
    }

    // Two names for the settings file are one too many: which is meant?
    if (!configArg.empty() && useConfig) {
        std::fprintf(stderr, "foxsdr-engine: --config and --use-config both name the settings file; give one\n");
        return 1;
    }

    // A web value with no --web-enable, and --cat-bind-all with no --cat-enable, would be a setting that nothing
    // switches on: refused, not quietly dropped (the provisioning says what it does or says why not).
    {
        const char* orphan = nullptr;
        if (!prov.webEnable) {
            if (!prov.webBind.empty()) { orphan = "--web-bind"; }
            else if (prov.webPort != 0) { orphan = "--web-port"; }
            else if (!prov.webUser.empty()) { orphan = "--web-user"; }
            else if (!prov.passwordFile.empty()) { orphan = "--web-password-file"; }
        }
        if (orphan != nullptr) {
            std::fprintf(stderr, "foxsdr-engine: %s needs --web-enable\n", orphan);
            return 1;
        }
        if (prov.catBindAll && !prov.catEnable) {
            std::fprintf(stderr, "foxsdr-engine: --cat-bind-all needs --cat-enable\n");
            return 1;
        }
    }

    boot.frames = frames;
    boot.seconds = seconds;
    boot.provision = prov;
    boot.engineConfigArg = configArg;
    boot.engineUseConfig = useConfig;
    boot.diagStallMs = diagStallMs;
    boot.diagToggle = diagToggle;
    boot.linkTesterReveal = linkTesterReveal;
    boot.tool.selftest = selftest;
    boot.tool.soapyCheck = soapyCheck;
    boot.tool.rtlsdrCheck = rtlsdrCheck;
    boot.tool.recordCheck = recordCheck;
    boot.tool.toneCheck = toneCheck;
    boot.tool.rdsCheckMhz = rdsCheckMhz;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// STEP 8 - the config path.
//
// POSITION: after the tool modes (they use no config, and a refused run must not
// have decided one) and BEFORE the stderr capture and the application object:
// the path is an input of the window's constructor, which comes last.
// The Host's rule is below, unchanged. THE ENGINE'S (stage 7b) is the hermetic
// one: no config path at all - not the user's file, and not CASCADE_CONFIG_TEST
// either, which is the bounded window run's test hook - so a headless run reads
// nothing and writes nothing of the user's. Stage 7c adds the one door: a path is
// handed on only when the command line ASKED for one (--config PATH, or
// --use-config for the per-user file); a bare run is as hermetic as it was.
// ---------------------------------------------------------------------------
void wireConfig(ProcessKind kind, BootResult& boot) {
    if (kind != ProcessKind::Host) {
        // The Engine's rule (7c): a settings file only when ASKED for one on the command line.
        boot.configPath = engineSettingsPath(boot);
        boot.announceConfig = false;
        return;
    }
    const int frames = boot.frames;
    // Config persistence wiring. Normal interactive runs load/save the
    // user's config at ConfigStore::defaultPath(). Bounded --frames runs
    // stay hermetic (no load, no save — the CI contract) UNLESS the
    // CASCADE_CONFIG_TEST env var supplies an explicit path: that is the
    // documented test hook that forces a config load+save round trip against
    // a caller-owned file and switches on AppWindow's one-line
    // "config applied: ..." diagnostic. It is honored ONLY with --frames, so
    // no interactive run can be silently redirected by a stray env var.
    std::string configPath;
    bool announceConfig = false;
    if (frames >= 0) {
        const char* hook = std::getenv("CASCADE_CONFIG_TEST");
        if (hook != nullptr && *hook != '\0') {
            configPath = hook;
            announceConfig = true;
        }
    } else {
        configPath = cascade::core::ConfigStore::defaultPath();
    }
    boot.configPath = configPath;
    boot.announceConfig = announceConfig;
}

// ---------------------------------------------------------------------------
// STEP 9 - the stderr capture. HOST ONLY.
//
// POSITION: only for a real session, after every tool mode has returned and
// after the argument errors have had their stderr, and BEFORE anything of the
// application can print: librtlsdr, libusb and UHD warn on stderr from the
// moment a radio is touched. An Engine keeps its stderr: under a service
// manager it IS the journal, and the capture exists for a window nobody was
// reading (INFERRED for a service; the capture also declines a watched console).
// ---------------------------------------------------------------------------
void captureStderr(ProcessKind kind, const BootResult& boot) {
    // KIND GATE.
    if (kind != ProcessKind::Host) { return; }
    const int frames = boot.frames;
    // THE STDERR CAPTURE, and only here: a real session, after every tool
    // mode has returned and after the argument errors above have had their
    // stderr. librtlsdr, libusb and UHD print their warnings with
    // fprintf(stderr, ...) - "rtlsdr_read_async: dev_lost" is the whole
    // diagnosis of a radio going quiet, and until 0.89.0 it went to a window
    // nobody was reading. A --frames run is a test and keeps its stderr for
    // the harness; a developer at a terminal keeps theirs (the capture
    // declines a console someone else is attached to - see diag_log.hpp).
    if (frames < 0) { cascade::core::installStderrCapture(); }
}

// STEP 10 - the sentinel test seam.
//
// POSITION: read once, here, because the console handler and the sentinel below
// both follow it, and after the capture so a test run keeps the stderr it asked
// for. Kind-blind.
void readSentinelSeam(BootResult& boot) {
    const int frames = boot.frames;
    // A bounded run standing in for a real session under the sentinel seam
    // (tests/test_sentinel_app.cpp): honoured only with --frames, like every test
    // seam in this file. Read once, for the two things below that follow it.
    const char* sentinelHook = std::getenv("CASCADE_SENTINEL_TEST");
    const bool sentinelTest = frames >= 0 && sentinelHook != nullptr && sentinelHook[0] == '1';
    boot.sentinelTest = sentinelTest;
}

// ---------------------------------------------------------------------------
// STEP 11 - the console control handler. HOST ONLY.
//
// POSITION: after the tool modes and the stderr capture (a refused run never
// reaches it) and BEFORE the sentinel and the application object, so a close of
// the console during the plugin load already takes the orderly path. An Engine
// installs no console handler HERE: its terminate path is the signal path of stage
// 7d (SIGTERM/SIGINT/SIGHUP on Linux, this same handler on Windows), and it is armed
// in step 14, last, because on Linux it has to be installed after the exit recorder
// (step 13) to compose with it.
// ---------------------------------------------------------------------------
void installConsoleHandler(ProcessKind kind, const BootResult& boot) {
    // KIND GATE.
    if (kind != ProcessKind::Host) { return; }
    const int frames = boot.frames;
    const bool sentinelTest = boot.sentinelTest;
    // THE CONSOLE CONTROL HANDLER (0.99.67, core/console_close.hpp), for a real
    // session and for a bounded run under the sentinel seam, so the test can send
    // the event. Until 0.99.69 cascade.exe was a console-subsystem program, so a
    // Start Menu launch had a console window of its own; closing that window - or
    // Ctrl+C in it - ended the process before a line of the shutdown had run, and
    // the sentinel filed it as an ending from outside (field report, 2026-10-06).
    // That window is gone now, but a session started from a terminal still shares
    // the terminal's console (applyConsoleOwnership), and closing the terminal or
    // Ctrl+C in it is the same event: it is the close the window's own button asks for.
    if (frames < 0 || sentinelTest) { cascade::core::installConsoleCloseHandler(); }
}

// STEP 12 - what applyConsoleOwnership did, in the log.
//
// POSITION: after the diagnostics log exists (step 3) so the line has somewhere
// to go, and after the console handler so one line tells the next report the whole
// console story. Kind-blind.
void logConsoleOutcome() {
    // What applyConsoleOwnership did at the top, in the log, for the next report
    // that starts "a black window appeared" (or "closing it killed the program").
    // Windows only: elsewhere there is no console to own and nothing to say.
#if defined(_WIN32)
    {
        const cascade::core::ConsoleOutcome co = cascade::core::consoleOutcome();
        const char* did = "kept";
        if (co.decision.action == cascade::core::ConsoleAction::Attach) {
            did = co.attached ? "attached to the parent's" : "none, and the parent has none";
        }
        cascade::core::diagLogf("console: %s (%s)", did, co.decision.reason);
    }
#endif
}

// ---------------------------------------------------------------------------
// STEP 13 - the sentinel.
//
// POSITION: LAST, after every tool mode has returned and immediately before the
// application object exists, so the stretch with the most to go wrong - the
// plugin load, the window, the first frame - is watched, and nothing that can
// still refuse the run has started a watcher. Kind-blind.
// ---------------------------------------------------------------------------
void armSentinel(const BootResult& boot) {
    const int frames = boot.frames;
    const bool mayWrite = boot.mayWrite;
    const bool wanted = boot.wanted;
    const bool sentinelTest = boot.sentinelTest;
    // THE SENTINEL, for an interactive session only (core/sentinel.hpp): started
    // HERE, after every tool mode has returned and before the application object
    // exists, so that the stretch with the most to go wrong - the plugin load, the
    // window, the first frame - is watched. A bounded --frames run never starts one
    // unless a test says so with CASCADE_SENTINEL_TEST, honoured only with --frames
    // and only when the run may write diagnostics at all, like every test seam in
    // this file. It follows the Diagnostics switch exactly: `wanted` is the stored
    // switch, and the Settings checkbox starts and ends it through the same call
    // that arms every other part of diagnostics (AppWindow::applyDiagnosticsEnabled).
    {
        if (mayWrite && (frames < 0 || sentinelTest)) {
            cascade::core::SentinelOptions so;
            so.crashDir = cascade::core::diagCrashDir();
            so.logDir = cascade::core::diagLogDir();
            // HOW THIS PROCESS ENDS, written down for the watcher (0.99.78, core/exit_record.hpp):
            // a Linux watcher is this process's child and cannot ask the kernel. A no-op where
            // the watcher reads the exit code from the operating system (Windows).
            cascade::core::installExitRecording();
            cascade::core::sentinelConfigure(so);
            cascade::core::sentinelSetEnabled(wanted);
            cascade::core::breadcrumb::setPhase(cascade::core::breadcrumb::Phase::BuildingApp);
        }
    }
}

// ---------------------------------------------------------------------------
// STEP 14 - the stop signals. ENGINE ONLY (stage 7d).
//
// POSITION: LAST, after the sentinel and the exit recorder it arms (step 13),
// and for one reason: on Linux the recorder installs its handler on SIGTERM,
// SIGINT and SIGHUP (core/exit_record.hpp) when they are still at the default,
// and the stop signals must find that handler beneath them and keep it, not
// displace it or be skipped by it. Installed here, a first signal only asks for
// the orderly stop and a later one is handed back to the recorder (which writes
// down "ended by signal N" and lets the signal end the process); installed
// before step 13 the recorder would have declined the signals as already
// handled and a second one would end the process unrecorded
// (core/stop_signal.hpp). On Windows this is the console handler step 11 gives
// the Host, the same one. After every tool mode and helper, so a refused or
// helper run never has a handler; before the engine exists, so a signal during
// the restore is held and acted on at the engine's first wait.
//
// The Host does not take part: its stop is the window's close (step 11).
// ---------------------------------------------------------------------------
void armStopSignals(ProcessKind kind) {
    // KIND GATE.
    if (kind != ProcessKind::Engine) { return; }
    if (!cascade::core::installStopSignals()) {
        // The engine runs without a way to be stopped but a kill, and says so: a service manager's stop would then end
        // it unrecorded, with the marker left FALSE.
        std::fprintf(stderr, "foxsdr-engine: could not install the stop signal handlers; only a kill will end this run\n");
    }
}

}  // namespace

// THE SEQUENCE. One line per step, in the order that is the contract; the step
// functions above carry the reasons. Do not reorder without reading them.
BootResult bootProcess(ProcessKind kind, int argc, char** argv) {
    // THE CONSOLE, before anything is printed or opened (0.99.69, core/console_owner.hpp).
    // cascade.exe is a windows-subsystem program (CMakeLists.txt), so a launch from
    // the Start Menu or Explorer gets no console and no black window beside the
    // real one - until 0.99.69 it did, and on a desktop with Windows Terminal as
    // the default host that was a Windows Terminal window for the whole session.
    // The tool modes below still print, so a process started from a shell borrows
    // the shell's console here (AttachConsole) and gets its standard handles back;
    // one started from Explorer finds no parent console and has none. FIRST in
    // main(), above the helper modes, because every line this function prints
    // afterwards must already know where it is printing.
    cascade::core::applyConsoleOwnership();
    BootResult boot;
    const auto finish = [&boot](int code) {
        boot.exit = true;
        boot.exitCode = code;
        return boot;
    };
    // STEP 2: the helper modes return before anything is armed.
    if (const Step done = runHelperModes(argc, argv)) { return finish(*done); }
    // STEP 3: the crash handlers, then the config read under them, then capture.
    armDiagnostics(kind, argc, argv, boot);
    // STEP 4: the instance name is claimed before the flags are parsed.
    claimInstance(kind, argc, argv, boot);
    // STEP 5: a foxsdr:// link in argv[1] is consumed before the parse sees it.
    if (const Step done = activateLink(kind, argc, argv, boot)) { return finish(*done); }
    // STEP 6: the argument parse.
    if (const Step done = parseFlags(kind, argc, argv, boot)) { return finish(*done); }
    // STEP 7: the tool modes (headless; a window would defeat them).
    if (const Step done = runToolMode(boot.tool, argc, argv)) { return finish(*done); }
    // STEP 8: the config path the window will load.
    wireConfig(kind, boot);
    // STEP 9: the stderr capture (Host).
    captureStderr(kind, boot);
    // STEP 10: the sentinel test seam, read once.
    readSentinelSeam(boot);
    // STEP 11: the console control handler (Host).
    installConsoleHandler(kind, boot);
    // STEP 12: the console outcome, in the log.
    logConsoleOutcome();
    // STEP 13: the sentinel, last before the application exists.
    armSentinel(boot);
    // STEP 14: the stop signals (Engine), after the exit recorder they compose with.
    armStopSignals(kind);
    return boot;
}

}  // namespace cascade::engine
