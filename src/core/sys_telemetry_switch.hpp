// sys_telemetry_switch.hpp - the usage-reporting on/off switch as a model, and the rule it must keep: OFF SENDS NOTHING.
//
// STATE OF THE REAL THING (opened for this lane): the switch already exists end to end - the engine's op (engine.cpp, "usage reporting is already on" / the off branch
// that clears the id) and the Slint host's `fact.telemetry.enabled` (slint/system_logic.cpp). What PRIVACY.md still describes as a hand edit of telemetryEnabled is
// stale text. This header is the standalone model the brief asks for: the three fields that matter (enabled, install id, pending record), the transitions, and the
// property the shipping rules and telemetry.hpp state - consent is the only way an identifier exists, off deletes it, and with reporting off NOTHING is transmitted whatever
// else is lying in the file - written as a function a test can run over every combination, and cross-checked in the test against the real ConfigStore::load.
//
//   setTelemetry(state, on): ON mints an id (injected maker; the real one is core::newInstallId, the system CSPRNG) if reporting was off; ON while already on changes
//   nothing (an id must not change behind its back); OFF clears the id and the pending record. An empty id from the maker (CSPRNG failure) leaves reporting OFF.
//   loadNormalise(state): what loading a file does - an id that is not exactly 32 lower-case hex digits is discarded together with the pending record and switches
//   reporting off (a hand edit is respected, never re-opted-in); an unreadable-sized pending record goes; enabled with no id is left for the first start to mint.
//   mayTransmit(state): enabled AND a well-formed id. transmissions(state, wanted): the records that may actually leave: none unless mayTransmit.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_SYS_TELEMETRY_SWITCH_HPP
#define CASCADE_CORE_SYS_TELEMETRY_SWITCH_HPP

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace cascade::core::sys {

struct TelemetryState {
    bool enabled = false;
    std::string installId;   // "" or exactly 32 lower-case hex digits
    std::string pending;     // the journalled record of the last session, sent at the next start
    bool operator==(const TelemetryState&) const = default;
};

inline bool wellFormedInstallId(const std::string& id) {
    if (id.size() != 32) { return false; }
    for (char c : id) { if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { return false; } }
    return true;
}

inline constexpr std::size_t kTelemetryMaxPendingBytes = 16 * 1024;   // a sanity bound of the model (the real limit is AppConfig::kMaxPendingReportBytes)

struct TelemetryOutcome {
    bool changed = false;
    std::string note;   // English, for a status line: what happened
    TelemetryState next;
};

inline TelemetryOutcome setTelemetry(const TelemetryState& cur, bool on, const std::function<std::string()>& makeId) {
    TelemetryOutcome o;
    o.next = cur;
    if (on) {
        if (cur.enabled && wellFormedInstallId(cur.installId)) { o.note = "Usage reporting is already on."; return o; }
        const std::string id = makeId ? makeId() : std::string();
        if (!wellFormedInstallId(id)) { o.next.enabled = false; o.next.installId.clear(); o.note = "Usage reporting stays off: no anonymous id could be made."; o.changed = (o.next != cur); return o; }
        o.next.enabled = true;
        o.next.installId = id;
        o.changed = true;
        o.note = "Usage reporting is on. A new anonymous id was made.";
        return o;
    }
    o.next.enabled = false;
    o.next.installId.clear();
    o.next.pending.clear();
    o.changed = (o.next != cur);
    o.note = o.changed ? "Usage reporting is off. The anonymous id was deleted and nothing will be sent." : "Usage reporting is already off.";
    return o;
}

inline TelemetryState loadNormalise(TelemetryState s) {
    if (!s.installId.empty() && !wellFormedInstallId(s.installId)) {
        s.installId.clear();
        s.enabled = false;
        s.pending.clear();
    }
    if (s.pending.size() > kTelemetryMaxPendingBytes) { s.pending.clear(); }
    return s;
}

inline bool mayTransmit(const TelemetryState& s) { return s.enabled && wellFormedInstallId(s.installId); }

inline std::vector<std::string> transmissions(const TelemetryState& s, const std::vector<std::string>& wanted) {
    if (!mayTransmit(s)) { return {}; }
    return wanted;
}

}  // namespace cascade::core::sys

#endif  // CASCADE_CORE_SYS_TELEMETRY_SWITCH_HPP
