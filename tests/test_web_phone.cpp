// Tests for net/web_phone.hpp: the phone-side web pieces, as strings and by the
// installability checker. They are not served by web_server.cpp yet (that file
// belongs to other work); the wiring patch is described in the feature record.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/web_phone.hpp"

#include <algorithm>
#include <string>

#include <nlohmann/json.hpp>

#include "test_check.hpp"

using namespace cascade::net::phone;

namespace {

bool contains(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

bool anyProblem(const Check& c, const std::string& what) {
    return std::any_of(c.problems.begin(), c.problems.end(), [&](const std::string& p) { return contains(p, what); });
}

}  // namespace

int main() {
    // The shipped manifest passes the installability checker and matches the served paths.
    const std::string m = manifestJson();
    {
        const Check c = manifestInstallable(m);
        CHECK(c.ok);
        CHECK(c.problems.empty());
        const nlohmann::json j = nlohmann::json::parse(m);
        CHECK(j["start_url"] == "/");
        CHECK(j["display"] == "standalone");
        bool saw192 = false, saw512 = false;
        for (const auto& i : j["icons"]) {
            if (i["src"] == kIcon192Path && i["sizes"] == "192x192" && i["type"] == "image/png") { saw192 = true; }
            if (i["src"] == kIcon512Path && i["sizes"] == "512x512" && i["type"] == "image/png") { saw512 = true; }
        }
        CHECK(saw192 && saw512);
        CHECK(contains(headTags(), std::string("href=\"") + kManifestPath + "\""));
    }

    // Break-it: each required member removed is caught by the checker (so the
    // pass above means something).
    {
        auto without = [&](const char* key) {
            nlohmann::json j = nlohmann::json::parse(m);
            j.erase(key);
            return manifestInstallable(j.dump());
        };
        CHECK(!without("start_url").ok && anyProblem(without("start_url"), "start_url"));
        CHECK(!without("display").ok && anyProblem(without("display"), "display"));
        CHECK(!without("icons").ok && anyProblem(without("icons"), "192"));
        {
            nlohmann::json j = nlohmann::json::parse(m);
            j.erase("name");
            CHECK(manifestInstallable(j.dump()).ok);       // short_name alone is enough
            j.erase("short_name");
            CHECK(!manifestInstallable(j.dump()).ok);
        }
        {
            nlohmann::json j = nlohmann::json::parse(m);
            j["icons"].erase(1);                            // drop the 512
            const Check c = manifestInstallable(j.dump());
            CHECK(!c.ok && anyProblem(c, "512") && !anyProblem(c, "192"));
        }
        {
            nlohmann::json j = nlohmann::json::parse(m);
            j["prefer_related_applications"] = true;
            CHECK(!manifestInstallable(j.dump()).ok);
            j["prefer_related_applications"] = false;
            CHECK(manifestInstallable(j.dump()).ok);
        }
        CHECK(!manifestInstallable("not json").ok);
        CHECK(!manifestInstallable("[]").ok);
        CHECK(!manifestInstallable("").ok);
    }

    // Transport rule.
    CHECK(originInstallable("https://radio.example.org").ok);
    CHECK(originInstallable("https://radio.example.org:8443/x").ok);
    CHECK(originInstallable("HTTPS://Radio.Example.org").ok);
    CHECK(originInstallable("http://localhost").ok);
    CHECK(originInstallable("http://localhost:8080").ok);
    CHECK(originInstallable("http://127.0.0.1:8080/").ok);
    CHECK(!originInstallable("http://192.168.1.20:8080").ok);
    CHECK(!originInstallable("http://radio.example.org").ok);
    CHECK(!originInstallable("ftp://radio.example.org").ok);
    CHECK(!originInstallable("https://").ok);
    CHECK(!originInstallable("").ok);
    CHECK(anyProblem(originInstallable("http://192.168.1.20:8080"), "https"));

    // CSP: the web UI's real policy blocks a manifest (default-src 'none') until
    // manifest-src is added; the helper adds it once and changes nothing else.
    {
        const std::string csp =
            "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' data:; "
            "font-src 'self'; form-action 'none'; frame-ancestors 'none'; base-uri 'none'";
        const std::string out = cspWithManifest(csp);
        CHECK(out == csp + "; manifest-src 'self'");
        CHECK(cspWithManifest(out) == out);
        CHECK(cspWithManifest("default-src 'none';") == "default-src 'none'; manifest-src 'self'");
        CHECK(cspWithManifest("") == "manifest-src 'self'");
        CHECK(cspWithManifest("manifest-src https://a.example/") == "manifest-src https://a.example/");
        CHECK(cspWithManifest("DEFAULT-SRC 'none'; Manifest-Src 'none'") == "DEFAULT-SRC 'none'; Manifest-Src 'none'");
    }

    // Phone CSS: balanced, inside a media query, 44 px targets, safe areas, and
    // nothing the strict CSP or the colour-token rule forbids.
    {
        const std::string css = phoneCss();
        int depth = 0, minDepth = 0;
        for (char c : css) {
            if (c == '{') { ++depth; }
            if (c == '}') { --depth; minDepth = std::min(minDepth, depth); }
        }
        CHECK(depth == 0 && minDepth == 0);
        CHECK(contains(css, "@media (max-width: 600px)"));
        CHECK(contains(css, "min-height: 44px"));
        CHECK(contains(css, "env(safe-area-inset-bottom)"));
        CHECK(!contains(css, "url("));
        CHECK(!contains(css, "@import"));
        CHECK(!contains(css, "#"));              // no literal colours: the --tokens govern
        CHECK(!contains(css, "!important"));
        // The head tags: one viewport (with viewport-fit), theme colour, manifest link.
        const std::string head = headTags();
        CHECK(contains(head, "viewport-fit=cover"));
        CHECK(contains(head, "width=device-width, initial-scale=1"));
        CHECK(contains(head, "theme-color"));
        CHECK(head.find("name=\"viewport\"") == head.rfind("name=\"viewport\""));
    }
    return testSummary("test_web_phone");
}
