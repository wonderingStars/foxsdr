// One radio, one row: hiding the native RSP when the SDRplay API is
// installed.
//
// The owner, 2026-09-21, after 0.99.9 made the duplicate visible by giving
// both rows the same model name: "yes hide the natve".
//
// REVISED 2026-09-28 (twice), after two RSP1A field reports and two rounds
// of code review of this file:
//
//   Round 1: the original design hid the native row only when the API's OWN
//   enumeration currently listed something, on the theory that a stopped
//   service left the native row as a working way in. It does not - the two
//   field reports show the API-opened radio failing over to the "fallback"
//   native row and THAT failing too, with a raw USB error. So the native row
//   was hidden whenever the API was installed AT ALL, and the risk this file
//   used to test - a row removed when the API turns out not to offer that
//   radio, leaving the user with nothing to click - became the exact
//   condition anyRspOrphanedByHiding exists to DETECT, so the caller can put
//   an actionable sentence where the row used to be.
//
//   Round 2 (the SAME review, next pass): "whenever the API is installed"
//   was ITSELF too broad - the mechanism that makes native access unsafe
//   (mirics-windex.md: a control transfer's recipient bits naming an
//   endpoint WinUSB validates and does not find) is a WINDOWS USB-stack
//   fact. Linux's usbfs skips that validation for vendor requests, so a
//   Linux user's native row may genuinely work as a fallback while the
//   SDRplay daemon is merely stopped - exactly what the Round-1 rule
//   protected before this investigation started. So both
//   withoutDuplicateRsps and anyRspOrphanedByHiding now take a
//   `nativeOpenUnsafe` parameter (kNativeMiricsOpenUnsafeWithApi at every
//   real call site: true on Windows, false on Linux) and Linux keeps the
//   ORIGINAL, Round-0 rule.
//
//   Also Round 2: sdrPlaySourceAdvice() fixes a second bug the review found
//   - the orphan sentence was overriding a MORE SPECIFIC one (a lost
//   session, a held-off enumeration, an API too old), each of which says
//   more than "did not list this radio" and must win.
//
// See source/rsp_rows.hpp's header for the full argument.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/rsp_rows.hpp"

#include "test_check.hpp"

#include <string>
#include <vector>

namespace {

using cascade::source::NativeDeviceInfo;

NativeDeviceInfo row(const std::string& driver, const std::string& label,
                     const std::string& args) {
    NativeDeviceInfo r;
    r.driver = driver;
    r.label = label;
    r.args = args;
    return r;
}

std::vector<std::string> labelsOf(const std::vector<NativeDeviceInfo>& rows) {
    std::vector<std::string> out;
    for (const NativeDeviceInfo& r : rows) { out.push_back(r.label); }
    return out;
}

}  // namespace

int main() {
    using cascade::source::anyRspOrphanedByHiding;
    using cascade::source::apiAlreadyLists;
    using cascade::source::isNativeRspRow;
    using cascade::source::sdrPlayHiddenRowAdvice;
    using cascade::source::sdrPlaySourceAdvice;
    using cascade::source::serialFromArgs;
    using cascade::source::withoutDuplicateRsps;

    // --- reading a serial out of an args string ---------------------------
    CHECK(serialFromArgs("serial=1809176812") == "1809176812");
    CHECK(serialFromArgs("index=0").empty());
    CHECK(serialFromArgs("").empty());
    CHECK(serialFromArgs("driver=x,serial=ABC,rate=2") == "ABC");
    // A key that merely ends in the same letters is not this key.
    CHECK(serialFromArgs("otherserial=NOPE").empty());

    // --- which rows are even candidates -----------------------------------
    CHECK(isNativeRspRow(row("mirisdr", "SDRplay RSP1A", "index=0")));
    CHECK(isNativeRspRow(row("mirisdr", "SDRplay RSP1 (serial 12345)", "serial=12345")));
    // A TELEVISION STICK IS NOT AN RSP, and it is the same driver: hiding it
    // because an RSP happens to be plugged in would remove a working radio.
    CHECK(!isNativeRspRow(row("mirisdr", "Mirics MSi2500", "index=0")));
    CHECK(!isNativeRspRow(row("mirisdr", "Hauppauge WinTV 133559 LF", "index=1")));
    // ...and neither is anybody else's driver, including the API's own rows.
    CHECK(!isNativeRspRow(row("sdrplay", "SDRplay RSP1A", "serial=12345")));
    CHECK(!isNativeRspRow(row("rtlsdr", "SDRplay RSP1A", "index=0")));

    // --- apiAlreadyLists: decides Linux hiding directly, and Windows
    //     orphaning ------------------------------------------------------
    const NativeDeviceInfo nativeWithSerial =
        row("mirisdr", "SDRplay RSP1A", "serial=12345");
    const NativeDeviceInfo nativeNoSerial = row("mirisdr", "SDRplay RSP1A", "index=0");

    // An API that listed nothing does not "already list" anything.
    CHECK(!apiAlreadyLists(nativeWithSerial, {}));
    CHECK(!apiAlreadyLists(nativeNoSerial, {}));

    // Same serial on both sides: the same radio, twice.
    CHECK(apiAlreadyLists(nativeWithSerial,
                          {row("sdrplay", "SDRplay RSP1A 12345", "serial=12345")}));

    // DIFFERENT SERIALS ARE DIFFERENT RADIOS. Two RSPs, one of them open
    // somewhere else so the API can only see the other: the one it cannot see
    // is not "already listed" by the one it can.
    CHECK(!apiAlreadyLists(nativeWithSerial,
                           {row("sdrplay", "SDRplay RSP2 99999", "serial=99999")}));
    // ...and it is found among several.
    CHECK(apiAlreadyLists(nativeWithSerial,
                          {row("sdrplay", "a", "serial=99999"),
                           row("sdrplay", "b", "serial=12345")}));

    // Nothing to match on: the duplicate is the likelier reading, both when
    // the native row has no serial and when the API named none.
    CHECK(apiAlreadyLists(nativeNoSerial, {row("sdrplay", "SDRplay RSP1A", "index=0")}));
    CHECK(apiAlreadyLists(nativeWithSerial, {row("sdrplay", "SDRplay RSP1A", "index=0")}));

    // --- withoutDuplicateRsps / anyRspOrphanedByHiding: WINDOWS
    //     (nativeOpenUnsafe = true) -------------------------------------
    {
        // An RSP the API can see, a television stick on the same driver, and
        // somebody else's radio: the RSP's native row goes because the API
        // is installed - same outcome as the Linux rule would give here
        // (the API DOES list it), but for the unconditional reason.
        const std::vector<NativeDeviceInfo> rows{
            row("rtlsdr", "RTL2838UHIDIR", "index=0"),
            row("mirisdr", "SDRplay RSP1A (serial 12345)", "serial=12345"),
            row("mirisdr", "Mirics MSi2500", "index=1"),
            row("sdrplay", "SDRplay RSP1A 12345", "serial=12345"),
        };
        const std::vector<std::string> want{"RTL2838UHIDIR", "Mirics MSi2500",
                                            "SDRplay RSP1A 12345"};
        CHECK(labelsOf(withoutDuplicateRsps(rows, true, true)) == want);
        CHECK(!anyRspOrphanedByHiding(rows, true, true));  // the API DOES list it
    }
    {
        // THE API IS NOT INSTALLED: the list is returned exactly as it came
        // in, on any platform - native access is the only way in, and
        // nothing is hidden.
        const std::vector<NativeDeviceInfo> rows{
            row("mirisdr", "SDRplay RSP1A", "index=0"),
            row("rtlsdr", "RTL2838UHIDIR", "index=0"),
        };
        const std::vector<std::string> want{"SDRplay RSP1A", "RTL2838UHIDIR"};
        CHECK(labelsOf(withoutDuplicateRsps(rows, false, true)) == want);
        CHECK(labelsOf(withoutDuplicateRsps(rows, false, false)) == want);
        CHECK(!anyRspOrphanedByHiding(rows, false, true));
        CHECK(!anyRspOrphanedByHiding(rows, false, false));
    }
    {
        // THE OVER-HIDING CASE THIS FILE ONCE GUARDED AGAINST UNCONDITIONALLY,
        // NOW DELIBERATE ON WINDOWS ONLY: two RSPs, the API installed but
        // currently listing only one of them (the other open elsewhere, or
        // simply not answering for this one). On Windows both native rows
        // go - the RSP1A's with NOTHING standing in for it, which
        // anyRspOrphanedByHiding must catch so the caller can say something.
        const std::vector<NativeDeviceInfo> rows{
            row("mirisdr", "SDRplay RSP1A (serial 111)", "serial=111"),
            row("mirisdr", "SDRplay RSP2 (serial 222)", "serial=222"),
            row("sdrplay", "SDRplay RSP2 222", "serial=222"),
        };
        const std::vector<std::string> wantWindows{"SDRplay RSP2 222"};
        CHECK(labelsOf(withoutDuplicateRsps(rows, true, true)) == wantWindows);
        CHECK(anyRspOrphanedByHiding(rows, true, true));

        // ON LINUX (nativeOpenUnsafe = false): the ORIGINAL rule - the
        // RSP1A's native row stays, because the API does not offer it this
        // scan and Linux still treats that as "the native row is the way
        // in". Nothing is ever orphaned by the Linux rule: it never hides a
        // row it has not matched.
        const std::vector<std::string> wantLinux{"SDRplay RSP1A (serial 111)",
                                                  "SDRplay RSP2 222"};
        CHECK(labelsOf(withoutDuplicateRsps(rows, true, false)) == wantLinux);
        CHECK(!anyRspOrphanedByHiding(rows, true, false));
    }
    {
        // The API installed and listing the SAME radio a native row would
        // have offered: nothing orphaned on Windows either, the duplicate
        // removal is free.
        const std::vector<NativeDeviceInfo> rows{
            row("mirisdr", "SDRplay RSP2 (serial 222)", "serial=222"),
            row("sdrplay", "SDRplay RSP2 222", "serial=222"),
        };
        CHECK(!anyRspOrphanedByHiding(rows, true, true));
    }
    {
        // No native RSP row at all: nothing to orphan, any platform, API
        // installed or not.
        const std::vector<NativeDeviceInfo> rows{
            row("rtlsdr", "RTL2838UHIDIR", "index=0"),
            row("sdrplay", "SDRplay RSP2 222", "serial=222"),
        };
        CHECK(!anyRspOrphanedByHiding(rows, true, true));
        CHECK(!anyRspOrphanedByHiding(rows, true, false));
        CHECK(!anyRspOrphanedByHiding(rows, false, true));
    }
    CHECK(withoutDuplicateRsps({}, true, true).empty());
    CHECK(withoutDuplicateRsps({}, true, false).empty());
    CHECK(withoutDuplicateRsps({}, false, true).empty());
    CHECK(!anyRspOrphanedByHiding({}, true, true));

    // --- the advisory sentence, pinned --------------------------------------
    //
    // Not a paraphrase test: the two things an affected owner actually needs
    // are named explicitly, because a rewording that drops either one sends
    // them nowhere - "restart X" without saying which service, or "pick the
    // row" without saying which one.
    {
        const std::string advice = sdrPlayHiddenRowAdvice();
        CHECK(advice.find("SDRplay API Service") != std::string::npos);
        CHECK(advice.find("restart") != std::string::npos ||
              advice.find("Restart") != std::string::npos);
        CHECK(advice.find("SDRplay row") != std::string::npos);
    }

    // --- sdrPlaySourceAdvice: priority order, each state pinned -------------
    //
    // Round 3 of the review: the orphan sentence must not override a MORE
    // SPECIFIC reason the enumeration itself already has. Every state named
    // in the review is exercised here, each against the code path that
    // would show it wrongly if the priority order regressed.
    {
        // STATE 1: skip empty, orphaned - the API answered cleanly and
        // simply does not have this radio. The orphan sentence wins, because
        // nothing more specific is known.
        const std::string a = sdrPlaySourceAdvice(/*orphaned=*/true, /*anyApiRowsFound=*/true,
                                                   /*apiResolved=*/true, /*apiVersion=*/3.15f,
                                                   /*enumerationSkip=*/std::string());
        CHECK(a == sdrPlayHiddenRowAdvice());
    }
    {
        // STATE 2: SESSION LOST. sdrPlayLastEnumerationSkip() carries the
        // session-lost sentence (restart FoxSDR, not just the service) -
        // orphaned or not, THIS wins, because it is what actually happened.
        const std::string skip =
            "the SDRplay service stopped answering while FoxSDR was using it, so FoxSDR will "
            "not call the SDRplay API again until it is restarted - restart the SDRplay API "
            "service, then restart FoxSDR";
        const std::string a = sdrPlaySourceAdvice(/*orphaned=*/true, /*anyApiRowsFound=*/false,
                                                   /*apiResolved=*/true, /*apiVersion=*/3.15f, skip);
        CHECK(a == skip);
        CHECK(a.find("restart FoxSDR") != std::string::npos);
        // The bug this state exists to catch: if the orphan sentence had won
        // instead, "restart FoxSDR" would be missing entirely.
        CHECK(a != sdrPlayHiddenRowAdvice());
    }
    {
        // STATE 3: HELD OFF (the 3 s enumeration-hang bound plus the 60 s
        // hold-off - sdrPlayServiceHungSentence's shape). Same priority.
        const std::string skip =
            "the SDRplay service did not answer within 3 s - restart the SDRplay API service";
        const std::string a = sdrPlaySourceAdvice(/*orphaned=*/true, /*anyApiRowsFound=*/false,
                                                   /*apiResolved=*/true, /*apiVersion=*/0.0f, skip);
        CHECK(a == skip);
    }
    {
        // STATE 4: API TOO OLD. No enumeration skip was ever recorded
        // (nothing has enumerated yet), so sdrPlaySourceAdvice falls to
        // sdrPlayPanelAdvice/sdrPlayApiAdvice, which reads the version gate
        // directly - "orphaned" cannot even be true here (no native row was
        // ever hidden without a scan), but the state is pinned regardless.
        const std::string a = sdrPlaySourceAdvice(/*orphaned=*/false, /*anyApiRowsFound=*/false,
                                                   /*apiResolved=*/true, /*apiVersion=*/3.05f,
                                                   /*enumerationSkip=*/std::string());
        CHECK(a.find("3.05") != std::string::npos || a.find("newer") != std::string::npos);
        CHECK(a.find("sdrplay.com") != std::string::npos);
    }
    {
        // STATE 5: LINUX. anyRspOrphanedByHiding structurally never returns
        // true when nativeOpenUnsafe is false (see its own tests above), so
        // the orphan sentence can never reach a Linux user - only the
        // ordinary "no rows found" advice can, exactly like any other
        // platform.
        CHECK(!anyRspOrphanedByHiding(
            {row("mirisdr", "SDRplay RSP1A", "serial=1"), row("sdrplay", "x", "serial=2")},
            /*apiInstalled=*/true, /*nativeOpenUnsafe=*/false));
    }
    {
        // Something found, nothing orphaned: no advice at all.
        const std::string a = sdrPlaySourceAdvice(/*orphaned=*/false, /*anyApiRowsFound=*/true,
                                                   /*apiResolved=*/true, /*apiVersion=*/3.15f,
                                                   /*enumerationSkip=*/std::string());
        CHECK(a.empty());
    }

    // --- the radio this process has open stays in the list (0.99.36) --------
    //
    // The API does not list a radio this process has selected (see
    // withClaimedSdrPlayRows). Before 0.99.36 every re-scan while an RSP was
    // playing dropped its row.
    using cascade::source::withClaimedSdrPlayRows;
    {
        // An RSPdx open in the receiver: the API lists nothing, and without
        // the claimed row the Source list is just the Pluto.
        const std::vector<NativeDeviceInfo> scanned{
            row("pluto", "ADALM-Pluto (network)", "uri=ip:192.168.2.1"),
        };
        const std::vector<NativeDeviceInfo> claimed{
            row("sdrplay", "SDRplay RSPdx (serial 2208054321)", "serial=2208054321"),
        };
        const std::vector<std::string> want{"ADALM-Pluto (network)",
                                            "SDRplay RSPdx (serial 2208054321)"};
        CHECK(labelsOf(withClaimedSdrPlayRows(scanned, claimed)) == want);
    }
    {
        // An RSP1A open through the API: the API lists nothing, the native
        // Mirics row for the SAME radio is back - and on Windows it must
        // stay hidden (unconditionally, whether or not the claimed row ran
        // first), with the claimed row keeping it from being orphaned.
        const std::vector<NativeDeviceInfo> scanned{
            row("mirisdr", "SDRplay RSP1A (serial 1811003EFB)", "serial=1811003EFB"),
        };
        const std::vector<NativeDeviceInfo> claimed{
            row("sdrplay", "SDRplay RSP1A (serial 1811003EFB)", "serial=1811003EFB"),
        };
        const std::vector<NativeDeviceInfo> withClaim = withClaimedSdrPlayRows(scanned, claimed);
        CHECK(!anyRspOrphanedByHiding(withClaim, true, true));
        const std::vector<NativeDeviceInfo> shown = withoutDuplicateRsps(withClaim, true, true);
        CHECK(shown.size() == 1);
        if (shown.size() == 1) {
            CHECK(shown[0].driver == "sdrplay");
            CHECK(shown[0].args == "serial=1811003EFB");
        }
    }
    {
        // Already listed (an API that does list it, or a second copy): never
        // twice.
        const std::vector<NativeDeviceInfo> scanned{
            row("sdrplay", "SDRplay RSP2 (serial 222)", "serial=222"),
        };
        const std::vector<NativeDeviceInfo> claimed{
            row("sdrplay", "SDRplay RSP2 (serial 222)", "serial=222"),
        };
        CHECK(withClaimedSdrPlayRows(scanned, claimed).size() == 1);
    }
    {
        // Only API rows are ever added back: a native radio the process holds
        // is enumerated by SetupAPI, which never hides it.
        const std::vector<NativeDeviceInfo> scanned{};
        const std::vector<NativeDeviceInfo> claimed{
            row("rtlsdr", "RTL2838UHIDIR", "serial=00000001"),
        };
        CHECK(withClaimedSdrPlayRows(scanned, claimed).empty());
    }
    CHECK(withClaimedSdrPlayRows({}, {}).empty());

    return testSummary("test_rsp_rows");
}
