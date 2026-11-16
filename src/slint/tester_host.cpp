// tester_host.cpp - see tester_host.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "tester_host.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core/telemetry.hpp"
#include "core/version.hpp"
#include "foxsdr_api.h"

namespace foxsdr::slinttester {

void TesterDuty::setToken(const std::string& activeToken, std::time_t wallNow, double monoNow) {
    if (activeToken == token_) { return; }
    const bool wasArmed = armed();
    token_ = activeToken;
    rec_.setArmed(armed());
    if (!armed()) {
        rec_.reset();   // a removed code leaves nothing behind
        return;
    }
    if (!wasArmed) {   // a code pasted mid-session starts the session record from now
        rec_.reset();
        startWall_ = wallNow;
        startMono_ = monoNow;
        lastNote_ = monoNow;
    }
}

void TesterDuty::note(const Facts& f, double monoNow) {
    if (!armed()) { return; }
    const std::uint32_t flags = f.flags;
    if ((flags & FOXAPI_RX_RECORDING_IQ) != 0u) { rec_.noteFeature("rec-iq"); }
    if ((flags & FOXAPI_RX_RECORDING_AUDIO) != 0u) { rec_.noteFeature("rec-audio"); }
    if ((flags & FOXAPI_RX_SCANNER_ACTIVE) != 0u) { rec_.noteFeature("scanner"); }
    if ((flags & FOXAPI_RX_TX_KEYED) != 0u) { rec_.noteFeature("transmit"); }
    if (f.webServed) { rec_.noteFeature("browser"); }
    if (f.catAccepted) { rec_.noteFeature("cat"); }
    if (f.sourceKind == "siggen") {
        if ((flags & FOXAPI_RX_RUNNING) != 0u) { rec_.noteFeature("siggen"); }
    } else if (!f.sourceKind.empty() && f.sourceKind != "file" && (flags & FOXAPI_RX_DEVICE_OPEN) != 0u) {
        rec_.noteRadio(f.sourceKind);
        if (f.sourceKind != "soapy" && f.sourceKind != "soundcard") { rec_.noteFeature("native-drivers"); }
    }
    const double gap = std::clamp(monoNow - lastNote_, 0.0, kMaxGapS);
    lastNote_ = monoNow;
    if (gap > 0.0) { for (const Facts::Fed& p : f.fed) { rec_.accruePlugin(p.id, p.version, gap); } }
    if (f.patchDecoder) { rec_.noteFeature("plugins"); }
}

cascade::core::TesterUsageReport TesterDuty::report(double monoNow, std::time_t wallNow) const {
    cascade::core::TesterUsageReport r;
    if (!armed()) { return r; }
    r.token = token_;
    r.version = cascade::versionString();
    r.platform = cascade::core::testerUsagePlatform();
    r.arch = cascade::core::archDescription();
    r.session.start = cascade::core::rfc3339Utc(startWall_);
    const double secs = monoNow > startMono_ ? monoNow - startMono_ : 0.0;
    const long long mins = std::llround(secs / 60.0);
    r.session.minutes = static_cast<std::uint64_t>(std::clamp<long long>(mins, 0, 1440));
    r.session.features = rec_.features();
    r.session.plugins = rec_.plugins();
    r.session.radios = rec_.radios();
    return cascade::core::finalizeTesterUsageReport(std::move(r), wallNow);
}

std::string TesterDuty::previewJson(double monoNow, std::time_t wallNow) const {
    if (!armed()) { return std::string(); }
    return cascade::core::maskedPreviewJson(report(monoNow, wallNow));
}

namespace {
std::string factText(const FoxEngineApi* api, FoxSession* session, const std::string& key) {
    char buf[512];
    const int32_t n = api->get_setting(session, key.c_str(), buf, sizeof(buf));
    if (n < 0) { return std::string(); }
    buf[sizeof(buf) - 1] = '\0';
    return std::string(buf);
}
}  // namespace

Facts collectFacts(const FoxEngineApi* api, FoxSession* session, std::uint32_t stateFlags, std::uint64_t& webSeen, std::uint64_t& catSeen) {
    Facts f;
    f.flags = stateFlags;
    if (api == nullptr || session == nullptr) { return f; }
    const auto counter = [&](const char* key, std::uint64_t& seen) {
        const std::string t = factText(api, session, key);
        if (t.empty()) { return false; }
        const std::uint64_t now = std::strtoull(t.c_str(), nullptr, 10);
        const bool moved = now > seen;
        seen = now;
        return moved;
    };
    f.webServed = counter("fact.web.served", webSeen);
    f.catAccepted = counter("fact.cat.accepted", catSeen);
    const auto count = [&](uint32_t list) { return std::max<int32_t>(0, api->read_list(session, list, 0, nullptr)); };
    // the source in use: the DEVICES row that is active; its kind is the part of the id before the first ':' (the generator's is "siggen", the I/Q file's "row:iqfile", the sound card's "row:soundcard")
    for (int32_t i = 0, rows = count(FOXAPI_LIST_DEVICES); i < rows; ++i) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        if (api->read_list(session, FOXAPI_LIST_DEVICES, static_cast<uint32_t>(i), &it) <= i) { break; }
        if ((it.flags & FOXAPI_ITEM_ACTIVE) == 0u) { continue; }
        const char* colon = std::strchr(it.detail, ':');
        std::string kind(it.detail, colon != nullptr ? static_cast<std::size_t>(colon - it.detail) : std::strlen(it.detail));
        if (kind == "row") { kind = colon != nullptr && std::strcmp(colon + 1, "soundcard") == 0 ? "soundcard" : "file"; }
        f.sourceKind = std::move(kind);
        break;
    }
    // the plugins fed now: the decoders being fed, joined by name to the active plugins' module files
    std::vector<std::string> fedNames;
    for (int32_t i = 0, n = count(FOXAPI_LIST_DECODERS); i < n; ++i) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        if (api->read_list(session, FOXAPI_LIST_DECODERS, static_cast<uint32_t>(i), &it) <= i) { break; }
        if ((it.flags & FOXAPI_ITEM_FED) != 0u) { fedNames.emplace_back(it.name); }
    }
    std::vector<std::string> files;
    if (!fedNames.empty()) {
        for (int32_t i = 0, n = count(FOXAPI_LIST_PLUGINS); i < n; ++i) {
            FoxListItem it{};
            it.structSize = sizeof(it);
            if (api->read_list(session, FOXAPI_LIST_PLUGINS, static_cast<uint32_t>(i), &it) <= i) { break; }
            if ((it.flags & FOXAPI_ITEM_ACTIVE) == 0u) { continue; }
            if (std::find(fedNames.begin(), fedNames.end(), std::string(it.name)) == fedNames.end()) { continue; }
            if (std::find(files.begin(), files.end(), std::string(it.detail)) == files.end()) { files.emplace_back(it.detail); }
        }
    }
    // the decoder nodes of a running patch (PATCH_PLAN: a decoder entry's name is its module's file name)
    if ((stateFlags & FOXAPI_RX_PATCH_RUNNING) != 0u) {
        for (int32_t i = 0, n = count(FOXAPI_LIST_PATCH_PLAN); i < n; ++i) {
            FoxListItem it{};
            it.structSize = sizeof(it);
            if (api->read_list(session, FOXAPI_LIST_PATCH_PLAN, static_cast<uint32_t>(i), &it) <= i) { break; }
            if (std::strncmp(it.detail, "decoder", 7) != 0 || it.name[0] == 0) { continue; }
            f.patchDecoder = true;
            if (std::find(files.begin(), files.end(), std::string(it.name)) == files.end()) { files.emplace_back(it.name); }
        }
    }
    {
        for (const std::string& file : files) {
            // the catalogue id from the install record that names the file ("<key> TAB ..."; "<id>@<site>" for a plugin from a site); none = the one sideloaded bucket, with no version
            const std::string rec = factText(api, session, "fact.plugin." + file + ".record");
            std::string id = rec.substr(0, rec.find('\t'));
            id = id.substr(0, id.find('@'));
            Facts::Fed fed;
            if (id.empty()) { fed.id = cascade::core::kSideloadedPluginId; }
            else { fed.id = id; fed.version = factText(api, session, "fact.plugin." + file + ".version"); }
            f.fed.push_back(std::move(fed));
        }
    }
    return f;
}

}  // namespace foxsdr::slinttester
