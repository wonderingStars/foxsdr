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

#include <atomic>
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
//
// TWO WORDS, because they mean different things. "rawiofallback": the stream is
// running WITHOUT RAW_IO. "altmoved": RAW_IO is on and the transport had to
// select another alternate setting / interface for the pipe; filed once per
// radio OPEN (it is set-up, not a fault, and the driver now selects the setting
// itself at open where the radio needs it; an altmoved at start therefore means
// the open-time select failed and the start-time recovery worked). Both are `what` words of
// docs/radio-reports-contract.md 3.6 (^[a-z0-9]{3,24}$).
// Returns true when it filed a note in the radio report, false when there was nothing to say or the "altmoved" of this radio open was filed already.
inline bool reportStreamNote(cascade::usb::UsbDevice& dev, const void* owner,
                             const char* driverKey) {
    std::string note = dev.takeStreamNote();
    if (note.empty()) { return false; }
    const std::string fb = cascade::usb::kStreamNoteFallback;
    const std::string mv = cascade::usb::kStreamNoteMoved;
    const char* word = "rawiofallback";
    bool once = false;
    if (note.compare(0, mv.size(), mv) == 0) {
        word = "altmoved";
        once = true;
        note.erase(0, mv.size());
    } else if (note.compare(0, fb.size(), fb) == 0) {
        note.erase(0, fb.size());
    }
    core::diagWarnf("%s: %s", driverKey, note.c_str());
    if (once && dev.takeAltMovedFiled()) { return false; }   // once per radio OPEN (the device object's own flag)
    core::radioreport::noteVendorCallFault(owner, driverKey, word, note, true);
    return true;
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
