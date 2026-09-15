// Tests for core/package_identity.{hpp,cpp} and the two decisions that hang
// off it: whether the startup update check runs, and where plugins live.
//
// WHAT CAN AND CANNOT BE TESTED HERE, stated plainly.
//
// The ctest run is an ORDINARY UNPACKAGED PROCESS, so the live query is
// exercised only in the direction it can be: it must answer "not packaged",
// with an empty name, and it must be cheap enough to call repeatedly. That is
// a real assertion - a query that answered "packaged" on this machine would
// stand the update check down for every developer build - but it is half of
// the question.
//
// The other half is the POLICY, and that is written as pure functions
// precisely so it can be tested without a package:
//
//   identityFromApiResult   - the Win32 contract, every branch
//   updateCheckDisposition  - the three states the startup check has
//   choosePluginDir(..., packaged) - the directory choice
//
// The end-to-end half - a real MSIX package on this machine, taking the
// packaged branches for real - is proven by installer/msix/build-msix.ps1 and
// recorded in installer/msix/README.md. It is not, and cannot be, a ctest
// entry: it needs a signed package, a certificate in a trust store, and an
// Add-AppxPackage.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/package_identity.hpp"

#include <cstdio>
#include <string>

#include "core/plugin_host.hpp"
#include "test_check.hpp"

using cascade::core::PackageIdentity;
using cascade::core::UpdateCheckDisposition;

namespace {

// From shared/winerror.h. Repeated here on purpose: if the implementation's
// copy of this number ever drifts, this test must not drift with it.
constexpr long kAppmodelErrorNoPackage = 15700L;
constexpr long kErrorSuccess = 0L;
constexpr long kErrorInsufficientBuffer = 122L;

// THE PURE MAPPING FROM THE WIN32 CONTRACT.
void testIdentityFromApiResult() {
    // Success carries a name, and the name is kept verbatim.
    const PackageIdentity ok =
        cascade::core::identityFromApiResult(kErrorSuccess, "FoxSDR_0.96.0.0_x64__abcdefghijklm");
    CHECK(ok.packaged);
    CHECK(ok.fullName == "FoxSDR_0.96.0.0_x64__abcdefghijklm");

    // The documented "there is no package" code. This is the answer every
    // unpackaged copy in the world gets, so it is the one that matters most.
    const PackageIdentity none =
        cascade::core::identityFromApiResult(kAppmodelErrorNoPackage, {});
    CHECK(!none.packaged);
    CHECK(none.fullName.empty());

    // A name supplied alongside a failure code is DISCARDED, not reported.
    // Reporting it would put a package name in the log of a process that is
    // not in a package.
    const PackageIdentity noneWithJunk =
        cascade::core::identityFromApiResult(kAppmodelErrorNoPackage, "leftover_junk");
    CHECK(!noneWithJunk.packaged);
    CHECK(noneWithJunk.fullName.empty());

    // ANY other code means "could not answer", and the safe answer is the one
    // that leaves the ordinary installed build behaving exactly as it does
    // today. ERROR_INSUFFICIENT_BUFFER never reaches this function (the caller
    // grows and retries), but if it ever did, it must land here too.
    CHECK(!cascade::core::identityFromApiResult(kErrorInsufficientBuffer, "x").packaged);
    CHECK(!cascade::core::identityFromApiResult(5L /* ERROR_ACCESS_DENIED */, {}).packaged);
    CHECK(!cascade::core::identityFromApiResult(-1L, {}).packaged);
    CHECK(!cascade::core::identityFromApiResult(1L, {}).packaged);

    // Success with an EMPTY name is still packaged. The name is for the log;
    // the boolean is the decision, and they must not be conflated.
    CHECK(cascade::core::identityFromApiResult(kErrorSuccess, {}).packaged);
}

// THE UPDATE CHECK'S THREE STATES.
void testUpdateCheckDisposition() {
    // Ordinary install, the user wants it: ask.
    CHECK(cascade::core::updateCheckDisposition(false, true) == UpdateCheckDisposition::Run);

    // Packaged: the Store is the update channel, so the check stands down
    // EVEN THOUGH the user's tick is on. This is the whole point of the file.
    CHECK(cascade::core::updateCheckDisposition(true, true) ==
          UpdateCheckDisposition::StorePackage);

    // Unticked wins over packaged when it comes to the REASON shown, because
    // the user's own choice is the one they can change. Both answers stop the
    // check; they differ in what the Settings row says.
    CHECK(cascade::core::updateCheckDisposition(false, false) ==
          UpdateCheckDisposition::OffByChoice);
    CHECK(cascade::core::updateCheckDisposition(true, false) ==
          UpdateCheckDisposition::OffByChoice);

    // Only one of the four combinations may reach the network.
    int runs = 0;
    for (const bool packaged : {false, true}) {
        for (const bool enabled : {false, true}) {
            if (cascade::core::updateCheckDisposition(packaged, enabled) ==
                UpdateCheckDisposition::Run) {
                ++runs;
            }
        }
    }
    CHECK(runs == 1);
}

// THE SENTENCES. Pinned, not because wording is sacred, but because the log
// line is what a support conversation greps for and the panel sentence is the
// only thing that tells a Store user why there is no update button.
void testStandDownWording() {
    const std::string line = cascade::core::updateCheckStandDownLine();
    CHECK(line == "update check: running from a Store package - the Store delivers updates");
    const std::string sentence = cascade::core::updateCheckStandDownSentence();
    CHECK(!sentence.empty());
    CHECK(sentence.find("Store") != std::string::npos);
    // It must not read as a failure. "could not", "error" and "failed" all
    // describe something going wrong; nothing here has.
    CHECK(sentence.find("could not") == std::string::npos);
    CHECK(sentence.find("failed") == std::string::npos);
}

// THE LIVE QUERY, in the only direction this process can prove.
void testLiveQueryIsUnpackagedHere() {
    cascade::core::clearPackageIdentityForTest();
    const PackageIdentity& id = cascade::core::packageIdentity();
    CHECK(!id.packaged);
    CHECK(id.fullName.empty());
    CHECK(!cascade::core::runningInPackage());
    // Cached: the second call must agree with the first, and must return the
    // same object rather than re-querying.
    const PackageIdentity& again = cascade::core::packageIdentity();
    CHECK(&again == &id);
    std::printf("  live query on this process: packaged=%d name=\"%s\"\n", id.packaged ? 1 : 0,
                id.fullName.c_str());
}

// THE IN-PROCESS SEAM, which is what lets everything above be exercised
// against code that asks the live question.
void testSeam() {
    cascade::core::clearPackageIdentityForTest();
    CHECK(!cascade::core::runningInPackage());

    cascade::core::setPackageIdentityForTest({true, "FoxSDR_0.96.0.0_x64__test"});
    CHECK(cascade::core::runningInPackage());
    CHECK(cascade::core::packageIdentity().fullName == "FoxSDR_0.96.0.0_x64__test");

    // Settable twice - a cache that answered once and froze would make every
    // test after the first one meaningless.
    cascade::core::setPackageIdentityForTest({true, "FoxSDR_0.97.0.0_x64__test"});
    CHECK(cascade::core::packageIdentity().fullName == "FoxSDR_0.97.0.0_x64__test");

    cascade::core::setPackageIdentityForTest({false, {}});
    CHECK(!cascade::core::runningInPackage());

    cascade::core::clearPackageIdentityForTest();
    CHECK(!cascade::core::runningInPackage());
}

// THE PLUGIN DIRECTORY CHOICE, with the packaged flag added.
//
// The three pre-existing rows are repeated verbatim from
// tests/test_plugin_host.cpp on purpose: the new parameter defaults to false,
// and "the default did not change the old answers" is the property that makes
// adding it safe.
void testPluginDirChoiceUnderPackage() {
    using cascade::core::PluginHost;

    // Unpackaged: unchanged, in all three rows.
    CHECK(PluginHost::choosePluginDir("/exe/plugins", "/user/plugins", true) == "/exe/plugins");
    CHECK(PluginHost::choosePluginDir("/exe/plugins", "/user/plugins", false) == "/user/plugins");
    CHECK(PluginHost::choosePluginDir("/exe/plugins", "", false) == "/exe/plugins");
    CHECK(PluginHost::choosePluginDir("/exe/plugins", "/user/plugins", true, false) ==
          "/exe/plugins");

    // PACKAGED: the per-user directory, and the writability claim about the
    // exe directory is irrelevant. A package directory that reported itself
    // writable could only be wrong; this row is what says so.
    CHECK(PluginHost::choosePluginDir("/pkg/plugins", "/user/plugins", false, true) ==
          "/user/plugins");
    CHECK(PluginHost::choosePluginDir("/pkg/plugins", "/user/plugins", true, true) ==
          "/user/plugins");

    // Packaged with nothing to derive a per-user path from: keep the real
    // path, so the error the user is shown names a directory that exists.
    // Refusing to answer, or answering "", would be worse than answering with
    // a directory that will refuse the write.
    CHECK(PluginHost::choosePluginDir("/pkg/plugins", "", true) == "/pkg/plugins");
    CHECK(PluginHost::choosePluginDir("/pkg/plugins", "", false, true) == "/pkg/plugins");
}

// defaultPluginDir() ASKS the live question, so the seam must move it.
//
// This is the assertion that fails if someone reinstates the unconditional
// probe: under a faked package the answer must be the per-user directory,
// which on Windows is %LOCALAPPDATA%\foxsdr\plugins.
void testDefaultPluginDirUnderFakedPackage() {
    using cascade::core::PluginHost;

    cascade::core::clearPackageIdentityForTest();
    const std::string userDir = PluginHost::userPluginDir();
    const std::string unpackagedAnswer = PluginHost::defaultPluginDir();

    cascade::core::setPackageIdentityForTest({true, "FoxSDR_0.96.0.0_x64__test"});
    const std::string packagedAnswer = PluginHost::defaultPluginDir();
    cascade::core::clearPackageIdentityForTest();

    if (!userDir.empty()) {
        CHECK(packagedAnswer == userDir);
        // And it is genuinely the OTHER candidate, not the same string
        // arrived at twice - in a developer build the exe directory IS
        // writable, so the two answers must differ.
        CHECK(packagedAnswer != PluginHost::exePluginDir());
    }
    std::printf("  unpackaged here: %s\n", unpackagedAnswer.c_str());
    std::printf("  under a package: %s\n", packagedAnswer.c_str());

    // Back to normal afterwards: a leaked override would silently redirect
    // every later test in this process.
    CHECK(PluginHost::defaultPluginDir() == unpackagedAnswer);
}

}  // namespace

int main() {
    testIdentityFromApiResult();
    testUpdateCheckDisposition();
    testStandDownWording();
    testLiveQueryIsUnpackagedHere();
    testSeam();
    testPluginDirChoiceUnderPackage();
    testDefaultPluginDirUnderFakedPackage();
    return testSummary("test_package_identity");
}
