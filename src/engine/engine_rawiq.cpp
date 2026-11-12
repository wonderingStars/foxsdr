// engine_rawiq.cpp - a raw I/Q stream on a network socket as a radio's source (engine serial lane A, row R5): SELECT_SOURCE / RADIO_ADD "open-rawiq:<address>".
//
// The address grammar and the framing are source/raw_iq_net_source.hpp's. This is the KiwiSDR / rtl_tcp recipe (Engine::openKiwiAt): the address is validated HERE, before anything is closed, so
// a typo can never take a live radio away; the open blocks (a TCP connect, a UDP bind, the header wait) and runs on the worker (launchDeviceOpen).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <optional>
#include <string>

#include "core/diag_log.hpp"
#include "core/receiver_tables.hpp"
#include "engine/engine.hpp"
#include "source/raw_iq_net_source.hpp"

namespace cascade::engine {

void Engine::openRawIqAt(const std::string& args) {
    rx().sourceError_.clear();
    const std::string text = (args.rfind("rawiq=", 0) == 0) ? args.substr(6) : args;
    cascade::source::rawiq::Spec spec;
    std::string why;
    if (!cascade::source::rawiq::parseSpec(text, spec, why)) {
        rx().sourceError_ = why;
        return;
    }
    const std::optional<double> keepCenterHz = carriedAirCentre();
    DeviceOpenResult req;
    if (rx().device_ != nullptr) {
        req.closedRadio = cascade::gui::rememberedSourceAfterFailedOpen(
            rx().sourceKind_, cfgSoapyArgs_, cfgNativeArgs_, "",
            rx().pipeline_.activeSource().sampleRateHz());
        req.closedLabel = rx().deviceModel_;
        // The model, never the address: this line goes in the report.
        cascade::core::diagLogf("source: closing %s before opening the I/Q stream", rx().deviceModel_.c_str());
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
    req.kind = cascade::gui::kRawIqDriverKey;
    req.args = std::string("rawiq=") + text;
    req.row = sourceSel_;
    req.requestRateHz = spec.rateHz > 0.0 ? spec.rateHz : 2.0e6;   // (a header stream refuses a rate that is not its own: the request is only a default)
    req.keepCenterHz = keepCenterHz;
    cascade::core::diagLogf("source: opening an I/Q stream (%s)", spec.udp ? "udp" : "tcp");
    launchDeviceOpen(std::move(req), std::string("I/Q stream ") + (spec.udp ? "listening on " : "from ") + spec.host);
}

}  // namespace cascade::engine
