// test_rtlsdr_stream_recovery.cpp - A FAILED OR SILENT RTL-SDR STREAM IS RE-ARMED, NOT CALLED "UNPLUGGED" (field faults 2 and 3, FoxSDR 0.99.109, 2026-11-19).
//
// The field evidence (journal of 2026-11-19, one install, one NESDR SMArt v5): the radio was closed five times with "reading samples: the radio stopped
// delivering samples (it may have been unplugged)" and once with "this radio has faulted", and the device scan listed the same radio as usable 2 to 79
// seconds later each time. The reader thread faulted the radio for good on ONE failed bulk read, while winusb_device.cpp documents that a stalled pipe
// fails one read (Windows error 31) and delivers on the next. This holds that
//   A  one failed read is survived: the stream is re-armed, the source is not faulted, and the samples that follow arrive;
//   B  a stream that keeps failing is a fault only after kMaxStreamRearms (3) re-arms, worded WITHOUT "unplugged", carrying the transport's own text,
//      and the close cause the report contract derives from it is `fault`, not `unplugged`;
//   C  an error that says the device is gone is still fatal at once and is still worded "unplugged";
//   D  a stream that is silent (every read times out) is re-armed and then faulted, instead of looking open and fine for ever;
//   E  a reader stuck inside a USB call that never returns is a fault the pipeline's thread hears through read().
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <complex>
#include <functional>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/radio_events.hpp"
#include "source/rtlsdr_source.hpp"
#include "test_check.hpp"
#include "usb/usb_fake.hpp"

using cascade::source::RtlSdrSource;
using cascade::usb::FakeUsbDevice;

namespace {

const std::vector<std::uint8_t> kTunerAnswer = {0x69, 0x00, 0x02, 0x00, 0xA4};

void dressAsR820T(FakeUsbDevice& fake) {
    fake.answerIn(0, 0x0034, 0x0600, kTunerAnswer);
    fake.answerIn(0, 0x00a0, 0x0600, {0x00});
}

std::vector<std::uint8_t> payload(std::size_t bytes) { return std::vector<std::uint8_t>(bytes, 0x80); }

bool waitFor(const std::function<bool()>& pred, int ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

std::size_t drain(RtlSdrSource& src, std::size_t want, int ms) {
    std::vector<std::complex<float>> buf(4096);
    std::size_t total = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (total < want && std::chrono::steady_clock::now() < until) { total += src.read(buf.data(), buf.size()); }
    return total;
}

}  // namespace

int main() {
    std::printf("test_rtlsdr_stream_recovery\n");

    // A: one failed read (Windows error 31), then data. The old reader faulted on the failed read.
    {
        RtlSdrSource src;
        auto fk = std::make_unique<FakeUsbDevice>();
        dressAsR820T(*fk);
        FakeUsbDevice* live = fk.get();
        CHECK(src.openWithTransport(std::move(fk), "fake R820T dongle"));
        live->failBulkReadsAt = {1};                                  // the very first read fails once
        for (int i = 0; i < 4; ++i) { live->bulkQueue.push_back(payload(2048)); }   // 4 x 1024 samples after it
        const int startsBefore = live->streamStarts;
        CHECK(src.start());
        const std::size_t got = drain(src, 4096, 3000);
        std::printf("  after one failed read: %zu samples, faulted %d, stream starts %d -> %d\n", got, src.faulted() ? 1 : 0, startsBefore, live->streamStarts);
        CHECK(!src.faulted());
        CHECK(!src.deviceDead());
        CHECK(got == 4096);
        CHECK(live->streamStarts >= startsBefore + 2);                // the start, and the re-arm
        src.stop();
    }

    // B: every read fails with error 31. Faulted only after the re-arms are spent, and not "unplugged".
    {
        RtlSdrSource src;
        auto fk = std::make_unique<FakeUsbDevice>();
        dressAsR820T(*fk);
        FakeUsbDevice* live = fk.get();
        CHECK(src.openWithTransport(std::move(fk), "fake R820T dongle"));
        live->failBulkAfter = 0;
        live->failBulkAfterText = "a bulk read failed (Windows error 31)";
        const int startsBefore = live->streamStarts;
        CHECK(src.start());
        CHECK(waitFor([&] { return src.faulted(); }, 5000));
        const std::string said = src.lastError();
        std::printf("  persistent failure: %s (stream starts %d -> %d)\n", said.c_str(), startsBefore, live->streamStarts);
        CHECK(src.deviceDead());
        CHECK(src.faultedWhile() == "reading samples");
        CHECK(said.find("unplugged") == std::string::npos);
        CHECK(said.find("Windows error 31") != std::string::npos);    // WHICH error, for the next report
        CHECK(said.find("3 restarts") != std::string::npos);
        CHECK(live->streamStarts == startsBefore + 1 + 3);            // the start and exactly three re-arms
        CHECK(cascade::core::radioreport::closeCauseFor(true, said) == "fault");
        const int readsAtFault = live->bulkReads;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(live->bulkReads == readsAtFault);                       // the reader left
        src.stop();
    }

    // C: an error that says the device is gone is fatal at once, and is still an unplug.
    {
        RtlSdrSource src;
        auto fk = std::make_unique<FakeUsbDevice>();
        dressAsR820T(*fk);
        FakeUsbDevice* live = fk.get();
        CHECK(src.openWithTransport(std::move(fk), "fake R820T dongle"));
        live->failBulkAfter = 0;
        live->failBulkAfterText = "a bulk read failed (Windows error 1167)";   // ERROR_DEVICE_NOT_CONNECTED
        const int startsBefore = live->streamStarts;
        CHECK(src.start());
        CHECK(waitFor([&] { return src.faulted(); }, 3000));
        const std::string said = src.lastError();
        std::printf("  device gone: %s\n", said.c_str());
        CHECK(said.find("unplugged") != std::string::npos);
        CHECK(live->streamStarts == startsBefore + 1);                // no re-arm for a dongle that left
        CHECK(cascade::core::radioreport::closeCauseFor(true, said) == "unplugged");
        src.stop();
    }

    // D: silence. Every read times out; the stream is re-armed, then faulted.
    {
        RtlSdrSource src;
        auto fk = std::make_unique<FakeUsbDevice>();
        dressAsR820T(*fk);
        FakeUsbDevice* live = fk.get();
        CHECK(src.openWithTransport(std::move(fk), "fake R820T dongle"));
        src.setStallBoundsForTest(150, 20000);
        const int startsBefore = live->streamStarts;
        CHECK(src.start());                                           // nothing queued: every read times out
        CHECK(waitFor([&] { return src.faulted(); }, 5000));
        const std::string said = src.lastError();
        std::printf("  silent stream: %s (stream starts %d -> %d)\n", said.c_str(), startsBefore, live->streamStarts);
        CHECK(said.find("delivered nothing") != std::string::npos);
        CHECK(said.find("unplugged") == std::string::npos);
        CHECK(live->streamStarts == startsBefore + 1 + 3);
        CHECK(src.deviceDead());
        src.stop();
    }

    // D2: a stream that is silent for a while and then delivers is carried through by one re-arm.
    {
        RtlSdrSource src;
        auto fk = std::make_unique<FakeUsbDevice>();
        dressAsR820T(*fk);
        FakeUsbDevice* live = fk.get();
        CHECK(src.openWithTransport(std::move(fk), "fake R820T dongle"));
        src.setStallBoundsForTest(150, 20000);
        // data arrives only for a stream that was re-armed (the way a halted endpoint wakes after the start sequence)
        live->onBeginBulk = [](FakeUsbDevice& d) {
            if (d.streamStarts >= 2) { for (int i = 0; i < 4; ++i) { d.bulkQueue.push_back(payload(2048)); } }
        };
        CHECK(src.start());
        const std::size_t got = drain(src, 4096, 4000);
        std::printf("  silent then re-armed: %zu samples\n", got);
        CHECK(got == 4096);
        CHECK(!src.faulted());
        src.stop();
    }

    // E: a reader stuck inside the transport for good is heard by the pipeline's read().
    {
        RtlSdrSource src;
        auto fk = std::make_unique<FakeUsbDevice>();
        dressAsR820T(*fk);
        FakeUsbDevice* live = fk.get();
        CHECK(src.openWithTransport(std::move(fk), "fake R820T dongle"));
        src.setStallBoundsForTest(3000, 300);
        live->blockReads.store(true);                                 // the reader parks inside the transport
        CHECK(src.start());
        std::vector<std::complex<float>> buf(1024);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!src.faulted() && std::chrono::steady_clock::now() < until) { src.read(buf.data(), buf.size()); }
        const std::string said = src.lastError();
        std::printf("  stuck reader: %s\n", said.c_str());
        CHECK(src.faulted());
        CHECK(said.find("did not return") != std::string::npos);
        live->blockReads.store(false);                                    // let the parked reader go so stop() can join it
        src.stop();
    }

    return testSummary("test_rtlsdr_stream_recovery");
}
