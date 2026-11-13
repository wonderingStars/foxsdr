// test_engine_api_wireb_history.cpp - THE SWEEP HISTORY SAVED AND LOADED (wire-b second pass, item 37 first half; FOXAPP_OP_SWEEP_HISTORY 0x8931; engine/wire_b_history.{hpp,cpp} over
// core/sweep_history.hpp), through a real Engine. The sweep lines come from the sweep test's STAND-IN tinySA (tests/test_engine_api_wireb_sweep.cpp's FakeTinySa, included under a renamed
// main): no COM port is opened.
//
//   H0  nothing kept: the fact is there with zero lines, save and clear are NO_CHANGE, a relative path is BAD_ARGUMENT, a file that is not there is FAILED, a REMOTE session is DENIED
//   H1  the lines the stand-in sweeps are kept; SWEEP_HISTORY save writes a PLAIN TEXT file whose first line is the header `#foxsdr-sweep-history,csv,1` and second the column line; a second
//       save to the same path is refused and the file is untouched; JSON and XML carry the same data; an unknown format is BAD_ARGUMENT
//   H2  clear empties the memory; load of the CSV, the JSON and the XML each brings back EXACTLY the lines that were saved (bit for bit), makes the host "history" and closed with its heat map
//       rebuilt and SWEEP_READ answering from the loaded lines (the 100 MHz tone)
//   H3  refusals change nothing: a corrupt file (BAD_ARGUMENT with its line), a file over 4 MiB, a folder, a load while a sweep is running (BUSY); the note fact says why in words
//   H4  no fact carries a path
//
// UNVERIFIED on a real tinySA. Files are written under the temp folder with the process id in the name and removed at the end.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#define main wb_sweep_model_main
#include "test_engine_api_wireb_sweep.cpp"
#undef main

#include <process.h>
#include <filesystem>
#include <fstream>

namespace {

namespace fsys = std::filesystem;

std::string readAll(const fsys::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

json historyFact(Rig& r) {
    const json j = json::parse(bigFact(r, "sweep.history"), nullptr, false);
    return j.is_discarded() ? json() : j;
}

std::string hist(const char* action, const fsys::path& p, const char* format = nullptr) {
    json j = json::object();
    j["action"] = action;
    if (!p.empty()) { j["path"] = p.string(); }
    if (format != nullptr) { j["format"] = format; }
    return j.dump();
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_history\n");
    engine_api_test::hermetic("wirebhistory");
    Access::installHooks();
    const fsys::path dir = fsys::temp_directory_path() / ("wirebhistory-" + std::to_string(static_cast<long long>(_getpid())));
    std::error_code ec;
    fsys::remove_all(dir, ec);
    fsys::create_directories(dir, ec);
    Rig r;
    cascade::engine::WireBState& w = r.eng.wireBStateForTest();
    FakeTinySa* fake = nullptr;
    w.sweepFactory = [&](const std::string&, std::string&) -> std::unique_ptr<tsa::SerialLink> {
        auto f = std::make_unique<FakeTinySa>();
        fake = f.get();
        return f;
    };
    w.rfeFactory = [](const std::string&, int, std::string& why) -> std::unique_ptr<tsa::SerialLink> { why = "no port in this test"; return nullptr; };
    const fsys::path csv = dir / "sweeps.csv", js = dir / "sweeps.json", xml = dir / "sweeps.xml";

    std::printf("H0: nothing kept\n");
    {
        const json j = historyFact(r);
        CHECK(j.is_object());
        if (j.is_object()) {
            CHECK_EQ(j["lines"].get<std::size_t>(), static_cast<std::size_t>(0));
            CHECK_EQ(j["points"].get<std::size_t>(), static_cast<std::size_t>(0));
            CHECK_EQ(j["keep_points"].get<std::size_t>(), static_cast<std::size_t>(300000));
        }
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_HISTORY, hist("save", csv)).status(), FOXAPI_NO_CHANGE);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_HISTORY, hist("clear", fsys::path())).status(), FOXAPI_NO_CHANGE);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_HISTORY, hist("save", fsys::path("relative.csv"))).status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_HISTORY, hist("load", dir / "not-there.csv")).status(), FOXAPI_FAILED);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_HISTORY, "{\"action\":\"burn\"}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_HISTORY, "{\"action\":\"save\",\"path\":\"C:/x\",\"extra\":1}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_HISTORY, hist("clear", dir / "x")).status(), FOXAPI_BAD_ARGUMENT);   // clear takes no path
        testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
        CHECK(remote.ok());
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_HISTORY, hist("save", csv), &remote).status(), FOXAPI_DENIED);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_HISTORY, hist("load", csv), &remote).status(), FOXAPI_DENIED);
        CHECK(!fsys::exists(csv));
    }

    std::printf("H1: the lines are kept, and saved as plain text with a header line\n");
    CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, kOpen).status(), FOXAPI_OK);
    CHECK(r.until([&] { const json j = state(r); return j.is_object() && j["sweeps"].get<std::uint64_t>() >= 6; }, 30000));
    CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_CLOSE, "").status(), FOXAPI_OK);
    CHECK(r.until([&] { return !state(r)["open"].get<bool>(); }, 10000));
    const cascade::core::SweepHistory kept = w.sweep.historySnapshot();
    const std::size_t nKept = kept.sweeps.size();
    std::printf("  kept %zu sweeps of %zu points\n", nKept, kept.sweeps.empty() ? std::size_t{0} : kept.sweeps[0].db.size());
    CHECK(nKept >= 6);
    {
        const json j = historyFact(r);
        CHECK_EQ(j["lines"].get<std::size_t>(), nKept);
        CHECK_EQ(j["points"].get<std::size_t>(), nKept * 450);
        const auto o = sendText(r, FOXAPP_OP_SWEEP_HISTORY, hist("save", csv));
        CHECK_EQ(o.status(), FOXAPI_OK);
        CHECK_EQ(o.result.applied[0], static_cast<double>(nKept));
        const std::string text = readAll(csv);
        const std::size_t nl1 = text.find('\n');
        const std::size_t nl2 = text.find('\n', nl1 + 1);
        CHECK(nl1 != std::string::npos && nl2 != std::string::npos);
        CHECK_EQ(text.substr(0, nl1), std::string("#foxsdr-sweep-history,csv,1"));
        CHECK_EQ(text.substr(nl1 + 1, nl2 - nl1 - 1), std::string("time_s,start_hz,stop_hz,points,db"));
        std::printf("  csv: %zu bytes, header \"%s\"\n", text.size(), text.substr(0, nl1).c_str());
        CHECK(text.find('\0') == std::string::npos);
        CHECK_EQ(historyFact(r)["note"].get<std::string>(), std::string("saved ") + std::to_string(nKept) + " sweeps");
        // never over an existing file
        const std::string before = readAll(csv);
        const auto again = sendText(r, FOXAPP_OP_SWEEP_HISTORY, hist("save", csv));
        CHECK_EQ(again.status(), FOXAPI_FAILED);
        CHECK(std::string(again.result.message).find("exists") != std::string::npos);
        CHECK(readAll(csv) == before);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_HISTORY, hist("save", js, "json")).status(), FOXAPI_OK);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_HISTORY, hist("save", xml, "xml")).status(), FOXAPI_OK);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_HISTORY, hist("save", dir / "x.yaml", "yaml")).status(), FOXAPI_BAD_ARGUMENT);
        CHECK(!fsys::exists(dir / "x.yaml"));
        CHECK(readAll(js).rfind("{\"format\":\"foxsdr-sweep-history\"", 0) == 0);
        CHECK(readAll(xml).find("<sweep-history format=\"foxsdr-sweep-history\"") != std::string::npos);
    }

    std::printf("H2: clear, then load each of the three files: the same lines, bit for bit\n");
    for (const fsys::path& f : {csv, js, xml}) {
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_HISTORY, hist("clear", fsys::path())).status(), FOXAPI_OK);
        CHECK_EQ(historyFact(r)["lines"].get<std::size_t>(), static_cast<std::size_t>(0));
        CHECK_EQ(w.sweep.historySnapshot().sweeps.size(), static_cast<std::size_t>(0));
        const auto o = sendText(r, FOXAPP_OP_SWEEP_HISTORY, hist("load", f));
        CHECK_EQ(o.status(), FOXAPI_OK);
        CHECK_EQ(o.result.applied[0], static_cast<double>(nKept));
        CHECK(w.sweep.historySnapshot() == kept);
        const json st = state(r);
        std::printf("  %s: sweep.state kind %s, open %d, heat %zu x %zu\n", f.extension().string().c_str(), st["kind"].get<std::string>().c_str(), st["open"].get<bool>() ? 1 : 0, st["heat_rows"].get<std::size_t>(),
                    st["heat_cols"].get<std::size_t>());
        CHECK_EQ(st["kind"].get<std::string>(), std::string("history"));
        CHECK(!st["open"].get<bool>());
        CHECK(st["heat_rows"].get<std::size_t>() >= 1);
        CHECK_EQ(st["sweeps"].get<std::uint64_t>(), static_cast<std::uint64_t>(nKept));
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_READ, "{\"max_points\":450}").status(), FOXAPI_OK);
        const json line = json::parse(bigFact(r, "sweep.read"), nullptr, false);
        CHECK(!line.is_discarded());
        if (!line.is_discarded()) {
            std::size_t best = 0;
            for (std::size_t i = 0; i < line["db"].size(); ++i) { if (line["db"][i].is_number() && (line["db"][best].is_null() || line["db"][i].get<double>() > line["db"][best].get<double>())) { best = i; } }
            const double hz = line["start_hz"].get<double>() + (line["stop_hz"].get<double>() - line["start_hz"].get<double>()) * static_cast<double>(best) / static_cast<double>(line["points"].get<std::size_t>() - 1);
            CHECK(std::fabs(hz - 100.0e6) < 0.5e6);   // the stand-in's tone
        }
    }

    std::printf("H3: refusals change nothing\n");
    {
        const cascade::core::SweepHistory before = w.sweep.historySnapshot();
        // a corrupt CSV: a row with the wrong number of fields
        std::string text = readAll(csv);
        text += "1,2,3\n";
        const fsys::path bad = dir / "bad.csv";
        { std::ofstream f(bad, std::ios::binary); f << text; }
        const auto o = sendText(r, FOXAPP_OP_SWEEP_HISTORY, hist("load", bad));
        std::printf("  corrupt file -> %d \"%s\"\n", o.status(), o.result.message);
        CHECK_EQ(o.status(), FOXAPI_BAD_ARGUMENT);
        CHECK(std::string(o.result.message).find("line") != std::string::npos);
        CHECK(w.sweep.historySnapshot() == before);
        CHECK(historyFact(r)["note"].get<std::string>().find("not loaded") == 0);
        // over 4 MiB
        const fsys::path big = dir / "big.csv";
        { std::ofstream f(big, std::ios::binary); f << std::string((std::size_t{4} << 20) + 16, 'x'); }
        const auto ob = sendText(r, FOXAPP_OP_SWEEP_HISTORY, hist("load", big));
        CHECK_EQ(ob.status(), FOXAPI_FAILED);
        CHECK(std::string(ob.result.message).find("too big") != std::string::npos);
        // a folder
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_HISTORY, hist("load", dir)).status(), FOXAPI_FAILED);
        // a file the reader knows nothing of
        const fsys::path junk = dir / "junk.txt";
        { std::ofstream f(junk, std::ios::binary); f << "hello\n"; }
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_HISTORY, hist("load", junk)).status(), FOXAPI_BAD_ARGUMENT);
        CHECK(w.sweep.historySnapshot() == before);
        // a load while a sweep is running
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, kOpen).status(), FOXAPI_OK);
        const auto busy = sendText(r, FOXAPP_OP_SWEEP_HISTORY, hist("load", csv));
        CHECK_EQ(busy.status(), FOXAPI_BUSY);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_CLOSE, "").status(), FOXAPI_OK);
        CHECK(r.until([&] { return !state(r)["open"].get<bool>(); }, 10000));
    }

    std::printf("H4: no fact carries a path\n");
    {
        const std::string dirName = dir.filename().string();
        for (const char* k : {"sweep.history", "sweep.state", "sweep.read"}) { CHECK(bigFact(r, k).find(dirName) == std::string::npos); }
        CHECK(bigFact(r, "sweep.history").find("sweeps.csv") == std::string::npos);
    }
    fsys::remove_all(dir, ec);
    return check::finish("test_engine_api_wireb_history");
}
