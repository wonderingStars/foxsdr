// One radio, one row: hiding the native RSP when the SDRplay API has it.
//
// The owner, 2026-09-21, after 0.99.9 made the duplicate visible by giving
// both rows the same model name: "yes hide the natve".
//
// The risk being tested is not the duplicate, it is the OVER-hiding: a native
// row removed when the API turns out not to offer that radio is a radio the
// user cannot select at all. Every case below is about that boundary.
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
    using cascade::source::apiAlreadyLists;
    using cascade::source::isNativeRspRow;
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

    // --- the match --------------------------------------------------------
    const NativeDeviceInfo nativeWithSerial =
        row("mirisdr", "SDRplay RSP1A", "serial=12345");
    const NativeDeviceInfo nativeNoSerial = row("mirisdr", "SDRplay RSP1A", "index=0");

    // AN API THAT LISTED NOTHING HIDES NOTHING. The API can be installed with
    // its service stopped; then the native row is the only way to the radio.
    CHECK(!apiAlreadyLists(nativeWithSerial, {}));
    CHECK(!apiAlreadyLists(nativeNoSerial, {}));

    // Same serial on both sides: the same radio, twice.
    CHECK(apiAlreadyLists(nativeWithSerial,
                          {row("sdrplay", "SDRplay RSP1A 12345", "serial=12345")}));

    // DIFFERENT SERIALS ARE DIFFERENT RADIOS. Two RSPs, one of them open
    // somewhere else so the API can only see the other: the one it cannot see
    // must stay in the list.
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

    // --- the whole list ---------------------------------------------------
    {
        // An RSP the API can see, a television stick on the same driver, and
        // somebody else's radio: only the duplicate goes.
        const std::vector<NativeDeviceInfo> rows{
            row("rtlsdr", "RTL2838UHIDIR", "index=0"),
            row("mirisdr", "SDRplay RSP1A (serial 12345)", "serial=12345"),
            row("mirisdr", "Mirics MSi2500", "index=1"),
            row("sdrplay", "SDRplay RSP1A 12345", "serial=12345"),
        };
        const std::vector<std::string> want{"RTL2838UHIDIR", "Mirics MSi2500",
                                            "SDRplay RSP1A 12345"};
        CHECK(labelsOf(withoutDuplicateRsps(rows)) == want);
    }
    {
        // NO API ROWS AT ALL: the list is returned exactly as it came in,
        // which is the case on every machine without the SDRplay API.
        const std::vector<NativeDeviceInfo> rows{
            row("mirisdr", "SDRplay RSP1A", "index=0"),
            row("rtlsdr", "RTL2838UHIDIR", "index=0"),
        };
        const std::vector<std::string> want{"SDRplay RSP1A", "RTL2838UHIDIR"};
        CHECK(labelsOf(withoutDuplicateRsps(rows)) == want);
    }
    {
        // Two RSPs, one visible to the API: one row goes, one stays.
        const std::vector<NativeDeviceInfo> rows{
            row("mirisdr", "SDRplay RSP1A (serial 111)", "serial=111"),
            row("mirisdr", "SDRplay RSP2 (serial 222)", "serial=222"),
            row("sdrplay", "SDRplay RSP2 222", "serial=222"),
        };
        const std::vector<std::string> want{"SDRplay RSP1A (serial 111)", "SDRplay RSP2 222"};
        CHECK(labelsOf(withoutDuplicateRsps(rows)) == want);
    }
    CHECK(withoutDuplicateRsps({}).empty());

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
        // Mirics row for the SAME radio is back - and it must stay hidden,
        // which only works once the claimed row is in the list again.
        const std::vector<NativeDeviceInfo> scanned{
            row("mirisdr", "SDRplay RSP1A (serial 1811003EFB)", "serial=1811003EFB"),
        };
        const std::vector<NativeDeviceInfo> claimed{
            row("sdrplay", "SDRplay RSP1A (serial 1811003EFB)", "serial=1811003EFB"),
        };
        const std::vector<NativeDeviceInfo> shown =
            withoutDuplicateRsps(withClaimedSdrPlayRows(scanned, claimed));
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
