// patch_io.hpp - a patch written down, and read back.
//
// A patch is a document: the user arranges it, closes the page, and expects it
// to still be there. This turns a Graph into text and back.
//
// THE ONE RULE THAT MATTERS HERE. Loading does NOT reconstruct the wire list
// directly. Every wire is offered to Graph::connect(), exactly as the canvas
// offers one, and a refusal drops that wire and is counted. So a file that has
// been hand-edited, truncated, merged badly or written by an older build
// cannot produce a graph the rules forbid - no cycle, no type mismatch, no
// input with two sample sources. The alternative, trusting the file, means a
// corrupt patch loads into a state the canvas itself could never have made and
// the DSP could not honour; it would be discovered, if at all, as silence.
//
// THE FORMAT IS LINE-BASED rather than JSON because the whole thing is stored
// INSIDE the config file's JSON as one string, and nesting a hand-rolled JSON
// document inside another one is a quoting problem nobody needs. Lines with an
// unknown leading word are skipped rather than refused, so a patch written by
// a later build loses what this build cannot understand and keeps the rest.
//
//   foxsdr-patch 1
//   view <panX> <panY> <zoom>
//   node <id> <kind> <feed> <x> <y> <name to end of line>
//   wire <fromId> <fromPort> <toId> <toPort>
//
// Ids in the file are the file's own. They are remapped on load, because the
// graph hands out its own and reusing a file's would collide with whatever is
// already there.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PATCH_IO_HPP
#define CASCADE_CORE_PATCH_IO_HPP

#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>

#include "core/patch_graph.hpp"

namespace cascade::core::patch {

inline constexpr const char* kPatchMagic = "foxsdr-patch";
inline constexpr int kPatchFormat = 1;

struct LoadResult {
    Graph graph;
    float panX = 0.0f;
    float panY = 0.0f;
    float zoom = 1.0f;
    // Lines that named something real but could not be honoured: a node with
    // an unreadable number, or a wire connect() refused. Counted rather than
    // silently ignored so a caller can say "this patch was repaired" instead
    // of presenting a quietly different patch as the one that was saved.
    int dropped = 0;
    bool ok = false;  // false only when the header is missing or unreadable
};

// A name is written last on its line and read to the end of it, so spaces are
// fine. Newlines are not, and a tab would confuse nothing but reads badly, so
// both become a space on the way out.
inline std::string sanitiseName(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        out.push_back((c == '\n' || c == '\r' || c == '\t') ? ' ' : c);
    }
    return out;
}

inline std::string serialise(const Graph& g, float panX, float panY, float zoom) {
    std::ostringstream o;
    o << kPatchMagic << ' ' << kPatchFormat << '\n';
    o << "view " << panX << ' ' << panY << ' ' << zoom << '\n';
    for (const Node& n : g.nodes()) {
        // The feed is what the node's FIRST port carries - the value
        // portsFor() was given. A node with no ports at all cannot exist
        // through addNode, but writing Iq for one is harmless and keeps the
        // field always present.
        PortType feed = PortType::Iq;
        if (!n.inputs.empty()) {
            feed = n.inputs[0];
        } else if (!n.outputs.empty()) {
            feed = n.outputs[0];
        }
        o << "node " << n.id << ' ' << static_cast<unsigned>(n.kind) << ' '
          << static_cast<unsigned>(feed) << ' ' << n.x << ' ' << n.y << ' '
          << sanitiseName(n.name) << '\n';
    }
    for (const Wire& w : g.wires()) {
        o << "wire " << w.from << ' ' << w.fromPort << ' ' << w.to << ' ' << w.toPort << '\n';
    }
    return o.str();
}

inline LoadResult parse(const std::string& text) {
    LoadResult r;
    std::istringstream in(text);
    std::string line;

    if (!std::getline(in, line)) { return r; }
    {
        std::istringstream h(line);
        std::string magic;
        int version = 0;
        if (!(h >> magic >> version) || magic != kPatchMagic || version < 1) { return r; }
        // A LATER format is read, not refused: every line this build does not
        // understand is skipped, so the worst case is losing what was added
        // after this build was made rather than losing the whole patch.
    }
    r.ok = true;

    // File id -> the id the graph actually handed out.
    std::map<NodeId, NodeId> remap;

    while (std::getline(in, line)) {
        std::istringstream s(line);
        std::string what;
        if (!(s >> what)) { continue; }

        if (what == "view") {
            float px = 0.0f, py = 0.0f, z = 1.0f;
            if (s >> px >> py >> z) {
                r.panX = px;
                r.panY = py;
                // A zoom of zero or a negative one would divide by zero in
                // screenToWorld and put every node at infinity.
                r.zoom = (z > 0.0f) ? z : 1.0f;
            }
            continue;
        }

        if (what == "node") {
            unsigned long fileId = 0, kind = 0, feed = 0;
            float x = 0.0f, y = 0.0f;
            if (!(s >> fileId >> kind >> feed >> x >> y)) {
                ++r.dropped;
                continue;
            }
            if (kind > static_cast<unsigned>(NodeKind::Sink) ||
                feed > static_cast<unsigned>(PortType::Control)) {
                ++r.dropped;
                continue;
            }
            std::string name;
            std::getline(s, name);
            if (!name.empty() && name.front() == ' ') { name.erase(0, 1); }

            const NodeId made = r.graph.addNode(static_cast<NodeKind>(kind), name,
                                                static_cast<PortType>(feed), x, y);
            remap[static_cast<NodeId>(fileId)] = made;
            continue;
        }

        if (what == "wire") {
            unsigned long a = 0, ap = 0, b = 0, bp = 0;
            if (!(s >> a >> ap >> b >> bp)) {
                ++r.dropped;
                continue;
            }
            const auto ia = remap.find(static_cast<NodeId>(a));
            const auto ib = remap.find(static_cast<NodeId>(b));
            if (ia == remap.end() || ib == remap.end()) {
                ++r.dropped;
                continue;
            }
            // THROUGH connect(), never straight into the wire list.
            if (r.graph.connect(ia->second, static_cast<PortIndex>(ap), ib->second,
                                static_cast<PortIndex>(bp)) != Connect::Ok) {
                ++r.dropped;
            }
            continue;
        }

        // Anything else belongs to a format this build does not know. Skipped,
        // not counted: it is not a dropped item, it is a line from the future.
    }
    return r;
}

}  // namespace cascade::core::patch

#endif  // CASCADE_CORE_PATCH_IO_HPP
