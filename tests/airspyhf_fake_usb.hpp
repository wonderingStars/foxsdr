// airspyhf_fake_usb.hpp - an Airspy HF+ that lives in the test process.
//
// THERE IS NO AIRSPY HF+ ON THIS BENCH, and there was none while the driver
// was written. That is not a gap to apologise for, it is the reason this file
// exists and the reason cascade::usb::UsbDevice is an interface at all: the
// only claim a driver can make without hardware is "these are the bytes I
// send", and the only way to prove it is to put something on the other end of
// the wire that records them.
//
// It is copied from tests/hackrf_fake_usb.hpp and then taught this firmware's
// answers; that file's header argues at length why it is not the shared
// src/usb/usb_fake.hpp, and every one of those three reasons applies here
// unchanged - a reader thread that calls readBulk concurrently with control
// transfers on the GUI thread needs a transcript behind a mutex and atomic
// counters, Exhausted::Block is what proves the bounded join, and the exact
// buffer size and count the driver asks for is an assertion rather than
// something to be validated against WinUSB's rules.
//
// WHAT THIS ONE ANSWERS THAT THE HACKRF'S DOES NOT. This firmware is asked
// SEVEN questions at open, not three, and four of them are variable-length
// lists whose shape is itself part of the protocol: the sample rates and
// their architectures, the attenuator's steps, the flash configuration page
// and the bias-tee count. Each is answered here the way the reference
// documents the firmware answering it - including the one place where the
// device answers ONE BYTE PER RATE to a request that asked for four
// (GET_SAMPLERATE_ARCHITECTURES, airspyhf.c:627-648), because a driver that
// only works against a fake that answers what was asked for would fail on
// real hardware in a way no test here could find.
//
// AND ONE DELIBERATE OMISSION. There is no separate "this firmware is too old
// for that request" switch, because at the transport there is no such thing:
// an unimplemented vendor request STALLS, and a stall reaches the driver as
// exactly the negative return an unplugged device produces. Old firmware is
// therefore scripted by putting the request number in `failingRequests`, and
// the driver's job - proven in the test - is to carry on for the five
// requests the reference treats as optional and to condemn the device for
// every other one.
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
struct AirspyHfControlRecord {
    bool in = false;  // true for a device-to-host transfer
    std::uint8_t requestType = 0;
    std::uint8_t request = 0;
    std::uint16_t value = 0;
    std::uint16_t index = 0;
    std::size_t requestedLen = 0;
    unsigned timeoutMs = 0;
    // For an OUT, the payload the driver sent. For an IN, the bytes this fake
    // answered with - so a test can check what the driver was told as well as
    // what it asked.
    std::vector<std::uint8_t> data;
};

class FakeAirspyHfUsb : public cascade::usb::UsbDevice {
public:
    explicit FakeAirspyHfUsb(std::string devicePath = "\\\\?\\usb#vid_03eb&pid_800c#fake")
        : path_(std::move(devicePath)) {
        setPartSerial(0x6ba00477u, 0x12345678u, 0x9abcdef0u, 0x00000000u, 0x00000000u);
        setConfig(true, -1200, 0x1234, 0x00000002u);
    }

    // --- what the firmware answers ---------------------------------------

    // airspyhf_read_partid_serialno_t: part_id then serial_no[4]
    // (airspyhf.h:76-79). airspyhf_info.c prints words 1 and 2 of THIS
    // struct's serial_no as the 64-bit serial number every HF+ tool quotes.
    void setPartSerial(std::uint32_t partId, std::uint32_t s0, std::uint32_t s1, std::uint32_t s2,
                       std::uint32_t s3) {
        const std::uint32_t words[5] = {partId, s0, s1, s2, s3};
        for (int w = 0; w < 5; ++w) {
            for (int b = 0; b < 4; ++b) {
                partSerial[w * 4 + b] = static_cast<std::uint8_t>((words[w] >> (8 * b)) & 0xFF);
            }
        }
    }

    // flash_config_t at the head of the 256-byte CONFIG_READ page
    // (airspyhf.c:137-143): magic, calibration_ppb, calibration_vctcxo,
    // frontend_options.
    void setConfig(bool withMagic, std::int32_t ppb, std::uint32_t vctcxo,
                   std::uint32_t frontend) {
        config.assign(256, 0);
        const std::uint32_t words[4] = {withMagic ? 0xA5CA71B0u : 0xDEADBEEFu,
                                        static_cast<std::uint32_t>(ppb), vctcxo, frontend};
        for (int w = 0; w < 4; ++w) {
            for (int b = 0; b < 4; ++b) {
                config[static_cast<std::size_t>(w) * 4 + static_cast<std::size_t>(b)] =
                    static_cast<std::uint8_t>((words[w] >> (8 * b)) & 0xFF);
            }
        }
    }

    std::uint8_t partSerial[20] = {0};
    std::string firmwareVersion = "R3.0.7-CD";

    // The Discovery's list as its current firmware reports it: DESCENDING,
    // which is why the driver keeps the firmware's index separately from the
    // ascending list it shows a panel. The architecture flags are scripted
    // rather than claimed to be true of any particular firmware - what the
    // test proves is that the driver HONOURS the answer (the top two rates
    // zero-IF, the rest low-IF), not that this is the answer a real board
    // gives.
    std::vector<std::uint32_t> rates = {912000, 768000, 456000, 384000, 256000, 192000};
    std::vector<std::uint8_t> architectures = {0, 0, 1, 1, 1, 1};

    // airspyhf.c:1049-1058's nine, as a device that HAS the request answers
    // them.
    std::vector<float> attSteps = {0.0f, 6.0f, 12.0f, 18.0f, 24.0f, 30.0f, 36.0f, 42.0f, 48.0f};

    std::vector<std::uint8_t> config;
    std::int32_t biasTeeCount = 1;

    // GET_FILTER_GAIN answers decibels; the driver turns it into 10^(-dB/20).
    std::uint8_t filterGainDb = 6;

    // GET_FREQ_DELTA's four bytes: exponent, then a 24-bit signed mantissa
    // low byte first (airspyhf.c:1404). 0x1234 over 2^16 is +71.106 Hz.
    std::uint8_t freqDelta[4] = {0x10, 0x34, 0x12, 0x00};

    // --- injected failures -------------------------------------------------
    // Control requests that fail outright - which is both a device that has
    // been unplugged between two transfers AND a firmware too old to have
    // that request (see the file header).
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
    std::vector<AirspyHfControlRecord> controls() const {
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
    int resetPipeCalls() const { return resetPipeCalls_.load(); }
    std::uint8_t lastBulkEndpoint() const { return lastEndpoint_.load(); }
    std::size_t lastBulkBufferBytes() const { return lastBufferBytes_.load(); }
    std::size_t lastBulkBufferCount() const { return lastBufferCount_.load(); }
    bool readBulkInFlight() const { return inFlight_.load(); }

    // --- cascade::usb::UsbDevice -------------------------------------------

    int controlOut(std::uint8_t requestType, std::uint8_t request, std::uint16_t value,
                   std::uint16_t index, const std::uint8_t* data, std::size_t len,
                   unsigned timeoutMs) override {
        AirspyHfControlRecord rec;
        rec.in = false;
        rec.requestType = requestType;
        rec.request = request;
        rec.value = value;
        rec.index = index;
        rec.requestedLen = len;
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
        AirspyHfControlRecord rec;
        rec.in = true;
        rec.requestType = requestType;
        rec.request = request;
        rec.value = value;
        rec.index = index;
        rec.requestedLen = len;
        rec.timeoutMs = timeoutMs;

        if (fails(request)) {
            {
                std::lock_guard<std::mutex> lk(mutex_);
                controls_.push_back(rec);
            }
            lastError_ = "fake: the request stalled";
            return -1;
        }

        int answered = 0;
        switch (request) {
            case 3:  // GET_SAMPLERATES
                // index 0 asks for the COUNT, index n for n rates
                // (airspyhf.c:585-604).
                if (index == 0) {
                    answered = writeWord(data, len, static_cast<std::uint32_t>(rates.size()));
                } else {
                    answered = writeWords(data, len, rates);
                }
                break;
            case 14: {  // GET_SAMPLERATE_ARCHITECTURES
                // ONE BYTE PER RATE, whatever length was asked for - the
                // reference asks for count * 4 into a buffer of count.
                const std::size_t n = std::min(len, architectures.size());
                if (data != nullptr && n > 0) { std::memcpy(data, architectures.data(), n); }
                answered = static_cast<int>(n);
                break;
            }
            case 19:  // GET_ATT_STEPS
                if (index == 0) {
                    answered = writeWord(data, len, static_cast<std::uint32_t>(attSteps.size()));
                } else {
                    answered = writeFloats(data, len, attSteps);
                }
                break;
            case 5: {  // CONFIG_READ
                const std::size_t n = std::min(len, config.size());
                if (data != nullptr && n > 0) { std::memcpy(data, config.data(), n); }
                answered = static_cast<int>(n);
                break;
            }
            case 7: {  // GET_SERIALNO_BOARDID
                const std::size_t n = std::min<std::size_t>(len, sizeof(partSerial));
                if (data != nullptr && n > 0) { std::memcpy(data, partSerial, n); }
                answered = static_cast<int>(n);
                break;
            }
            case 9: {  // GET_VERSION_STRING
                const std::size_t n = std::min(len, firmwareVersion.size());
                if (data != nullptr && n > 0) { std::memcpy(data, firmwareVersion.data(), n); }
                answered = static_cast<int>(n);
                break;
            }
            case 15:  // GET_FILTER_GAIN
                if (len >= 1 && data != nullptr) {
                    data[0] = filterGainDb;
                    answered = 1;
                }
                break;
            case 16: {  // GET_FREQ_DELTA
                const std::size_t n = std::min<std::size_t>(len, sizeof(freqDelta));
                if (data != nullptr && n > 0) { std::memcpy(data, freqDelta, n); }
                answered = static_cast<int>(n);
                break;
            }
            case 20:  // GET_BIAS_TEE_COUNT
                answered = writeWord(data, len, static_cast<std::uint32_t>(biasTeeCount));
                break;
            default:
                answered = 0;
                break;
        }
        if (data != nullptr && answered > 0) { rec.data.assign(data, data + answered); }
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

    static int writeWord(std::uint8_t* data, std::size_t len, std::uint32_t value) {
        if (data == nullptr || len < 4) { return 0; }
        for (int b = 0; b < 4; ++b) {
            data[b] = static_cast<std::uint8_t>((value >> (8 * b)) & 0xFF);
        }
        return 4;
    }

    static int writeWords(std::uint8_t* data, std::size_t len,
                          const std::vector<std::uint32_t>& words) {
        if (data == nullptr) { return 0; }
        const std::size_t n = std::min(len / 4, words.size());
        for (std::size_t i = 0; i < n; ++i) {
            for (int b = 0; b < 4; ++b) {
                data[i * 4 + static_cast<std::size_t>(b)] =
                    static_cast<std::uint8_t>((words[i] >> (8 * b)) & 0xFF);
            }
        }
        return static_cast<int>(n * 4);
    }

    static int writeFloats(std::uint8_t* data, std::size_t len, const std::vector<float>& values) {
        if (data == nullptr) { return 0; }
        const std::size_t n = std::min(len / 4, values.size());
        for (std::size_t i = 0; i < n; ++i) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &values[i], sizeof(bits));
            for (int b = 0; b < 4; ++b) {
                data[i * 4 + static_cast<std::size_t>(b)] =
                    static_cast<std::uint8_t>((bits >> (8 * b)) & 0xFF);
            }
        }
        return static_cast<int>(n * 4);
    }

    mutable std::mutex mutex_;
    std::vector<AirspyHfControlRecord> controls_;
    std::deque<std::vector<std::uint8_t>> bulk_;
    std::string path_;
    std::string lastError_;
    std::atomic<bool> streaming_{false};
    std::atomic<bool> inFlight_{false};
    std::atomic<int> beginBulkCalls_{0};
    std::atomic<int> endBulkCalls_{0};
    std::atomic<int> resetPipeCalls_{0};
    std::atomic<std::uint8_t> lastEndpoint_{0};
    std::atomic<std::size_t> lastBufferBytes_{0};
    std::atomic<std::size_t> lastBufferCount_{0};
};

}  // namespace cascade::test
