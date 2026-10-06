// hydrasdr_source.hpp - FoxSDR's native HydraSDR RFOne driver: an AirspySource
// given a HydraSDR's profile, and nothing more.
//
// WHY THERE IS NO SECOND DRIVER HERE. The RFOne is an Airspy R2 on the wire - the
// same LPC4370, the same twelve-bit ADC, the same twenty-eight vendor requests
// under the same numbers, the same packed stream, the same gain registers and
// tables - and the vendor's own host library is a fork of libairspy. What
// differs is a handful of constants (hydrasdr_protocol.hpp: the USB ids, the
// width of SET_FREQ's payload, the request that switches the bias tee, the
// tuning range, the three receive ports) and they live in a DeviceProfile
// (airspy_source.hpp). Every thread, ring, rate rule, gain mode, decimation and
// recovery path of the Airspy driver therefore applies to this radio unchanged,
// and so does everything built on top of it: the Source section's gain-mode
// panel, the decimation combo, the per-radio memory, the bias tee box.
//
// ONE DEPENDENCY WORTH SAYING: because HydraSdrSource IS an AirspySource,
// gui::asAirspy() answers for it, and the Airspy panel is drawn. That is the
// intent. The panel's two gain-table tooltips still say "Airspy's linearity
// table" and "Airspy's sensitivity table"; the tables are libairspy's and the
// HydraSDR host carries the same twenty-two rows (tests/test_hydrasdr_source.cpp
// holds the vendor's copy against ours), and no new on-screen string was added.
//
// WHAT IT DOES NOT DO. It does not claim the prototype's USB id (1D50:60A1, an
// Airspy's own) and it does not send the five requests libhydrasdr 1.1.0 added
// (29..33), which the published firmware does not implement. Both are argued in
// hydrasdr_protocol.hpp.
//
// HARDWARE-UNCONFIRMED. Nobody has run this against a HydraSDR; the proof is
// tests/test_hydrasdr_source.cpp against a fake built from the vendor's
// published firmware.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>
#include <vector>

#include "source/airspy_source.hpp"
#include "source/hydrasdr_protocol.hpp"

namespace cascade::source {

// The HydraSDR RFOne's profile (see AirspySource's DeviceProfile).
const DeviceProfile& hydraSdrProfile();

// Every RFOne bound to WinUSB, for the Source section. NEVER OPENS A DEVICE
// (usb_device.hpp rule 1). On a build with no WinUSB-or-usbfs transport it is
// empty, like every native USB driver's.
std::vector<NativeDeviceInfo> enumerateHydraSdr();

// The pure half: which of a list of USB devices are RFOnes, and each one's
// label and args. The args string is what open() takes back.
std::vector<NativeDeviceInfo> hydraSdrDevicesFrom(
    const std::vector<cascade::usb::UsbDeviceInfo>& devices);

// The one id enumerateHydraSdr asks the transport for: 38AF:0001.
std::vector<cascade::usb::UsbId> hydraSdrUsbIds();

// "HydraSDR RFOne" when the text names one, plain "HydraSDR" when it does not.
// There is one product; the only thing a string can fail to say is which.
std::string hydraSdrModelFrom(const std::string& text);

class HydraSdrSource : public AirspySource {
public:
    HydraSdrSource() : AirspySource(hydraSdrProfile()) {}
};

}  // namespace cascade::source
