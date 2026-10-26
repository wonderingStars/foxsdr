// plugin_store_reasons.hpp - why a plugin cannot be fitted, kept in English
// where it is made and translated only where it is drawn.
//
// Moved verbatim from gui/plugin_store_view.hpp (engine extraction stage 3):
// the reasons are MADE by the engine's store machinery
// (Engine::pluginInstallBlockedReason, the ADD ALL queue) as well as drawn by
// the store window, and the engine may not include a window header.
// gui/plugin_store_view.hpp includes this, so every caller still finds them.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_ENGINE_PLUGIN_STORE_REASONS_HPP
#define CASCADE_ENGINE_PLUGIN_STORE_REASONS_HPP

#include <string>

#include "core/i18n.hpp"  // FOX_TR_NOOP
#include "core/plugin_cleanup.hpp"

namespace cascade::gui {

// --- a reason kept in English, drawn in the language in force -----------------
//
// WHY A MODULE CANNOT BE FITTED is one English sentence
// (AppWindow::pluginInstallBlockedReason), and it has to stay English where it
// is made: ADD ALL compares it ("already installed" is not a failure), the log
// records it, and the web page is handed it. So each reason is a FOX_TR_NOOP
// literal where it is defined, and translated only where it is DRAWN - the
// "Cannot fit:" line on a row and on the data plate, the red result line
// under them, the fitted window's copy of that line and the ADD ALL summary.
//
// Two of the reasons carry a value - the plugin ABI it was built for, the
// platform nobody built it for - so the English is made from a format string
// by the functions below, and trStoredReason() recognises a sentence made by
// either and formats the same values into that format's translation. The
// round trip is checked: a sentence that merely looks like one of them is
// drawn as it came.
std::string pluginAbiMismatchReason(unsigned builtFor, unsigned required);
std::string pluginNoBuildReason(const std::string& platform);  // "windows/x64"

// `english` in the language in force: its catalogue entry when it has one, a
// sentence from one of the two formats above re-made in its translation,
// otherwise `english` itself - so with English in force, and for words the
// host passes on verbatim (PluginRepo's sha256 and I/O errors), the text is
// byte for byte what it was.
std::string trStoredReason(const std::string& english);

// THE ACKNOWLEDGEMENT GATE'S SENTENCE, ONE LITERAL FOR BOTH SIDES. The engine's install gate
// (Engine::pluginInstallBlockedReason) answers it last, when an entry that carries a legal notice is asked about
// without the notice taken as acknowledged; the store window (window port group 10) makes the same sentence for a
// row whose table fact (`fact.store.<id>.blocked`, asked WITH the notice acknowledged) is empty and whose notice
// the tick has not covered - the tick belongs to one row and is the window's own, so the table cannot say it.
// Marked FOX_TR_NOOP where it is defined and drawn through trStoredReason like every other reason, so its
// translation is the catalogue entry the literal has always had.
inline constexpr const char* kLegalNoticeFirstReason = FOX_TR_NOOP("the legal notice must be acknowledged first");

// WHAT A CLEAN-UP DID, in one or more sentences for the store's line and for an update's report. Empty when it did nothing.
std::string pluginCleanupReport(const cascade::core::PluginCleanupResult& r);

}  // namespace cascade::gui

#endif  // CASCADE_ENGINE_PLUGIN_STORE_REASONS_HPP
