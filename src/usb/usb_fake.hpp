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
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "usb/usb_device.hpp"

namespace cascade::usb {

// One isochronous packet as the fake's "device" sends it: `payload` is what
// arrives (its size is the packet's ACTUAL length - 0, short or full), and a
// nonzero `status` is a packet the host controller reported as failed.
struct FakeIsoPacket {
    std::vector<std::uint8_t> payload;
    int status = 0;
};

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
    // THE JOURNAL: one line per transport event, in the order the driver
    // caused them - "control", "bulk-out 02 [5A A5 00 02 41 53]",
    // "iso-begin 86", "iso-end", "closed" - so a test can assert ORDER across
    // kinds of traffic (START only after the iso transfers are armed, STOP
    // before they are cancelled, the interface released last). Shared, so it
    // outlives the device and still holds "closed" after the driver has
    // destroyed it. Null disables.
    std::shared_ptr<std::vector<std::string>> journal;

    ~FakeUsbDevice() override { note("closed"); }

    void note(const std::string& line) {
        if (!journal) { return; }
        std::lock_guard<std::mutex> lk(journalMutex_);
        journal->push_back(line);
    }

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

    // ANSWERS THAT CHANGE FROM ONE READ TO THE NEXT, for a device that reads
    // the same (request, value, index) repeatedly and expects a different
    // byte each time - the RTL2832U's EEPROM is the case: its address pointer
    // auto-increments, so eight identical single-byte reads return eight
    // different bytes. Consumed front first; once a key's queue is empty the
    // plain answerIn() script (or defaultInByte) answers it again.
    std::map<InKey, std::deque<std::vector<std::uint8_t>>> inQueue;

    void queueIn(std::uint8_t request, std::uint16_t value, std::uint16_t index,
                 std::vector<std::uint8_t> bytes) {
        inQueue[InKey{request, value, index}].push_back(std::move(bytes));
    }

    // Called with every OUT transfer after it is recorded, so a test can make
    // the fake REMEMBER a register a driver writes and answer it back on the
    // next read. Without it every read-modify-write reads the default byte,
    // and a test cannot tell a driver that preserves other bits from one that
    // clobbers them - which is exactly the question two users of one GPIO
    // register raise.
    std::function<void(const FakeControl&)> onControlOut;

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

    // After this many beginBulkStream() calls, every later one fails and
    // leaves the pipe closed, as the WinUSB transport does when it cannot
    // queue its transfers. -1 disables. This is how "the stream would not
    // come back after a restart" is staged.
    int failBeginBulkAfter = -1;
    int beginBulkCalls = 0;

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
        controls.push_back(c);
        note("control " + c.text());
        if (failControlAfter >= 0 && controlCalls > failControlAfter) {
            lastError_ = "fake: control transfer failed";
            return -1;
        }
        // Only a transfer the device ACCEPTED reaches the register model: a
        // refused write must not change what the next read answers.
        if (onControlOut) { onControlOut(c); }
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
        const auto q = inQueue.find(InKey{request, value, index});
        if (!descriptor && q != inQueue.end() && !q->second.empty()) {
            const std::vector<std::uint8_t> next = q->second.front();
            q->second.pop_front();
            for (std::size_t i = 0; i < len; ++i) {
                data[i] = (i < next.size()) ? next[i] : defaultInByte;
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
        ++beginBulkCalls;
        if (failBeginBulkAfter >= 0 && beginBulkCalls > failBeginBulkAfter) {
            lastError_ = "fake: the bulk pipe would not start";
            streaming_ = false;
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

    // --- bulk OUT ------------------------------------------------------------

    struct BulkWrite {
        std::uint8_t endpoint = 0;
        std::vector<std::uint8_t> data;
    };
    std::vector<BulkWrite> bulkWrites;
    // Fails every bulk write from this call number on; -1 disables.
    int failBulkWriteAfter = -1;
    int bulkWriteCalls = 0;
    // Called with each ACCEPTED bulk write, so a test can make the fake's
    // "device" react to START (begin streaming) and STOP.
    std::function<void(const BulkWrite&)> onBulkWrite;

    int writeBulk(std::uint8_t endpoint, const std::uint8_t* data, std::size_t len,
                  unsigned) override {
        ++bulkWriteCalls;
        BulkWrite w;
        w.endpoint = endpoint;
        if (data != nullptr && len > 0) { w.data.assign(data, data + len); }
        char head[24];
        std::snprintf(head, sizeof(head), "bulk-out %02X [", endpoint);
        std::string line(head);
        for (std::size_t i = 0; i < w.data.size(); ++i) {
            char b[8];
            std::snprintf(b, sizeof(b), "%s%02X", i ? " " : "", w.data[i]);
            line += b;
        }
        line += "]";
        if (failBulkWriteAfter >= 0 && bulkWriteCalls > failBulkWriteAfter) {
            note(line + " FAILED");
            lastError_ = "fake: bulk write failed";
            return -1;
        }
        note(line);
        bulkWrites.push_back(w);
        if (onBulkWrite) { onBulkWrite(w); }
        return static_cast<int>(len);
    }

    // --- isochronous IN ------------------------------------------------------
    //
    // Transfers the fake's "device" completes, one per readIso(), each a list
    // of packets of chosen lengths. They are laid into a transfer buffer at
    // one slot per packet exactly as usbfs lays them out and handed to the
    // SAME concatIsoPackets() the real transports use, so what a driver gets
    // from here is what it would get from them. An EMPTY transfer (no
    // packets at all) is a readIso() that times out. Guarded by a mutex so a
    // test can feed transfers while the driver's reader thread is reading.

    // The slot size the fake reports as the endpoint's maximum bytes per
    // interval - what a real transport reads from the device's descriptors.
    // 0 means "use what the driver asked for".
    std::size_t isoEndpointSlotBytes = 0;
    // After this many beginIsoStream() calls, every later one fails. -1 disables.
    // This is how "the endpoint is not there" (unprogrammed interface) is staged.
    int failBeginIsoAfter = -1;
    int beginIsoCalls = 0;
    // After this many readIso() calls every later one fails. -1 disables.
    int failIsoAfter = -1;
    int isoReads = 0;
    int isoStarts = 0;
    int isoStops = 0;

    void feedIso(std::vector<FakeIsoPacket> transfer) {
        std::lock_guard<std::mutex> lk(isoMutex_);
        isoQueue_.push_back(std::move(transfer));
    }
    // What a device does on STOP: nothing further arrives.
    void clearIso() {
        std::lock_guard<std::mutex> lk(isoMutex_);
        isoQueue_.clear();
    }
    std::size_t isoQueued() const {
        std::lock_guard<std::mutex> lk(isoMutex_);
        return isoQueue_.size();
    }

    bool beginIsoStream(std::uint8_t endpoint, std::size_t packetBytes,
                        std::size_t packetsPerTransfer, std::size_t transferCount) override {
        ++beginIsoCalls;
        if (packetsPerTransfer == 0 || packetsPerTransfer > 128 || transferCount == 0) {
            lastError_ = "fake: illegal isochronous ring";
            return false;
        }
        if (failBeginIsoAfter >= 0 && beginIsoCalls > failBeginIsoAfter) {
            note("iso-begin FAILED");
            lastError_ = "fake: endpoint is not an isochronous IN endpoint";
            return false;
        }
        isoEndpoint_ = endpoint;
        isoSlot_ = isoEndpointSlotBytes != 0 ? isoEndpointSlotBytes : packetBytes;
        isoPackets_ = packetsPerTransfer;
        isoStreaming_ = true;
        ++isoStarts;
        char line[32];
        std::snprintf(line, sizeof(line), "iso-begin %02X", endpoint);
        note(line);
        return true;
    }

    IsoRead readIso(std::uint8_t* dst, std::size_t cap, unsigned timeoutMs,
                    IsoTransferStats& stats) override {
        stats = IsoTransferStats{};
        if (!isoStreaming_) { return IsoRead::Failed; }
        ++isoReads;
        if (failIsoAfter >= 0 && isoReads > failIsoAfter) {
            lastError_ = "fake: the device is gone";
            return IsoRead::Failed;
        }
        std::vector<FakeIsoPacket> t;
        {
            std::lock_guard<std::mutex> lk(isoMutex_);
            if (!isoQueue_.empty()) {
                t = std::move(isoQueue_.front());
                isoQueue_.pop_front();
            }
        }
        if (t.empty()) {
            // As readBulk: a real empty wait costs time.
            const unsigned nap = timeoutMs < 5u ? timeoutMs : 5u;
            if (nap > 0) { std::this_thread::sleep_for(std::chrono::milliseconds(nap)); }
            return IsoRead::Timeout;
        }
        // The kernel's layout: packet k's slot at k * slot. A payload longer
        // than its slot is the device babbling; the fake reports the length
        // it was given and lets concatIsoPackets refuse it, as a malformed
        // descriptor would be refused from a real transport.
        const std::size_t slot = isoSlot_;
        std::vector<std::uint8_t> buffer(slot * t.size(), 0xEE);
        std::vector<IsoPacket> packets(t.size());
        for (std::size_t k = 0; k < t.size(); ++k) {
            const std::size_t n = t[k].payload.size() < slot ? t[k].payload.size() : slot;
            for (std::size_t b = 0; b < n; ++b) { buffer[k * slot + b] = t[k].payload[b]; }
            packets[k].offset = k * slot;
            packets[k].length = t[k].payload.size();
            packets[k].status = t[k].status;
        }
        concatIsoPackets(buffer.data(), buffer.size(), packets.data(), packets.size(), slot, dst,
                         cap, stats);
        return IsoRead::Completed;
    }

    void endIsoStream() override {
        if (isoStreaming_) {
            ++isoStops;
            note("iso-end");
        }
        isoStreaming_ = false;
    }
    bool isoStreaming() const override { return isoStreaming_; }
    std::size_t isoPacketBytes() const override { return isoSlot_; }
    std::size_t isoTransferBytes() const override { return isoSlot_ * isoPackets_; }
    std::uint8_t isoEndpoint() const { return isoEndpoint_; }

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

    void setPath(std::string p) { path_ = std::move(p); }

private:
    std::string path_ = "fake://usb";
    std::mutex journalMutex_;
    mutable std::mutex isoMutex_;
    std::deque<std::vector<FakeIsoPacket>> isoQueue_;
    std::uint8_t isoEndpoint_ = 0;
    std::size_t isoSlot_ = 0;
    std::size_t isoPackets_ = 0;
    bool isoStreaming_ = false;
    std::string lastError_;
    bool streaming_ = false;
    std::uint8_t endpoint_ = 0;
    std::size_t bufferBytes_ = 0;
};

}  // namespace cascade::usb
