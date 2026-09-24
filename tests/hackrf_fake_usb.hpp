// hackrf_fake_usb.hpp - a HackRF that lives in the test process.
//
// THERE IS NO HACKRF ON THIS BENCH, and there was none while the driver was
// written. That is not a gap to apologise for, it is the reason this file
// exists and the reason cascade::usb::UsbDevice is an interface at all: the
// only claim a driver can make without hardware is "these are the bytes I
// send", and the only way to prove it is to put something on the other end of
// the wire that records them.
//
// So this records EVERY control transfer - direction, request type, request
// number, value, index, payload - in order, and answers the three reads the
// firmware answers (board id, version string, part id / serial) plus the
// one-byte accept/refuse the gain requests return. The bulk side is a queue
// of scripted buffers, so a test can hand the reader a known ramp and check
// that every sample of it comes out of read() once, in order.
//
// It also answers the bulk pipe as the FIRMWARE does across a
// transceiver-mode change - both the current firmware's behaviour and the
// pre-2021 one that disables the endpoint (see Firmware below) - because a
// fake that streams whatever order the driver does things in is what let a
// start/stop order that breaks real radios pass this suite.
//
// It also has to be able to MISBEHAVE, because the interesting half of a
// driver is what it does when the radio stops: a device that vanishes
// mid-stream (readBulk answers negative), a pipe that never completes (the
// bounded join's whole reason for existing), a firmware that refuses a gain,
// and a control request that fails. Each of those is one flag here.
//
// IT IS DELIBERATELY NOT THE SHARED src/usb/usb_fake.hpp, and the reason it
// was written separately has expired while the reason to KEEP it separate has
// not. It began as one of two drivers being written against this interface at
// the same time, by agents who could not afford a fake bending under both.
// Both drivers shipped in 0.91.0, so that reason is gone; these three are what
// it was replaced by, and each one is a thing the shared fake CANNOT currently
// do rather than a preference:
//
//  1. THIS FAKE IS THREAD-SAFE AND THAT ONE IS NOT. HackRfSource's reader
//     thread calls readBulk() concurrently with control transfers on the GUI
//     thread, deliberately - devMutex_ is not taken by the reader, which is
//     the whole reason a retune costs one transfer and does not wait for a
//     stream. So the transcript here is guarded by a mutex and every counter
//     is std::atomic. usb_fake.hpp records into a plain std::vector and
//     mutates plain ints; pointing the HackRF suite at it would be a data
//     race in the harness, of exactly the kind that produces an intermittent
//     failure nobody can reproduce.
//  2. Exhausted::Block HAS NO EQUIVALENT THERE, and it is what proves the one
//     property this driver's bounded join exists for: a readBulk that NEVER
//     RETURNS, so stop() must abandon the reader rather than wait. The shared
//     fake's readBulk always returns within min(timeout, 5) ms, so that path
//     cannot be reached at all. readBulkInFlight() is the other half - it is
//     how a test asserts no thread is inside readBulk when endBulkStream
//     frees the ring, which is the transport's own ordering obligation.
//  3. THE TWO FAKES ANSWER beginBulkStream DIFFERENTLY ON PURPOSE. The shared
//     one REFUSES a buffer size that is not a multiple of 512, mirroring what
//     WinUSB does, because that is a rule the RTL-SDR driver must be held to.
//     This one accepts and RECORDS whatever it is given, because what the
//     HackRF tests assert is the exact size and count the driver asked for.
//     Merging them means one of the two suites stops proving what it proves.
//
// Both implement cascade::usb::UsbDevice, so the compiler guarantees they
// stay faithful to the same interface; what differs is only what each can
// stage. If the shared fake ever grows a guarded transcript and a blocking
// exhaustion mode, fold this into it - the merge is worth doing, it is simply
// not mechanical, and the evidence for a driver nobody here has the hardware
// for is not the thing to refactor on a hunch.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "usb/usb_device.hpp"

namespace cascade::test {

// One control transfer as the device saw it.
struct ControlRecord {
    bool in = false;  // true for a device-to-host transfer
    std::uint8_t requestType = 0;
    std::uint8_t request = 0;
    std::uint16_t value = 0;
    std::uint16_t index = 0;
    unsigned timeoutMs = 0;
    // For an OUT, the payload the driver sent. For an IN, the bytes this fake
    // answered with - so a test can check what the driver was told as well as
    // what it asked.
    std::vector<std::uint8_t> data;
};

class FakeHackRfUsb : public cascade::usb::UsbDevice {
public:
    explicit FakeHackRfUsb(std::string devicePath = "\\\\?\\usb#vid_1d50&pid_6089#fake")
        : path_(std::move(devicePath)) {
        // A plausible part-id/serial: two part-id words then the four-word MCU
        // unique id every HackRF tool prints as the serial number.
        const std::uint32_t words[6] = {0xa000cb3c, 0x00584654, 0x00000000,
                                        0x00000000, 0x457863c8, 0x2e1a51df};
        for (int w = 0; w < 6; ++w) {
            for (int b = 0; b < 4; ++b) {
                partSerial[w * 4 + b] = static_cast<std::uint8_t>((words[w] >> (8 * b)) & 0xFF);
            }
        }
    }

    // --- what the firmware answers ---------------------------------------
    std::uint8_t boardId = 2;  // BOARD_ID_HACKRF_ONE in libhackrf's enum
    std::string firmwareVersion = "2024.02.1";
    std::uint8_t partSerial[24] = {0};
    // The byte SET_LNA_GAIN / SET_VGA_GAIN answer with: nonzero accepted,
    // zero refused (libhackrf hackrf.c:2039 checks exactly this).
    std::uint8_t gainReply = 1;

    // --- injected failures -------------------------------------------------
    // Control requests that fail outright, as a device that has been unplugged
    // between two transfers does.
    std::vector<std::uint8_t> failingRequests;

    // What readBulk does once the scripted queue is empty.
    enum class Exhausted {
        Timeout,     // the honest answer of a radio that is simply slow: 0
        DeviceGone,  // the pipe has failed: negative, lastError() set
        Block,       // never completes, until releaseBlock is set
    };
    std::atomic<Exhausted> onExhausted{Exhausted::Timeout};
    std::atomic<bool> releaseBlock{false};

    // --- THE FIRMWARE'S BULK ENDPOINT -----------------------------------
    //
    // What a real HackRF does with endpoint 0x81 across SET_TRANSCEIVER_MODE,
    // read out of greatscottgadgets/hackrf rather than guessed. It CHANGED in
    // 2020, and radios in the field run both, so both are modelled:
    //
    //  FlushOnModeChange - firmware v2021.03.1 and later (commit d8250c6396,
    //    "Don't re-init bulk endpoints on every set_transceiver_mode call").
    //    The endpoint is initialised once, at SET_CONFIGURATION
    //    (hackrf_usb.c usb_configuration_changed), and a mode change only
    //    FLUSHES it (usb_api_transceiver.c request_transceiver_mode ->
    //    usb_endpoint_flush): primed buffers are dropped, the endpoint stays
    //    enabled, and the host's reads simply wait (NAK) until the firmware
    //    primes it again in RECEIVE. Nothing ever fails. The fake's default,
    //    matching its "2024.02.1" version string.
    //
    //  DisableOnModeChange - firmware v2018.01.1 and every release before it.
    //    set_transceiver_mode calls usb_endpoint_disable(&usb_endpoint_bulk_in)
    //    (TXE cleared, endpoint flushed - common/usb.c) on EVERY mode change,
    //    and only RECEIVE re-initialises it (usb_endpoint_init). Nothing
    //    enables it at SET_CONFIGURATION either, so from plug-in to the first
    //    RECEIVE, and after every OFF, the endpoint is DISABLED. This is the
    //    same code, on the same LPC43xx USB controller, as the Airspy
    //    firmware (a declared HackRF derivative), and on a real Airspy R2
    //    reads queued against that endpoint failed with Windows error 31 on
    //    the first read. That symptom - the host halts its end of the pipe,
    //    and every read fails until it is reset - is what is modelled here;
    //    how the controller answers a token to a disabled endpoint is NOT in
    //    the firmware source, and is taken from that field report.
    //    The firmware does the disable inside the USB ISR BEFORE it
    //    acknowledges the request (usb_vendor_request_set_transceiver_mode:
    //    set_transceiver_mode, then usb_transfer_schedule_ack), after the
    //    RF-path and clock work, so a read already queued fails while the
    //    control transfer is still in flight. modeSwitchDelayMs is that work;
    //    the real window is shorter than 10 ms but it is not zero, and this
    //    makes the race land the same way every run.
    enum class Firmware {
        FlushOnModeChange,    // v2021.03.1 onwards
        DisableOnModeChange,  // v2018.01.1 and earlier
    };
    std::atomic<Firmware> firmware{Firmware::FlushOnModeChange};
    std::atomic<int> modeSwitchDelayMs{10};

    // An old radio: the version string it reports and the endpoint behaviour
    // that goes with it. Call before open(), which reads the version.
    void runFirmware2018() {
        firmwareVersion = "2018.01.1";
        firmware.store(Firmware::DisableOnModeChange);
        // usb_configuration_changed at v2018.01.1 sends the transceiver OFF
        // and initialises no bulk endpoint: disabled until the first RECEIVE.
        endpointEnabled_.store(false);
    }

    // beginBulkStream fails outright, as a transport that cannot allocate or
    // submit its ring does.
    std::atomic<bool> failBeginBulk{false};

    bool receiverInRx() const { return rxMode_.load(); }
    bool hostPipeHalted() const { return hostHalted_.load(); }
    int failedBulkReads() const { return failedReads_.load(); }

    // EVERYTHING THAT REACHES THE DEVICE, in order: "OUT 1 val 1"
    // (SET_TRANSCEIVER_MODE RECEIVE), "IN 19", "BEGIN_BULK", "END_BULK". The
    // controls() transcript cannot say whether the ring was queued before or
    // after RECEIVE, because the bulk calls are not control transfers - and
    // that ordering is the whole of hackrf_start_rx and hackrf_stop_rx.
    std::vector<std::string> events() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return events_;
    }
    void clearEvents() {
        std::lock_guard<std::mutex> lk(mutex_);
        events_.clear();
    }

    // --- scripted bulk data ------------------------------------------------
    void queueBulk(std::vector<std::uint8_t> buf) {
        std::lock_guard<std::mutex> lk(mutex_);
        bulk_.push_back(std::move(buf));
    }

    // --- what the driver did -----------------------------------------------
    std::vector<ControlRecord> controls() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return controls_;
    }
    std::size_t controlCount() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return controls_.size();
    }
    void clearControls() {
        std::lock_guard<std::mutex> lk(mutex_);
        controls_.clear();
    }

    int beginBulkCalls() const { return beginBulkCalls_.load(); }
    int endBulkCalls() const { return endBulkCalls_.load(); }
    std::uint8_t lastBulkEndpoint() const { return lastEndpoint_.load(); }
    std::size_t lastBulkBufferBytes() const { return lastBufferBytes_.load(); }
    std::size_t lastBulkBufferCount() const { return lastBufferCount_.load(); }
    bool readBulkInFlight() const { return inFlight_.load(); }

    // --- cascade::usb::UsbDevice -------------------------------------------

    int controlOut(std::uint8_t requestType, std::uint8_t request, std::uint16_t value,
                   std::uint16_t index, const std::uint8_t* data, std::size_t len,
                   unsigned timeoutMs) override {
        ControlRecord rec;
        rec.in = false;
        rec.requestType = requestType;
        rec.request = request;
        rec.value = value;
        rec.index = index;
        rec.timeoutMs = timeoutMs;
        if (data != nullptr && len > 0) { rec.data.assign(data, data + len); }
        {
            std::lock_guard<std::mutex> lk(mutex_);
            controls_.push_back(rec);
            events_.push_back("OUT " + std::to_string(request) + " val " + std::to_string(value));
        }
        if (fails(request)) {
            lastError_ = "fake: the device is gone";
            return -1;
        }
        if (request == 1) {  // SET_TRANSCEIVER_MODE
            if (firmware.load() == Firmware::DisableOnModeChange) {
                // v2018.01.1 set_transceiver_mode: usb_endpoint_disable first,
                // whatever the new mode - a read queued on the pipe now meets a
                // disabled endpoint and the host halts the pipe under it.
                endpointEnabled_.store(false);
                if (streaming_.load()) { hostHalted_.store(true); }
                // RECEIVE alone re-initialises it (usb_endpoint_init).
                if (value == 1) { endpointEnabled_.store(true); }
                rxMode_.store(value == 1);
                // The ISR's work before the ack (see modeSwitchDelayMs).
                const int delay = modeSwitchDelayMs.load();
                if (delay > 0) { std::this_thread::sleep_for(std::chrono::milliseconds(delay)); }
            } else {
                // v2021.03.1+ request_transceiver_mode only flushes and acks;
                // the endpoint stays enabled, nothing already queued fails,
                // and the mode work happens later in the main loop.
                rxMode_.store(value == 1);
            }
        }
        return static_cast<int>(len);
    }

    int controlIn(std::uint8_t requestType, std::uint8_t request, std::uint16_t value,
                  std::uint16_t index, std::uint8_t* data, std::size_t len,
                  unsigned timeoutMs) override {
        ControlRecord rec;
        rec.in = true;
        rec.requestType = requestType;
        rec.request = request;
        rec.value = value;
        rec.index = index;
        rec.timeoutMs = timeoutMs;

        if (fails(request)) {
            {
                std::lock_guard<std::mutex> lk(mutex_);
                controls_.push_back(rec);
            }
            lastError_ = "fake: the device is gone";
            return -1;
        }

        int answered = 0;
        switch (request) {
            case 14:  // BOARD_ID_READ
                if (len >= 1 && data != nullptr) {
                    data[0] = boardId;
                    answered = 1;
                }
                break;
            case 15: {  // VERSION_STRING_READ
                const std::size_t n = std::min(len, firmwareVersion.size());
                if (data != nullptr && n > 0) { std::memcpy(data, firmwareVersion.data(), n); }
                answered = static_cast<int>(n);
                break;
            }
            case 18: {  // BOARD_PARTID_SERIALNO_READ
                const std::size_t n = std::min<std::size_t>(len, sizeof(partSerial));
                if (data != nullptr && n > 0) { std::memcpy(data, partSerial, n); }
                answered = static_cast<int>(n);
                break;
            }
            case 19:  // SET_LNA_GAIN
            case 20:  // SET_VGA_GAIN
                if (len >= 1 && data != nullptr) {
                    data[0] = gainReply;
                    answered = 1;
                }
                break;
            default:
                answered = 0;
                break;
        }
        if (data != nullptr && answered > 0) {
            rec.data.assign(data, data + answered);
        }
        {
            std::lock_guard<std::mutex> lk(mutex_);
            controls_.push_back(rec);
            events_.push_back("IN " + std::to_string(request));
        }
        return answered;
    }

    bool beginBulkStream(std::uint8_t endpoint, std::size_t bufferBytes,
                         std::size_t bufferCount) override {
        lastEndpoint_.store(endpoint);
        lastBufferBytes_.store(bufferBytes);
        lastBufferCount_.store(bufferCount);
        beginBulkCalls_.fetch_add(1);
        {
            std::lock_guard<std::mutex> lk(mutex_);
            events_.push_back("BEGIN_BULK");
        }
        if (failBeginBulk.load()) {
            lastError_ = "fake: the transfer ring could not be queued";
            return false;
        }
        // The real transport resets the pipe before it queues anything
        // (winusb_device.cpp beginBulkStream: WinUsb_ResetPipe; usbfs likewise),
        // which clears a halt the host's end was left in...
        hostHalted_.store(false);
        // ...and then the ring goes onto the bus. Against a DISABLED endpoint
        // (old firmware, transceiver not in RECEIVE) it halts again at once.
        if (firmware.load() == Firmware::DisableOnModeChange && !endpointEnabled_.load()) {
            hostHalted_.store(true);
        }
        streaming_.store(true);
        return true;
    }

    int readBulk(std::uint8_t* dst, std::size_t cap, unsigned timeoutMs) override {
        inFlight_.store(true);
        struct Clear {
            std::atomic<bool>* f;
            ~Clear() { f->store(false); }
        } clear{&inFlight_};

        if (!streaming_.load()) { return 0; }
        if (hostHalted_.load()) { return haltedRead(); }
        if (!rxMode_.load()) {
            // Transceiver off: the firmware primes nothing, so the read waits
            // (NAK) and then says nothing arrived - unless the pipe halts
            // underneath it, which completes it with an error at once.
            return quietWait(timeoutMs);
        }
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (!bulk_.empty()) {
                std::vector<std::uint8_t> buf = std::move(bulk_.front());
                bulk_.pop_front();
                const std::size_t n = std::min(cap, buf.size());
                if (dst != nullptr && n > 0) { std::memcpy(dst, buf.data(), n); }
                return static_cast<int>(n);
            }
        }
        switch (onExhausted.load()) {
            case Exhausted::DeviceGone:
                lastError_ = "fake: the device was removed";
                return -1;
            case Exhausted::Block: {
                // Never completes until the test lets it. Sliced rather than a
                // single long sleep so the test can end it promptly once it
                // has proved the bounded join did not wait for it.
                while (!releaseBlock.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                return 0;
            }
            case Exhausted::Timeout:
            default:
                // What a real bulk pipe does with nothing queued: block for
                // the timeout, then say nothing arrived. Capped so a suite is
                // not paced by it.
                return quietWait(timeoutMs);
        }
    }

    void endBulkStream() override {
        streaming_.store(false);
        endBulkCalls_.fetch_add(1);
        std::lock_guard<std::mutex> lk(mutex_);
        events_.push_back("END_BULK");
    }

    bool streaming() const override { return streaming_.load(); }

    bool resetPipe(std::uint8_t endpoint) override {
        (void)endpoint;
        return true;
    }

    const std::string& path() const override { return path_; }
    const std::string& lastError() const override { return lastError_; }

private:
    // WinUSB's words for a read on a halted pipe, as winusb_device.cpp's
    // readBulk builds them from GetLastError().
    int haltedRead() {
        failedReads_.fetch_add(1);
        lastError_ = "a bulk read failed (Windows error 31)";
        return -1;
    }

    // Blocks up to the timeout (capped at 5 ms so a suite is not paced by it)
    // in 1 ms slices, returning -1 as soon as the pipe halts under the read -
    // an overlapped read completes with an error the moment the endpoint
    // goes, not at the end of its timeout.
    int quietWait(unsigned timeoutMs) {
        const unsigned total = timeoutMs > 5 ? 5 : timeoutMs;
        for (unsigned waited = 0; waited < total; ++waited) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (hostHalted_.load()) { return haltedRead(); }
        }
        return 0;
    }

    bool fails(std::uint8_t request) const {
        for (const std::uint8_t r : failingRequests) {
            if (r == request) { return true; }
        }
        return false;
    }

    mutable std::mutex mutex_;
    std::vector<ControlRecord> controls_;
    std::vector<std::string> events_;
    // The firmware's side (see Firmware): transceiver in RECEIVE, whether
    // endpoint 0x81 is enabled (always, on current firmware), and whether the
    // HOST's end of the pipe is halted.
    std::atomic<bool> rxMode_{false};
    std::atomic<bool> endpointEnabled_{true};
    std::atomic<bool> hostHalted_{false};
    std::atomic<int> failedReads_{0};
    std::deque<std::vector<std::uint8_t>> bulk_;
    std::string path_;
    std::string lastError_;
    std::atomic<bool> streaming_{false};
    std::atomic<bool> inFlight_{false};
    std::atomic<int> beginBulkCalls_{0};
    std::atomic<int> endBulkCalls_{0};
    std::atomic<std::uint8_t> lastEndpoint_{0};
    std::atomic<std::size_t> lastBufferBytes_{0};
    std::atomic<std::size_t> lastBufferCount_{0};
};

}  // namespace cascade::test
