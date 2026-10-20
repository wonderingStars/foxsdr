// patch_device_label.hpp - the words a patch Radio's device key is shown by (window port group 13).
//
// ONE RULE, TWO READERS. The Engine lists the devices a Radio can be set to (Engine::patchDeviceChoices, served as
// PATCH_DEVICES) and names a node's device by that list; the standard window draws a Radio's face and its Device combo
// from the SAME list read back through the engine API, and must name a device the list does not hold (a SoapySDR radio
// is only listed after a scan, a recording by its file, a radio by the label its key carries) by the same fallbacks - so
// the rule is a function of the key and the list, written once, here.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <filesystem>
#include <string>

#include "core/i18n.hpp"
#include "core/patch_devices.hpp"
#include "core/utf8_text.hpp"

namespace cascade::core::patch {

// The file name of an I/Q recording's key, for its label - as UTF-8 for the screen.
inline std::string iqFileDisplayName(const std::string& key) {
    try {
        const auto s = std::filesystem::path(iqFilePath(key)).filename().u8string();
        return std::string(s.begin(), s.end());
    } catch (...) {
        return iqFilePath(key);
    }
}

// The label of `key`: its entry in `choices` (anything with a `key` and a `label`) if the list holds it, else what the
// key itself says. "No device chosen" for an empty key; "Recording: <file>" for a recording whether or not the list has
// been read (the key carries the path); a radio's own label from its args; else the driver (and serial) "not listed" -
// NOT LISTED IS NOT "NOT CONNECTED".
template <class Choices>
std::string deviceLabelFor(const Choices& choices, const std::string& key) {
    using cascade::i18n::tr;
    if (key.empty()) { return tr("No device chosen"); }
    for (const auto& c : choices) {
        if (c.key == key) { return c.label; }
    }
    if (isIqFileKey(key)) {
        std::string buf;
        cascade::core::formatUtf8(buf, tr("Recording: %s"), iqFileDisplayName(key).c_str());
        return buf;
    }
    const std::string args = deviceArgs(key);
    const std::string own = argField(args, "label");
    if (!own.empty()) { return own; }
    const std::string serial = argField(args, "serial");
    std::string buf;
    if (serial.empty()) {
        cascade::core::formatUtf8(buf, tr("%s (not listed)"), deviceDriver(key).c_str());
    } else {
        cascade::core::formatUtf8(buf, tr("%s %s (not listed)"), deviceDriver(key).c_str(), serial.c_str());
    }
    return buf;
}

}  // namespace cascade::core::patch
