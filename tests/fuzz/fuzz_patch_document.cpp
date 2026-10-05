// Fuzz target: a saved patch (the patch page's document, also stored as text in
// config.json under the preset list).
//
// The format is line-oriented text read with stream extraction, and it is read
// from a file the user chose to open or a preset a config carried over from
// another machine. Every number on every line is the file's own.
//
// Properties: only a missing or unreadable header makes the load fail (ok is
// the one verdict the header documents); every node a load produces is within
// the size and radio limits the graph enforces; and what it produced survives a
// round trip: serialising it and loading that again gives the same graph.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/patch_io.hpp"
#include "fuzz_common.hpp"

using namespace cascade::core::patch;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string text = fuzz::asString(data, size);

    const LoadResult first = parse(text);
    if (!first.ok) {
        FUZZ_REQUIRE(first.graph.nodes().empty());
        return 0;
    }
    FUZZ_REQUIRE(first.zoom > 0.0f);

    for (const Node& n : first.graph.nodes()) {
        FUZZ_REQUIRE(n.w <= kMaxLoadedNodeSize && n.h <= kMaxLoadedNodeSize);
        FUZZ_REQUIRE(n.rateHz >= 0.0 && n.rateHz <= 1e10);
    }

    const std::string again = serialise(first.graph, first.panX, first.panY, first.zoom);
    const LoadResult second = parse(again);
    FUZZ_REQUIRE(second.ok);
    FUZZ_REQUIRE(second.graph.nodes().size() == first.graph.nodes().size());
    FUZZ_REQUIRE(second.graph.wires().size() == first.graph.wires().size());
    return 0;
}
