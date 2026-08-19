// Tests for source/soapy_modules.{hpp,cpp} - finding the vendor modules.
//
// WHY THESE EXIST, and why they are written against injected seams rather
// than the machine they run on.
//
// The bug this code fixes was invisible to the existing test suite for a
// structural reason worth naming: tests/test_soapy_source.cpp is
// hardware-optional, so on a machine with no radio it skips, and on the
// development machine it PASSED - because that machine had a
// SOAPY_SDR_PLUGIN_PATH environment variable left behind by a radioconda
// install. Every test agreed the source worked. Every user found it did not.
// A test that depends on the developer's environment is testing the developer.
//
// So the resolution here is split into pure functions with the environment and
// the filesystem injected, and these tests supply both. They therefore fail or
// pass identically on a machine with PothosSDR, a machine with radioconda, a
// machine with neither, and CI.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/soapy_modules.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "test_check.hpp"

using cascade::source::VendorRoot;

namespace {

// An environment, as a lookup rather than the real one.
std::function<std::string(const char*)> envFrom(std::map<std::string, std::string> vars) {
    return [vars](const char* name) -> std::string {
        const auto it = vars.find(name);
        return (it != vars.end()) ? it->second : std::string();
    };
}

// A filesystem, as a set of directories that exist.
std::function<bool(const std::string&)> fsWith(std::set<std::string> dirs) {
    return [dirs](const std::string& p) -> bool {
        // Compare with both separators normalised, because the code under test
        // builds paths with std::filesystem and the fixtures are written by
        // hand.
        std::string norm = p;
        for (char& c : norm) {
            if (c == '\\') { c = '/'; }
        }
        for (const std::string& d : dirs) {
            std::string dn = d;
            for (char& c : dn) {
                if (c == '\\') { c = '/'; }
            }
            if (dn == norm) { return true; }
        }
        return false;
    };
}

bool hasRootNamed(const std::vector<VendorRoot>& v, const std::string& name) {
    return std::any_of(v.begin(), v.end(), [&name](const VendorRoot& r) { return r.name == name; });
}

void testCandidatesCoverTheInstallersPeopleActuallyUse() {
    const auto env = envFrom({
        {"ProgramFiles", "C:\\Program Files"},
        {"USERPROFILE", "C:\\Users\\ham"},
        {"LOCALAPPDATA", "C:\\Users\\ham\\AppData\\Local"},
    });
    const auto cands = cascade::source::candidateVendorRoots(env);

    const auto named = [&cands](const char* n) {
        return std::any_of(cands.begin(), cands.end(),
                           [n](const std::pair<std::string, std::string>& c) {
                               return c.first == n;
                           });
    };
    // The two the documentation tells users to install. If either of these
    // ever stops being probed, the docs become false again.
    CHECK(named("PothosSDR"));
    CHECK(named("radioconda"));
    CHECK(!cands.empty());
}

void testAnExplicitRootOutranksEveryGuess() {
    // A user who set SOAPY_SDR_ROOT has said where their install is. Nothing
    // guessed here may be preferred over that.
    const auto env = envFrom({
        {"SOAPY_SDR_ROOT", "D:\\sdr"},
        {"ProgramFiles", "C:\\Program Files"},
    });
    const auto cands = cascade::source::candidateVendorRoots(env);
    CHECK(!cands.empty());
    if (!cands.empty()) {
        CHECK(cands[0].second == "D:\\sdr");
    }
}

void testAnEmptyEnvironmentProbesNothingAndDoesNotCrash() {
    const auto cands = cascade::source::candidateVendorRoots(envFrom({}));
    CHECK(cands.empty());
    const auto roots = cascade::source::resolveVendorRoots(cands, "0.8", fsWith({}));
    CHECK(roots.empty());
}

void testOnlyRootsThatActuallyHoldModulesAreAdopted() {
    const auto env = envFrom({
        {"ProgramFiles", "C:\\Program Files"},
        {"USERPROFILE", "C:\\Users\\ham"},
    });
    const auto cands = cascade::source::candidateVendorRoots(env);

    // radioconda present, PothosSDR not.
    const auto exists = fsWith({"C:/Users/ham/radioconda/Library/lib/SoapySDR/modules0.8"});
    const auto roots = cascade::source::resolveVendorRoots(cands, "0.8", exists);

    CHECK(roots.size() == 1);
    CHECK(hasRootNamed(roots, "radioconda"));
    CHECK(!hasRootNamed(roots, "PothosSDR"));
    if (roots.size() == 1) {
        CHECK(roots[0].binDir == std::string("C:\\Users\\ham\\radioconda\\Library\\bin") ||
              roots[0].binDir == std::string("C:/Users/ham/radioconda/Library/bin"));
    }
}

void testTheAbiVersionIsPartOfThePath() {
    // The ABI string comes from the library at runtime rather than being
    // written down, so that a dependency bump cannot silently stop matching.
    // A directory for a DIFFERENT ABI must not be adopted: loading a module
    // built against another ABI is the kind of near-miss that becomes memory
    // corruption later, which is the same rule the plugin loader applies.
    const auto env = envFrom({{"USERPROFILE", "C:\\Users\\ham"}});
    const auto cands = cascade::source::candidateVendorRoots(env);
    const auto exists = fsWith({"C:/Users/ham/radioconda/Library/lib/SoapySDR/modules0.7"});

    CHECK(cascade::source::resolveVendorRoots(cands, "0.8", exists).empty());
    CHECK(cascade::source::resolveVendorRoots(cands, "0.7", exists).size() == 1);
}

void testTheSameInstallReachedTwiceIsListedOnce() {
    // An active conda environment that is also the default radioconda
    // location is one installation, not two.
    const auto env = envFrom({
        {"USERPROFILE", "C:\\Users\\ham"},
        {"CONDA_PREFIX", "C:\\Users\\ham\\radioconda"},
    });
    const auto cands = cascade::source::candidateVendorRoots(env);
    CHECK(cands.size() >= 2);  // both spellings are probed...
    const auto exists = fsWith({"C:/Users/ham/radioconda/Library/lib/SoapySDR/modules0.8"});
    CHECK(cascade::source::resolveVendorRoots(cands, "0.8", exists).size() == 1);  // ...one adopted
}

void testPluginPathAppendsAndNeverReplaces() {
    // THE RULE THAT MATTERS MOST HERE. A user who set SOAPY_SDR_PLUGIN_PATH by
    // hand has a working setup; overwriting it to fix a broken one would break
    // theirs. Their entries keep their place and their precedence.
    VendorRoot v;
    v.name = "radioconda";
    v.moduleDir = "C:\\rc\\lib\\SoapySDR\\modules0.8";

    const std::string existing = "D:\\mine\\modules0.8";
    const std::string out = cascade::source::pluginPathWith(existing, {v}, ';');

    CHECK(out.rfind(existing, 0) == 0);  // starts with what was there
    CHECK(out == existing + ";" + v.moduleDir);
}

void testPluginPathDoesNotAddADirectoryTwice() {
    // This runs on every runtime probe, so a value that grew each time would
    // eventually be a very long environment variable.
    VendorRoot v;
    v.name = "radioconda";
    v.moduleDir = "C:\\rc\\lib\\SoapySDR\\modules0.8";

    CHECK(cascade::source::pluginPathWith(v.moduleDir, {v}, ';').empty());
    // And case- and separator-insensitively on Windows, where these are all
    // the same directory.
#ifdef _WIN32
    CHECK(cascade::source::pluginPathWith("c:/rc/lib/soapysdr/modules0.8", {v}, ';').empty());
#endif
}

void testPluginPathHandlesAnEmptyStartingValue() {
    VendorRoot v;
    v.moduleDir = "C:\\rc\\lib\\SoapySDR\\modules0.8";
    CHECK(cascade::source::pluginPathWith("", {v}, ';') == v.moduleDir);
    // No leading separator, which would be an empty entry SoapySDR then skips
    // - harmless, but it would look like a bug to anyone reading the variable.
    CHECK(cascade::source::pluginPathWith("", {v}, ';').front() != ';');
}

void testNothingToAddYieldsNoChange() {
    CHECK(cascade::source::pluginPathWith("D:\\mine", {}, ';').empty());
}

void testTwoInstallsAreBothAdded() {
    // Someone with PothosSDR for their HackRF and radioconda for everything
    // else should get both, in candidate order.
    VendorRoot a;
    a.moduleDir = "C:\\Program Files\\PothosSDR\\lib\\SoapySDR\\modules0.8";
    VendorRoot b;
    b.moduleDir = "C:\\Users\\ham\\radioconda\\Library\\lib\\SoapySDR\\modules0.8";
    const std::string out = cascade::source::pluginPathWith("", {a, b}, ';');
    CHECK(out == a.moduleDir + ";" + b.moduleDir);
}

}  // namespace

int main() {
    testCandidatesCoverTheInstallersPeopleActuallyUse();
    testAnExplicitRootOutranksEveryGuess();
    testAnEmptyEnvironmentProbesNothingAndDoesNotCrash();
    testOnlyRootsThatActuallyHoldModulesAreAdopted();
    testTheAbiVersionIsPartOfThePath();
    testTheSameInstallReachedTwiceIsListedOnce();
    testPluginPathAppendsAndNeverReplaces();
    testPluginPathDoesNotAddADirectoryTwice();
    testPluginPathHandlesAnEmptyStartingValue();
    testNothingToAddYieldsNoChange();
    testTwoInstallsAreBothAdded();
    return testSummary("test_soapy_modules");
}
