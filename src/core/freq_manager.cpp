// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/freq_manager.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unordered_set>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace cascade::core {

namespace {

// Same tolerance rule as ConfigStore: absent OR wrong-typed leaves the
// default in place, so one hand-edited mistake never poisons its neighbors.

void getString(const json& j, const char* key, std::string& dst) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_string()) {
        dst = it->get<std::string>();
    }
}

void getDouble(const json& j, const char* key, double& dst) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_number()) {  // integer literals accepted too
        dst = it->get<double>();
    }
}

}  // namespace

std::string FreqManager::defaultPath() {
    // Mirrors ConfigStore::defaultPath so both files live in one app dir.
#ifdef _WIN32
    const char* base = std::getenv("APPDATA");
    const fs::path dir = (base && *base) ? fs::path(base) : fs::path(".");
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    const fs::path dir = (xdg && *xdg)    ? fs::path(xdg)
                         : (home && *home) ? fs::path(home) / ".config"
                                           : fs::path(".");
#endif
    return (dir / "foxsdr" / "bookmarks.json").string();
}

bool FreqManager::load(const std::string& path, std::string& error) {
    // Clear first: every return below then already satisfies the contract
    // that failure paths leave an EMPTY list, never a stale one.
    list_.clear();
    ++version_;
    error.clear();

    std::error_code ec;
    if (!fs::exists(fs::path(path), ec)) {
        return true;  // first run: no bookmarks, and nothing went wrong
    }

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "bookmarks: cannot open \"" + path + "\" for reading";
        return false;
    }

    // allow_exceptions=false: a corrupt file is an expected condition here,
    // not an exceptional one; parse errors surface as a discarded value.
    const json j = json::parse(f, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        error = "bookmarks: \"" + path + "\" is not valid JSON";
        return false;
    }
    if (!j.is_object()) {
        error = "bookmarks: root of \"" + path + "\" is not a JSON object";
        return false;
    }

    // Schema gate before reading entries, same reasoning as ConfigStore: a
    // different schema number may have reinterpreted fields, so none of the
    // entries can be trusted. Missing schemaVersion is fine (defaults to 1).
    {
        const auto it = j.find("schemaVersion");
        if (it != j.end() && (!it->is_number_integer() || it->get<int>() != 1)) {
            error = "bookmarks: \"" + path +
                    "\" has an unsupported schemaVersion (expected 1)";
            return false;
        }
    }

    const auto bm = j.find("bookmarks");
    if (bm == j.end()) {
        return true;  // valid object with no collection: empty list
    }
    if (!bm->is_array()) {
        error = "bookmarks: \"bookmarks\" in \"" + path + "\" is not an array";
        return false;
    }

    for (const auto& e : *bm) {
        // PER-ENTRY TOLERANCE — every `continue` here is one damaged entry
        // being dropped while the rest of the list survives.
        if (!e.is_object()) {
            continue;  // bare strings/numbers/etc. cannot carry a frequency
        }
        const auto fit = e.find("freqHz");
        if (fit == e.end() || !fit->is_number()) {
            continue;  // no usable frequency: the bookmark points at nothing
        }
        const double freq = fit->get<double>();
        if (!std::isfinite(freq) || freq < 0.0) {
            continue;  // non-finite or negative: not a tunable frequency
        }

        Bookmark b;
        b.freqHz = freq;
        getString(e, "name", b.name);
        getString(e, "mode", b.mode);  // unknown mode strings kept verbatim
        getDouble(e, "bandwidthHz", b.bandwidthHz);
        getString(e, "group", b.group);
        {
            const auto fav = e.find("favourite");
            if (fav != e.end() && fav->is_boolean()) { b.favourite = fav->get<bool>(); }
        }
        if (!std::isfinite(b.bandwidthHz) || b.bandwidthHz <= 0.0) {
            // The demod chain divides by bandwidth; repair with the struct
            // default rather than inventing an epsilon floor.
            b.bandwidthHz = Bookmark{}.bandwidthHz;
        }
        list_.push_back(std::move(b));
    }

    // stable_sort: entries with equal frequencies keep their file order,
    // matching the insertion-order stability add() provides.
    std::stable_sort(list_.begin(), list_.end(),
                     [](const Bookmark& a, const Bookmark& b) {
                         return a.freqHz < b.freqHz;
                     });
    return true;
}

bool FreqManager::save(const std::string& path, std::string& error) const {
    error.clear();
    const fs::path target(path);

    std::error_code ec;
    const fs::path parent = target.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        // create_directories is a no-op without error on an existing
        // directory, but reports one if a FILE squats on the path.
        if (ec || !fs::is_directory(parent)) {
            error = "bookmarks: cannot create directory \"" + parent.string() +
                    "\": " + (ec ? ec.message() : "path exists and is not a directory");
            return false;
        }
    }

    json arr = json::array();
    for (const Bookmark& b : list_) {
        json e;
        e["name"] = b.name;
        e["freqHz"] = b.freqHz;
        e["mode"] = b.mode;
        e["bandwidthHz"] = b.bandwidthHz;
        if (!b.group.empty()) { e["group"] = b.group; }
        if (b.favourite) { e["favourite"] = true; }
        arr.push_back(std::move(e));
    }
    json j;
    j["schemaVersion"] = 1;
    j["bookmarks"] = std::move(arr);
    // error_handler_t::replace, like every dump() in this tree: this text
    // includes user-entered or remote strings, and a byte that is not valid
    // UTF-8 must cost one replacement character, never a throw out of a save
    // path. tests/test_json_dump_policy.cpp holds every site to this.
    const std::string text =
        j.dump(4, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";

    // ATOMIC WRITE — the ConfigStore approach verbatim. The temp file lives
    // in the target's own directory so the final rename is a same-volume
    // move; cross-volume "renames" degrade to copy+delete, which is exactly
    // the partial-write window this exists to close. The pid suffix keeps
    // parallel test suites from colliding.
#ifdef _WIN32
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    fs::path tmp = target;
    tmp += "." + std::to_string(pid) + ".tmp";

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            error = "bookmarks: cannot create temp file \"" + tmp.string() + "\"";
            return false;
        }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f) {
            f.close();
            fs::remove(tmp, ec);  // best effort; the write already failed
            error = "bookmarks: write to temp file \"" + tmp.string() + "\" failed";
            return false;
        }
    }

    fs::rename(tmp, target, ec);
    if (ec) {
        // Target locked, permission lost, etc. The old file is untouched —
        // that is the whole point — but the temp must not accumulate.
        std::error_code ignored;
        fs::remove(tmp, ignored);
        error = "bookmarks: atomic replace of \"" + path + "\" failed: " + ec.message();
        return false;
    }
    return true;
}

std::size_t FreqManager::insertSorted(Bookmark b) {
    // upper_bound (not lower_bound): equal frequencies land AFTER their
    // peers, so repeated adds of one frequency keep insertion order.
    const auto it = std::upper_bound(list_.begin(), list_.end(), b.freqHz,
                                     [](double f, const Bookmark& x) {
                                         return f < x.freqHz;
                                     });
    const std::size_t idx = static_cast<std::size_t>(it - list_.begin());
    list_.insert(it, std::move(b));
    ++version_;
    return idx;
}

int FreqManager::add(Bookmark b) {
    const auto nameInUse = [this](const std::string& n) {
        for (const Bookmark& x : list_) {
            if (x.name == n) {
                return true;
            }
        }
        return false;
    };
    if (nameInUse(b.name)) {
        // First unused counter wins: "X" -> "X (2)" -> "X (3)" ... skipping
        // over suffixes the user already claimed by hand.
        int n = 2;
        std::string candidate;
        do {
            candidate = b.name + " (" + std::to_string(n) + ")";
            ++n;
        } while (nameInUse(candidate));
        b.name = std::move(candidate);
    }
    return static_cast<int>(insertSorted(std::move(b)));
}

bool FreqManager::removeAt(std::size_t index) {
    if (index >= list_.size()) {
        return false;
    }
    list_.erase(list_.begin() + static_cast<std::ptrdiff_t>(index));
    ++version_;
    return true;
}

bool FreqManager::updateAt(std::size_t index, const Bookmark& b) {
    if (index >= list_.size()) {
        return false;
    }
    // Erase-then-reinsert rather than assign-then-sort: it reuses the one
    // insertion path that maintains the invariant, so there is exactly one
    // place where ordering can be right or wrong.
    list_.erase(list_.begin() + static_cast<std::ptrdiff_t>(index));
    insertSorted(b);
    return true;
}

std::size_t FreqManager::addMany(std::vector<Bookmark> items) {
    // What is already here, by (frequency, name), so a re-import is a no-op.
    // Hashing the pair as a string keeps this O(n) for the lookups.
    const auto key = [](const Bookmark& b) {
        char f[40];
        std::snprintf(f, sizeof(f), "%.3f|", b.freqHz);
        return std::string(f) + b.name;
    };
    std::unordered_set<std::string> have;
    have.reserve(list_.size() + items.size());
    for (const Bookmark& b : list_) { have.insert(key(b)); }
    std::size_t added = 0;
    list_.reserve(list_.size() + items.size());
    for (Bookmark& b : items) {
        if (!std::isfinite(b.freqHz) || b.freqHz < 0.0) { continue; }
        if (!have.insert(key(b)).second) { continue; }
        list_.push_back(std::move(b));
        ++added;
    }
    if (added > 0) {
        std::stable_sort(list_.begin(), list_.end(),
                         [](const Bookmark& a, const Bookmark& b) { return a.freqHz < b.freqHz; });
        ++version_;
    }
    return added;
}

std::size_t FreqManager::removeGroup(const std::string& group) {
    const std::size_t before = list_.size();
    list_.erase(std::remove_if(list_.begin(), list_.end(),
                               [&](const Bookmark& b) { return b.group == group; }),
                list_.end());
    const std::size_t removed = before - list_.size();
    if (removed > 0) { ++version_; }
    return removed;
}

std::vector<std::size_t> FreqManager::nearestSubset(double hereHz, std::size_t maxCount,
                                                    std::size_t maxFavourites) const {
    std::vector<std::size_t> out;
    if (list_.size() <= maxCount) {
        for (std::size_t i = 0; i < list_.size(); ++i) { out.push_back(i); }
        return out;
    }
    for (std::size_t i = 0; i < list_.size() && out.size() < std::min(maxFavourites, maxCount); ++i) {
        if (list_[i].favourite) { out.push_back(i); }
    }
    const std::size_t favs = out.size();
    // Walk outward from where hereHz would sit, nearer side first.
    std::size_t hi = range(hereHz, 1.0e300).first;
    std::size_t lo = hi;
    while (out.size() < maxCount && (lo > 0 || hi < list_.size())) {
        const bool takeLo = lo > 0 && (hi >= list_.size() ||
                                       hereHz - list_[lo - 1].freqHz <= list_[hi].freqHz - hereHz);
        const std::size_t i = takeLo ? --lo : hi++;
        // A favourite already taken is not taken twice; one beyond the
        // favourite cap is as welcome here as any other.
        if (list_[i].favourite && std::binary_search(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(favs), i)) {
            continue;
        }
        out.push_back(i);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::pair<std::size_t, std::size_t> FreqManager::range(double loHz, double hiHz) const {
    const auto lo = std::lower_bound(list_.begin(), list_.end(), loHz,
                                     [](const Bookmark& x, double f) { return x.freqHz < f; });
    const auto hi = std::upper_bound(lo, list_.end(), hiHz,
                                     [](double f, const Bookmark& x) { return f < x.freqHz; });
    return {static_cast<std::size_t>(lo - list_.begin()), static_cast<std::size_t>(hi - list_.begin())};
}

}  // namespace cascade::core
