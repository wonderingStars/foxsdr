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
//   foxsdr-patch 5
//   view <panX> <panY> <zoom>
//   node <id> <kind> <feed> <x> <y> <w> <h> <freqHz> <mode> <plugin> <device> <rateHz>
//        <squelch 0|1> <squelchDb> <name to end of line>
//   wire <fromId> <fromPort> <toId> <toPort>
//
// THE NAME IS ALWAYS LAST, and that is why adding fields is a format CHANGE
// rather than an addition: the name is read to the end of the line so it may
// contain spaces, so anything appended after it would be swallowed into it.
// Older lines are still read, because a patch saved by a build from this
// morning should not be lost by a build from this afternoon:
//
//   format 1  node <id> <kind> <feed> <x> <y> <name>
//   format 2  ... <x> <y> <freqHz> <mode> <name>             (settings)
//   format 3  ... <x> <y> <w> <h> <freqHz> <mode> <plugin> <name>
//   format 4  ... <plugin> <device> <rateHz> <name>    (a radio's own device, 0.99.17)
//   format 5  ... <rateHz> <squelch> <squelchDb> <name>  (a demodulator's squelch, 0.99.18)
//
// <device> is encoded exactly as <plugin> is. A format-3 radio has no device,
// so it loads as one still to be chosen rather than guessing which radio it
// meant.
//
// A node from format 1 or 2 opens at its kind's default size and runs no
// plugin. <plugin> is the module key percent-encoded to one token, or "-" for
// none, because a key is a file name and a file name may hold a space.
//
// Ids in the file are the file's own. They are remapped on load, because the
// graph hands out its own and reusing a file's would collide with whatever is
// already there.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PATCH_IO_HPP
#define CASCADE_CORE_PATCH_IO_HPP

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <string>

#include "core/patch_graph.hpp"

namespace cascade::core::patch {

inline constexpr const char* kPatchMagic = "foxsdr-patch";
inline constexpr int kPatchFormat = 5;

// The largest size a loaded node may claim. A hand-edited or corrupt file can
// say anything, and a node 10^9 units wide covers the whole canvas and every
// other node under it; clamped, it is merely a large node the user can shrink.
inline constexpr float kMaxLoadedNodeSize = 4000.0f;

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

// A plugin key as one whitespace-free token. Everything outside a small safe
// set is written %XX, so a space, a '%' or anything non-ASCII in a file name
// survives the round trip byte for byte. Empty is "-", and a key that is
// itself exactly "-" is written "%2D", so the sentinel can never be produced
// by a real key - whether or not anyone would ever name a module that.
inline std::string encodePluginKey(const std::string& key) {
    if (key.empty()) { return "-"; }
    if (key == "-") { return "%2D"; }
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (const char ch : key) {
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (safe) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

// The inverse. A malformed escape - '%' not followed by two hex digits - makes
// the whole key unreadable rather than half-decoded: a key that is nearly
// right names a DIFFERENT plugin, or none, and the node should say "no plugin"
// honestly instead of trying to load something the user never chose.
inline bool decodePluginKey(const std::string& token, std::string& key) {
    key.clear();
    if (token == "-") { return true; }
    auto hex = [](char c, int& v) {
        if (c >= '0' && c <= '9') { v = c - '0'; return true; }
        if (c >= 'A' && c <= 'F') { v = c - 'A' + 10; return true; }
        if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; return true; }
        return false;
    };
    for (std::size_t i = 0; i < token.size(); ++i) {
        if (token[i] != '%') {
            key.push_back(token[i]);
            continue;
        }
        int hi = 0, lo = 0;
        // Both digits must exist: "%4" at the end of the token is truncated.
        if (i + 2 >= token.size() || !hex(token[i + 1], hi) || !hex(token[i + 2], lo)) {
            key.clear();
            return false;
        }
        key.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }
    return true;
}

inline std::string serialise(const Graph& g, float panX, float panY, float zoom) {
    // ROUND-TRIP PRECISION, for every number. A stream's default is six
    // significant digits, which wrote 446.00625 MHz as 4.46006e+08 - a
    // channel 250 Hz off after a save and a load, and one that decodes
    // nothing. max_digits10 is the count that guarantees a value reads back
    // as the SAME value: 17 for a double (frequencies), 9 for a float
    // (positions, sizes, the view), which is set per field below.
    constexpr int kF = std::numeric_limits<float>::max_digits10;
    constexpr int kD = std::numeric_limits<double>::max_digits10;
    std::ostringstream o;
    o << std::setprecision(kF);
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
          << static_cast<unsigned>(feed) << ' ' << n.x << ' ' << n.y << ' ' << n.w << ' '
          << n.h << ' ' << std::setprecision(kD) << n.freqHz << std::setprecision(kF) << ' '
          << n.mode << ' ' << encodePluginKey(n.plugin) << ' ' << encodePluginKey(n.device)
          << ' ' << std::setprecision(kD) << n.rateHz << std::setprecision(kF) << ' '
          << (n.squelch ? 1 : 0) << ' ' << n.squelchDb << ' ' << sanitiseName(n.name) << '\n';
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
    int version = 0;
    {
        std::istringstream h(line);
        std::string magic;
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
            if (kind > static_cast<unsigned>(NodeKind::Map) ||
                feed > static_cast<unsigned>(PortType::Track)) {
                ++r.dropped;
                continue;
            }
            // Format 3 puts the size straight after the position.
            float w = 0.0f, h = 0.0f;
            if (version >= 3 && !(s >> w >> h)) {
                ++r.dropped;
                continue;
            }
            // Format 1 has no settings on the line; its nodes take the
            // defaults rather than reading the name as a number.
            double freqHz = 0.0;
            int mode = 0;
            if (version >= 2 && !(s >> freqHz >> mode)) {
                ++r.dropped;
                continue;
            }
            // Format 3's plugin token. An undecodable one keeps the node -
            // its place, its wires and its name are still the user's - but
            // with no plugin, which it then reports as a node that cannot
            // run. Counted, so the load can say it repaired something.
            std::string plugin;
            if (version >= 3) {
                std::string token;
                if (!(s >> token)) {
                    ++r.dropped;
                    continue;
                }
                if (!decodePluginKey(token, plugin)) { ++r.dropped; }
            }
            // Format 4's device and rate, the same repair rule as the plugin:
            // an undecodable device keeps the node with none chosen.
            std::string device;
            double rateHz = 0.0;
            if (version >= 4) {
                std::string token;
                if (!(s >> token >> rateHz)) {
                    ++r.dropped;
                    continue;
                }
                if (!decodePluginKey(token, device)) { ++r.dropped; }
                if (!(rateHz >= 0.0) || rateHz > 1e10) { rateHz = 0.0; }
            }
            // Format 5's squelch. Older documents take the default (on, at
            // -50 dB) - a patch saved before there was a squelch is exactly
            // the patch that was recording noise.
            int squelchOn = 1;
            float squelchDb = Node{}.squelchDb;
            if (version >= 5) {
                if (!(s >> squelchOn >> squelchDb)) {
                    ++r.dropped;
                    continue;
                }
                if (!(squelchDb >= kSquelchMinDb) || squelchDb > kSquelchMaxDb) {
                    squelchDb = Node{}.squelchDb;
                }
            }

            std::string name;
            std::getline(s, name);
            if (!name.empty() && name.front() == ' ') { name.erase(0, 1); }

            const NodeId made = r.graph.addNode(static_cast<NodeKind>(kind), name,
                                                static_cast<PortType>(feed), x, y);
            // A radio past the fifth is refused by the graph itself; the file
            // loses that node and anything wired to it, and says so.
            if (made == kNoNode) {
                ++r.dropped;
                continue;
            }
            if (Node* n = r.graph.mutableNode(made); n != nullptr) {
                n->freqHz = freqHz;
                n->mode = mode;
                n->plugin = plugin;
                n->device = device;
                n->rateHz = rateHz;
                n->squelch = squelchOn != 0;
                n->squelchDb = squelchDb;
                // A size is taken only when it is a real one. Zero, negative
                // or NaN (every comparison false) keeps the default addNode
                // gave the kind; anything absurdly large is clamped.
                if (w > 0.0f && h > 0.0f) {
                    n->w = std::min(w, kMaxLoadedNodeSize);
                    n->h = std::min(h, kMaxLoadedNodeSize);
                }
            }
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
