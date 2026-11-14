// gaps1_publish.hpp - WINDOW GAPS, LANE 1: the publisher of ui/gaps1.slint's global (the diagnostics offer, the tester link prompt, the plugin fault cards) over gaps1_logic.
//
// Everything with an effect outside the window's memory is behind the Env: the facts, the plugin re-enable, the diagnostics bundle and the reports folder (the SYSTEM bank's own keys,
// so Copy diagnostics here IS Copy diagnostics there), the settings half, and the LinkNet - the site's answer to "who owns this link?". The live LinkNet is core::BetaLinkNameSender; a
// bounded run's is a CANNED one (FOXSDR_FAKE_LINK_NAME) so a test sends nothing to the network.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <slint.h>

#include "core/config.hpp"
#include "gaps1_logic.hpp"
#include "main.h"

namespace foxsdr::slintgaps1 {

// The site's answer to a link's token, asked off the window thread and polled.
class LinkNet {
public:
    virtual ~LinkNet() = default;
    virtual void resolve(const std::string& appToken) = 0;
    virtual bool take(ResolveOutcome& outcome, std::string& name) = 0;   // true once per answer
};
std::unique_ptr<LinkNet> makeLiveLinkNet();                              // core::BetaLinkNameSender over core::betaApiBaseUrl()
// "name" (the site names the owner), "!invalid" (the site does not know the token) or "!network" (it could not be asked): an answer with no network behind it.
std::unique_ptr<LinkNet> makeCannedLinkNet(const std::string& spec);

class Publisher {
public:
    struct Env {
        MainWindow* ui = nullptr;
        std::string configDir;                                           // where the one-shot link-request file is looked for; "" = hermetic (the disk is not asked)
        std::function<bool(const char* key, std::string& value)> fact;  // get_setting
        std::function<void(const std::string& pluginId)> reenable;       // FOXAPP_OP_PLUGIN_REENABLE
        std::function<void()> copyDiagnostics;                           // the SYSTEM bank's Copy diagnostics
        std::function<void()> openReports;                               // ...and its Open reports folder
        std::function<std::string()> diagStatus;                         // the line under Copy diagnostics
        std::function<cascade::core::AppConfig()> iface;                 // the interface half as the duties hold it
        std::function<void(const cascade::core::AppConfig&)> saveIface;  // ...and the way to save a changed one
        std::function<void(const std::string&)> say;
        std::unique_ptr<LinkNet> net;
    };
    explicit Publisher(Env env);
    ~Publisher();
    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;

    void wire();
    // At start: the last run did not end normally and diagnostics were on.
    void offerDiagnostics(bool due);
    // A tester link pasted into SYSTEM > Beta tester (a 40-hex app token): the question is asked once the site has named its owner.
    void linkRequested(const std::string& appToken);
    void tick(long tickNo);
    // `state gaps1.<what>` for the drive scripts: diag, link, linktext, linkerr, faults, seenfaults, stored.
    std::string readState(const std::string& what, std::string& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace foxsdr::slintgaps1
