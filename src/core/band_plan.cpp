// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/band_plan.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace cascade::core {

namespace {

// Same tolerance rule as ConfigStore/FreqManager: absent OR wrong-typed
// leaves the default in place, so one hand-edited mistake never poisons its
// neighbours.
void getString(const json& j, const char* key, std::string& dst) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_string()) {
        dst = it->get<std::string>();
    }
}

// The one ordering rule, in one place, so loadFile and loadDirectory can
// never disagree about it. Widest-first on equal starts makes the sorted
// vector a back-to-front draw order (rationale in band_plan.hpp).
void sortEntries(std::vector<BandEntry>& v) {
    std::stable_sort(v.begin(), v.end(), [](const BandEntry& a, const BandEntry& b) {
        if (a.startHz != b.startHz) {
            return a.startHz < b.startHz;
        }
        return a.endHz > b.endHz;
    });
}

std::string lowerAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

}  // namespace

std::uint32_t BandPlan::colorForService(const std::string& service) {
    // Okabe-Ito colour-blind-safe hues at a uniform 0x60 overlay alpha; the
    // full rationale and the hex list live in band_plan.hpp so the palette is
    // documented where callers read it.
    constexpr std::uint32_t kAlpha = 0x60u;
    const auto rgba = [](std::uint32_t rgb) {
        return (rgb << 8) | kAlpha;
    };
    if (service == "broadcast") { return rgba(0xE69F00u); }
    if (service == "amateur")   { return rgba(0x56B4E9u); }
    if (service == "aviation")  { return rgba(0x009E73u); }
    if (service == "marine")    { return rgba(0x0072B2u); }
    if (service == "mobile")    { return rgba(0xD55E00u); }
    if (service == "satellite") { return rgba(0xCC79A7u); }
    if (service == "iss")       { return rgba(0xF0E442u); }
    // "other", missing, empty, or anything a future file invents: neutral
    // grey. The band still draws — an unknown class must never make an
    // allocation invisible.
    return rgba(0x9E9E9Eu);
}

std::string BandPlan::defaultDir() {
    fs::path exe;
#ifdef _WIN32
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof(buf)));
    // n == sizeof(buf) means the path was truncated: treat it as unknown
    // rather than building a directory out of a chopped path.
    if (n > 0 && n < sizeof(buf)) {
        exe = fs::path(std::string(buf, n));
    }
#else
    std::error_code ec;
    const fs::path link = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        exe = link;
    }
#endif
    // Relative fallback documented in the header: resolves when the app is
    // run from its install root, which is the only sane guess left.
    const fs::path dir = exe.empty() ? fs::path() : exe.parent_path();
    return (dir / "resources" / "bandplans").string();
}

bool BandPlan::parseInto(const std::string& path, std::vector<BandEntry>& out,
                         std::string& planName, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "band plan: cannot open \"" + path + "\" for reading";
        return false;
    }

    // allow_exceptions=false: a hand-edited file is expected to be broken
    // sometimes; that is a reportable condition, not an exceptional one.
    const json j = json::parse(f, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        error = "band plan: \"" + path + "\" is not valid JSON";
        return false;
    }
    if (!j.is_object()) {
        error = "band plan: root of \"" + path + "\" is not a JSON object";
        return false;
    }

    // Stem first, then let the file override it: a plan always has a name to
    // show in the menu even when the author forgot the key.
    planName = fs::path(path).stem().string();
    getString(j, "name", planName);

    const auto bands = j.find("bands");
    if (bands == j.end()) {
        return true;  // valid object, no collection: a legitimately empty plan
    }
    if (!bands->is_array()) {
        error = "band plan: \"bands\" in \"" + path + "\" is not an array";
        return false;
    }

    for (const auto& e : *bands) {
        // PER-ENTRY TOLERANCE — each `continue` drops one damaged band while
        // the rest of the plan survives.
        if (!e.is_object()) {
            continue;
        }
        const auto sit = e.find("start");
        const auto eit = e.find("end");
        if (sit == e.end() || eit == e.end() || !sit->is_number() || !eit->is_number()) {
            continue;  // a band without both edges covers no frequencies
        }
        const double startHz = sit->get<double>();
        const double endHz = eit->get<double>();
        if (!std::isfinite(startHz) || !std::isfinite(endHz) || !(endHz > startHz)) {
            // Zero-width and inverted bands are dropped, not repaired: there
            // is no way to guess which edge the author meant, and at()'s
            // narrowest-wins rule would rank a zero-width band above every
            // real one.
            continue;
        }

        BandEntry b;
        b.startHz = startHz;
        b.endHz = endHz;
        getString(e, "name", b.name);
        getString(e, "service", b.service);
        b.colorRgba = colorForService(b.service);
        out.push_back(std::move(b));
    }
    return true;
}

bool BandPlan::loadFile(const std::string& path, std::string& error) {
    error.clear();

    // Parse into a scratch vector and commit only on success — that is the
    // whole mechanism behind "contents unchanged on failure".
    std::vector<BandEntry> parsed;
    std::string planName;
    if (!parseInto(path, parsed, planName, error)) {
        return false;
    }
    sortEntries(parsed);
    entries_ = std::move(parsed);
    name_ = std::move(planName);
    return true;
}

bool BandPlan::loadDirectory(const std::string& dir, std::string& error) {
    error.clear();

    std::error_code ec;
    if (!fs::is_directory(fs::path(dir), ec)) {
        error = "band plan: \"" + dir + "\" is not a directory";
        return false;
    }

    // Collect first, then sort by path: directory_iterator order is
    // filesystem-defined, and the merge must not depend on it.
    std::vector<std::string> files;
    fs::directory_iterator it(fs::path(dir), ec);
    if (ec) {
        error = "band plan: cannot list \"" + dir + "\": " + ec.message();
        return false;
    }
    for (const auto& entry : it) {
        std::error_code fec;
        if (!entry.is_regular_file(fec) || fec) {
            continue;
        }
        if (lowerAscii(entry.path().extension().string()) != ".json") {
            continue;
        }
        files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());

    std::vector<BandEntry> merged;
    std::string joined;
    for (const std::string& file : files) {
        std::string planName;
        if (!parseInto(file, merged, planName, error)) {
            return false;  // all-or-nothing: nothing committed, error names the file
        }
        if (!joined.empty()) {
            joined += " + ";
        }
        joined += planName;
    }

    sortEntries(merged);
    entries_ = std::move(merged);
    name_ = std::move(joined);
    return true;
}

std::vector<const BandEntry*> BandPlan::visible(double lowHz, double highHz) const {
    std::vector<const BandEntry*> out;
    // Also rejects NaN endpoints: every comparison against NaN is false.
    if (!(highHz > lowHz)) {
        return out;
    }
    for (const BandEntry& e : entries_) {
        // Strict on both edges: a band that only touches the window edge
        // covers zero pixels (rationale in the header).
        if (e.startHz < highHz && e.endHz > lowHz) {
            out.push_back(&e);
        }
    }
    return out;
}

const BandEntry* BandPlan::at(double hz) const {
    if (!std::isfinite(hz)) {
        return nullptr;  // without this, every NaN comparison below is false
    }                    // and a NaN would "match" every band.
    const BandEntry* best = nullptr;
    double bestWidth = 0.0;
    for (const BandEntry& e : entries_) {
        if (hz < e.startHz || hz > e.endHz) {
            continue;
        }
        const double width = e.endHz - e.startHz;
        // Strictly narrower wins, so an equal-width tie keeps the FIRST match
        // in entries() order and the readout stays deterministic.
        if (best == nullptr || width < bestWidth) {
            best = &e;
            bestWidth = width;
        }
    }
    return best;
}

void BandPlan::clear() {
    entries_.clear();
    name_.clear();
}

}  // namespace cascade::core
