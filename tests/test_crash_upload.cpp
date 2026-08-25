// Tests for core/crash_upload.hpp - the half of the diagnostics feature that
// carries a captured report off the machine.
//
// Most of these are NEGATIVE, because every rule the owner set is a rule about
// something NOT happening:
//
//   - nothing is sent when the Settings switch is off (asserted against the
//     REAL binary, with a socket listening that must never be connected to);
//   - a minidump is never uploaded, ever (a .dmp with recognisable bytes sits
//     beside a report, and no request body may contain them);
//   - a machine in a crash loop stops sending before the server has to say so;
//   - a 429 that does arrive is honoured;
//   - a dead OR HANGING endpoint does not block, crash, or delay shutdown -
//     measured against the real binary, because a unit test that calls the
//     transport directly cannot see the shutdown path at all.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/crash_upload.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <winsock2.h>

#include <windows.h>

#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

namespace fs = std::filesystem;
using namespace cascade::core;

namespace {

// BOUNDS-SAFE INDEXING. CHECK records and continues, so v[0] guarded only by a
// preceding size CHECK is an out-of-bounds read in exactly the run that has
// something to report - the test then dies at 0xC0000005 instead of naming the
// broken expectation. This project has lost red-phase runs to that twice.
template <typename T>
T at(const std::vector<T>& v, std::size_t i) {
    return (i < v.size()) ? v[i] : T();
}

// THE SAME RULE FOR JSON, and it was needed immediately: the red phase feeds
// these assertions an EMPTY payload, and nlohmann::json::parse throws on that,
// which kills the process instead of failing the checks. Parsing without
// exceptions and normalising a failure to an empty object keeps every
// assertion below live and failing on its own terms - it is not a guard the
// assertions hide inside, because they all still run.
nlohmann::json parseOrEmpty(const std::string& s) {
    nlohmann::json j = nlohmann::json::parse(s, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { return nlohmann::json::object(); }
    return j;
}

nlohmann::json arrayAt(const nlohmann::json& j, const char* key) {
    if (j.contains(key) && j[key].is_array()) { return j[key]; }
    return nlohmann::json::array();
}

std::string jsonStr(const nlohmann::json& arr, std::size_t i, const char* key) {
    if (i >= arr.size() || !arr[i].is_object()) { return std::string(); }
    return arr[i].value(key, std::string());
}

std::uint64_t jsonNum(const nlohmann::json& arr, std::size_t i, const char* key) {
    if (i >= arr.size() || !arr[i].is_object()) { return 0; }
    return arr[i].value(key, static_cast<std::uint64_t>(0));
}

// ---------------------------------------------------------------------------
// PRIVACY.md, parsed
// ---------------------------------------------------------------------------
//
// The inventory in the CODE and the payload have been compared both ways since
// phase one, and that pair can still be wrong TOGETHER: rename a field in both
// and the document silently describes something the application no longer
// sends. PRIVACY.md is the promise a user actually reads, so it is the third
// party to the comparison rather than a copy nobody checks.
std::string readPrivacyDoc() {
    std::ifstream in(fs::path(CASCADE_SOURCE_DIR) / "PRIVACY.md", std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<std::string> backtickedIn(const std::string& cell) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (true) {
        const std::size_t a = cell.find('`', at);
        if (a == std::string::npos) { break; }
        const std::size_t b = cell.find('`', a + 1);
        if (b == std::string::npos) { break; }
        const std::string tok = cell.substr(a + 1, b - a - 1);
        // Field names only: an example value in the same cell (`0.62.0`,
        // `crash`) is not a field, and neither is prose.
        bool ident = !tok.empty();
        for (char c : tok) {
            if (!std::isalnum(static_cast<unsigned char>(c))) { ident = false; }
        }
        if (ident) { out.push_back(tok); }
        at = b + 1;
    }
    return out;
}

// The uploaded-payload table's first column (top-level fields), and the second
// column of its `context` row (the context sub-fields).
void documentedUploadFields(std::set<std::string>& top, std::set<std::string>& context) {
    const std::string doc = readPrivacyDoc();
    const std::size_t start = doc.find("## What is sent when a report is uploaded");
    if (start == std::string::npos) { return; }
    const std::size_t end = doc.find("\n## ", start + 4);
    const std::string section = doc.substr(start, (end == std::string::npos) ? end : end - start);

    std::size_t at = 0;
    while (at < section.size()) {
        const std::size_t nl = section.find('\n', at);
        const std::string line =
            section.substr(at, (nl == std::string::npos) ? std::string::npos : nl - at);
        at = (nl == std::string::npos) ? section.size() : nl + 1;
        if (line.rfind("| `", 0) != 0) { continue; }
        // "| a | b | c |" -> cells after the leading pipe.
        std::vector<std::string> cells;
        std::size_t p = 1;
        while (p <= line.size()) {
            const std::size_t bar = line.find('|', p);
            if (bar == std::string::npos) { break; }
            cells.push_back(line.substr(p, bar - p));
            p = bar + 1;
        }
        if (cells.empty()) { continue; }
        const std::vector<std::string> names = backtickedIn(cells[0]);
        bool isContextRow = false;
        for (const std::string& n : names) {
            top.insert(n);
            if (n == "context") { isContextRow = true; }
        }
        if (isContextRow && cells.size() > 1) {
            for (const std::string& n : backtickedIn(cells[1])) { context.insert(n); }
        }
    }
}

fs::path scratchDir(const char* tag) {
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const unsigned long pid = 0;
#endif
    const fs::path dir =
        base / (std::string("cascade-upload-") + tag + "-" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeFile(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

// A crash report in EXACTLY the shape crash_handler.cpp writes one: the header
// lines its inventory declares, the shared context block, one stack, the module
// table with build ids, and the log ring. Written out by hand rather than
// produced by faulting a child process, because these tests are about what
// happens to a report AFTER it exists - tests/test_crash_capture.cpp already
// proves a real fault produces this shape.
std::string crashReportText(const std::string& signature = "0123456789ABCDEF",
                            const std::string& logLine = "source opened") {
    return "kind: crash\n"
           "reason: access violation\n"
           "code: 0xC0000005\n"
           "address: cascade.exe+0x1A2B\n"
           "signature: " + signature + "\n"
           "thread: 24180\n"
           "--- context ---\n"
           "version: 0.62.0\n"
           "commit: abc123def456\n"
           "os: Windows 10.0.22631\n"
           "arch: x64\n"
           "mode: WFM\n"
           "source: soapy\n"
           "sample-rate: 2400000\n"
           "device-open: yes\n"
           "sdr-model: uhd b200\n"
           "plugin: ADS-B 1.1.0\n"
           "plugin: APRS 1.0.0\n"
           "--- stack (thread 24180) ---\n"
           "  cascade.exe+0x1A2B\n"
           "  cascade.exe+0x5C10\n"
           "  0x00007FFAB0001234\n"
           "--- modules ---\n"
           "  cascade.exe base=0x00007FF700000000 size=0x2C8000 pdb=cascade.pdb "
           "build=651FD5EB776649E7B91461B1EB1EB8C525\n"
           "  ntdll.dll base=0x00007FFAB0000000 size=0x200000 pdb=ntdll.pdb build=(none)\n"
           "--- log (last 2 of 4011 lines) ---\n" +
           logLine + "\n" +
           "plugin started\n";
}

// The other writer's shape: hang_watchdog.cpp. Multiple thread sections, a
// different header, and the same context block.
std::string hangReportText() {
    return "kind: hang\n"
           "stalled-ms: 7213\n"
           "threshold-ms: 5000\n"
           "signature: FEDCBA9876543210\n"
           "threads: 2\n"
           "--- context ---\n"
           "version: 0.62.0\n"
           "commit: abc123def456\n"
           "os: Windows 10.0.22631\n"
           "arch: x64\n"
           "mode: NFM\n"
           "source: generator\n"
           "sample-rate: 1000000\n"
           "device-open: no\n"
           "sdr-model: (none)\n"
           "plugin: (none)\n"
           "--- modules ---\n"
           "  cascade.exe base=0x00007FF700000000 size=0x2C8000 pdb=cascade.pdb "
           "build=651FD5EB776649E7B91461B1EB1EB8C525\n"
           "--- thread 100 (gui, stalled) ---\n"
           "  cascade.exe+0x9999\n"
           "--- thread 200 (watchdog) ---\n"
           "  cascade.exe+0x1111\n"
           "--- log (last 1 of 7 lines) ---\n"
           "watchdog armed\n";
}

#if defined(_WIN32)

// ---------------------------------------------------------------------------
// A local HTTP stub, so every transport assertion is made against a real
// socket rather than a mock. The three behaviours that matter are the three
// the wire contract names plus the one it cannot: a server that accepts the
// connection and never answers.
// ---------------------------------------------------------------------------
class StubServer {
public:
    enum class Mode { Accept204, RateLimit429, TooLarge413, Hang, Refuse };

    bool start(Mode mode) {
        mode_ = mode;
        WSADATA wsa{};
        ::WSAStartup(MAKEWORD(2, 2), &wsa);
        listen_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_ == INVALID_SOCKET) { return false; }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            return false;
        }
        int len = sizeof(addr);
        ::getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ::ntohs(addr.sin_port);
        if (mode_ == Mode::Refuse) {
            // Nothing listens. The port was bound only to learn a number that
            // is very unlikely to be in use, then released - which is as close
            // to "the server is down" as a test can get deterministically.
            ::closesocket(listen_);
            listen_ = INVALID_SOCKET;
            return true;
        }
        if (::listen(listen_, 8) != 0) { return false; }
        run_ = true;
        thread_ = std::thread([this] { loop(); });
        return true;
    }

    void stop() {
        run_ = false;
        if (listen_ != INVALID_SOCKET) {
            ::closesocket(listen_);
            listen_ = INVALID_SOCKET;
        }
        if (thread_.joinable()) { thread_.join(); }
        for (SOCKET s : held_) { ::closesocket(s); }
        held_.clear();
    }

    ~StubServer() { stop(); }

    int port() const { return port_; }
    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/api/crash";
    }
    int connections() const { return connections_.load(); }
    std::vector<std::string> bodies() {
        std::lock_guard<std::mutex> lk(mu_);
        return bodies_;
    }

private:
    void loop() {
        while (run_) {
            fd_set rd;
            FD_ZERO(&rd);
            if (listen_ == INVALID_SOCKET) { break; }
            FD_SET(listen_, &rd);
            timeval tv{0, 100 * 1000};
            const int n = ::select(0, &rd, nullptr, nullptr, &tv);
            if (n <= 0) { continue; }
            SOCKET c = ::accept(listen_, nullptr, nullptr);
            if (c == INVALID_SOCKET) { continue; }
            connections_.fetch_add(1);
            std::string req;
            // Read headers, then exactly Content-Length bytes.
            char buf[4096];
            std::size_t contentLength = 0;
            std::size_t headerEnd = std::string::npos;
            while (run_) {
                const int got = ::recv(c, buf, sizeof(buf), 0);
                if (got <= 0) { break; }
                req.append(buf, buf + got);
                if (headerEnd == std::string::npos) {
                    headerEnd = req.find("\r\n\r\n");
                    if (headerEnd != std::string::npos) {
                        const std::size_t at = lowerFind(req, "content-length:");
                        if (at != std::string::npos) {
                            contentLength = static_cast<std::size_t>(
                                std::strtoull(req.c_str() + at + 15, nullptr, 10));
                        }
                    }
                }
                if (headerEnd != std::string::npos &&
                    req.size() >= headerEnd + 4 + contentLength) {
                    break;
                }
            }
            if (headerEnd != std::string::npos) {
                std::lock_guard<std::mutex> lk(mu_);
                bodies_.push_back(req.substr(headerEnd + 4));
            }
            if (mode_ == Mode::Hang) {
                // Accepted, read, and DELIBERATELY never answered. The client
                // must not sit here, and must not make the application sit
                // here either.
                held_.push_back(c);
                continue;
            }
            const char* resp = "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n";
            if (mode_ == Mode::RateLimit429) {
                resp =
                    "HTTP/1.1 429 Too Many Requests\r\nRetry-After: 120\r\n"
                    "Content-Length: 0\r\n\r\n";
            } else if (mode_ == Mode::TooLarge413) {
                resp = "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\n\r\n";
            }
            ::send(c, resp, static_cast<int>(std::strlen(resp)), 0);
            ::shutdown(c, SD_BOTH);
            ::closesocket(c);
        }
    }

    static std::size_t lowerFind(const std::string& hay, const std::string& needle) {
        std::string l;
        l.reserve(hay.size());
        for (char ch : hay) {
            l.push_back((ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch);
        }
        return l.find(needle);
    }

    Mode mode_ = Mode::Accept204;
    SOCKET listen_ = INVALID_SOCKET;
    int port_ = 0;
    std::atomic<bool> run_{false};
    std::atomic<int> connections_{0};
    std::thread thread_;
    std::mutex mu_;
    std::vector<std::string> bodies_;
    std::vector<SOCKET> held_;
};

// Runs the SHIPPED binary with the diagnostics tree and the config redirected
// into caller-owned scratch directories, and returns how long it took. This is
// the only way to see the shutdown path: every unit test in this file calls the
// transport directly and so cannot tell whether closing the window waits for it.
double runAppMs(const fs::path& diagDir, const fs::path& cfgPath, const std::string& crashUrl,
                int frames) {
    ::SetEnvironmentVariableA("FOXSDR_DIAG_DIR", diagDir.string().c_str());
    ::SetEnvironmentVariableA("CASCADE_CONFIG_TEST", cfgPath.string().c_str());
    ::SetEnvironmentVariableA("FOXSDR_CRASH_URL",
                              crashUrl.empty() ? nullptr : crashUrl.c_str());
    const std::string exe = std::string(CASCADE_APP_BINDIR) + "/cascade.exe";
    const std::string cmd = "\"\"" + exe + "\" --frames " + std::to_string(frames) + " 2>&1\"";
    const auto t0 = std::chrono::steady_clock::now();
    FILE* p = _popen(cmd.c_str(), "r");
    char buf[512];
    while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { /* drained */ }
    if (p != nullptr) { _pclose(p); }
    const auto t1 = std::chrono::steady_clock::now();
    ::SetEnvironmentVariableA("FOXSDR_DIAG_DIR", nullptr);
    ::SetEnvironmentVariableA("CASCADE_CONFIG_TEST", nullptr);
    ::SetEnvironmentVariableA("FOXSDR_CRASH_URL", nullptr);
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

void writeConfig(const fs::path& p, bool diagnostics) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << "{\n";
    out << "  \"schemaVersion\": 1,\n";
    out << "  \"diagnosticsEnabled\": " << (diagnostics ? "true" : "false") << ",\n";
    out << "  \"diagnosticsMinidump\": false,\n";
    out << "  \"telemetryEnabled\": false\n";
    out << "}\n";
}

#endif  // _WIN32

}  // namespace

int main() {
    // --- The report parses back into the fields the wire contract needs -----
    {
        ParsedReport r;
        CHECK(parseReportText(crashReportText(), r));
        CHECK(r.kind == "crash");
        CHECK(r.signature == "0123456789ABCDEF");
        CHECK(r.version == "0.62.0");
        CHECK(r.commit == "abc123def456");
        CHECK(r.os == "Windows 10.0.22631");
        CHECK(r.arch == "x64");
        // The faulting module and offset, taken apart from the address line -
        // the offset is what a PDB turns back into a function, and a report
        // that carried "cascade.exe+0x1A2B" as one opaque string would make
        // every consumer parse it again.
        CHECK(r.module == "cascade.exe");
        CHECK(r.offset == 0x1A2Bull);
        // ...and the BUILD ID of that module, which is the only key that finds
        // the right PDB. Read from the report's own module table.
        CHECK(r.buildId == "651FD5EB776649E7B91461B1EB1EB8C525");
        CHECK(r.mode == "WFM");
        CHECK(r.source == "soapy");
        CHECK(r.sampleRateHz == 2400000.0);
        CHECK(r.deviceOpen);
        CHECK(r.sdrModel == "uhd b200");
        CHECK(r.plugins.size() == 2);
        CHECK(at(r.plugins, 0) == "ADS-B 1.1.0");
        CHECK(r.threads.size() == 1);
        CHECK(at(r.threads, 0).id == 24180);
        CHECK(at(r.threads, 0).frames.size() == 3);
        CHECK(at(at(r.threads, 0).frames, 0).module == "cascade.exe");
        CHECK(at(at(r.threads, 0).frames, 0).offset == 0x1A2Bull);
        CHECK(at(at(r.threads, 0).frames, 0).buildId == "651FD5EB776649E7B91461B1EB1EB8C525");
        // A frame in no known module keeps its raw address rather than being
        // dropped: "we could not attribute this" is information.
        CHECK(at(at(r.threads, 0).frames, 2).module.empty());
        CHECK(at(at(r.threads, 0).frames, 2).rawAddress == 0x00007FFAB0001234ull);
        // "(none)" is the report's way of writing "no PDB recorded" and must
        // not travel as a literal build id.
        CHECK(at(at(r.threads, 0).frames, 1).buildId == "651FD5EB776649E7B91461B1EB1EB8C525");
        CHECK(r.log.size() == 2);
        CHECK(at(r.log, 0) == "source opened");
    }

    // --- A freeze report is the same payload from a different writer --------
    {
        ParsedReport r;
        CHECK(parseReportText(hangReportText(), r));
        CHECK(r.kind == "hang");
        CHECK(r.signature == "FEDCBA9876543210");
        CHECK(r.threads.size() == 2);
        CHECK(at(r.threads, 0).id == 100);
        CHECK(at(r.threads, 1).id == 200);
        // The stalled thread's top frame is what the signature was built from,
        // so it is what `module`/`offset` name.
        CHECK(r.module == "cascade.exe");
        CHECK(r.offset == 0x9999ull);
        CHECK(r.mode == "NFM");
        CHECK(!r.deviceOpen);
        // "plugin: (none)" is not a plugin.
        CHECK(r.plugins.empty());
    }

    // --- Rubbish is refused rather than posted ------------------------------
    {
        ParsedReport r;
        CHECK(!parseReportText("", r));
        CHECK(!parseReportText("this is not a report\n", r));
        CHECK(!parseReportText("kind: something-else\nsignature: X\n", r));
        // A report truncated by a handler that died mid-write still has the
        // identifying half, and that half is worth sending.
        ParsedReport half;
        CHECK(parseReportText("kind: crash\nreason: access violation\ncode: 0xC0000005\n"
                              "address: cascade.exe+0x1A2B\nsignature: AAAA\nthread: 1\n",
                              half));
        CHECK(half.signature == "AAAA");
        CHECK(half.threads.empty());
    }

    // --- THE PAYLOAD INVENTORY, BOTH DIRECTIONS ----------------------------
    //
    // PRIVACY.md documents the uploaded payload field by field. A field added
    // to the payload must fail here, and so must a field the document claims
    // that the payload stopped emitting.
    {
        ParsedReport r;
        CHECK(parseReportText(crashReportText(), r));
        const std::string js = uploadJson(r, "4f9c1d2e3a4b5c6d7e8f90a1b2c3d4e5");
        CHECK(!js.empty());
        const nlohmann::json j = parseOrEmpty(js);

        std::set<std::string> actual;
        for (auto it = j.begin(); it != j.end(); ++it) { actual.insert(it.key()); }
        const std::set<std::string> declared(uploadFieldNames().begin(),
                                             uploadFieldNames().end());
        CHECK(!declared.empty());
        CHECK(actual == declared);

        std::set<std::string> ctxActual;
        if (j.contains("context") && j["context"].is_object()) {
            for (auto it = j["context"].begin(); it != j["context"].end(); ++it) {
                ctxActual.insert(it.key());
            }
        }
        const std::set<std::string> ctxDeclared(uploadContextFieldNames().begin(),
                                                uploadContextFieldNames().end());
        CHECK(!ctxDeclared.empty());
        CHECK(ctxActual == ctxDeclared);

        // The contract, checked value by value rather than by shape alone.
        CHECK(j.value("schema", 0) == 1);
        CHECK(j.value("kind", std::string()) == "crash");
        CHECK(j.value("version", std::string()) == "0.62.0");
        CHECK(j.value("commit", std::string()) == "abc123def456");
        CHECK(j.value("buildId", std::string()) == "651FD5EB776649E7B91461B1EB1EB8C525");
        CHECK(j.value("module", std::string()) == "cascade.exe");
        CHECK(j.value("offset", 0ull) == 0x1A2Bull);
        CHECK(j.value("signature", std::string()) == "0123456789ABCDEF");
        CHECK(j.value("os", std::string()) == "Windows 10.0.22631");
        CHECK(j.value("arch", std::string()) == "x64");
        CHECK(j.value("installId", std::string()) == "4f9c1d2e3a4b5c6d7e8f90a1b2c3d4e5");
        CHECK(arrayAt(j, "plugins").size() == 2);
        CHECK(jsonStr(arrayAt(j, "plugins"), 0, "name") == "ADS-B");
        CHECK(jsonStr(arrayAt(j, "plugins"), 0, "version") == "1.1.0");
        CHECK(arrayAt(j, "log").size() == 2);
        CHECK(arrayAt(j, "threads").size() == 1);
        const nlohmann::json thread0 =
            arrayAt(j, "threads").empty() ? nlohmann::json::object() : arrayAt(j, "threads")[0];
        CHECK(arrayAt(thread0, "frames").size() == 3);
        CHECK(jsonNum(arrayAt(thread0, "frames"), 0, "offset") == 0x1A2Bull);

        // NEVER a tuned frequency, and never anything decoded. The same
        // absence tests/test_diagnostics.cpp makes of the local report, made
        // of the thing that actually leaves the machine.
        CHECK(js.find("frequency") == std::string::npos);
        CHECK(js.find("centerHz") == std::string::npos);
        CHECK(js.find("bookmark") == std::string::npos);

        // ...AND THE SAME COMPARISON AGAINST PRIVACY.md ITSELF, both ways. The
        // code inventory and the payload can be wrong together - rename a field
        // in both and the document quietly describes something the application
        // no longer sends. The document is the promise a user reads, so it is a
        // third party to the comparison rather than a copy nobody checks.
        {
            std::set<std::string> docTop;
            std::set<std::string> docContext;
            documentedUploadFields(docTop, docContext);
            CHECK(!docTop.empty());
            CHECK(!docContext.empty());
            for (const std::string& f : docTop) { std::printf("privacy field: %s\n", f.c_str()); }
            CHECK(docTop == actual);
            CHECK(docContext == ctxActual);
        }

        // An install id that does not exist is sent EMPTY, not minted: usage
        // reporting being off is exactly the state in which there is no id,
        // and a crash report must not be what creates one.
        const nlohmann::json anon = parseOrEmpty(uploadJson(r, std::string()));
        CHECK(anon.value("installId", std::string("x")).empty());
    }

    // --- The payload is capped locally, not by making the server say 413 ---
    {
        ParsedReport r;
        CHECK(parseReportText(crashReportText(), r));
        // A pathological report: a log ring of long lines and a deep stack.
        r.log.clear();
        for (int i = 0; i < 400; ++i) { r.log.push_back(std::string(400, 'x')); }
        ReportThread t;
        t.id = 7;
        for (int i = 0; i < 400; ++i) {
            ReportFrame f;
            f.module = "cascade.exe";
            f.buildId = "651FD5EB776649E7B91461B1EB1EB8C525";
            f.offset = static_cast<std::uint64_t>(i) * 16u;
            t.frames.push_back(f);
        }
        r.threads.assign(24, t);
        const std::string js = uploadJson(r, "4f9c1d2e3a4b5c6d7e8f90a1b2c3d4e5");
        CHECK(js.size() <= kMaxUploadBytes);
        // ...and the identifying half survives the trimming. A capped payload
        // that dropped the signature would group nothing.
        const nlohmann::json j = parseOrEmpty(js);
        CHECK(j.value("signature", std::string()) == "0123456789ABCDEF");
        CHECK(j.value("buildId", std::string()) == "651FD5EB776649E7B91461B1EB1EB8C525");
    }

    // --- Client-side dedup: the same fault twice is sent once ---------------
    {
        UploadPolicyState st;
        const std::uint64_t t0 = 1'700'000'000ull;
        CHECK(decideUpload(st, true, "AAAA", t0) == UploadDecision::Send);
        noteSent(st, "AAAA", t0);
        // The identical fault, one second later - a crash loop.
        CHECK(decideUpload(st, true, "AAAA", t0 + 1) == UploadDecision::Duplicate);
        CHECK(decideUpload(st, true, "AAAA", t0 + kDedupSeconds - 1) ==
              UploadDecision::Duplicate);
        // A DIFFERENT fault is not a duplicate.
        CHECK(decideUpload(st, true, "BBBB", t0 + 1) == UploadDecision::Send);
        // ...and after the dedup window the same one is worth having again:
        // "still crashing a day later" is new information.
        CHECK(decideUpload(st, true, "AAAA", t0 + kDedupSeconds + 1) == UploadDecision::Send);
    }

    // --- Client-side rate limit: distinct faults are capped too -------------
    {
        UploadPolicyState st;
        const std::uint64_t t0 = 1'700'000'000ull;
        for (int i = 0; i < kMaxPerWindow; ++i) {
            const std::string sig = "SIG" + std::to_string(i);
            CHECK(decideUpload(st, true, sig, t0 + static_cast<std::uint64_t>(i)) ==
                  UploadDecision::Send);
            noteSent(st, sig, t0 + static_cast<std::uint64_t>(i));
        }
        CHECK(decideUpload(st, true, "SIGX", t0 + 10) == UploadDecision::RateLimited);
        // The window rolls.
        CHECK(decideUpload(st, true, "SIGX", t0 + kWindowSeconds + 1) == UploadDecision::Send);
    }

    // --- A 429 is honoured ---------------------------------------------------
    {
        UploadPolicyState st;
        const std::uint64_t t0 = 1'700'000'000ull;
        noteRateLimited(st, t0, 120);
        CHECK(st.blockedUntil == t0 + 120);
        CHECK(decideUpload(st, true, "AAAA", t0 + 1) == UploadDecision::Backoff);
        CHECK(decideUpload(st, true, "AAAA", t0 + 121) == UploadDecision::Send);

        // No Retry-After: a default, not "immediately".
        UploadPolicyState st2;
        noteRateLimited(st2, t0, 0);
        CHECK(st2.blockedUntil == t0 + kDefaultBackoffSeconds);
        // A hostile header can neither disable the limit nor mute the client
        // for a year.
        UploadPolicyState st3;
        noteRateLimited(st3, t0, 10ull * 365ull * 24ull * 3600ull);
        CHECK(st3.blockedUntil == t0 + kMaxBackoffSeconds);
    }

    // --- Off means off, in the decision itself ------------------------------
    {
        UploadPolicyState st;
        CHECK(decideUpload(st, false, "AAAA", 1'700'000'000ull) == UploadDecision::Disabled);
    }

    // --- The policy state survives a restart, and is bounded on the way in --
    {
        UploadPolicyState st;
        const std::uint64_t t0 = 1'700'000'000ull;
        noteSent(st, "AAAA", t0);
        const std::vector<std::string> enc = encodePolicyRecent(st);
        CHECK(enc.size() == 1);
        const UploadPolicyState back =
            decodePolicyState(enc, st.windowStart, st.windowCount, st.blockedUntil);
        CHECK(decideUpload(back, true, "AAAA", t0 + 1) == UploadDecision::Duplicate);

        // Hand-edited rubbish in the config cannot make the limiter forget.
        std::vector<std::string> junk = {"", "no-timestamp", "AAAA notanumber",
                                         std::string(500, 'z')};
        const UploadPolicyState j = decodePolicyState(junk, 0, 0, 0);
        CHECK(j.recent.size() <= kMaxRecentSignatures);

        // ...and an over-long list is truncated rather than loaded whole.
        std::vector<std::string> many;
        for (int i = 0; i < 400; ++i) {
            many.push_back("SIG" + std::to_string(i) + " " + std::to_string(t0));
        }
        const UploadPolicyState m = decodePolicyState(many, 0, 0, 0);
        CHECK(m.recent.size() <= kMaxRecentSignatures);
    }

#if defined(_WIN32)
    // --- A real POST to a real socket, and a real 204 ----------------------
    {
        StubServer srv;
        CHECK(srv.start(StubServer::Mode::Accept204));
        auto cancel = std::make_shared<UploadCancel>();
        const UploadResult res = postCrashReport(srv.url(), "{\"schema\":1}", cancel);
        CHECK(res.attempted);
        CHECK(res.accepted);
        CHECK(res.status == 204);
        CHECK(srv.connections() == 1);
        CHECK(at(srv.bodies(), 0) == "{\"schema\":1}");
        srv.stop();
    }

    // --- A 429 is read, not ignored ----------------------------------------
    {
        StubServer srv;
        CHECK(srv.start(StubServer::Mode::RateLimit429));
        auto cancel = std::make_shared<UploadCancel>();
        const UploadResult res = postCrashReport(srv.url(), "{\"schema\":1}", cancel);
        CHECK(res.attempted);
        CHECK(!res.accepted);
        CHECK(res.status == 429);
        CHECK(res.rateLimited);
        CHECK(res.retryAfterSeconds == 120);
        srv.stop();
    }

    // --- A hanging endpoint is abandoned, not waited out --------------------
    //
    // The server accepts the connection, reads the body and never answers.
    // Cancelling must return the caller in milliseconds, not at the receive
    // timeout - that difference IS the shutdown promise.
    {
        StubServer srv;
        CHECK(srv.start(StubServer::Mode::Hang));
        auto cancel = std::make_shared<UploadCancel>();
        const auto t0 = std::chrono::steady_clock::now();
        std::thread worker([&] {
            const UploadResult res = postCrashReport(srv.url(), "{\"schema\":1}", cancel);
            CHECK(!res.accepted);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        cancel->cancel();
        worker.join();
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        std::printf("cancel returned after %.0f ms\n", ms);
        CHECK(ms < 2000.0);
        srv.stop();
    }

    // --- Plain http off the loopback is refused ----------------------------
    {
        auto cancel = std::make_shared<UploadCancel>();
        const UploadResult res = postCrashReport("http://example.com/api/crash", "{}", cancel);
        CHECK(!res.attempted);
    }

    // --- THE SWEEP: off means off ------------------------------------------
    {
        const fs::path dir = scratchDir("off");
        writeFile(dir / "crash-20260825-120000-1234-1.txt", crashReportText());
        StubServer srv;
        CHECK(srv.start(StubServer::Mode::Accept204));

        SweepParams p;
        p.crashDir = dir.string();
        p.url = srv.url();
        p.enabled = false;  // the Settings switch
        p.nowEpoch = 1'700'000'000ull;
        auto cancel = std::make_shared<UploadCancel>();
        const SweepOutcome out = sweepCrashDir(p, cancel);

        CHECK(out.considered == 0);
        CHECK(out.sent == 0);
        // Not a single connection, and NOT A SINGLE FILE either: a sidecar
        // saying "declined" would still be this feature writing to a machine
        // whose owner switched it off.
        CHECK(srv.connections() == 0);
        int files = 0;
        for (const auto& e : fs::directory_iterator(dir)) {
            (void)e;
            ++files;
        }
        CHECK(files == 1);
        srv.stop();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // --- THE SWEEP: a minidump is never uploaded ---------------------------
    {
        const fs::path dir = scratchDir("dump");
        const fs::path report = dir / "crash-20260825-120000-1234-1.txt";
        writeFile(report, crashReportText());
        // A dump with bytes nothing else could produce, so its presence in a
        // request body would be unmistakable.
        writeFile(dir / "crash-20260825-120000-1234-1.dmp",
                  "MDMP\x93\xa7SECRET-IQ-AND-FILE-PATHS-SECRET");

        StubServer srv;
        CHECK(srv.start(StubServer::Mode::Accept204));
        SweepParams p;
        p.crashDir = dir.string();
        p.url = srv.url();
        p.enabled = true;
        p.installId = "4f9c1d2e3a4b5c6d7e8f90a1b2c3d4e5";
        p.nowEpoch = 1'700'000'000ull;
        auto cancel = std::make_shared<UploadCancel>();
        const SweepOutcome out = sweepCrashDir(p, cancel);

        CHECK(out.sent == 1);
        CHECK(srv.connections() == 1);
        const std::vector<std::string> bodies = srv.bodies();
        CHECK(bodies.size() == 1);
        for (const std::string& b : bodies) {
            CHECK(b.find("SECRET-IQ-AND-FILE-PATHS-SECRET") == std::string::npos);
            CHECK(b.find("MDMP") == std::string::npos);
        }
        // The dump is still exactly where the user can find it.
        CHECK(fs::exists(dir / "crash-20260825-120000-1234-1.dmp"));
        // ...and no sidecar was written for it: the sweep never looked at it.
        CHECK(!fs::exists(dir / "crash-20260825-120000-1234-1.dmp.upload"));
        // The report's own sidecar records what happened, in words.
        const std::string side = readFile(uploadSidecarPath(report.string()));
        CHECK(side.find("status: sent") != std::string::npos);
        srv.stop();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // --- THE SWEEP: a report is swept once, not on every start -------------
    {
        const fs::path dir = scratchDir("once");
        writeFile(dir / "crash-20260825-120000-1234-1.txt", crashReportText());
        StubServer srv;
        CHECK(srv.start(StubServer::Mode::Accept204));
        SweepParams p;
        p.crashDir = dir.string();
        p.url = srv.url();
        p.enabled = true;
        p.nowEpoch = 1'700'000'000ull;
        auto cancel = std::make_shared<UploadCancel>();
        const SweepOutcome first = sweepCrashDir(p, cancel);
        CHECK(first.sent == 1);
        // The next start: the same directory, the same report, the state the
        // first sweep produced.
        p.state = first.state;
        const SweepOutcome second = sweepCrashDir(p, cancel);
        CHECK(second.sent == 0);
        CHECK(second.considered == 0);
        CHECK(srv.connections() == 1);
        srv.stop();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // --- THE SWEEP: a crash loop stops itself ------------------------------
    //
    // Twelve restarts, each finding one more report with the SAME signature -
    // which is what a machine that crashes on start actually produces. The
    // client must stop sending on its own; the server never has to say 429.
    {
        const fs::path dir = scratchDir("loop");
        StubServer srv;
        CHECK(srv.start(StubServer::Mode::Accept204));
        SweepParams p;
        p.crashDir = dir.string();
        p.url = srv.url();
        p.enabled = true;
        auto cancel = std::make_shared<UploadCancel>();
        int duplicates = 0;
        for (int i = 0; i < 12; ++i) {
            writeFile(dir / ("crash-20260825-1200" + std::to_string(i) + "-1234-1.txt"),
                      crashReportText());
            p.nowEpoch = 1'700'000'000ull + static_cast<std::uint64_t>(i) * 30ull;
            const SweepOutcome out = sweepCrashDir(p, cancel);
            p.state = out.state;
            duplicates += out.duplicate;
        }
        CHECK(srv.connections() == 1);
        CHECK(duplicates == 11);
        srv.stop();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // --- THE SWEEP: a 429 stops the rest of the sweep and is remembered ----
    {
        const fs::path dir = scratchDir("429");
        writeFile(dir / "crash-20260825-120000-1234-1.txt", crashReportText("AAAA0000AAAA0000"));
        writeFile(dir / "crash-20260825-120001-1234-1.txt", crashReportText("BBBB0000BBBB0000"));
        StubServer srv;
        CHECK(srv.start(StubServer::Mode::RateLimit429));
        SweepParams p;
        p.crashDir = dir.string();
        p.url = srv.url();
        p.enabled = true;
        p.nowEpoch = 1'700'000'000ull;
        auto cancel = std::make_shared<UploadCancel>();
        const SweepOutcome out = sweepCrashDir(p, cancel);
        CHECK(out.sent == 0);
        // One attempt, then stop: carrying on through the directory after a
        // 429 is exactly the behaviour the header exists to prevent.
        CHECK(srv.connections() == 1);
        CHECK(out.state.blockedUntil == 1'700'000'000ull + 120ull);
        // ...and the next start honours it without opening a socket at all.
        SweepParams p2 = p;
        p2.state = out.state;
        p2.nowEpoch = 1'700'000'000ull + 60ull;
        const SweepOutcome out2 = sweepCrashDir(p2, cancel);
        CHECK(out2.sent == 0);
        CHECK(srv.connections() == 1);
        srv.stop();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // --- THE SWEEP: a report that cannot be sent is neither lost nor -------
    // --- retried for ever --------------------------------------------------
    {
        const fs::path dir = scratchDir("dead");
        const fs::path report = dir / "crash-20260825-120000-1234-1.txt";
        writeFile(report, crashReportText());
        StubServer srv;
        CHECK(srv.start(StubServer::Mode::Refuse));  // nothing is listening
        SweepParams p;
        p.crashDir = dir.string();
        p.url = srv.url();
        p.enabled = true;
        auto cancel = std::make_shared<UploadCancel>();
        for (int i = 0; i < kMaxAttempts + 2; ++i) {
            p.nowEpoch = 1'700'000'000ull + static_cast<std::uint64_t>(i) * 3600ull;
            const SweepOutcome out = sweepCrashDir(p, cancel);
            p.state = out.state;
        }
        const std::string side = readFile(uploadSidecarPath(report.string()));
        CHECK(side.find("status: abandoned") != std::string::npos);
        CHECK(side.find("attempts: " + std::to_string(kMaxAttempts)) != std::string::npos);
        // THE REPORT ITSELF IS UNTOUCHED. "Open reports folder" and "Copy
        // diagnostics" still work; the user can still send it by hand.
        CHECK(fs::exists(report));
        CHECK(readFile(report) == crashReportText());
        // And a further start does not attempt it again.
        p.nowEpoch = 1'700'000'000ull + 100'000ull;
        const SweepOutcome out = sweepCrashDir(p, cancel);
        CHECK(out.considered == 0);
        srv.stop();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // --- THE REAL BINARY: nothing is sent with the switch off ---------------
    //
    // A unit test cannot prove this. main() decides whether diagnostics are
    // armed BEFORE AppWindow exists, and the question is whether the shipped
    // ordering honours the stored switch - so the shipped binary is what is
    // run, with a socket listening that must never be connected to.
    {
        const fs::path root = scratchDir("app-off");
        const fs::path diagDir = root / "diag";
        const fs::path cfg = root / "config.json";
        writeConfig(cfg, false);  // diagnostics OFF
        fs::create_directories(diagDir / "crashes");
        writeFile(diagDir / "crashes" / "crash-20260825-120000-1234-1.txt", crashReportText());

        StubServer srv;
        CHECK(srv.start(StubServer::Mode::Accept204));
        runAppMs(diagDir, cfg, srv.url(), 3);
        CHECK(srv.connections() == 0);
        CHECK(srv.bodies().empty());
        srv.stop();
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // --- THE REAL BINARY: with the switch on, the report goes ---------------
    {
        const fs::path root = scratchDir("app-on");
        const fs::path diagDir = root / "diag";
        const fs::path cfg = root / "config.json";
        writeConfig(cfg, true);
        fs::create_directories(diagDir / "crashes");
        const fs::path report = diagDir / "crashes" / "crash-20260825-120000-1234-1.txt";
        writeFile(report, crashReportText("CAFEBABECAFEBABE"));

        StubServer srv;
        CHECK(srv.start(StubServer::Mode::Accept204));
        runAppMs(diagDir, cfg, srv.url(), 20);
        CHECK(srv.connections() == 1);
        const std::vector<std::string> bodies = srv.bodies();
        CHECK(bodies.size() == 1);
        CHECK(at(bodies, 0).find("CAFEBABECAFEBABE") != std::string::npos);
        CHECK(fs::exists(uploadSidecarPath(report.string())));
        srv.stop();
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // --- THE REAL BINARY: a hanging endpoint does not delay shutdown -------
    //
    // THE ONE THAT MATTERS MOST, and the one no unit test can make. The server
    // accepts the connection and never answers; the application must close in
    // the same time it closes with nothing to send. Measured, not asserted.
    {
        const fs::path root = scratchDir("app-hang");
        const fs::path diagDir = root / "diag";
        const fs::path cfg = root / "config.json";

        // Control: identical run, no pending report, no endpoint.
        writeConfig(cfg, true);
        fs::create_directories(diagDir / "crashes");
        const double control = runAppMs(diagDir, cfg, std::string(), 20);

        // The same run with a report waiting and a server that never answers.
        const fs::path root2 = scratchDir("app-hang2");
        const fs::path diagDir2 = root2 / "diag";
        const fs::path cfg2 = root2 / "config.json";
        writeConfig(cfg2, true);
        fs::create_directories(diagDir2 / "crashes");
        writeFile(diagDir2 / "crashes" / "crash-20260825-120000-1234-1.txt", crashReportText());
        StubServer srv;
        CHECK(srv.start(StubServer::Mode::Hang));
        const double hung = runAppMs(diagDir2, cfg2, srv.url(), 20);
        CHECK(srv.connections() == 1);  // it really did try
        srv.stop();

        std::printf("app run: control %.0f ms, hanging endpoint %.0f ms\n", control, hung);
        // A second of slack for process-start jitter. A client that waited out
        // its receive timeout would be several seconds over, and one that
        // waited for the server would never return at all.
        CHECK(hung - control < 1500.0);

        std::error_code ec;
        fs::remove_all(root, ec);
        fs::remove_all(root2, ec);
    }
#endif  // _WIN32

    // --- THE OFFER DIALOG SAYS ONLY WHAT ACTUALLY HAPPENS -------------------
    //
    // The dialog shown after an unclean exit is the ONE piece of disclosure a
    // user reads at the moment a report is being handled, and it is the one
    // sentence in the product that can be falsified by the sweep's own
    // decisions. A sweep that answers Duplicate, RateLimited, Backoff, refused
    // or too-large sends NOTHING, and all five are ordinary outcomes on the
    // machine most likely to see this dialog - one that has just crashed
    // twice. A dialog that says a report "is being sent now" is therefore
    // wrong on exactly the machines it appears on most.
    //
    // The copy is checked as SOURCE TEXT because it lives in an ImGui call
    // that cannot be rendered headlessly here, which is the same reason
    // PRIVACY.md is read as text above. The extraction is asserted to have
    // found something first: a "does not contain" check against an empty
    // string passes vacuously, which would make this whole section a test that
    // cannot fail.
    {
        std::ifstream in(fs::path(CASCADE_SOURCE_DIR) / "src" / "gui" / "app_window.cpp",
                         std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        const std::size_t fn = src.find("void AppWindow::drawDiagnosticsOffer()");
        CHECK(fn != std::string::npos);
        const std::size_t end = src.find("\nvoid AppWindow::", fn + 20);
        const std::string offer =
            (fn == std::string::npos)
                ? std::string()
                : src.substr(fn, (end == std::string::npos) ? std::string::npos : end - fn);
        CHECK(offer.size() > 200);
        CHECK(offer.find("ImGui::TextWrapped") != std::string::npos);

        // THE SENTENCE THE USER READS, not the source that produces it.
        // Matching the raw function text had two faults, both found in review.
        // It matched COMMENTS: `find("ever")` was satisfied by the word
        // "never" in the rationale above, so it still passed with the promise
        // "No memory dump is sent, ever" deleted outright - an assertion that
        // could not fail. And it matched across LITERAL BOUNDARIES: adjacent
        // "..." chunks are joined by the compiler but not by a substring
        // search, so re-wrapping a line broke phrases that were still on
        // screen unchanged. Both disappear once the literals are concatenated
        // the way the compiler concatenates them: what is asserted is then the
        // text, and where the author happened to press return cannot affect it.
        const std::string dialog = [&offer]() {
            const std::size_t call = offer.find("ImGui::TextWrapped(");
            if (call == std::string::npos) { return std::string(); }
            std::string out;
            bool inLiteral = false;
            for (std::size_t i = call; i < offer.size(); ++i) {
                const char c = offer[i];
                if (!inLiteral && c == ')') { break; }   // end of the call
                if (c == '"' && (i == 0 || offer[i - 1] != '\\')) {
                    inLiteral = !inLiteral;
                    continue;
                }
                if (inLiteral) { out += c; }
            }
            return out;
        }();
        // Non-empty, or every assertion below would pass vacuously - the same
        // trap this block already guards against for `offer`.
        CHECK(dialog.size() > 100);
        CHECK(dialog.find("//") == std::string::npos);  // no comment leaked in

        // The claim that fails whenever the sweep declines to send.
        CHECK(dialog.find("is being sent") == std::string::npos);
        CHECK(dialog.find("being sent now") == std::string::npos);
        // ...replaced by a statement of the ATTEMPT rather than the outcome.
        // "is sent unless X or Y" still promises delivery whenever neither
        // exception applies, and the commonest reason of all was missing from
        // that list: the machine cannot reach the site. Reproduced against the
        // real binary with a blackholed endpoint - nothing sent, neither named
        // exception true. So the copy must say it TRIES, and must name
        // unreachability alongside the other two.
        CHECK(dialog.find("TRY to send") != std::string::npos);
        CHECK(dialog.find("It is not sent if") != std::string::npos);
        CHECK(dialog.find("repeats one already sent") != std::string::npos);
        CHECK(dialog.find("over its send limit") != std::string::npos);
        CHECK(dialog.find("cannot be reached") != std::string::npos);
        // NOTHING IS SILENTLY LOST: the dialog must say where the report is
        // when it is not sent. Matched on the sentence rather than on the
        // words "reports folder", which the button label already satisfies -
        // an assertion a button can pass is not an assertion about the copy.
        CHECK(dialog.find("stays in the reports folder") != std::string::npos);
        // The two promises that were already right and must stay: no dump,
        // ever, and the switch that turns the whole thing off.
        // THE WHOLE PHRASE, not the bare word "ever". The extraction covers
        // this function's comment block too, and the comments contain "never",
        // so `find("ever")` matched even with the promise deleted outright -
        // an assertion that could not fail, dressed as one that could.
        CHECK(dialog.find("No memory dump is sent, ever") != std::string::npos);
        CHECK(dialog.find("Settings > Diagnostics") != std::string::npos);
    }

    return testSummary("test_crash_upload");
}
