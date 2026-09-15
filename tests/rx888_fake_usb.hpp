// rx888_fake_usb.hpp - an RX888 that lives in the test process, in both of
// the identities a real one has.
//
// THERE IS NO RX888 ON THIS BENCH, and there was none while the driver was
// written. That is not a gap to apologise for, it is the reason
// cascade::usb::UsbDevice is an interface at all: the only claim a driver can
// make without hardware is "these are the bytes I send", and the only way to
// prove it is to put something on the other end of the wire that records
// them.
//
// So this records EVERY control transfer - direction, request type, request
// number, value, index, payload, timeout - in order, and answers the one read
// the SDDC firmware answers (TESTFX3's four bytes of model and version). The
// bulk side is a queue of scripted buffers, so a test can hand the reader a
// known ramp and check that every sample of it comes out of read() once, in
// order.
//
// WHAT IT HAS THAT THE HACKRF'S FAKE DOES NOT, and why this file exists
// beside that one rather than extending it: AN RX888 IS TWO DEVICES. Out of a
// power cycle it is a Cypress FX3 bootloader (04B4:00F3) that implements
// exactly one vendor request - 0xA0, "write this block of RAM" - and after
// the host has written a firmware image and jumped to it, it disappears and
// comes back as a different USB device (04B4:00F1) that speaks the SDDC
// protocol. No other driver in this project has an open() that changes which
// device it is talking to half way through, so no other fake has to be able
// to BE both, to refuse the wrong requests in each, and to hand the test a
// device LIST that changes when the jump happens. FakeRx888Bus below is that
// list.
//
// It also has to be able to MISBEHAVE, because the interesting half of a
// driver is what it does when the radio stops: a device that vanishes
// mid-stream (readBulk answers negative), a pipe that never completes (the
// bounded join's whole reason for existing), a bootloader that refuses the
// image, and a control request that fails. Each of those is one flag here.
//
// Everything is guarded, like the HackRF's fake and unlike the shared
// src/usb/usb_fake.hpp: Rx888Source's reader thread calls readBulk()
// concurrently with control transfers on the GUI thread, deliberately, so a
// transcript in a plain std::vector would be a data race in the harness.
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
struct Rx888ControlRecord {
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

class FakeRx888Usb : public cascade::usb::UsbDevice {
public:
    enum class Identity {
        Bootloader,  // 04B4:00F3, only request 0xA0 exists
        Streamer,    // 04B4:00F1, the SDDC protocol
    };

    explicit FakeRx888Usb(Identity id, std::string devicePath)
        : identity_(id), path_(std::move(devicePath)) {}

    // --- what the firmware answers ---------------------------------------
    // TESTFX3's four bytes: model, firmware high, firmware low, and the
    // firmware's own count of vendor requests served.
    std::uint8_t model = 0x04;  // RX888r2
    std::uint16_t firmware = 0x0202;
    std::atomic<std::uint8_t> requestCount{0};

    // --- injected failures -------------------------------------------------
    // Control requests that fail outright, as a device that has been
    // unplugged between two transfers does.
    std::vector<std::uint8_t> failingRequests;
    // The bootloader refuses any write whose destination address is at or
    // past this. Zero means it refuses nothing.
    std::atomic<std::uint32_t> refuseWritesAtOrAbove{0};

    // What readBulk does once the scripted queue is empty.
    enum class Exhausted {
        Timeout,     // the honest answer of a radio that is simply slow: 0
        DeviceGone,  // the pipe has failed: negative, lastError() set
        Block,       // never completes, until releaseBlock is set
    };
    std::atomic<Exhausted> onExhausted{Exhausted::Timeout};
    std::atomic<bool> releaseBlock{false};

    // --- what the bootloader was given ------------------------------------
    // Set by the zero-length 0xA0 that transfers execution; the entry address
    // it carried is what a test compares against the image's.
    std::atomic<bool> jumped{false};
    std::atomic<std::uint32_t> jumpAddress{0};

    Identity identity() const { return identity_; }

    // --- scripted bulk data ------------------------------------------------
    void queueBulk(std::vector<std::uint8_t> buf) {
        std::lock_guard<std::mutex> lk(mutex_);
        bulk_.push_back(std::move(buf));
    }

    // --- what the driver did -----------------------------------------------
    std::vector<Rx888ControlRecord> controls() const {
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

    // Every byte the bootloader was told to write, in order, with the address
    // each block went to. Kept separately from the transcript so a test can
    // reassemble the image the driver actually sent and compare it with the
    // one it was supposed to send, which is a different question from "were
    // the transfers shaped right".
    struct BootWrite {
        std::uint32_t address = 0;
        std::vector<std::uint8_t> data;
    };
    std::vector<BootWrite> bootWrites() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return bootWrites_;
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
        Rx888ControlRecord rec;
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

        if (identity_ == Identity::Bootloader) {
            // A BOOTLOADER KNOWS ONE REQUEST. Anything else is answered the
            // way a real one answers an unimplemented vendor request - a
            // stall, which the transport reports as a failure - so a driver
            // that sent an SDDC command to a bootloader would fail here
            // rather than quietly appearing to work.
            if (request != 0xA0) {
                lastError_ = "fake: the FX3 bootloader stalled an unknown vendor request";
                return -1;
            }
            const std::uint32_t address = (static_cast<std::uint32_t>(index) << 16) | value;
            if (len == 0) {
                jumpAddress.store(address);
                jumped.store(true);
                return 0;
            }
            const std::uint32_t refuse = refuseWritesAtOrAbove.load();
            if (refuse != 0 && address >= refuse) {
                lastError_ = "fake: the bootloader refused that address";
                return -1;
            }
            {
                std::lock_guard<std::mutex> lk(mutex_);
                BootWrite w;
                w.address = address;
                w.data.assign(data, data + len);
                bootWrites_.push_back(std::move(w));
            }
            return static_cast<int>(len);
        }

        requestCount.fetch_add(1);
        return static_cast<int>(len);
    }

    int controlIn(std::uint8_t requestType, std::uint8_t request, std::uint16_t value,
                  std::uint16_t index, std::uint8_t* data, std::size_t len,
                  unsigned timeoutMs) override {
        Rx888ControlRecord rec;
        rec.in = true;
        rec.requestType = requestType;
        rec.request = request;
        rec.value = value;
        rec.index = index;
        rec.timeoutMs = timeoutMs;

        if (fails(request) || identity_ == Identity::Bootloader) {
            {
                std::lock_guard<std::mutex> lk(mutex_);
                controls_.push_back(rec);
            }
            lastError_ = "fake: the device is gone";
            return -1;
        }

        int answered = 0;
        if (request == 0xAC) {  // TESTFX3
            // SDDC_FX3/USBhandler.c:496-503, in that order.
            const std::uint8_t reply[4] = {model, static_cast<std::uint8_t>(firmware >> 8),
                                           static_cast<std::uint8_t>(firmware & 0xFF),
                                           requestCount.load()};
            const std::size_t n = std::min<std::size_t>(len, sizeof(reply));
            if (data != nullptr && n > 0) { std::memcpy(data, reply, n); }
            answered = static_cast<int>(n);
            requestCount.fetch_add(1);
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
                // Never completes until the test lets it. Sliced rather than
                // a single long sleep so the test can end it promptly once it
                // has proved the bounded join did not wait for it.
                while (!releaseBlock.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                return 0;
            }
            case Exhausted::Timeout:
            default: {
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

    Identity identity_;
    mutable std::mutex mutex_;
    std::vector<Rx888ControlRecord> controls_;
    std::vector<BootWrite> bootWrites_;
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

// THE BUS, which is the half of this file the other fakes do not need. It
// owns the devices, answers the driver's enumeration, and - this is the
// point - CHANGES WHAT IT LISTS when the bootloader is jumped, exactly as a
// real one does a second after the firmware lands. Nothing else in the suite
// can express "the device you were talking to is gone and a different one is
// here now", which is the one behaviour the firmware path exists to have.
class FakeRx888Bus {
public:
    static constexpr std::uint16_t kVid = 0x04B4;
    static constexpr std::uint16_t kPidBootloader = 0x00F3;
    static constexpr std::uint16_t kPidStreamer = 0x00F1;

    // A radio already running its firmware.
    void addStreamer(const std::string& serial) {
        Entry e;
        e.info.vid = kVid;
        e.info.pid = kPidStreamer;
        e.info.serial = serial;
        e.info.description = "RX888mk2";
        e.info.path = "\\\\?\\usb#vid_04b4&pid_00f1#" + serial;
        e.device = std::make_shared<FakeRx888Usb>(FakeRx888Usb::Identity::Streamer, e.info.path);
        entries_.push_back(std::move(e));
    }

    // A radio that has just been plugged in: a bootloader, plus the streamer
    // it BECOMES once the image is loaded and jumped. Only one of the two is
    // ever listed at a time.
    void addBootloader(const std::string& bootSerial, const std::string& runSerial) {
        Entry boot;
        boot.info.vid = kVid;
        boot.info.pid = kPidBootloader;
        boot.info.serial = bootSerial;
        boot.info.description = "WestBridge";
        boot.info.path = "\\\\?\\usb#vid_04b4&pid_00f3#" + bootSerial;
        boot.device =
            std::make_shared<FakeRx888Usb>(FakeRx888Usb::Identity::Bootloader, boot.info.path);

        Entry run;
        run.info.vid = kVid;
        run.info.pid = kPidStreamer;
        run.info.serial = runSerial;
        run.info.description = "RX888mk2";
        run.info.path = "\\\\?\\usb#vid_04b4&pid_00f1#" + runSerial;
        run.device = std::make_shared<FakeRx888Usb>(FakeRx888Usb::Identity::Streamer, run.info.path);
        // Hidden until the jump.
        run.becomesVisibleOnJumpOf = boot.device;
        boot.hiddenAfterOwnJump = true;

        entries_.push_back(std::move(boot));
        entries_.push_back(std::move(run));
    }

    // A bootloader that takes the image, jumps, and is never seen again -
    // which is what an RX888 looks like when WinUSB has been bound to the
    // bootloader's USB identity and not to the running radio's.
    void addBootloaderWithNoRadioBehindIt(const std::string& bootSerial) {
        Entry boot;
        boot.info.vid = kVid;
        boot.info.pid = kPidBootloader;
        boot.info.serial = bootSerial;
        boot.info.description = "WestBridge";
        boot.info.path = "\\\\?\\usb#vid_04b4&pid_00f3#" + bootSerial;
        boot.device =
            std::make_shared<FakeRx888Usb>(FakeRx888Usb::Identity::Bootloader, boot.info.path);
        boot.hiddenAfterOwnJump = true;
        entries_.push_back(std::move(boot));
    }

    // A device on the bus that is not one of ours, to prove the driver's
    // filter is a filter.
    void addStranger(std::uint16_t vid, std::uint16_t pid) {
        Entry e;
        e.info.vid = vid;
        e.info.pid = pid;
        e.info.serial = "stranger";
        e.info.path = "\\\\?\\usb#stranger";
        entries_.push_back(std::move(e));
    }

    // What the driver's enumeration sees, right now.
    std::vector<cascade::usb::UsbDeviceInfo> list() const {
        std::vector<cascade::usb::UsbDeviceInfo> out;
        for (const Entry& e : entries_) {
            if (e.hiddenAfterOwnJump && e.device && e.device->jumped.load()) { continue; }
            if (e.becomesVisibleOnJumpOf && !e.becomesVisibleOnJumpOf->jumped.load()) { continue; }
            out.push_back(e.info);
        }
        return out;
    }

    // What the driver's opener gets for a path.
    std::unique_ptr<cascade::usb::UsbDevice> open(const std::string& path, std::string& error) {
        for (const Entry& e : entries_) {
            if (e.info.path == path && e.device) {
                opened_.push_back(e.device);
                return std::make_unique<Handle>(e.device);
            }
        }
        error = "fake: no such device";
        return nullptr;
    }

    // The fake behind a path, for a test that wants to script or inspect it.
    // Shared, so a test keeps its inspection handle after the driver has
    // closed and destroyed its own.
    std::shared_ptr<FakeRx888Usb> deviceFor(std::uint16_t pid) const {
        for (const Entry& e : entries_) {
            if (e.info.pid == pid && e.device) { return e.device; }
        }
        return nullptr;
    }

    std::size_t openCount() const { return opened_.size(); }

private:
    // The driver owns a unique_ptr to a UsbDevice and destroys it at close;
    // the test wants to keep reading the transcript afterwards. So what the
    // driver gets is a thin forwarding handle and the fake itself outlives
    // it - which is also how the abandonment path is safe to exercise here:
    // a leaked handle refers to an object the bus still owns.
    class Handle : public cascade::usb::UsbDevice {
    public:
        explicit Handle(std::shared_ptr<FakeRx888Usb> d) : d_(std::move(d)) {}

        int controlOut(std::uint8_t rt, std::uint8_t r, std::uint16_t v, std::uint16_t i,
                       const std::uint8_t* data, std::size_t len, unsigned t) override {
            return d_->controlOut(rt, r, v, i, data, len, t);
        }
        int controlIn(std::uint8_t rt, std::uint8_t r, std::uint16_t v, std::uint16_t i,
                      std::uint8_t* data, std::size_t len, unsigned t) override {
            return d_->controlIn(rt, r, v, i, data, len, t);
        }
        bool beginBulkStream(std::uint8_t ep, std::size_t bytes, std::size_t count) override {
            return d_->beginBulkStream(ep, bytes, count);
        }
        int readBulk(std::uint8_t* dst, std::size_t cap, unsigned t) override {
            return d_->readBulk(dst, cap, t);
        }
        void endBulkStream() override { d_->endBulkStream(); }
        bool streaming() const override { return d_->streaming(); }
        bool resetPipe(std::uint8_t ep) override { return d_->resetPipe(ep); }
        const std::string& path() const override { return d_->path(); }
        const std::string& lastError() const override { return d_->lastError(); }

    private:
        std::shared_ptr<FakeRx888Usb> d_;
    };

    struct Entry {
        cascade::usb::UsbDeviceInfo info;
        std::shared_ptr<FakeRx888Usb> device;
        bool hiddenAfterOwnJump = false;
        std::shared_ptr<FakeRx888Usb> becomesVisibleOnJumpOf;
    };

    std::vector<Entry> entries_;
    std::vector<std::shared_ptr<FakeRx888Usb>> opened_;
};

}  // namespace cascade::test
