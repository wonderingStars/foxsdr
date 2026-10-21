// Tests for the system-font scan that runs OFF the GUI thread (gui/fonts.hpp,
// 0.99.74).
//
// THE DEFECT. Opening the "Language & country" section asked, inside the
// frame, which of the CJK languages this machine can draw - and answering that
// meant reading Microsoft YaHei, JhengHei, Yu Gothic and Malgun Gothic (14 to
// 21 MB each) one byte at a time, and then reading each of them AGAIN when its
// face went into the atlas. On a laptop with a slow disk that was a frame of
// many seconds: field hangs CE8EBCD986C97CEB (0.99.70) and D28A155E1A128889
// (0.99.71), both in the frame-start step.
//
// WHAT IS PINNED HERE, each against a disk made slow on purpose
// (setReadDelayForTests sleeps inside every read that finds a file):
//   1. every system face in the chain was read from disk exactly once;
//   2. requestLanguageNames() + applyPending() return at once while the scan is
//      still reading, and the names' faces arrive when it is done;
//   3. canDrawNow() says Checking while the scan runs and, once it is done,
//      exactly what the blocking canDraw() says;
//   4. the bulk file reader returns the same bytes a byte-by-byte read does;
//   5. the scan finds the very faces the lazy, on-demand walk finds;
//   6. a scan still running when the process exits is joined, not terminated.
//
// The CJK checks need a system CJK face. On a machine without one (a Linux
// desktop without fonts-noto-cjk) they are reported and counted as SKIPPED,
// never as passed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "core/i18n.hpp"
#include "gui/fonts.hpp"
#include "imgui.h"
#include "test_check.hpp"

namespace fonts = cascade::gui::fonts;
using Clock = std::chrono::steady_clock;

namespace {

void skip(const char* why) {
    ++g_checksSkipped;
    std::printf("    SKIP: %s\n", why);
}

// One frame, so the atlas is exercised the way the application uses it.
void frame() {
    ImGui::NewFrame();
    ImGui::Begin("probe");
    ImGui::TextUnformatted("FoxSDR 145.800 MHz");
    ImGui::End();
    ImGui::Render();
}

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

std::string labels() {
    std::string s;
    for (const std::string& l : fonts::uiChainLabels()) { s += (s.empty() ? "" : " > ") + l; }
    return s;
}

bool hasNamesFace() {
    for (const std::string& l : fonts::uiChainLabels()) {
        if (l.find("(names)") != std::string::npos) { return true; }
    }
    return false;
}

const char* name(fonts::Drawable d) {
    switch (d) {
    case fonts::Drawable::Yes: return "Yes";
    case fonts::Drawable::No: return "No";
    case fonts::Drawable::Checking: return "Checking";
    }
    return "?";
}

// Whether this machine has a system CJK face at all, learned in the first test
// (the only one that runs against a process that has read nothing yet).
bool g_hasCjk = false;

// The atlas back to English and the scan back to "never started": what each
// test below begins from, so none is answered by what an earlier one read.
void coldStart() {
    fonts::setReadDelayForTests(0);
    CHECK(fonts::load());
    fonts::resetScanForTests();
    frame();
}

// The file a chain label names: "msyh.ttc#1" -> "msyh.ttc", "Georgia" ->
// "georgia.ttf" (empty for an embedded face, which has no file).
std::string fileOfLabel(std::string label) {
    const std::size_t names = label.find(" (names)");
    if (names != std::string::npos) { label.erase(names); }
    const std::size_t hash = label.find('#');
    if (hash != std::string::npos) { return label.substr(0, hash); }
    if (label == "Georgia") { return "georgia.ttf"; }
    if (label == "Georgia Bold") { return "georgiab.ttf"; }
    return {};
}

// The file names of the system CJK faces a chain label list names (the ones
// with a "#index"), with or without the " (names)" faces.
std::set<std::string> cjkFilesOf(const std::vector<std::string>& chainLabels, bool namesOnly) {
    std::set<std::string> files;
    for (const std::string& label : chainLabels) {
        const bool names = label.find("(names)") != std::string::npos;
        if (namesOnly && !names) { continue; }
        if (label.find('#') != std::string::npos) { files.insert(fileOfLabel(label)); }
    }
    return files;
}

// 1. READ ONCE. Every file a face in the chain came from was opened one time
// by the scan, and the atlas was given THOSE bytes: the scan's read is the
// atlas's read. (Before 0.99.74 it was two for each: the scan read the file to
// look at it and let the bytes go, then the atlas read it again.)
//
// The footprint after the names pass is the one it always was: the files the
// scan found usable but the names pass did not put in the atlas (JhengHei and
// Yu Gothic here, whose names YaHei already draws) are let go, so nothing is
// held that is not in a chain.
//
// THE EXACT COUNTS. A language chosen AFTER the list was opened whose file the
// names pass did not pin - Japanese, on a machine where YaHei draws 日本語 - has
// to read that file again, once, in one bulk read: it is the one file in this
// test opened TWICE, by design, because keeping it for everyone was 35 MB held
// for a choice most never make. Every other file, the names faces included,
// stays at one.
void testReadOnce() {
    std::printf("  every system face in the chain was read from disk exactly once\n");
    fonts::requestLanguageNames();
    fonts::waitForSystemFontScan();
    fonts::applyPending();
    frame();
    std::printf("    UI chain: %s\n", labels().c_str());
    g_hasCjk = hasNamesFace();

    // After the names pass: no file at all - a face in the chain or one the scan
    // only looked at - was opened twice.
    const std::map<std::string, int> counts = fonts::fileReadCountsForTests();
    for (const auto& [path, n] : counts) {
        std::printf("    opened %d x  %s\n", n, path.c_str());
        CHECK(n == 1);
    }
    // And every face in the chain that comes from a file is one of those opened
    // (so the counts above are about the files the chain really uses).
    std::size_t checked = 0;
    for (const std::string& label : fonts::uiChainLabels()) {
        const std::string file = fileOfLabel(label);
        if (file.empty()) { continue; }
        bool found = false;
        for (const auto& [path, n] : counts) {
            if (std::filesystem::path(path).filename().string() == file && n == 1) { found = true; }
        }
        CHECK(found);
        ++checked;
    }
    if (checked == 0) {
        skip("no system face in the chain on this machine - nothing was read, nothing to count");
    }
    if (!g_hasCjk) {
        skip("no system CJK face on this machine - only Georgia (if any) was counted");
        return;
    }

    // Nothing is kept that is not in the chain.
    std::printf("    files kept in memory beyond the chain: %zu\n", fonts::keptBytesForTests());
    CHECK(fonts::keptBytesForTests() == 0);

    // A language chosen afterwards.
    if (fonts::canDrawNow("ja") != fonts::Drawable::Yes) {
        skip("no Japanese face on this machine - the later choice of Japanese was not run");
        return;
    }
    fonts::applyLanguage("ja");
    fonts::applyPending();
    frame();
    std::printf("    ja: %s\n", labels().c_str());
    const std::set<std::string> namesFiles = cjkFilesOf(fonts::uiChainLabels(), true);
    std::set<std::string> jaFiles;
    for (const fonts::ChainFace& f : fonts::planChain("ja").ui) {
        if (f.label.find('#') != std::string::npos) { jaFiles.insert(fileOfLabel(f.label)); }
    }
    CHECK(!jaFiles.empty());
    bool someTwice = false;
    for (const auto& [path, n] : fonts::fileReadCountsForTests()) {
        const std::string file = std::filesystem::path(path).filename().string();
        // Twice only when the scan read it BEFORE (it is in `counts`), Japanese
        // uses it, and the names pass did not keep it in the atlas.
        const bool again = counts.count(path) != 0 && jaFiles.count(file) != 0 && namesFiles.count(file) == 0;
        const int want = again ? 2 : 1;
        std::printf("    opened %d x (want %d)  %s\n", n, want, path.c_str());
        CHECK(n == want);
        someTwice = someTwice || want == 2;
    }
    if (!someTwice) {
        skip("this machine's Japanese face is a names face already - no file is read a second time");
    }
    CHECK(fonts::keptBytesForTests() == 0);
}

// 2. THE GUI THREAD DOES NOT WAIT. With every file read costing 400 ms, the
// call the language section makes each frame - ask for the names, then let the
// atlas catch up - comes back at once, and the names' faces are not there yet.
void testNotBlocking() {
    std::printf("  requestLanguageNames + applyPending return while the files are still being read\n");
    if (!g_hasCjk) {
        skip("no system CJK face on this machine - there is no file read to be slow");
        return;
    }
    coldStart();
    fonts::setReadDelayForTests(400);
    const Clock::time_point t0 = Clock::now();
    fonts::requestLanguageNames();
    fonts::applyPending();
    const double ms = msSince(t0);
    std::printf("    returned in %.1f ms with a 400 ms delay in every file read\n", ms);
    CHECK(ms < 150.0);
    CHECK(!hasNamesFace());  // not yet: the worker has not finished the first file
    frame();

    fonts::waitForSystemFontScan();
    CHECK(fonts::applyPending());
    frame();
    std::printf("    after the scan: %s\n", labels().c_str());
    CHECK(hasNamesFace());
    fonts::setReadDelayForTests(0);
}

// 3. THREE ANSWERS. Checking while the scan runs; once it is done, exactly the
// blocking canDraw()'s Yes or No. English needs no scan and is never Checking.
void testTriState() {
    std::printf("  canDrawNow: Checking while the scan runs, then what canDraw says\n");
    coldStart();
    fonts::setReadDelayForTests(400);
    // The first question starts the scan and cannot have its answer yet.
    const fonts::Drawable first = fonts::canDrawNow("ja");
    std::printf("    ja, asked cold: %s\n", name(first));
    CHECK(first == fonts::Drawable::Checking);
    CHECK(fonts::canDrawNow("en") == fonts::Drawable::Yes);
    CHECK(fonts::canDrawNow("fr") == fonts::Drawable::Yes);  // Latin: the compiled-in faces
    if (g_hasCjk) {
        const fonts::Drawable second = fonts::canDrawNow("zh-CN");
        std::printf("    zh-CN, asked while the scan runs: %s\n", name(second));
        CHECK(second == fonts::Drawable::Checking);
    } else {
        skip("no system CJK face - the scan may be over before the second question");
    }
    fonts::waitForSystemFontScan();
    for (const char* code : {"ja", "zh-CN", "zh-TW", "ko", "fr", "en"}) {
        const fonts::Drawable now = fonts::canDrawNow(code);
        const bool exact = fonts::canDraw(code);
        std::printf("    %-6s canDrawNow %-8s canDraw %s\n", code, name(now), exact ? "true" : "false");
        CHECK(now == (exact ? fonts::Drawable::Yes : fonts::Drawable::No));
    }
    fonts::setReadDelayForTests(0);
}

// The bytes a plain byte-by-byte read gives - the old reader's answer.
std::vector<unsigned char> readByteWise(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// 4. THE BULK READ IS THE SAME READ: the file reader now sizes the file and
// reads it in one call; what it returns must be every byte, in order, for
// sizes on either side of the stream buffer's, and nothing for a file that is
// empty, missing or a directory.
void testBulkRead() {
    std::printf("  the bulk file read returns exactly the bytes a byte-by-byte read does\n");
#ifdef _WIN32
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("foxsdr_font_scan_" + std::to_string(pid));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    CHECK(std::filesystem::is_directory(dir));

    std::uint32_t x = 0x2545F491u;  // xorshift32: the same "random" bytes on every run
    auto next = [&x]() {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return static_cast<unsigned char>(x >> 11);
    };
    for (const std::size_t size : {std::size_t{1}, std::size_t{4095}, std::size_t{4096}, std::size_t{4097},
                                   std::size_t{(1u << 20) + 13}}) {
        std::vector<unsigned char> want(size);
        for (unsigned char& b : want) { b = next(); }
        const std::filesystem::path file = dir / ("random_" + std::to_string(size) + ".bin");
        {
            std::ofstream out(file, std::ios::binary);
            out.write(reinterpret_cast<const char*>(want.data()), static_cast<std::streamsize>(want.size()));
        }
        const std::vector<unsigned char> byteWise = readByteWise(file);
        CHECK(byteWise == want);  // the premise: the reference read is the file
        const std::vector<unsigned char> got = fonts::readFileForTests(file.string());
        std::printf("    %8zu bytes: read %zu, %s\n", size, got.size(), got == byteWise ? "identical" : "DIFFERENT");
        CHECK(got.size() == size);
        CHECK(got == byteWise);
    }

    const std::filesystem::path empty = dir / "empty.bin";
    { std::ofstream out(empty, std::ios::binary); }
    CHECK(fonts::readFileForTests(empty.string()).empty());
    CHECK(fonts::readFileForTests((dir / "missing.bin").string()).empty());
    CHECK(fonts::readFileForTests(dir.string()).empty());  // a directory is not a font
    CHECK(fonts::readFileForTests("").empty());

    std::filesystem::remove_all(dir, ec);
}

// The CJK faces plan puts in a language's chain, by label.
std::string cjkLabels(const char* code) {
    std::string s;
    for (const fonts::ChainFace& f : fonts::planChain(code).ui) {
        if (f.label.find('#') != std::string::npos) { s += (s.empty() ? "" : " ") + f.label; }
    }
    return s;
}

// 5. THE WORKER WALKS LIKE THE LAZY PATH: for each standard, the face the
// background scan settled on is the one the on-demand walk (a language chosen
// before any list was opened) would have found.
void testScanFindsWhatLazyWalkFinds() {
    std::printf("  the scan finds the same faces as the on-demand walk\n");
    if (!g_hasCjk) {
        skip("no system CJK face on this machine - there is nothing to find");
        return;
    }
    const char* codes[] = {"zh-CN", "zh-TW", "ja", "ko"};
    std::vector<std::string> viaScan, viaLazy;
    coldStart();
    fonts::requestLanguageNames();
    fonts::waitForSystemFontScan();
    for (const char* c : codes) { viaScan.push_back(cjkLabels(c)); }
    coldStart();  // no scan: planChain walks each standard's candidates itself
    for (const char* c : codes) { viaLazy.push_back(cjkLabels(c)); }
    for (std::size_t i = 0; i < viaScan.size(); ++i) {
        std::printf("    %-6s scan: %-24s lazy: %s\n", codes[i], viaScan[i].c_str(), viaLazy[i].c_str());
        CHECK(viaScan[i] == viaLazy[i]);
    }
    CHECK(!viaScan[0].empty() || !viaScan[1].empty() || !viaScan[2].empty() || !viaScan[3].empty());
}

// Costs of the work that still happens on the GUI thread, for the notes.
void reportCosts() {
    std::printf("  what the GUI thread still computes (information, not asserted)\n");
    const std::vector<cascade::i18n::Language>& langs = cascade::i18n::languages();  // parses the catalogues
    Clock::time_point t0 = Clock::now();
    const std::size_t fr = cascade::i18n::codePoints("fr").size();
    std::printf("    codePoints(\"fr\") once: %.2f ms (%zu code points)\n", msSince(t0), fr);
    t0 = Clock::now();
    std::size_t total = 0;
    for (const cascade::i18n::Language& l : langs) { total += cascade::i18n::codePoints(l.code).size(); }
    std::printf("    codePoints for all %zu listed languages (English included): %.2f ms (%zu code points)\n",
                langs.size(), msSince(t0), total);
}

// WHAT STILL WAITS. Two questions the language section asks every frame go
// through i18n's drawable predicate, which is the exact, blocking canDraw():
// "Automatic (...)" asks resolveFor("auto", <the operating system's language>),
// and the country's suggested-language key asks matchCatalogue(<its language>).
// For a CJK language neither the user's own nor yet decided, that waits for the
// scan - the file reads are off the GUI thread, but the wait is not. Printed so
// the limit is visible in every run; not asserted, because it is a limit and not
// a promise. (A user on a Japanese Windows with the interface in English meets
// it once, in the first frames of opening the section.)
void reportResidualWaits() {
    std::printf("  what still waits for the scan on the GUI thread (information, not asserted)\n");
    if (!g_hasCjk) {
        skip("no system CJK face on this machine - there is no scan to wait for");
        return;
    }
    for (const char* which : {"resolveFor(\"auto\", \"ja-JP\")", "matchCatalogue(\"ja\")"}) {
        coldStart();
        fonts::setReadDelayForTests(150);
        fonts::requestLanguageNames();
        const Clock::time_point t0 = Clock::now();
        const std::string answer = which[0] == 'r' ? cascade::i18n::resolveFor("auto", "ja-JP")
                                                   : cascade::i18n::matchCatalogue("ja");
        std::printf("    %s -> \"%s\" after %.0f ms, with every file read taking 150 ms\n", which,
                    answer.c_str(), msSince(t0));
        fonts::setReadDelayForTests(0);
    }
}

}  // namespace

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 1000.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    const bool loaded = fonts::load();
    CHECK(loaded);
    if (loaded) {
        frame();
        testReadOnce();  // first: it counts against a process that has read nothing yet
        testNotBlocking();
        testTriState();
        testBulkRead();
        testScanFindsWhatLazyWalkFinds();
        reportCosts();
        reportResidualWaits();
        // 6. LEFT RUNNING ON PURPOSE. A scan still reading when the process ends
        // must be joined by the backstop in fonts.cpp: a joinable std::thread
        // destroyed at exit is std::terminate, which fails this test by its exit
        // code (0xC0000409 on Windows, SIGABRT elsewhere) after the summary line
        // below has been printed - so the output is flushed first.
        std::printf("  a scan still running at exit is joined, not terminated\n");
        coldStart();
        fonts::setReadDelayForTests(250);
        fonts::requestLanguageNames();
    }
    ImGui::DestroyContext();
    const int result = testSummary("test_font_scan");
    std::fflush(stdout);
    return result;
}
