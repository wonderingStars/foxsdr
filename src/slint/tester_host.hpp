// tester_host.hpp - LANE X, row WG2-1492: the tester usage RECORDER of the Slint host, and with it the Preview box ("Show what is sent") of SYSTEM > Beta tester.
//
// The classic window keeps a core::TesterUsageRecorder and feeds it from the engine's facts (gui/app_window_tester.cpp testerNoteFacts, testerFedPlugins, testerUsageAccrue). This is the same
// duty for the Slint host, split in two so it can be tested without an engine: `Facts` is what one look at the engine came to, TesterDuty turns facts into marks and a report.
//
// WHAT IS NOT HERE: the sender, the retry queue and the journal. They are the radio-reports lane's (docs/multi-radio-records/lane-x.md, OPEN ROWS); this file never opens a connection and
// writes nothing to disk. Nothing is recorded while no tester code is set (armed = false), exactly as in the classic window.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "core/tester_usage.hpp"

struct FoxEngineApi;
struct FoxSession;

namespace foxsdr::slinttester {

struct Facts {
    std::uint32_t flags = 0;                 // the receiver state's flags (FOXAPI_RX_*)
    std::string sourceKind;                  // the kind of the source in use: "siggen", "soundcard", "file", or a radio driver kind ("rtlsdr", "soapy", ...); "" = unknown
    bool webServed = false, catAccepted = false;   // the listeners' counters moved since the last look
    struct Fed { std::string id, version; };
    std::vector<Fed> fed;                    // the plugins being fed samples now, by catalogue id (kSideloadedPluginId and no version for one with no install record)
    bool patchDecoder = false;               // a decoder node of the running patch
};

class TesterDuty {
public:
    // The token the session is recorded under ("" = none: nothing is recorded). A change from none to a code arms the recorder and starts a session; a change back to none clears it.
    void setToken(const std::string& activeToken, std::time_t wallNow, double monoNow);
    bool armed() const { return !token_.empty(); }
    // One look: marks the features and the radio kind the facts show and credits the seconds since the last look to the plugins fed now (a gap longer than `kMaxGapS` is credited as that).
    void note(const Facts& f, double monoNow);
    // The report as it would be built at this moment (finalised to the site's bounds); empty token when not armed.
    cascade::core::TesterUsageReport report(double monoNow, std::time_t wallNow) const;
    // The masked JSON of that report: what "Show what is sent" displays. "" when not armed.
    std::string previewJson(double monoNow, std::time_t wallNow) const;
    static constexpr double kMaxGapS = 120.0;

private:
    std::string token_;
    cascade::core::TesterUsageRecorder rec_;
    std::time_t startWall_ = 0;
    double startMono_ = 0.0, lastNote_ = 0.0;
};

// One look at the engine, through the table: the flags, the source in use (the DEVICES list's active row), the plugins fed now (DECODERS joined to PLUGINS, then each file's install
// record), and whether the web and CAT counters moved. `webSeen` and `catSeen` carry the counters between looks.
Facts collectFacts(const FoxEngineApi* api, FoxSession* session, std::uint32_t stateFlags, std::uint64_t& webSeen, std::uint64_t& catSeen);

}  // namespace foxsdr::slinttester
