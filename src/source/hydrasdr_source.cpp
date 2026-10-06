// The HydraSDR RFOne's profile and enumeration. The driver itself is
// airspy_source.cpp; see hydrasdr_source.hpp for why there is nothing more.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/hydrasdr_source.hpp"

#include <cctype>

namespace cascade::source {

namespace {

std::string upperAscii(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
    return s;
}

}  // namespace

std::string hydraSdrModelFrom(const std::string& text) {
    // The product string is "HydraSDR RFOne" (rfone_fw m0/usb_descriptor.c:
    // 179-197) and the firmware version string begins with the same words
    // (usb_req.c:355). "HYDRASDR" is enough to know it is one of theirs; there
    // is only the RFOne to be.
    if (upperAscii(text).find("HYDRASDR") != std::string::npos) { return "HydraSDR RFOne"; }
    return "HydraSDR";
}

const DeviceProfile& hydraSdrProfile() {
    static const DeviceProfile p = {
        /*driverKey*/ "hydrasdr",
        /*productName*/ "HydraSDR",
        /*genericModel*/ "HydraSDR",
        // The label already says "HydraSDR RFOne", so the name does not repeat
        // it the way the Airspy's "Airspy: Airspy R2" does.
        /*namePrefix*/ "",
        /*noDeviceName*/ "HydraSDR: (no device)",
        /*usbVid*/ hydrasdr::kUsbVid,
        /*usbPid*/ hydrasdr::kUsbPid,
        /*freqPayloadBytes*/ hydrasdr::kFreqPayloadBytes,
        /*minFrequencyHz*/ hydrasdr::kMinFrequencyHz,
        /*maxFrequencyHz*/ hydrasdr::kMaxFrequencyHz,
        /*biasTee*/ BiasTeeVia::RfBiasRequest,
        /*ports*/ hydrasdr::kPortNames,
        /*portCount*/ hydrasdr::kPortCount,
        /*firmwarePrefix*/ hydrasdr::kFirmwarePrefix,
        // No fallback: the vendor's host has none.
        /*fallbackRatesHz*/ nullptr,
        /*fallbackRateCount*/ 0,
        /*modelFrom*/ &hydraSdrModelFrom,
    };
    return p;
}

std::vector<cascade::usb::UsbId> hydraSdrUsbIds() {
    return {{hydrasdr::kUsbVid, hydrasdr::kUsbPid}};
}

std::vector<NativeDeviceInfo> hydraSdrDevicesFrom(
    const std::vector<cascade::usb::UsbDeviceInfo>& devices) {
    return devicesFromProfile(hydraSdrProfile(), devices);
}

std::vector<NativeDeviceInfo> enumerateHydraSdr() {
    // enumerateWinUsb() is the one entry point on every platform (WinUSB on
    // Windows, usbfs on Linux), as the Airspy's is.
    return hydraSdrDevicesFrom(cascade::usb::enumerateWinUsb(hydraSdrUsbIds()));
}

}  // namespace cascade::source
