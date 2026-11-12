// vendor_lib.hpp - loading a radio maker's library that the USER installed, at run time, without ever raising a Windows dialog.
//
// WHY. Several radios have no driver FoxSDR may ship or read: their only interface is a library from the maker (WiNRADiO's G3xDDCAPI.dll) or
// under a licence this product does not take in (LGPL, MPL). FoxSDR ships none of those files. It looks for the file the person already has,
// proves it is a library this process CAN load, loads it, and says in one plain sentence why it did not when it could not.
//
// THE FOUR WAYS A LOAD HURTS, AND WHAT IS DONE ABOUT EACH
//   * a file that is not a library at all (a download page saved as .dll, a truncated file, a text file): the PE headers are read from the file's
//     BYTES first (checkBytes), and nothing is mapped unless they describe a DLL built for this machine;
//   * a library of the other bitness (a 32-bit file cannot be mapped into a 64-bit process; the loader's own answer is error 193, which tells a
//     person nothing): named plainly, "this is a 32-bit library and FoxSDR is 64-bit", BEFORE the loader is asked;
//   * a library whose own dependency is missing (the loader's error 126, and, with default error mode, a "The program can't start because X.dll is
//     missing" box on the visible desktop): the thread's error mode is set to fail silently around the load and restored after, so the answer
//     is a sentence, never a box;
//   * a library found by a search that a stray file can win: the file is named by an absolute path the person gave, or found in the PATH
//     directories (never the current directory, never beside the executable), and loaded with the DLL's own directory and System32 as the only
//     places its dependencies are looked for.
//
// THE SENTENCES carry the file's NAME and never its folder (a folder holds the user name); nothing here is written to disk.
//
// PURE HALF: checkBytes() judges bytes and is tested with hand-built images on any platform. The Windows half (Library) loads and unloads.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_SOURCE_VENDOR_LIB_HPP
#define CASCADE_SOURCE_VENDOR_LIB_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cascade::source::vendorlib {

enum class Verdict : std::uint8_t {
    Ok = 0,
    NotFound,       // no such file / not in any PATH directory
    Unreadable,     // exists but cannot be read, or absurdly large
    NotAnImage,     // does not begin like a Windows image
    Truncated,      // a header or section runs past the end of the file
    NotALibrary,    // an executable, not a DLL
    Wrong32Bit,     // a 32-bit library and this process is 64-bit (or the reverse)
    WrongMachine,   // built for another processor
    LoadFailed,     // the checks passed and the loader still refused
    SymbolMissing,  // loaded, but an entry point the bridge needs is not exported
    NotThisPlatform // Windows-only library on another platform
};
const char* verdictWord(Verdict v);  // "ok", "not-found", "unreadable", "not-an-image", "truncated", "not-a-library", "wrong-32-bit", ...

struct Check {
    Verdict verdict = Verdict::Ok;
    std::uint32_t machine = 0;  // the image's PE Machine field when the headers were readable
    std::string sentence;       // one sentence for a person; empty when Ok
    bool ok() const { return verdict == Verdict::Ok; }
};

inline constexpr std::uint32_t kMachineX86 = 0x14C;
inline constexpr std::uint32_t kMachineX64 = 0x8664;
inline constexpr std::uint32_t kMachineArm64 = 0xAA64;
inline constexpr std::uint64_t kMaxLibraryBytes = 512ull * 1024ull * 1024ull;

// The machine this process runs on (a PE Machine number).
std::uint32_t hostMachine();

// The headers of `data` (`n` bytes: the whole file) judged for a process of machine `host`. `fileName` is only used in the sentence.
Check checkBytes(const std::uint8_t* data, std::size_t n, std::uint32_t host, const std::string& fileName);

// The file at `utf8Path`, read (bounded) and judged for THIS process. Reads only; maps nothing.
Check checkFile(const std::string& utf8Path);

// Where `fileName` is. `where` may be empty (search the PATH directories only), a directory (that directory only), or a file (that file; its
// own name is then what is loaded). Returns the absolute path as UTF-8, or empty with `why` set. Never searches the current directory.
std::string findLibrary(const std::string& where, const std::string& fileName, std::string& why);

// The last operating-system error of the calling thread (Windows: GetLastError); 0 elsewhere.
std::uint32_t lastOsError();

// A loaded library. Not copyable. open() checks the file first (checkFile), then loads it silently (see the header). close() and the
// destructor unload it; the owner must have closed every device handle first (the makers' documents say so).
class Library {
public:
    Library() = default;
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    Check open(const std::string& utf8Path);
    bool isOpen() const { return module_ != nullptr; }
    // The address of an export, or nullptr. Never throws.
    void* symbol(const char* name) const;
    void close();
    // Test hook: how many Library::open calls changed and restored the thread error mode (see the .cpp).
    static std::uint64_t silentLoadCount();

private:
    void* module_ = nullptr;
};

// Resolves `names` from `lib` into `out` (same order). Returns the names that are missing, joined with ", " (empty = all present).
std::string resolveAll(const Library& lib, const std::vector<const char*>& names, std::vector<void*>& out);

}  // namespace cascade::source::vendorlib

#endif  // CASCADE_SOURCE_VENDOR_LIB_HPP
