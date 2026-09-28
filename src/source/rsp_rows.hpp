// rsp_rows.hpp - one radio, one row in the Source list.
//
// WHAT THIS IS FOR. FoxSDR can reach an SDRplay RSP two ways: natively, over
// USB, because an RSP is a Mirics MSi2500 + MSi001 pair (source/msi2500.hpp);
// and through SDRplay's own API, if the user has installed it
// (source/sdrplay_source.cpp). Both enumerations run, so an RSP owner with the
// API installed was offered the SAME RADIO TWICE, under two names, with
// nothing on the screen saying which to pick - and picking wrong means the
// less capable of the two. 0.99.9's correction to the product ids made the
// pair more visible, because both rows finally called the device by the same
// model name.
//
// THE OWNER'S DECISION (2026-09-21): hide the native row. The API is the
// supported route for every RSP - it is the only one that reaches an RSPduo,
// RSPdx, RSP1B or RSPdx-R2 at all, and on the models this driver does know it
// still drives front-end hardware the native path cannot.
//
// REVISED (2026-09-28, after two RSP1A field reports and a code review):
// the row is now hidden WHENEVER THE API IS INSTALLED, full stop - not only
// when the API's own enumeration currently lists something. The original
// narrowing ("only when the API actually listed something, so a stopped
// service still has the native row as a way in") was built on the belief
// that the native row is a working fallback when the API is briefly
// unavailable. It is not: the two field reports show the SAME radio opening
// fine through the API and then, the moment the API answered
// sdrplay_api_Fail once, the "fallback" to the native row failing outright
// with a raw Windows error 87 partway through the open sequence - see
// mirisdr_source.cpp's guard and scratchpad/bugs0928/sdrplay/mirics-windex.md
// for what is and is not established about why. Whatever the exact cause,
// showing the row and letting the user click it during exactly the window
// when the API is having trouble is not a rescue path, it is a trap: the
// row looks identical to the one that just worked, and clicking it can only
// fail.
//
// WINDOWS ONLY (Round 3 of the same review, same day). The mechanism
// mirics-windex.md lays out for WHY native access fails - a control
// transfer's recipient bits naming an endpoint that WinUSB validates and
// does not find - is a Windows USB-stack fact; it says nothing about Linux,
// where usbfs's own check_ctrlrecip skips that validation for any
// vendor-type request (see the doc). A Linux user's native row may
// genuinely work as a fallback while the SDRplay daemon is merely stopped,
// which is exactly the case the ORIGINAL (2026-09-21) narrower rule was
// written to protect - and two Windows-only field reports do not justify
// taking that away from Linux. So:
//
//   - Only a row this driver believes is an SDRplay unit is ever hidden. A
//     Mirics television stick shares the driver and must always be listed.
//   - ON WINDOWS: hidden whenever source::sdrPlayApiPresent() is true,
//     REGARDLESS of whether this scan's enumeration listed anything - a
//     service that is merely slow to answer looks exactly like one that is
//     not running, and the row must not flicker back into a state that only
//     fails.
//   - ON LINUX: the original, narrower rule - hidden only when the API's
//     enumeration actually lists the same radio (apiAlreadyLists). A row the
//     API cannot currently see keeps its native row as a way in, same as
//     before this whole investigation started.
//   - The one thing the Windows rule trades away: a radio the API genuinely
//     cannot see this session (the service is down, or a serial the API
//     does not name) no longer has ANY row to open it from, on Windows.
//     anyRspOrphanedByHiding() below answers exactly that question, pure,
//     so the caller can put an actionable sentence where the row would have
//     been instead of leaving silence - sdrPlaySourceAdvice() decides when
//     that sentence should actually win over a more specific one (a lost
//     session, a held-off enumeration, an old API version all say more than
//     "did not list this radio" and take priority over it).
//
// See kNativeMiricsOpenUnsafeWithApi for the platform switch and why it is a
// parameter rather than a bare #ifdef.
//
// PURE, so tests/test_rsp_rows.cpp can drive every one of those cases without
// an API, a radio, or a USB bus.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_SOURCE_RSP_ROWS_HPP
#define CASCADE_SOURCE_RSP_ROWS_HPP

#include <string>
#include <vector>

#include "core/i18n.hpp"
#include "source/device_source.hpp"
#include "source/sdrplay_source.hpp"

namespace cascade::source {

// WHETHER HIDING THE NATIVE ROW IS SAFE TO DO UNCONDITIONALLY - Round 3 of
// the same review (2026-09-28): on WINDOWS, the two field reports and
// mirics-windex.md's research say native access never reliably works once
// the SDRplay API is installed, so hiding it whenever the API exists costs
// nothing real. On LINUX, that has not been shown - usbfs does not enforce
// the endpoint-recipient rule this driver's writes violate (see
// mirics-windex.md), so a Linux user MAY be able to fall back to the native
// row while the SDRplay daemon is merely stopped, and taking that away is
// not something two Windows-only field reports justify. So the unconditional
// rule applies on Windows only; Linux keeps the ORIGINAL rule (hide only
// when the API's enumeration actually lists the same radio - see
// apiAlreadyLists). A parameter, not a bare #ifdef, so
// tests/test_rsp_rows.cpp can drive both platforms without a build matrix.
#if defined(_WIN32)
constexpr bool kNativeMiricsOpenUnsafeWithApi = true;
#else
constexpr bool kNativeMiricsOpenUnsafeWithApi = false;
#endif

// The "serial=..." value from an args string, or empty for "index=N" and for
// anything else. The args grammar is the one enumerate* builds and open()
// parses: comma-separated key=value.
inline std::string serialFromArgs(const std::string& args) {
    const std::string key = "serial=";
    std::size_t at = args.find(key);
    while (at != std::string::npos) {
        // Only at the start or straight after a separator, so a key that
        // merely ENDS in "serial=" cannot be mistaken for this one.
        if (at == 0 || args[at - 1] == ',' || args[at - 1] == ' ') {
            const std::size_t from = at + key.size();
            const std::size_t end = args.find(',', from);
            return args.substr(from, (end == std::string::npos) ? std::string::npos : end - from);
        }
        at = args.find(key, at + 1);
    }
    return std::string();
}

// Is this row the native driver offering an SDRplay unit? The label is what
// msi2500::resolveModel decided, so a television stick answers false here even
// though it is the same driver and the same silicon.
inline bool isNativeRspRow(const NativeDeviceInfo& row) {
    return row.driver == "mirisdr" && row.label.rfind("SDRplay", 0) == 0;
}

// Would the SDRplay API's own list already show this native row's radio?
// Decides the Linux hiding rule directly (see withoutDuplicateRsps) and, on
// Windows, whether a hidden row has been orphaned (anyRspOrphanedByHiding):
// hiding a row that the API also offers loses nothing; hiding one it does
// not is the case that needs an explanation on screen.
//
// ASSUMES THE NATIVE DRIVER'S USB SERIAL STRING AND THE API'S SerNo FIELD
// ARE THE SAME BYTES for one physical radio - unverified on real hardware
// (no RSP on this desk; see mirics-windex.md's "no hardware" caveats
// generally). If that assumption is ever wrong for some model, the practical
// effect is narrow: the serials never match, apiAlreadyLists reads as
// "different radio", and - combined with the H1 rule that the orphan
// sentence only shows when the enumeration's own skip reason is EMPTY (the
// API genuinely answered and simply did not name this serial) - the result
// is the advisory sentence showing for a radio the API can, in fact, see
// under a serial spelled differently. That is a wrong REASON given for a
// correct outcome (the row is still hidden either way on Windows), not a
// radio the user cannot reach - acceptable until it can be checked against
// real hardware.
inline bool apiAlreadyLists(const NativeDeviceInfo& nativeRow,
                            const std::vector<NativeDeviceInfo>& apiRows) {
    if (apiRows.empty()) { return false; }
    const std::string serial = serialFromArgs(nativeRow.args);
    if (serial.empty()) { return true; }  // nothing to match on; see the header
    bool anyApiSerial = false;
    for (const NativeDeviceInfo& api : apiRows) {
        const std::string apiSerial = serialFromArgs(api.args);
        if (apiSerial.empty()) { continue; }
        anyApiSerial = true;
        if (apiSerial == serial) { return true; }
    }
    // The API listed devices but named no serials at all: it cannot be
    // matched, and the duplicate is still the likelier reading.
    return !anyApiSerial;
}

// The Source list with the duplicates taken out. `rows` is every row, in the
// order the list will show them. `apiInstalled` is source::sdrPlayApiPresent()
// - whether the SDRplay API is on this machine AT ALL, not whether this
// scan's enumeration happened to list anything (see the header for why that
// distinction matters). `nativeOpenUnsafe` is kNativeMiricsOpenUnsafeWithApi
// at every real call site (Windows: true, Linux: false) - a parameter so
// tests can drive both without a build matrix.
//
// nativeOpenUnsafe true (Windows): EVERY row this driver believes is an
// SDRplay unit is hidden whenever the API is installed, unconditionally.
// nativeOpenUnsafe false (Linux): the ORIGINAL, narrower rule - hidden only
// when the API's enumeration actually lists the same radio (apiAlreadyLists),
// so a radio the API cannot currently see keeps its native row as a way in.
// Either way, when the API is not installed at all, native access is the
// only way in and nothing is hidden.
inline std::vector<NativeDeviceInfo> withoutDuplicateRsps(
    const std::vector<NativeDeviceInfo>& rows, bool apiInstalled, bool nativeOpenUnsafe) {
    if (!apiInstalled) { return rows; }
    std::vector<NativeDeviceInfo> apiRows;
    for (const NativeDeviceInfo& r : rows) {
        if (r.driver == "sdrplay") { apiRows.push_back(r); }
    }
    if (!nativeOpenUnsafe && apiRows.empty()) { return rows; }
    std::vector<NativeDeviceInfo> out;
    out.reserve(rows.size());
    for (const NativeDeviceInfo& r : rows) {
        if (!isNativeRspRow(r)) {
            out.push_back(r);
            continue;
        }
        if (nativeOpenUnsafe || apiAlreadyLists(r, apiRows)) { continue; }
        out.push_back(r);
    }
    return out;
}

// Did hiding above leave a radio with NOTHING standing in for it? Only
// possible when `nativeOpenUnsafe` (Windows) - the Linux rule never hides a
// row the API cannot already match, so it never orphans one either. True
// when `apiInstalled` and at least one native RSP row in `rows` has no
// matching "sdrplay" row (apiAlreadyLists) - the API exists, but did not
// offer this particular radio this scan, and its row is now gone with
// nothing in its place. The caller uses this to decide whether to show the
// advisory sentence (see the header) rather than leave the Source list
// silently shorter than it was.
inline bool anyRspOrphanedByHiding(const std::vector<NativeDeviceInfo>& rows, bool apiInstalled,
                                   bool nativeOpenUnsafe) {
    if (!apiInstalled || !nativeOpenUnsafe) { return false; }
    std::vector<NativeDeviceInfo> apiRows;
    for (const NativeDeviceInfo& r : rows) {
        if (r.driver == "sdrplay") { apiRows.push_back(r); }
    }
    for (const NativeDeviceInfo& r : rows) {
        if (isNativeRspRow(r) && !apiAlreadyLists(r, apiRows)) { return true; }
    }
    return false;
}

// The sentence for where a hidden native RSP row would have been. Pure and
// pinned here (rather than composed at the draw site) for the same reason
// sdrPlayApiAdvice lives in sdrplay_source.cpp: it is the one instruction an
// affected owner gets, tr()'d once, here, so every caller shows the exact
// same words. "Restart the service, then pick the SDRplay row" is still the
// right instruction even though "open the native row" no longer is - once
// the service answers, the SDRplay row reappears and works.
inline std::string sdrPlayHiddenRowAdvice() {
    return cascade::i18n::tr(
        "the SDRplay API is installed but did not list this radio - restart the SDRplay API "
        "service (Windows Services, \"SDRplay API Service\"), then pick the SDRplay row");
}

// THE ONE SENTENCE THE SOURCE SECTION SHOWS FOR SDRPLAY, priority order
// pinned here so it can be tested without a window (round 3 of the
// 2026-09-28 review: the first version of this let `orphaned` win outright,
// which meant a lost session or a held-off enumeration - each with their
// OWN specific, more actionable sentence - was silently replaced by "did not
// list this radio", losing "restart FoxSDR" or "wait" information the user
// needed).
//
//   1. `orphaned` AND `enumerationSkip` is EMPTY: the API answered this scan
//      - no session-lost, no hold-off, no version gate - and simply did not
//      list this specific radio. sdrPlayHiddenRowAdvice() is the right and
//      most specific thing to say.
//   2. `enumerationSkip` is non-empty, OR no SDRplay row of any kind was
//      found (`!anyApiRowsFound`): something more specific is known (the
//      skip reason - session lost, held off, API too old - or, with no skip
//      recorded, the load result via sdrPlayApiAdvice) and it always beats
//      the generic orphan sentence, REGARDLESS of `orphaned` - a skip
//      reason means the WHOLE enumeration failed, so nothing was found
//      either way, but the more specific reason is what the user needs.
//   3. Otherwise (something was found, nothing orphaned): no advice - the
//      Source list already shows what there is to show.
//
// `anyApiRowsFound` is whether the LAST scan's SDRplay-API enumeration
// listed anything at all (app_window's `sdrPlayRowsFound_` - true only when
// GetDevices returned at least one device, independent of native rows).
inline std::string sdrPlaySourceAdvice(bool orphaned, bool anyApiRowsFound, bool apiResolved,
                                       float apiVersion, const std::string& enumerationSkip) {
    if (orphaned && enumerationSkip.empty()) { return sdrPlayHiddenRowAdvice(); }
    if (!anyApiRowsFound) {
        return sdrPlayPanelAdvice(apiResolved, apiVersion, enumerationSkip);
    }
    return std::string();
}

// THE RADIO THIS PROCESS HAS OPEN IS NOT IN THE API'S LIST (0.99.36).
//
// sdrplay_api_GetDevices lists the RSPs that are free to select, and a radio
// this process has SELECTED is not one of them - the header above already
// relies on that for "the other being open elsewhere", and SDRplay's own
// SoapySDR module (SoapySDRPlay3 Registration.cpp, findSDRPlay) works around
// it by adding "the cached results for claimed handles" back after every
// GetDevices. FoxSDR did not, so the Source combo re-read the list every time
// it was opened and the RSP the receiver was playing VANISHED from it: the
// row index then pointed at whatever row slid into its place (the Pluto row,
// or - at the time this was written - the native Mirics row for the same
// radio, back because the duplicate rule then only hid it when the API
// listed something; the 2026-09-28 revision above hides that row
// unconditionally, so this specific slide no longer happens, but the row
// COUNT still moves whenever the claimed radio drops out of the API's own
// list, and a patch radio handed back to the receiver was "not listed any
// more" for exactly that reason.
//
// `claimed` is every SDRplay radio this process has open - driver "sdrplay",
// its args exactly as the row that opened it carried them, and the label that
// row showed. Each one the list does not already carry is appended, in the
// order given, so the list says what the process holds. Anything that is not
// an API row is ignored: only the API has this blind spot. Call BEFORE
// withoutDuplicateRsps, so the native duplicate of a claimed radio stays
// hidden.
inline std::vector<NativeDeviceInfo> withClaimedSdrPlayRows(
    const std::vector<NativeDeviceInfo>& rows, const std::vector<NativeDeviceInfo>& claimed) {
    std::vector<NativeDeviceInfo> out = rows;
    for (const NativeDeviceInfo& c : claimed) {
        if (c.driver != "sdrplay") { continue; }
        bool listed = false;
        for (const NativeDeviceInfo& r : out) {
            if (r.driver == c.driver && r.args == c.args) {
                listed = true;
                break;
            }
        }
        if (!listed) { out.push_back(c); }
    }
    return out;
}

}  // namespace cascade::source

#endif  // CASCADE_SOURCE_RSP_ROWS_HPP
