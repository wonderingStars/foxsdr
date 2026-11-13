// web_wire_c_bookmarks.cpp - MY BOOKMARKS (wiring lane C row W2): the routes of net/web_bookmarks.hpp on the web server. See web_wire_c.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/web_wire_c.hpp"

// Must match every other TU in the program that includes httplib.h on non-Windows (see web_server.cpp for the ASan trace this was found from).
#if !defined(_WIN32)
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#endif
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cmath>

namespace cascade::net {

namespace {

using nlohmann::json;

constexpr const char* kOpenPageViewer = "open-page-viewer";   // the one set of a server that has no sessions

std::string dumpJson(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

json bookmarkJson(const WebBookmark& b) {
    json o;
    o["id"] = b.id;
    o["name"] = b.name;
    o["freqHz"] = b.freqHz;
    o["mode"] = b.mode;
    o["bandwidthHz"] = b.bandwidthHz;
    o["group"] = b.group;
    o["tags"] = b.tags;
    return o;
}

// The body of a POST or PUT: an object with the bookmark's fields. False (and a sentence) when it is not one.
bool parseBookmarkBody(const std::string& body, WebBookmark& bm, std::string& why) {
    const json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        why = "the body is not a JSON object";
        return false;
    }
    auto str = [&j](const char* key, std::string& out) {
        const auto it = j.find(key);
        if (it != j.end() && it->is_string()) { out = it->get<std::string>(); }
    };
    auto numberOf = [&j](const char* key, double& out) {
        const auto it = j.find(key);
        if (it != j.end() && it->is_number()) { out = it->get<double>(); }
    };
    bm.freqHz = 0.0;   // a body that names no frequency is refused by the model, not given a default one
    str("name", bm.name);
    str("mode", bm.mode);
    str("group", bm.group);
    numberOf("freqHz", bm.freqHz);
    numberOf("bandwidthHz", bm.bandwidthHz);
    const auto tags = j.find("tags");
    if (tags != j.end() && tags->is_array()) {
        for (const json& t : *tags) {
            if (t.is_string()) { bm.tags.push_back(t.get<std::string>()); }
        }
    }
    return true;
}

void answerBad(httplib::Response& res, const std::string& why) {
    json j;
    j["ok"] = false;
    j["error"] = why;
    res.status = 400;
    res.set_content(dumpJson(j), "application/json");
}

void answerResult(httplib::Response& res, BookmarkResult r, std::uint64_t id) {
    json j;
    j["ok"] = r == BookmarkResult::Ok;
    if (r == BookmarkResult::Ok) {
        j["id"] = id;
        res.status = 200;
    } else {
        j["error"] = bookmarkResultText(r);
        res.status = r == BookmarkResult::NotFound ? 404 : 400;
    }
    res.set_content(dumpJson(j), "application/json");
}

double paramNumber(const httplib::Request& req, const char* key) {
    if (!req.has_param(key)) { return 0.0; }
    try {
        const double v = std::stod(req.get_param_value(key));
        return std::isfinite(v) ? v : 0.0;
    } catch (const std::exception&) {
        return 0.0;
    }
}

std::uint64_t idOf(const httplib::Request& req) {
    try {
        return std::stoull(req.matches[1].str());
    } catch (const std::exception&) {
        return 0;
    }
}

}  // namespace

std::string WebWireC::viewerOf(const httplib::Request& req) const {
    const std::string t = hooks_.viewerToken ? hooks_.viewerToken(req) : std::string();
    return t.empty() ? std::string(kOpenPageViewer) : t;
}

void WebWireC::forgetViewer(const std::string& token) { bookmarks_.forgetViewer(token); }

// A viewer whose sign-in has ended is forgotten (there is no end-of-session hook, so the book is swept when it is full).
void WebWireC::pruneViewers() {
    if (!hooks_.tokenStillValid) { return; }
    for (const std::string& t : bookmarks_.viewers()) {
        if (t != kOpenPageViewer && !hooks_.tokenStillValid(t)) { bookmarks_.forgetViewer(t); }
    }
}

void WebWireC::installRoutes(httplib::Server& svr, WebWireCHooks hooks) {
    hooks_ = std::move(hooks);
    installBookmarkRoutes(svr);
    installMetricsRoute(svr);
}

void WebWireC::installBookmarkRoutes(httplib::Server& svr) {
    auto gate = [this](const httplib::Request& req, httplib::Response& res) {
        if (hooks_.signedIn && hooks_.signedIn(req)) { return true; }
        if (hooks_.deny) { hooks_.deny(res, "sign in first", 401); } else { res.status = 401; }
        return false;
    };

    svr.Get("/api/mybookmarks", [this, gate](const httplib::Request& req, httplib::Response& res) {
        if (!gate(req, res)) { return; }
        const std::string who = viewerOf(req);
        BookmarkQuery q;
        q.text = req.get_param_value("text");
        q.group = req.get_param_value("group");
        q.mode = req.get_param_value("mode");
        q.tag = req.get_param_value("tag");
        q.minHz = paramNumber(req, "min");
        q.maxHz = paramNumber(req, "max");
        const bool any = !q.text.empty() || !q.group.empty() || !q.mode.empty() || !q.tag.empty() || q.minHz != 0.0 || q.maxHz != 0.0;
        const std::vector<WebBookmark> found = any ? bookmarks_.search(who, q) : bookmarks_.list(who);
        json j;
        json arr = json::array();
        for (const WebBookmark& b : found) { arr.push_back(bookmarkJson(b)); }
        j["bookmarks"] = std::move(arr);
        j["groups"] = bookmarks_.groups(who);
        res.set_content(dumpJson(j), "application/json");
    });

    svr.Post("/api/mybookmarks", [this, gate](const httplib::Request& req, httplib::Response& res) {
        if (!gate(req, res)) { return; }
        WebBookmark bm;
        std::string why;
        if (!parseBookmarkBody(req.body, bm, why)) {
            answerBad(res, why);
            return;
        }
        const std::string who = viewerOf(req);
        std::uint64_t id = 0;
        BookmarkResult r = bookmarks_.add(who, bm, &id);
        if (r == BookmarkResult::TooManyViewers) {
            pruneViewers();
            r = bookmarks_.add(who, bm, &id);
        }
        answerResult(res, r, id);
    });

    svr.Put(R"(/api/mybookmarks/(\d+))", [this, gate](const httplib::Request& req, httplib::Response& res) {
        if (!gate(req, res)) { return; }
        WebBookmark bm;
        std::string why;
        if (!parseBookmarkBody(req.body, bm, why)) {
            answerBad(res, why);
            return;
        }
        const std::uint64_t id = idOf(req);
        answerResult(res, bookmarks_.update(viewerOf(req), id, bm), id);
    });

    svr.Delete(R"(/api/mybookmarks/(\d+))", [this, gate](const httplib::Request& req, httplib::Response& res) {
        if (!gate(req, res)) { return; }
        const std::uint64_t id = idOf(req);
        answerResult(res, bookmarks_.remove(viewerOf(req), id), id);
    });

    svr.Get("/api/mybookmarks/export", [this, gate](const httplib::Request& req, httplib::Response& res) {
        if (!gate(req, res)) { return; }
        res.set_header("Content-Disposition", "attachment; filename=\"foxsdr-bookmarks.json\"");
        res.set_content(bookmarks_.exportJson(viewerOf(req)), "application/json");
    });

    svr.Post("/api/mybookmarks/import", [this, gate](const httplib::Request& req, httplib::Response& res) {
        if (!gate(req, res)) { return; }
        const bool replace = req.get_param_value("replace") == "1";
        const std::string who = viewerOf(req);
        BookmarkImportResult r = bookmarks_.importJson(who, req.body, replace);
        if (r.result == BookmarkResult::TooManyViewers) {
            pruneViewers();
            r = bookmarks_.importJson(who, req.body, replace);
        }
        json j;
        if (r.result == BookmarkResult::Ok) {
            j["imported"] = r.imported;
            j["skipped"] = r.skipped;
            res.status = 200;
        } else {
            j["ok"] = false;
            j["error"] = bookmarkResultText(r.result);
            res.status = 400;
        }
        res.set_content(dumpJson(j), "application/json");
    });
}

}  // namespace cascade::net
