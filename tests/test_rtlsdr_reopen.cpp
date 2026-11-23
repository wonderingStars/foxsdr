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
//   U4   another program took the radio in the gap: a fault at once, cause `fault`, no polling;
//   U5   the chip lost its registers (the handle is fine, the endpoint is silent): only the full bring-up on the new handle makes it deliver;
//   U6   a radio that keeps dropping off: two re-opens per window, then a fault saying so; and twelve per open when the window is forgiven;
//   U7   a stop() during the wait wins at once and joins the reader (the radio is not condemned);
//   U8   a stop() during the bring-up wins inside the join bound (the keep-going token);
//   U9   closeDevice() during the wait returns and leaks nothing;
//   U10  control calls during the re-open return at once, and what they set is what the radio comes back to;
//   U11  the bias tee comes back on the first re-open and is left OFF on a repeat in the same window;
//   U12  a restart that loses the race for the device lock to a control call is a retry, not a fault (defect D1);
//   U13  the outcome words: nothing is raised before samples flow; streamback / usbreopen after; streamfail / usbreopenfail when it did not work;
//   U14  Windows error 995 is restarted like 31;
//   U15  the issue sentences are neutral (no radio family named for a word that is not limited to it).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
};

RtlSdrSource::ReopenFn seamOf(const std::shared_ptr<Pool>& p) {
    return [p](std::string& error, bool& present) -> std::unique_ptr<cascade::usb::UsbDevice> {
        const int n = ++p->calls;
        std::lock_guard<std::mutex> lk(p->m);
        if (p->inUse) {
            error = cascade::usb::kInUseError;
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
        std::unique_ptr<FakeUsbDevice> f = std::move(p->fakes.front());
        p->fakes.pop_front();
        p->handed.push_back(f.get());
        if (p->onHand) { p->onHand(*f); }
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

    // U4: another program took the radio in the gap. A fault at once, a fault (not an unplug), and no further polling.
    {
        auto pool = std::make_shared<Pool>();
        pool->inUse = true;
        Opened o;
        openOn(o, goodFake());
        o.live->bulkQueue.push_back(payload(2048));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(5000, 50);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        const auto t0 = Clock::now();
        kill(*o.live);
        CHECK(waitFor([&] { return o.src.faulted(); }, 4000));
        const long long took = msSince(t0);
        const std::string said = o.src.lastError();
        std::printf("  taken by another program: %lld ms, %d seam calls: %s\n", took, pool->calls.load(), said.c_str());
        CHECK(took < 1500);
        CHECK(pool->calls.load() == 1);
        CHECK(said.find("another program") != std::string::npos);
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
        o.src.stop();
    }

    // U7: stop() during the wait wins at once; the reader is joined, so the radio is not condemned; nothing polls after it.
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
        o.src.stop();
        const long long took = msSince(t0);
        const int callsAtStop = pool->calls.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        std::printf("  stop during the wait: %lld ms, deviceDead %d, seam calls %d -> %d\n", took, o.src.deviceDead() ? 1 : 0, callsAtStop, pool->calls.load());
        CHECK(took < 1000);
        CHECK(!o.src.deviceDead());
        CHECK(!o.src.faulted());
        CHECK(pool->calls.load() == callsAtStop);
    }

    // U8: stop() during the bring-up: every control transfer of the new device takes 20 ms; the token makes the rest return at once.
    {
        auto pool = std::make_shared<Pool>();
        auto slow = goodFake();
        slow->refuseControlOut = [](const cascade::usb::FakeControl&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return false;
        };
        // (refusing is fine: the bring-up is stopped before it could fail on it, and a refusal would end it as a failure either way)
        std::atomic<bool> handed{false};
        pool->onHand = [&handed](FakeUsbDevice&) { handed = true; };
        pool->fakes.push_back(std::move(slow));
        Opened o;
        openOn(o, goodFake());
        o.live->bulkQueue.push_back(payload(2048));
        o.src.setReopenForTest(seamOf(pool));
        o.src.setReopenBoundsForTest(3000, 20);
        CHECK(o.src.start());
        CHECK(drain(o.src, 1024, 3000) >= 1024);
        kill(*o.live);
        CHECK(waitFor([&] { return handed.load(); }, 4000));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        const auto t0 = Clock::now();
        o.src.stop();
        const long long took = msSince(t0);
        std::printf("  stop during the bring-up: %lld ms, deviceDead %d\n", took, o.src.deviceDead() ? 1 : 0);
        CHECK(took < 1000);
        CHECK(!o.src.deviceDead());
        CHECK(pool->handed.empty() || pool->handed[0]->streamStarts == 0);
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

    return testSummary("test_rtlsdr_reopen");
}
