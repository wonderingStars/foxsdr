// safe_file_io.hpp - two small file helpers for the map models: write a whole file atomically (temp file in the same folder, then
// rename over the target) and read a whole file with a size cap. Standalone; no existing writer is used or changed (the project's
// config and diagnostics writers have their own policies and are not for these data files).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace cascade::core::safeio {

// Writes `data` to `<path>.tmp` then renames it over `path`. Creates the parent folder. Returns false on any failure (the old
// file, if any, is then still intact and the temp file is removed).
inline bool writeAtomic(const std::filesystem::path& path, const std::string& data) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        f.flush();
        if (!f) {
            f.close();
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

enum class ReadResult { Ok, Missing, TooLarge, Error };

// Reads the whole file, refusing one larger than maxBytes (a runaway or hostile file must not become a huge allocation).
inline ReadResult readCapped(const std::filesystem::path& path, std::uint64_t maxBytes, std::string& out) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return ReadResult::Missing;
    const std::uint64_t size = fs::file_size(path, ec);
    if (ec) return ReadResult::Error;
    if (size > maxBytes) return ReadResult::TooLarge;
    std::ifstream f(path, std::ios::binary);
    if (!f) return ReadResult::Error;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return ReadResult::Ok;
}

}  // namespace cascade::core::safeio
