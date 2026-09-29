// usb_iso.hpp - the parts of ISOCHRONOUS IN streaming that are the same on
// every platform, pulled out as pure functions so that the WinUSB transport,
// the usbfs transport and the test fake all run the SAME code over a
// completed transfer, and so that code can be proven without a device.
//
// WHAT AN ISOCHRONOUS TRANSFER LOOKS LIKE WHEN IT COMES BACK. A transfer is
// a buffer divided into N packet slots, one per (micro)frame, and a table of
// N packet descriptors. Each descriptor says where its slot starts, how many
// bytes the device ACTUALLY sent into it, and whether that packet failed:
//
//   Linux usbfs   struct usbdevfs_iso_packet_desc {length, actual_length,
//                 status} - the slots are laid end to end at the REQUESTED
//                 lengths, so slot k starts at the sum of the requested
//                 lengths before it (Linux usbfs documentation,
//                 <linux/usbdevice_fs.h>).
//   WinUSB        USBD_ISO_PACKET_DESCRIPTOR {Offset, Length, Status}, filled
//                 in by WinUsb_ReadIsochPipeAsap on completion (Microsoft's
//                 WinUSB isochronous documentation; Windows 8.1 and later).
//
// Either way the transport turns them into IsoPacket below and calls
// concatIsoPackets(), which is the one rule: HONOUR EACH PACKET'S ACTUAL
// LENGTH and concatenate the payloads in order. A device with a variable
// rate - the AOR digital-I/Q interface sends 0, 512, 1024 or 1536 bytes per
// microframe (AOR developer document, "Streaming transport") - leaves most
// slots partly empty, and a short packet is NOT lost data: reading whole
// slots would splice garbage into the stream, and treating a short packet as
// an error would throw good samples away.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace cascade::usb {

// One packet of a completed isochronous transfer, platform-neutral.
struct IsoPacket {
    std::size_t offset = 0;   // where its slot starts in the transfer buffer
    std::size_t length = 0;   // bytes the device actually sent
    int status = 0;           // 0 = delivered; anything else = this packet failed
};

// What one completed transfer contained. Accumulated by the caller if it
// wants totals.
struct IsoTransferStats {
    std::size_t bytes = 0;             // payload bytes concatenated into dst
    std::size_t packets = 0;           // descriptors in the transfer
    std::size_t emptyPackets = 0;      // delivered, 0 bytes (an idle microframe)
    std::size_t shortPackets = 0;      // delivered, fewer bytes than the slot holds
    std::size_t errorPackets = 0;      // status != 0: payload skipped
    std::size_t malformedPackets = 0;  // descriptor points outside the buffer: skipped
    std::size_t overflowBytes = 0;     // payload that did not fit in dst: dropped

    void add(const IsoTransferStats& o) {
        bytes += o.bytes;
        packets += o.packets;
        emptyPackets += o.emptyPackets;
        shortPackets += o.shortPackets;
        errorPackets += o.errorPackets;
        malformedPackets += o.malformedPackets;
        overflowBytes += o.overflowBytes;
    }
};

// Concatenates the delivered payloads of `packets` (in order) from `buffer`
// into `dst`, and returns the byte count written.
//
// NEVER READS OR WRITES OUT OF BOUNDS, whatever the descriptors say: a packet
// whose [offset, offset + length) is not inside [0, bufferBytes) or whose
// length exceeds its slot (`slotBytes`) is counted malformed and skipped
// rather than trusted, and payload beyond `cap` is counted and dropped. A
// failed packet (status != 0) contributes nothing, even if the kernel
// reported a length for it - its bytes are not known to be the device's.
inline std::size_t concatIsoPackets(const std::uint8_t* buffer, std::size_t bufferBytes,
                                    const IsoPacket* packets, std::size_t count,
                                    std::size_t slotBytes, std::uint8_t* dst, std::size_t cap,
                                    IsoTransferStats& stats) {
    stats = IsoTransferStats{};
    stats.packets = count;
    std::size_t at = 0;
    for (std::size_t k = 0; k < count; ++k) {
        const IsoPacket& p = packets[k];
        if (p.status != 0) {
            ++stats.errorPackets;
            continue;
        }
        if (p.length == 0) {
            ++stats.emptyPackets;
            continue;
        }
        if (p.offset > bufferBytes || p.length > bufferBytes - p.offset ||
            (slotBytes != 0 && p.length > slotBytes)) {
            ++stats.malformedPackets;
            continue;
        }
        if (slotBytes != 0 && p.length < slotBytes) { ++stats.shortPackets; }
        std::size_t n = p.length;
        if (n > cap - at) {
            stats.overflowBytes += n - (cap - at);
            n = cap - at;
        }
        if (n > 0) {
            std::memcpy(dst + at, buffer + p.offset, n);
            at += n;
        }
    }
    stats.bytes = at;
    return at;
}

// THE BYTES ONE (MICRO)FRAME OF AN ISOCHRONOUS ENDPOINT CAN CARRY, read out of
// the device's own descriptors rather than assumed, so a transport never
// requests packet slots smaller than what the device may send (a slot too
// small is a babble error on every full packet).
//
// `desc` is a device descriptor followed by configuration descriptors - what
// read() on a usbfs device node returns (Linux usbfs documentation) - or a
// configuration descriptor alone. The first configuration is searched for an
// endpoint descriptor (bDescriptorType 5) with bEndpointAddress `endpoint`
// and isochronous transfer type (bmAttributes bits 1..0 == 01), inside
// interface `ifaceNumber` alternate setting 0. wMaxPacketSize bits 10..0 are
// the packet size and bits 12..11 the number of ADDITIONAL transactions per
// microframe (USB 2.0 specification 9.6.6), so the answer is
// size * (1 + extra). False when there is no such endpoint - which is itself
// information: an interface with no isochronous endpoint where one is
// expected is not running the firmware that provides it.
inline bool isoEndpointBytesPerInterval(const std::uint8_t* desc, std::size_t len,
                                        std::uint8_t ifaceNumber, std::uint8_t endpoint,
                                        std::size_t& bytes) {
    std::size_t at = 0;
    bool inConfig = false;
    bool inWantedIface = false;
    while (at + 2 <= len) {
        const std::size_t bLength = desc[at];
        const std::uint8_t type = desc[at + 1];
        if (bLength < 2 || bLength > len - at) { return false; }
        if (type == 2) {  // configuration
            if (inConfig) { return false; }  // only the first configuration
            inConfig = true;
        } else if (type == 4 && bLength >= 4) {  // interface
            inWantedIface = desc[at + 2] == ifaceNumber && desc[at + 3] == 0;
        } else if (type == 5 && bLength >= 7 && inWantedIface) {  // endpoint
            if (desc[at + 2] == endpoint && (desc[at + 3] & 0x03u) == 0x01u) {
                const unsigned w = static_cast<unsigned>(desc[at + 4]) |
                                   (static_cast<unsigned>(desc[at + 5]) << 8);
                const std::size_t size = w & 0x07FFu;
                const std::size_t extra = (w >> 11) & 0x03u;
                if (extra == 3) { return false; }  // reserved value
                bytes = size * (1 + extra);
                return bytes > 0;
            }
        }
        at += bLength;
    }
    return false;
}

}  // namespace cascade::usb
