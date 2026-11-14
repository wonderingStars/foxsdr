// gaps1_logic.hpp - WINDOW GAPS, LANE 1 (docs/multi-radio-records/window-gaps-1.md): the pure rules of the start-up questions the classic window asks and the Slint window did not -
// "FoxSDR did not close normally last time" (its offer), "Link this FoxSDR?" (the tester link prompt) and the plugin fault cards ("Started without a plugin"). No window, no network,
// no disk: the publisher (gaps1_publish) does the touching, and a test drives these directly.
//
// The classic window's code is the specification: gui/app_window.cpp drawDiagnosticsOffer (~21315), gui/app_window_tester.cpp (testerLinkPoll, drawTesterLinkPrompt,
// computeLinkPending) and gui/app_window_updates.cpp drawPluginFaultNotices (~270). The words are theirs.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace foxsdr::slintgaps1 {

// ---- the diagnostics offer ----------------------------------------------------------------------------------------------------
// Due when the last run did not end normally AND diagnostics were on (app_window.cpp 22228). The sentence states the ATTEMPT, never the outcome (the long comment in drawDiagnosticsOffer).
bool diagOfferDue(bool lastRunUnclean, bool diagnosticsEnabled);
std::string diagOfferText();

// ---- the plugin fault cards ---------------------------------------------------------------------------------------------------
// A plugin LEFT OUT OF THIS START: the previous run ended with a plugin in play and no orderly stop. `fact.plugins.fault.notices` is one line per notice, tab separated:
// id, version, strikes, permanent (1/0), phase. A line with fewer than five fields or an empty id is skipped.
struct FaultNotice {
    std::string id;
    std::string version;
    unsigned strikes = 0;
    bool permanent = false;
    std::string phase;
    bool operator==(const FaultNotice&) const = default;
};
std::vector<FaultNotice> parseFaultNotices(const std::string& text);
// The card's sentence: strike one is for this start only, strike two stays off until it is re-enabled.
std::string faultSentence(const FaultNotice& n);
// The notices the person has not put away this session ("Seen" or "RE-ENABLE" puts one away).
std::vector<FaultNotice> unseenFaults(const std::vector<FaultNotice>& all, const std::vector<std::string>& seen);

// ---- the tester link prompt ---------------------------------------------------------------------------------------------------
// What the prompt asks: a fresh link, a replacement of a confirmed link, or a replacement of a legacy portal code.
struct LinkPending {
    std::string token;                    // the 40-hex app token the link carried (a credential: never logged, never shown)
    std::string name;                     // the tester's own name as the site gave it (sanitised by the sender)
    bool replacing = false;
    bool replacingLegacyPortalToken = false;
    bool keepsLegacyQueue = false;
    bool operator==(const LinkPending&) const = default;
};
LinkPending computeLinkPending(const std::string& token, const std::string& name, bool appTokenHeld, bool portalTokenHeld,
                               const std::optional<std::string>& migrationResolvedAppToken);
// "(no name set)" for a name the site left empty: a blank formatted into "Link this FoxSDR to tester %s?" would read as a sentence missing a word.
std::string nameOrPlaceholder(const std::string& name);
// The prompt's words, one string, lines separated by a blank line. `currentName` is the name of the link now held (the replace case). The user-visible word is "tester", never "beta tester".
std::string linkQuestion(const LinkPending& p, const std::string& currentName);

enum class ResolveOutcome { Ok, Invalid, NetworkError };

// THE FLOW (testerLinkPoll's confirm-by-name half): a token arrives from the one-shot link-request file; it is put to the site, which names its owner; only then is a question asked.
// Latest wins: a second arrival before the first is acted on replaces it. Nothing is stored until the person presses Link.
class LinkFlow {
public:
    // A token claimed from the link-request file.
    void arrive(const std::string& token);
    // True once, with the token to put to the site, when a lookup should start (nothing is pending or being looked up).
    bool takeLookup(std::string& token);
    // The site's answer to the lookup started by takeLookup.
    void resolved(ResolveOutcome outcome, const std::string& name, bool appTokenHeld, bool portalTokenHeld);
    bool lookingUp() const { return !resolving_.empty(); }
    bool hasPending() const { return pending_.has_value(); }
    const LinkPending& pending() const { return *pending_; }
    const std::string& error() const { return error_; }
    bool open() const { return pending_.has_value() || !error_.empty(); }
    // Not now, or the close box: whatever is shown is put away.
    void dismiss();
    // Link: the pending link is taken (the caller stores it); the prompt is put away.
    std::optional<LinkPending> accept();

private:
    std::string queued_;
    std::string resolving_;
    std::optional<LinkPending> pending_;
    std::string error_;
};

// A link-request token is exactly 40 lowercase hex (core::validAppToken); the one-shot file's claim already checks it, this is the defensive copy for the flow's own entry.
bool plausibleAppToken(const std::string& token);

}  // namespace foxsdr::slintgaps1
