// test_vendor_lib.cpp - source/vendor_lib: judging a file as a library BEFORE it is loaded, finding it, and loading it silently.
//
// Hand-built PE images prove every verdict on bytes (every truncation point, a fuzz of the headers); real stand-in libraries built by this
// project (tests/fixtures) prove the load itself, including a library whose own dependency is missing: the loader's error 126 must come back as
// a sentence with the thread's error mode left as it was found, never as a box.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "eol_fixture.hpp"
#include "source/vendor_lib.hpp"
#include "test_check.hpp"

using namespace cascade::source::vendorlib;

namespace {

void put16(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    b[at] = static_cast<std::uint8_t>(v);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    put16(b, at, v & 0xFFFF);
    put16(b, at + 2, v >> 16);
}

// A well-formed image: DOS header, PE signature, COFF header, optional header, sections whose last raw data ends exactly at the end of the file.
std::vector<std::uint8_t> makeImage(std::uint32_t machine, std::uint32_t magic, std::uint32_t characteristics, std::uint32_t sections = 1) {
    const std::size_t pe = 0x80;
    const std::size_t optSize = magic == 0x20B ? 240 : 224;
    const std::size_t table = pe + 24 + optSize;
    const std::size_t dataAt = (table + sections * 40 + 0x1FF) & ~std::size_t{0x1FF};
    std::vector<std::uint8_t> b(dataAt + 0x200, 0);
    b[0] = 'M';
    b[1] = 'Z';
    put32(b, 0x3C, static_cast<std::uint32_t>(pe));
    std::memcpy(&b[pe], "PE\0\0", 4);
    put16(b, pe + 4, machine);
    put16(b, pe + 6, sections);
    put16(b, pe + 20, static_cast<std::uint32_t>(optSize));
    put16(b, pe + 22, characteristics);
    put16(b, pe + 24, magic);
    for (std::uint32_t i = 0; i < sections; ++i) {
        const std::size_t s = table + i * 40;
        std::memcpy(&b[s], ".text", 5);
        put32(b, s + 16, i == sections - 1 ? 0x200 : 0);  // SizeOfRawData
        put32(b, s + 20, i == sections - 1 ? static_cast<std::uint32_t>(dataAt) : 0);
    }
    return b;
}

constexpr std::uint32_t kDll = 0x2022;  // DLL | EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE
constexpr std::uint32_t kExe = 0x0022;

Check judge(const std::vector<std::uint8_t>& b, std::uint32_t host = kMachineX64) { return checkBytes(b.data(), b.size(), host, "G39DDCAPI.dll"); }

void testBytes() {
    // The good ones.
    CHECK(judge(makeImage(kMachineX64, 0x20B, kDll)).ok());
    CHECK(judge(makeImage(kMachineX86, 0x10B, kDll), kMachineX86).ok());
    CHECK(judge(makeImage(kMachineArm64, 0x20B, kDll), kMachineArm64).ok());
    CHECK(judge(makeImage(kMachineX64, 0x20B, kDll, 6)).ok());

    // A 32-bit library in a 64-bit process (and the reverse): its own verdict, and a sentence a person can act on.
    {
        const Check c = judge(makeImage(kMachineX86, 0x10B, kDll));
        CHECK(c.verdict == Verdict::Wrong32Bit);
        CHECK(c.machine == kMachineX86);
        CHECK(c.sentence.find("32-bit") != std::string::npos);
        CHECK(c.sentence.find("64-bit") != std::string::npos);
        CHECK(c.sentence.find("G39DDCAPI.dll") != std::string::npos);
        const Check r = judge(makeImage(kMachineX64, 0x20B, kDll), kMachineX86);
        CHECK(r.verdict == Verdict::Wrong32Bit);
    }
    // Another processor entirely.
    CHECK(judge(makeImage(kMachineArm64, 0x20B, kDll)).verdict == Verdict::WrongMachine);
    // A program, not a library.
    CHECK(judge(makeImage(kMachineX64, 0x20B, kExe)).verdict == Verdict::NotALibrary);
    // Headers that disagree about the processor (a 32-bit optional header on a 64-bit machine field).
    CHECK(judge(makeImage(kMachineX64, 0x10B, kDll)).verdict == Verdict::NotAnImage);
    // Not images at all.
    CHECK(checkBytes(nullptr, 0, kMachineX64, "x.dll").verdict == Verdict::NotAnImage);
    CHECK(judge(std::vector<std::uint8_t>{}).verdict == Verdict::NotAnImage);
    CHECK(judge(std::vector<std::uint8_t>(4096, 'A')).verdict == Verdict::NotAnImage);
    {
        std::vector<std::uint8_t> html;
        const char* t = "<!DOCTYPE html><html><body>Download page</body></html>";
        html.assign(t, t + std::strlen(t));
        html.resize(1000, ' ');
        CHECK(judge(html).verdict == Verdict::NotAnImage);
    }
    // A PE offset that points outside the file, or at no signature.
    {
        auto b = makeImage(kMachineX64, 0x20B, kDll);
        put32(b, 0x3C, 0xFFFFFFF0u);
        CHECK(judge(b).verdict == Verdict::Truncated);
        put32(b, 0x3C, 0x100);
        CHECK(judge(b).verdict == Verdict::NotAnImage);
    }
    // An impossible section count; an optional header size past the end.
    {
        auto b = makeImage(kMachineX64, 0x20B, kDll);
        put16(b, 0x80 + 6, 500);
        CHECK(judge(b).verdict == Verdict::NotAnImage);
        b = makeImage(kMachineX64, 0x20B, kDll);
        put16(b, 0x80 + 20, 0xFFF0);
        CHECK(judge(b).verdict == Verdict::Truncated);
    }
    // EVERY truncation point of a good image is refused, and none is accepted by mistake: the raw data of the last section ends at the last byte.
    {
        const auto good = makeImage(kMachineX64, 0x20B, kDll, 3);
        int accepted = 0;
        for (std::size_t len = 0; len < good.size(); ++len) {
            const Check c = checkBytes(good.data(), len, kMachineX64, "x.dll");
            if (c.ok()) { ++accepted; }
            if (!c.ok()) { CHECK(!c.sentence.empty()); }
        }
        CHECK(accepted == 0);
        CHECK(checkBytes(good.data(), good.size(), kMachineX64, "x.dll").ok());
    }
    // A name with control characters or a folder in it never reaches the sentence unbounded.
    {
        const std::string evil = std::string("C:\\Users\\someone\\") + std::string(300, 'a') + "\x01.dll";
        const Check c = checkBytes(nullptr, 0, kMachineX64, evil);
        CHECK(c.sentence.size() < 200);
        CHECK(c.sentence.find('\x01') == std::string::npos);
    }
    // The verdict words are fixed.
    CHECK(std::string(verdictWord(Verdict::Ok)) == "ok");
    CHECK(std::string(verdictWord(Verdict::Wrong32Bit)) == "wrong-32-bit");
    CHECK(std::string(verdictWord(Verdict::NotFound)) == "not-found");
    CHECK(std::string(verdictWord(Verdict::SymbolMissing)) == "symbol-missing");
}

void testFuzz() {
    // 30000 mutated headers: never a crash, and an accepted image carries the expected machine and no sentence.
    const auto base = makeImage(kMachineX64, 0x20B, kDll, 4);
    std::mt19937 rng(12345);
    int ok = 0, refused = 0;
    for (int it = 0; it < 30000; ++it) {
        auto b = base;
        const int flips = 1 + static_cast<int>(rng() % 6);
        for (int f = 0; f < flips; ++f) {
            const std::size_t at = rng() % (it % 3 == 0 ? b.size() : 0x200);
            b[at] = static_cast<std::uint8_t>(rng());
        }
        if (it % 11 == 0) { b.resize(rng() % b.size()); }
        const Check c = checkBytes(b.data(), b.size(), kMachineX64, "f.dll");
        if (c.ok()) {
            ++ok;
            CHECK(c.machine == kMachineX64);
            CHECK(c.sentence.empty());
        } else {
            ++refused;
            CHECK(!c.sentence.empty());
        }
    }
    CHECK(refused > 1000);
    CHECK(ok > 100);
}

void testFind() {
    const auto dir = eol::scratch("find");
    const auto sub = dir / "lib";
    std::filesystem::create_directories(sub);
    eol::writeFile(sub / "G39DDCAPI.dll", {'M', 'Z'});
    std::string why;
    // A folder.
    CHECK(!findLibrary(eol::u8(sub), "G39DDCAPI.dll", why).empty());
    CHECK(why.empty());
    CHECK(findLibrary(eol::u8(dir), "G39DDCAPI.dll", why).empty());
    CHECK(why.find("not in the folder") != std::string::npos);
    // A file.
    CHECK(!findLibrary(eol::u8(sub / "G39DDCAPI.dll"), "G39DDCAPI.dll", why).empty());
    CHECK(findLibrary(eol::u8(sub / "nothere.dll"), "G39DDCAPI.dll", why).empty());
    // A name that is not a bare file name.
    CHECK(findLibrary(eol::u8(sub), "..\\x.dll", why).empty());
    CHECK(findLibrary(eol::u8(sub), "sub/x.dll", why).empty());
    CHECK(findLibrary(eol::u8(sub), "C:x.dll", why).empty());
    CHECK(findLibrary(eol::u8(sub), "", why).empty());
    // The PATH directories: absolute entries are searched; an empty entry or a relative one (the current directory) never is.
    {
        const char* old = std::getenv("PATH");
        const std::string saved = old != nullptr ? old : "";
        const std::string withDir = ";;.;relative\\dir;\"" + eol::u8(sub) + "\";" + saved;
        _putenv_s("PATH", withDir.c_str());
        const std::string found = findLibrary("", "G39DDCAPI.dll", why);
        CHECK(!found.empty());
        // A file of that name in the CURRENT directory must not be found through ".".
        const auto cwdSave = std::filesystem::current_path();
        eol::writeFile(dir / "OnlyHere.dll", {'M', 'Z'});
        std::filesystem::current_path(dir);
        const std::string here = findLibrary("", "OnlyHere.dll", why);
        std::filesystem::current_path(cwdSave);
        CHECK(here.empty());
        CHECK(why.find("OnlyHere.dll") != std::string::npos);
        _putenv_s("PATH", saved.c_str());
        CHECK(findLibrary("", "G39DDCAPI.dll", why).empty());
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

DWORD threadMode() { return ::GetThreadErrorMode(); }

void testLoad() {
    const auto dir = eol::scratch("load");
    const auto fx = eol::fixtureDir();
    CHECK(std::filesystem::exists(fx / "G39DDCAPI.dll"));
    // A real stand-in library loads, its exports resolve, an unknown name does not.
    {
        Library lib;
        const Check c = lib.open(eol::u8(fx / "G39DDCAPI.dll"));
        CHECK(c.ok());
        CHECK(lib.isOpen());
        CHECK(lib.symbol("GetDeviceList") != nullptr);
        CHECK(lib.symbol("NoSuchCall") == nullptr);
        CHECK(lib.symbol(nullptr) == nullptr);
        std::vector<void*> out;
        CHECK(resolveAll(lib, {"OpenDevice", "CloseDevice"}, out).empty());
        CHECK(out.size() == 2 && out[0] != nullptr && out[1] != nullptr);
        const std::string miss = resolveAll(lib, {"OpenDevice", "NopeA", "NopeB"}, out);
        CHECK(miss == "NopeA, NopeB");
        lib.close();
        CHECK(!lib.isOpen());
        CHECK(lib.symbol("GetDeviceList") == nullptr);
        lib.close();  // idempotent
    }
    // Not a library at all: refused from the bytes, the loader never asked.
    {
        const std::uint64_t before = Library::silentLoadCount();
        eol::writeFile(dir / "text.dll", std::vector<std::uint8_t>(2000, 'x'));
        eol::writeFile(dir / "empty.dll", {});
        Library lib;
        CHECK(lib.open(eol::u8(dir / "text.dll")).verdict == Verdict::NotAnImage);
        CHECK(lib.open(eol::u8(dir / "empty.dll")).verdict == Verdict::NotAnImage);
        CHECK(lib.open(eol::u8(dir / "missing.dll")).verdict == Verdict::NotFound);
        auto cut = eol::readFile(fx / "G39DDCAPI.dll");
        cut.resize(cut.size() / 2);
        eol::writeFile(dir / "cut.dll", cut);
        CHECK(lib.open(eol::u8(dir / "cut.dll")).verdict == Verdict::Truncated);
        CHECK(!lib.isOpen());
        CHECK(Library::silentLoadCount() == before);  // none of those reached LoadLibrary
        // The sentences name the file, not the folder.
        const Check c = lib.open(eol::u8(dir / "text.dll"));
        CHECK(c.sentence.find("text.dll") != std::string::npos);
        CHECK(c.sentence.find(eol::u8(dir)) == std::string::npos);
        CHECK(c.sentence.find("eol-load") == std::string::npos);
    }
    // A library whose dependency is missing: an answer, with the thread's error mode as it was.
    {
        std::filesystem::copy_file(fx / "eol_needs_dep.dll", dir / "eol_needs_dep.dll", std::filesystem::copy_options::overwrite_existing);
        const DWORD modeBefore = threadMode();
        const std::uint64_t loads = Library::silentLoadCount();
        Library lib;
        const Check c = lib.open(eol::u8(dir / "eol_needs_dep.dll"));
        CHECK(c.verdict == Verdict::LoadFailed);
        CHECK(c.sentence.find("126") != std::string::npos);
        CHECK(c.sentence.find("needs another library") != std::string::npos);
        CHECK(!lib.isOpen());
        CHECK(Library::silentLoadCount() == loads + 1);
        CHECK(threadMode() == modeBefore);
        // The same file with its dependency beside it loads (the control for the failure above).
        std::filesystem::copy_file(eol::depDir() / "eol_dep.dll", dir / "eol_dep.dll", std::filesystem::copy_options::overwrite_existing);
        Library ok;
        const Check c2 = ok.open(eol::u8(dir / "eol_needs_dep.dll"));
        CHECK(c2.ok());
        CHECK(ok.symbol("eol_needs_dep_fn") != nullptr);
        CHECK(threadMode() == modeBefore);
    }
    // Opening again closes the first; a second Library on the same file is independent.
    {
        Library a, b;
        CHECK(a.open(eol::u8(fx / "G39DDCAPI.dll")).ok());
        CHECK(b.open(eol::u8(fx / "G39DDCAPI.dll")).ok());
        CHECK(a.open(eol::u8(fx / "eol_partial.dll")).ok());
        CHECK(a.symbol("OpenDevice") == nullptr);
        CHECK(a.symbol("GetDeviceList") != nullptr);
        CHECK(b.symbol("OpenDevice") != nullptr);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

}  // namespace

int main() {
    testBytes();
    testFuzz();
    testFind();
    testLoad();
    return testSummary("test_vendor_lib");
}
