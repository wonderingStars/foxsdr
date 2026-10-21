// hydrasdr_protocol.hpp - what the HydraSDR RFOne does DIFFERENTLY from the
// Airspy R2 / Mini, and nothing else. Numbers only; nothing in it can touch a
// device.
//
// WHY THIS IS SO SHORT. The RFOne is an Airspy R2 in every way the wire cares
// about: the same LPC4370 microcontroller, the same twelve-bit ADC, the same
// bulk endpoint, the same twenty-eight vendor requests under the same numbers
// (the firmware's hydrasdr_commands.h even keeps libairspy's order), the same
// twelve-bit packing, the same LNA / mixer / VGA register ranges and the same
// two 22-entry combined-gain tables. FoxSDR's Airspy driver therefore drives it
// unchanged, parameterised by the handful of constants below (AirspySource's
// DeviceProfile, src/source/airspy_source.hpp). Everything that is shared lives
// in airspy_protocol.hpp and is not repeated here.
//
// WHERE EVERY NUMBER COMES FROM. The vendor's published sources, and ONLY
// those: the host library (github.com/hydrasdr/rfone_host, tag v1.1.3, commit
// 16942cb, 2026-09-11, libhydrasdr/src/) and the firmware (github.com/hydrasdr/
// rfone_fw, main at 6c7c1fe, 2026-09-11). Each constant names its file and
// line. The firmware is GPL-2.0 and the host library MIT; nothing is
// transcribed from either - what is taken is the wire protocol, which is the
// only way to talk to the device.
//
// NOTHING HERE HAS BEEN TRIED ON A HYDRASDR. There is none on the bench this
// was written on; the proof is tests/test_hydrasdr_source.cpp against a fake
// (tests/hydrasdr_fake_usb.hpp) built from those same sources.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>

#include "source/airspy_protocol.hpp"

namespace cascade::source::hydrasdr {

// --- identity -------------------------------------------------------------

// rfone_fw m0/usb_descriptor.c:29-30 (USB_VENDOR_ID 0x38AF, a usb.org-assigned
// vendor id, USB_PRODUCT_ID 0x0001, "HydraSDR RFOne"), and the host's own
// device registry, libhydrasdr/src/hydrasdr.c:65.
constexpr std::uint16_t kUsbVid = 0x38AF;
constexpr std::uint16_t kUsbPid = 0x0001;

// hydrasdr.c:64 also lists 0x1D50:0x60A1 as "HydraSDR RFOne Legacy VID/PID" -
// the PROTOTYPE id, which is Airspy's own. The host library tells a prototype
// from an Airspy by opening it and reading the firmware version string
// (hydrasdr.c:75-76, :187). This driver does not claim that id, because
// enumeration never opens a device (usb_device.hpp rule 1) and an id the
// Airspy driver already lists cannot be told apart without opening it. A
// prototype is therefore offered as an "Airspy" and is NOT supported here.
constexpr std::uint16_t kLegacyUsbVid = 0x1D50;
constexpr std::uint16_t kLegacyUsbPid = 0x60A1;

// rfone_fw common/core.h:36-37, BOARD_ID_HYDRASDR 1; libhydrasdr hydrasdr.h:277,
// HYDRASDR_BOARD_ID_HYDRASDR_RFONE_OFFICIAL = 1. (An Airspy answers 0.)
constexpr std::uint8_t kBoardId = 1;

// hydrasdr.c:75-76, HYDRASDR_EXPECTED_FW_PREFIX "HydraSDR RF". The host
// refuses a device whose VERSION_STRING_READ does not start with it
// (hydrasdr.c:187), and so does this driver. The firmware builds the string as
// the hardware name "HydraSDR RFOne" (hydrasdr_rfone_conf.c:55) followed by
// " <git tag> <date>" (rfone_fw m0/m0.c:75, m0/usb_req.c:345-365), padded with
// NULs to a multiple of four.
constexpr const char* kFirmwarePrefix = "HydraSDR RF";

// hydrasdr.c:78-80: the USB serial string is "HYDRASDR SN:" and sixteen hex
// digits, 28 characters (rfone_fw m0/usb_descriptor.c:199-234 fills the sixteen
// from the MCU's 64-bit id). Windows carries it in the instance id with the
// space as an underscore, as it does Airspy's "AIRSPY_SN:" - which is an
// ASSUMPTION about the RFOne, held only by analogy.
constexpr const char* kSerialPrefix = "HYDRASDR SN:";

// --- what differs on the wire ---------------------------------------------

// SET_FREQ's payload is a 64-bit little-endian Hz value: rfone_fw
// m0/usb_req.c:63-67 (set_freq_params_t is one uint64_t) and :528-545 (the
// firmware schedules an OUT block of sizeof it), libhydrasdr
// hydrasdr_shared.c:1388-1415 (TO_LE_64, length sizeof). Airspy's is 32-bit.
constexpr std::size_t kFreqPayloadBytes = 8;

// libhydrasdr hydrasdr_rfone.c:33-34, RFONE_MIN_FREQ_HZ / RFONE_MAX_FREQ_HZ.
// (The host's own set_freq refuses only 0 and above 10 GHz,
// hydrasdr_shared.c:1399; these are the device's published range.) The
// tuner is an R828D (hydrasdr_rfone.c:416); the firmware's frequency table
// ends at "650 to 1800 MHz" (rfone_fw common/r82x.c:233, :253).
constexpr double kMinFrequencyHz = 24.0e6;
constexpr double kMaxFrequencyHz = 1.8e9;

// THE BIAS TEE IS REQUEST 20, NOT A GPIO WRITE. libhydrasdr
// hydrasdr_shared.c:2175-2186 sends HYDRASDR_SET_RF_BIAS_CMD (commands.h:48) as
// an OUT with value 0, the state in the INDEX word and no payload; the
// firmware (m0/usb_req.c:654-670) switches the bias tee on for index 1 and off
// for anything else (common/core.c:818-826, GPIO1[13] - the same pin libairspy
// reaches through GPIO_WRITE). libairspy, and so this project's Airspy driver,
// use the GPIO write; the HydraSDR host does not, and this follows the HydraSDR
// host. 4.5 V, 300 mA, on the ANT port only (hydrasdr_rfone.c:41-45, :440-443).
constexpr airspy::VendorRequest kBiasTeeRequest = airspy::VendorRequest::SetRfBiasCmd;

// THE ONE REQUEST AN AIRSPY DOES NOT HAVE: SET_RF_PORT, number 28
// (libhydrasdr hydrasdr_commands.h:56, firmware common/hydrasdr_commands.h:70,
// HYDRASDR_CMD_MAX). An IN, value 0, the port in the INDEX word, one byte
// answered - 1 for success - and a STALL for a port the firmware does not know
// (hydrasdr_shared.c:2221-2242, firmware m0/usb_req.c:897-913). Ports 0, 1 and
// 2 (firmware common/hydrasdr_commands.h:121-126).
constexpr airspy::VendorRequest kSetRfPortRequest = static_cast<airspy::VendorRequest>(28);

// libhydrasdr hydrasdr_rfone.c:431-456: three receive ports, named "ANT" (the
// SMA, with the bias tee), "CABLE1" and "CABLE2" (no bias tee). The firmware
// calls the same three RX0, RX1, RX2 and R828D input switches (r82x.c:752-800).
constexpr std::size_t kPortCount = 3;
constexpr const char* kPortNames[kPortCount] = {"ANT", "CABLE1", "CABLE2"};

// --- what is deliberately NOT sent ----------------------------------------

// libhydrasdr 1.1.0 and later send five requests the published firmware does
// not implement - GET_CAPABILITIES 29, SET_BANDWIDTH 30, GET_BANDWIDTHS 31,
// GET_TEMPERATURE 32, SET_GAIN 33 (hydrasdr_commands.h:58-62) - and fall back
// when the device stalls them (hydrasdr_shared.c:2404-2411). The firmware's
// dispatcher stalls anything above HYDRASDR_CMD_MAX = 28
// (m0/usb_req.c:977-993). This driver sends none of them: a stall on a control
// request is, in the Airspy driver's transport rules, a device that has gone
// (AirspySource::noteTransportFault), and the only thing they would buy is
// information about firmware that has not been published. See the report for
// what that leaves unverified: a firmware newer than the public source that
// advertises extended sample-rate information would be read as the 12-bit
// packed stream this driver always asks for.
constexpr std::uint8_t kFirstUnpublishedRequest = 29;

}  // namespace cascade::source::hydrasdr
