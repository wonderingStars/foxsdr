// ONE CHIP, ONE ROUTE: every way into Mirics / SDRplay silicon obeys one
// quarantine decision.
//
// THE FIELD FAULT (two crash reports, one Linux user, an SDRplay RSP1A with the
// SDRplay API 3 installed, FoxSDR 0.99.59):
//
//   A. SIGSEGV inside libmiriSupport.so (SoapyMiri), called from
//      SoapySource::read, after the log said "1 native SDRplay row(s) hidden -
//      the SDRplay API is installed". The Mirics row had been hidden for the
//      radio the SDRplay API manages, and the SoapySDR `miri` row for the SAME
//      radio was still offered, and still opened: nothing looked at it.
//   B. abort() from glibc on the GUI thread, seconds after the patch page
//      opened the radio through SoapySDR's `sdrplay` module - AFTER the native
//      driver had logged "the SDRplay session was lost earlier ... no further
//      SDRplay API calls are made until FoxSDR is restarted". The Soapy module
//      made them. (That the open CAUSED the abort is an inference from timing;
//      what is certain is that the open was allowed.)
//
// THE RULE. Native `sdrplay`, native `mirisdr`, SoapySDR `driver=sdrplay` and
// SoapySDR `driver=miri` are one chip reached four ways. The decision is PURE
// (source/rsp_rows.hpp, miricsSoapyRouteRefusal) and is asked from the one
// place every SoapySDR open passes through (SoapySource::open), so the
// receiver's Source list, a saved config, the prefer-native fallback and the
// patch page's radios all obey it without each carrying a copy:
//
//   (a) once the SDRplay API session is lost, or a worker was abandoned inside
//       the API, no SoapySDR sdrplay or miri device is opened for the rest of
//       the process, and the refusal says why;
//   (b) a SoapySDR miri device is not offered or opened for a radio the SDRplay
//       API is managing - exactly the radios whose native Mirics row
//       withoutDuplicateRsps hides - and a genuine Mirics dongle on a machine
//       with no SDRplay API keeps its route.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/rsp_rows.hpp"
#include "source/sdrplay_source.hpp"
#include "source/soapy_source.hpp"

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Types.hpp>
#include <SoapySDR/Version.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "test_check.hpp"

namespace {

using cascade::source::MiricsSoapyRefusal;
using cascade::source::NativeDeviceInfo;
using cascade::source::SdrPlayApiState;
using cascade::source::SoapySource;

NativeDeviceInfo row(const std::string& driver, const std::string& label, const std::string& args) {
    NativeDeviceInfo r;
    r.driver = driver;
    r.label = label;
    r.args = args;
    return r;
}

// The native Mirics row for an RSP, the API's row for the same radio, and a
// television stick on the same native driver.
NativeDeviceInfo rspNative(const std::string& serial) {
    return row("mirisdr", "SDRplay RSP1A (serial " + serial + ")", "serial=" + serial);
}
NativeDeviceInfo rspApi(const std::string& serial) {
    return row("sdrplay", "SDRplay RSP1A (serial " + serial + ")", "serial=" + serial);
}
NativeDeviceInfo stick(int index) {
    return row("mirisdr", "Mirics MSi2500 (index " + std::to_string(index) + ")",
               "index=" + std::to_string(index));
}

SdrPlayApiState state(bool installed, bool lost, bool abandoned) {
    SdrPlayApiState s;
    s.installed = installed;
    s.sessionLost = lost;
    s.workerAbandoned = abandoned;
    return s;
}

MiricsSoapyRefusal refusal(const std::string& args, const SdrPlayApiState& s,
                           const std::vector<NativeDeviceInfo>& rows, bool nativeOpenUnsafe) {
    return cascade::source::miricsSoapyRouteRefusal(args, s, rows, nativeOpenUnsafe);
}

bool hasRow(const std::vector<NativeDeviceInfo>& rows, const NativeDeviceInfo& r) {
    for (const NativeDeviceInfo& x : rows) {
        if (x.driver == r.driver && x.args == r.args && x.label == r.label) { return true; }
    }
    return false;
}

// --- a SoapySDR module's device, registered under the real module names -------
//
// SoapySDR's registry is a public in-process API (test_soapy_source.cpp does
// the same): a factory registered from this file is found by Device::make
// exactly like SoapySDRPlay3's or SoapyMiri's, so SoapySource::open runs for
// real, and the counters say whether the refusal happened BEFORE the module
// was ever asked to make a device.
std::atomic<int> g_makesSdrplay{0};
std::atomic<int> g_makesMiri{0};
std::atomic<int> g_makesOther{0};

class FakeChip : public SoapySDR::Device {
public:
    explicit FakeChip(std::string key) : key_(std::move(key)) {}
    std::string getDriverKey() const override { return key_; }
    std::string getHardwareKey() const override { return "fake " + key_; }
    size_t getNumChannels(const int) const override { return 1; }
    SoapySDR::Stream* setupStream(const int, const std::string&, const std::vector<size_t>&,
                                  const SoapySDR::Kwargs&) override {
        return reinterpret_cast<SoapySDR::Stream*>(this);
    }
    void closeStream(SoapySDR::Stream*) override {}
    int activateStream(SoapySDR::Stream*, const int, const long long, const size_t) override {
        return 0;
    }
    int deactivateStream(SoapySDR::Stream*, const int, const long long) override { return 0; }
    double getSampleRate(const int, const size_t) const override { return 2.0e6; }
    double getFrequency(const int, const size_t) const override { return 100.0e6; }

private:
    std::string key_;
};

SoapySDR::KwargsList findNamed(const SoapySDR::Kwargs& args, const char* name) {
    const auto d = args.find("driver");
    if (d != args.end() && d->second == name) { return SoapySDR::KwargsList{args}; }
    return {};
}
SoapySDR::KwargsList findSdrplay(const SoapySDR::Kwargs& a) { return findNamed(a, "sdrplay"); }
SoapySDR::KwargsList findMiri(const SoapySDR::Kwargs& a) { return findNamed(a, "miri"); }
SoapySDR::KwargsList findOther(const SoapySDR::Kwargs& a) { return findNamed(a, "fakeother"); }
SoapySDR::Device* makeSdrplay(const SoapySDR::Kwargs&) {
    g_makesSdrplay.fetch_add(1);
    return new FakeChip("sdrplay");
}
SoapySDR::Device* makeMiri(const SoapySDR::Kwargs&) {
    g_makesMiri.fetch_add(1);
    return new FakeChip("miri");
}
SoapySDR::Device* makeOther(const SoapySDR::Kwargs&) {
    g_makesOther.fetch_add(1);
    return new FakeChip("fakeother");
}

struct SoapyRow {
    std::string label;
    std::string args;
};

}  // namespace

int main() {
    using cascade::source::kNativeMiricsOpenUnsafeWithApi;
    using cascade::source::miricsSoapyRefusalSentence;
    using cascade::source::withoutDuplicateRsps;
    using cascade::source::withoutRefusedMiricsSoapyRows;

    // =========================================================================
    // THE PURE DECISION
    // =========================================================================

    // (a) A LOST SESSION CLOSES BOTH SOAPY MODULES, for every spelling of the
    //     driver name and on every platform, whatever the rows say.
    const std::vector<std::string> miricsArgs = {
        "driver=sdrplay",
        "driver=sdrplay, serial=1811003EFB",
        "driver=miri",
        "driver=miri,index=0",
        "DRIVER=Miri,serial=1",
        "label=SDRplay RSP1A,driver=sdrplay",
        "driver=mirisdr",
    };
    for (const bool unsafe : {false, true}) {
        for (const std::string& a : miricsArgs) {
            CHECK(refusal(a, state(true, true, false), {}, unsafe) == MiricsSoapyRefusal::SessionLost);
            // ...and a worker abandoned inside the API is the same quarantine.
            CHECK(refusal(a, state(true, false, true), {}, unsafe) == MiricsSoapyRefusal::SessionLost);
        }
    }

    // It must not over-reach: every OTHER SoapySDR driver is nobody's chip.
    for (const std::string a : {"driver=rtlsdr", "driver=uhd,serial=31", "driver=plutosdr",
                                "driver=remote,remote=tcp://x", "serial=1811003EFB", ""}) {
        CHECK(refusal(a, state(true, true, true), {rspNative("1"), rspApi("1")}, false) ==
              MiricsSoapyRefusal::None);
    }

    // A HEALTHY API closes nothing for `driver=sdrplay`: the existing
    // behaviour is kept - the prefer-native rule upgrades the row, and where
    // the native route cannot take it the Soapy module is the way in.
    for (const bool unsafe : {false, true}) {
        CHECK(refusal("driver=sdrplay,serial=111", state(true, false, false),
                      {rspNative("111"), rspApi("111")}, unsafe) == MiricsSoapyRefusal::None);
    }

    // (b) THE MIRICS MODULE, FOR A RADIO THE API MANAGES - Linux rule
    //     (nativeOpenUnsafe false): the radios whose native row is hidden are
    //     the ones the API's own list names.
    {
        const SdrPlayApiState ok = state(true, false, false);
        const std::vector<NativeDeviceInfo> managed = {rspNative("1811003EFB"),
                                                       rspApi("1811003EFB")};
        // No serial on the Soapy row: nothing says it is a different radio.
        CHECK(refusal("driver=miri", ok, managed, false) == MiricsSoapyRefusal::ManagedByApi);
        CHECK(refusal("driver=miri,index=0", ok, managed, false) == MiricsSoapyRefusal::ManagedByApi);
        // Its serial is the managed radio's - in either case.
        CHECK(refusal("driver=miri, serial=1811003EFB", ok, managed, false) ==
              MiricsSoapyRefusal::ManagedByApi);
        CHECK(refusal("driver=miri,serial=1811003efb", ok, managed, false) ==
              MiricsSoapyRefusal::ManagedByApi);
        // A serial that is PROVABLY another radio keeps its route.
        CHECK(refusal("driver=miri,serial=99999999", ok, managed, false) == MiricsSoapyRefusal::None);

        // The API installed but listing NOTHING (service stopped): on Linux the
        // native row is not hidden then (the original rule), so no route is
        // taken from anybody.
        CHECK(refusal("driver=miri", ok, {rspNative("1811003EFB")}, false) == MiricsSoapyRefusal::None);
        // The API lists a different radio: this one is not managed by it.
        CHECK(refusal("driver=miri,serial=1811003EFB", ok, {rspNative("1811003EFB"), rspApi("2002000ABC")},
                      false) == MiricsSoapyRefusal::None);

        // A GENUINE MIRICS DONGLE ON A MACHINE WITH THE API INSTALLED: no
        // native row of an SDRplay unit exists, so there is nothing for the
        // API to be managing - the API lists an RSPdx, which is not a Mirics
        // chip at all.
        CHECK(refusal("driver=miri", ok, {stick(0), rspApi("2002000ABC")}, false) ==
              MiricsSoapyRefusal::None);
        CHECK(refusal("driver=miri,index=0", ok, {stick(0)}, false) == MiricsSoapyRefusal::None);

        // AN RSP AND A STICK TOGETHER. A Soapy row that names no serial cannot
        // be told apart, so it is refused (the stick's own native row is
        // listed and works); one that names a serial the RSP does not have
        // keeps its route.
        const std::vector<NativeDeviceInfo> both = {stick(1), rspNative("111"), rspApi("111")};
        CHECK(refusal("driver=miri", ok, both, false) == MiricsSoapyRefusal::ManagedByApi);
        CHECK(refusal("driver=miri,serial=999", ok, both, false) == MiricsSoapyRefusal::None);
        // A managed radio whose own row has no serial cannot be ruled out by
        // any serial.
        CHECK(refusal("driver=miri,serial=999", ok,
                      {row("mirisdr", "SDRplay RSP1A", "index=0"), rspApi("111")},
                      false) == MiricsSoapyRefusal::ManagedByApi);
    }

    // NO SDRPLAY API ON THIS MACHINE: a genuine Mirics dongle, and an RSP that
    // is reachable only natively, both keep the SoapySDR route.
    for (const bool unsafe : {false, true}) {
        CHECK(refusal("driver=miri", state(false, false, false), {rspNative("111")}, unsafe) ==
              MiricsSoapyRefusal::None);
        CHECK(refusal("driver=miri", state(false, false, false), {stick(0)}, unsafe) ==
              MiricsSoapyRefusal::None);
    }

    // Windows rule (nativeOpenUnsafe true): the native row of an RSP is hidden
    // whenever the API is installed, listed or not - and the Soapy Mirics
    // route closes with it.
    CHECK(refusal("driver=miri", state(true, false, false), {rspNative("111")}, true) ==
          MiricsSoapyRefusal::ManagedByApi);
    CHECK(refusal("driver=miri,serial=999", state(true, false, false), {rspNative("111")}, true) ==
          MiricsSoapyRefusal::None);
    CHECK(refusal("driver=miri", state(true, false, false), {stick(0)}, true) ==
          MiricsSoapyRefusal::None);

    // ONE RULE, NOT TWO. For every scenario and both platform switches, a
    // native RSP row is hidden by withoutDuplicateRsps exactly when a SoapySDR
    // Mirics row carrying its serial is refused: the two lists cannot drift.
    {
        const std::vector<std::vector<NativeDeviceInfo>> scenarios = {
            {rspNative("111"), rspApi("111")},
            {rspNative("111")},
            {rspNative("111"), rspApi("222")},
            {stick(1), rspNative("111"), rspApi("111")},
            {rspNative("111"), rspNative("222"), rspApi("222")},
        };
        for (const bool unsafe : {false, true}) {
            for (const auto& rows : scenarios) {
                const std::vector<NativeDeviceInfo> shown = withoutDuplicateRsps(rows, true, unsafe);
                for (const NativeDeviceInfo& r : rows) {
                    if (!cascade::source::isNativeRspRow(r)) { continue; }
                    const bool hidden = !hasRow(shown, r);
                    const bool refused =
                        refusal("driver=miri,serial=" + cascade::source::serialFromArgs(r.args),
                                state(true, false, false), rows, unsafe) ==
                        MiricsSoapyRefusal::ManagedByApi;
                    CHECK(hidden == refused);
                }
            }
        }
    }

    // THE SENTENCES, pinned: what an affected user is told is the reason the
    // radio would not open. Key words named, so a rewording cannot drop the
    // remedy or the route.
    {
        const std::string lost = miricsSoapyRefusalSentence(MiricsSoapyRefusal::SessionLost);
        CHECK(lost.find("SoapySDR") != std::string::npos);
        CHECK(lost.find("SDRplay API") != std::string::npos);
        CHECK(lost.find("restart FoxSDR") != std::string::npos);
        const std::string managed = miricsSoapyRefusalSentence(MiricsSoapyRefusal::ManagedByApi);
        CHECK(managed.find("SoapySDR") != std::string::npos);
        CHECK(managed.find("SDRplay row") != std::string::npos);
        CHECK(lost != managed);
        CHECK(std::string(miricsSoapyRefusalSentence(MiricsSoapyRefusal::None)).empty());
    }

    // THE ROWS THE LISTS OFFER: the same decision applied to a Soapy scan's
    // result, so a row that can only fail is not put in front of the user.
    {
        const std::vector<SoapyRow> scan = {
            {"SDRplay RSP1A", "driver=sdrplay,serial=111"},
            {"Mirics", "driver=miri"},
            {"B200", "driver=uhd,serial=31"},
            {"RTL", "driver=rtlsdr"},
        };
        const auto labelsOf = [](const std::vector<SoapyRow>& v) {
            std::vector<std::string> out;
            for (const SoapyRow& r : v) { out.push_back(r.label); }
            return out;
        };
        const std::vector<NativeDeviceInfo> managed = {rspNative("111"), rspApi("111")};
        // Lost: both Mirics-family rows go; nobody else's does.
        CHECK(labelsOf(withoutRefusedMiricsSoapyRows(scan, state(true, true, false), managed, false)) ==
              (std::vector<std::string>{"B200", "RTL"}));
        // Healthy, and the API manages the RSP: only the miri row goes.
        CHECK(labelsOf(withoutRefusedMiricsSoapyRows(scan, state(true, false, false), managed, false)) ==
              (std::vector<std::string>{"SDRplay RSP1A", "B200", "RTL"}));
        // No API: the list is exactly as the scan found it.
        CHECK(labelsOf(withoutRefusedMiricsSoapyRows(scan, state(false, false, false), managed, false)) ==
              labelsOf(scan));
        CHECK(withoutRefusedMiricsSoapyRows(std::vector<SoapyRow>{}, state(true, true, true), managed,
                                            false)
                  .empty());
    }

    // =========================================================================
    // THE OPEN: SoapySource::open, through a registered module of each name
    // =========================================================================
    SoapySDR::Registry regSdrplay("sdrplay", &findSdrplay, &makeSdrplay, SOAPY_SDR_ABI_VERSION);
    SoapySDR::Registry regMiri("miri", &findMiri, &makeMiri, SOAPY_SDR_ABI_VERSION);
    SoapySDR::Registry regOther("fakeother", &findOther, &makeOther, SOAPY_SDR_ABI_VERSION);

    const auto expectOpens = [](const std::string& args, std::atomic<int>& makes) {
        const int before = makes.load();
        SoapySource src;
        const bool ok = src.open(args);
        CHECK(ok);
        CHECK(makes.load() == before + 1);
        if (!ok) { std::printf("     %s: %s\n", args.c_str(), src.lastError()); }
    };
    // Refused: false, the stated reason, and - the point - the vendor module
    // was never asked to make anything.
    const auto expectRefused = [](const std::string& args, std::atomic<int>& makes,
                                  MiricsSoapyRefusal why) {
        const int before = makes.load();
        SoapySource src;
        const bool ok = src.open(args);
        CHECK(!ok);
        CHECK(makes.load() == before);
        CHECK(std::string(src.lastError()) == std::string(miricsSoapyRefusalSentence(why)));
        CHECK(std::strlen(src.lastError()) > 0);
        // Nothing half-open is left behind.
        CHECK(!src.faulted());
    };

    // CONTROL: with a healthy API and nothing managed the fakes open, so every
    // refusal below is the rule's doing and not the fixture's.
    cascade::source::setSdrPlayApiStateForTest(state(true, false, false));
    cascade::source::sdrPlayPublishNativeRows({});
    expectOpens("driver=sdrplay,serial=1811003EFB", g_makesSdrplay);
    expectOpens("driver=miri", g_makesMiri);
    expectOpens("driver=fakeother", g_makesOther);

    // REPORT B: the session is lost, then the patch page's open of the
    // SoapySDR sdrplay module (and the receiver's click on the same row).
    cascade::source::setSdrPlayApiStateForTest(state(true, true, false));
    expectRefused("driver=sdrplay,serial=1811003EFB", g_makesSdrplay, MiricsSoapyRefusal::SessionLost);
    expectRefused("driver=miri", g_makesMiri, MiricsSoapyRefusal::SessionLost);
    expectRefused("driver=miri, serial=1811003EFB", g_makesMiri, MiricsSoapyRefusal::SessionLost);
    // Not over-reaching: a module that is nobody's chip still opens.
    expectOpens("driver=fakeother", g_makesOther);

    // A worker abandoned inside the API is the same quarantine.
    cascade::source::setSdrPlayApiStateForTest(state(true, false, true));
    expectRefused("driver=sdrplay,serial=1811003EFB", g_makesSdrplay, MiricsSoapyRefusal::SessionLost);
    expectRefused("driver=miri", g_makesMiri, MiricsSoapyRefusal::SessionLost);
    expectOpens("driver=fakeother", g_makesOther);

    // REPORT A: the API is healthy and lists the RSP the Mirics module would
    // open. The published native scan is the evidence; the serial or its
    // absence decides which radio the row names.
    cascade::source::setSdrPlayApiStateForTest(state(true, false, false));
    cascade::source::sdrPlayPublishNativeRows({rspNative("1811003EFB"), rspApi("1811003EFB")});
    expectRefused("driver=miri", g_makesMiri, MiricsSoapyRefusal::ManagedByApi);
    expectRefused("driver=miri,serial=1811003EFB", g_makesMiri, MiricsSoapyRefusal::ManagedByApi);
    // A provably different radio, and the SDRplay module for a healthy API,
    // keep their routes (the second is the behaviour 0.99.59 already had).
    expectOpens("driver=miri,serial=99999999", g_makesMiri);
    expectOpens("driver=sdrplay,serial=1811003EFB", g_makesSdrplay);

    // A GENUINE MIRICS DONGLE, NO SDRPLAY API ANYWHERE: the route is kept
    // even though the scan once held a row that looks like an RSP.
    cascade::source::setSdrPlayApiStateForTest(state(false, false, false));
    expectOpens("driver=miri", g_makesMiri);
    cascade::source::sdrPlayPublishNativeRows({stick(0)});
    expectOpens("driver=miri,index=0", g_makesMiri);

    // The override is a test seam only: cleared, the process answers for
    // itself, and a machine with no SDRplay API there is not quarantined.
    cascade::source::setSdrPlayApiStateForTest(std::nullopt);
    cascade::source::sdrPlayPublishNativeRows({});
    {
        const SdrPlayApiState real = cascade::source::sdrPlayApiState();
        CHECK(!real.sessionLost);
        CHECK(!real.workerAbandoned);
    }

    (void) kNativeMiricsOpenUnsafeWithApi;
    return testSummary("test_one_chip_one_route");
}
