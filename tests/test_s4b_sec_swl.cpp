// test_s4b_sec_swl.cpp - security lane S4b-sec, finding "SwlBook::load throws on a wrong-typed field": the listening log is a file in the person's own folder; a field of the wrong type (a text
// where a number belongs) must be a damaged-file answer with a plain sentence and an empty log, never an exception while the engine starts. The damaged file is kept beside the log.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "engine/s4b_swl.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::engine::s4b::SwlBook;

namespace {

void put(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

// Constructing the book runs load(): returns false when it threw.
bool loads(const fs::path& file, std::size_t& count, std::string& error) {
    try {
        SwlBook b(file);
        count = b.size();
        error = b.error();
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "foxsdr-s4bsec-swl";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const fs::path file = dir / "swl-log.json";
    std::size_t n = 99;
    std::string err;

    // a good file: both entries read
    put(file, R"({"version":1,"entries":[{"t":1700000000,"d":0,"f":9410000,"m":"AM","c":"BBC","s":"World","n":"x","r":1,"p":-40.5},{"t":1700000100,"f":6000000}]})");
    CHECK(loads(file, n, err) && n == 2 && err.empty());

    // every field of the wrong type is a damaged file, not an exception: text for each number, a number for each text, a null, an array, a bare string as an entry
    const char* bad[] = {
        R"({"entries":[{"t":"1700000000","f":9410000}]})",
        R"({"entries":[{"t":1700000000,"f":"9410000"}]})",
        R"({"entries":[{"t":1700000000,"f":9410000,"d":"long"}]})",
        R"({"entries":[{"t":1700000000,"f":9410000,"m":5}]})",
        R"({"entries":[{"t":1700000000,"f":9410000,"c":[1]}]})",
        R"({"entries":[{"t":1700000000,"f":9410000,"s":null}]})",
        R"({"entries":[{"t":1700000000,"f":9410000,"n":{"a":1}}]})",
        R"({"entries":[{"t":1700000000,"f":9410000,"r":"one"}]})",
        R"({"entries":[{"t":1700000000,"f":9410000,"r":1.5}]})",
        R"({"entries":[{"t":1700000000,"f":9410000,"p":"loud"}]})",
        R"({"entries":[{"t":1.5e9,"f":9410000}]})",
        R"({"entries":["not an object"]})",
        R"({"entries":{"a":1}})",
        R"([1,2,3])",
        "not json at all",
    };
    for (const char* text : bad) {
        put(file, text);
        fs::remove(dir / "swl-log.json.damaged", ec);
        n = 99;
        err.clear();
        const bool ok = loads(file, n, err);
        CHECK(ok);                                                      // never an exception
        CHECK(ok && n == 0);                                            // an empty log
        CHECK(ok && err == "the listening log is damaged and was not used");   // the plain sentence
        CHECK(fs::exists(dir / "swl-log.json.damaged"));                 // the person's file is kept, not left for the next save to overwrite
        CHECK(!fs::exists(file));
    }

    // the kept file is the damaged one, byte for byte; a fresh book on the now-missing file starts empty and saves normally
    {
        put(file, R"({"entries":[{"t":"x","f":1}]})");
        fs::remove(dir / "swl-log.json.damaged", ec);
        SwlBook b(file);
        std::ifstream f(dir / "swl-log.json.damaged", std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        CHECK(ss.str() == R"({"entries":[{"t":"x","f":1}]})");
        std::string why;
        CHECK(b.add("", 1700000000, 9410000.0, "AM", -50.0, 1, why));
        CHECK(fs::exists(file));
        SwlBook again(file);
        CHECK(again.size() == 1 && again.error().empty());
    }

    // an entry that is merely unusable (no frequency) is skipped as before, it is not damage
    put(file, R"({"entries":[{"t":1700000000,"f":0},{"t":1700000001,"f":7000000}]})");
    CHECK(loads(file, n, err) && n == 1 && err.empty());

    fs::remove_all(dir, ec);
    return testSummary("test_s4b_sec_swl");
}
