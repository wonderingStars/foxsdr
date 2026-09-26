// plugin_store_reasons.cpp - see plugin_store_reasons.hpp. Moved verbatim
// from gui/plugin_store_view.cpp (engine extraction stage 3).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/plugin_store_reasons.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "core/i18n.hpp"
#include "core/utf8_text.hpp"

namespace cascade::gui {

using cascade::i18n::tr;

// --- a reason kept in English, drawn in the language in force -----------------
//
// The two formats the valued reasons are made from. ONE STRING SERVES THREE
// JOBS: it makes the English (pluginAbiMismatchReason), it is the pattern that
// sentence is recognised by (trStoredReason), and it is the catalogue key the
// translation is found under - so the three cannot drift apart.
namespace {
constexpr const char* kAbiReasonFormat =
    FOX_TR_NOOP("not compatible with this version (built for plugin ABI %u, this build "
                "requires exactly %u)");
constexpr const char* kNoBuildReasonFormat = FOX_TR_NOOP("no build for %s");
}  // namespace

std::string pluginAbiMismatchReason(unsigned builtFor, unsigned required) {
    return cascade::core::formatText(kAbiReasonFormat, builtFor, required);
}

std::string pluginNoBuildReason(const std::string& platform) {
    // Built as a string: the platform is the host's own "os/arch" and short,
    // but there is no length that makes a fixed buffer the right tool.
    const std::string_view fmt(kNoBuildReasonFormat);
    const std::size_t at = fmt.find("%s");
    return std::string(fmt.substr(0, at)) + platform + std::string(fmt.substr(at + 2));
}

std::string trStoredReason(const std::string& english) {
    if (english.empty()) { return english; }
    // tr() hands back its own argument when nothing translates it, so a
    // different pointer is a catalogue hit.
    const char* hit = tr(english.c_str());
    if (hit != english.c_str()) { return hit; }

    unsigned builtFor = 0;
    unsigned required = 0;
    if (std::sscanf(english.c_str(), kAbiReasonFormat, &builtFor, &required) == 2 &&
        pluginAbiMismatchReason(builtFor, required) == english) {
        return cascade::core::formatText(tr(kAbiReasonFormat), builtFor, required);
    }

    const std::string_view fmt(kNoBuildReasonFormat);
    const std::string_view lead = fmt.substr(0, fmt.find("%s"));
    if (english.size() > lead.size() && english.compare(0, lead.size(), lead) == 0) {
        const std::string platform = english.substr(lead.size());
        if (pluginNoBuildReason(platform) == english) {
            const char* local = tr(kNoBuildReasonFormat);
            std::vector<char> buf(std::strlen(local) + platform.size() + 8);
            cascade::core::formatUtf8(buf.data(), buf.size(), local, platform.c_str());
            return buf.data();
        }
    }
    return english;
}

}  // namespace cascade::gui
