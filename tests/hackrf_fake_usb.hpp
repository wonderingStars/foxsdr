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
        }
        if (fails(request)) {
            lastError_ = "fake: the device is gone";
            return -1;
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
        }
        return answered;
    }

    bool beginBulkStream(std::uint8_t endpoint, std::size_t bufferBytes,
                         std::size_t bufferCount) override {
        lastEndpoint_.store(endpoint);
        lastBufferBytes_.store(bufferBytes);
        lastBufferCount_.store(bufferCount);
        beginBulkCalls_.fetch_add(1);
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
            default: {
                // What a real bulk pipe does with nothing queued: block for
                // the timeout, then say nothing arrived. Capped so a suite is
                // not paced by it.
                const unsigned slice = timeoutMs > 5 ? 5 : timeoutMs;
                std::this_thread::sleep_for(std::chrono::milliseconds(slice));
                return 0;
            }
        }
    }

    void endBulkStream() override {
        streaming_.store(false);
        endBulkCalls_.fetch_add(1);
    }

    bool streaming() const override { return streaming_.load(); }

    bool resetPipe(std::uint8_t endpoint) override {
        (void)endpoint;
        return true;
    }

    const std::string& path() const override { return path_; }
    const std::string& lastError() const override { return lastError_; }

private:
    bool fails(std::uint8_t request) const {
        for (const std::uint8_t r : failingRequests) {
            if (r == request) { return true; }
        }
        return false;
    }

    mutable std::mutex mutex_;
    std::vector<ControlRecord> controls_;
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
