// Webhook for decoded messages: an HTTP POST of the same JSON the MQTT publisher sends (core/decode_json.hpp), through the project's existing
// bounded, cancellable client, core::postBounded() (core/crash_upload.hpp; WinHTTP on Windows, cpp-httplib elsewhere), with its timeouts and its
// response cap. Nothing new is written for the transport.
//
// WHAT THAT CLIENT ALLOWS (and this block therefore inherits): https to any host; plain http ONLY to the loopback address (127.0.0.1, localhost,
// ::1). A webhook on another machine of the LAN therefore needs https. That rule exists so a shipped binary can never be talked into sending
// data in clear over somebody's network; relaxing it for a user-typed LAN address is an owner decision (see the record).
//
// Off by default: with enabled == false nothing is sent and nothing touches the network. BLOCKING: call it from a worker thread, never the DSP
// thread. The URL may carry a secret (a token in the path or query): it is never logged or put into an error text here; only the status is.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <memory>
#include <string>

#include "core/crash_upload.hpp"

namespace cascade::core::webhook {

struct Options {
    bool enabled = false;
    std::string url;  // http(s)://host[:port]/path
};

struct Result {
    bool attempted = false;  // a request was made (false: refused locally)
    bool ok = false;         // a 2xx answer
    int status = 0;          // 0 when no answer arrived
    std::string error;       // never contains the URL
};

inline Result send(const Options& o, const std::string& json, const std::shared_ptr<UploadCancel>& cancel = nullptr) {
    Result r;
    if (!o.enabled) { r.error = "disabled"; return r; }
    const bool http = o.url.rfind("http://", 0) == 0, https = o.url.rfind("https://", 0) == 0;
    if (!http && !https) { r.error = "the URL must start with http:// or https://"; return r; }
    if (o.url.size() > 2048 || o.url.find_first_of(" \r\n\t") != std::string::npos || o.url.find('\0') != std::string::npos) {
        r.error = "bad URL";
        return r;
    }
    if (json.empty()) { r.error = "nothing to send"; return r; }
    const std::shared_ptr<UploadCancel> c = cancel ? cancel : std::make_shared<UploadCancel>();
    const RawPostResult p = postBounded(o.url, json, c, false);
    r.attempted = p.attempted;
    r.status = p.status;
    if (p.cancelled) { r.error = "cancelled"; return r; }
    if (!p.attempted) { r.error = http ? "refused: plain http is only allowed to this machine, use https for another host" : "not sent"; return r; }
    if (p.status == 0) { r.error = "no answer"; return r; }
    r.ok = p.status >= 200 && p.status < 300;
    if (!r.ok) { r.error = "HTTP " + std::to_string(p.status); }
    return r;
}

}  // namespace cascade::core::webhook
