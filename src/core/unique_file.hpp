// unique_file.hpp - a file is never opened over one that is already there (0.99.65).
//
// WHAT WENT WRONG. A take is named for the SECOND it was asked for
// ("audio_20261005_101010_48000Hz.wav") and was opened with fopen("wb"), which truncates. Stop
// followed by Record inside one second therefore opened the same name over the take that had just
// been finished (and was perhaps still being finalised): one file, holding only the second take,
// three runs out of three. The same name could also be asked for by two writers of this process
// in the same second - two patch speakers named alike, the main recorder and a speaker - or be a
// file another program had left there.
//
// THE RULE. When the name a file would get is taken, the file gets a number before its extension:
// "name.wav", then "name-2.wav", "name-3.wav", up to kMaxUniqueNames. The name is decided WHERE THE
// FILE IS OPENED - on the worker, never on the window's thread - and the check and the create are
// one operation: the file is created EXCLUSIVELY (O_EXCL / _O_EXCL, i.e. CREATE_NEW), so there is
// no window between looking and opening in which another writer, or another program, can take the
// name. A name that is taken - by a finished file, one still being finalised, one another writer
// of this process has just made, or a stranger's - is stepped past; the first "exists" is not an
// error. Anything else (the folder is not there, access refused, the disk is full) fails at once.
//
// TWO FLAVOURS. createUnique returns the open stream, for a writer that owns its FILE* (the
// recorders). reserveUnique creates the file empty and closes it, for a writer that opens by name
// itself (Media Foundation's MP3 sink writer, a std::ofstream): the name is still taken atomically
// by the exclusive create, and the writer then opens a file that is already its own. (That second
// open is not exclusive - Media Foundation creates its own file - but nobody else who uses this
// can have the name.)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_UNIQUE_FILE_HPP
#define CASCADE_CORE_UNIQUE_FILE_HPP

#include <cstdio>
#include <string>

namespace cascade::core {

// How many names are tried: "name.ext", "name-2.ext" ... "name-99.ext".
inline constexpr int kMaxUniqueNames = 99;

// `wanted` for n == 1; for n >= 2 the number goes before the LAST extension of the file name
// ("a/b.wav" -> "a/b-2.wav"), or at the end when there is none. Everything else, including the
// separators, is kept exactly as given.
std::string numberedPath(const std::string& wanted, int n);

enum class CreateResult { Created, Exists, Failed };

// Creates `path` exclusively and returns it open for binary writing (null unless Created). Exists
// means something is there already - a file, a read-only file, a folder - and is the only result
// worth stepping past.
std::FILE* createExclusive(const std::string& path, CreateResult& result);

// Steps through numberedPath(wanted, 1..kMaxUniqueNames) until one is created. `usedPath` is the
// name that was; `index` (when given) the n it was. Null with `exhausted` true when every name was
// taken; null with `exhausted` false on any other failure.
std::FILE* createUnique(const std::string& wanted, std::string& usedPath, bool& exhausted,
                        int* index = nullptr);

// The same, creating the file empty and closing it. False on exhaustion or any other failure.
bool reserveUnique(const std::string& wanted, std::string& usedPath, bool& exhausted,
                   int* index = nullptr);

}  // namespace cascade::core

#endif  // CASCADE_CORE_UNIQUE_FILE_HPP
