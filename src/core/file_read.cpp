// file_read.cpp - see file_read.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/file_read.hpp"

#include <atomic>
#include <fstream>
#include <ios>
#include <streambuf>

namespace fs = std::filesystem;

namespace cascade::core {

namespace {

std::atomic<ReadFaultHook> g_hook{nullptr};

// A buffer whose read fails by THROWING, as libstdc++'s filebuf does for a directory or a disk
// that returns an error.
class ThrowingBuf final : public std::streambuf {
protected:
    int_type underflow() override { throw std::ios_base::failure("simulated read error"); }
};

}  // namespace

void setReadFaultHookForTest(ReadFaultHook hook) { g_hook.store(hook); }

bool slurpStream(std::istream& in, std::string& out) {
    out.clear();
    char buf[16384];
    for (;;) {
        // read(), not the buffer's own iterators: it catches what the buffer throws and sets badbit.
        in.read(buf, sizeof buf);
        const std::streamsize n = in.gcount();
        if (n > 0) { out.append(buf, static_cast<std::size_t>(n)); }
        if (!in) { break; }
    }
    return !in.bad();
}

ReadResult readTextFile(const fs::path& path, std::string& text) {
    text.clear();
    try {
        std::error_code ec;
        if (fs::is_directory(path, ec)) { return ReadResult::IsDirectory; }
        std::ifstream f(path, std::ios::binary);
        if (!f) { return ReadResult::CannotOpen; }
        if (const ReadFaultHook hook = g_hook.load(); hook != nullptr && hook(path.string())) {
            ThrowingBuf buf;
            std::istream is(&buf);
            return slurpStream(is, text) ? ReadResult::Ok : ReadResult::ReadError;
        }
        return slurpStream(f, text) ? ReadResult::Ok : ReadResult::ReadError;
    } catch (...) {
        // Nothing here may end the program: a name that cannot be converted, an allocation.
        return ReadResult::ReadError;
    }
}

}  // namespace cascade::core
