// gaps1_logic.cpp - see gaps1_logic.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gaps1_logic.hpp"

#include <algorithm>
#include <cstdlib>

#include "core/i18n.hpp"
#include "core/utf8_text.hpp"

namespace foxsdr::slintgaps1 {

using cascade::i18n::tr;

// ---- the diagnostics offer ----------------------------------------------------------------------------------------------------

bool diagOfferDue(bool lastRunUnclean, bool diagnosticsEnabled) { return lastRunUnclean && diagnosticsEnabled; }

std::string diagOfferText() {
    // The classic's sentence word for word (drawDiagnosticsOffer): the same catalogue key, so the translations apply.
    return std::string(tr("The previous session ended without shutting down. If a report was "
                          "written, it is on this machine. On this start we TRY to send its "
                          "text - the version, what failed and where, the thread stacks and "
                          "the recent log lines. It is not sent if it repeats one already "
                          "sent, if this machine is over its send limit, or if foxsdr.com "
                          "cannot be reached. Either way the report stays in the reports "
                          "folder below. No memory dump is sent, ever. Settings > "
                          "Diagnostics lists exactly what goes and turns it off."));
}

// ---- the plugin fault cards ---------------------------------------------------------------------------------------------------

std::vector<FaultNotice> parseFaultNotices(const std::string& text) {
    std::vector<FaultNotice> out;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t nl = text.find('\n', at);
        const std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? text.size() : nl + 1;
        std::vector<std::string> f;
        std::size_t b = 0;
        for (;;) {
            const std::size_t tab = line.find('\t', b);
            f.push_back(line.substr(b, tab == std::string::npos ? std::string::npos : tab - b));
            if (tab == std::string::npos) { break; }
            b = tab + 1;
        }
        if (f.size() < 5 || f[0].empty()) { continue; }
        FaultNotice n;
        n.id = f[0];
        n.version = f[1];
        n.strikes = static_cast<unsigned>(std::strtoul(f[2].c_str(), nullptr, 10));
        n.permanent = f[3] == "1";
        n.phase = f[4];
        out.push_back(std::move(n));
    }
    return out;
}

std::string faultSentence(const FaultNotice& n) {
    return cascade::core::formatText(n.permanent ? tr("%s was in use when FoxSDR stopped unexpectedly twice, so it stays off until you re-enable it.")
                                                 : tr("%s was in use when FoxSDR last stopped unexpectedly, so it was left out of this start."),
                                     n.id.c_str());
}

std::vector<FaultNotice> unseenFaults(const std::vector<FaultNotice>& all, const std::vector<std::string>& seen) {
    std::vector<FaultNotice> out;
    for (const FaultNotice& n : all) {
        if (std::find(seen.begin(), seen.end(), n.id) == seen.end()) { out.push_back(n); }
    }
    return out;
}

// ---- the tester link prompt ---------------------------------------------------------------------------------------------------

LinkPending computeLinkPending(const std::string& token, const std::string& name, bool appTokenHeld, bool portalTokenHeld,
                               const std::optional<std::string>& migrationResolvedAppToken) {
    LinkPending p;
    p.token = token;
    p.name = name;
    const bool replacingApp = appTokenHeld;
    const bool replacingLegacy = !replacingApp && portalTokenHeld;
    p.replacing = replacingApp || replacingLegacy;
    p.replacingLegacyPortalToken = replacingLegacy;
    p.keepsLegacyQueue = replacingLegacy && migrationResolvedAppToken.has_value() && *migrationResolvedAppToken == p.token;
    return p;
}

std::string nameOrPlaceholder(const std::string& name) { return name.empty() ? std::string(tr("(no name set)")) : name; }

std::string linkQuestion(const LinkPending& p, const std::string& currentName) {
    const std::string shown = nameOrPlaceholder(p.name);
    if (p.replacingLegacyPortalToken) {
        // A legacy PORTAL token has no confirmed name of its own to offer as "linked to OLDNAME": it was never resolved by name.
        std::string s = cascade::core::formatText(tr("This FoxSDR has a tester code set. Replace it with a confirmed link to %s?"), shown.c_str());
        s += "\n\n";
        s += p.keepsLegacyQueue ? std::string(tr("Reports already queued under the old code will be kept, linked to the new one."))
                                : std::string(tr("Reports already queued under the old code will be discarded."));
        return s;
    }
    if (p.replacing) { return cascade::core::formatText(tr("This FoxSDR is linked to %s. Replace with %s?"), nameOrPlaceholder(currentName).c_str(), shown.c_str()); }
    return cascade::core::formatText(tr("Link this FoxSDR to tester %s?"), shown.c_str());
}

bool plausibleAppToken(const std::string& token) {
    if (token.size() != 40) { return false; }
    return std::all_of(token.begin(), token.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

void LinkFlow::arrive(const std::string& token) {
    if (!plausibleAppToken(token)) { return; }
    queued_ = token;   // LATEST WINS: only the most recent click is the one the tester meant
}

bool LinkFlow::takeLookup(std::string& token) {
    if (queued_.empty() || !resolving_.empty() || pending_.has_value()) { return false; }
    resolving_ = queued_;
    token = queued_;
    queued_.clear();
    error_.clear();
    return true;
}

void LinkFlow::resolved(ResolveOutcome outcome, const std::string& name, bool appTokenHeld, bool portalTokenHeld) {
    if (resolving_.empty()) { return; }
    if (outcome == ResolveOutcome::Ok) {
        pending_ = computeLinkPending(resolving_, name, appTokenHeld, portalTokenHeld, std::nullopt);
        error_.clear();
    } else if (outcome == ResolveOutcome::Invalid) {
        error_ = tr("that link is not valid");
    } else {
        error_ = tr("could not check that link - try again");
    }
    resolving_.clear();
}

void LinkFlow::dismiss() {
    pending_.reset();
    error_.clear();
}

std::optional<LinkPending> LinkFlow::accept() {
    std::optional<LinkPending> p = pending_;
    pending_.reset();
    return p;
}

}  // namespace foxsdr::slintgaps1
