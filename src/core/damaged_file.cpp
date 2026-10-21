// damaged_file.cpp - see damaged_file.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/damaged_file.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace cascade::core {

namespace {

// "<leaf>.bad-" - what every kept copy of `leaf` starts with.
std::string badPrefix(const std::string& leaf) { return leaf + ".bad-"; }

// The part of a copy's name after the prefix: "yyyymmdd-hhmmss" and an optional "-N". Ordered by
// the stamp, then NUMERICALLY by N (so "-10" is newer than "-2", which a plain string compare
// would get wrong).
struct CopyKey {
    std::string stamp;
    int n = 1;
    bool operator<(const CopyKey& o) const {
        if (stamp != o.stamp) { return stamp < o.stamp; }
        return n < o.n;
    }
};

bool parseKey(const std::string& rest, CopyKey& key) {
    if (rest.size() < 15) { return false; }
    key.stamp = rest.substr(0, 15);
    key.n = 1;
    if (rest.size() == 15) { return true; }
    if (rest[15] != '-' || rest.size() == 16) { return false; }
    int n = 0;
    for (std::size_t i = 16; i < rest.size(); ++i) {
        if (rest[i] < '0' || rest[i] > '9') { return false; }
        n = n * 10 + (rest[i] - '0');
        if (n > 100000) { return false; }
    }
    key.n = n;
    return true;
}

}  // namespace

SetAsideResult setDamagedFileAside(const std::string& path, std::time_t now, int keep) {
    SetAsideResult r;
    try {
        const fs::path file(path);
        std::error_code ec;
        const fs::file_status st = fs::status(file, ec);
        if (ec || !fs::is_regular_file(st)) { return r; }  // missing, or a folder: nothing of ours
        const std::uintmax_t size = fs::file_size(file, ec);
        if (ec || size == 0) { return r; }                 // empty: nothing to lose
        r.bytes = size;

        std::tm tmv{};
#if defined(_WIN32)
        gmtime_s(&tmv, &now);
#else
        gmtime_r(&now, &tmv);
#endif
        char stamp[32];
        std::snprintf(stamp, sizeof stamp, "%04d%02d%02d-%02d%02d%02d", tmv.tm_year + 1900,
                      tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

        const fs::path dir = file.parent_path();
        const std::string leaf = file.filename().string();
        const std::string prefix = badPrefix(leaf);

        // THE COPIES THERE ARE ALREADY, in this folder.
        std::vector<std::pair<CopyKey, fs::path>> copies;
        std::error_code ls;
        for (fs::directory_iterator it(dir.empty() ? fs::path(".") : dir, ls), end; !ls && it != end;
             it.increment(ls)) {
            std::error_code te;
            if (!it->is_regular_file(te) || te) { continue; }
            const std::string name = it->path().filename().string();
            if (name.rfind(prefix, 0) != 0) { continue; }
            CopyKey key;
            if (!parseKey(name.substr(prefix.size()), key)) { continue; }
            copies.emplace_back(key, it->path());
        }

        // THE NAME: this second's stamp, and after it the number past every copy already made in
        // this second - never a free number below one that is there (a number freed by the pruning
        // below would make the copy just made sort as the OLDEST, and the pruning would take it).
        // filesystem::rename replaces what is there, so the name must be known to be free.
        int n = 1;
        for (const auto& c : copies) {
            if (c.first.stamp == stamp) { n = std::max(n, c.first.n + 1); }
        }
        fs::path target = dir / (prefix + stamp + (n == 1 ? "" : "-" + std::to_string(n)));
        std::error_code ex;
        if (n > 99 || fs::symlink_status(target, ex).type() != fs::file_type::not_found) {
            r.outcome = SetAsideResult::Outcome::CouldNotKeep;
            return r;
        }
        std::error_code rn;
        fs::rename(file, target, rn);
        if (rn) {
            r.outcome = SetAsideResult::Outcome::CouldNotKeep;
            return r;
        }
        r.outcome = SetAsideResult::Outcome::KeptAside;
        r.keptAs = target.string();

        // THE THREE NEWEST COPIES OF THIS FILE ARE KEPT: the rest, oldest first, go.
        CopyKey made;
        made.stamp = stamp;
        made.n = n;
        copies.emplace_back(made, target);
        std::sort(copies.begin(), copies.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        const std::size_t kept = keep < 0 ? 0u : static_cast<std::size_t>(keep);
        if (copies.size() > kept) {
            for (std::size_t i = 0; i + kept < copies.size(); ++i) {
                std::error_code rm;
                fs::remove(copies[i].second, rm);
            }
        }
    } catch (...) {
        // Nothing here may throw out of start-up: say it could not be kept.
        if (r.bytes != 0 && r.outcome == SetAsideResult::Outcome::NothingToKeep) {
            r.outcome = SetAsideResult::Outcome::CouldNotKeep;
        }
    }
    return r;
}

}  // namespace cascade::core
