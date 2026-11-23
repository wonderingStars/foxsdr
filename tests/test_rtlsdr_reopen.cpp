// test_rtlsdr_reopen.cpp - AN RTL-SDR WHOSE HANDLE WENT BAD IS CLOSED AND OPENED AGAIN, NOT CALLED DEAD (diagnosis 2026-11-23, signature 80913FA5).
//
// The field (44 radios lost in four days, 2026-11-19 to 2026-11-23): a bulk read fails with Windows error 31, the stream restart's control write is refused in the same
// millisecond ("the radio would not reset its endpoint"), and the radio is called dead although it is still on the bus - because the device re-enumerated under the
// handle, or the chip lost its registers. A pipe restart cannot repair that; the reader thread must close the handle, wait for the same radio to come back, open it,
// run the whole bring-up, put the user's settings back and restart the stream into the same ring. This holds, against a fake transport for each failure shape:
//   U1   dead handle after a re-enumeration: the radio comes back, the source is the same object, and the new transport received BYTE FOR BYTE the writes a plain
//        open plus the same setters would have sent; nothing is raised until samples flow, then `usbreopen`;
//   U2   the device is back after a delay: the wait polls until it is listed;
//   U3   the device is gone for good: a fault inside the wait, worded "unplugged", cause `unplugged`, the polling bounded;
//   U4   another program took the radio in the gap and kept it: a fault at the END of the wait (decision S9: not at once), cause `fault`, the polling goes on to there;
//   U5   the chip lost its registers (the handle is fine, the endpoint is silent): only the full bring-up on the new handle makes it deliver;
//   U6   a radio that keeps dropping off: two re-opens per window, then a fault saying so; and twelve per open when the window is forgiven;
//   U7   a stop() inside a re-open, in each of its five phases (the handle close, the wait, the open of the new handle, the bring-up, the stream restart): the
//        stop wins inside the join bound, the radio is left CLOSED and latched FAULTED with the true sentence (kReopenStoppedSentence; cause `fault`, never
//        unplugged), no half-initialised chip is reachable, a following start() is REFUSED with those words, and a fresh open of the same radio streams
//        (decision F1; this replaces the first U7 and U8, which asserted the half-open state the review found wrong: "not faulted", "not dead");
//   U8   (see U7: the bring-up phase is one of its five)
//   U9   closeDevice() during the wait returns and leaks nothing;
//   U10  control calls during the re-open return at once, and what they set is what the radio comes back to;
//   U11  the bias tee comes back on the first re-open and is left OFF on a repeat in the same window;
//   U12  a restart that loses the race for the device lock to a control call is a retry, not a fault (defect D1);
//   U13  the outcome words: nothing is raised before samples flow; streamback / usbreopen after; streamfail / usbreopenfail when it did not work;
//   U14  Windows error 995 is restarted like 31;
//   U15  the issue sentences are neutral (no radio family named for a word that is not limited to it);
// and the second independent review's decisions (2026-11-23, S1 to S11):
//   U27  (S1)  the bound of one attempt's bring-up, settings and stream restart is 10000 ms, and a short bound set through the hook still ends a bring-up;
//   U28  (S9, S2) "in use by another program" keeps the wait going to its end: gone a moment later = the radio comes back; still the answer at the end = the fault;
//        with two same-serial dongles listed, one of them in use, the re-open does not fault at once;
//   U29  (S3)  a stop() that lands before the re-open's first step latches the fault (and so does a start() that fails on a dead handle); picking again streams;
//   U30  (S4)  the Failed, Absent and Mismatch exits write nothing for an abandoned reader, and a stop() that raced them is the stopped sentence;
//   U31  (S5)  an empty USB string at the re-open is read again and is not a different radio; two different non-empty strings are;
//   U32  (S6)  a setting changed back to ZERO during the settings rounds is applied; after three unsettled rounds one last pass applies the newest wish;
//   U33  (S7)  a setter held through the end of a re-open reaches the chip through retakeAfterReopen, the reader standing aside for it;
//   U34  (S10) a user's stop during a reconnect is not journalled as a radio fault; a real fault still is.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/bias_tee.hpp"
#include "core/diag_log.hpp"
#include "core/health_events.hpp"
#include "core/issue_report.hpp"
#include "core/radio_events.hpp"
#include "source/rtlsdr_source.hpp"
#include "test_check.hpp"
#include "usb/usb_fake.hpp"

using cascade::source::RtlSdrSource;
using cascade::usb::FakeUsbDevice;
namespace health = cascade::core::health;

namespace {

using Clock = std::chrono::steady_clock;

const std::vector<std::uint8_t> kTunerAnswer = {0x69, 0x00, 0x02, 0x00, 0xA4};

void dressAsR820T(FakeUsbDevice& fake) {
    fake.answerIn(0, 0x0034, 0x0600, kTunerAnswer);
    fake.answerIn(0, 0x00a0, 0x0600, {0x00});
}

std::vector<std::uint8_t> payload(std::size_t bytes) { return std::vector<std::uint8_t>(bytes, 0x80); }

bool waitFor(const std::function<bool()>& pred, int ms) {
    const auto until = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < until) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

std::size_t drain(RtlSdrSource& src, std::size_t want, int ms) {
    std::vector<std::complex<float>> buf(4096);
    std::size_t total = 0;
    const auto until = Clock::now() + std::chrono::milliseconds(ms);
    while (total < want && Clock::now() < until) { total += src.read(buf.data(), buf.size()); }
    return total;
}

long long msSince(Clock::time_point t) { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count(); }

// A fake that delivers samples after every stream start, as a healthy dongle does.
std::unique_ptr<FakeUsbDevice> goodFake() {
    auto f = std::make_unique<FakeUsbDevice>();
    dressAsR820T(*f);
    f->onBeginBulk = [](FakeUsbDevice& d) { for (int i = 0; i < 4; ++i) { d.bulkQueue.push_back(payload(2048)); } };
    return f;
}

// A fake whose handle is dead: every read fails with Windows error 31 and (when `controlsToo`) every control transfer from now on fails.
void kill(FakeUsbDevice& f, bool controlsToo = true) {
    if (controlsToo) { f.failControlAfter = f.controlCalls; }
    f.failBulkAfterText = "a bulk read failed (Windows error 31)";
    f.failBulkAfter = f.bulkReads;
}

// What the seam hands out, in order, and how it behaves.
struct Pool {
    std::mutex m;
    std::deque<std::unique_ptr<FakeUsbDevice>> fakes;
    std::vector<FakeUsbDevice*> handed;
    std::atomic<int> calls{0};
    int absentFirst = 0;                 // the first N calls say "not on the bus"
    bool inUse = false;                  // every call says another program holds the radio
    std::function<void(FakeUsbDevice&)> onHand;
    std::function<void()> beforeHand;    // run on the reader's thread inside the seam, after the device is chosen and before it is returned (a CreateFile that takes time)
    std::function<std::string()> ambiguous;   // when set and it returns text: every call answers "more than one radio could be this one" with that error
    std::function<void()> beforeAnswer;       // run on the reader's thread at the top of every call, before the answer is chosen (a finder that takes time to say no)
};

RtlSdrSource::ReopenFn seamOf(const std::shared_ptr<Pool>& p) {
    return [p](std::string& error, bool& present) -> std::unique_ptr<cascade::usb::UsbDevice> {
        const int n = ++p->calls;
        if (p->beforeAnswer) { p->beforeAnswer(); }
        std::unique_ptr<FakeUsbDevice> f;
        {
            std::lock_guard<std::mutex> lk(p->m);
            if (p->inUse) {
                error = cascade::usb::kInUseError;
                present = true;
                return nullptr;
            }
            if (p->ambiguous) {
                error = p->ambiguous();
                present = true;
                return nullptr;
            }
            if (n <= p->absentFirst) {
                present = false;
                return nullptr;
            }
            present = true;
            if (p->fakes.empty()) {
                error = "fake: no device to hand out";
                return nullptr;
            }
            f = std::move(p->fakes.front());
            p->fakes.pop_front();
            p->handed.push_back(f.get());
            if (p->onHand) { p->onHand(*f); }
        }
        if (p->beforeHand) { p->beforeHand(); }
        return f;
    };
}

std::vector<std::string> writeTexts(const FakeUsbDevice& f) {
    std::vector<std::string> v;
    for (const cascade::usb::FakeControl& c : f.writes()) { v.push_back(c.text()); }
    return v;
}

// The words raised since the last clear, as a set of bit positions of health::Recovered.
bool raised(health::Recovered r) { return (health::raisedRecoveredForTest() & (1ull << static_cast<unsigned>(r))) != 0; }

struct Opened {
    RtlSdrSource src;
    FakeUsbDevice* live = nullptr;
};

void openOn(Opened& o, std::unique_ptr<FakeUsbDevice> fk) {
    o.live = fk.get();
    CHECK(o.src.openWithTransport(std::move(fk), "fake R820T dongle"));
}

// WHAT A FAKE THAT THE SOURCE FREES CAN STILL TELL: these outlive it (the hooks keep a shared pointer).
struct Count {
    std::atomic<int> begins{0};   // stream starts on it
    std::atomic<int> ends{0};     // stream ends on it
    std::shared_ptr<std::vector<std::string>> journal = std::make_shared<std::vector<std::string>>();
};
void instrument(FakeUsbDevice& f, const std::shared_ptr<Count>& c) {
    f.journal = c->journal;
    const auto prevBegin = f.onBeginBulk;
    f.onBeginBulk = [c, prevBegin](FakeUsbDevice& d) {
        c->begins.fetch_add(1);
        if (prevBegin) { prevBegin(d); }
    };
    const auto prevEnd = f.onEndBulk;
    f.onEndBulk = [c, prevEnd] {
        c->ends.fetch_add(1);
        if (prevEnd) { prevEnd(); }
    };
}
bool closedIn(const Count& c) {
    for (const std::string& l : *c.journal) {
        if (l == "closed") { return true; }
    }
    return false;
}

// A radio that LEFT THE BUS: the read says Windows' own words for it, so the reader goes straight to the re-open (no stream restart first).
const char* const kGoneText = "a bulk read failed (Windows error 1167)";
void killGone(FakeUsbDevice& f) {
    f.failBulkAfterText = kGoneText;
    f.failBulkAfter = f.bulkReads;
}

// How many lines of the driver's log contain `what` (the ring of core/diag_log: nothing is written to disk).
int logCount(const char* what) {
    int n = 0;
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (l.find(what) != std::string::npos) { ++n; }
    }
    return n;
}

// THE BIAS TEE ON THE WIRE. A dongle whose three GPIO registers are remembered as a real chip remembers them, and whose every write to the output register is
// kept: the bias tee is bit 0 (GPIO 0). `forced` is a valid EEPROM whose byte 7 bit 1 is clear (the maker wired the tee on; the driver switches it on at open);
// `valid` without it is a valid EEPROM that does not; neither is a dongle with no EEPROM at all.
constexpr std::uint16_t kSysWrite = 0x0210;
constexpr std::uint16_t kSysRead = 0x0200;
constexpr std::uint16_t kGpo = 0x3001;
constexpr std::uint16_t kGpoe = 0x3003;
constexpr std::uint16_t kGpd = 0x3004;
struct Wire {
    std::mutex m;
    std::vector<std::uint8_t> gpo;   // every byte written to GPO, in order
    void add(std::uint8_t b) {
        std::lock_guard<std::mutex> lk(m);
        gpo.push_back(b);
    }
    bool everOn() {
        std::lock_guard<std::mutex> lk(m);
        for (const std::uint8_t b : gpo) {
            if ((b & 0x01) != 0) { return true; }
        }
        return false;
    }
    bool lastOn() {
        std::lock_guard<std::mutex> lk(m);
        return !gpo.empty() && (gpo.back() & 0x01) != 0;
    }
    std::size_t writes() {
        std::lock_guard<std::mutex> lk(m);
        return gpo.size();
    }
};
std::unique_ptr<FakeUsbDevice> dongle(const std::shared_ptr<Wire>& wire, bool valid, bool forced) {
    auto f = goodFake();
    if (valid) {
        const std::uint8_t byte7 = forced ? 0x00 : 0x02;
        for (const std::uint8_t b : {std::uint8_t{0x28}, std::uint8_t{0x32}, std::uint8_t{0x00}, std::uint8_t{0x00}, std::uint8_t{0x00}, std::uint8_t{0x00}, std::uint8_t{0x00}, byte7}) {
            f->queueIn(0, 0x00a0, 0x0600, {b});
        }
    }
    for (const std::uint16_t r : {kGpo, kGpoe, kGpd}) { f->answerIn(0, r, kSysRead, {0x00}); }
    FakeUsbDevice* self = f.get();
    f->onControlOut = [self, wire](const cascade::usb::FakeControl& c) {
        for (const std::uint16_t r : {kGpo, kGpoe, kGpd}) {
            if (c.out && c.request == 0 && c.index == kSysWrite && c.value == r && !c.data.empty()) {
                self->answerIn(0, r, kSysRead, {c.data[0]});
                if (r == kGpo) { wire->add(c.data[0]); }
            }
        }
    };
    return f;
}

// USB strings, as GET_DESCRIPTOR returns them (length, type 3, UTF-16LE), with the device descriptor naming their indexes: the identity the re-open compares.
std::vector<std::uint8_t> strDescriptor(const std::string& s) {
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>(2 + 2 * s.size()));
    out.push_back(0x03);
    for (const char c : s) {
        out.push_back(static_cast<std::uint8_t>(c));
        out.push_back(0x00);
    }
    return out;
}
void withStrings(FakeUsbDevice& f, const std::string& manufacturer, const std::string& product) {
    f.answerIn(0x06, 0x0100, 0x0000, {0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0xDA, 0x0B, 0x38, 0x28, 0x00, 0x01, 0x01, 0x02, 0x03, 0x01});
    f.answerIn(0x06, 0x0300, 0x0000, {0x04, 0x03, 0x09, 0x04});
    f.answerIn(0x06, 0x0301, 0x0409, strDescriptor(manufacturer));
    f.answerIn(0x06, 0x0302, 0x0409, strDescriptor(product));
}

}  // namespace

int main() {
    std::printf("test_rtlsdr_reopen\n");

    // U1: the handle is dead (reads fail with 31, every control transfer fails). The seam supplies a new device; samples flow; the source is the same object; the
    // new transport got the same bytes as a plain open plus the same setters.
    {
        health::clearRaisedRecoveredForTest();
        auto pool = std::make_shared<Pool>();
        pool->fakes.push_back(goodFake());
        Opened o;
        auto f0 = goodFake();
        f0->bulkQueue.push_back(payload(2048));
        openOn(o, std::move(f0));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(3000, 20);
        // the user's settings, in the order a re-open puts them back
        CHECK(o.src.setFreqCorrectionPpm(7));
        CHECK(o.src.setSampleRateHz(1024000.0));
        CHECK(o.src.setIfBandwidthHz(1500000));
        CHECK(o.src.setGainDb("TUNER", 20.0));
        CHECK(o.src.setCenterFrequencyHz(433920000.0));
        CHECK(o.src.setBiasT(true));
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        kill(*o.live);
        const auto t0 = Clock::now();
        CHECK(waitFor([&] { return pool->calls.load() >= 1 && !pool->handed.empty(); }, 4000));
        const std::size_t got = drain(o.src, 4096, 4000);
        std::printf("  dead handle: %zu samples after the re-open, %lld ms, faulted %d, seam calls %d\n", got, msSince(t0), o.src.faulted() ? 1 : 0, pool->calls.load());
        CHECK(got >= 4096);
        CHECK(!o.src.faulted());
        CHECK(!o.src.deviceDead());
        CHECK(o.src.reopenCount() == 1);
        CHECK(o.src.centerFrequencyHz() == 433920000.0);
        CHECK(o.src.biasT());
        CHECK(waitFor([] { return raised(health::Recovered::UsbReopen); }, 2000));
        CHECK(!raised(health::Recovered::UsbReopenFail));
        CHECK(!raised(health::Recovered::StreamFail));
        CHECK(!raised(health::Recovered::StreamRestart));      // no native driver files the word that says only "a restart began"
        FakeUsbDevice* fresh = pool->handed.empty() ? nullptr : pool->handed[0];
        CHECK(fresh != nullptr);
        if (fresh != nullptr) {
            // THE REFERENCE: a plain open on a fresh fake, the same setters in the same order, then the stream start.
            RtlSdrSource ref;
            auto rf = std::make_unique<FakeUsbDevice>();
            dressAsR820T(*rf);
            FakeUsbDevice* refLive = rf.get();
            CHECK(ref.openWithTransport(std::move(rf), "fake R820T dongle"));
            CHECK(ref.setFreqCorrectionPpm(7));
            CHECK(ref.setSampleRateHz(1024000.0));
            CHECK(ref.setIfBandwidthHz(1500000));
            CHECK(ref.setGainDb("TUNER", 20.0));
            CHECK(ref.setCenterFrequencyHz(433920000.0));
            CHECK(ref.setBiasT(true));
            CHECK(ref.start());
            const std::vector<std::string> want = writeTexts(*refLive);
            const std::vector<std::string> have = writeTexts(*fresh);
            ref.stop();
            std::printf("  the new transport got %zu writes, the reference %zu\n", have.size(), want.size());
            CHECK(!want.empty());
            CHECK(have.size() == want.size());
            for (std::size_t i = 0; i < want.size() && i < have.size(); ++i) {
                if (want[i] != have[i]) { std::printf("  FIRST DIFFERENCE at write %zu: want %s, have %s\n", i, want[i].c_str(), have[i].c_str()); }
                CHECK(want[i] == have[i]);
                if (want[i] != have[i]) { break; }
            }
        }
        o.src.stop();
    }

    // U2: the radio is listed again only after three polls.
    {
        auto pool = std::make_shared<Pool>();
        pool->fakes.push_back(goodFake());
        pool->absentFirst = 3;
        Opened o;
        openOn(o, goodFake());
        o.live->bulkQueue.push_back(payload(2048));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(3000, 20);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        kill(*o.live);
        const std::size_t got = drain(o.src, 2048, 4000);
        std::printf("  back after a delay: %zu samples, seam calls %d, faulted %d\n", got, pool->calls.load(), o.src.faulted() ? 1 : 0);
        CHECK(got >= 2048);
        CHECK(!o.src.faulted());
        CHECK(pool->calls.load() == 4);
        o.src.stop();
    }

    // U3: the radio never comes back. A fault inside the wait, worded as an unplug, and the polling is bounded.
    {
        auto pool = std::make_shared<Pool>();
        pool->absentFirst = 1000000;
        Opened o;
        openOn(o, goodFake());
        o.live->bulkQueue.push_back(payload(2048));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(400, 50);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        const auto t0 = Clock::now();
        kill(*o.live);
        CHECK(waitFor([&] { return o.src.faulted(); }, 4000));
        const long long took = msSince(t0);
        const std::string said = o.src.lastError();
        std::printf("  gone for good: %lld ms, %d seam calls: %s\n", took, pool->calls.load(), said.c_str());
        CHECK(took >= 350 && took < 2500);
        CHECK(said.find("unplugged") != std::string::npos);
        CHECK(cascade::core::radioreport::closeCauseFor(true, said) == "unplugged");
        CHECK(pool->calls.load() >= 2 && pool->calls.load() <= 12);
        CHECK(o.src.deviceDead());
        o.src.stop();
    }

    // U4 (changed by decision S9 of the second review: it asserted "a fault at once, no further polling"): another program took the radio in the gap and KEEPS it. The wait
    // goes on to its end, because the other holder may be gone a moment later; the fault is made only because that is still the answer when the wait ends - a fault (not
    // an unplug), worded with the existing "already in use by another program" sentence.
    {
        auto pool = std::make_shared<Pool>();
        pool->inUse = true;
        Opened o;
        openOn(o, goodFake());
        o.live->bulkQueue.push_back(payload(2048));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(600, 50);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        const auto t0 = Clock::now();
        kill(*o.live);
        CHECK(waitFor([&] { return o.src.faulted(); }, 4000));
        const long long took = msSince(t0);
        const std::string said = o.src.lastError();
        std::printf("  taken by another program, kept: %lld ms, %d seam calls: %s\n", took, pool->calls.load(), said.c_str());
        CHECK(took >= 550 && took < 3000);       // the end of the 600 ms wait, not the first poll
        CHECK(pool->calls.load() >= 5);          // polled every 50 ms meanwhile
        CHECK(said.find("another program") != std::string::npos);
        CHECK(said.find("reopening it did not help") != std::string::npos);
        CHECK(cascade::core::radioreport::closeCauseFor(true, said) == "fault");
        o.src.stop();
    }

    // U5: the chip lost its registers: the handle answers, the endpoint is silent. The new device delivers ONLY when the bring-up has been run on it.
    {
        auto pool = std::make_shared<Pool>();
        auto f1 = std::make_unique<FakeUsbDevice>();
        dressAsR820T(*f1);
        f1->onBeginBulk = [](FakeUsbDevice& d) {
            // the first write of the bring-up (the USB system control register, 0x09) must have been seen on THIS device, and the demodulator's power-up with it
            bool sawBringUp = false;
            for (const cascade::usb::FakeControl& c : d.controls) {
                if (c.out && c.value == 0x2000 && !c.data.empty() && c.data[0] == 0x09) { sawBringUp = true; }
            }
            if (sawBringUp && d.controls.size() > 40) { for (int i = 0; i < 4; ++i) { d.bulkQueue.push_back(payload(2048)); } }
        };
        pool->fakes.push_back(std::move(f1));
        Opened o;
        auto f0 = std::make_unique<FakeUsbDevice>();
        dressAsR820T(*f0);
        // the old handle delivers while "its registers are right", and is silent after the loss
        f0->bulkQueue.push_back(payload(2048));
        auto oldStarts = std::make_shared<std::atomic<int>>(0);   // (the old fake is freed when the radio is reopened, so its count is kept here)
        f0->onBeginBulk = [oldStarts](FakeUsbDevice&) { oldStarts->fetch_add(1); };
        openOn(o, std::move(f0));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(3000, 20);
        o.src.setStallBoundsForTest(150, 20000);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        const std::size_t got = drain(o.src, 2048, 6000);
        std::printf("  chip lost its registers: %zu samples after the re-open, seam calls %d, stream starts on the old handle %d\n", got, pool->calls.load(), oldStarts->load());
        CHECK(got >= 2048);
        CHECK(!o.src.faulted());
        CHECK(oldStarts->load() == 4);          // the start and three restarts did not bring samples back
        CHECK(pool->calls.load() == 1);
        o.src.stop();
    }

    // U6: a radio that keeps dropping off. Two re-opens in a window, then a fault that says so.
    {
        health::clearRaisedRecoveredForTest();
        auto pool = std::make_shared<Pool>();
        for (int i = 0; i < 4; ++i) {
            auto f = std::make_unique<FakeUsbDevice>();
            dressAsR820T(*f);
            f->failBulkAfter = 0;               // every read fails with error 31 from the start
            f->failBulkAfterText = "a bulk read failed (Windows error 31)";
            pool->fakes.push_back(std::move(f));
        }
        Opened o;
        openOn(o, goodFake());
        o.live->bulkQueue.push_back(payload(2048));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(3000, 20);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        kill(*o.live);
        CHECK(waitFor([&] { return o.src.faulted(); }, 8000));
        const std::string said = o.src.lastError();
        std::printf("  flapping: %s (seam calls %d)\n", said.c_str(), pool->calls.load());
        CHECK(pool->calls.load() == 2);
        CHECK(said.find("keeps dropping off USB") != std::string::npos);
        CHECK(said.find("unplugged") == std::string::npos);
        CHECK(cascade::core::radioreport::closeCauseFor(true, said) == "fault");
        CHECK(raised(health::Recovered::UsbReopenFail));
        CHECK(!raised(health::Recovered::UsbReopen));
        o.src.stop();
    }

    // U6b: the window is forgiven once samples flow, so only the per-open cap of twelve stops it: the radio delivers one buffer after every re-open, and dies again.
    {
        health::clearRaisedRecoveredForTest();
        auto pool = std::make_shared<Pool>();
        for (int i = 0; i < 20; ++i) {
            auto f = std::make_unique<FakeUsbDevice>();
            dressAsR820T(*f);
            f->onBeginBulk = [](FakeUsbDevice& d) { if (d.streamStarts == 1) { d.bulkQueue.push_back(payload(2048)); } };
            f->failBulkAfter = 1;               // the first read delivers; every later one fails
            f->failBulkAfterText = "a bulk read failed (Windows error 31)";
            pool->fakes.push_back(std::move(f));
        }
        Opened o;
        openOn(o, goodFake());
        o.live->bulkQueue.push_back(payload(2048));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(3000, 5, 0);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        kill(*o.live);
        std::vector<std::complex<float>> buf(4096);
        CHECK(waitFor([&] { o.src.read(buf.data(), buf.size()); return o.src.faulted(); }, 20000));
        const std::string said = o.src.lastError();
        std::printf("  per-open cap: %s (seam calls %d, re-opens %d)\n", said.c_str(), pool->calls.load(), o.src.reopenCount());
        CHECK(pool->calls.load() == 12);
        CHECK(o.src.reopenCount() == 12);
        CHECK(said.find("keeps dropping off USB") != std::string::npos);
        // THE WORD (decision F7): the cap ended a NEW episode (the earlier re-opens belonged to episodes that samples had closed), and the word is `usbreopenfail` all the same -
        // never `streamfail`, never nothing.
        CHECK(waitFor([] { return raised(health::Recovered::UsbReopenFail); }, 2000));
        CHECK(!raised(health::Recovered::StreamFail));
        o.src.stop();
    }

    // U7 / U8 (decision F1): a stop() that lands INSIDE a re-open, in each of the five phases, leaves ONE well-defined state - the radio closed and latched faulted with
    // the true sentence - however far the re-open had got. The two blocks this replaces asserted "not dead, not faulted" (a radio that was neither working nor marked
    // dead: a following start() failed for good and picking the row again did nothing; or, from the bring-up, a half-initialised chip a start() would stream from).
    //   phase 1  the handle is being closed          (the old transport's endBulkStream, on the reader's thread)
    //   phase 2  the wait for the radio to be listed (the poll slices)
    //   phase 3  the new handle is being opened      (CreateFile / WinUsb_Initialize: the seam blocks, then hands a device back)
    //   phase 4  the chip bring-up                   (every control transfer of the new device takes 20 ms)
    //   phase 5  the stream restart                  (the new device's beginBulkStream)
    for (int phase = 1; phase <= 5; ++phase) {
        health::clearRaisedRecoveredForTest();
        auto pool = std::make_shared<Pool>();
        auto newCount = std::make_shared<Count>();
        auto oldCount = std::make_shared<Count>();
        auto inside = std::make_shared<std::atomic<bool>>(false);
        auto armed = std::make_shared<std::atomic<bool>>(false);
        auto fresh = goodFake();
        instrument(*fresh, newCount);
        if (phase == 2) { pool->absentFirst = 1000000; }
        if (phase == 3) {
            pool->beforeHand = [inside] {
                inside->store(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            };
        }
        if (phase == 4) {
            fresh->refuseControlOut = [inside](const cascade::usb::FakeControl&) {
                inside->store(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                return false;
            };
        }
        if (phase == 5) {
            const auto counted = fresh->onBeginBulk;
            fresh->onBeginBulk = [inside, counted](FakeUsbDevice& d) {
                if (counted) { counted(d); }
                inside->store(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            };
        }
        pool->fakes.push_back(std::move(fresh));
        Opened o;
        auto f0 = goodFake();
        instrument(*f0, oldCount);
        f0->bulkQueue.push_back(payload(2048));
        if (phase == 1) {
            const auto counted = f0->onEndBulk;
            auto taken = std::make_shared<std::atomic<bool>>(false);
            f0->onEndBulk = [inside, armed, taken, counted] {
                if (counted) { counted(); }
                if (armed->load() && !taken->exchange(true)) {
                    inside->store(true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                }
            };
        }
        openOn(o, std::move(f0));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(5000, phase == 2 ? 250 : 50);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        armed->store(true);
        killGone(*o.live);
        if (phase == 2) {
            CHECK(waitFor([&] { return pool->calls.load() >= 1; }, 4000));
        } else {
            CHECK(waitFor([&] { return inside->load(); }, 4000));
            if (phase == 4) { std::this_thread::sleep_for(std::chrono::milliseconds(60)); }
        }
        const int callsBefore = pool->calls.load();
        const auto t0 = Clock::now();
        o.src.stop();
        const long long took = msSince(t0);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        const std::string said = o.src.lastError();
        std::printf("  stop in phase %d: %lld ms, faulted %d, dead %d, seam calls %d -> %d, new device streams started %d ended %d, closed %d: %s\n", phase, took, o.src.faulted() ? 1 : 0,
                    o.src.deviceDead() ? 1 : 0, callsBefore, pool->calls.load(), newCount->begins.load(), newCount->ends.load(), closedIn(*newCount) ? 1 : 0, said.c_str());
        CHECK(took < 1000);
        CHECK(o.src.faulted());
        CHECK(o.src.deviceDead());
        CHECK(said == cascade::source::kReopenStoppedSentence);
        CHECK(said.find("unplugged") == std::string::npos);
        CHECK(cascade::core::radioreport::closeCauseFor(true, said) == "fault");
        CHECK(!o.src.running());
        CHECK(closedIn(*oldCount));                     // the old handle is closed
        if (phase >= 3) { CHECK(closedIn(*newCount)); }  // the new handle, installed or not, is closed again
        // no half-initialised chip is reachable: a stream was never left running on the new device
        if (phase == 5) {
            CHECK(newCount->begins.load() == 1);
            CHECK(newCount->ends.load() >= 1);
        } else {
            CHECK(newCount->begins.load() == 0);
        }
        if (phase != 3 && phase != 4 && phase != 5) { CHECK(pool->calls.load() == callsBefore); }   // nothing polls after the stop
        // THE FOLLOWING start() IS REFUSED, with the same words (not "no radio open"), and nothing streams
        CHECK(!o.src.start());
        CHECK(std::string(o.src.lastError()) == cascade::source::kReopenStoppedSentence);
        CHECK(!o.src.running());
        CHECK(newCount->begins.load() == (phase == 5 ? 1 : 0));
        // ...and PICKING THE SAME ROW AGAIN is a full fresh open of the radio: a new source object on a fresh device streams and has the open's own settings (the engine
        // side of the pick is tests/test_rtlsdr_repick_engine.cpp)
        {
            RtlSdrSource again;
            auto f1 = goodFake();
            f1->bulkQueue.push_back(payload(2048));
            CHECK(again.openWithTransport(std::move(f1), "fake R820T dongle"));
            CHECK(again.start());
            CHECK(drain(again, 1024, 3000) >= 1024);
            CHECK(again.centerFrequencyHz() == 100000000.0);
            CHECK(!again.faulted());
            again.stop();
        }
    }

    // U9: closeDevice() during the wait returns, and the radio is closed.
    {
        auto pool = std::make_shared<Pool>();
        pool->absentFirst = 1000000;
        Opened o;
        openOn(o, goodFake());
        o.live->bulkQueue.push_back(payload(2048));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(5000, 250);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        kill(*o.live);
        CHECK(waitFor([&] { return pool->calls.load() >= 1; }, 4000));
        const auto t0 = Clock::now();
        o.src.closeDevice();
        std::printf("  close during the wait: %lld ms, open %d\n", msSince(t0), o.src.isOpen() ? 1 : 0);
        CHECK(msSince(t0) < 1500);
        CHECK(!o.src.isOpen());
    }

    // U10: control calls during the re-open return at once; a frequency set during the gap is the one the radio comes back to, by the bytes a plain set would send.
    {
        auto pool = std::make_shared<Pool>();
        auto slowNew = goodFake();
        slowNew->refuseControlOut = [](const cascade::usb::FakeControl&) {   // (every write takes 4 ms: the bring-up lasts long enough to call into the radio DURING it)
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            return false;
        };
        pool->fakes.push_back(std::move(slowNew));
        pool->absentFirst = 5;
        Opened o;
        openOn(o, goodFake());
        o.live->bulkQueue.push_back(payload(2048));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(5000, 100);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        kill(*o.live);
        CHECK(waitFor([&] { return pool->calls.load() >= 1; }, 4000));
        long long worst = 0;
        auto timed = [&](const std::function<void()>& f) {
            const auto t0 = Clock::now();
            f();
            worst = std::max(worst, msSince(t0));
        };
        bool ok1 = false, ok2 = false;
        double g = -1.0;
        timed([&] { ok1 = o.src.setCenterFrequencyHz(98000000.0); });
        timed([&] { ok2 = o.src.setGainDb("TUNER", 15.0); });
        timed([&] { g = o.src.gainDb("TUNER"); });
        timed([&] { (void)o.src.autoGain(); });
        timed([&] { (void)o.src.tunerName(); });
        // ...and DURING the bring-up, when the reader itself holds the device lock: the answer is still at once, not a wait for the lock
        CHECK(waitFor([&] { return !pool->handed.empty(); }, 4000));
        for (int i = 0; i < 6; ++i) {
            timed([&] { g = o.src.gainDb("TUNER"); });
            timed([&] { (void)o.src.autoGain(); });
            timed([&] { (void)o.src.directSamplingMode(); });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::printf("  controls during the re-open: slowest %lld ms, set ok %d %d, gain reads %.1f\n", worst, ok1 ? 1 : 0, ok2 ? 1 : 0, g);
        CHECK(worst < 50);
        CHECK(ok1 && ok2);
        CHECK(g > 14.9 && g < 15.1);
        CHECK(o.src.centerFrequencyHz() == 98000000.0);
        CHECK(drain(o.src, 2048, 6000) >= 2048);
        CHECK(!o.src.faulted());
        FakeUsbDevice* fresh = pool->handed.empty() ? nullptr : pool->handed[0];
        CHECK(fresh != nullptr);
        if (fresh != nullptr) {
            RtlSdrSource ref;
            auto rf = std::make_unique<FakeUsbDevice>();
            dressAsR820T(*rf);
            FakeUsbDevice* refLive = rf.get();
            CHECK(ref.openWithTransport(std::move(rf), "fake R820T dongle"));
            CHECK(ref.setGainDb("TUNER", 15.0));
            CHECK(ref.setCenterFrequencyHz(98000000.0));
            CHECK(ref.start());
            const std::vector<std::string> want = writeTexts(*refLive);
            const std::vector<std::string> have = writeTexts(*fresh);
            ref.stop();
            CHECK(!want.empty());
            CHECK(have == want);
        }
        o.src.stop();
    }

    // U10b (review A2): a gain written DURING the re-open while automatic gain is on ends automatic gain too. It used to be acknowledged, logged and then dropped: the re-open put automatic gain
    // back (the wanted flag still said on) and skipped the gain (applyWanted does not write a gain under automatic mode). After the re-open the radio is on manual gain and holds the gain written.
    {
        auto pool = std::make_shared<Pool>();
        auto slowNew = goodFake();
        slowNew->refuseControlOut = [](const cascade::usb::FakeControl&) {   // (every write takes 4 ms: the re-open lasts long enough to call into the radio during it)
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            return false;
        };
        pool->fakes.push_back(std::move(slowNew));
        pool->absentFirst = 5;
        Opened o;
        openOn(o, goodFake());
        o.live->bulkQueue.push_back(payload(2048));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(5000, 100);
        CHECK(o.src.setAutoGain(true));
        CHECK(o.src.autoGain());
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        kill(*o.live);
        CHECK(waitFor([&] { return pool->calls.load() >= 1; }, 4000));
        const bool ok = o.src.setGainDb("TUNER", 15.0);   // during the re-open: kept, and put on with the rest
        CHECK(ok);
        CHECK(!o.src.autoGain());                          // the answer during the re-open is already manual
        CHECK(drain(o.src, 2048, 6000) >= 2048);
        CHECK(!o.src.faulted());
        std::printf("  gain written during the re-open under automatic gain: auto after %d, gain %.1f\n", o.src.autoGain() ? 1 : 0, o.src.gainDb("TUNER"));
        CHECK(!o.src.autoGain());                          // and after it: the radio is on manual gain...
        {   // ...holding the gain that was written: what a plain write of 15 dB on a radio that was on automatic gain reads back (the gain steps are coarser than 0.1 dB)
            RtlSdrSource ref;
            auto rf = std::make_unique<FakeUsbDevice>();
            dressAsR820T(*rf);
            CHECK(ref.openWithTransport(std::move(rf), "fake R820T dongle"));
            CHECK(ref.setAutoGain(true));
            CHECK(ref.setGainDb("TUNER", 15.0));
            CHECK(!ref.autoGain());
            const double want = ref.gainDb("TUNER");
            CHECK(want > 14.0 && want < 16.0);
            CHECK(std::fabs(o.src.gainDb("TUNER") - want) < 0.05);
        }
        o.src.stop();
    }

    // U11: the bias tee. On before; on again after the first re-open; OFF after the second in the same window, with the readout saying so.
    {
        auto pool = std::make_shared<Pool>();
        auto log1 = std::make_shared<std::vector<std::string>>();
        auto log2 = std::make_shared<std::vector<std::string>>();
        {
            auto f1 = std::make_unique<FakeUsbDevice>();           // the first new device delivers a buffer and then dies
            dressAsR820T(*f1);
            f1->onBeginBulk = [](FakeUsbDevice& d) { if (d.streamStarts == 1) { d.bulkQueue.push_back(payload(2048)); } };
            f1->failBulkAfter = 1;
            f1->failBulkAfterText = "a bulk read failed (Windows error 31)";
            // each new device's writes are kept by their own log, because the source frees the first device when it reopens the second
            f1->onControlOut = [log1](const cascade::usb::FakeControl& c) { log1->push_back(c.text()); };
            pool->fakes.push_back(std::move(f1));
            auto f2 = goodFake();
            f2->onControlOut = [log2](const cascade::usb::FakeControl& c) { log2->push_back(c.text()); };
            pool->fakes.push_back(std::move(f2));
        }
        Opened o;
        openOn(o, goodFake());
        o.live->bulkQueue.push_back(payload(2048));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(3000, 20);
        CHECK(o.src.setBiasT(true));
        CHECK(o.src.biasT());
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        kill(*o.live);
        std::vector<std::complex<float>> buf(4096);
        CHECK(waitFor([&] { o.src.read(buf.data(), buf.size()); return pool->calls.load() >= 1; }, 6000));
        CHECK(waitFor([&] { o.src.read(buf.data(), buf.size()); return pool->calls.load() >= 2 && o.src.reopenCount() >= 2; }, 8000));
        CHECK(drain(o.src, 2048, 6000) >= 2048);
        // What each new device was told, against a reference with the bias tee set and one without: the first got the switch put back, the second did not.
        auto referenceWrites = [](bool bias) {
            RtlSdrSource ref;
            auto rf = std::make_unique<FakeUsbDevice>();
            dressAsR820T(*rf);
            FakeUsbDevice* refLive = rf.get();
            CHECK(ref.openWithTransport(std::move(rf), "fake R820T dongle"));
            if (bias) { CHECK(ref.setBiasT(true)); }
            CHECK(ref.start());
            std::vector<std::string> w = writeTexts(*refLive);
            ref.stop();
            return w;
        };
        CHECK(pool->handed.size() == 2);
        if (pool->handed.size() == 2) {
            const std::vector<std::string> withBias = referenceWrites(true);
            const std::vector<std::string> noBias = referenceWrites(false);
            CHECK(withBias.size() > noBias.size());
            std::printf("  writes: first new device %zu, with-bias reference %zu; second %zu, no-bias reference %zu\n", log1->size(), withBias.size(), log2->size(), noBias.size());
            for (std::size_t i = 0; i < log1->size() && i < withBias.size(); ++i) {
                if ((*log1)[i] != withBias[i]) { std::printf("  first device differs at %zu: %s vs %s\n", i, (*log1)[i].c_str(), withBias[i].c_str()); break; }
            }
            // (a device that fails again afterwards is sent the stream restarts' writes after the stream start, so what is held to the reference is the prefix: the
            // bring-up, the settings and the first stream start)
            auto startsWith = [](const std::vector<std::string>& have, const std::vector<std::string>& want) {
                return have.size() >= want.size() && std::equal(want.begin(), want.end(), have.begin());
            };
            CHECK(startsWith(*log1, withBias));   // the first re-open puts the bias tee back
            CHECK(startsWith(*log2, noBias));     // the second, in the same window, leaves it off
        }
        std::printf("  bias tee: after the second re-open %s, re-opens %d\n", o.src.biasT() ? "ON" : "OFF", o.src.reopenCount());
        CHECK(!o.src.biasT());
        CHECK(!o.src.faulted());
        o.src.stop();
    }

    // U12 (defect D1): a restart that loses the race for the device lock to a control call is a retry. A control call takes the lock at the moment the read fails and
    // keeps it for 900 ms; the radio must not be condemned and must stream afterwards.
    {
        Opened o;
        auto f0 = goodFake();
        f0->bulkQueue.push_back(payload(2048));
        openOn(o, std::move(f0));
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        o.src.stop();                           // (the hooks are installed on a quiet fake: the reader is not inside it while they are written)
        std::thread setter;
        std::atomic<bool> inside{false};
        std::atomic<bool> armed{false};
        std::atomic<bool> fired{false};
        o.live->refuseControlOut = [&](const cascade::usb::FakeControl&) {
            if (armed.exchange(false)) {
                inside = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(900));
            }
            return false;
        };
        o.live->onBulkFail = [&] {
            // on the reader's thread, holding the device lock: start the control call that will take it next, and give it time to be waiting
            if (fired.exchange(true)) { return; }
            armed = true;
            setter = std::thread([&] { o.src.setGainDb("TUNER", 12.0); });
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
        };
        o.live->failBulkReadsAt = {o.live->bulkReads + 20};   // read number 20 from here fails once; the ones before it time out (5 ms each)
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        CHECK(waitFor([&] { return fired.load(); }, 5000));        // the read fails, and the control call takes the lock behind it
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));   // (its 900 ms, and the restart's 750 ms wait for the lock, run out)
        if (setter.joinable()) { setter.join(); }
        const std::size_t got = drain(o.src, 4096, 8000);
        std::printf("  lock lost to a control call: %zu samples, faulted %d, control call entered %d\n", got, o.src.faulted() ? 1 : 0, inside.load() ? 1 : 0);
        CHECK(inside.load());
        CHECK(!o.src.faulted());
        CHECK(!o.src.deviceDead());
        CHECK(got >= 4096);
        o.src.stop();
    }

    // U13: the outcome words. Nothing is raised until samples flow; streamback after a plain restart; streamfail when the restarts did not hold.
    {
        // (a) a re-open to a device that is silent: nothing raised yet
        health::clearRaisedRecoveredForTest();
        auto pool = std::make_shared<Pool>();
        auto silent = std::make_unique<FakeUsbDevice>();
        dressAsR820T(*silent);
        pool->fakes.push_back(std::move(silent));
        Opened o;
        openOn(o, goodFake());
        o.live->bulkQueue.push_back(payload(2048));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(3000, 20);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        kill(*o.live);
        CHECK(waitFor([&] { return pool->calls.load() >= 1; }, 4000));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::printf("  before samples flow: words raised 0x%llx\n", static_cast<unsigned long long>(health::raisedRecoveredForTest()));
        CHECK(!raised(health::Recovered::UsbReopen));
        CHECK(!raised(health::Recovered::UsbReopenFail));
        CHECK(!raised(health::Recovered::StreamBack));
        CHECK(!raised(health::Recovered::StreamFail));
        o.src.stop();
    }
    {
        // (b) a plain restart that works: streamback, not usbreopen
        health::clearRaisedRecoveredForTest();
        Opened o;
        auto f0 = goodFake();
        openOn(o, std::move(f0));
        o.live->failBulkReadsAt = {1};
        CHECK(o.src.start());
        CHECK(drain(o.src, 4096, 4000) >= 4096);
        CHECK(waitFor([] { return raised(health::Recovered::StreamBack); }, 2000));
        CHECK(!raised(health::Recovered::UsbReopen));
        CHECK(!raised(health::Recovered::StreamFail));
        CHECK(!raised(health::Recovered::StreamRestart));
        o.src.stop();
    }
    {
        // (c) restarts that do not hold, no re-open possible (the tests' door): streamfail
        health::clearRaisedRecoveredForTest();
        Opened o;
        auto f0 = std::make_unique<FakeUsbDevice>();
        dressAsR820T(*f0);
        f0->failBulkAfter = 0;
        f0->failBulkAfterText = "a bulk read failed (Windows error 31)";
        openOn(o, std::move(f0));
        CHECK(o.src.start());
        CHECK(waitFor([&] { return o.src.faulted(); }, 5000));
        CHECK(raised(health::Recovered::StreamFail));
        CHECK(!raised(health::Recovered::StreamBack));
        CHECK(!raised(health::Recovered::UsbReopenFail));
        o.src.stop();
    }
    {
        // (d) an unplug (the device never came back): neither failure word, only the radio's close
        health::clearRaisedRecoveredForTest();
        auto pool = std::make_shared<Pool>();
        pool->absentFirst = 1000000;
        Opened o;
        openOn(o, goodFake());
        o.live->bulkQueue.push_back(payload(2048));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(200, 50);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        kill(*o.live);
        CHECK(waitFor([&] { return o.src.faulted(); }, 4000));
        CHECK(!raised(health::Recovered::UsbReopenFail));
        CHECK(!raised(health::Recovered::StreamFail));
        o.src.stop();
    }

    // U14: Windows error 995 (the I/O was aborted) is restarted like 31.
    {
        Opened o;
        auto f0 = goodFake();
        openOn(o, std::move(f0));
        o.live->failBulkText = "a bulk read failed (Windows error 995)";
        o.live->failBulkReadsAt = {1};
        const int before = o.live->streamStarts;
        CHECK(o.src.start());
        const std::size_t got = drain(o.src, 4096, 4000);
        std::printf("  error 995: %zu samples, stream starts %d -> %d\n", got, before, o.live->streamStarts);
        CHECK(got >= 4096);
        CHECK(!o.src.faulted());
        CHECK(o.live->streamStarts >= before + 2);
        o.src.stop();
    }

    // U15: the issue sentences are neutral. The word that stands for every native radio does not name the Airspy; no sentence names a family it is not limited to.
    {
        using cascade::core::issue::issueSentence;
        const std::string restart = issueSentence(health::Recovered::StreamRestart);
        std::printf("  streamrestart sentence: %s\n", restart.c_str());
        CHECK(restart.find("Airspy") == std::string::npos);
        for (std::size_t i = 0; i < health::kRecoveredCount; ++i) {
            const std::string s = issueSentence(static_cast<health::Recovered>(i));
            const std::string w = health::recoveredWord(static_cast<health::Recovered>(i));
            if (s.find("Airspy") != std::string::npos && w != "ratesdefault") { std::printf("FAIL the sentence of %s names the Airspy: %s\n", w.c_str(), s.c_str()); }
            if (w != "ratesdefault") { CHECK(s.find("Airspy") == std::string::npos); }
        }
        CHECK(std::string(issueSentence(health::Recovered::StreamBack)).find("flowed again") != std::string::npos);
        CHECK(std::string(issueSentence(health::Recovered::UsbReopenFail)).find("did not bring it back") != std::string::npos);
    }

    // U16 (decision F2): a re-open that loses the race for the device lock after a FAILED re-arm is tried again, never left with the stream down. A control call takes
    // the lock behind the re-arm and keeps it for `holdMs`; the re-arm's control write is refused at once (the handle is dead), so the reader goes for the re-open, whose
    // step A waits 750 ms for the lock. (The first version `continue`d, saw "not streaming" at the top of the loop and ended the reader with no fault: 20 s later the
    // wrong sentence came from read().)
    //   2000 ms: the second try gets the lock, the radio comes back;
    //   4200 ms: three losses, then the radio is called dead with the sentence that says the driver was busy, and `usbreopenfail` is filed.
    for (const int holdMs : {2000, 4200}) {
        health::clearRaisedRecoveredForTest();
        auto pool = std::make_shared<Pool>();
        pool->fakes.push_back(goodFake());
        Opened o;
        auto f0 = goodFake();
        f0->bulkQueue.push_back(payload(2048));
        auto refuse = std::make_shared<std::atomic<bool>>(false);
        auto readerId = std::make_shared<std::atomic<std::thread::id>>(std::this_thread::get_id());
        auto slowOnce = std::make_shared<std::atomic<bool>>(false);
        auto setterInside = std::make_shared<std::atomic<bool>>(false);
        auto spawned = std::make_shared<std::atomic<bool>>(false);
        auto setter = std::make_shared<std::thread>();
        auto taken = std::make_shared<std::atomic<bool>>(false);
        RtlSdrSource* srcPtr = &o.src;
        f0->refuseControlOut = [refuse, readerId, slowOnce, setterInside, holdMs](const cascade::usb::FakeControl&) {
            if (!refuse->load()) { return false; }
            if (std::this_thread::get_id() != readerId->load() && !slowOnce->exchange(true)) {
                setterInside->store(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
            }
            return true;
        };
        f0->onEndBulk = [refuse, readerId, taken, setter, spawned, srcPtr] {
            if (!refuse->load() || taken->exchange(true)) { return; }
            // the re-arm's own endBulkStream, on the reader's thread, holding the device lock: a control call queues behind it
            readerId->store(std::this_thread::get_id());
            *setter = std::thread([srcPtr] { srcPtr->setGainDb("TUNER", 12.0); });
            spawned->store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
        };
        openOn(o, std::move(f0));
        // THE ORDER IS MADE CERTAIN: the lock is a plain timed mutex with no fairness, and a reader that unlocks and locks again at once can take it back before the control
        // call that was waiting wakes. The one place the reader goes between the failed re-arm and the re-open that is not under the lock - the diagnostic line's "is it
        // still listed" question - waits until the control call is inside the device, so the re-open meets a held lock, as it would in the field.
        o.src.setReopenForTest(seamOf(pool), [spawned, setterInside]() -> int {
            if (spawned->load()) {
                const auto until = Clock::now() + std::chrono::milliseconds(3000);
                while (!setterInside->load() && Clock::now() < until) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
            }
            return -1;
        });
        o.src.setReopenBoundsForTest(3000, 20);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        refuse->store(true);
        o.live->failBulkAfterText = "a bulk read failed (Windows error 31)";
        o.live->failBulkAfter = o.live->bulkReads;
        const auto t0 = Clock::now();
        if (holdMs == 2000) {
            CHECK(waitFor([] { return raised(health::Recovered::UsbReopen); }, 9000));   // (raised only once samples flow again)
            const std::size_t got = drain(o.src, 2048, 4000);
            std::printf("  lock lost after a failed re-arm (held %d ms): %zu samples, %lld ms, seam calls %d, faulted %d\n", holdMs, got, msSince(t0), pool->calls.load(), o.src.faulted() ? 1 : 0);
            CHECK(got >= 2048);
            CHECK(!o.src.faulted());
            CHECK(pool->calls.load() == 1);
            CHECK(o.src.reopenCount() == 1);
        } else {
            CHECK(waitFor([&] { return o.src.faulted(); }, 9000));
            const std::string said = o.src.lastError();
            std::printf("  lock lost three times (held %d ms): %lld ms, seam calls %d: %s\n", holdMs, msSince(t0), pool->calls.load(), said.c_str());
            CHECK(said == cascade::source::kReopenBusySentence);
            CHECK(said.find("the driver was busy") != std::string::npos);
            CHECK(cascade::core::radioreport::closeCauseFor(true, said) == "fault");
            CHECK(pool->calls.load() == 0);   // the device was never touched
            CHECK(waitFor([] { return raised(health::Recovered::UsbReopenFail); }, 2000));
            CHECK(!raised(health::Recovered::StreamFail));
        }
        o.src.stop();
        if (setter->joinable()) { setter->join(); }
    }

    // U17 (decision F3a): a dongle whose EEPROM forces the bias tee. The first re-open puts the tee back where the user's wish had it (ON, by the settings' own write: the
    // bring-up does not switch it on by itself); the SECOND in the same window withholds it, and NOTHING is ever written ON to that device - the readback, `wanted` and the
    // wire all say OFF, and one log line says why.
    {
        auto wireOld = std::make_shared<Wire>();
        auto wire1 = std::make_shared<Wire>();
        auto wire2 = std::make_shared<Wire>();
        auto pool = std::make_shared<Pool>();
        {
            auto d1 = dongle(wire1, true, true);          // the first new device delivers one buffer and dies
            d1->onBeginBulk = [](FakeUsbDevice& d) { if (d.streamStarts == 1) { d.bulkQueue.push_back(payload(2048)); } };
            d1->failBulkAfter = 1;
            d1->failBulkAfterText = "a bulk read failed (Windows error 31)";
            pool->fakes.push_back(std::move(d1));
            pool->fakes.push_back(dongle(wire2, true, true));
        }
        Opened o;
        auto f0 = dongle(wireOld, true, true);
        f0->bulkQueue.push_back(payload(2048));
        openOn(o, std::move(f0));
        CHECK(o.src.eepromValid());
        CHECK(o.src.biasTForcedByEeprom());
        CHECK(o.src.biasT());                              // the open switched the forced tee on
        CHECK(wireOld->lastOn());
        cascade::gui::BiasTeePanel panel;
        cascade::gui::biasTeeAfterOpen(panel, o.src, "serial=00000001");
        CHECK(panel.present);
        CHECK(panel.shown);
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(3000, 20);
        const int withheldBefore = logCount("Its bias tee was left off because the radio dropped out twice");
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        kill(*o.live);
        std::vector<std::complex<float>> buf(4096);
        CHECK(waitFor([&] { o.src.read(buf.data(), buf.size()); return pool->calls.load() >= 2 && o.src.reopenCount() >= 2; }, 9000));
        CHECK(drain(o.src, 2048, 6000) >= 2048);
        std::printf("  forced tee: first new device wrote GPO %zu times, ends %s; second %zu times, ever on %d, ends %s; readback %s\n", wire1->writes(), wire1->lastOn() ? "ON" : "OFF", wire2->writes(),
                    wire2->everOn() ? 1 : 0, wire2->lastOn() ? "ON" : "OFF", o.src.biasT() ? "ON" : "OFF");
        CHECK(wire1->writes() >= 2);
        CHECK(wire1->lastOn());                            // first re-open: back ON, by the settings' own write
        CHECK(!wire2->everOn());                           // second: never switched on at all
        // THE BOX (decision F3c): the panel's remembered state is stale after the withhold, and following the radio's readback puts it right
        CHECK(panel.shown);
        cascade::gui::biasTeeFollowRadio(panel, &o.src);
        CHECK(!panel.shown);
        CHECK(!wire2->lastOn());
        CHECK(!o.src.biasT());                             // the readback says what is on the wire
        CHECK(logCount("Its bias tee was left off because the radio dropped out twice") == withheldBefore + 1);   // one line
        CHECK(!o.src.faulted());
        o.src.stop();
    }

    // U18 (decision F3b): a bias-tee tick made DURING a re-open is what the readback answers from, and what the re-open applies.
    //   on : tee OFF, the user ticks ON in the gap: biasT() says ON at once, and the new device is switched ON;
    //   off: tee ON, the user unticks in the gap: biasT() says OFF at once, and the new device is never switched ON.
    for (const bool tickOn : {true, false}) {
        auto wireOld = std::make_shared<Wire>();
        auto wireNew = std::make_shared<Wire>();
        auto pool = std::make_shared<Pool>();
        pool->absentFirst = 5;
        pool->fakes.push_back(dongle(wireNew, false, false));
        Opened o;
        auto f0 = dongle(wireOld, false, false);
        f0->bulkQueue.push_back(payload(2048));
        openOn(o, std::move(f0));
        if (!tickOn) { CHECK(o.src.setBiasT(true)); }
        CHECK(o.src.biasT() == !tickOn);
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(5000, 100);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        kill(*o.live);
        CHECK(waitFor([&] { return pool->calls.load() >= 1; }, 4000));
        CHECK(o.src.reopeningForTest());
        CHECK(o.src.setBiasT(tickOn));                     // acknowledged during the gap
        const bool during = o.src.biasT();
        CHECK(drain(o.src, 2048, 6000) >= 2048);
        std::printf("  tick %s during the gap: readback during %s, after %s, new device ever on %d, ends %s\n", tickOn ? "ON" : "OFF", during ? "ON" : "OFF", o.src.biasT() ? "ON" : "OFF",
                    wireNew->everOn() ? 1 : 0, wireNew->lastOn() ? "ON" : "OFF");
        CHECK(during == tickOn);                           // the panel shows what the re-open will do
        CHECK(o.src.biasT() == tickOn);
        CHECK(wireNew->lastOn() == tickOn);
        if (!tickOn) { CHECK(!wireNew->everOn()); }
        CHECK(!o.src.faulted());
        o.src.stop();
    }

    // U19 (decision F3b, the race): a tick OFF that arrives at the instant the re-open writes the tee ON is applied by another round; the wire ends OFF and the readback agrees
    // (the first version applied a copy of the user's settings taken before the tick, and left 4.5 V on under an unticked box).
    {
        auto wireOld = std::make_shared<Wire>();
        auto wireNew = std::make_shared<Wire>();
        auto pool = std::make_shared<Pool>();
        pool->absentFirst = 2;
        auto d1 = dongle(wireNew, false, false);
        RtlSdrSource* srcPtr = nullptr;
        Opened o;
        srcPtr = &o.src;
        const auto inner = d1->onControlOut;
        auto fired = std::make_shared<std::atomic<bool>>(false);
        d1->onControlOut = [inner, srcPtr, fired](const cascade::usb::FakeControl& c) {
            if (inner) { inner(c); }
            // the re-open's own write that switches the tee ON (GPO, bit 0 set): the user unticks at this very instant, from the reader's own thread (the setter hands
            // its change to the re-open, which is what it does for any other thread)
            if (c.out && c.request == 0 && c.index == kSysWrite && c.value == kGpo && !c.data.empty() && (c.data[0] & 0x01) != 0 && srcPtr->reopeningForTest() && !fired->exchange(true)) { srcPtr->setBiasT(false); }
        };
        pool->fakes.push_back(std::move(d1));
        auto f0 = dongle(wireOld, false, false);
        f0->bulkQueue.push_back(payload(2048));
        openOn(o, std::move(f0));
        CHECK(o.src.setBiasT(true));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(5000, 50);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        kill(*o.live);
        CHECK(drain(o.src, 2048, 6000) >= 2048);
        std::printf("  tick OFF at the instant of the ON write: fired %d, new device ends %s, readback %s\n", fired->load() ? 1 : 0, wireNew->lastOn() ? "ON" : "OFF", o.src.biasT() ? "ON" : "OFF");
        CHECK(fired->load());
        CHECK(!wireNew->lastOn());
        CHECK(!o.src.biasT());
        o.src.stop();
    }

    // U20 (decision F5): a setter that arrives AFTER the re-open's last look at what the user asked for still reaches the chip. It arrives while the stream is started
    // (the new device's beginBulkStream), from another thread: the re-open has already come down, the setter waits for the device lock and then tunes the radio in the
    // ordinary way. The new device's writes are the reference's: the same settings, the stream start, and then the tune.
    {
        auto pool = std::make_shared<Pool>();
        auto d1 = goodFake();
        auto late = std::make_shared<std::thread>();
        auto lateStarted = std::make_shared<std::atomic<bool>>(false);
        Opened o;
        RtlSdrSource* srcPtr = &o.src;
        const auto counted = d1->onBeginBulk;
        d1->onBeginBulk = [counted, late, lateStarted, srcPtr](FakeUsbDevice& d) {
            if (counted) { counted(d); }
            if (!lateStarted->load()) {
                *late = std::thread([srcPtr] { srcPtr->setCenterFrequencyHz(98000000.0); });
                lateStarted->store(true);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));   // let it get to the lock
        };
        auto log1 = std::make_shared<std::vector<std::string>>();
        auto log1m = std::make_shared<std::mutex>();
        d1->onControlOut = [log1, log1m](const cascade::usb::FakeControl& c) {
            std::lock_guard<std::mutex> lk(*log1m);
            log1->push_back(c.text());
        };
        pool->fakes.push_back(std::move(d1));
        auto f0 = goodFake();
        f0->bulkQueue.push_back(payload(2048));
        openOn(o, std::move(f0));
        CHECK(o.src.setCenterFrequencyHz(433920000.0));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(3000, 20);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        kill(*o.live);
        CHECK(waitFor([&] { return lateStarted->load(); }, 6000));
        late->join();
        CHECK(drain(o.src, 1024, 4000) >= 1024);
        std::printf("  setter after the last look: centre %.0f Hz, re-opens %d\n", o.src.centerFrequencyHz(), o.src.reopenCount());
        CHECK(o.src.centerFrequencyHz() == 98000000.0);
        std::vector<std::string> have;
        {
            std::lock_guard<std::mutex> lk(*log1m);
            have = *log1;
        }
        RtlSdrSource ref;
        auto rf = std::make_unique<FakeUsbDevice>();
        dressAsR820T(*rf);
        FakeUsbDevice* refLive = rf.get();
        CHECK(ref.openWithTransport(std::move(rf), "fake R820T dongle"));
        CHECK(ref.setCenterFrequencyHz(433920000.0));
        CHECK(ref.start());
        CHECK(ref.setCenterFrequencyHz(98000000.0));
        const std::vector<std::string> want = writeTexts(*refLive);
        ref.stop();
        CHECK(!want.empty());
        CHECK(have.size() >= want.size());
        for (std::size_t i = 0; i < want.size() && i < have.size(); ++i) {
            if (want[i] != have[i]) { std::printf("  FIRST DIFFERENCE at write %zu: want %s, have %s\n", i, want[i].c_str(), have[i].c_str()); }
            CHECK(want[i] == have[i]);
            if (want[i] != have[i]) { break; }
        }
        o.src.stop();
    }

    // U21 (decision F5, the bound): setters that keep arriving in every round are served for three rounds, then accepted - one log line says so, the re-open still completes
    // and the radio streams. (A bound that was not there would loop for as long as the user kept dragging.)
    {
        auto pool = std::make_shared<Pool>();
        auto d1 = goodFake();
        Opened o;
        RtlSdrSource* srcPtr = &o.src;
        auto fires = std::make_shared<std::atomic<int>>(0);
        auto writes = std::make_shared<std::atomic<int>>(0);
        d1->onControlOut = [srcPtr, fires, writes](const cascade::usb::FakeControl&) {
            const int n = writes->fetch_add(1) + 1;
            // after the bring-up (about 140 writes), every fifth write: a tune arrives; only while the re-open is taking settings (the same thread: no race)
            if (n > 150 && n % 5 == 0 && srcPtr->reopeningForTest() && fires->load() < 40) { srcPtr->setCenterFrequencyHz(90000000.0 + 1000.0 * fires->fetch_add(1)); }
        };
        pool->fakes.push_back(std::move(d1));
        auto f0 = goodFake();
        f0->bulkQueue.push_back(payload(2048));
        openOn(o, std::move(f0));
        CHECK(o.src.setCenterFrequencyHz(433920000.0));   // settings to put back, so every round writes
        CHECK(o.src.setGainDb("TUNER", 20.0));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(3000, 20);
        const int saidBefore = logCount("a setting arrived while the radio was being put back");
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        kill(*o.live);
        const auto t0 = Clock::now();
        CHECK(drain(o.src, 2048, 8000) >= 2048);
        std::printf("  setters in every round: %d arrived, completed in %lld ms, faulted %d\n", fires->load(), msSince(t0), o.src.faulted() ? 1 : 0);
        CHECK(fires->load() >= 3 && fires->load() <= 30);   // three rounds' worth, not the forty the hook allows
        CHECK(logCount("a setting arrived while the radio was being put back") == saidBefore + 1);
        CHECK(!o.src.faulted());
        o.src.stop();
    }

    // U22 (decision F4): the new device is not the same radio. A different tuner (an R828D where an R820T left) is closed again and the radio is called dead with the sentence
    // that says which; the user's settings, bias tee included, are NOT put onto it.
    {
        auto wireNew = std::make_shared<Wire>();
        auto wireOld = std::make_shared<Wire>();
        health::clearRaisedRecoveredForTest();
        auto pool = std::make_shared<Pool>();
        auto newCount = std::make_shared<Count>();
        auto other = dongle(wireNew, false, false);
        other->inAnswers.erase(FakeUsbDevice::InKey{0, 0x0034, 0x0600});
        other->answerIn(0, 0x0074, 0x0600, kTunerAnswer);   // an R828D answers at 0x74
        instrument(*other, newCount);
        pool->fakes.push_back(std::move(other));
        Opened o;
        auto f0 = dongle(wireOld, false, false);
        f0->bulkQueue.push_back(payload(2048));
        openOn(o, std::move(f0));
        CHECK(o.src.setBiasT(true));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(3000, 20);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        kill(*o.live);
        CHECK(waitFor([&] { return o.src.faulted(); }, 6000));
        const std::string said = o.src.lastError();
        std::printf("  a different tuner came back: %s (tee ever on %d, streams started %d, closed %d)\n", said.c_str(), wireNew->everOn() ? 1 : 0, newCount->begins.load(), closedIn(*newCount) ? 1 : 0);
        CHECK(said == cascade::source::kReopenWhichSentence);
        CHECK(cascade::core::radioreport::closeCauseFor(true, said) == "fault");
        CHECK(!wireNew->everOn());
        CHECK(newCount->begins.load() == 0);
        CHECK(waitFor([&] { return closedIn(*newCount); }, 2000));
        CHECK(waitFor([] { return raised(health::Recovered::UsbReopenFail); }, 2000));
        o.src.stop();
    }

    // U23 (decision F4): the finder says more than one radio could be this one and none is provably it. The wait keeps looking until its end (the right one may show up), then
    // the radio is called dead with the sentence that says so; nothing is opened.
    {
        auto pool = std::make_shared<Pool>();
        pool->ambiguous = [] { return std::string(cascade::source::kRadioAmbiguousError); };
        pool->fakes.push_back(goodFake());
        Opened o;
        auto f0 = goodFake();
        f0->bulkQueue.push_back(payload(2048));
        openOn(o, std::move(f0));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(400, 50);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        const auto t0 = Clock::now();
        kill(*o.live);
        CHECK(waitFor([&] { return o.src.faulted(); }, 4000));
        const long long took = msSince(t0);
        const std::string said = o.src.lastError();
        std::printf("  ambiguous: %lld ms, %d seam calls: %s\n", took, pool->calls.load(), said.c_str());
        CHECK(said == cascade::source::kReopenWhichSentence);
        CHECK(took >= 350 && took < 2500);
        CHECK(pool->calls.load() >= 2);
        CHECK(pool->handed.empty());
        CHECK(cascade::core::radioreport::closeCauseFor(true, said) == "fault");
        o.src.stop();
    }

    // U24 (decision F8): the bring-up is bounded as a whole. The new device answers every transfer in 30 ms; with a 500 ms bound the re-open fails with the step named, long
    // before the bring-up's hundred-odd transfers could finish, and the radio is called dead.
    {
        health::clearRaisedRecoveredForTest();
        auto pool = std::make_shared<Pool>();
        auto slow = goodFake();
        slow->refuseControlOut = [](const cascade::usb::FakeControl&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            return false;
        };
        pool->fakes.push_back(std::move(slow));
        Opened o;
        auto f0 = goodFake();
        f0->bulkQueue.push_back(payload(2048));
        openOn(o, std::move(f0));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(3000, 20, 60000, 500);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        const auto t0 = Clock::now();
        kill(*o.live);
        CHECK(waitFor([&] { return o.src.faulted(); }, 8000));
        const long long took = msSince(t0);
        const std::string said = o.src.lastError();
        std::printf("  bring-up bound: %lld ms: %s\n", took, said.c_str());
        CHECK(said.find("the chip bring-up did not finish in time") != std::string::npos);
        CHECK(took < 3000);
        CHECK(cascade::core::radioreport::closeCauseFor(true, said) == "fault");
        CHECK(waitFor([] { return raised(health::Recovered::UsbReopenFail); }, 2000));
        o.src.stop();
    }

    // U25 (decision F8): the reader's liveness beat is kept alive through a long bring-up. The new device is slow (a bring-up of about a second) and the stuck-reader bound is
    // 300 ms: read(), on the pipeline's own thread, must not call the radio stuck while the re-open runs, and the radio must come back.
    {
        auto pool = std::make_shared<Pool>();
        auto slow = goodFake();
        slow->refuseControlOut = [](const cascade::usb::FakeControl&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            return false;
        };
        pool->fakes.push_back(std::move(slow));
        pool->absentFirst = 3;
        Opened o;
        auto f0 = goodFake();
        f0->bulkQueue.push_back(payload(2048));
        openOn(o, std::move(f0));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(3000, 100);
        o.src.setStallBoundsForTest(3000, 300);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        std::vector<std::complex<float>> buf(4096);
        while (o.src.read(buf.data(), buf.size()) > 0) {}   // what is left of the old stream, so that what is counted below is the new one's
        kill(*o.live);
        bool sawStuck = false;
        const auto t0 = Clock::now();
        std::size_t got = 0;
        while (msSince(t0) < 6000 && got < 2048) {
            got += o.src.read(buf.data(), buf.size());
            if (o.src.faulted()) { sawStuck = true; break; }
        }
        std::printf("  beat through a long re-open: %zu samples in %lld ms, faulted %d (%s)\n", got, msSince(t0), o.src.faulted() ? 1 : 0, o.src.lastError());
        CHECK(!sawStuck);
        CHECK(got >= 2048);
        CHECK(o.src.reopenCount() == 1);
        o.src.stop();
    }

    // U26 (decision F8): a reader abandoned INSIDE a re-open leaves nothing a later destructor or process exit can touch after free. The seam blocks (a CreateFile that does
    // not return) for 1500 ms; stop() abandons the reader after its 1000 ms bound and condemns the radio; the source object is DESTROYED while the reader is still inside the
    // seam; the seam then returns a device. The reader holds the link by shared pointer, sees the stop token before it installs anything, lets the new handle go (it is
    // closed), notes no fault of its own, and ends - without a crash and with the new device's handle closed.
    {
        health::clearRaisedRecoveredForTest();
        auto pool = std::make_shared<Pool>();
        auto newCount = std::make_shared<Count>();
        auto inside = std::make_shared<std::atomic<bool>>(false);
        auto released = std::make_shared<std::atomic<bool>>(false);
        pool->beforeHand = [inside, released] {
            inside->store(true);
            const auto until = Clock::now() + std::chrono::milliseconds(1500);
            while (Clock::now() < until) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
            released->store(true);
        };
        auto fresh = goodFake();
        instrument(*fresh, newCount);
        pool->fakes.push_back(std::move(fresh));
        const int leakBefore = logCount("reader thread did not exit within its bound");
        {
            Opened o;
            auto f0 = goodFake();
            f0->bulkQueue.push_back(payload(2048));
            openOn(o, std::move(f0));
            o.src.setReopenForTest(seamOf(pool));
            o.src.setReopenBoundsForTest(5000, 50);
            CHECK(o.src.start());
            CHECK(drain(o.src, 1024, 3000) >= 1024);
            killGone(*o.live);
            CHECK(waitFor([&] { return inside->load(); }, 4000));
            const auto t0 = Clock::now();
            o.src.stop();
            const long long took = msSince(t0);
            std::printf("  reader abandoned inside the open of the new handle: stop took %lld ms, dead %d, words: %s\n", took, o.src.deviceDead() ? 1 : 0, o.src.lastError());
            CHECK(took >= 900 && took < 1900);
            CHECK(o.src.deviceDead());
            CHECK(raised(health::Recovered::ReaderLeak));
            CHECK(!released->load());            // the reader really is still inside the seam
        }                                        // <- the source object is destroyed here, under the stranded reader
        CHECK(waitFor([&] { return released->load(); }, 4000));
        CHECK(waitFor([&] { return closedIn(*newCount); }, 4000));   // the late handle was let go by the stranded reader
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(newCount->begins.load() == 0);     // nothing was started on it
        CHECK(logCount("reader thread did not exit within its bound") == leakBefore + 1);
        // the process is still sound: a fresh source opens and streams
        RtlSdrSource again;
        auto f1 = goodFake();
        f1->bulkQueue.push_back(payload(2048));
        CHECK(again.openWithTransport(std::move(f1), "fake R820T dongle"));
        CHECK(again.start());
        CHECK(drain(again, 1024, 3000) >= 1024);
        again.stop();
    }

    // U27 (decision S1): the bound of one attempt's bring-up, settings and stream restart is 10000 ms in the product (a refused tuner transfer costs about 200 ms and a bench
    // run logged a 5.0 s bring-up), and the hook still sets a short bound: a source whose bounds were never touched says 10000, and one given the hook's default says the
    // same. (U24 above is the bound at work, with a short one set through the hook.)
    {
        RtlSdrSource fresh;
        CHECK(fresh.reopenBringUpMsForTest() == 10000);
        CHECK(cascade::source::kReopenBringUpMs == 10000);
        Opened o;
        openOn(o, goodFake());
        CHECK(o.src.reopenBringUpMsForTest() == 10000);
        o.src.setReopenBoundsForTest(3000, 20, 60000, 500);
        CHECK(o.src.reopenBringUpMsForTest() == 500);
        o.src.setReopenBoundsForTest(3000, 20);
        CHECK(o.src.reopenBringUpMsForTest() == 10000);
        std::printf("  bring-up bound: %d ms by default\n", o.src.reopenBringUpMsForTest());
    }

    // U28 (decisions S9 and S2): "in use by another program" during the wait is not an instant fault. Two same-serial dongles are listed and OURS (found by its path and
    // its port) is held by another program for the first three polls: the wait goes on, the radio is opened on the fourth, and the stream comes back. Nothing is faulted.
    {
        health::clearRaisedRecoveredForTest();
        cascade::source::RadioIdentity id;
        id.path = "path-ours";
        id.location = "port-1";
        id.serial = "00000001";
        id.serialUnique = false;
        auto mk = [](const char* path, const char* loc) {
            cascade::usb::UsbDeviceInfo d;
            d.path = path;
            d.location = loc;
            d.serial = "00000001";
            return d;
        };
        const std::vector<cascade::usb::UsbDeviceInfo> both = {mk("path-ours", "port-1"), mk("path-other", "port-2")};
        auto polls = std::make_shared<std::atomic<int>>(0);
        auto asked = std::make_shared<std::vector<std::string>>();
        auto askedM = std::make_shared<std::mutex>();
        auto fn = cascade::source::makeRadioReopenFn(
            id, [both] { return both; },
            [polls, asked, askedM](const std::string& path, std::string& error) -> std::unique_ptr<cascade::usb::UsbDevice> {
                {
                    std::lock_guard<std::mutex> lk(*askedM);
                    asked->push_back(path);
                }
                if (polls->fetch_add(1) < 3) {
                    error = cascade::usb::kInUseError;
                    return nullptr;
                }
                return goodFake();
            });
        Opened o;
        auto f0 = goodFake();
        f0->bulkQueue.push_back(payload(2048));
        openOn(o, std::move(f0));
        o.src.setReopenForTest(fn);
        o.src.setReopenBoundsForTest(3000, 40);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        const auto t0 = Clock::now();
        kill(*o.live);
        const std::size_t got = drain(o.src, 4096, 5000);
        std::printf("  in use for three polls, then free: %zu samples, %lld ms, faulted %d, opener asked %d times\n", got, msSince(t0), o.src.faulted() ? 1 : 0, polls->load());
        CHECK(got >= 4096);
        CHECK(!o.src.faulted());
        CHECK(o.src.reopenCount() == 1);
        CHECK(polls->load() == 4);
        {
            std::lock_guard<std::mutex> lk(*askedM);
            for (const std::string& a : *asked) { CHECK(a == "path-ours"); }   // never the other dongle's path
        }
        CHECK(waitFor([] { return raised(health::Recovered::UsbReopen); }, 2000));
        o.src.stop();
    }

    // U29 (decision S3): the fault of a stop that lands BEFORE the re-open's first step, and of a start() that fails on a dead handle.
    // (a) the stream restart is failing (a control write is refused, the handle is dead) and a stop() lands inside it: the reader ends with the radio latched faulted with
    //     the stopped sentence; a following start() is refused with those words; picking the radio again streams. (Before this the reader ended with the dead handle and no
    //     fault, and a later start() failed for good without a word.)
    {
        health::clearRaisedRecoveredForTest();
        auto pool = std::make_shared<Pool>();
        pool->fakes.push_back(goodFake());
        auto inside = std::make_shared<std::atomic<bool>>(false);
        auto armed = std::make_shared<std::atomic<bool>>(false);
        Opened o;
        auto f0 = goodFake();
        f0->bulkQueue.push_back(payload(2048));
        const auto counted = f0->onEndBulk;
        auto taken = std::make_shared<std::atomic<bool>>(false);
        f0->onEndBulk = [inside, armed, taken, counted] {
            if (counted) { counted(); }
            if (armed->load() && !taken->exchange(true)) {
                inside->store(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }
        };
        openOn(o, std::move(f0));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(3000, 20);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        armed->store(true);
        kill(*o.live);                      // not "gone": the reader first tries to restart the stream, whose end-of-stream call is where the stop lands
        CHECK(waitFor([&] { return inside->load(); }, 4000));
        const auto t0 = Clock::now();
        o.src.stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const std::string said = o.src.lastError();
        std::printf("  stop before the re-open's first step: %lld ms, faulted %d, dead %d, seam calls %d: %s\n", msSince(t0), o.src.faulted() ? 1 : 0, o.src.deviceDead() ? 1 : 0, pool->calls.load(), said.c_str());
        CHECK(o.src.faulted());
        CHECK(o.src.deviceDead());
        CHECK(said == cascade::source::kReopenStoppedSentence);
        CHECK(pool->calls.load() == 0);     // the re-open never began: the stop landed before it
        CHECK(!o.src.start());
        CHECK(std::string(o.src.lastError()) == cascade::source::kReopenStoppedSentence);
        RtlSdrSource again;
        auto f1 = goodFake();
        f1->bulkQueue.push_back(payload(2048));
        CHECK(again.openWithTransport(std::move(f1), "fake R820T dongle"));
        CHECK(again.start());
        CHECK(drain(again, 1024, 3000) >= 1024);
        again.stop();
    }
    // (b) a start() that fails on a dead handle latches its fault: the words are the failing step's, the radio reads as faulted and dead (so the window shows them and the
    //     engine's re-pick opens the radio afresh), and picking again streams.
    {
        Opened o;
        openOn(o, goodFake());
        o.live->failControlAfter = o.live->controlCalls;   // the handle is dead: every control write is refused
        CHECK(!o.src.faulted());
        CHECK(!o.src.start());
        const std::string said = o.src.lastError();
        std::printf("  start() on a dead handle: faulted %d, dead %d: %s\n", o.src.faulted() ? 1 : 0, o.src.deviceDead() ? 1 : 0, said.c_str());
        CHECK(o.src.faulted());
        CHECK(o.src.deviceDead());
        CHECK(said.find("would not reset its endpoint") != std::string::npos);
        CHECK(!o.src.start());
        CHECK(std::string(o.src.lastError()) == said);     // the fault keeps its own words
        RtlSdrSource again;
        auto f1 = goodFake();
        f1->bulkQueue.push_back(payload(2048));
        CHECK(again.openWithTransport(std::move(f1), "fake R820T dongle"));
        CHECK(again.start());
        CHECK(drain(again, 1024, 3000) >= 1024);
        again.stop();
    }

    // U30 (decision S4): the Failed, Absent and Mismatch exits look at the stop and abandon state before anything is written. The finder blocks for 1500 ms and then says
    // no (in use / not on the bus / more than one radio could be this one); the wait is 100 ms, so its end has passed when the finder answers. A stop() abandons the
    // reader after its 1000 ms bound; the reader, when it wakes, writes NOTHING: no fault sentence over the abandonment's, no `usbreopenfail`, no log line of a failure.
    // The same three with a stop() that wins inside the bound: the radio is latched with the stopped sentence, not with the exit's.
    for (int variant = 0; variant < 3; ++variant) {
        health::clearRaisedRecoveredForTest();
        auto pool = std::make_shared<Pool>();
        if (variant == 0) { pool->inUse = true; }
        if (variant == 1) { pool->absentFirst = 1000000; }
        if (variant == 2) { pool->ambiguous = [] { return std::string(cascade::source::kRadioAmbiguousError); }; }
        auto inside = std::make_shared<std::atomic<bool>>(false);
        auto released = std::make_shared<std::atomic<bool>>(false);
        pool->beforeAnswer = [inside, released] {
            inside->store(true);
            const auto until = Clock::now() + std::chrono::milliseconds(1500);
            while (Clock::now() < until) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
            released->store(true);
        };
        const int failedBefore = logCount("failed while reading samples");
        Opened o;
        auto f0 = goodFake();
        f0->bulkQueue.push_back(payload(2048));
        openOn(o, std::move(f0));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(100, 50);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        killGone(*o.live);
        CHECK(waitFor([&] { return inside->load(); }, 4000));
        o.src.stop();                                                   // abandons the reader inside the finder
        CHECK(raised(health::Recovered::ReaderLeak));
        CHECK(!released->load());
        CHECK(waitFor([&] { return released->load(); }, 4000));
        std::this_thread::sleep_for(std::chrono::milliseconds(400));    // the reader wakes, sees the deadline passed, and takes its exit
        const std::string said = o.src.lastError();
        std::printf("  abandoned reader, exit %d: words \"%s\", failure lines %d\n", variant, said.c_str(), logCount("failed while reading samples") - failedBefore);
        CHECK(said.find("would not stop") != std::string::npos);        // the abandonment's own sentence is still the one
        CHECK(said.find("did not help") == std::string::npos);
        CHECK(said.find("USB bus") == std::string::npos);
        CHECK(said != cascade::source::kReopenWhichSentence);
        CHECK(!raised(health::Recovered::UsbReopenFail));
        CHECK(logCount("failed while reading samples") == failedBefore);
    }
    for (int variant = 0; variant < 3; ++variant) {
        health::clearRaisedRecoveredForTest();
        auto pool = std::make_shared<Pool>();
        if (variant == 0) { pool->inUse = true; }
        if (variant == 1) { pool->absentFirst = 1000000; }
        if (variant == 2) { pool->ambiguous = [] { return std::string(cascade::source::kRadioAmbiguousError); }; }
        auto inside = std::make_shared<std::atomic<bool>>(false);
        pool->beforeAnswer = [inside] {
            inside->store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        };
        Opened o;
        auto f0 = goodFake();
        f0->bulkQueue.push_back(payload(2048));
        openOn(o, std::move(f0));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(100, 50);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        killGone(*o.live);
        CHECK(waitFor([&] { return inside->load(); }, 4000));
        o.src.stop();                                                   // inside the bound: the reader finishes its exit and is joined
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const std::string said = o.src.lastError();
        std::printf("  stop racing exit %d: %s\n", variant, said.c_str());
        CHECK(said == cascade::source::kReopenStoppedSentence);
        CHECK(o.src.faulted());
        CHECK(!raised(health::Recovered::UsbReopenFail));
    }

    // U31 (decision S5): an empty USB string at the re-open is not a different radio. The open read "Realtek" / "RTL2832U".
    //   (a) the new device answers no string at all (every descriptor read fails): each is read up to three times, 100 ms apart, and the radio comes back;
    //   (b) the new device's strings are non-empty and DIFFERENT: a different radio, the fault says so, nothing is put onto it;
    //   (c) the open had NO strings (an empty read) and the new device has them: nothing contradicts, the radio comes back.
    for (int variant = 0; variant < 3; ++variant) {
        health::clearRaisedRecoveredForTest();
        auto pool = std::make_shared<Pool>();
        auto fresh = goodFake();
        if (variant == 1) { withStrings(*fresh, "Somebody Else", "Another Stick"); }
        if (variant == 2) { withStrings(*fresh, "Realtek", "RTL2832U"); }
        FakeUsbDevice* freshPtr = fresh.get();
        pool->fakes.push_back(std::move(fresh));
        Opened o;
        auto f0 = goodFake();
        if (variant != 2) { withStrings(*f0, "Realtek", "RTL2832U"); }
        f0->bulkQueue.push_back(payload(2048));
        openOn(o, std::move(f0));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(3000, 20);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        const auto t0 = Clock::now();
        kill(*o.live);
        if (variant == 1) {
            CHECK(waitFor([&] { return o.src.faulted(); }, 6000));
            std::printf("  strings differ: %s\n", o.src.lastError());
            CHECK(std::string(o.src.lastError()) == cascade::source::kReopenWhichSentence);
        } else {
            const std::size_t got = drain(o.src, 2048, 6000);
            std::printf("  strings %s: %zu samples in %lld ms, faulted %d\n", variant == 0 ? "empty at the re-open" : "empty at the open", got, msSince(t0), o.src.faulted() ? 1 : 0);
            CHECK(got >= 2048);
            CHECK(!o.src.faulted());
            CHECK(o.src.reopenCount() == 1);
        }
        o.src.stop();
        if (variant == 0) {
            int reads = 0;   // device-descriptor reads: two per bring-up (manufacturer and product), two more for each read of the strings again
            for (const cascade::usb::FakeControl& c : freshPtr->controls) {
                if (!c.out && c.requestType == 0x80 && c.request == 0x06 && c.value == 0x0100) { ++reads; }
            }
            std::printf("  descriptor reads on the new device: %d\n", reads);
            CHECK(reads == 6);                       // the bring-up's, then the second and the third read of both strings
        }
    }

    // U32 (decision S6). (a) A setting changed back to ZERO while the rounds run is applied, not merely acknowledged: the user's correction is 7 ppm; the round that puts
    // it back is interrupted by a setter that makes it 0 (the hook fires inside the write of 7); the next round must write the zero. The register of the correction is 0x3f
    // of the demodulator (data 0x8B for 7 ppm, 0x00 for none): the LAST write of it on the new device is the zero.
    {
        auto pool = std::make_shared<Pool>();
        auto d1 = goodFake();
        Opened o;
        RtlSdrSource* srcPtr = &o.src;
        auto fired = std::make_shared<std::atomic<bool>>(false);
        auto ppmWrites = std::make_shared<std::vector<int>>();
        auto ppmM = std::make_shared<std::mutex>();
        d1->onControlOut = [srcPtr, fired, ppmWrites, ppmM](const cascade::usb::FakeControl& c) {
            if (!(c.out && c.request == 0 && c.value == 0x3f20 && c.index == 0x0011 && !c.data.empty())) { return; }
            {
                std::lock_guard<std::mutex> lk(*ppmM);
                ppmWrites->push_back(c.data[0]);
            }
            if (c.data[0] == 0x8B && srcPtr->reopeningForTest() && !fired->exchange(true)) { srcPtr->setFreqCorrectionPpm(0); }
        };
        pool->fakes.push_back(std::move(d1));
        auto f0 = goodFake();
        f0->bulkQueue.push_back(payload(2048));
        openOn(o, std::move(f0));
        CHECK(o.src.setFreqCorrectionPpm(7));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(3000, 20);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        kill(*o.live);
        CHECK(drain(o.src, 2048, 6000) >= 2048);
        o.src.stop();
        std::vector<int> seen;
        {
            std::lock_guard<std::mutex> lk(*ppmM);
            seen = *ppmWrites;
        }
        std::string line;
        for (const int b : seen) { line += (line.empty() ? "" : " ") + std::to_string(b); }
        std::printf("  correction register writes on the new device (data bytes): %s\n", line.c_str());
        CHECK(fired->load());
        CHECK(o.src.freqCorrectionPpm() == 0);
        int last7 = -1;
        int last0 = -1;
        for (std::size_t i = 0; i < seen.size(); ++i) {
            if (seen[i] == 0x8B) { last7 = static_cast<int>(i); }
            if (seen[i] == 0x00) { last0 = static_cast<int>(i); }
        }
        CHECK(last7 >= 0);          // the 7 ppm was put back in the first round
        CHECK(last0 > last7);       // and the zero after it: the chip ends with no correction
    }
    // (b) after three unsettled rounds the newest wish is applied by one last pass. The tee is the witness (it is on the wire as GPO bit 0): the user's wish is ON, and every
    // write of the tee by the re-open is answered, from the reader's own thread, by the opposite wish, three times over. The wire must end on the LAST wish (OFF); a re-open
    // that stopped after its third round would end on the third round's ON.
    {
        auto wireOld = std::make_shared<Wire>();
        auto wireNew = std::make_shared<Wire>();
        auto pool = std::make_shared<Pool>();
        auto d1 = dongle(wireNew, false, false);
        Opened o;
        RtlSdrSource* srcPtr = &o.src;
        const auto inner = d1->onControlOut;
        auto fires = std::make_shared<std::atomic<int>>(0);
        auto seenOn = std::make_shared<std::atomic<bool>>(false);
        auto lastWish = std::make_shared<std::atomic<bool>>(true);
        d1->onControlOut = [inner, srcPtr, fires, seenOn, lastWish](const cascade::usb::FakeControl& c) {
            if (inner) { inner(c); }
            if (!(c.out && c.request == 0 && c.index == kSysWrite && c.value == kGpo && !c.data.empty())) { return; }
            const bool on = (c.data[0] & 0x01) != 0;
            if (on) { seenOn->store(true); }
            // the bring-up's own write is an OFF before any ON; only the settings' writes (from the first ON on) are answered
            if (seenOn->load() && srcPtr->reopeningForTest() && fires->load() < 3) {
                fires->fetch_add(1);
                lastWish->store(!on);
                srcPtr->setBiasT(!on);
            }
        };
        pool->fakes.push_back(std::move(d1));
        auto f0 = dongle(wireOld, false, false);
        f0->bulkQueue.push_back(payload(2048));
        openOn(o, std::move(f0));
        CHECK(o.src.setBiasT(true));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(5000, 50);
        const int saidBefore = logCount("the newest one was put on in a last pass");
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        kill(*o.live);
        CHECK(drain(o.src, 2048, 6000) >= 2048);
        std::printf("  three unsettled rounds: %d answers, last wish %s, new device ends %s, readback %s\n", fires->load(), lastWish->load() ? "ON" : "OFF", wireNew->lastOn() ? "ON" : "OFF", o.src.biasT() ? "ON" : "OFF");
        CHECK(fires->load() == 3);
        CHECK(!lastWish->load());
        CHECK(!wireNew->lastOn());                 // the newest wish is on the wire
        CHECK(!o.src.biasT());
        CHECK(logCount("the newest one was put on in a last pass") == saidBefore + 1);
        CHECK(!o.src.faulted());
        o.src.stop();
    }

    // U33 (decision S7): the branch the first lane said was missing for this branch. A setter that saw `reopening` true is HELD (by the test seam) until the re-open has ended,
    // so its second look finds the flag down and it goes through retakeAfterReopen. The reader is at that moment reading (each bulk read holds the device lock): the setter
    // must still get the lock and reach the chip, because it announces itself in controlWaiters like any control call. The new device's writes are those of a reference:
    // the same settings, the stream start, and then the tune.
    {
        auto pool = std::make_shared<Pool>();
        auto d1 = goodFake();
        auto log1 = std::make_shared<std::vector<std::string>>();
        auto log1m = std::make_shared<std::mutex>();
        auto late = std::make_shared<std::thread>();
        auto setterOk = std::make_shared<std::atomic<int>>(-1);
        auto started = std::make_shared<std::atomic<bool>>(false);
        auto held = std::make_shared<std::atomic<bool>>(false);
        Opened o;
        RtlSdrSource* srcPtr = &o.src;
        d1->onControlOut = [log1, log1m, late, setterOk, started, held, srcPtr](const cascade::usb::FakeControl& c) {
            {
                std::lock_guard<std::mutex> lk(*log1m);
                log1->push_back(c.text());
            }
            if (srcPtr->reopeningForTest() && !started->exchange(true)) {
                *late = std::thread([srcPtr, setterOk] { setterOk->store(srcPtr->setCenterFrequencyHz(98000000.0) ? 1 : 0); });
                // the re-open goes no further until the setter is inside its hold (it has seen `reopening` and is about to hand its change over): the race is then not one
                const auto until = Clock::now() + std::chrono::milliseconds(5000);
                while (!held->load() && Clock::now() < until) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            }
        };
        pool->fakes.push_back(std::move(d1));
        auto f0 = goodFake();
        f0->bulkQueue.push_back(payload(2048));
        openOn(o, std::move(f0));
        CHECK(o.src.setCenterFrequencyHz(433920000.0));
        auto maxWaiters = std::make_shared<std::atomic<int>>(0);
        o.src.setRetakeProbeForTest([maxWaiters](int waiters) {
            int seen = maxWaiters->load();
            while (waiters > seen && !maxWaiters->compare_exchange_weak(seen, waiters)) {}
        });
        o.src.setBeforeDeferForTest([srcPtr, held] {
            held->store(true);
            const auto until = Clock::now() + std::chrono::milliseconds(8000);
            while (srcPtr->reopeningForTest() && Clock::now() < until) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        });
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(3000, 20);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        kill(*o.live);
        CHECK(waitFor([&] { return started->load(); }, 6000));
        CHECK(waitFor([&] { return setterOk->load() >= 0; }, 8000));
        late->join();
        CHECK(drain(o.src, 1024, 4000) >= 1024);
        std::printf("  setter held through the end of the re-open: held %d, answered %d, waiters announced %d, centre %.0f Hz, faulted %d\n", held->load() ? 1 : 0, setterOk->load(), maxWaiters->load(), o.src.centerFrequencyHz(), o.src.faulted() ? 1 : 0);
        CHECK(held->load());
        CHECK(maxWaiters->load() >= 1);         // the wait for the device lock announced itself, so the reader stood aside for it
        CHECK(setterOk->load() == 1);
        CHECK(o.src.centerFrequencyHz() == 98000000.0);
        o.src.stop();
        std::vector<std::string> have;
        {
            std::lock_guard<std::mutex> lk(*log1m);
            have = *log1;
        }
        RtlSdrSource ref;
        auto rf = std::make_unique<FakeUsbDevice>();
        dressAsR820T(*rf);
        FakeUsbDevice* refLive = rf.get();
        CHECK(ref.openWithTransport(std::move(rf), "fake R820T dongle"));
        CHECK(ref.setCenterFrequencyHz(433920000.0));
        CHECK(ref.start());
        CHECK(ref.setCenterFrequencyHz(98000000.0));
        const std::vector<std::string> want = writeTexts(*refLive);
        ref.stop();
        CHECK(!want.empty());
        CHECK(have.size() >= want.size());
        for (std::size_t i = 0; i < want.size() && i < have.size(); ++i) {
            if (want[i] != have[i]) { std::printf("  FIRST DIFFERENCE at write %zu: want %s, have %s\n", i, want[i].c_str(), have[i].c_str()); }
            CHECK(want[i] == have[i]);
            if (want[i] != have[i]) { break; }
        }
    }

    // U34 (decision S10): a user's stop during a reconnect is not a radio fault in the journal. The radio is registered as the engine registers it; stop() lands in the wait.
    // The driver says nothing to the journal (takeCloseReported stays false: the engine writes its own close event, with the cause of a user's stop) while the sentence on
    // screen is the same. A radio that really gave up (it never comes back) still says its ending.
    {
        namespace rr = cascade::core::radioreport;
        const rr::RadioIdentity rid = rr::identify("rtlsdr", "serial=00000001");
        {
            auto pool = std::make_shared<Pool>();
            pool->absentFirst = 1000000;
            Opened o;
            auto f0 = goodFake();
            f0->bulkQueue.push_back(payload(2048));
            openOn(o, std::move(f0));
            rr::registerSource(&o.src, rid);
            o.src.setReopenForTest(seamOf(pool));
            o.src.setReopenBoundsForTest(5000, 100);
            CHECK(o.src.start());
            CHECK(drain(o.src, 1024, 3000) >= 1024);
            killGone(*o.live);
            CHECK(waitFor([&] { return pool->calls.load() >= 1; }, 4000));
            o.src.stop();
            const bool said = rr::takeCloseReported(rid.key);
            std::printf("  user stop in the reconnect: words \"%s\", driver said the close: %d\n", o.src.lastError(), said ? 1 : 0);
            CHECK(std::string(o.src.lastError()) == cascade::source::kReopenStoppedSentence);
            CHECK(o.src.faulted());
            CHECK(!said);
            rr::forgetRadio(rid.key);
        }
        {
            auto pool = std::make_shared<Pool>();
            pool->absentFirst = 1000000;
            Opened o;
            auto f0 = goodFake();
            f0->bulkQueue.push_back(payload(2048));
            openOn(o, std::move(f0));
            rr::registerSource(&o.src, rid);
            o.src.setReopenForTest(seamOf(pool));
            o.src.setReopenBoundsForTest(300, 50);
            CHECK(o.src.start());
            CHECK(drain(o.src, 1024, 3000) >= 1024);
            killGone(*o.live);
            CHECK(waitFor([&] { return o.src.faulted(); }, 4000));
            const bool said = waitFor([&] { return rr::takeCloseReported(rid.key); }, 2000);   // the fault is latched first, the journal's event a moment after
            std::printf("  a radio that never came back: driver said the close: %d\n", said ? 1 : 0);
            CHECK(said);
            o.src.stop();
            rr::forgetRadio(rid.key);
        }
    }

    return testSummary("test_rtlsdr_reopen");
}
