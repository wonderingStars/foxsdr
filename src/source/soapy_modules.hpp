// soapy_modules.hpp - finding the SoapySDR vendor modules a user installed.
//
// THE BUG THIS EXISTS TO FIX, because it is not obvious and it was invisible
// on every development machine.
//
// SoapySDR reaches radios through vendor modules - rtlsdrSupport.dll,
// uhdSupport.dll and so on - which are separate binaries loaded at runtime.
// FoxSDR deliberately ships none of them: librtlsdr is GPL-2.0 and this is a
// PolyForm-licensed product, so the user installs PothosSDR or radioconda and
// FoxSDR uses what it finds. That decision is sound and is not changed here.
//
// What was broken is that FoxSDR never looked. The installer puts vcpkg's
// SoapySDR.dll directly in the application directory, and that library decides
// where to search by taking the directory of its own DLL and going TWO levels
// up (SoapySDR's Modules.cpp: `libPath.substr(0, slash1Pos)`), on the
// assumption it was installed as <root>/bin/SoapySDR.dll. Ours is not in a
// bin/, so for a default installation the computed root is the PARENT of the
// install directory - "C:\Program Files" - and the search path becomes
// "C:\Program Files/lib/SoapySDR/modules0.8", which exists nowhere and never
// will.
//
// The consequence was total: no modules loaded, so no devices enumerated, for
// EVERY user and EVERY radio, no matter what they had installed. The dropdown
// simply did not list their hardware and nothing said why. It worked on the
// development machine for one reason - a SOAPY_SDR_PLUGIN_PATH user
// environment variable left behind by a radioconda install - which is exactly
// the kind of accident that makes a shipped product look fine to its author.
//
// So this module does what the application should have been doing: find the
// vendor installs the user actually has, add their module directories to the
// search path, and make the vendor DLLs those modules depend on loadable.
//
// TWO STEPS ARE NEEDED, AND ONE IS NOT ENOUGH. Pointing SoapySDR at
// rtlsdrSupport.dll only gets as far as LoadLibrary failing, because that
// module links rtlsdr.dll and libusb-1.0.dll, which live in the vendor's bin
// directory. UHD happens to work with one step because its own installer puts
// uhd.dll on the machine PATH; RTL-SDR has no such luck, which is precisely
// why the B200 was the device this was developed against and dongles were the
// device that never worked.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#ifndef CASCADE_SOURCE_SOAPY_MODULES_HPP
#define CASCADE_SOURCE_SOAPY_MODULES_HPP

#include <functional>
#include <string>
#include <vector>

namespace cascade::source {

// One vendor SDR installation found on this machine.
struct VendorRoot {
    std::string name;       // "PothosSDR", "radioconda", ...
    std::string root;       // the install root
    std::string moduleDir;  // <root>/lib/SoapySDR/modules<abi>
    std::string binDir;     // <root>/bin, holding rtlsdr.dll, libusb-1.0.dll...
};

// The candidate roots to probe, in order, built from the environment.
//
// Separated from the probing so the list can be tested without a filesystem
// and without any of these products installed. `getenv` is injected for the
// same reason: the test supplies an environment rather than depending on the
// machine it runs on.
std::vector<std::pair<std::string, std::string>> candidateVendorRoots(
    const std::function<std::string(const char*)>& getenv);

// Which candidates actually hold a module directory for this ABI.
//
// `abi` is SoapySDR's ABI version string ("0.8"), read from the library at
// runtime rather than written down here: a hard-coded "0.8" would silently
// stop matching the day the dependency moved, and the failure would look
// exactly like the bug this file fixes.
//
// `isDirectory` is injected so the whole resolution is testable against a
// fabricated filesystem. Roots whose module directory is missing are dropped;
// the result preserves the candidate order, so an explicitly installed
// PothosSDR wins over a conda environment that happens to be present.
std::vector<VendorRoot> resolveVendorRoots(
    const std::vector<std::pair<std::string, std::string>>& candidates, const std::string& abi,
    const std::function<bool(const std::string&)>& isDirectory);

// The value SOAPY_SDR_PLUGIN_PATH should hold, given what it holds already.
//
// APPENDS, never replaces: a user who set that variable by hand has said
// something deliberate about where their modules are, and overwriting it would
// break a working setup in the name of fixing a broken one. Existing entries
// keep their position and their precedence. Directories already present are
// not added twice, which matters because this runs on every runtime probe.
//
// Returns an empty string when there is nothing to add, so the caller can skip
// touching the environment at all.
std::string pluginPathWith(const std::string& existing, const std::vector<VendorRoot>& roots,
                           char separator);

// Finds the vendor installs on this machine, extends SOAPY_SDR_PLUGIN_PATH to
// cover them, and registers their bin directories for DLL resolution.
//
// Idempotent and cheap after the first call. Safe to call before SoapySDR has
// loaded any module, which is the only moment it has any effect: the search
// path is read once, when the first enumeration triggers module loading.
//
// Returns the roots it adopted, for the diagnostics the GUI shows.
const std::vector<VendorRoot>& ensureVendorModulesVisible(const std::string& abi);

}  // namespace cascade::source

#endif  // CASCADE_SOURCE_SOAPY_MODULES_HPP
