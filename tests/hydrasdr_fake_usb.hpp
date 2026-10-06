// hydrasdr_fake_usb.hpp - a HydraSDR RFOne that lives in the test process.
//
// THERE IS NO HYDRASDR ON THIS BENCH, and nobody who wrote this driver has
// held one. So this answers as the vendor's PUBLISHED FIRMWARE does
// (github.com/hydrasdr/rfone_fw, main at 6c7c1fe, m0/usb_req.c), request by
// request, and records every control transfer the driver sends. What it proves
// is that the driver sends what that firmware's source accepts; it cannot prove
// the firmware on a real unit is that source. Each behaviour below names the
// firmware line it models.
//
// It is a sibling of FakeAirspyUsb and derives from it, deliberately: the RFOne
// is an Airspy R2 in every way the bulk side cares about (the same endpoint, the
// same packed stream, the same start sequence), so the bulk queue, the halted-
// pipe model, the injected failures and the transcript are the Airspy fake's
// own and are not written a second time. What this overrides is only where the
// RFOne's firmware answers differently:
//
//  - BOARD_ID_READ answers 1 (common/core.h:36-37), not 0.
//  - VERSION_STRING_READ answers "HydraSDR RFOne <tag> <date>", NUL-padded to a
//    multiple of four (usb_req.c:345-365).
//  - GET_SAMPLERATES lists the firmware's three configurations,
//    10, 5 and 2.5 MS/s (common/hydrasdr_rfone_conf.c:161-226).
//  - SET_SAMPLERATE stalls an index the firmware has no configuration for
//    (usb_req.c:460-471).
//  - SET_FREQ takes an OUT block of EXACTLY sizeof(uint64_t) (usb_req.c:63-67,
//    :528-545). A different length is modelled as a stall: the firmware has
//    scheduled a read of eight bytes and a four-byte transfer leaves it waiting
//    for the rest - the one assumption in this fake about what a real unit does
//    with a short payload, and the reason an Airspy-width SET_FREQ is a RED
//    test rather than a quiet success.
//  - SET_PACKING stalls a state above 2 (usb_req.c:417-419).
//  - SET_RF_BIAS_CMD (request 20) switches the bias tee on for index 1 and off
//    for anything else (usb_req.c:654-670); GPIO_WRITE (21) is NOT the bias tee
//    here, whatever pin it reaches.
//  - SET_RF_PORT (request 28) answers one byte, 1, for ports 0..2 and stalls the
//    rest (usb_req.c:897-913).
//  - Any request above HYDRASDR_CMD_MAX = 28 is stalled (usb_req.c:977-993),
//    which is what the unpublished requests 29..33 meet.
//
// A "stall" is a failed control transfer: -1 and a lastError(), which is what a
// WinUSB control pipe reports.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "airspy_fake_usb.hpp"

namespace cascade::test {

class FakeHydraSdrUsb : public FakeAirspyUsb {
public:
    explicit FakeHydraSdrUsb(std::string devicePath = "\\\\?\\usb#vid_38af&pid_0001#fake")
        : FakeAirspyUsb(std::move(devicePath)) {
        boardId = 1;
        // "<hardware name> <git tag> <date>": the tag and the date here are
        // made up, only the shape is the firmware's.
        firmwareVersion = "HydraSDR RFOne v1.0.0 2025-01-01";
        sampleRates = {10000000u, 5000000u, 2500000u};
    }

    // --- what the firmware did, for the test to read -----------------------
    std::atomic<std::uint64_t> lastFreqHz{0};
    std::atomic<int> freqSets{0};
    std::atomic<int> rfPort{0};
    std::atomic<int> portSets{0};
    std::atomic<bool> biasTee{false};
    std::atomic<int> packingState{-1};
    std::atomic<int> sampleRateConf{-1};
    // Every request number the firmware stalled, in order.
    std::vector<int> stalled() const {
        std::lock_guard<std::mutex> lk(stalledMutex_);
        return stalled_;
    }

    // When set, SET_RF_PORT stalls whatever the port - a firmware that has lost
    // the R828D's I2C bus.
    std::atomic<bool> refusePorts{false};

    int controlOut(std::uint8_t requestType, std::uint8_t request, std::uint16_t value,
                   std::uint16_t index, const std::uint8_t* data, std::size_t len,
                   unsigned timeoutMs) override {
        if (request == 13) {  // HYDRASDR_SET_FREQ
            const int ret = FakeAirspyUsb::controlOut(requestType, request, value, index, data,
                                                      len, timeoutMs);
            if (ret < 0) { return ret; }
            if (len != sizeof(std::uint64_t) || data == nullptr) {
                return stall(request, "SET_FREQ takes an eight-byte frequency");
            }
            std::uint64_t hz = 0;
            for (int b = 7; b >= 0; --b) { hz = (hz << 8) | data[b]; }
            lastFreqHz.store(hz);
            freqSets.fetch_add(1);
            return ret;
        }
        if (request > 28) {
            const int ret = FakeAirspyUsb::controlOut(requestType, request, value, index, data,
                                                      len, timeoutMs);
            (void)ret;
            return stall(request, "a request above HYDRASDR_CMD_MAX");
        }
        const int ret =
            FakeAirspyUsb::controlOut(requestType, request, value, index, data, len, timeoutMs);
        if (ret >= 0 && request == 20) { biasTee.store(index == 1); }  // SET_RF_BIAS_CMD
        return ret;
    }

    int controlIn(std::uint8_t requestType, std::uint8_t request, std::uint16_t value,
                  std::uint16_t index, std::uint8_t* data, std::size_t len,
                  unsigned timeoutMs) override {
        if (request > 28) {
            AirspyControlRecord rec;
            rec.in = true;
            rec.requestType = requestType;
            rec.request = request;
            rec.value = value;
            rec.index = index;
            rec.timeoutMs = timeoutMs;
            transcript()->push(rec);
            return stall(request, "a request above HYDRASDR_CMD_MAX");
        }
        if (request == 28) {  // HYDRASDR_SET_RF_PORT
            AirspyControlRecord rec;
            rec.in = true;
            rec.requestType = requestType;
            rec.request = request;
            rec.value = value;
            rec.index = index;
            rec.timeoutMs = timeoutMs;
            if (refusePorts.load() || index > 2 || len < 1 || data == nullptr) {
                transcript()->push(rec);
                return stall(request, "an RF port the firmware does not have");
            }
            data[0] = 1;
            rec.data.assign(data, data + 1);
            transcript()->push(rec);
            rfPort.store(index);
            portSets.fetch_add(1);
            return 1;
        }
        if (request == 26 && index > 2) {  // HYDRASDR_SET_PACKING
            AirspyControlRecord rec;
            rec.in = true;
            rec.requestType = requestType;
            rec.request = request;
            rec.value = value;
            rec.index = index;
            rec.timeoutMs = timeoutMs;
            transcript()->push(rec);
            return stall(request, "a packing state above 2");
        }
        if (request == 12) {  // HYDRASDR_SET_SAMPLERATE, by configuration index
            if (index < 64 && index >= sampleRates.size()) {
                AirspyControlRecord rec;
                rec.in = true;
                rec.requestType = requestType;
                rec.request = request;
                rec.value = value;
                rec.index = index;
                rec.timeoutMs = timeoutMs;
                transcript()->push(rec);
                return stall(request, "a sample-rate configuration the firmware does not have");
            }
        }
        if (request == 10) {  // HYDRASDR_VERSION_STRING_READ: NUL-padded to four
            std::string s = firmwareVersion;
            s.push_back('\0');
            while (s.size() % 4 != 0) { s.push_back('\0'); }
            const std::size_t n = std::min(len, s.size());
            if (data != nullptr && n > 0) { std::memcpy(data, s.data(), n); }
            AirspyControlRecord rec;
            rec.in = true;
            rec.requestType = requestType;
            rec.request = request;
            rec.value = value;
            rec.index = index;
            rec.timeoutMs = timeoutMs;
            rec.data.assign(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
            transcript()->push(rec);
            return static_cast<int>(n);
        }
        const int ret =
            FakeAirspyUsb::controlIn(requestType, request, value, index, data, len, timeoutMs);
        if (ret >= 0) {
            if (request == 26) { packingState.store(static_cast<int>(index)); }
            if (request == 12) { sampleRateConf.store(static_cast<int>(index)); }
        }
        return ret;
    }

private:
    int stall(std::uint8_t request, const char* why) {
        {
            std::lock_guard<std::mutex> lk(stalledMutex_);
            stalled_.push_back(request);
        }
        setLastError(std::string("fake: the firmware stalled request ") + std::to_string(request) +
                     " (" + why + ")");
        return -1;
    }

    mutable std::mutex stalledMutex_;
    std::vector<int> stalled_;
};

}  // namespace cascade::test
