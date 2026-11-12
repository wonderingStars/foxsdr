// wire_b_sources.cpp - the wire-b source table (wire_b_sources.hpp) and the one function that opens a row on a radio (Engine::openWireBSourceAt).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/wire_b_sources.hpp"

#include <optional>

#include "core/diag_log.hpp"
#include "core/receiver_tables.hpp"
#include "engine/engine.hpp"
#include "source/aaronia_source.hpp"
#include "source/ascp_source.hpp"
#include "source/device_source.hpp"
#include "source/fcd_control.hpp"
#include "source/g33ddc_source.hpp"
#include "source/g39ddc_source.hpp"
#include "source/hpsdr1_source.hpp"
#include "source/hpsdr2_source.hpp"
#include "source/icr8600_source.hpp"
#include "source/sdrconnect_source.hpp"
#include "source/soapy_remote_source.hpp"
#include "source/spyserver_source.hpp"

namespace cascade::engine::wireb {

namespace {

namespace src = cascade::source;

template <class T>
std::unique_ptr<src::DeviceSource> makeOne() {
    return std::make_unique<T>();
}

// Order = the order a Source list would show them. Default rates: the ones each source's own record names as its default (UNVERIFIED on hardware); a rate in the
// arguments wins inside the source.
const SourceKind kTable[] = {
    {"rfspace", "RFspace NetSDR / SDR-IP / Afedri (network)", true, 250000.0, &makeOne<src::AscpSource>},
    {"hpsdr1", "HPSDR protocol 1: Hermes, Red Pitaya, Hermes-Lite 2 (network)", true, 192000.0, &makeOne<src::Hpsdr1Source>},
    {"hpsdr2", "HPSDR protocol 2: Orion, Angelia (network)", true, 192000.0, &makeOne<src::Hpsdr2Source>},
    {"aaronia", "Aaronia RTSA server (network)", true, 2000000.0, &makeOne<src::AaroniaSource>},
    {"spyserver", "SpyServer (network)", true, 2500000.0, &makeOne<src::SpyServerSource>},
    {"soapyremote", "SoapyRemote server (network)", true, 2000000.0, &makeOne<src::SoapyRemoteSource>},
    {"sdrconnect", "SDRconnect (network)", true, 2000000.0, &makeOne<src::SdrConnectSource>},
    {"icr8600", "Icom IC-R8600 (USB I/Q)", false, 1920000.0, &makeOne<src::Icr8600Source>},
    {"funcube", "FUNcube Dongle Pro / Pro+", false, 192000.0, &makeOne<src::FuncubeSource>},
    {"g39ddc", "WiNRADiO G39DDC (vendor library)", false, 192000.0, &makeOne<src::G39DdcSource>},
    {"g33ddc", "WiNRADiO G33DDC (vendor library)", false, 192000.0, &makeOne<src::G33DdcSource>},
};

bool hasControlChar(const std::string& s) {
    for (const char ch : s) {
        if (static_cast<unsigned char>(ch) < 0x20u || static_cast<unsigned char>(ch) == 0x7Fu) { return true; }
    }
    return false;
}

}  // namespace

const SourceKind* sourceKinds(std::size_t& count) {
    count = sizeof(kTable) / sizeof(kTable[0]);
    return kTable;
}

const SourceKind* findSourceByKey(std::string_view key) {
    for (const SourceKind& k : kTable) {
        if (key == k.key) { return &k; }
    }
    return nullptr;
}

const SourceKind* findSourceByOpenWord(std::string_view word) {
    if (word.size() <= 5 || word.substr(0, 5) != "open-") { return nullptr; }
    return findSourceByKey(word.substr(5));
}

bool isSourceOpenId(std::string_view id) {
    const std::size_t colon = id.find(':');
    return colon != std::string_view::npos && findSourceByOpenWord(id.substr(0, colon)) != nullptr;
}

bool normaliseSourceArgs(const SourceKind& kind, const std::string& typed, std::string& args, std::string& why) {
    std::string t = typed;
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) { t.erase(t.begin()); }
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) { t.pop_back(); }
    if (t.size() > 512) { why = "the arguments are longer than 512 characters"; return false; }
    if (hasControlChar(t)) { why = "the arguments hold a control character"; return false; }
    const std::string prefix = std::string(kind.key) + "=";
    if (!kind.needsAddress) {
        args = t;   // device options only ("lib=...,serial=..."); may be empty
        return true;
    }
    if (t.rfind(prefix, 0) != 0) {
        const std::size_t comma = t.find(',');
        const std::string first = t.substr(0, comma);
        if (first.find('=') != std::string::npos) {
            why = std::string("start with the address of the ") + kind.name + " (host or host:port), then the options";
            return false;
        }
        t = prefix + t;
    }
    const std::size_t comma = t.find(',');
    const std::string address = t.substr(prefix.size(), comma == std::string::npos ? std::string::npos : comma - prefix.size());
    if (address.empty()) {
        why = std::string("type the address of the ") + kind.name + ": nothing connects while it is empty";
        return false;
    }
    args = t;
    return true;
}

}  // namespace cascade::engine::wireb

namespace cascade::engine {

// THE KIWISDR / RAW-I/Q RECIPE (Engine::openKiwiAt, engine_rawiq.cpp) for a row of the table: the arguments are checked HERE, before anything is closed, so a typo can
// never take a live radio away; the open itself blocks (a connect, a library load, a handshake) and runs on the device-open worker.
void Engine::openWireBSourceAt(const wireb::SourceKind& kind, const std::string& typed) {
    rx().sourceError_.clear();
    std::string args, why;
    if (!wireb::normaliseSourceArgs(kind, typed, args, why)) {
        rx().sourceError_ = why;
        return;
    }
    const std::optional<double> keepCenterHz = carriedAirCentre();
    DeviceOpenResult req;
    if (rx().device_ != nullptr) {
        req.closedRadio = cascade::gui::rememberedSourceAfterFailedOpen(rx().sourceKind_, cfgSoapyArgs_, cfgNativeArgs_, "", rx().pipeline_.activeSource().sampleRateHz());
        req.closedLabel = rx().deviceModel_;
        // The model, never the address: this line goes in the report.
        cascade::core::diagLogf("source: closing %s before opening a %s source", rx().deviceModel_.c_str(), kind.key);
        rx().device_ = nullptr;
        rx().soapyView_ = nullptr;
        rx().deviceArgs_.clear();
        rx().deviceModel_.clear();
        ++rx().sourceGen_;
        installSource(nullptr);
        rx().sourceKind_ = "siggen";
        applyConverterForSource();
        followInputRate();
    }
    req.kind = kind.key;
    req.args = args;
    req.row = sourceSel_;
    req.requestRateHz = kind.requestRateHz;
    req.keepCenterHz = keepCenterHz;
    cascade::core::diagLogf("source: opening a %s source", kind.key);
    launchDeviceOpen(std::move(req), std::string(kind.name));
}

}  // namespace cascade::engine
