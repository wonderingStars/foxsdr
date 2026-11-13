// test_wire_b_saved_kinds.cpp - THE WIRE-B SOURCE KINDS AS SAVED RADIO KINDS (core/wire_b_source_kinds.hpp; wire-b second pass, item 33), pure: no engine run, no radio.
//
//   K1  the core's list of keys and the engine's source table (engine/wire_b_sources.cpp) are the SAME set: a key in one and not the other fails here
//   K2  every key is a native source kind (gui::isNativeSourceKind: it saves, restores and keeps its name when an open fails); a word that is not a key is not
//   K3  the configuration file keeps every key as the kind of radio 1 and of an entry of `radios` (serialize -> parse), with its arguments exactly
//   K4  seven radios, seven different kinds, in one file: all come back in their numbers
//   K5  a hand-edited file: an unknown kind or an over-long argument list becomes the generator (as before this change); a table key with NO arguments is the generator too
//   K6  the table's argument normaliser invents no secret: the arguments are the key, then what was typed (it may add a plain default such as udpport=0), never a password or token
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <set>
#include <string>
#include <vector>

#include "core/config.hpp"
#include "core/tune_control.hpp"
#include "core/wire_b_source_kinds.hpp"
#include "engine/wire_b_sources.hpp"
#include "test_check.hpp"

#ifndef CHECK_EQ
#define CHECK_EQ(a, b) CHECK((a) == (b))
#endif

using cascade::core::AppConfig;
using cascade::core::ConfigStore;
using cascade::core::SavedRadio;

namespace {
std::string argsFor(const std::string& key) { return key + "=127.0.0.1:50000,rate=250000"; }
}  // namespace

int main() {
    std::printf("K1: the core's keys and the engine's table are one set\n");
    {
        std::size_t n = 0;
        const cascade::engine::wireb::SourceKind* rows = cascade::engine::wireb::sourceKinds(n);
        std::set<std::string> table, core;
        for (std::size_t i = 0; i < n; ++i) { table.insert(rows[i].key); }
        for (const char* k : cascade::core::kWireBSourceKinds) { core.insert(k); }
        std::printf("  engine table %zu keys, core list %zu keys\n", table.size(), core.size());
        CHECK(table == core);
        CHECK_EQ(core.size(), cascade::core::kWireBSourceKindCount);
        CHECK_EQ(table.size(), n);   // no duplicate key in the table
    }

    std::printf("K2: every key is a native kind\n");
    for (const char* k : cascade::core::kWireBSourceKinds) {
        CHECK(cascade::gui::isNativeSourceKind(k));
        CHECK(cascade::core::isWireBSourceKind(k));
    }
    CHECK(!cascade::gui::isNativeSourceKind("nonesuch"));
    CHECK(!cascade::gui::isNativeSourceKind(""));
    CHECK(!cascade::gui::isNativeSourceKind("rfspac"));
    CHECK(!cascade::gui::isNativeSourceKind("RFSPACE"));   // (the key is the exact driver key)
    CHECK(cascade::gui::isNativeSourceKind("rtlsdr") && cascade::gui::isNativeSourceKind("kiwisdr") && cascade::gui::isNativeSourceKind("rtltcp"));   // (the old ones still are)
    CHECK(!cascade::gui::isNativeSourceKind("siggen") && !cascade::gui::isNativeSourceKind("file") && !cascade::gui::isNativeSourceKind("soapy"));

    std::printf("K3: the file keeps each key, for radio 1 and for a saved radio\n");
    for (const char* k : cascade::core::kWireBSourceKinds) {
        AppConfig cfg;
        cfg.sourceKind = k;
        cfg.nativeArgs = argsFor(k);
        SavedRadio r;
        r.number = 2;
        r.sourceKind = k;
        r.args = argsFor(k);
        r.centerHz = 7.1e6;
        r.sampleRateHz = 250000.0;
        cfg.radios.push_back(r);
        AppConfig back;
        std::string error;
        CHECK(ConfigStore::parse(ConfigStore::serialize(cfg), "test", back, error));
        CHECK_EQ(back.sourceKind, std::string(k));
        CHECK_EQ(back.nativeArgs, argsFor(k));
        CHECK_EQ(back.radios.size(), static_cast<std::size_t>(1));
        if (back.radios.size() == 1) {
            CHECK_EQ(back.radios[0].sourceKind, std::string(k));
            CHECK_EQ(back.radios[0].args, argsFor(k));
            CHECK_EQ(back.radios[0].number, 2);
            CHECK(back.radios[0].centerHz == 7.1e6 && back.radios[0].sampleRateHz == 250000.0);
        }
    }

    std::printf("K4: seven radios, seven kinds\n");
    {
        AppConfig cfg;
        const std::vector<std::string> kinds = {"rfspace", "hpsdr1", "spyserver", "soapyremote", "icr8600", "g39ddc", "sdriq"};
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            SavedRadio r;
            r.number = static_cast<int>(i) + 2;
            r.sourceKind = kinds[i];
            r.args = argsFor(kinds[i]);
            cfg.radios.push_back(r);
        }
        AppConfig back;
        std::string error;
        CHECK(ConfigStore::parse(ConfigStore::serialize(cfg), "test", back, error));
        CHECK_EQ(back.radios.size(), kinds.size());
        for (std::size_t i = 0; i < back.radios.size() && i < kinds.size(); ++i) {
            CHECK_EQ(back.radios[i].number, static_cast<int>(i) + 2);
            CHECK_EQ(back.radios[i].sourceKind, kinds[i]);
            CHECK_EQ(back.radios[i].args, argsFor(kinds[i]));
        }
    }

    std::printf("K5: a hand-edited file\n");
    {
        AppConfig cfg;
        SavedRadio a, b, c, d;
        a.number = 2; a.sourceKind = "rfspacee"; a.args = "rfspacee=1.2.3.4:5";   // unknown kind
        b.number = 3; b.sourceKind = "hpsdr1"; b.args = "hpsdr1=" + std::string(600, 'x');   // over-long arguments
        c.number = 4; c.sourceKind = "spyserver"; c.args = "";                                // a kind with no arguments names nothing
        d.number = 5; d.sourceKind = "aaronia"; d.args = "aaronia=127.0.0.1:54664";            // a good one beside them
        cfg.radios = {a, b, c, d};
        cfg.sourceKind = "nonesuch";
        AppConfig back;
        std::string error;
        CHECK(ConfigStore::parse(ConfigStore::serialize(cfg), "test", back, error));
        CHECK_EQ(back.radios.size(), static_cast<std::size_t>(4));
        if (back.radios.size() == 4) {
            CHECK_EQ(back.radios[0].sourceKind, std::string("siggen"));
            CHECK(back.radios[0].args.empty());
            CHECK_EQ(back.radios[1].sourceKind, std::string("siggen"));
            CHECK(back.radios[1].args.empty());
            CHECK(back.radios[2].args.empty());   // (a kind with no arguments stays as the file had it, as every other kind does: the engine reads it as the generator, and its own save writes the generator)
            CHECK_EQ(back.radios[3].sourceKind, std::string("aaronia"));
            CHECK_EQ(back.radios[3].args, std::string("aaronia=127.0.0.1:54664"));
        }
        CHECK(back.sourceKind != "nonesuch");
    }

    std::printf("K6: the normaliser invents no secret\n");
    {
        for (const char* k : cascade::core::kWireBSourceKinds) {
            const cascade::engine::wireb::SourceKind* row = cascade::engine::wireb::findSourceByKey(k);
            CHECK(row != nullptr);
            if (row == nullptr) { continue; }
            std::string args, why;
            const std::string typed = row->needsAddress ? std::string("127.0.0.1:50000,rate=250000") : std::string("rate=250000");
            CHECK(cascade::engine::wireb::normaliseSourceArgs(*row, typed, args, why));
            std::printf("  %s -> %s\n", k, args.c_str());
            CHECK(!row->needsAddress || args.rfind(std::string(k) + "=", 0) == 0);   // an address row: behind the key
            CHECK(args.find(typed) != std::string::npos);                              // what the person typed is in it, as typed
            CHECK(args.find("password") == std::string::npos && args.find("token") == std::string::npos && args.find("secret") == std::string::npos);
        }
        // a row with no address, opened with nothing typed, is saved under its source's documented default: never an empty list (an empty list is the generator to the file)
        for (const char* k : cascade::core::kWireBSourceKinds) {
            const cascade::engine::wireb::SourceKind* row2 = cascade::engine::wireb::findSourceByKey(k);
            if (row2 == nullptr || row2->needsAddress) { continue; }
            std::string a2, w2;
            CHECK(cascade::engine::wireb::normaliseSourceArgs(*row2, "", a2, w2));
            std::printf("  %s with nothing typed -> %s\n", k, a2.c_str());
            CHECK(!a2.empty());
            CHECK(row2->defaultArgs != nullptr);
            std::string a3;
            CHECK(cascade::engine::wireb::normaliseSourceArgs(*row2, "serial=ABC", a3, w2));
            CHECK(a3 == "serial=ABC");   // what was typed wins
        }
        // an address the table needs is never read as this machine
        const cascade::engine::wireb::SourceKind* row = cascade::engine::wireb::findSourceByKey("rfspace");
        std::string args, why;
        CHECK(!cascade::engine::wireb::normaliseSourceArgs(*row, "", args, why));
    }
    return testSummary("test_wire_b_saved_kinds");
}
