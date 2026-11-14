// Window gaps lane 1's pure rules (src/slint/gaps1_logic.hpp; docs/multi-radio-records/window-gaps-1.md): when the "did not close normally" offer is due, the plugin fault
// notices' parse and sentences, and the tester link prompt's decisions and flow.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <memory>
#include <string>
#include <vector>

#include "gaps1_logic.hpp"
#include "sysmenu_win.hpp"
#include "test_check.hpp"

using namespace foxsdr::slintgaps1;

namespace {
const std::string kTok(40, 'a');
const std::string kTok2 = std::string(39, 'b') + "c";
}  // namespace

int main() {
    // ---------------------------------------------------------------- the diagnostics offer
    CHECK(diagOfferDue(true, true));
    CHECK(!diagOfferDue(true, false));    // diagnostics off: nothing was written, nothing to offer
    CHECK(!diagOfferDue(false, true));    // the last run ended normally
    CHECK(!diagOfferDue(false, false));
    const std::string offer = diagOfferText();
    CHECK(offer.find("The previous session ended without shutting down.") == 0);
    CHECK(offer.find("we TRY to send") != std::string::npos);                       // the attempt, never the outcome
    CHECK(offer.find("No memory dump is sent, ever.") != std::string::npos);

    // ---------------------------------------------------------------- the plugin fault cards
    const std::string text = "aprs-decoder\t1.0.2\t1\t0\tprocess\nadsb\t2.0\t2\t1\tprocess\nshort\tonly\tthree\n\t1\t1\t0\tx\n";
    const std::vector<FaultNotice> f = parseFaultNotices(text);
    CHECK(f.size() == 2);                                                           // the short line and the empty id are skipped
    CHECK(f[0].id == "aprs-decoder" && f[0].version == "1.0.2" && f[0].strikes == 1 && !f[0].permanent && f[0].phase == "process");
    CHECK(f[1].id == "adsb" && f[1].strikes == 2 && f[1].permanent);
    CHECK(parseFaultNotices("").empty());
    CHECK(parseFaultNotices("a\tb\tc\n").empty());
    CHECK(parseFaultNotices("x\t1\t1\t0\tp").size() == 1);                           // no trailing newline
    CHECK(faultSentence(f[0]) == "aprs-decoder was in use when FoxSDR last stopped unexpectedly, so it was left out of this start.");
    CHECK(faultSentence(f[1]) == "adsb was in use when FoxSDR stopped unexpectedly twice, so it stays off until you re-enable it.");
    CHECK(unseenFaults(f, {}).size() == 2);
    CHECK(unseenFaults(f, {"adsb"}).size() == 1 && unseenFaults(f, {"adsb"})[0].id == "aprs-decoder");
    CHECK(unseenFaults(f, {"adsb", "aprs-decoder"}).empty());
    CHECK(unseenFaults({}, {"adsb"}).empty());

    // ---------------------------------------------------------------- the link prompt's question
    LinkPending fresh = computeLinkPending(kTok, "Ada", false, false, std::nullopt);
    CHECK(!fresh.replacing && !fresh.replacingLegacyPortalToken && !fresh.keepsLegacyQueue && fresh.name == "Ada");
    CHECK(linkQuestion(fresh, "") == "Link this FoxSDR to tester Ada?");           // "tester", never "beta tester"
    CHECK(linkQuestion(fresh, "").find("beta") == std::string::npos);
    CHECK(linkQuestion(computeLinkPending(kTok, "", false, false, std::nullopt), "") == "Link this FoxSDR to tester (no name set)?");   // a blank name reads as a missing word otherwise
    const LinkPending rep = computeLinkPending(kTok, "Bo", true, false, std::nullopt);
    CHECK(rep.replacing && !rep.replacingLegacyPortalToken);
    CHECK(linkQuestion(rep, "Ada") == "This FoxSDR is linked to Ada. Replace with Bo?");
    CHECK(linkQuestion(rep, "") == "This FoxSDR is linked to (no name set). Replace with Bo?");
    const LinkPending legacy = computeLinkPending(kTok, "Bo", false, true, std::nullopt);
    CHECK(legacy.replacing && legacy.replacingLegacyPortalToken && !legacy.keepsLegacyQueue);
    CHECK(linkQuestion(legacy, "").find("This FoxSDR has a tester code set. Replace it with a confirmed link to Bo?") == 0);
    CHECK(linkQuestion(legacy, "").find("Reports already queued under the old code will be discarded.") != std::string::npos);
    const LinkPending kept = computeLinkPending(kTok, "Bo", false, true, std::optional<std::string>(kTok));
    CHECK(kept.keepsLegacyQueue && linkQuestion(kept, "").find("will be kept, linked to the new one.") != std::string::npos);
    CHECK(!computeLinkPending(kTok, "Bo", false, true, std::optional<std::string>(kTok2)).keepsLegacyQueue);   // another tester's migration: not kept
    CHECK(computeLinkPending(kTok, "Bo", true, true, std::nullopt).replacing && !computeLinkPending(kTok, "Bo", true, true, std::nullopt).replacingLegacyPortalToken);   // an app token wins

    // ---------------------------------------------------------------- the flow
    CHECK(plausibleAppToken(kTok) && !plausibleAppToken("") && !plausibleAppToken(std::string(40, 'A')) && !plausibleAppToken(std::string(39, 'a')) && !plausibleAppToken(std::string(41, 'a')));
    LinkFlow flow;
    std::string t;
    CHECK(!flow.open() && !flow.takeLookup(t));                                     // nothing arrived: nothing to look up
    flow.arrive("not a token");
    CHECK(!flow.takeLookup(t));                                                     // a malformed token never starts a lookup
    flow.arrive(kTok);
    CHECK(flow.takeLookup(t) && t == kTok && flow.lookingUp() && !flow.open());     // asking the site shows nothing yet
    CHECK(!flow.takeLookup(t));                                                     // one lookup at a time
    flow.arrive(kTok2);                                                             // a second click while the first is out: held, latest wins
    CHECK(!flow.takeLookup(t));
    flow.resolved(ResolveOutcome::Ok, "Ada", false, false);
    CHECK(flow.hasPending() && flow.pending().name == "Ada" && flow.pending().token == kTok && flow.open() && !flow.lookingUp());
    CHECK(!flow.takeLookup(t));                                                     // the question is up: the held token waits for the answer to it
    const std::optional<LinkPending> got = flow.accept();
    CHECK(got.has_value() && got->token == kTok && got->name == "Ada" && !flow.open());
    CHECK(!flow.accept().has_value());                                              // nothing is accepted twice
    CHECK(flow.takeLookup(t) && t == kTok2);                                        // the held one is looked up now
    flow.resolved(ResolveOutcome::Invalid, "", false, false);
    CHECK(!flow.hasPending() && flow.error() == "that link is not valid" && flow.open());
    flow.dismiss();
    CHECK(!flow.open() && flow.error().empty());
    flow.arrive(kTok);
    CHECK(flow.takeLookup(t));
    flow.resolved(ResolveOutcome::NetworkError, "", false, false);
    CHECK(flow.error() == "could not check that link - try again" && !flow.hasPending());
    flow.resolved(ResolveOutcome::Ok, "Ghost", false, false);                       // an answer nobody asked for is ignored
    CHECK(!flow.hasPending());
    flow.dismiss();
    flow.arrive(kTok);
    CHECK(flow.takeLookup(t));
    flow.resolved(ResolveOutcome::Ok, "Cy", true, false);                            // a confirmed link is held: this is a replacement
    CHECK(flow.pending().replacing);
    flow.dismiss();                                                                  // Not now: nothing was stored, nothing is left
    CHECK(!flow.open() && !flow.accept().has_value());

    // ---------------------------------------------------------------- the title rail's system menu (the hand-over, with a stand-in: no test raises the real menu)
    {
        struct Stand final : foxsdr::slintsysmenu::SystemMenu {
            void* handle = nullptr;
            bool has = true;
            int shown = 0;
            void* window() override { return handle; }
            int itemCount(void*) override { return has ? 7 : -1; }
            bool show(void* w) override { if (w == nullptr || !has) { return false; } ++shown; return true; }
        };
        Stand s;
        std::string note;
        CHECK(!foxsdr::slintsysmenu::requestSystemMenu(s, note) && s.shown == 0 && note.find("handle is not there yet") != std::string::npos);   // no window yet: says so, shows nothing
        s.handle = reinterpret_cast<void*>(0x1234);
        CHECK(foxsdr::slintsysmenu::requestSystemMenu(s, note) && s.shown == 1 && note.empty());
        s.has = false;
        CHECK(!foxsdr::slintsysmenu::requestSystemMenu(s, note) && s.shown == 1 && note.find("has none to show") != std::string::npos);
        // the real one, without showing anything: with no window of this process it answers nothing and is never asked to show
        std::unique_ptr<foxsdr::slintsysmenu::SystemMenu> real = foxsdr::slintsysmenu::makeWindowsSystemMenu();
        CHECK(real != nullptr && real->itemCount(nullptr) == -1 && !real->show(nullptr));
    }

    return testSummary("test_slint_gaps1_host");
}
