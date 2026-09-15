// mirisdr_fake_usb.hpp - a Mirics MSi2500 that lives in the test process.
//
// THERE IS NO MIRICS DEVICE ON THIS BENCH, and there was none while the driver
// was written. That is not a gap to apologise for, it is the reason
// cascade::usb::UsbDevice is an interface at all: the only claim a driver can
// make without hardware is "these are the bytes I send", and the only way to
// prove it is to put something on the other end of the wire that records them.
//
// It is hackrf_fake_usb.hpp's shape, with two differences that are about this
// chip rather than about taste:
//
//  1. THIS DEVICE ANSWERS NOTHING. The MSi2500 has no readable identity - no
//     board id, no version string, no serial - and this driver sends no IN
//     transfer at all. controlIn is implemented because the interface has it,
//     records what it was asked for, and answers zero bytes; a test that sees
//     an IN record has caught the driver doing something it should not.
//  2. A FAILURE HAS TO BE AIMED AT A REGISTER, not at a request number. Every
//     register in the chip AND every register in the tuner behind it is
//     written with the same vendor request (0x41), so `failingRequests` alone
//     could only break all of them at once. `failingRegisters` matches the low
//     byte of wValue, which is where the register number rides.
//
// The bulk side is a queue of scripted buffers, so a test can hand the reader
// a known ramp and check that every sample of it comes out of read() once and
// in order, and it can MISBEHAVE in the three ways that matter: a device that
// vanishes mid-stream (readBulk answers negative), a pipe that never completes
// (the bounded join's whole reason for existing), and a control transfer that
// fails.
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

// One control transfer as the device saw it. For a register write the whole
// payload is in `value` and `index`, so `reg` and `regValue` below decode them
// once rather than in every expectation.
struct MiriControlRecord {
    bool in = false;  // true for a device-to-host transfer
    std::uint8_t requestType = 0;
    std::uint8_t request = 0;
    std::uint16_t value = 0;
    std::uint16_t index = 0;
    unsigned timeoutMs = 0;
    std::size_t payloadBytes = 0;  // always 0 for this device: no data stage

    // The register number and the 24-bit value a WriteRegister transfer
    // carries. Meaningless for any other request, and a test that compares
    // them on one is comparing noise - which is why every helper in the suite
    // checks the request first.
    std::uint8_t reg() const { return static_cast<std::uint8_t>(value & 0xFFu); }
    std::uint32_t regValue() const {
        return (static_cast<std::uint32_t>(index) << 8) |
               (static_cast<std::uint32_t>(value >> 8) & 0xFFu);
    }
};

class FakeMiriSdrUsb : public cascade::usb::UsbDevice {
public:
    explicit FakeMiriSdrUsb(std::string devicePath = "\\\\?\\usb#vid_1df7&pid_2500#fake")
        : path_(std::move(devicePath)) {}

    // --- injected failures -------------------------------------------------
    // Vendor requests that fail outright (0x43 start, 0x45 stop), and register
    // numbers whose WRITE fails - as a device unplugged between two transfers
    // does.
    std::vector<std::uint8_t> failingRequests;
    std::vector<std::uint8_t> failingRegisters;

    // What readBulk does once the scripted queue is empty.
    enum class Exhausted {
        Timeout,     // the honest answer of a device that is simply slow: 0
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
    std::vector<MiriControlRecord> controls() const {
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
        MiriControlRecord rec;
        rec.in = false;
        rec.requestType = requestType;
        rec.request = request;
        rec.value = value;
        rec.index = index;
        rec.timeoutMs = timeoutMs;
        rec.payloadBytes = data != nullptr ? len : 0;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            controls_.push_back(rec);
        }
        if (fails(request, rec.reg())) {
            lastError_ = "fake: the device is gone";
            return -1;
        }
        // The byte count moved, which for this driver is ALWAYS ZERO: every
        // transfer it sends is setup-only, with the whole register in the
        // setup packet. So zero is the success answer here, and a driver that
        // treated a zero return as a short transfer would fail against this
        // fake exactly as it would against WinUSB.
        return static_cast<int>(len);
    }

    int controlIn(std::uint8_t requestType, std::uint8_t request, std::uint16_t value,
                  std::uint16_t index, std::uint8_t* data, std::size_t len,
                  unsigned timeoutMs) override {
        (void)data;
        (void)len;
        MiriControlRecord rec;
        rec.in = true;
        rec.requestType = requestType;
        rec.request = request;
        rec.value = value;
        rec.index = index;
        rec.timeoutMs = timeoutMs;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            controls_.push_back(rec);
        }
        // This chip has nothing to read back. See the file header.
        return 0;
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
                // single long sleep so the test can end it promptly once it has
                // proved the bounded join did not wait for it.
                while (!releaseBlock.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                return 0;
            }
            case Exhausted::Timeout:
            default: {
                // What a real bulk pipe does with nothing queued: block for the
                // timeout, then say nothing arrived. Capped so a suite is not
                // paced by it.
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
    bool fails(std::uint8_t request, std::uint8_t reg) const {
        for (const std::uint8_t r : failingRequests) {
            if (r == request) { return true; }
        }
        if (request == 0x41) {
            for (const std::uint8_t r : failingRegisters) {
                if (r == reg) { return true; }
            }
        }
        return false;
    }

    mutable std::mutex mutex_;
    std::vector<MiriControlRecord> controls_;
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
