// plugin_dir_signature.cpp - see plugin_dir_signature.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_dir_signature.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace cascade::core {

bool readPluginDirSignature(const std::string& dir, PluginDirSignature& out) {
    out = PluginDirSignature{};
    out.dir = dir;

    // The same question PluginHost::scan asks first, and the same answer to a
    // folder that is not there: an empty list, not a failure. A folder whose
    // state cannot be told at all is a failure here - "I do not know" - because
    // a skip decided on a guess is the one wrong answer this file can give.
    std::error_code ec;
    const fs::file_status st = fs::status(fs::path(dir), ec);
    if (st.type() == fs::file_type::not_found) { return true; }
    if (ec) { return false; }
    if (!fs::is_directory(st)) { return true; }  // a file where the folder should be: no modules

    // skip_permission_denied is what scan() lists with: an entry the scan cannot
    // see is one the signature does not need to.
    fs::directory_iterator it(fs::path(dir), fs::directory_options::skip_permission_denied, ec);
    if (ec) { return false; }
    out.listed = true;
    const fs::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) { return false; }  // a folder that changed under the listing
        std::error_code entryEc;
        if (!it->is_regular_file(entryEc)) {
            if (entryEc) { return false; }
            continue;
        }
        PluginDirSignature::Entry e;
        e.name = it->path().filename().string();
        // Both come with the listing's entry (on Windows, from the find data it
        // was made from): neither is a question to the file.
        const std::uintmax_t bytes = it->file_size(entryEc);
        if (entryEc) { return false; }
        const fs::file_time_type when = it->last_write_time(entryEc);
        if (entryEc) { return false; }
        e.bytes = static_cast<std::uint64_t>(bytes);
        e.writeTime = static_cast<std::int64_t>(when.time_since_epoch().count());
        out.files.push_back(std::move(e));
    }
    std::sort(out.files.begin(), out.files.end(),
              [](const PluginDirSignature::Entry& a, const PluginDirSignature::Entry& b) {
                  return a.name < b.name;
              });
    return true;
}

}  // namespace cascade::core
