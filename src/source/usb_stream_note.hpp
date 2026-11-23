// usb_stream_note.hpp - what a native driver does with the transport's
// stream notes and endpoint layout (usb/usb_device.hpp endpointLayout(),
// takeStreamNote()).
//
// WHY. 19-23 Nov 2026: the Airspy HF+ and Mirics MSi2500 drivers closed on
// every start with WinUsb_SetPipePolicy(RAW_IO) failed (Windows error 87),
// and nothing in the report said which endpoint, interface or alternate
// setting the driver had asked for. The transport now falls back to plain
// reads when RAW_IO is refused, and these two helpers put the facts in the
// log (which the radio report carries) and count the fallback as a recovery.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>

#include "core/diag_log.hpp"
#include "core/radio_events.hpp"
#include "usb/usb_device.hpp"

namespace cascade::source {

// One line at open: the device's endpoints. Nothing when the transport cannot
// say (a fake, usbfs). No serial number.
inline void logUsbLayout(cascade::usb::UsbDevice& dev, const char* driverTag) {
    const std::string layout = dev.endpointLayout();
    if (!layout.empty()) { core::diagLogf("%s: USB layout - %s", driverTag, layout.c_str()); }
}

// After a beginBulkStream() that succeeded: if the transport took a fallback,
// say so once in the log and count it in the radio report as an absorbed
// recovery ("rawiofallback").
inline void reportStreamNote(cascade::usb::UsbDevice& dev, const void* owner,
                             const char* driverKey) {
    const std::string note = dev.takeStreamNote();
    if (note.empty()) { return; }
    core::diagWarnf("%s: %s", driverKey, note.c_str());
    core::radioreport::noteVendorCallFault(owner, driverKey, "rawiofallback", note, true);
}

// The user-facing sentence for a transfer that failed while `what`. A fault in
// the transport's own words about the radio's USB layout is a fact about the
// device: it is passed through whole, with no "unplug it" advice (which is
// both wrong and read by the close classifier as an unplugged radio).
inline std::string transportFaultSentence(const char* what, const std::string& detail) {
    if (cascade::usb::errorIsLayoutProblem(detail)) {
        return std::string("the radio cannot be streamed while ") + what + ": " + detail;
    }
    return std::string("the radio stopped answering while ") + what +
           (detail.empty() ? std::string() : (": " + detail)) +
           "; unplug it and plug it back in";
}

}  // namespace cascade::source
