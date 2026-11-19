// test_hackrf_stream_recovery.cpp - A SILENT OR STALLED HACKRF STREAM IS RE-ARMED AND THEN FAULTED, NEVER LEFT "OPEN AND FINE" (field fault 3, FoxSDR 0.99.109, 2026-11-19).
//
// The field evidence (journal of 2026-11-19, one install, one HackRF One, 2 MS/s): 12 of 18 one-minute windows carried ZERO samples, the driver's own
// health window read 449 s and 440 s with one stall each, the radio delivered about 3 percent of its data, and nothing was reported, closed or recovered:
// every read() answered the pipeline's "0, nothing yet" and nothing in the pipeline or the driver turned minutes of nothing into a fault. This holds that
//   A  one failed read (Windows error 31) is survived by re-arming the stream, and the samples after it arrive;
//   B  a stream that says nothing (every read times out) is re-armed three times and then FAULTED, in words that say what it is, which the radio
//      reports and the pipeline's source loop turns into a closed radio;
//   C  a reader stuck inside the transport for good is a fault the pipeline's thread hears through read();
//   D  a device that is plainly gone is still fatal at once (no re-arm), as before.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "hackrf_fake_usb.hpp"
#include "source/hackrf_protocol.hpp"
#include "source/hackrf_source.hpp"
#include "test_check.hpp"

using cascade::source::HackRfSource;
using cascade::test::FakeHackRfUsb;

namespace {

std::vector<cascade::usb::UsbDeviceInfo> oneFakeDevice() {
    cascade::usb::UsbDeviceInfo d;
    d.path = "\\\\?\\usb#vid_1d50&pid_6089#fake";
    d.serial = "0000000000000000457863c8";
    d.description = "HackRF One";
    d.vid = 0x1d50;
    d.pid = 0x6089;
    return {d};
}

FakeHackRfUsb* attachFake(HackRfSource& src) {
    auto owned = std::make_unique<FakeHackRfUsb>();
    FakeHackRfUsb* raw = owned.get();
    auto holder = std::make_shared<std::unique_ptr<cascade::usb::UsbDevice>>(std::move(owned));
    src.setTransportForTest(oneFakeDevice(), [holder](const std::string&, std::string& error) {
        if (*holder == nullptr) { error = "fake: already handed out"; }
        return std::move(*holder);
    });
    return raw;
}

template <typename Fn>
bool waitFor(Fn fn, int ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (fn()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return fn();
}

std::vector<std::uint8_t> transfer() { return std::vector<std::uint8_t>(cascade::source::hackrf::kTransferBufferBytes, 0x80); }

std::size_t drain(HackRfSource& src, std::size_t want, int ms) {
    std::vector<std::complex<float>> buf(65536);
    std::size_t total = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (total < want && std::chrono::steady_clock::now() < until) { total += src.read(buf.data(), buf.size()); }
    return total;
}

}  // namespace

int main() {
    std::printf("test_hackrf_stream_recovery\n");

    // A: one failed read, then data.
    {
        HackRfSource src;
        FakeHackRfUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->failNextReads.store(1);
        for (int i = 0; i < 2; ++i) { fake->queueBulk(transfer()); }
        const int beginsBefore = fake->beginBulkCalls();
        CHECK(src.start());
        const std::size_t want = 2 * cascade::source::hackrf::kSamplesPerTransfer;
        const std::size_t got = drain(src, want, 3000);
        std::printf("  after one failed read: %zu of %zu samples, faulted %d, stream begins %d -> %d\n", got, want, src.faulted() ? 1 : 0, beginsBefore,
                    fake->beginBulkCalls());
        CHECK(!src.faulted());
        CHECK(got == want);
        CHECK(fake->beginBulkCalls() >= beginsBefore + 2);   // the start, and the re-arm
        src.stop();
        src.closeDevice();
    }

    // B: silence. Every read times out; re-armed three times, then faulted.
    {
        HackRfSource src;
        FakeHackRfUsb* fake = attachFake(src);
        CHECK(src.open(""));
        src.setStallBoundsForTest(150, 20000);
        const int beginsBefore = fake->beginBulkCalls();
        CHECK(src.start());                                  // nothing queued: every read times out
        CHECK(waitFor([&src] { return src.faulted(); }, 6000));
        const std::string said = src.lastError();
        std::printf("  silent stream: %s (stream begins %d -> %d)\n", said.c_str(), beginsBefore, fake->beginBulkCalls());
        CHECK(src.deviceDead());
        CHECK(src.faultedWhile() == "reading samples");
        CHECK(said.find("delivered nothing") != std::string::npos);
        CHECK(fake->beginBulkCalls() == beginsBefore + 1 + 3);
        std::complex<float> buf[8];
        CHECK(src.read(buf, 8) == 0);
        src.stop();
        src.closeDevice();
    }

    // B2: silent for a while and then delivering after a re-arm: carried through.
    {
        HackRfSource src;
        FakeHackRfUsb* fake = attachFake(src);
        CHECK(src.open(""));
        src.setStallBoundsForTest(150, 20000);
        CHECK(src.start());
        // let the first silence pass, then data arrives
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        for (int i = 0; i < 2; ++i) { fake->queueBulk(transfer()); }
        const std::size_t want = 2 * cascade::source::hackrf::kSamplesPerTransfer;
        const std::size_t got = drain(src, want, 3000);
        std::printf("  silent then data: %zu samples, faulted %d\n", got, src.faulted() ? 1 : 0);
        CHECK(got == want);
        CHECK(!src.faulted());
        src.stop();
        src.closeDevice();
    }

    // C: a reader stuck inside the transport is heard by read().
    {
        HackRfSource src;
        FakeHackRfUsb* fake = attachFake(src);
        CHECK(src.open(""));
        src.setStallBoundsForTest(3000, 300);
        fake->onExhausted.store(FakeHackRfUsb::Exhausted::Block);
        CHECK(src.start());
        std::vector<std::complex<float>> buf(1024);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!src.faulted() && std::chrono::steady_clock::now() < until) { src.read(buf.data(), buf.size()); }
        const std::string said = src.lastError();
        std::printf("  stuck reader: %s\n", said.c_str());
        CHECK(src.faulted());
        CHECK(said.find("did not return") != std::string::npos);
        fake->releaseBlock.store(true);
        src.stop();
        src.closeDevice();
    }

    // D: a device that is plainly gone is fatal at once: no re-arm.
    {
        HackRfSource src;
        FakeHackRfUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->onExhausted.store(FakeHackRfUsb::Exhausted::DeviceGone);
        const int beginsBefore = fake->beginBulkCalls();
        CHECK(src.start());
        CHECK(waitFor([&src] { return src.faulted(); }, 2000));
        CHECK(fake->beginBulkCalls() == beginsBefore + 1);
        src.stop();
        src.closeDevice();
    }

    return testSummary("test_hackrf_stream_recovery");
}
