// test_omnirig_client.cpp - the OmniRig client model (core/omnirig_client.hpp) against an in-memory stand-in for IRigX. No COM server is started; no
// rig is touched.
//
//   O1  the RigParamX / RigStatusX numbers are the ones the OmniRig.ridl declares.
//   O2  reads: a snapshot of an online rig (frequency, mode, split, VFO, the transmit STATE); an offline rig yields nothing but the status.
//   O3  writes: frequency and mode, with every refusal (no port, offline, out of range, NaN, transmitting, unknown mode).
//   O4  NO TRANSMIT CALL, three ways: the port and client have no member that could key the rig (compile-time), the source mentions none in code,
//       and a randomised run records only the two allowed puts, never while the rig transmits or is offline.
//   O5  boundaries: rounding, the ends of the frequency range, every mode name, a mode value that is not a mode bit.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "core/omnirig_client.hpp"
#include "hamops_util.hpp"
#include "test_check.hpp"

using namespace cascade::core::rig;

namespace {

// The stand-in records every call that reaches it.
class FakeRig : public OmniRigRigPort {
public:
    std::int32_t st = ST_ONLINE, f = 14074000, fa = 14074000, fb = 7040000;
    std::uint32_t md = PM_SSB_U, vf = PM_VFOAA, sp = PM_SPLITOFF, tx = PM_RX;
    bool acceptPuts = true;
    std::vector<std::string> log;
    std::int32_t status() override { return st; }
    std::int32_t freq() override { return f; }
    std::int32_t freqA() override { return fa; }
    std::int32_t freqB() override { return fb; }
    std::uint32_t mode() override { return md; }
    std::uint32_t vfo() override { return vf; }
    std::uint32_t split() override { return sp; }
    std::uint32_t txState() override { return tx; }
    bool putFreq(std::int32_t hz) override { log.push_back("putFreq:" + std::to_string(hz)); if (acceptPuts) { f = hz; } return acceptPuts; }
    bool putMode(std::uint32_t p) override { log.push_back("putMode:" + std::to_string(p)); if (acceptPuts) { md = p; } return acceptPuts; }
};

// Compile-time absence of anything that keys a transmitter (names from IRigX / IPortBits and the usual spellings).
template <class T> concept CanPutTx = requires(T& t) { t.putTx(0u); };
template <class T> concept CanSetTx = requires(T& t) { t.setTx(0u); };
template <class T> concept CanPtt = requires(T& t) { t.setPtt(true); } || requires(T& t) { t.putPtt(true); } || requires(T& t) { t.ptt(true); } || requires(T& t) { t.key(true); };
template <class T> concept CanCustom = requires(T& t) { t.sendCustomCommand(0); } || requires(T& t) { t.SendCustomCommand(0); } || requires(T& t) { t.sendCustom(std::string()); };
template <class T> concept CanPort = requires(T& t) { t.putRts(true); } || requires(T& t) { t.putDtr(true); } || requires(T& t) { t.setRts(true); } || requires(T& t) { t.setDtr(true); } || requires(T& t) { t.portBits(); };
template <class T> concept CanSplit = requires(T& t) { t.putSplit(0u); } || requires(T& t) { t.setSplit(0u); } || requires(T& t) { t.setSplitMode(1, 2); } || requires(T& t) { t.setSimplexMode(1); };
static_assert(!CanPutTx<OmniRigRigPort> && !CanSetTx<OmniRigRigPort> && !CanPtt<OmniRigRigPort> && !CanCustom<OmniRigRigPort> && !CanPort<OmniRigRigPort> && !CanSplit<OmniRigRigPort>);
static_assert(!CanPutTx<OmniRigClient> && !CanSetTx<OmniRigClient> && !CanPtt<OmniRigClient> && !CanCustom<OmniRigClient> && !CanPort<OmniRigClient> && !CanSplit<OmniRigClient>);
// the concepts do detect such a member when it exists (so the assertions above are not vacuous)
struct HasPutTx { void putTx(unsigned) {} };
struct HasPtt { void setPtt(bool) {} };
struct HasRts { void putRts(bool) {} };
static_assert(CanPutTx<HasPutTx> && CanPtt<HasPtt> && CanPort<HasRts>);

void o1Numbers() {
    CHECK(PM_UNKNOWN == 1 && PM_FREQ == 2 && PM_FREQA == 4 && PM_FREQB == 8 && PM_PITCH == 16 && PM_RITOFFSET == 32 && PM_RIT0 == 64);
    CHECK(PM_VFOAA == 128 && PM_VFOAB == 256 && PM_VFOBA == 512 && PM_VFOBB == 1024 && PM_VFOA == 2048 && PM_VFOB == 4096);
    CHECK(PM_SPLITON == 32768 && PM_SPLITOFF == 65536 && PM_RX == 2097152 && PM_TX == 4194304);
    CHECK(PM_CW_U == 8388608 && PM_CW_L == 16777216 && PM_SSB_U == 33554432 && PM_SSB_L == 67108864 && PM_DIG_U == 134217728 && PM_DIG_L == 268435456);
    CHECK(PM_AM == 536870912 && PM_FM == 1073741824);
    CHECK(ST_NOTCONFIGURED == 0 && ST_DISABLED == 1 && ST_PORTBUSY == 2 && ST_NOTRESPONDING == 3 && ST_ONLINE == 4);
}

void o2Reads() {
    FakeRig r;
    OmniRigClient c(&r);
    RigSnapshot s = c.snapshot();
    CHECK(s.online && s.status == ST_ONLINE && s.freqHz == 14074000 && s.freqAHz == 14074000 && s.freqBHz == 7040000);
    CHECK(s.mode == PM_SSB_U && s.modeText == "USB" && !s.splitOn && !s.transmitting && s.vfoA);
    r.sp = PM_SPLITON; r.tx = PM_TX; r.vf = PM_VFOBB; r.md = PM_CW_L;
    s = c.snapshot();
    CHECK(s.splitOn && s.transmitting && !s.vfoA && s.modeText == "CW-R");
    r.vf = PM_VFOAB;
    CHECK(c.snapshot().vfoA);
    r.vf = PM_VFOA;
    CHECK(c.snapshot().vfoA);
    // a mode value that is not one of the eight: unknown, no text
    r.md = PM_UNKNOWN;
    s = c.snapshot();
    CHECK(s.mode == 0 && s.modeText.empty());
    r.md = 3;
    CHECK(c.snapshot().mode == 0);
    // not online: nothing but the status, whatever the port would answer
    for (std::int32_t st : std::initializer_list<std::int32_t>{ST_NOTCONFIGURED, ST_DISABLED, ST_PORTBUSY, ST_NOTRESPONDING, 99, -1}) {
        r.st = st;
        s = c.snapshot();
        CHECK(!s.online && s.status == st && s.freqHz == 0 && s.mode == 0 && !s.transmitting);
    }
    OmniRigClient none(nullptr);
    CHECK(!none.snapshot().online);
    CHECK(r.log.empty());   // reads never write
}

void o3Writes() {
    FakeRig r;
    OmniRigClient c(&r);
    std::string err;
    CHECK(c.setFrequency(7100000, err) && err.empty() && r.f == 7100000);
    CHECK(c.setMode("LSB", err) && r.md == PM_SSB_L);
    CHECK(r.log == (std::vector<std::string>{"putFreq:7100000", "putMode:" + std::to_string(PM_SSB_L)}));
    r.log.clear();
    // refusals leave no trace in the log
    CHECK(!c.setFrequency(0, err) && !err.empty());
    CHECK(!c.setFrequency(-5, err));
    CHECK(!c.setFrequency(0.4, err));
    CHECK(!c.setFrequency(2147483648.0, err));
    CHECK(!c.setFrequency(std::nan(""), err));
    CHECK(!c.setFrequency(std::numeric_limits<double>::infinity(), err));
    CHECK(!c.setMode("PM_TX", err));
    CHECK(!c.setMode("TX", err));
    CHECK(!c.setMode("", err));
    CHECK(!c.setMode("usb", err));          // names are exact
    r.st = ST_NOTRESPONDING;
    CHECK(!c.setFrequency(7100000, err) && !c.setMode("USB", err));
    r.st = ST_ONLINE;
    r.tx = PM_TX;                            // the rig is transmitting: nothing is sent
    CHECK(!c.setFrequency(7100000, err) && err == "the rig is transmitting");
    CHECK(!c.setMode("USB", err) && err == "the rig is transmitting");
    r.tx = PM_RX;
    CHECK(r.log.empty());
    OmniRigClient none(nullptr);
    CHECK(!none.setFrequency(7100000, err) && !none.setMode("USB", err));
    // OmniRig declines
    r.acceptPuts = false;
    CHECK(!c.setFrequency(7100000, err) && !err.empty());
    CHECK(!c.setMode("AM", err) && !err.empty());
    CHECK(r.log.size() == 2);
}

void o4NoTransmit() {
    // the writable set is exactly the eight modes
    int writable = 0;
    for (int bit = 0; bit < 31; ++bit) {
        const std::uint32_t p = 1u << bit;
        if (isWritableMode(p)) { ++writable; CHECK(p >= PM_CW_U); }
    }
    CHECK(writable == 8);
    CHECK(!isWritableMode(PM_TX) && !isWritableMode(PM_RX) && !isWritableMode(0) && !isWritableMode(PM_CW_U | PM_FM) && !isWritableMode(PM_SPLITON));
    // the source: PM_TX / PM_RX appear in code only as the enum value or in a comparison; none of the keying names appear
    const std::string src = hamops_test::stripCommentsAndStrings(hamops_test::srcFile("src/core/omnirig_client.hpp"));
    CHECK(!src.empty());
    for (const char* bad : {"putTx", "setTx", "Ptt", "ptt", "SendCustomCommand", "sendCustom", "PortBits", "putRts", "putDtr", "Rts", "Dtr", "stderr", "stdout", "std::cerr", "CoCreateInstance",
                            "CoInitialize", "#include <windows.h>", "IDispatch", "settings_set", "std::thread"}) {
        CHECK(src.find(bad) == std::string::npos);
    }
    std::size_t pos = 0;
    int enumUse = 0, compareUse = 0;
    while ((pos = src.find("PM_TX", pos)) != std::string::npos) {
        const bool inEnum = src.compare(pos, 13, "PM_TX = 41943") == 0;
        const bool compare = pos >= 3 && src.compare(pos - 3, 3, "== ") == 0;
        CHECK(inEnum || compare);
        if (inEnum) { ++enumUse; }
        if (compare) { ++compareUse; }
        pos += 5;
    }
    CHECK(enumUse == 1 && compareUse >= 2);
    pos = 0;
    while ((pos = src.find("PM_RX", pos)) != std::string::npos) {
        CHECK(src.compare(pos, 13, "PM_RX = 20971") == 0);   // the enum value only: never written anywhere
        pos += 5;
    }
    // randomised: whatever the inputs and the rig's state, the stand-in sees only allowed puts, never under power or offline
    hamops_test::Rng rng(13);
    FakeRig r;
    OmniRigClient c(&r);
    const std::vector<std::string> names = {"CW", "CW-R", "USB", "LSB", "DIG-U", "DIG-L", "AM", "FM", "TX", "RX", "PM_TX", "", "usb", "SPLIT"};
    std::string err;
    int accepted = 0;
    for (int i = 0; i < 20000; ++i) {
        r.st = rng.below(4) == 0 ? static_cast<std::int32_t>(rng.below(6)) : ST_ONLINE;
        r.tx = rng.below(3) == 0 ? PM_TX : PM_RX;
        r.log.clear();
        bool ok = false;
        if (rng.below(2)) {
            const double hz = (static_cast<double>(rng.next() % 4000000000ull) - 100.0) * (rng.below(10) == 0 ? 0.5 : 1.0);
            ok = c.setFrequency(hz, err);
            if (ok) { CHECK(r.log.size() == 1 && r.log[0].rfind("putFreq:", 0) == 0); const long long v = std::stoll(r.log[0].substr(8)); CHECK(v >= 1 && v <= 2147483647LL); }
        } else {
            ok = c.setMode(names[rng.below(static_cast<std::uint32_t>(names.size()))], err);
            if (ok) { CHECK(r.log.size() == 1 && r.log[0].rfind("putMode:", 0) == 0); CHECK(isWritableMode(static_cast<std::uint32_t>(std::stoul(r.log[0].substr(8))))); }
        }
        if (ok) { ++accepted; CHECK(r.st == ST_ONLINE && r.tx != PM_TX); } else { CHECK(r.log.empty()); }
        c.snapshot();
        CHECK(r.log.size() <= 1);
    }
    CHECK(accepted > 1000);
}

void o5Boundaries() {
    FakeRig r;
    OmniRigClient c(&r);
    std::string err;
    CHECK(c.setFrequency(14074000.4, err) && r.f == 14074000);
    CHECK(c.setFrequency(14074000.5, err) && r.f == 14074001);
    CHECK(c.setFrequency(1.0, err) && r.f == 1);                       // the smallest accepted
    CHECK(c.setFrequency(2147483647.0, err) && r.f == 2147483647);     // the largest
    CHECK(!c.setFrequency(0.99, err));
    for (const char* n : {"CW", "CW-R", "USB", "LSB", "DIG-U", "DIG-L", "AM", "FM"}) {
        CHECK(c.setMode(n, err));
        CHECK(modeName(r.md) == std::string(n));
        CHECK(modeFromName(n) == r.md);
    }
    CHECK(modeFromName("nope") == 0 && std::string(modeName(PM_TX)).empty() && std::string(modeName(0)).empty());
}

}  // namespace

int main() {
    o1Numbers();
    o2Reads();
    o3Writes();
    o4NoTransmit();
    o5Boundaries();
    return testSummary("test_omnirig_client");
}
