// unique_file.cpp - see unique_file.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/unique_file.hpp"

#include <cerrno>
#include <filesystem>

#include <fcntl.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace cascade::core {

std::string numberedPath(const std::string& wanted, int n) {
    if (n <= 1) { return wanted; }
    const std::size_t sep = wanted.find_last_of("/\\");
    const std::size_t start = sep == std::string::npos ? 0 : sep + 1;
    const std::size_t dot = wanted.rfind('.');
    // A dot at the start of the leaf (".hidden") or before the last separator is not an extension.
    const bool hasExt = dot != std::string::npos && dot > start;
    const std::string suffix = "-" + std::to_string(n);
    if (!hasExt) { return wanted + suffix; }
    return wanted.substr(0, dot) + suffix + wanted.substr(dot);
}

std::FILE* createExclusive(const std::string& path, CreateResult& result) {
#if defined(_WIN32)
    // The narrow path, as fopen takes it (the active code page): the names made here are ASCII.
    // _O_NOINHERIT: a child process (the SDRplay probe, a plugin's helper) must not keep a take open.
    const int fd = ::_open(path.c_str(),
                           _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT,
                           _S_IREAD | _S_IWRITE);
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
#endif
    if (fd < 0) {
        const int why = errno;
        // "Exists" is EEXIST; Windows answers EACCES for a folder (or a read-only file) in the way.
        // Something being there is what matters, so ask rather than guess from the code.
        std::error_code ec;
        const bool there = std::filesystem::exists(std::filesystem::path(path), ec);
        result = (why == EEXIST || (there && (why == EACCES || why == EISDIR))) ? CreateResult::Exists
                                                                                 : CreateResult::Failed;
        return nullptr;
    }
#if defined(_WIN32)
    std::FILE* f = ::_fdopen(fd, "wb");
#else
    std::FILE* f = ::fdopen(fd, "wb");
#endif
    if (f == nullptr) {
        // The stream could not be made: close what was created and take it away again.
#if defined(_WIN32)
        ::_close(fd);
#else
        ::close(fd);
#endif
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path(path), ec);
        result = CreateResult::Failed;
        return nullptr;
    }
    result = CreateResult::Created;
    return f;
}

std::FILE* createUnique(const std::string& wanted, std::string& usedPath, bool& exhausted,
                        int* index) {
    exhausted = false;
    for (int n = 1; n <= kMaxUniqueNames; ++n) {
        const std::string candidate = numberedPath(wanted, n);
        CreateResult r = CreateResult::Failed;
        std::FILE* f = createExclusive(candidate, r);
        if (r == CreateResult::Created) {
            usedPath = candidate;
            if (index != nullptr) { *index = n; }
            return f;
        }
        if (r == CreateResult::Failed) { return nullptr; }
        // Exists: the next name.
    }
    exhausted = true;
    return nullptr;
}

bool reserveUnique(const std::string& wanted, std::string& usedPath, bool& exhausted, int* index) {
    std::FILE* f = createUnique(wanted, usedPath, exhausted, index);
    if (f == nullptr) { return false; }
    std::fclose(f);
    return true;
}

}  // namespace cascade::core
