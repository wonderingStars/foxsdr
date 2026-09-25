// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/ui_census.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>

namespace cascade::gui::census {

namespace {

struct Rect {
    float x0, y0, x1, y1;
};

const char* path() {
    static const char* const p = [] {
        const char* v = std::getenv("FOXSDR_UI_CENSUS");
        return (v != nullptr && v[0] != '\0') ? v : nullptr;
    }();
    return p;
}

std::set<std::string>& items() {
    static std::set<std::string> s;
    return s;
}

std::map<std::string, Rect>& rects() {
    static std::map<std::string, Rect> m;
    return m;
}

}  // namespace

namespace detail {

bool readEnabled() { return path() != nullptr; }

void noteParts(std::string_view prefix, std::string_view name) {
    items().insert(std::string(prefix) + std::string(name));
}

void noteIndex(std::string_view prefix, int index) {
    items().insert(std::string(prefix) + std::to_string(index));
}

void rectParts(std::string_view prefix, std::string_view name, float x0, float y0, float x1,
               float y1) {
    rects()[std::string(prefix) + std::string(name)] = Rect{x0, y0, x1, y1};
}

void rectIndex(std::string_view prefix, int index, float x0, float y0, float x1, float y1) {
    rects()[std::string(prefix) + std::to_string(index)] = Rect{x0, y0, x1, y1};
}

}  // namespace detail

bool write() {
    if (!enabled()) { return false; }
    std::FILE* f = std::fopen(path(), "wb");
    if (f == nullptr) { return false; }
    for (const std::string& s : items()) { std::fprintf(f, "item %s\n", s.c_str()); }
    for (const auto& [name, r] : rects()) {
        std::fprintf(f, "rect %s %.2f %.2f %.2f %.2f\n", name.c_str(), static_cast<double>(r.x0),
                     static_cast<double>(r.y0), static_cast<double>(r.x1),
                     static_cast<double>(r.y1));
    }
    return std::fclose(f) == 0;
}

}  // namespace cascade::gui::census
