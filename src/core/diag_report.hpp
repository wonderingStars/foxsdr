// diag_report.hpp - what a fault report contains, and the module table that
// makes it symbolisable.
//
// THE GOVERNING REQUIREMENT. A report has to be enough for an engineer who
// was not there to diagnose the fault, fix it and ship an update, with the
// user out of the loop. That rules out "it crashed" and it rules out a raw
// address: a captured stack is module+offset, and an offset is unreadable hex
// forever unless the PDB that matches THAT EXACT BINARY can be found again.
//
// WHY THE BUILD ID AND NOT THE VERSION. Two builds of "0.61.0" - a rebuild
// after a one-line fix, a release and its nightly, an /MT and an /MD variant -
// have different code at the same offsets and different PDBs. The durable key
// is the CodeView RSDS record the linker stamps into the PE: a GUID plus an
// age counter, unique per link, recorded identically in the binary and in its
// PDB. Every module in a report carries its own, so a future session can find
// the right symbols for a report it has never seen before. See
// docs/DIAGNOSTICS.md for where the archive lives.
//
// WHY THE MODULE TABLE IS SNAPSHOTTED IN ADVANCE. Resolving an address to
// module+offset means walking the loader's module list, and the crash handler
// is the one place that must not do that: EnumProcessModules and the loader
// data it reads are guarded by the loader lock, and a fault that happened
// while another thread held that lock would turn into a deadlock inside the
// reporter. So the table is built on the HEALTHY path - at start-up, and again
// after anything that loads code (plugins, SoapySDR vendor modules) - into
// fixed storage, and the fault path does nothing but a linear search of an
// array it already has. A module loaded after the last refresh resolves as
// "?" with the raw address preserved, which is a known and stated limit
// rather than a hang.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_DIAG_REPORT_HPP
#define CASCADE_CORE_DIAG_REPORT_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/diag_history.hpp"

namespace cascade::core {

// One loaded module, in the form the fault path needs it. Fixed-size char
// arrays rather than std::string on purpose: this whole structure has to be
// readable from a handler running on a corrupted heap.
struct DiagModule {
    std::uintptr_t base = 0;
    std::size_t size = 0;
    char name[64] = {};     // file name only, e.g. "cascade.exe"
    char pdb[64] = {};      // file name of the PDB the linker recorded
    char buildId[48] = {};  // 32 hex GUID digits + the age, uppercase hex
};

// Rebuild the snapshot. Call from the healthy path only, and after anything
// that loaded code into this process. The shipped call sites are, in full:
//
//   - start-up (crash_handler.cpp and hang_watchdog.cpp, each only if the
//     table is still empty),
//   - AppWindow::rescanPlugins() and AppWindow::refreshDiagContext(), which
//     between them cover every plugin load and unload,
//   - AppWindow::pollSoapyAsync(), when a device open completes. The vendor
//     module and its dependencies (rtlsdrSupport.dll, rtlsdr.dll, libusb)
//     are mapped by SoapySDR::Device::make() on the open worker, so a table
//     built at start-up cannot name them. This line was missing until 0.75.0
//     while this comment already promised it: a field report whose radio was
//     opened 102 s after launch printed all three as bare addresses, and the
//     frame that identified the fault had to be recovered by hand.
//
//   - the enumeration CHILD (soapy_enum_proc, runEnumerateHelper), through
//     SoapySource::setModulesLoadedHook: once its walk has mapped the vendor
//     modules and before any probe runs, so its own report of a probe fault
//     names the module (2026-10-01; it used to say "?" and hash to
//     650B88A1735695DB).
//
// No list of call sites can be complete, because code is mapped by things this
// file cannot see: a vendor module's own LoadLibrary in the middle of a probe
// (RtAudio mapping an ASIO driver) arrives after every refresh above. That is
// what adoptModuleContaining, below, is for - the fault path names such a
// module itself, so the table need not have been refreshed for it - and
// describeModuleContaining is the same for the freeze report, which may not
// write to the table.
//
// The device SCAN needs no entry in the application - it runs in that child
// process, so no vendor module is mapped into this one.
//
// Returns the number of modules captured (capped; see kMaxDiagModules in the
// .cpp).
int refreshModuleTable();

int moduleCount();
bool moduleAt(int index, DiagModule& out);

// Linear search of the snapshot. False when `addr` belongs to no module
// currently in the table, in which case the caller reports the raw address.
bool resolveAddress(std::uintptr_t addr, DiagModule& out, std::uintptr_t& offset);

// A MODULE MAPPED AFTER THE LAST refreshModuleTable(), named from the FAULT
// PATH (2026-10-04). The snapshot cannot know about code that arrived after it
// was taken, and refreshing it from a handler means the loader lock - so a
// fault in such code used to report a bare address, list no module for it, and
// hash to the signature every unresolved fault of that code shares. That is the
// field report of 2026-10-01: a Native Instruments ASIO driver, which RtAudio
// maps in the middle of SoapyAudio's probe, long after the enumeration child
// had refreshed its table for the vendor modules (setModulesLoadedHook), was
// nine frames named "-".
//
// If `addr` is inside a mapped IMAGE the table does not yet cover, this appends
// that image - file name, base, size - and returns true; true as well when the
// table already covered it. False for anything that is not an image (heap, JIT
// page, stack: nothing to name, and nothing is invented), when the table is
// full, and on every non-Windows platform, whose handler still has only what
// refreshModuleTable() saw.
//
// FAULT-PATH SAFE, and held to the handler's rules: no allocation, no lock, no
// CRT formatting. VirtualQuery and NtQueryVirtualMemory read the address
// space's own bookkeeping and take neither the loader lock nor the heap lock;
// the module's headers are read behind a __try; the scratch is static, which
// is safe because the fault path admits one writer at a time. The entry has no
// pdb and no build id (reading the CodeView record means formatting it); it
// lasts until the next refreshModuleTable(), which rebuilds the table.
bool adoptModuleContaining(std::uintptr_t addr);

// THE SAME NAMING, FOR A WRITER THAT MUST NOT TOUCH THE TABLE: the freeze
// report (core/hang_watchdog.cpp). It had the crash handler's blind spot - a
// stalled thread inside code mapped after the last refreshModuleTable() printed
// bare addresses, and the display-stall classification, which reads module
// names off the top frames, could not see a graphics driver the table had never
// heard of - and adoptModuleContaining is the wrong tool for it: it APPENDS to
// the table, whose one-writer rule is the fault path's, while the watchdog
// thread runs beside a GUI thread that rebuilds the table at the end of every
// plugin rescan. A racing append could drop modules from the table, and the
// next crash report would pay for it.
//
// So this fills `out` and `offset` and writes nothing shared. True when the
// table already covered `addr` (the ordinary case, answered by resolveAddress)
// or when `addr` lies in a mapped IMAGE it does not cover; false for anything
// that is not an image (heap, stack, a JIT page: nothing to name, and nothing is
// invented), and on every non-Windows platform. The entry carries a name, a base
// and a size and no pdb or build id, for adoptModuleContaining's reason.
//
// SAFE WHERE THE WATCHDOG USES IT: VirtualQuery and NtQueryVirtualMemory take
// neither the loader lock nor the heap lock, and the module's headers are read
// behind a __try. It needs prepareModuleAdoption() to have run; without it the
// module is named "unknown-image" with its base, size and offset - still more
// than a bare address.
bool describeModuleContaining(std::uintptr_t addr, DiagModule& out, std::uintptr_t& offset);

// The healthy-path half of adoptModuleContaining: resolves the one ntdll entry
// point it needs, because GetProcAddress from a handler is a call into the
// loader. Called by installCrashHandlers and by HangWatchdog::start; harmless to
// call again.
void prepareModuleAdoption();

// The CodeView build id of a PE ON DISK - the same value refreshModuleTable
// reads out of the mapped image. This is what tools/archive-symbols.ps1 keys
// the archive by, and what a test uses to prove a report could be symbolised.
// False when the file is not a PE or carries no RSDS debug record (a build
// with PDBs turned off, which is exactly the state this feature exists to
// prevent shipping).
bool peBuildId(const std::string& path, std::string& buildId, std::string& pdbName);

// ---------------------------------------------------------------------------
// Application context
// ---------------------------------------------------------------------------
//
// "Which plugin was loaded" has already been the answer to real faults in this
// product - plugins are third-party code running in-process - so the plugin
// list with versions is context, not decoration. Everything here is state the
// application already knows; none of it is re-derived.
//
// THE SOUND PATH (0.99.61). "I am getting no audio from my speakers", with a
// log of five minutes and a receiver delivering every sample, was
// undiagnosable: a stream that never opened, one that opened and died, a
// squelch above the signal, the Mute key, a decoder plugin muting the audio on
// its preset and a volume of nothing are all silence from a healthy-looking
// radio, and the bundle recorded none of them. These are those facts, as plain
// state the window already holds. None of them is a frequency, and none names
// a device: the host API is a driver model ("MME", "Windows WASAPI"), while a
// device's own name is an operating-system label that is often a person's name
// ("Headset (Alice's AirPods Pro)").
struct DiagAudio {
    bool known = false;       // false: nothing filled this in (headless, a test)
    bool opening = false;     // a worker is inside the driver's open right now
    bool everOpened = false;  // an output device has opened at least once
    bool alive = false;       // ...and its stream is still being served
    int channels = 0;         // 1 or 2, of the open stream
    std::string hostApi;      // "MME", "Windows WASAPI"; never the device's name
    unsigned restarts = 0;    // times the watchdog reopened a dead stream
    int volumePercent = 100;  // the volume control, 0-100
    bool mutedByUser = false;      // the Mute key
    bool mutedByPlugin = false;    // a decoder plugin parked on its preset
    bool mutedByTransmit = false;  // the transmit key is down
    double squelchDb = 0.0;        // the squelch threshold, dB on the channel power
    bool squelchOpen = false;      // the gate's own state (Pipeline::squelchOpen)
    double signalDb = -200.0;      // the channel power the gate is judging; <= -199: none yet
};

struct DiagContext {
    std::string version;     // "0.61.0", or the full nightly string
    std::string commit;      // short git SHA the binary was built from
    std::string os;          // "Windows 10.0.22631"
    std::string arch;        // "x64"
    std::string mode;        // demodulator, e.g. "WFM"
    std::string sourceKind;  // "generator" | "iqfile" | "soapy"
    double sampleRateHz = 0.0;
    bool deviceOpen = false;
    std::string sdrModel;              // serial-stripped, see sanitiseDevice()
    // The crystal correction (0.99.56): "off", "not applicable", or the value
    // and how it is applied - "+1.5 in the radio", "+1.5 by retuning". A
    // property of the radio's crystal, never a frequency.
    std::string ppm = "off";
    // THE RADIOS THE PATCH PAGE HAS RUNNING (0.99.62), by DRIVER KIND ONLY - one
    // entry per running radio: "rtlsdr", "soapy", "sdrplay", "iqfile",
    // "siggen". Never a label, a serial, the arguments the radio was opened
    // with or a frequency. A second signal path is invisible in every other
    // line of the block (`source` describes the receiver, which the patch page
    // may have handed its radio to), and a fault on a patch radio's thread then
    // reads as a fault with no radio at all: the 0.99.59 report of a SoapySDR
    // `sdrplay` radio opened by the patch page said `source: siggen` and
    // `device-open: no`. Each entry is reduced to lower-case letters and digits
    // when it is rendered, so a caller that is handed something odd cannot put
    // it in a report.
    std::vector<std::string> patchRadioKinds;
    std::vector<std::string> plugins;  // "name version", loaded plugins only
    DiagAudio audio;                   // the sound path, see DiagAudio
};

// Renders `ctx` into a fixed static buffer, on the healthy path. The fault path
// writes those bytes out and formats nothing. Safe to call as often as the
// application likes - AppWindow calls it every frame and after every event that
// changes what it says (AppWindow::refreshDiagContext) - because a call whose
// rendering is byte-for-byte what the buffer already holds writes nothing: the
// cost of an unchanged block is building the text, and the buffer a fault
// handler may be reading at that instant is not touched.
void setDiagContext(const DiagContext& ctx);

// The rendered block, for tests and for the bundle. Empty until the first
// setDiagContext call.
std::string diagContextBlock();

// THE SAME BYTES, for the fault path: a pointer into the fixed storage above
// and its length, with no allocation. diagContextBlock() returns a std::string
// and a std::string means a heap allocation, which is the one thing a crash
// handler running on a possibly-corrupt heap must not do.
const char* diagContextRaw(int& lenOut);

// ---------------------------------------------------------------------------
// Grouping
// ---------------------------------------------------------------------------
//
// A stable signature is what turns 400 reports into "three bugs". It is
// deliberately built from the fault kind, the faulting module and the offset
// within that module, and NOT from the absolute address (ASLR moves it every
// run) or the timestamp (unique by construction). Two runs of the same binary
// failing the same way produce the same string; a different build of the same
// source produces a different one, which is correct - the offsets differ, and
// so do the symbols needed to read them.
std::string crashSignature(unsigned long code, const char* moduleName,
                           std::uintptr_t offset);

// The same value, for the fault path: no allocation, no CRT formatting (a
// locale lock is still a lock), 16 uppercase hex digits and a terminator into
// storage the caller already owns. crashSignature() is a thin wrapper over
// this, so the string a test compares is byte-for-byte the one a crash report
// carries - two implementations of one signature would be a grouping bug
// nobody would ever see.
void crashSignatureRaw(unsigned long code, const char* moduleName,
                       std::uintptr_t offset, char out[17]);

// THE MAIN EXECUTABLE'S OWN IMAGE (0.99.62), for the question "is this frame
// ours?". The base address of the running program: GetModuleHandle(nullptr) on
// Windows (read from the process block, no loader lock) and module 0 of the
// snapshot on POSIX, which dl_iterate_phdr always lists first. 0 when it cannot
// be told - a POSIX non-PIE executable has a load bias of 0 - and every caller
// treats 0 as "no frame is ours", which is the safe direction.
std::uintptr_t mainImageBase();

// True when `addr` lies inside the main executable, by the module snapshot.
bool inMainImage(std::uintptr_t addr);

// THIS PROCESS'S OWN EXECUTABLE AS A MODULE-TABLE ENTRY (0.99.66), read straight from
// its mapped image and WITHOUT touching the shared table: file name (the leaf), base,
// size, the PDB's file name and the CodeView build id - the same values
// refreshModuleTable() would give the first entry, and so the same `build=` a crash
// report writes for it. It exists for the sentinel, which is a second copy of the
// application's own executable (core/sentinel.hpp) and so knows the application's
// build id without reading anything of the application. Healthy path only. False
// where it cannot be told - on every non-Windows platform, where nothing asks - and
// then `out` is untouched; `buildId` is empty when the image carries no CodeView record.
bool describeMainModule(DiagModule& out);

// THE SIGNATURE OF A FREEZE (kind hang or stall), over the stalled thread's
// frames, top first.
//
// It identifies the code of OURS that was waiting, not the kernel stub it was
// waiting in. A frozen GUI thread is almost always parked in the same few wait
// stubs (ntdll.dll's NtWaitForSingleObject, win32u.dll's message wait), so a key
// built from frame 0 gave every freeze one signature whatever had stopped, and
// the uploader's 24-hour de-duplication then dropped the second of two different
// freezes as a repeat of the first. So: the hash is `kindTag` plus the module
// and the module-relative offset of the FIRST frame (nearest the top) that lies
// in the main executable. Falls back to frame 0 - module and offset, or "?" and 0
// when it names no module - when no frame of the stack is ours: a thread parked
// on a stack with nothing of this program on it has nothing better to be keyed by.
//
// The offset is build-specific, so one freeze in two builds is two signatures.
// That has always been true of a crash signature and is correct (the offsets
// differ, and so do the symbols needed to read them).
//
// `kindTag` keeps a display stall ('STAL') out of the group of a hang ('HANG'),
// whatever frame the two share. A frame whose top is already in the main
// executable hashes exactly as the old key did. Healthy-path only: it takes a
// std::string and is not for a fault handler. The result is 16 uppercase hex
// digits, the same form as crashSignature().
std::string freezeSignature(unsigned long kindTag, const std::uintptr_t* frames, int count);

// ---------------------------------------------------------------------------
// The "copy diagnostics" bundle
// ---------------------------------------------------------------------------
struct DiagBundleInput {
    DiagContext context;
    std::vector<std::string> logLines;
    std::string logPath;
    std::string crashDir;
    bool lastRunUnclean = false;  // reuses telemetryCleanExit, see PRIVACY.md
    std::uint64_t launches = 0;
    std::uint64_t crashes = 0;
    std::uint64_t logLinesTotal = 0;
    // What Windows says the SDRplay API Service is doing, in
    // source::sdrPlayServiceSummary()'s words - "running, auto start
    // (SDRplayAPIService)", "not installed", "not applicable" (0.99.55).
    // Bundle-only, not in the crash context: it is asked of the Service
    // Control Manager, which a fault path must not do. Empty prints "(none)".
    std::string sdrPlayService;
    // THE SESSIONS BEFORE THIS ONE (0.99.62): the end of the previous session's
    // log and one line per crash or freeze report on the machine, written after
    // the current log under headings of their own. `history.included` false -
    // diagnostics off, or nothing asked for it - adds neither section, not even a
    // heading, and nothing was read to fill it. See core/diag_history.hpp for why.
    DiagHistory history;
};

// The headings the two history sections are written under. The problem-report
// attachment's truncation (problem_report.cpp) keeps the NEWEST lines after the
// log marker, which is where these sit, so they outlast the current log's oldest
// lines when a bundle is cut to its size cap.
inline constexpr const char* kPreviousSessionHeading =
    "--- previous session (the end of its log) ---";
inline constexpr const char* kReportsHeading = "--- reports on this machine ---";

std::string buildDiagnosticsBundle(const DiagBundleInput& in);

// THE FIELD INVENTORY, in the same spirit as TelemetryReport's: PRIVACY.md
// documents what a bundle contains field by field, and a test asserts the
// bundle matches this list exactly. A new field cannot be added without the
// test failing and the document being updated with it.
const std::vector<std::string>& bundleFieldNames();

// THE SAME INVENTORY, FOR THE TWO WRITERS THAT PRODUCE THE MORE REVEALING
// DOCUMENT. The bundle is what a user chooses to copy; a crash or hang report
// is written without anyone watching, and PRIVACY.md lists its fields and
// claims - in those words - that the list is asserted "in both directions".
// It was not: the set comparison covered buildDiagnosticsBundle() only, and
// the header lines crash_handler.cpp and hang_watchdog.cpp write were asserted
// PRESENT but never EXHAUSTIVE. A line added to either writer - a command
// line, a tuned frequency - would have shipped undocumented with every test
// green, which is exactly the failure the inventory exists to prevent.
//
// These are the header fields only, the "name: value" lines each writer emits
// before its first "--- section ---" marker. Everything after that marker is
// either the shared context block (inventoried through bundleFieldNames(),
// because the bundle reuses the same bytes) or free-form program addresses.
const std::vector<std::string>& crashReportFieldNames();
const std::vector<std::string>& hangReportFieldNames();

}  // namespace cascade::core

#endif  // CASCADE_CORE_DIAG_REPORT_HPP
