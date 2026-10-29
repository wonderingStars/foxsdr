// Fuzz target: what a dead run left behind, as the next start reads it (plugin containment P2, docs/engine-stage4.md 6.29; the breaker is P1's,
// core/plugin_marker.hpp, 6.28 rules 18-21).
//
// TWO FILES, both written by a process that may have been killed in the middle of writing them, both read at the very start of the next run - before
// anything else has a chance to go wrong, and by the code whose whole job is to stop a crash loop:
//   plugin-play.marker   8 KiB of memory-mapped slots, written by plain stores from the crash handler and the plugin phases (marker::open);
//   plugin-faults.json   the strike records (loadStore / planStart).
// A damaged marker or store must never cost a person a plugin they did nothing wrong with, and must never stop the application starting.
//
// THE PROPERTIES, for any bytes as either file: the previous run's slots are bounded (64), each has an id and a phase from the closed list; the
// guilty set is a subset of what was in play; a start plan skips each plugin once, tells the person about no plugin it does not skip, and names no
// plugin that was not in the marker or the store; planning twice (this start's records, then the next start's) keeps the records unique by id.
// A store that loads is saved and loads again as the same records; one that does not is an empty store.
//
// The input is cut at its first NUL byte into two halves (so a corpus file can carry both kinds of file), and each half, and the whole, is tried
// as the marker and as the store.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "core/plugin_marker.hpp"
#include "fuzz_common.hpp"

namespace marker = cascade::core::marker;
using cascade::core::StartPlan;
using cascade::core::StrikeRecord;

namespace {

void requireUniqueIds(const std::vector<StrikeRecord>& records) {
    std::set<std::string> ids;
    for (const StrikeRecord& r : records) {
        FUZZ_REQUIRE(!r.id.empty());
        FUZZ_REQUIRE(ids.insert(r.id).second);
    }
}

void judgeMarkerFile(const std::string& path) {
    marker::resetForTest();   // as a fresh process: nothing mapped
    const std::vector<marker::InPlay> prev = marker::open(path);
    marker::resetForTest();
    FUZZ_REQUIRE(prev.size() <= marker::kSlots);
    std::set<std::string> inPlay;
    for (const marker::InPlay& p : prev) {
        FUZZ_REQUIRE(!p.id.empty());
        FUZZ_REQUIRE(static_cast<unsigned>(p.phase) <= static_cast<unsigned>(marker::Phase::Unload));
        inPlay.insert(p.id);
    }
    for (const marker::InPlay& s : cascade::core::suspectsOf(prev)) { FUZZ_REQUIRE(inPlay.count(s.id) == 1); }

    const StartPlan plan = cascade::core::planStart({}, prev, 1000);
    std::set<std::string> skipped;
    for (const std::string& id : plan.skip) {
        FUZZ_REQUIRE(skipped.insert(id).second);   // each plugin once
        FUZZ_REQUIRE(inPlay.count(id) == 1);       // and only a plugin the marker held
    }
    for (const cascade::core::StartNotice& n : plan.notices) { FUZZ_REQUIRE(skipped.count(n.id) == 1); }
    requireUniqueIds(plan.records);
    // the start after it: the records it wrote, against whatever the next marker holds
    const StartPlan next = cascade::core::planStart(plan.records, prev, 2000);
    requireUniqueIds(next.records);
    requireUniqueIds(cascade::core::graduate(next.records, inPlay));
}

void judgeStore(const std::string& path) {
    std::vector<StrikeRecord> loaded;
    std::string error;
    const bool ok = cascade::core::loadStore(path, loaded, error);
    if (!ok) {
        FUZZ_REQUIRE(loaded.empty());   // a damaged or alien file is an empty store
        return;
    }
    requireUniqueIds(loaded);   // (a hand-edited file may name a plugin twice: it is one record once loaded)
    requireUniqueIds(cascade::core::planStart(loaded, {}, 3000).records);
    // saved, it loads again as the same records
    const std::string out = path + ".out";
    std::string saveError;
    FUZZ_REQUIRE(cascade::core::saveStore(out, loaded, saveError));
    std::vector<StrikeRecord> again;
    FUZZ_REQUIRE(cascade::core::loadStore(out, again, saveError));
    FUZZ_REQUIRE(again.size() == loaded.size());
    for (std::size_t i = 0; i < loaded.size(); ++i) {
        FUZZ_REQUIRE(again[i].id == loaded[i].id && again[i].strikes == loaded[i].strikes && again[i].state == loaded[i].state);
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::size_t cut = static_cast<std::size_t>(std::find(data, data + size, std::uint8_t{0}) - data);
    const std::uint8_t* tail = data + (cut < size ? cut + 1 : size);
    const std::size_t tailSize = static_cast<std::size_t>((data + size) - tail);

    judgeMarkerFile(fuzz::writeScratch("plugin-play.marker", data, size));
    judgeMarkerFile(fuzz::writeScratch("plugin-play.marker", tail, tailSize));
    judgeStore(fuzz::writeScratch("plugin-faults.json", data, cut));
    judgeStore(fuzz::writeScratch("plugin-faults.json", tail, tailSize));
    judgeStore(fuzz::writeScratch("plugin-faults.json", data, size));
    return 0;
}
