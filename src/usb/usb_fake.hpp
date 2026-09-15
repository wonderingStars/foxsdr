// FakeUsbDevice: the transport a driver test talks to instead of a radio.
//
// WHY A FAKE AND NOT A MOCK LIBRARY. What these tests have to prove is not
// "the driver called something" but "the driver put THESE BYTES on the wire,
// in THIS ORDER" - a tuner PLL programmed one register out of order tunes to
// the wrong frequency and still returns success from every call. So this
// records every control transfer verbatim and hands the test the transcript,
// and a test asserts the transcript against the register arithmetic read out
// of the behaviour oracle. A sequence that changes is then a red line with
// the offending step named, not a radio that hears nothing.
//
// Shared by the RTL-SDR and HackRF drivers, so nothing here knows anything
// about either: it is a UsbDevice that remembers what it was told, answers
// controlIn() from a script, and serves bulk reads from a queue.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <cstdio>
#include <chrono>
#include <deque>
#include <map>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "usb/usb_device.hpp"

namespace cascade::usb {

// One control transfer as the driver made it.
struct FakeControl {
    bool out = false;  // true for controlOut (host to device)
    std::uint8_t requestType = 0;
    std::uint8_t request = 0;
    std::uint16_t value = 0;
    std::uint16_t index = 0;
    std::vector<std::uint8_t> data;  // what was sent (out) or answered (in)

    // "OUT 40/00 v=2000 i=0110 [09]" - the form a failing test prints, so a
    // mismatch reads as a wire trace rather than as a vector of integers.
    std::string text() const {
        char head[64];
        std::snprintf(head, sizeof(head), "%s %02X/%02X v=%04X i=%04X [", out ? "OUT" : "IN ",
                      requestType, request, value, index);
        std::string s(head);
        for (std::size_t i = 0; i < data.size(); ++i) {
            char b[8];
            std::snprintf(b, sizeof(b), "%s%02X", i ? " " : "", data[i]);
            s += b;
        }
        s += "]";
        return s;
    }
};

class FakeUsbDevice final : public UsbDevice {
public:
    // --- what the driver did ------------------------------------------------
    std::vector<FakeControl> controls;

    // Only the OUT transfers, which is what a register-sequence assertion
    // almost always wants: the reads a driver interleaves (a PLL lock poll,
    // an i2c read-back) are real traffic but not the thing under test.
    std::vector<FakeControl> writes() const {
        std::vector<FakeControl> v;
        for (const FakeControl& c : controls) {
            if (c.out) { v.push_back(c); }
        }
        return v;
    }

    void clear() { controls.clear(); }

    // --- what the device answers -------------------------------------------

    // Scripted controlIn answers, keyed by (request, value, index). An
    // unscripted read is answered with `defaultInByte` and counted, so a test
    // can assert it scripted everything the driver actually asks for.
    using InKey = std::tuple<std::uint8_t, std::uint16_t, std::uint16_t>;
    std::map<InKey, std::vector<std::uint8_t>> inAnswers;
    std::uint8_t defaultInByte = 0x00;
    int unscriptedReads = 0;

    void answerIn(std::uint8_t request, std::uint16_t value, std::uint16_t index,
                  std::vector<std::uint8_t> bytes) {
        inAnswers[InKey{request, value, index}] = std::move(bytes);
    }

    // Bulk payloads, served one per readBulk() call. An empty entry means
    // "this read times out" (readBulk returns 0), which is how a test drives
    // the reader thread's retry path without real timing.
    std::deque<std::vector<std::uint8_t>> bulkQueue;

    // After this many readBulk() calls, every later one fails (-1). -1
    // disables. This is how "the dongle was unplugged" is staged.
    int failBulkAfter = -1;
    int bulkReads = 0;

    // Fails every control transfer from this call number on; -1 disables.
    int failControlAfter = -1;
    int controlCalls = 0;

    // --- UsbDevice ----------------------------------------------------------

    int controlOut(std::uint8_t requestType, std::uint8_t request, std::uint16_t value,
                   std::uint16_t index, const std::uint8_t* data, std::size_t len,
                   unsigned) override {
        ++controlCalls;
        FakeControl c;
        c.out = true;
        c.requestType = requestType;
        c.request = request;
        c.value = value;
        c.index = index;
        if (data != nullptr && len > 0) { c.data.assign(data, data + len); }
        controls.push_back(std::move(c));
        if (failControlAfter >= 0 && controlCalls > failControlAfter) {
            lastError_ = "fake: control transfer failed";
            return -1;
        }
        return static_cast<int>(len);
    }

    // A STANDARD GET_DESCRIPTOR IS NOT A VENDOR READ, and this fake used to
    // answer it as though it were: it returned the REQUESTED length for every
    // scripted answer, so a driver asking for 256 bytes of a 22-byte string
    // got 256 back and read the string happily. A real RTL2832U does neither
    // of those things. Measured on the RTL2838 on this desk (2026-09-15),
    // through WinUsb_ControlTransfer and WinUsb_GetDescriptor alike:
    //   request 255 bytes of string 1 -> 16 bytes, the descriptor
    //   request 256 bytes of string 1 ->  0 bytes, SUCCESS, no error
    // A descriptor's bLength is one byte, so 256 is one past every legal
    // answer and the device's control endpoint simply returns nothing. That
    // silent zero is what hid a Blog V4 from this driver in the field while
    // this fake reported it recognised - see Rtl2832u::stringDescriptor.
    static constexpr std::size_t kDescriptorRequestCeiling = 255;
    bool isGetDescriptor(std::uint8_t requestType, std::uint8_t request) const {
        return requestType == 0x80 && request == 0x06;
    }

    int controlIn(std::uint8_t requestType, std::uint8_t request, std::uint16_t value,
                  std::uint16_t index, std::uint8_t* data, std::size_t len, unsigned) override {
        ++controlCalls;
        const bool descriptor = isGetDescriptor(requestType, request);
        if (descriptor && len > kDescriptorRequestCeiling) {
            // The device's answer, byte for byte: nothing written, nothing
            // moved, no error.
            FakeControl c;
            c.out = false;
            c.requestType = requestType;
            c.request = request;
            c.value = value;
            c.index = index;
            controls.push_back(std::move(c));
            return 0;
        }
        const auto it = inAnswers.find(InKey{request, value, index});
        std::size_t moved = len;
        if (it == inAnswers.end()) {
            ++unscriptedReads;
            for (std::size_t i = 0; i < len; ++i) { data[i] = defaultInByte; }
            // An unscripted DESCRIPTOR read is a descriptor the device does
            // not have: a stall, which the transport reports as a failure.
            if (descriptor) { moved = 0; }
        } else {
            for (std::size_t i = 0; i < len; ++i) {
                data[i] = (i < it->second.size()) ? it->second[i] : defaultInByte;
            }
            // THE COUNT IS THE ACTUAL COUNT (usb_device.hpp's own contract) -
            // for descriptors, where a driver decides what to believe from it.
            if (descriptor && it->second.size() < len) { moved = it->second.size(); }
        }
        if (descriptor && moved != len) {
            FakeControl c;
            c.out = false;
            c.requestType = requestType;
            c.request = request;
            c.value = value;
            c.index = index;
            c.data.assign(data, data + moved);
            controls.push_back(std::move(c));
            if (failControlAfter >= 0 && controlCalls > failControlAfter) {
                lastError_ = "fake: control transfer failed";
                return -1;
            }
            return static_cast<int>(moved);
        }
        FakeControl c;
        c.out = false;
        c.requestType = requestType;
        c.request = request;
        c.value = value;
        c.index = index;
        c.data.assign(data, data + len);
        controls.push_back(std::move(c));
        if (failControlAfter >= 0 && controlCalls > failControlAfter) {
            lastError_ = "fake: control transfer failed";
            return -1;
        }
        return static_cast<int>(len);
    }

    bool beginBulkStream(std::uint8_t endpoint, std::size_t bufferBytes,
                         std::size_t bufferCount) override {
        // The real transport's own rule, enforced here too: a test that
        // hands the driver an illegal buffer size must fail the same way a
        // dongle would rather than passing against the fake.
        if (bufferBytes == 0 || (bufferBytes % 512) != 0 || bufferCount == 0) {
            lastError_ = "fake: illegal bulk ring";
            return false;
        }
        endpoint_ = endpoint;
        bufferBytes_ = bufferBytes;
        streaming_ = true;
        ++streamStarts;
        return true;
    }

    int readBulk(std::uint8_t* dst, std::size_t cap, unsigned timeoutMs) override {
        if (!streaming_) { return -1; }
        ++bulkReads;
        if (failBulkAfter >= 0 && bulkReads > failBulkAfter) {
            lastError_ = "fake: the device is gone";
            return -1;
        }
        // A REAL EMPTY READ COSTS TIME, and a fake that returns 0 instantly
        // turns a driver's perfectly correct retry loop into a hot spin that
        // only exists in tests. Capped well under the caller's timeout so a
        // test never waits on this.
        if (bulkQueue.empty()) {
            const unsigned nap = timeoutMs < 5u ? timeoutMs : 5u;
            if (nap > 0) { std::this_thread::sleep_for(std::chrono::milliseconds(nap)); }
            return 0;
        }
        const std::vector<std::uint8_t> payload = bulkQueue.front();
        bulkQueue.pop_front();
        if (payload.empty()) {
            const unsigned nap = timeoutMs < 5u ? timeoutMs : 5u;
            if (nap > 0) { std::this_thread::sleep_for(std::chrono::milliseconds(nap)); }
            return 0;
        }
        const std::size_t n = payload.size() < cap ? payload.size() : cap;
        for (std::size_t i = 0; i < n; ++i) { dst[i] = payload[i]; }
        return static_cast<int>(n);
    }

    void endBulkStream() override {
        if (streaming_) { ++streamStops; }
        streaming_ = false;
    }

    bool streaming() const override { return streaming_; }

    bool resetPipe(std::uint8_t) override {
        ++pipeResets;
        return true;
    }

    const std::string& path() const override { return path_; }
    const std::string& lastError() const override { return lastError_; }

    // Observable bookkeeping a test can assert on.
    int streamStarts = 0;
    int streamStops = 0;
    int pipeResets = 0;
    std::uint8_t endpoint() const { return endpoint_; }
    std::size_t bufferBytes() const { return bufferBytes_; }

private:
    std::string path_ = "fake://usb";
    std::string lastError_;
    bool streaming_ = false;
    std::uint8_t endpoint_ = 0;
    std::size_t bufferBytes_ = 0;
};

}  // namespace cascade::usb
