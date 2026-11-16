// test_companion_programs.cpp - THE COMPANION APPLICATIONS' PROGRAMS (src/core/companion_programs.{hpp,cpp} and its hooks in core/plugin_repo.cpp), against a STAND-IN
// CATALOGUE and a stand-in transport: no network, no download, no real companion program (the "program" here is a few hundred bytes of PE header made in memory, never run).
//
//   C1  the catalogue's row: the companion object is read (upstream, licence, why, source, identifier line, program); every unusable program block (not published, a
//       published one with no address, no hash, a short hash, an http address, a name that is a library or holds a separator) is "not available"; an ordinary entry has none
//   C2  the check: the right bytes pass; wrong bytes, a file gone, a folder where a file belongs and an EMPTY or malformed expectation never pass
//   C3  the website copy: installing the bridge downloads the program beside it with the catalogue's hash, records it, and the state reads Installed
//   C4  all or nothing: a wrong hash, a program that is not a program of this machine, a transfer that fails half way, a cancel, an unpublished program and a bridge that
//       cannot be moved in each leave the plugins folder as it was
//   C5  before every start: the right program runs; a program swapped after install, truncated, deleted, or recorded with no hash is refused with a plain sentence and
//       counted on the fault path; put back, it runs again
//   C6  the Store copy: the program is found INSIDE the package and copied into place, and NOT ONE transport call is made; a program the package lacks is refused; a copy
//       swapped in the helpers folder is put right from the package; a package program that changed is refused
//   C7  the owner's signature on a row: built and inert (no key = not enforced), enforced when a key is present (missing, wrong and right)
//   C8  the record: remove deletes the program and forgets the record; a program another record still names is kept; an update replaces and a failed update restores
//   C9  the shelf's states (unavailable, package, download, installed, changed) and their sentences
//   C10 hostile input: a package list with a path that climbs, a record with a bad name, a companion object of the wrong type
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "core/companion_programs.hpp"
#include "core/plugin_repo.hpp"
#include "core/sys_manifest_sig.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#define TEST_MODULE_EXT ".dll"
#else
#define TEST_MODULE_EXT ".so"
#endif

namespace fs = std::filesystem;
namespace cp = cascade::core::companion;
using cascade::core::PluginCatalogEntry;
using cascade::core::PluginRepo;

namespace {

fs::path g_dir;

std::string sha(const std::string& bytes) {
    std::string hex, err;
    CHECK(PluginRepo::sha256Hex(bytes.data(), bytes.size(), hex, err));
    return hex;
}

std::string program(const std::string& tag = "") {
    std::string b;
    const std::string os = PluginRepo::hostOs();
    const std::string arch = PluginRepo::hostArch();
    if (os == "windows") {
        b.assign(512, '\0');
        b[0] = 'M';
        b[1] = 'Z';
        b[0x3C] = static_cast<char>(0x80);
        std::memcpy(&b[0x80], "PE\0\0", 4);
        const unsigned m = arch == "arm64" ? 0xAA64u : arch == "x86" ? 0x014Cu : 0x8664u;
        b[0x84] = static_cast<char>(m & 0xFF);
        b[0x85] = static_cast<char>((m >> 8) & 0xFF);
    } else {
        b.assign(64, '\0');
        b[0] = 0x7F;
        b[1] = 'E';
        b[2] = 'L';
        b[3] = 'F';
        b[4] = 2;
        b[18] = static_cast<char>(arch == "arm64" ? 183 : 62);
    }
    // the tag sits after the headers, so two programs of one kind differ in their bytes and their hash
    b.resize(b.size() + tag.size());
    std::memcpy(&b[b.size() - tag.size()], tag.data(), tag.size());
    return b;
}

// a "program" of another machine's kind
std::string foreignProgram() {
    std::string b(512, '\0');
    b[0] = 'M';
    b[1] = 'Z';
    b[0x3C] = static_cast<char>(0x80);
    std::memcpy(&b[0x80], "PE\0\0", 4);
    const bool x86 = std::string(PluginRepo::hostArch()) == "x86";
    const unsigned m = x86 ? 0x8664u : 0x014Cu;
    b[0x84] = static_cast<char>(m & 0xFF);
    b[0x85] = static_cast<char>((m >> 8) & 0xFF);
    return b;
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
void put(const fs::path& p, const std::string& bytes) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::string> tree(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
        if (e.is_regular_file()) { out.push_back(fs::relative(e.path(), dir).generic_string()); }
    }
    std::sort(out.begin(), out.end());
    return out;
}

const char* kSource = "https://github.com/wonderingStars/foxsdr-companions";
const char* kProgUrl = "https://dl.example.invalid/companions/multimon-helper.exe";

std::vector<std::string> sorted(std::vector<std::string> v) {
    std::sort(v.begin(), v.end());
    return v;
}

struct World {
    std::map<std::string, std::string> files;
    std::atomic<int> hits{0};
    std::atomic<int> programHits{0};   // asks for the PROGRAM's address
    std::string failUrl, halfUrl;
    std::atomic<bool>* cancelOnFirst = nullptr;
    void install() {
        PluginRepo::setTransportForTest([this](const std::string& url, std::uint64_t maxBytes, const std::function<bool(const void*, std::size_t)>& sink, std::string& error) {
            ++hits;
            if (url == kProgUrl) { ++programHits; }
            if (cancelOnFirst != nullptr) { cancelOnFirst->store(true); }
            if (url == failUrl) {
                error = "connection reset";
                return false;
            }
            const auto it = files.find(url);
            if (it == files.end()) {
                error = "HTTP 404";
                return false;
            }
            if (it->second.size() > maxBytes) {
                error = "too large";
                return false;
            }
            if (url == halfUrl) {
                sink(it->second.data(), it->second.size() / 2);
                error = "connection reset";
                return false;
            }
            return sink(it->second.data(), it->second.size());
        });
    }
    ~World() { PluginRepo::setTransportForTest(nullptr); }
};


// M1 (security review 1): the website copy downloads only what the owner signed. Every row of this file is signed with a key of its own unless a test says
// otherwise (g_autoSign), and main() sets that key as the trusted one.
cascade::core::sys::Ed25519Seed g_seed{};
bool g_autoSign = true;

struct Spec {
    std::string progFile = "multimon-helper.exe";
    std::string progUrl = kProgUrl;
    std::string progSha;         // "" = of `progBytes`
    std::string status = "published";
    std::string signature;
    std::string progBytes;
    std::string dll = "stand-in bridge";
    bool omitProgram = false;
    std::string extraDescription = " This program cannot hide identifiers: whatever it decodes (callsigns, addresses, serial numbers, message text) is shown exactly as the program prints it.";
};

std::string indexOf(World& w, const Spec& s) {
    const std::string dllName = std::string("companion-multimon-0.1.2-abi3-") + PluginRepo::hostOs() + "-" + PluginRepo::hostArch() + TEST_MODULE_EXT;
    const std::string dllUrl = "https://dl.example.invalid/plugins/" + dllName;
    w.files[dllUrl] = s.dll;
    if (s.progUrl == kProgUrl) { w.files[s.progUrl] = s.progBytes; }
    const std::string psha = s.progSha.empty() ? sha(s.progBytes) : s.progSha;
    const std::string autoSig = (g_autoSign && s.signature.empty())
                                    ? cascade::core::sys::makeEnvelope(g_seed, cp::signedStatement("companion-multimon", s.progFile, psha))
                                    : s.signature;
    std::string companion = std::string("\"companion\":{\"upstream\":\"multimon-ng\",\"source\":\"") + kSource +
                            "\",\"licence\":\"GPL-2.0-or-later\",\"whySeparate\":\"This is another person's open-source program and stays an application of its own.\"," +
                            "\"program\":{\"file\":\"" + s.progFile + "\",\"url\":\"" + s.progUrl + "\",\"sha256\":\"" + psha + "\",\"status\":\"" + s.status + "\"" +
                            (autoSig.empty() ? std::string() : ",\"signature\":" + nlohmann::json(autoSig).dump()) + "}}";
    if (s.omitProgram) { companion = std::string("\"companion\":{\"upstream\":\"multimon-ng\",\"source\":\"") + kSource + "\",\"licence\":\"GPL-2.0-or-later\",\"whySeparate\":\"x\"}"; }
    return std::string("{\"schemaVersion\":1,\"plugins\":[{\"id\":\"companion-multimon\",\"name\":\"Pager tones - multimon-ng companion\",\"version\":\"0.1.2\",\"author\":\"FoxSDR project\",") +
           "\"licence\":\"MIT\",\"abiVersion\":" + std::to_string(CASCADE_PLUGIN_ABI_VERSION) + ",\"category\":\"companions\",\"summary\":\"Runs multimon-ng.\",\"description\":\"A companion plugin." +
           s.extraDescription + " The program's source is published.\"," + companion + ",\"platforms\":[{\"os\":\"" + PluginRepo::hostOs() + "\",\"arch\":\"" + PluginRepo::hostArch() +
           "\",\"file\":\"" + dllName + "\",\"url\":\"" + dllUrl + "\",\"sha256\":\"" + sha(s.dll) + "\",\"sizeBytes\":10}]}]}";
}

std::string bridgeFile() { return std::string("companion-multimon-0.1.2-abi3-") + PluginRepo::hostOs() + "-" + PluginRepo::hostArch() + TEST_MODULE_EXT; }

bool parseOne(const std::string& json, PluginCatalogEntry& e, std::string& err) {
    std::vector<PluginCatalogEntry> es;
    if (!PluginRepo::parseIndex(json, es, err) || es.size() != 1u) { return false; }
    e = es[0];
    return true;
}

bool installOne(World& w, const std::string& json, const fs::path& dir, std::string& err, PluginRepo* keep = nullptr) {
    PluginCatalogEntry e;
    if (!parseOne(json, e, err)) { err = "parse: " + err; return false; }
    w.install();
    PluginRepo local;
    PluginRepo& repo = keep != nullptr ? *keep : local;
    std::string path;
    return repo.install(e, dir.string(), path, err);
}

// ---- C1 ----------------------------------------------------------------------------------------------------------------------------------
void testParse() {
    std::printf("[C1] the catalogue's row\n");
    World w;
    Spec s;
    s.progBytes = program();
    PluginCatalogEntry e;
    std::string err;
    CHECK(parseOne(indexOf(w, s), e, err));
    CHECK(e.companion.present);
    CHECK(e.companion.upstream == "multimon-ng");
    CHECK(e.companion.licence == "GPL-2.0-or-later");
    CHECK(e.companion.source == kSource);
    CHECK(e.companion.whySeparate.find("open-source program") != std::string::npos);
    CHECK(e.companion.program.file == "multimon-helper.exe" && e.companion.program.usable);
    CHECK(e.companion.identifierLine.rfind("This program cannot hide identifiers", 0) == 0);
    CHECK(e.companion.identifierLine.back() == '.' && e.companion.identifierLine.find("The program's source") == std::string::npos);
    CHECK(e.category == "companions");

    // a row with no such sentence has no identifier line
    Spec none = s;
    none.extraDescription = "";
    CHECK(parseOne(indexOf(w, none), e, err) && e.companion.identifierLine.empty());

    // every unusable program block is "not available": never a download
    struct Bad { const char* why; std::function<void(Spec&)> shape; };
    const std::vector<Bad> bad = {
        {"unpublished", [](Spec& x) { x.status = "unpublished"; }},
        {"unpublished with an address and a hash", [](Spec& x) { x.status = "unpublished"; }},
        {"no address", [](Spec& x) { x.progUrl = ""; }},
        {"http address", [](Spec& x) { x.progUrl = "http://dl.example.invalid/m.exe"; }},
        {"short hash", [](Spec& x) { x.progSha = "abc"; }},
        {"empty hash status", [](Spec& x) { x.progSha = std::string(63, 'a'); }},
        {"a library", [](Spec& x) { x.progFile = "multimon.dll"; }},
        {"a separator", [](Spec& x) { x.progFile = "sub/multimon.exe"; }},
        {"a backslash", [](Spec& x) { x.progFile = "sub\\\\multimon.exe"; }},
        {"dotdot", [](Spec& x) { x.progFile = "..multimon.exe"; }},
        {"not an exe (Windows)", [](Spec& x) { if (std::string(PluginRepo::hostOs()) == "windows") { x.progFile = "multimon.bat"; } else { x.progFile = "a b"; } }},
        {"no program block", [](Spec& x) { x.omitProgram = true; }},
        {"status of another word", [](Spec& x) { x.status = "available"; }},
    };
    for (const Bad& b : bad) {
        Spec x = s;
        b.shape(x);
        PluginCatalogEntry ee;
        const bool parsed = parseOne(indexOf(w, x), ee, err);
        if (!parsed) { std::printf("      NOT PARSED: %s (%s)\n", b.why, err.c_str()); }
        CHECK(parsed);   // the CATALOGUE is never refused for it
        const bool usable = parsed && ee.companion.present && ee.companion.program.usable;
        if (usable) { std::printf("      USABLE: %s\n", b.why); }
        CHECK(!usable);
    }
    // the source link: an address that is not https is not shown
    {
        std::string doc = indexOf(w, s);
        const std::size_t at = doc.find(std::string("\"source\":\"") + kSource);
        doc.replace(at, std::string("\"source\":\"").size() + std::strlen(kSource), "\"source\":\"http://x.invalid/s");
        CHECK(parseOne(doc, e, err) && e.companion.present && e.companion.source.empty());
    }
    // an ordinary entry has no companion object
    const std::string plain = std::string("{\"schemaVersion\":1,\"plugins\":[{\"id\":\"p\",\"name\":\"P\",\"version\":\"1.0.0\",\"licence\":\"MIT\",\"abiVersion\":") + std::to_string(CASCADE_PLUGIN_ABI_VERSION) + "}]}";
    CHECK(parseOne(plain, e, err) && !e.companion.present);
    // a companion key of the wrong type is dropped, not fatal
    const std::string wrong = std::string("{\"schemaVersion\":1,\"plugins\":[{\"id\":\"p\",\"name\":\"P\",\"version\":\"1.0.0\",\"licence\":\"MIT\",\"abiVersion\":") + std::to_string(CASCADE_PLUGIN_ABI_VERSION) +
                              ",\"companion\":[1,2]}]}";
    CHECK(parseOne(wrong, e, err) && !e.companion.present);
    const std::string wrong2 = std::string("{\"schemaVersion\":1,\"plugins\":[{\"id\":\"p\",\"name\":\"P\",\"version\":\"1.0.0\",\"licence\":\"MIT\",\"abiVersion\":") + std::to_string(CASCADE_PLUGIN_ABI_VERSION) +
                               ",\"companion\":{\"upstream\":5,\"licence\":[],\"program\":\"x\"}}]}";
    CHECK(parseOne(wrong2, e, err) && e.companion.present && !e.companion.program.usable && e.companion.upstream.empty());
}

// ---- C2 ----------------------------------------------------------------------------------------------------------------------------------
void testCheck() {
    std::printf("[C2] the check\n");
    const fs::path d = g_dir / "c2";
    fs::create_directories(d);
    const std::string bytes = program("c2");
    put(d / "a.exe", bytes);
    const std::string h = sha(bytes);
    CHECK(cp::checkProgram(d / "a.exe", h).kind == cp::Check::Ok);
    std::string upper = h;
    for (char& c : upper) { if (c >= 'a' && c <= 'f') { c = static_cast<char>(c - 'a' + 'A'); } }
    CHECK(cp::checkProgram(d / "a.exe", upper).kind == cp::Check::Ok);   // case of the hex digits does not matter
    CHECK(cp::checkProgram(d / "a.exe", sha("something else")).kind == cp::Check::Mismatch);
    CHECK(cp::checkProgram(d / "gone.exe", h).kind == cp::Check::Missing);
    fs::create_directories(d / "folder.exe");
    CHECK(cp::checkProgram(d / "folder.exe", h).kind == cp::Check::Missing);   // a folder where a file belongs
    CHECK(cp::checkProgram(d / "a.exe", "").kind == cp::Check::NoHash);        // an EMPTY expectation never passes, not even for an empty file
    put(d / "empty.exe", "");
    CHECK(cp::checkProgram(d / "empty.exe", "").kind == cp::Check::NoHash);
    CHECK(cp::checkProgram(d / "a.exe", std::string(64, 'z')).kind == cp::Check::NoHash);
    CHECK(cp::checkProgram(d / "a.exe", h.substr(0, 63)).kind == cp::Check::NoHash);
    CHECK(cp::checkProgram(d / "empty.exe", sha("")).kind == cp::Check::Ok);
    // one flipped bit is a different file
    std::string flipped = bytes;
    flipped[flipped.size() - 1] ^= 1;
    put(d / "b.exe", flipped);
    CHECK(cp::checkProgram(d / "b.exe", h).kind == cp::Check::Mismatch);
    // the words
    CHECK(cp::refusalSentence(cp::Check::Mismatch, "a.exe").find("changed on disk") != std::string::npos);
    CHECK(cp::refusalSentence(cp::Check::Missing, "a.exe").find("not where it was installed") != std::string::npos);
    CHECK(cp::refusalSentence(cp::Check::NoHash, "a.exe").find("no checksum") != std::string::npos);
    CHECK(cp::refusalSentence(cp::Check::Ok, "a.exe").empty());
    fs::remove_all(d);
}

// ---- C3 ----------------------------------------------------------------------------------------------------------------------------------
void testWebsiteInstall() {
    std::printf("[C3] the website copy downloads the program\n");
    cp::setPackagedForTest(0);
    const fs::path dir = g_dir / "c3";
    fs::create_directories(dir);
    World w;
    Spec s;
    s.progBytes = program("c3");
    std::string err;
    std::atomic<float> progress{0.0f};
    PluginRepo repo;
    CHECK(installOne(w, indexOf(w, s), dir, err, &repo));
    if (!err.empty()) { std::printf("      %s\n", err.c_str()); }
    CHECK(w.hits == 2 && w.programHits == 1);   // the bridge and the program, nothing else
    CHECK(fs::exists(dir / bridgeFile()));
    CHECK(slurp(dir / "helpers" / "multimon-helper.exe") == s.progBytes);
    CHECK(tree(dir) == sorted({"companions.json", "helpers/multimon-helper.exe", bridgeFile()}));   // no .part, no staging
    cp::Record rec;
    CHECK(cp::readRecord(dir.string(), bridgeFile(), rec));
    CHECK(rec.id == "companion-multimon" && rec.file == "multimon-helper.exe" && rec.sha256 == sha(s.progBytes) && rec.size == s.progBytes.size() && rec.via == "download");
    // the state, before and after
    PluginCatalogEntry e;
    CHECK(parseOne(indexOf(w, s), e, err));
    const cp::Places places = cp::placesFor(dir.string());
    CHECK(!places.packaged);
    CHECK(cp::stateOf(e, places, {}, bridgeFile()) == cp::State::Installed);
    // and it runs: the check before a start allows it
    const cp::StartVerdict v = cp::checkBeforeStart((dir / bridgeFile()).string());
    CHECK(v.allowed && v.sentence.empty());
    fs::remove_all(dir);
}

// ---- C4 ----------------------------------------------------------------------------------------------------------------------------------
void testAllOrNothing() {
    std::printf("[C4] all or nothing\n");
    cp::setPackagedForTest(0);
    struct Case {
        const char* name;
        std::function<void(World&, Spec&)> shape;
        int programHitsAtMost;          // -1 = any
        const char* sentence;           // part of the error
    };
    const std::vector<Case> cases = {
        {"wrong hash in the catalogue", [](World&, Spec& s) { s.progSha = sha("not the program"); }, -1, "integrity"},
        {"not a program of this machine", [](World&, Spec& s) { s.progBytes = foreignProgram(); }, -1, "machine"},
        {"not a program at all", [](World&, Spec& s) { s.progBytes = "this is a text file"; }, -1, "not a"},
        {"unpublished", [](World&, Spec& s) { s.status = "unpublished"; }, 0, "not available"},
        {"no hash", [](World&, Spec& s) { s.progSha = ""; s.status = "unpublished"; }, 0, "not available"},
        {"the transfer fails", [](World& w, Spec&) { w.failUrl = kProgUrl; }, -1, "connection reset"},
        {"the transfer fails half way", [](World& w, Spec&) { w.halfUrl = kProgUrl; }, -1, "connection reset"},
        {"the server has no such file", [](World& w, Spec&) { w.files.erase(kProgUrl); w.failUrl = ""; }, -1, "404"},
    };
    for (const Case& c : cases) {
        const fs::path dir = g_dir / "c4";
        fs::remove_all(dir);
        fs::create_directories(dir);
        World w;
        Spec s;
        s.progBytes = program("c4");
        std::string doc;
        c.shape(w, s);
        doc = indexOf(w, s);
        c.shape(w, s);   // (a case that edits the served files after the index made them)
        if (std::string(c.name) == "the server has no such file") { w.files.erase(kProgUrl); }
        std::string err;
        const bool ok = installOne(w, doc, dir, err);
        if (ok) { std::printf("      INSTALLED: %s\n", c.name); }
        CHECK(!ok);
        if (!ok && std::strlen(c.sentence) > 0 && err.find(c.sentence) == std::string::npos) { std::printf("      %s: error was '%s'\n", c.name, err.c_str()); }
        CHECK(!ok && err.find(c.sentence) != std::string::npos);
        if (c.programHitsAtMost >= 0) { CHECK(w.programHits <= c.programHitsAtMost); }
        CHECK(tree(dir).empty());   // nothing left behind: no bridge, no program, no .part, no staging
        std::error_code ec;
        CHECK(!fs::exists(dir / "helpers", ec));
    }
    // a cancel: the flag is set while the program is being fetched
    {
        const fs::path dir = g_dir / "c4";
        fs::remove_all(dir);
        fs::create_directories(dir);
        World w;
        Spec s;
        s.progBytes = program("c4cancel");
        PluginCatalogEntry e;
        std::string err;
        CHECK(parseOne(indexOf(w, s), e, err));
        PluginRepo repo;
        // the stand-in sets the repo's own cancel flag through the transport's first call after the bridge: the program's fetch then sees it
        std::atomic<int> calls{0};
        PluginRepo::setTransportForTest([&](const std::string& url, std::uint64_t, const std::function<bool(const void*, std::size_t)>& sink, std::string& error) {
            const auto it = w.files.find(url);
            if (it == w.files.end()) { error = "HTTP 404"; return false; }
            if (++calls == 2) {   // the program: cancelled mid-transfer
                sink(it->second.data(), it->second.size() / 2);
                error = "cancelled";
                return false;
            }
            return sink(it->second.data(), it->second.size());
        });
        std::string path;
        CHECK(!repo.install(e, dir.string(), path, err));
        CHECK(err.find("cancelled") != std::string::npos);
        CHECK(tree(dir).empty());
        PluginRepo::setTransportForTest(nullptr);
    }
    // the bridge cannot be moved in (its name is a folder): the program goes back too, and an earlier program is restored
    {
        const fs::path dir = g_dir / "c4";
        fs::remove_all(dir);
        fs::create_directories(dir / bridgeFile());   // a folder where the bridge must go
        put(dir / "helpers" / "multimon-helper.exe", program("earlier"));
        World w;
        Spec s;
        s.progBytes = program("c4new");
        std::string err;
        CHECK(!installOne(w, indexOf(w, s), dir, err));
        CHECK(slurp(dir / "helpers" / "multimon-helper.exe") == program("earlier"));
        cp::Record rec;
        CHECK(!cp::readRecord(dir.string(), bridgeFile(), rec));
        CHECK(tree(dir) == (std::vector<std::string>{"helpers/multimon-helper.exe"}));
    }
    // a plugin SITE's entry never brings a program
    {
        const fs::path dir = g_dir / "c4";
        fs::remove_all(dir);
        fs::create_directories(dir);
        World w;
        Spec s;
        s.progBytes = program("site");
        PluginCatalogEntry e;
        std::string err;
        CHECK(parseOne(indexOf(w, s), e, err));
        e.source = "https://site.example.invalid/index.json";
        cp::Txn t{dir};
        CHECK(t.stage(e, nullptr, nullptr, err));
        CHECK(!t.active() && w.hits == 0);
    }
    fs::remove_all(g_dir / "c4");
}

// ---- C5 ----------------------------------------------------------------------------------------------------------------------------------
void testBeforeStart() {
    std::printf("[C5] the check before every start\n");
    cp::setPackagedForTest(0);
    const fs::path dir = g_dir / "c5";
    fs::remove_all(dir);
    fs::create_directories(dir);
    World w;
    Spec s;
    s.progBytes = program("c5");
    std::string err;
    CHECK(installOne(w, indexOf(w, s), dir, err));
    const std::string bridge = (dir / bridgeFile()).string();
    const fs::path prog = dir / "helpers" / "multimon-helper.exe";
    std::array<std::uint64_t, 4> sums{};
    cp::takeHostFaults(sums);   // start clean

    CHECK(cp::checkBeforeStart(bridge).allowed);   // the right bytes run
    sums = {};
    cp::takeHostFaults(sums);
    CHECK(sums[2] == 0);

    // swapped after install: refused, plainly, and counted as a BREAK
    put(prog, program("evil"));
    cp::StartVerdict v = cp::checkBeforeStart(bridge);
    CHECK(!v.allowed);
    CHECK(v.sentence.find("changed on disk") != std::string::npos && v.sentence.find("multimon-helper.exe") != std::string::npos);
    sums = {};
    cp::takeHostFaults(sums);
    CHECK(sums[0] == 0 && sums[1] == 0 && sums[2] == 1 && sums[3] == 0);
    // put back: runs again (the check is on the bytes, not on a latch)
    put(prog, s.progBytes);
    CHECK(cp::checkBeforeStart(bridge).allowed);
    // one byte appended
    put(prog, s.progBytes + "x");
    CHECK(!cp::checkBeforeStart(bridge).allowed);
    // truncated to nothing
    put(prog, "");
    CHECK(!cp::checkBeforeStart(bridge).allowed);
    // deleted
    fs::remove(prog);
    v = cp::checkBeforeStart(bridge);
    CHECK(!v.allowed && v.sentence.find("not where it was installed") != std::string::npos);
    // a folder where the program belongs
    fs::create_directories(prog);
    CHECK(!cp::checkBeforeStart(bridge).allowed);
    fs::remove_all(prog);
    put(prog, s.progBytes);
    CHECK(cp::checkBeforeStart(bridge).allowed);
    // a record with NO hash never runs anything
    cp::Record rec;
    CHECK(cp::readRecord(dir.string(), bridgeFile(), rec));
    rec.sha256 = "";
    std::string e2;
    CHECK(cp::recordProgram(dir.string(), rec, e2));
    v = cp::checkBeforeStart(bridge);
    CHECK(!v.allowed && v.sentence.find("no checksum") != std::string::npos);
    // a plugin that is not a recorded companion is not this check's business
    CHECK(cp::checkBeforeStart((dir / "something-else-1.0-abi3-win-x64.dll").string()).allowed);
    sums = {};
    cp::takeHostFaults(sums);
    CHECK(sums[2] >= 1);
    fs::remove_all(dir);
}

// ---- C6 ----------------------------------------------------------------------------------------------------------------------------------
void writePackage(const fs::path& pkg, const std::string& bytes, const std::string& id = "companion-multimon", const std::string& file = "multimon-helper.exe", const std::string& rel = "multimon/multimon-helper.exe") {
    put(pkg / rel, bytes);
    nlohmann::json j = {{"version", 1}, {"programs", nlohmann::json::array({{{"id", id}, {"file", file}, {"path", rel}, {"sha256", sha(bytes)}, {"size", bytes.size()}}})}};
    put(pkg / "companions.json", j.dump());
}

void testPackaged() {
    std::printf("[C6] the Store copy carries the programs inside its package\n");
    // M1: the Store copy needs no owner key and never fetches: its programs come out of the package, checked against the package's own list
    cp::setSignatureKeysForTest(true, {});
    const fs::path pkg = g_dir / "c6-package";
    const fs::path dir = g_dir / "c6";
    fs::remove_all(pkg);
    fs::remove_all(dir);
    fs::create_directories(dir);
    World w;
    Spec s;
    s.progBytes = program("c6-catalogue");           // what the CATALOGUE would download: never touched here
    const std::string inPackage = program("c6-package");
    writePackage(pkg, inPackage);
    cp::setPackagedForTest(1, pkg);

    PluginCatalogEntry e;
    std::string err;
    CHECK(parseOne(indexOf(w, s), e, err));
    std::vector<cp::PackageProgram> list;
    CHECK(cp::readPackageList(pkg, list, err) && list.size() == 1u);
    const cp::Places places = cp::placesFor(dir.string());
    CHECK(places.packaged && places.packageDir == pkg);
    CHECK(cp::stateOf(e, places, list, bridgeFile()) == cp::State::InPackage);

    // INSTALL: the program comes out of the package. The transport answers for the BRIDGE only; the count of calls for the program is zero.
    std::atomic<int> programCalls{0};
    std::atomic<int> allCalls{0};
    PluginRepo::setTransportForTest([&](const std::string& url, std::uint64_t, const std::function<bool(const void*, std::size_t)>& sink, std::string& error) {
        ++allCalls;
        if (url == kProgUrl) { ++programCalls; }
        const auto it = w.files.find(url);
        if (it == w.files.end()) { error = "HTTP 404"; return false; }
        return sink(it->second.data(), it->second.size());
    });
    std::string path;
    PluginRepo repo;
    CHECK(repo.install(e, dir.string(), path, err));
    if (!err.empty()) { std::printf("      %s\n", err.c_str()); }
    CHECK(programCalls == 0);                 // THE PACKAGED PATH PERFORMS NO DOWNLOAD OF THE PROGRAM
    CHECK(allCalls == 1);                     // the bridge only
    CHECK(slurp(dir / "helpers" / "multimon-helper.exe") == inPackage);
    cp::Record rec;
    CHECK(cp::readRecord(dir.string(), bridgeFile(), rec) && rec.via == "package" && rec.sha256 == sha(inPackage));
    CHECK(cp::stateOf(e, places, list, bridgeFile()) == cp::State::Installed);
    CHECK(cp::checkBeforeStart((dir / bridgeFile()).string()).allowed);

    // the copy in the helpers folder is swapped: the package is the truth, it is put right and runs
    put(dir / "helpers" / "multimon-helper.exe", program("evil"));
    CHECK(cp::checkBeforeStart((dir / bridgeFile()).string()).allowed);
    CHECK(slurp(dir / "helpers" / "multimon-helper.exe") == inPackage);
    // and deleted: copied again
    fs::remove(dir / "helpers" / "multimon-helper.exe");
    CHECK(cp::checkBeforeStart((dir / bridgeFile()).string()).allowed);
    CHECK(slurp(dir / "helpers" / "multimon-helper.exe") == inPackage);

    // the package's own program changed (a damaged package): refused, counted
    std::array<std::uint64_t, 4> sums{};
    cp::takeHostFaults(sums);
    put(pkg / "multimon" / "multimon-helper.exe", program("damaged"));
    put(dir / "helpers" / "multimon-helper.exe", program("evil2"));
    const cp::StartVerdict v = cp::checkBeforeStart((dir / bridgeFile()).string());
    CHECK(!v.allowed);
    sums = {};
    cp::takeHostFaults(sums);
    CHECK(sums[2] == 1);
    CHECK(programCalls == 0);

    // a program the package does not hold: refused with a sentence, and again no download
    {
        const fs::path dir2 = g_dir / "c6b";
        fs::remove_all(dir2);
        fs::create_directories(dir2);
        const fs::path emptyPkg = g_dir / "c6-empty-package";
        fs::remove_all(emptyPkg);
        writePackage(emptyPkg, inPackage, "companion-other", "other.exe", "other/other.exe");
        cp::setPackagedForTest(1, emptyPkg);
        std::string err2, path2;
        PluginRepo repo2;
        CHECK(!repo2.install(e, dir2.string(), path2, err2));
        CHECK(err2.find("does not include the program") != std::string::npos);
        CHECK(programCalls == 0);
        CHECK(tree(dir2).empty());
        const std::vector<cp::PackageProgram> otherList = {{"companion-other", "other.exe", "other/other.exe", sha(inPackage), inPackage.size()}};
        CHECK(cp::stateOf(e, cp::placesFor(dir2.string()), otherList, bridgeFile()) == cp::State::NotAvailable);
        // no list at all
        const fs::path noList = g_dir / "c6-nolist";
        fs::remove_all(noList);
        fs::create_directories(noList);
        cp::setPackagedForTest(1, noList);
        CHECK(!repo2.install(e, dir2.string(), path2, err2));
        CHECK(err2.find("no list of companion programs") != std::string::npos);
        CHECK(programCalls == 0 && tree(dir2).empty());
    }
    // the catalogue says unpublished, the package holds it: the PACKAGE decides
    {
        const fs::path dir3 = g_dir / "c6c";
        fs::remove_all(dir3);
        fs::create_directories(dir3);
        cp::setPackagedForTest(1, pkg);
        writePackage(pkg, inPackage);
        Spec u = s;
        u.status = "unpublished";
        PluginCatalogEntry eu;
        CHECK(parseOne(indexOf(w, u), eu, err));
        CHECK(!eu.companion.program.usable);
        CHECK(cp::stateOf(eu, cp::placesFor(dir3.string()), list, bridgeFile()) == cp::State::InPackage);
        PluginRepo repo3;
        std::string err3, path3;
        CHECK(repo3.install(eu, dir3.string(), path3, err3));
        CHECK(programCalls == 0);
    }
    PluginRepo::setTransportForTest(nullptr);
    cp::setPackagedForTest(-1);
}

// ---- C7 ----------------------------------------------------------------------------------------------------------------------------------
void testSignature() {
    std::printf("[C7] the owner's signature on a row\n");
    cp::setPackagedForTest(0);
    cascade::core::sys::Ed25519Seed seed{};
    for (std::size_t i = 0; i < seed.size(); ++i) { seed[i] = static_cast<std::uint8_t>(i + 1); }
    const cascade::core::sys::Ed25519PublicKey pub = cascade::core::sys::ed25519PublicKey(seed);
    Spec base;
    base.progBytes = program("c7");
    const std::string h = sha(base.progBytes);
    const std::string good = cascade::core::sys::makeEnvelope(seed, cp::signedStatement("companion-multimon", "multimon-helper.exe", h));
    const std::string forOther = cascade::core::sys::makeEnvelope(seed, cp::signedStatement("companion-multimon", "other.exe", h));
    cascade::core::sys::Ed25519Seed seed2 = seed;
    seed2[0] ^= 0x55;
    const std::string wrongKey = cascade::core::sys::makeEnvelope(seed2, cp::signedStatement("companion-multimon", "multimon-helper.exe", h));

    struct Case { const char* name; std::string sig; bool keyed; bool installs; int hitsForProgram; };
    const std::vector<Case> cases = {
        // (M1, security review 1: with no owner key a program is NEVER downloaded; was: installs true, hits 1 for each of these three)
        {"no key, no signature: no download", "", false, false, 0},
        {"no key, a signature present: no download", good, false, false, 0},
        {"no key, a garbage signature: no download", "garbage", false, false, 0},
        {"key, the right signature", good, true, true, 1},
        {"key, no signature", "", true, false, 0},
        {"key, signed by another key", wrongKey, true, false, 0},
        {"key, signed for another file", forOther, true, false, 0},
        {"key, garbage", "garbage", true, false, 0},
    };
    for (const Case& c : cases) {
        const fs::path dir = g_dir / "c7";
        fs::remove_all(dir);
        fs::create_directories(dir);
        cp::setSignatureKeysForTest(true, c.keyed ? std::vector<cascade::core::sys::Ed25519PublicKey>{pub} : std::vector<cascade::core::sys::Ed25519PublicKey>{});
        g_autoSign = false;   // each case says exactly what the row carries
        World w;
        Spec s = base;
        s.signature = c.sig;
        std::string err;
        const bool ok = installOne(w, indexOf(w, s), dir, err);
        if (ok != c.installs) { std::printf("      %s: installed=%d err='%s'\n", c.name, ok ? 1 : 0, err.c_str()); }
        CHECK(ok == c.installs);
        if (!c.installs) {
            CHECK(tree(dir).empty());
            CHECK(w.programHits == 0);   // the program was never asked for
            CHECK(err.find(c.keyed ? "signature" : "not available") != std::string::npos);
        }
    }
    g_autoSign = true;
    cp::setSignatureKeysForTest(true, {cascade::core::sys::ed25519PublicKey(g_seed)});
    // the shelf says so too: with no owner key a published program reads "not available", with one it reads "download"
    {
        Spec sp;
        sp.progBytes = program("c7-state");
        World ws;
        PluginCatalogEntry ent;
        std::string perr;
        CHECK(parseOne(indexOf(ws, sp), ent, perr));
        const fs::path sdir = g_dir / "c7-state";
        fs::create_directories(sdir);
        const cp::Places places = cp::placesFor(sdir.string());
        CHECK(cp::stateOf(ent, places, {}, bridgeFile()) == cp::State::Download);
        cp::setSignatureKeysForTest(true, {});
        CHECK(cp::stateOf(ent, places, {}, bridgeFile()) == cp::State::NotAvailable);
        cp::setSignatureKeysForTest(true, {cascade::core::sys::ed25519PublicKey(g_seed)});
    }
    cp::setSignatureKeysForTest(true, {cascade::core::sys::ed25519PublicKey(g_seed)});
    // the product's own list is EMPTY until the owner fills in his key: the check is built, and until then the website copy downloads nothing
    CHECK(cascade::core::sys::ownerTrustedKeys().empty());
    PluginCatalogEntry e;
    std::string err;
    World w;
    CHECK(parseOne(indexOf(w, base), e, err));
    CHECK(cp::checkRowSignature("companion-multimon", e.companion.program, cascade::core::sys::ownerTrustedKeys()) == cp::Signature::NotEnforced);
    fs::remove_all(g_dir / "c7");
}

// ---- C8 ----------------------------------------------------------------------------------------------------------------------------------
void testRecord() {
    std::printf("[C8] the record: remove, share, update\n");
    cp::setPackagedForTest(0);
    const fs::path dir = g_dir / "c8";
    fs::remove_all(dir);
    fs::create_directories(dir);
    World w;
    Spec s;
    s.progBytes = program("c8-v1");
    std::string err;
    CHECK(installOne(w, indexOf(w, s), dir, err));
    // a second bridge file that names the same program (an old version left beside the new one)
    cp::Record rec;
    CHECK(cp::readRecord(dir.string(), bridgeFile(), rec));
    cp::Record old = rec;
    old.plugin = "companion-multimon-0.1.1-abi3-win-x64" TEST_MODULE_EXT;
    CHECK(cp::recordProgram(dir.string(), old, err));
    // removing the OLD bridge keeps the program (the new one still names it)
    CHECK(cp::removeProgram(dir.string(), old.plugin, err));
    CHECK(fs::exists(dir / "helpers" / "multimon-helper.exe"));
    cp::Record gone;
    CHECK(!cp::readRecord(dir.string(), old.plugin, gone) && cp::readRecord(dir.string(), bridgeFile(), gone));
    // removing the last one deletes the program, the record file and the empty helpers folder
    CHECK(cp::removeProgram(dir.string(), bridgeFile(), err));
    CHECK(!fs::exists(dir / "helpers") && !fs::exists(dir / "companions.json"));
    // removing what was never recorded is fine
    CHECK(cp::removeProgram(dir.string(), "nothing.dll", err));
    // THE STORE'S OWN REMOVE (PluginRepo::remove) takes the program and its record with the bridge
    {
        fs::remove_all(dir);
        fs::create_directories(dir);
        CHECK(installOne(w, indexOf(w, s), dir, err));
        CHECK(fs::exists(dir / "helpers" / "multimon-helper.exe") && fs::exists(dir / "companions.json"));
        PluginRepo repoForRemove;
        CHECK(repoForRemove.remove(dir.string(), bridgeFile(), err));
        if (!err.empty()) { std::printf("      %s\n", err.c_str()); }
        CHECK(tree(dir).empty());   // the bridge, the program, the record and the helpers folder: all gone
    }

    // an UPDATE replaces the program; a failed update (hash) leaves the old one and its record
    fs::remove_all(dir);
    fs::create_directories(dir);
    CHECK(installOne(w, indexOf(w, s), dir, err));
    Spec s2 = s;
    s2.progBytes = program("c8-v2");
    s2.dll = "stand-in bridge 2";
    // (the same bridge file name, a new program: what a reinstall of a version looks like)
    CHECK(installOne(w, indexOf(w, s2), dir, err));
    CHECK(slurp(dir / "helpers" / "multimon-helper.exe") == s2.progBytes);
    CHECK(cp::readRecord(dir.string(), bridgeFile(), rec) && rec.sha256 == sha(s2.progBytes));
    Spec s3 = s2;
    s3.progBytes = program("c8-v3");
    s3.progSha = sha("lies");
    s3.dll = "stand-in bridge 3";
    CHECK(!installOne(w, indexOf(w, s3), dir, err));
    CHECK(slurp(dir / "helpers" / "multimon-helper.exe") == s2.progBytes);
    CHECK(cp::readRecord(dir.string(), bridgeFile(), rec) && rec.sha256 == sha(s2.progBytes));
    CHECK(cp::checkBeforeStart((dir / bridgeFile()).string()).allowed);
    CHECK(tree(dir) == sorted({"companions.json", "helpers/multimon-helper.exe", bridgeFile()}));
    fs::remove_all(dir);
}

// ---- C9 ----------------------------------------------------------------------------------------------------------------------------------
void testStates() {
    std::printf("[C9] the shelf's states and sentences\n");
    cp::setPackagedForTest(0);
    const fs::path dir = g_dir / "c9";
    fs::remove_all(dir);
    fs::create_directories(dir);
    World w;
    Spec s;
    s.progBytes = program("c9");
    PluginCatalogEntry e, unp;
    std::string err;
    CHECK(parseOne(indexOf(w, s), e, err));
    Spec u = s;
    u.status = "unpublished";
    CHECK(parseOne(indexOf(w, u), unp, err));
    const cp::Places places = cp::placesFor(dir.string());
    CHECK(cp::stateOf(e, places, {}, bridgeFile()) == cp::State::Download);
    CHECK(cp::stateOf(unp, places, {}, bridgeFile()) == cp::State::NotAvailable);
    CHECK(std::string(cp::stateWord(cp::State::NotAvailable)) == "unavailable" && std::string(cp::stateWord(cp::State::Download)) == "download");
    CHECK(std::string(cp::stateWord(cp::State::InPackage)) == "package" && std::string(cp::stateWord(cp::State::Installed)) == "installed" && std::string(cp::stateWord(cp::State::Changed)) == "changed");
    // no promise of when in the not-available words
    const std::string na = cp::stateSentence(cp::State::NotAvailable);
    CHECK(na.find("not available") != std::string::npos);
    for (const char* promise : {"soon", "later", "coming", "will be", "not yet", "beta", "phase"}) { CHECK(na.find(promise) == std::string::npos); }
    for (const cp::State st : {cp::State::InPackage, cp::State::Download, cp::State::Installed, cp::State::Changed}) {
        const std::string t = cp::stateSentence(st);
        CHECK(!t.empty());
        for (const char* promise : {"soon", "later", "coming", "not yet", "beta", "phase"}) { CHECK(t.find(promise) == std::string::npos); }
    }
    // installed, then the file changes size: Changed
    CHECK(installOne(w, indexOf(w, s), dir, err));
    CHECK(cp::stateOf(e, places, {}, bridgeFile()) == cp::State::Installed);
    put(dir / "helpers" / "multimon-helper.exe", program("c9") + "more");
    CHECK(cp::stateOf(e, places, {}, bridgeFile()) == cp::State::Changed);
    fs::remove(dir / "helpers" / "multimon-helper.exe");
    CHECK(cp::stateOf(e, places, {}, bridgeFile()) == cp::State::Changed);
    // an entry with no companion object
    PluginCatalogEntry plain;
    CHECK(cp::stateOf(plain, places, {}, "x.dll") == cp::State::NotAvailable);
    fs::remove_all(dir);
}

// ---- C10 ---------------------------------------------------------------------------------------------------------------------------------
void testHostile() {
    std::printf("[C10] hostile input\n");
    const fs::path pkg = g_dir / "c10";
    fs::remove_all(pkg);
    fs::create_directories(pkg);
    std::string err;
    std::vector<cp::PackageProgram> list;
    // a list with a path that climbs, one with a separator in the file name, one with a short hash: those lines are left out, the sound one stays
    const std::string good = sha("g");
    nlohmann::json j = {{"version", 1},
                        {"programs", nlohmann::json::array({{{"id", "a"}, {"file", "a.exe"}, {"path", "../evil/a.exe"}, {"sha256", good}},
                                                             {{"id", "b"}, {"file", "sub/b.exe"}, {"path", "b/b.exe"}, {"sha256", good}},
                                                             {{"id", "c"}, {"file", "c.exe"}, {"path", "c/c.exe"}, {"sha256", "abc"}},
                                                             {{"id", "d"}, {"file", "d.exe"}, {"path", "/abs/d.exe"}, {"sha256", good}},
                                                             {{"id", "e"}, {"file", "e.exe"}, {"path", "e\\e.exe"}, {"sha256", good}},
                                                             {{"id", "ok"}, {"file", "ok.exe"}, {"path", "ok/ok.exe"}, {"sha256", good}},
                                                             5, "x"})}};
    put(pkg / "companions.json", j.dump());
    CHECK(cp::readPackageList(pkg, list, err));
    CHECK(list.size() == 1u && list[0].id == "ok");
    // a list that is not JSON, a list that is the wrong shape, no list
    put(pkg / "companions.json", "not json");
    CHECK(!cp::readPackageList(pkg, list, err) && list.empty());
    put(pkg / "companions.json", "{\"programs\":5}");
    CHECK(!cp::readPackageList(pkg, list, err));
    fs::remove(pkg / "companions.json");
    CHECK(!cp::readPackageList(pkg, list, err));
    // a record with a bad program name is not read; a record file that is not JSON is no record
    const fs::path dir = g_dir / "c10b";
    fs::remove_all(dir);
    fs::create_directories(dir);
    put(dir / "companions.json", "{\"version\":1,\"programs\":[{\"plugin\":\"p.dll\",\"id\":\"x\",\"file\":\"../evil.exe\",\"sha256\":\"" + good + "\",\"size\":1,\"via\":\"download\"}]}");
    cp::Record rec;
    CHECK(!cp::readRecord(dir.string(), "p.dll", rec));
    // L1 (security review 1): an entry that names the plugin but has no usable program name is NOT "not a catalogue companion": the plugin is refused, with the way out.
    // (Was: allowed, the check skipped.) A plugin the list does not name is still left alone.
    {
        const cp::StartVerdict broken = cp::checkBeforeStart((dir / "p.dll").string());
        CHECK(!broken.allowed);
        CHECK(broken.sentence.find("damaged") != std::string::npos && broken.sentence.find("fit it again") != std::string::npos);
        CHECK(cp::checkBeforeStart((dir / "other-plugin.dll").string()).allowed);
    }
    put(dir / "companions.json", "garbage");
    CHECK(!cp::readRecord(dir.string(), "p.dll", rec));
    // a record FILE that is there but unreadable, with a program in the helpers folder: every plugin is refused until the list is repaired (the host cannot tell which
    // plugin the program belongs to); with nothing in the helpers folder there is nothing to run, and with no record file at all nothing was ever installed
    CHECK(cp::checkBeforeStart((dir / "p.dll").string()).allowed);                      // helpers folder empty or absent
    put(dir / "helpers" / "stray.exe", "MZ stand-in program");
    {
        const cp::StartVerdict v1 = cp::checkBeforeStart((dir / "p.dll").string());
        CHECK(!v1.allowed && v1.sentence.find("damaged") != std::string::npos);
        const cp::StartVerdict v2 = cp::checkBeforeStart((dir / "anything-else.dll").string());
        CHECK(!v2.allowed);
        std::array<std::uint64_t, 4> sums{};
        cp::takeHostFaults(sums);
        CHECK(sums[2] >= 2);   // counted on the companion fault path (kind BREAK)
    }
    fs::remove(dir / "companions.json");
    CHECK(cp::checkBeforeStart((dir / "p.dll").string()).allowed);                      // no record file: nothing was installed, a program the user chose is the user's
    put(dir / "companions.json", "{\"version\":1,\"programs\":5}");                     // the wrong shape counts as damaged too
    CHECK(!cp::checkBeforeStart((dir / "p.dll").string()).allowed);
    fs::remove(dir / "companions.json");
    fs::remove_all(dir / "helpers");
    // a fuzz of the row parser: random JSON of every type under the companion key never throws and never makes a usable program of nothing
    unsigned seed = 12345u;
    auto next = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    const std::vector<nlohmann::json> pieces = {nullptr, 1, 2.5, true, "text", "https://a.invalid/x.exe", nlohmann::json::array(), nlohmann::json::object(), nlohmann::json::array({1, "a"}), std::string(5000, 'x')};
    for (int i = 0; i < 400; ++i) {
        nlohmann::json prog = nlohmann::json::object();
        for (const char* k : {"file", "url", "sha256", "status", "signature"}) {
            if (next() % 3 != 0) { prog[k] = pieces[next() % pieces.size()]; }
        }
        nlohmann::json comp = nlohmann::json::object();
        for (const char* k : {"upstream", "licence", "whySeparate", "source"}) {
            if (next() % 3 != 0) { comp[k] = pieces[next() % pieces.size()]; }
        }
        if (next() % 4 != 0) { comp["program"] = (next() % 5 == 0) ? pieces[next() % pieces.size()] : prog; }
        nlohmann::json entry = {{"id", "x"}, {"description", pieces[next() % pieces.size()]}, {"companion", (next() % 7 == 0) ? pieces[next() % pieces.size()] : comp}};
        PluginCatalogEntry e;
        cp::parseCompanionFields(entry, e);
        if (e.companion.program.usable) {
            // usable only with every field sound
            CHECK(e.companion.program.status == "published" && cp::validProgramName(e.companion.program.file).empty() && e.companion.program.url.rfind("https://", 0) == 0 &&
                  e.companion.program.sha256.size() == 64u);
        }
    }
    fs::remove_all(pkg);
    fs::remove_all(dir);
}

// ---- C11 ---------------------------------------------------------------------------------------------------------------------------------
// THE REAL PROGRAMS (optional): when FOXSDR_COMPANIONS_COLLECTED names the folder installer/msix/collect-companions.ps1 made, every real built companion program goes
// through the Store copy's path (package list -> copy into helpers/ -> record -> the check before every start). Nothing is run: the programs are black boxes whose bytes are
// hashed and whose image is looked at, never started. Skipped, and said so, when the variable is not set.
void testRealPrograms() {
    const char* env = std::getenv("FOXSDR_COMPANIONS_COLLECTED");
    if (env == nullptr || *env == '\0') {
        std::printf("[C11] the real programs: SKIPPED (FOXSDR_COMPANIONS_COLLECTED is not set)\n");
        return;
    }
    std::printf("[C11] the real programs from %s\n", env);
    const fs::path collected(env);
    std::string manifestText = slurp(collected / "companions.manifest.json");
    if (manifestText.size() >= 3 && static_cast<unsigned char>(manifestText[0]) == 0xEF) { manifestText.erase(0, 3); }   // a UTF-8 byte-order mark
    const nlohmann::json manifest = nlohmann::json::parse(manifestText, nullptr, false);
    CHECK(manifest.is_object() && manifest.contains("companions") && manifest["companions"].is_array());
    if (!manifest.is_object() || !manifest.contains("companions")) { return; }
    const fs::path pkg = g_dir / "c11-package";
    const fs::path dir = g_dir / "c11";
    fs::remove_all(pkg);
    fs::remove_all(dir);
    fs::create_directories(dir);
    nlohmann::json list = nlohmann::json::array();
    int count = 0;
    for (const nlohmann::json& c : manifest["companions"]) {
        const std::string folder = c["folder"].get<std::string>();
        const std::string file = c["file"].get<std::string>();
        CHECK(folder != "hfserial" && folder != "dvframes");
        std::error_code ec;
        fs::create_directories(pkg / folder, ec);
        fs::copy_file(collected / folder / file, pkg / folder / file, fs::copy_options::overwrite_existing, ec);
        std::string h, e2;
        CHECK(PluginRepo::sha256File((pkg / folder / file).string(), h, e2));
        list.push_back({{"id", "companion-" + folder}, {"file", file}, {"path", folder + "/" + file}, {"sha256", h}, {"size", fs::file_size(pkg / folder / file, ec)}});
        ++count;
    }
    put(pkg / "companions.json", nlohmann::json({{"version", 1}, {"programs", list}}).dump());
    cp::setPackagedForTest(1, pkg);
    int installed = 0;
    World w;
    for (const nlohmann::json& c : manifest["companions"]) {
        const std::string folder = c["folder"].get<std::string>();
        const std::string file = c["file"].get<std::string>();
        // the catalogue row for it: unpublished (the package is the authority), bridge named as the catalogue names it
        const std::string dllName = std::string("companion-") + folder + "-0.1.2-abi3-" + PluginRepo::hostOs() + "-" + PluginRepo::hostArch() + TEST_MODULE_EXT;
        const std::string dllUrl = "https://dl.example.invalid/plugins/" + dllName;
        w.files[dllUrl] = "bridge " + folder;
        const std::string doc = std::string("{\"schemaVersion\":1,\"plugins\":[{\"id\":\"companion-") + folder + "\",\"name\":\"Companion " + folder + "\",\"version\":\"0.1.2\",\"licence\":\"MIT\",\"abiVersion\":" +
                                std::to_string(CASCADE_PLUGIN_ABI_VERSION) + ",\"category\":\"companions\",\"companion\":{\"upstream\":\"x\",\"licence\":\"GPL\",\"whySeparate\":\"x\",\"program\":{\"file\":\"" + file +
                                "\",\"url\":\"\",\"sha256\":\"\",\"status\":\"unpublished\"}},\"platforms\":[{\"os\":\"" + PluginRepo::hostOs() + "\",\"arch\":\"" + PluginRepo::hostArch() + "\",\"file\":\"" + dllName +
                                "\",\"url\":\"" + dllUrl + "\",\"sha256\":\"" + sha("bridge " + folder) + "\"}]}]}";
        std::string err;
        const bool ok = installOne(w, doc, dir, err);
        if (!ok) { std::printf("      %s: %s\n", folder.c_str(), err.c_str()); }
        CHECK(ok);
        if (ok) {
            ++installed;
            CHECK(cp::checkBeforeStart((dir / dllName).string()).allowed);
            CHECK(slurp(dir / "helpers" / file) == slurp(collected / folder / file));
        }
    }
    CHECK(installed == count && count >= 18);
    CHECK(w.hits == count);   // one request per bridge and NOT ONE for a program: the packaged path asks the transport for nothing else
    std::printf("      %d real programs installed through the package path and checked before a start\n", installed);
    cp::setPackagedForTest(-1);
}

}  // namespace

int main() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_dir = fs::temp_directory_path() / ("foxsdr_companion_programs_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    for (std::size_t i = 0; i < g_seed.size(); ++i) { g_seed[i] = static_cast<std::uint8_t>(i + 3); }
    cp::setSignatureKeysForTest(true, {cascade::core::sys::ed25519PublicKey(g_seed)});
    testParse();
    testCheck();
    testWebsiteInstall();
    testAllOrNothing();
    testBeforeStart();
    testPackaged();
    testSignature();
    testRecord();
    testStates();
    testHostile();
    testRealPrograms();
    cp::setPackagedForTest(-1);
    fs::remove_all(g_dir, ec);
    return testSummary("test_companion_programs");
}
