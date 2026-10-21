// file_read.hpp - reading a whole file in a way a bad file cannot end the program (0.99.65).
//
// WHAT WENT WRONG. A loader read its file through a stream - `json::parse(std::ifstream&)`, or
// `std::string((std::istreambuf_iterator<char>(f)), {})`. Both read the stream's BUFFER directly,
// past the stream's own error handling, so when the buffer's read fails (libstdc++'s filebuf throws
// `std::ios_base::failure` from underflow) the exception goes straight out of the loader. On
// Linux, opening a DIRECTORY with an ifstream succeeds and the read then fails - "basic_filebuf::
// underflow error reading the file: Is a directory" - so a folder where config.json, bookmarks.json,
// markers.json or a band plan should be ended the program with an uncaught exception (found by
// tests/test_failure_files.cpp on Linux). On Windows the open fails and nothing throws. The same
// exception comes from a real read error on a disk that is failing (EIO).
//
// THE RULE. A file is read with readTextFile: a directory is answered IsDirectory, a file that will
// not open CannotOpen, and a read that fails - by an error return or by a throw from the buffer -
// ReadError. Never an exception. slurpStream is the reading loop: it uses istream::read, which turns
// a throwing buffer into badbit, and answers false when the stream went bad.
//
// A seam (setReadFaultHookForTest, null in every shipped build): for a path it returns true for, the
// read behaves as a buffer that throws - the platform-independent way to meet the failure the
// loaders had on Linux only.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_FILE_READ_HPP
#define CASCADE_CORE_FILE_READ_HPP

#include <filesystem>
#include <istream>
#include <string>

namespace cascade::core {

enum class ReadResult { Ok, IsDirectory, CannotOpen, ReadError };

// Reads the rest of `in` into `out`. False when the stream went bad (an error, or a buffer that
// threw); `out` then holds what was read before. Never throws.
bool slurpStream(std::istream& in, std::string& out);

// Reads the whole of `path` into `text` (cleared first). Never throws.
ReadResult readTextFile(const std::filesystem::path& path, std::string& text);
inline ReadResult readTextFile(const std::string& path, std::string& text) {
    return readTextFile(std::filesystem::path(path), text);
}
inline ReadResult readTextFile(const char* path, std::string& text) {
    return readTextFile(std::filesystem::path(path), text);
}

// Tests only: a path this returns true for is read as a buffer that throws, as a failing disk's
// is. Pass nullptr to remove it.
using ReadFaultHook = bool (*)(const std::string& path);
void setReadFaultHookForTest(ReadFaultHook hook);

}  // namespace cascade::core

#endif  // CASCADE_CORE_FILE_READ_HPP
