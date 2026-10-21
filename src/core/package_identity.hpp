// package_identity.hpp - "am I running from an MSIX package, or from an
// ordinary install?"
//
// WHY THIS EXISTS. FoxSDR can be delivered two ways: the Inno Setup installer
// from foxsdr.com, and (if the Store route is taken) an MSIX package the
// Microsoft Store installs and updates. The SAME binary runs in both, and two
// of its behaviours have to differ, because inside a package they are wrong
// rather than merely unnecessary:
//
//   1. THE UPDATE CHECK. The unpackaged build downloads foxsdr-setup-<ver>.exe
//      and hands it to the shell. Inside a package that is a category error:
//      the package directory is read-only, the installed identity belongs to
//      the deployment stack, and running an Inno installer would leave a
//      SECOND FoxSDR on the machine beside the packaged one. The Store is the
//      update channel for a Store install, so a packaged copy never asks
//      foxsdr.com - since 0.99.53 it asks the MICROSOFT STORE instead
//      (core/store_update.hpp), because the Store's own background updating
//      was measured to leave copies a day or more behind without a word.
//
//   2. THE PLUGIN DIRECTORY. PluginHost::defaultPluginDir() normally decides
//      between the exe-adjacent directory and the per-user one by WRITING A
//      PROBE FILE, because an installed program directory looks perfectly
//      writable until something tries. Under a package that probe is a write
//      into C:\Program Files\WindowsApps\<full name>, which Microsoft
//      documents as "Not allowed. The package is read-only."
//      (learn.microsoft.com/windows/msix/desktop/desktop-to-uwp-behind-the-scenes,
//      "Common file system operations"). The answer is known in advance, so
//      the probe is skipped and %LOCALAPPDATA%\foxsdr\plugins is used
//      directly. Knowing beforehand is the point: the probe would fail
//      harmlessly, but it would still be an attempted write into a directory
//      the OS protects, on every launch.
//
// HOW IT ASKS. GetCurrentPackageFullName (kernel32, Windows 8 and later,
// declared in appmodel.h) returns ERROR_SUCCESS with a name for a packaged
// process and APPMODEL_ERROR_NO_PACKAGE (15700) for an unpackaged one. It is
// resolved with GetProcAddress rather than linked, so a build that somehow
// runs on a system without the app-model exports reports "unpackaged" instead
// of failing to start.
//
// WHAT IS PURE AND WHAT IS NOT. The MAPPING from the API's return code to a
// decision is a pure function (identityFromApiResult) and is where the tests
// live; the query itself is one call, cached. Every behavioural decision that
// follows - which update channel to ask, choose a plugin directory - is also a
// pure function taking `packaged` as an argument, so the whole policy is
// testable on a machine with no package in sight.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PACKAGE_IDENTITY_HPP
#define CASCADE_CORE_PACKAGE_IDENTITY_HPP

#include <string>

namespace cascade::core {

// What the process is, and - when it is packaged - what it is called.
struct PackageIdentity {
    bool packaged = false;
    // The package full name, e.g. "FoxSDR_0.96.0.0_x64__8wekyb3d8bbwe".
    // Empty for an unpackaged process. Logged, never parsed: the shape of a
    // full name is the deployment stack's business, not this program's.
    std::string fullName;
};

// THE PURE MAPPING, and the only place the API's contract is written down.
//
//   ERROR_SUCCESS (0)               -> packaged, with the name that came back
//   APPMODEL_ERROR_NO_PACKAGE       -> not packaged
//   anything else                   -> not packaged
//
// The last line is a decision, not a shrug. An unexpected code means the
// question could not be answered, and "behave as the ordinary installed build
// does" is the answer that cannot break an ordinary installed build - which is
// every copy in the world today. Treating an unknown code as "packaged" would
// silently disable the update check for them, which is the fault this whole
// file exists to avoid the mirror image of.
//
// ERROR_INSUFFICIENT_BUFFER is NOT special-cased here: the caller grows its
// buffer and calls again, so it never reaches this function with a decision to
// make. If one ever did, the fall-through above is the safe answer.
PackageIdentity identityFromApiResult(long apiResult, const std::string& fullName);

// The live answer for THIS process. Queried once and cached: the answer cannot
// change inside a process, and the plugin scan, the update check and the
// Settings panel all ask.
const PackageIdentity& packageIdentity();

// Shorthand for the common question.
bool runningInPackage();

// --- THE TEST SEAM ---------------------------------------------------------
//
// Faking the packaged state without a package, two ways:
//
//   setPackageIdentityForTest(id)  - in-process, for the unit tests.
//   FOXSDR_FAKE_PACKAGE=<name>     - environment, for a CHILD PROCESS: it is
//                                    how an ordinary unpackaged cascade.exe
//                                    can be made to take the packaged branches
//                                    so they can be observed end to end. The
//                                    value becomes the reported full name;
//                                    the literal value "none" forces the
//                                    unpackaged answer.
//
// WHY AN ENVIRONMENT HOOK IS SAFE HERE, stated rather than assumed: both
// behaviours it can reach are fail-safe. It can move the update check from
// foxsdr.com to the Microsoft Store (which, for a copy the Store did not
// install, answers with an error and offers nothing) and it can move the
// plugin directory to the per-user one (which the unpackaged build already
// uses whenever it is installed to Program Files). It cannot grant anything,
// load anything, or send anything of ours anywhere. Contrast CASCADE_CONFIG_TEST,
// which is honoured only under --frames precisely because redirecting a real
// session's config file would be a hazard.
void setPackageIdentityForTest(const PackageIdentity& id);
void clearPackageIdentityForTest();

// --- THE DECISIONS THAT DEPEND ON IT ---------------------------------------

// What the startup update check should do.
enum class UpdateCheckDisposition {
    Run,             // ordinary install, the user wants it: ask foxsdr.com
    OffByChoice,     // the user unticked it: ask nobody
    AskStore,        // packaged, the user wants it: ask the Microsoft Store,
                     // never foxsdr.com (core/store_update.hpp)
};

// Pure. The user's tick comes FIRST, for both kinds of copy: unticked means
// nobody is asked - not foxsdr.com, not the Store. Only then does the kind of
// copy choose the channel. (Until 0.99.53 a packaged copy asked nobody even
// with the tick on, and the Settings row had no tick to show.)
UpdateCheckDisposition updateCheckDisposition(bool packaged, bool userEnabled);

}  // namespace cascade::core

#endif  // CASCADE_CORE_PACKAGE_IDENTITY_HPP
