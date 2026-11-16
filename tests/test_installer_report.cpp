// Tests for the installer's failed/cancelled/blocked report (engine lane E1, owner decision 15): installer/install-report.iss and its hooks in installer/cascade.iss.
// The Inno Setup compiler is not on the build machine (nothing is downloaded), so this is a SOURCE test: it proves the report is OFF in the shipped script, that the
// message holds exactly the three allowed facts and nothing that names a person, a machine, a path or an error, that it is gated to interactive installs, and that the
// body is the contract's envelope (schema 2, a kind, a messageId, t, a body). Argument: the source root.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>

#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); }
bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// The non-comment lines of an .iss file (a line starting with ';' or '//' after blanks is a comment).
std::string codeLines(const std::string& text) {
    std::string out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) { end = text.size(); }
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        std::size_t a = 0;
        while (a < line.size() && (line[a] == ' ' || line[a] == '\t' || line[a] == '\r')) { ++a; }
        if (a >= line.size() || line[a] == ';' || line.compare(a, 2, "//") == 0) { continue; }
        out += line + "\n";
    }
    return out;
}

void run(const fs::path& root) {
    const std::string iss = slurp(root / "installer" / "cascade.iss");
    const std::string rep = slurp(root / "installer" / "install-report.iss");
    CHECK(!iss.empty());
    CHECK(!rep.empty());
    const std::string issCode = codeLines(iss);
    const std::string repCode = codeLines(rep);

    // 1. OFF: the define is a comment in the shipped script, so none of the report is compiled in.
    CHECK(has(iss, ";#define ReportInstallOutcome"));
    CHECK(!std::regex_search(issCode, std::regex("(^|\n)\s*#define\s+ReportInstallOutcome")));
    // the include and every call are behind the define
    CHECK(has(iss, "#ifdef ReportInstallOutcome\r\n#include \"install-report.iss\"") || has(iss, "#ifdef ReportInstallOutcome\n#include \"install-report.iss\""));
    CHECK(has(iss, "ReportNoteStep(CurStep);"));
    {
        const std::size_t call = iss.find("ReportNoteStep(CurStep);");
        const std::size_t guard = iss.rfind("#ifdef ReportInstallOutcome", call);
        const std::size_t endif = iss.find("#endif", guard);
        CHECK(guard != std::string::npos && guard < call && call < endif);
    }

    // 2. THE MESSAGE: the envelope of docs/radio-reports-contract.md and the three facts, nothing else.
    CHECK(has(repCode, "\"schema\":2"));
    CHECK(has(repCode, "\"kind\":\"install_outcome\""));
    CHECK(has(repCode, "\"messageId\":\""));
    CHECK(has(repCode, "\"t\":0"));
    CHECK(has(repCode, "\"body\":{\"outcome\":\""));
    CHECK(has(repCode, "\"appVersion\":\"{#AppVersion}\""));
    CHECK(has(repCode, "\"windows\":\""));
    // the only outcomes
    CHECK(has(repCode, "'cancelled'") && has(repCode, "'failed'") && has(repCode, "'blocked'"));
    // the body's field names, counted: schema kind messageId t body outcome appVersion windows - eight keys and no ninth
    std::size_t keys = 0;
    const std::regex keyRe("\"[A-Za-z]+\":");
    for (std::sregex_iterator it(repCode.begin(), repCode.end(), keyRe), e; it != e; ++it) { ++keys; }
    CHECK(keys == 8);
    // nothing that names a person, a machine, a path, an address or an error goes into the body (the function that builds it is checked, not the gate)
    const std::size_t bf = repCode.find("function ReportBody");
    const std::size_t bg = repCode.find("procedure ReportSend");
    CHECK(bf != std::string::npos && bg != std::string::npos && bf < bg);
    const std::string bodyFn = repCode.substr(bf, bg - bf);
    for (const char* bad : {"{username}", "{computername}", "{userdocs}", "{userappdata}", "{localappdata}", "{src}", "{srcexe}", "{app}", "{pf", "{sys}", "{win}",
                            "GetUserNameString", "GetComputerNameString", "installId", "sessionId", "radioKey", "GetExceptionMessage", "SysErrorMessage"}) {
        CHECK(!has(bodyFn, bad));
    }
    // the body is built from exactly: the random id, the outcome word, the version constant, the Windows version numbers
    {
        const std::size_t f = repCode.find("function ReportBody");
        const std::size_t g = repCode.find("procedure ReportSend");
        CHECK(f != std::string::npos && g != std::string::npos && f < g);
        const std::string body = repCode.substr(f, g - f);
        CHECK(has(body, "ReportRandomHex(16)"));
        CHECK(has(body, "V.Major") && has(body, "V.Minor") && has(body, "V.Build"));
        CHECK(!has(body, "ExpandConstant"));
        CHECK(!has(body, "GetEnv"));
    }

    // 3. A SECURE, BOUNDED REQUEST, WITH NO HELPER PROGRAM
    CHECK(has(iss, "#define ReportInstallUrl \"https://"));
    CHECK(!has(iss, "#define ReportInstallUrl \"http://"));
    CHECK(has(repCode, "WinHttp.WinHttpRequest.5.1"));
    CHECK(has(repCode, "SetTimeouts("));
    CHECK(has(repCode, "'POST'"));
    CHECK(has(repCode, "except"));   // a failure to report is logged, never raised
    CHECK(!has(repCode, "Exec("));
    CHECK(!has(repCode, "ShellExec"));
    CHECK(!has(repCode, "DownloadTemporaryFile"));
    CHECK(!has(repCode, "idp"));

    // 4. WHEN: interactive installs only, once, only if the install did not finish
    CHECK(has(repCode, "not WizardSilent"));
    CHECK(has(repCode, "{param:noreport|0}"));
    CHECK(has(repCode, "{param:dataroots|}"));
    CHECK(has(repCode, "if ReportRan then"));
    CHECK(has(repCode, "procedure DeinitializeSetup"));
    CHECK(has(repCode, "procedure CancelButtonClick"));
    CHECK(has(repCode, "ReportFinished"));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: test_installer_report <source root>\n"); return 2; }
    run(fs::path(argv[1]));
    return testSummary("test_installer_report");
}
