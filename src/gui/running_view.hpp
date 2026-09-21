// running_view.hpp - which decoders are running, and the keys that stop them.
//
// WHAT THIS IS FOR. A beta tester, through the owner (2026-09-21): "if I am
// playing around I may end up with several decoders running that are not
// applicable. This wouldn't be a big deal except that I find more audio
// underruns and the computers resources get consumed and then it can start to
// hiccup and stutter on other decodes. So I currently go into the plugins tab
// and manually stop the other decoders."
//
// Stopping a decoder was deliberately kept OUT of the rail when the fitted
// modules window was built - the rail's DECODERS section says as much, and
// listed start/stop among the things it would not repeat. That was right when
// stopping was a housekeeping act. It is wrong for the case above, where
// stopping is what you do to get your audio back, on the screen you are
// already looking at. So the keys are on the rail now, and the fitted window
// keeps its own copies: two ways to the same call, not two states.
//
// WHY THERE IS NO AUTOMATIC STOP HERE. The same report offered one: "stop
// decoders that are no longer being used... if I tune off of ACARS
// frequencies, stop the ACARS decoder". A decoder is not finished with the
// moment the VFO moves - tuning away to check a signal, or running one decoder
// on the audio while another reads the raw band, are ordinary things to do -
// and a decoder that switched itself off would be a bug report of its own. The
// decision belongs to the user, so this file gives them a key rather than a
// guess; an opt-in automatic stop can be built on top of it later.
//
// PURE FIRST, DRAWN SECOND: every decision here is a function of plain values,
// so tests/test_running_view.cpp can drive the whole truth table without an
// ImGui context or a plugin.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_RUNNING_VIEW_HPP
#define CASCADE_GUI_RUNNING_VIEW_HPP

#include <cstdio>
#include <string>
#include <vector>

namespace cascade::gui {

// One decoder as the rail sees it. `stopped` is the user's own switch (the
// durable list in AppWindow), `feeding` is the runner actually handing it
// samples this moment - they are different questions, and a row needs both.
struct RunnableDecoder {
    std::string name;
    std::string key;  // module file name; what setPluginStopped takes
    bool stopped = false;
    bool feeding = false;
};

// RUNNING MEANS BEING FED, WITH THE RECEIVER ON. Not "loaded", not "switched
// on and waiting": the tester's complaint is about decoders consuming the
// machine, and one that is not being fed is not consuming anything.
//
// `receiverRunning` IS NOT DECORATION, and the first self-capture of this
// feature is why it is here. The runner keeps its instances across a stopped
// receiver, so isFeeding() still answers true with the radio stopped - and the
// rail then carried "2 decoders are running at once" directly above its own
// DECODERS chip reading "0 FED". The chip gates on the pipeline
// (fedDecoderCount is only consulted when pipeline_.running()), so anything
// that counts running decoders has to gate the same way or the rail says two
// different things about the same modules in one glance.
inline bool decoderIsRunning(const RunnableDecoder& d, bool receiverRunning) {
    return receiverRunning && !d.stopped && d.feeding;
}

inline int runningCount(const std::vector<RunnableDecoder>& all, bool receiverRunning) {
    int n = 0;
    for (const RunnableDecoder& d : all) {
        if (decoderIsRunning(d, receiverRunning)) { ++n; }
    }
    return n;
}

// The keys the runner's own list can be stopped from, in the order given.
inline std::vector<RunnableDecoder> runningDecoders(const std::vector<RunnableDecoder>& all,
                                                    bool receiverRunning) {
    std::vector<RunnableDecoder> out;
    for (const RunnableDecoder& d : all) {
        if (decoderIsRunning(d, receiverRunning)) { out.push_back(d); }
    }
    return out;
}

// STOP, OR START, and never both: the key on a row is the one that changes
// what the user is looking at. A decoder that is switched on but not being fed
// (the receiver is stopped, or it wants a rate the radio is not giving) still
// offers STOP - it is armed, it will start the moment the receiver does, and
// the user deciding they do not want it should not have to start the radio
// first to say so.
inline bool rowOffersStop(const RunnableDecoder& d) { return !d.stopped; }

inline const char* rowKeyLabel(const RunnableDecoder& d) {
    return rowOffersStop(d) ? "STOP" : "START";
}

// STOP THE OTHERS is only worth a key when there ARE others: with one decoder
// running its own STOP key is the whole of it, and a second key beside it that
// does exactly the same thing is noise.
inline bool showStopAll(int running) { return running >= 2; }

inline std::string stopAllLabel(int running) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "STOP ALL %d RUNNING", running);
    return std::string(buf);
}

// What the rail says about the cost, in the tester's own terms. Silent at one
// decoder: one running decoder is the normal case and needs no warning.
inline std::string runningCostNote(int running) {
    if (running < 2) { return std::string(); }
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "%d decoders are running at once. They share one machine, and several "
                  "at a time is what makes the audio stutter.",
                  running);
    return std::string(buf);
}

// STOPPING EVERYTHING MEANS EVERYTHING THAT IS RUNNING, and nothing else: a
// decoder the user already stopped must not be restarted, and one that is not
// being fed is not part of the problem the key exists to solve.
inline std::vector<std::string> stopAllKeys(const std::vector<RunnableDecoder>& all,
                                            bool receiverRunning) {
    std::vector<std::string> keys;
    for (const RunnableDecoder& d : all) {
        if (decoderIsRunning(d, receiverRunning)) { keys.push_back(d.key); }
    }
    return keys;
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_RUNNING_VIEW_HPP
