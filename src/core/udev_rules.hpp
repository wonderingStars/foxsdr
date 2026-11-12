// udev_rules.hpp - Linux USB permissions for FoxSDR's native radio drivers, generated from the
// application's OWN device tables, and the one-click install through pkexec.
//
// ONE TABLE. supportedRadioIds() collects the vendor/product pairs from the tables the drivers
// enumerate against (rtlSdrUsbIds, the HackRF / RX888 / Airspy / Airspy HF+ / HydraSDR / AOR protocol
// constants, msi2500::deviceModels). The rules text is generated from that list; the committed
// installer/linux/99-foxsdr-sdr.rules is checked against it by a test, so the two cannot drift.
//
// THE INSTALL, AND WHAT THE USER'S INPUT CAN REACH. The program never writes /etc itself and never
// runs a shell. It asks polkit to run ONE fixed, argument-free helper
//     pkexec /usr/libexec/foxsdr/foxsdr-udev-helper
// whose entire content is generated here (generateHelperScript) with the rules text baked in: it
// takes no arguments, reads no input, writes /etc/udev/rules.d/99-foxsdr-sdr.rules through a
// temporary file and a rename, and reloads the rules. The app first reads the helper from disk and
// refuses to run it unless it equals what this version would generate, so a helper from another
// version, or one somebody edited, is never started. The user is shown the exact path and the exact
// file content and must agree before anything is run. Packaging installs the helper and the polkit
// policy (packaging/linux/); an AppImage has neither, and then the user is given the manual steps.
//
// Everything is decided against core/linux_update.hpp's UpdateHost, so the refusals are tested on
// Windows with a fake. Running it on a real Linux with a real polkit prompt is UNPROVEN.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_UDEV_RULES_HPP
#define CASCADE_CORE_UDEV_RULES_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "core/linux_update.hpp"

namespace cascade::core::udev {

inline constexpr const char* kRulesPath = "/etc/udev/rules.d/99-foxsdr-sdr.rules";
inline constexpr const char* kHelperPath = "/usr/libexec/foxsdr/foxsdr-udev-helper";
inline constexpr const char* kPkexecPath = "/usr/bin/pkexec";
inline constexpr const char* kPolicyActionId = "com.foxsdr.install-udev-rules";

struct RadioId {
    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
    std::string family;  // for the comment above its group
};

// The application's own table, sorted by (vid, pid), no duplicates.
std::vector<RadioId> supportedRadioIds();

// The text of the rules file for those ids. Deterministic.
std::string generateRulesText();

// One rule line for an id (lower-case hex).
std::string ruleLine(std::uint16_t vid, std::uint16_t pid);

// True when every line of `text` is empty, a comment of printable ASCII, or exactly ruleLine() of
// some id. The helper only ever contains text that passes this.
bool validateRulesText(const std::string& text, std::string& error);

// The ids a rules text names (from its rule lines).
std::vector<std::pair<std::uint16_t, std::uint16_t>> idsInRulesText(const std::string& text);

// The helper script, with `rulesText` baked in. Refuses (empty string) text that fails
// validateRulesText, so nothing but rule lines can ever be written by the privileged script.
std::string generateHelperScript(const std::string& rulesText);

// The polkit policy for the helper (org.freedesktop.policykit.exec.path = kHelperPath).
std::string generatePolicyXml();

enum class State { Ready, AlreadyInstalled, NoPkexec, NoHelper, HelperMismatch };

struct Plan {
    State state = State::NoPkexec;
    std::string rulesPath;  // what is shown to the user
    std::string rulesText;  // exactly what will be written
    std::string manualSteps;  // what to do by hand when the one-click route is not available
    std::string message;
};
Plan planInstall(linuxupdate::UpdateHost& host);

enum class Outcome {
    Installed, AlreadyInstalled, Declined, NoPkexec, NoHelper, HelperMismatch,
    AuthDismissed,   // pkexec exit 126: the user closed the password dialog
    NotAuthorized,   // pkexec exit 127: not authorised, or no agent to ask
    WriteFailed,     // the helper failed, or the file is not what was expected afterwards
    Timeout          // could not be started, or did not finish
};
struct Result {
    Outcome outcome = Outcome::Declined;
    std::string detail;
};
std::string outcomeName(Outcome o);

// Runs the install. `userAgreed` must be true: it is the user's click on a dialog that showed
// Plan::rulesPath and Plan::rulesText. False runs nothing.
Result installRules(linuxupdate::UpdateHost& host, bool userAgreed);

}  // namespace cascade::core::udev

#endif  // CASCADE_CORE_UDEV_RULES_HPP
