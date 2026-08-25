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

// Rebuild the snapshot. Call from the healthy path only - start-up, after a
// plugin loads, after a device is opened. Returns the number of modules
// captured (capped; see kMaxDiagModules in the .cpp).
int refreshModuleTable();

int moduleCount();
bool moduleAt(int index, DiagModule& out);

// Linear search of the snapshot. False when `addr` belongs to no module
// currently in the table, in which case the caller reports the raw address.
bool resolveAddress(std::uintptr_t addr, DiagModule& out, std::uintptr_t& offset);

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
    std::vector<std::string> plugins;  // "name version", loaded plugins only
};

// Renders `ctx` into a fixed static buffer, ONCE, on the healthy path. The
// fault path writes those bytes out and formats nothing. Safe to call as
// often as the application likes; the cost is one snprintf of a few hundred
// bytes.
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
};

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
