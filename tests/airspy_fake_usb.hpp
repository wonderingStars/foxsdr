// airspy_fake_usb.hpp - an Airspy that lives in the test process.
//
// THERE IS NO AIRSPY ON THIS BENCH, and there was none while the driver was
// written. That is not a gap to apologise for, it is the reason this file
// exists and the reason cascade::usb::UsbDevice is an interface at all: the
// only claim a driver can make without hardware is "these are the bytes I
// send", and the only way to prove it is to put something on the other end of
// the wire that records them.
//
// So this records EVERY control transfer - direction, request type, request
// number, value, index, payload - in order, and answers what the firmware
// answers: the board id, the version string, the part id / serial, the
// sample-rate count and list, and the one-byte acknowledgement every setter
// returns. The bulk side is a queue of scripted buffers, so a test can hand
// the reader a known pattern and check that every sample of it comes out of
// read() once, in order.
//
// It also has to be able to MISBEHAVE, because the interesting half of a
// driver is what it does when the radio stops: a device that vanishes
// mid-stream (readBulk answers negative), a pipe that never completes (the
// bounded join's whole reason for existing), and a control request that fails.
// Each of those is one flag here.
//
// IT IS A SIBLING OF tests/hackrf_fake_usb.hpp RATHER THAN THE SHARED
// src/usb/usb_fake.hpp, and that header's own note says why the HackRF's could
// not be folded into the shared one: the shared fake records into a plain
// std::vector with plain ints, and this driver's reader thread calls readBulk
// concurrently with control transfers on the GUI thread by design, so pointing
// this suite at it would be a data race in the harness. It also has no
// equivalent of Exhausted::Block, which is the only way to prove the bounded
// join does what it exists for. What is different from the HackRF's fake is
// only what an Airspy answers: GET_SAMPLERATES, the packing acknowledgement,
// and a bulk queue that carries PACKED bytes.
//
// One deliberate naming difference: the transfer record here is
// AirspyControlRecord, not ControlRecord. Both fakes live in cascade::test and
// a future test that wanted to drive an Airspy and a HackRF in one translation
// unit would otherwise be a redefinition; there is no reason to leave that
// trap set.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "usb/usb_device.hpp"

namespace cascade::test {

// One control transfer as the device saw it.
struct AirspyControlRecord {
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

// THE TRANSCRIPT OUTLIVES THE FAKE, deliberately, and this is the one thing
// here that the HackRF's fake does not do.
//
// Two of the things this driver must be proven to do happen AS THE DEVICE IS
// BEING LET GO: closeDevice() switches the bias tee off on its way out, and
// the abandoned-reader path deliberately leaks the UsbDevice rather than
// destroying it under a thread that is still inside it. In both cases the
// test's raw pointer to the fake is either dangling or must not be touched by
// the time there is anything to check - so the record of what was sent lives
// behind a shared_ptr the test can take a copy of BEFORE the device goes, and
// read afterwards. Reaching through a pointer to a destroyed fake is exactly
// the use-after-free a test exists to catch, not to commit; this file's first
// draft did commit it, and the check it was writing passed for the wrong
// reason until the fake outlived its owner.
//
// Its own mutex, because the driver's reader thread and the test's thread both
// reach it.
struct AirspyTranscript {
    mutable std::mutex mutex;
    std::vector<AirspyControlRecord> controls;

    std::vector<AirspyControlRecord> snapshot() const {
        std::lock_guard<std::mutex> lk(mutex);
        return controls;
    }
    std::size_t count() const {
        std::lock_guard<std::mutex> lk(mutex);
        return controls.size();
    }
    void clear() {
        std::lock_guard<std::mutex> lk(mutex);
        controls.clear();
    }
    void push(AirspyControlRecord rec) {
        std::lock_guard<std::mutex> lk(mutex);
        controls.push_back(std::move(rec));
    }
};

class FakeAirspyUsb : public cascade::usb::UsbDevice {
public:
    explicit FakeAirspyUsb(std::string devicePath = "\\\\?\\usb#vid_1d50&pid_60a1#fake")
        : path_(std::move(devicePath)) {
        // A plausible part-id/serial: two part-id words then the four-word MCU
        // unique id every Airspy tool prints as the serial number.
        const std::uint32_t words[6] = {0xa000cb3c, 0x0044004c, 0x00000000,
                                        0x00000000, 0x644866c8, 0x3f1a51df};
        for (int w = 0; w < 6; ++w) {
            for (int b = 0; b < 4; ++b) {
                partSerial[w * 4 + b] = static_cast<std::uint8_t>((words[w] >> (8 * b)) & 0xFF);
            }
        }
    }

    // --- what the firmware answers ---------------------------------------
    // AIRSPY_BOARD_ID_PROTO_AIRSPY, which is what BOTH an R2 and a Mini answer
    // (airspy.h:78-82) - the reason the driver reads the model out of strings
    // instead.
    std::uint8_t boardId = 0;
    std::string firmwareVersion = "AirSpy NOS v1.0.0-rc10-6-g4008185 2020-05-08";
    std::uint8_t partSerial[24] = {0};

    // What GET_SAMPLERATES answers with, HIGHEST FIRST, exactly as libairspy's
    // own fallback list is ordered (airspy.c:904-905). These are COMPLEX rates
    // - see airspy_protocol.hpp for the three places in the reference that
    // prove it.
    std::vector<std::uint32_t> sampleRates{10000000u, 2500000u};
    // Set to make the count request answer this instead of sampleRates.size(),
    // which is how a firmware that has lost its mind is imitated.
    int forcedRateCount = -1;

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
    // Take a copy of this BEFORE closing or abandoning the device if anything
    // sent on the way out matters; it stays alive after the fake does.
    std::shared_ptr<AirspyTranscript> transcript() const { return tx_; }

    std::vector<AirspyControlRecord> controls() const { return tx_->snapshot(); }
    std::size_t controlCount() const { return tx_->count(); }
    void clearControls() { tx_->clear(); }

    int beginBulkCalls() const { return beginBulkCalls_.load(); }
    int endBulkCalls() const { return endBulkCalls_.load(); }
    int resetPipeCalls() const { return resetPipeCalls_.load(); }
    std::uint8_t lastResetEndpoint() const { return lastResetEndpoint_.load(); }
    std::uint8_t lastBulkEndpoint() const { return lastEndpoint_.load(); }
    std::size_t lastBulkBufferBytes() const { return lastBufferBytes_.load(); }
    std::size_t lastBulkBufferCount() const { return lastBufferCount_.load(); }
    bool readBulkInFlight() const { return inFlight_.load(); }

    // --- cascade::usb::UsbDevice -------------------------------------------

    int controlOut(std::uint8_t requestType, std::uint8_t request, std::uint16_t value,
                   std::uint16_t index, const std::uint8_t* data, std::size_t len,
                   unsigned timeoutMs) override {
        AirspyControlRecord rec;
        rec.in = false;
        rec.requestType = requestType;
        rec.request = request;
        rec.value = value;
        rec.index = index;
        rec.timeoutMs = timeoutMs;
        if (data != nullptr && len > 0) { rec.data.assign(data, data + len); }
        tx_->push(rec);
        if (fails(request)) {
            lastError_ = "fake: the device is gone";
            return -1;
        }
        return static_cast<int>(len);
    }

    int controlIn(std::uint8_t requestType, std::uint8_t request, std::uint16_t value,
                  std::uint16_t index, std::uint8_t* data, std::size_t len,
                  unsigned timeoutMs) override {
        AirspyControlRecord rec;
        rec.in = true;
        rec.requestType = requestType;
        rec.request = request;
        rec.value = value;
        rec.index = index;
        rec.timeoutMs = timeoutMs;

        if (fails(request)) {
            tx_->push(rec);
            lastError_ = "fake: the device is gone";
            return -1;
        }

        int answered = 0;
        switch (request) {
            case 9:  // AIRSPY_BOARD_ID_READ
                if (len >= 1 && data != nullptr) {
                    data[0] = boardId;
                    answered = 1;
                }
                break;
            case 10: {  // AIRSPY_VERSION_STRING_READ
                const std::size_t n = std::min(len, firmwareVersion.size());
                if (data != nullptr && n > 0) { std::memcpy(data, firmwareVersion.data(), n); }
                answered = static_cast<int>(n);
                break;
            }
            case 11: {  // AIRSPY_BOARD_PARTID_SERIALNO_READ
                const std::size_t n = std::min<std::size_t>(len, sizeof(partSerial));
                if (data != nullptr && n > 0) { std::memcpy(data, partSerial, n); }
                answered = static_cast<int>(n);
                break;
            }
            case 25: {  // AIRSPY_GET_SAMPLERATES
                // airspy.c:812-832: the INDEX word is how many rates are
                // wanted, and zero asks for the COUNT.
                std::vector<std::uint32_t> words;
                if (index == 0) {
                    const std::uint32_t count =
                        forcedRateCount >= 0 ? static_cast<std::uint32_t>(forcedRateCount)
                                             : static_cast<std::uint32_t>(sampleRates.size());
                    words.push_back(count);
                } else {
                    for (std::uint16_t i = 0; i < index && i < sampleRates.size(); ++i) {
                        words.push_back(sampleRates[i]);
                    }
                }
                const std::size_t want = std::min(len, words.size() * 4);
                for (std::size_t b = 0; b < want; ++b) {
                    if (data != nullptr) {
                        data[b] = static_cast<std::uint8_t>((words[b / 4] >> (8 * (b % 4))) & 0xFF);
                    }
                }
                answered = static_cast<int>(want);
                break;
            }
            // The setters. Every one of them is an IN carrying its new value
            // in the INDEX word and answering one byte (airspy.c:1151, :1696,
            // :1726, :1756, :1783, :1810, :1913); the reference never looks at
            // that byte's VALUE, only that it arrived, so this answers a plain
            // 1.
            case 12:  // AIRSPY_SET_SAMPLERATE
            case 14:  // AIRSPY_SET_LNA_GAIN
            case 15:  // AIRSPY_SET_MIXER_GAIN
            case 16:  // AIRSPY_SET_VGA_GAIN
            case 17:  // AIRSPY_SET_LNA_AGC
            case 18:  // AIRSPY_SET_MIXER_AGC
            case 26:  // AIRSPY_SET_PACKING
                if (len >= 1 && data != nullptr) {
                    data[0] = 1;
                    answered = 1;
                }
                break;
            default:
                answered = 0;
                break;
        }
        if (data != nullptr && answered > 0) { rec.data.assign(data, data + answered); }
        tx_->push(rec);
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
        lastResetEndpoint_.store(endpoint);
        resetPipeCalls_.fetch_add(1);
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

    const std::shared_ptr<AirspyTranscript> tx_ = std::make_shared<AirspyTranscript>();
    mutable std::mutex mutex_;  // guards the scripted bulk queue only
    std::deque<std::vector<std::uint8_t>> bulk_;
    std::string path_;
    std::string lastError_;
    std::atomic<bool> streaming_{false};
    std::atomic<bool> inFlight_{false};
    std::atomic<int> beginBulkCalls_{0};
    std::atomic<int> endBulkCalls_{0};
    std::atomic<int> resetPipeCalls_{0};
    std::atomic<std::uint8_t> lastResetEndpoint_{0};
    std::atomic<std::uint8_t> lastEndpoint_{0};
    std::atomic<std::size_t> lastBufferBytes_{0};
    std::atomic<std::size_t> lastBufferCount_{0};
};

}  // namespace cascade::test
