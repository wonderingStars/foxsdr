// web_phone.hpp - N26 PHONE APP, WEB SIDE: what the existing web UI needs to be
// usable and installable on a phone, as NEW, self-contained pieces. Nothing here
// is served yet: net/web_server.cpp (owned by other work) has to be patched to
// serve them; the patch is described in docs/feature-records/array-maths.md.
//
// Facts used (pages opened 2026-11-11):
//  * Installability (MDN, "Making PWAs installable"): a manifest linked with
//    <link rel="manifest">; for Chromium browsers name or short_name, icons
//    containing a 192 px and a 512 px icon, start_url, display (or
//    display_override), prefer_related_applications false or absent; served over
//    https, or from localhost / 127.0.0.1 with or without a port. A service
//    worker is NOT required. Firefox does not install PWAs from a manifest;
//    Android installs as an app only from Chrome (with Google Mobile Services) or
//    Samsung Internet, other browsers add a shortcut; iOS 16.4+ installs from the
//    Share menu in several browsers.
//    https://developer.mozilla.org/en-US/docs/Web/Progressive_web_apps/Guides/Making_PWAs_installable
//  * CSP (MDN, manifest-src): a manifest is governed by manifest-src, which falls
//    back to default-src. The web UI's policy is default-src 'none', so the
//    manifest is BLOCKED unless "manifest-src 'self'" is added (cspWithManifest).
//    https://developer.mozilla.org/en-US/docs/Web/HTTP/Reference/Headers/Content-Security-Policy/manifest-src
//  * Touch targets (W3C WCAG 2.2, SC 2.5.8 Target Size (Minimum)): at least 24 by
//    24 CSS px; the page also describes SC 2.5.5 (Enhanced) with a 44 by 44 CSS px
//    measure. The phone CSS uses 44.
//    https://www.w3.org/WAI/WCAG22/Understanding/target-size-minimum.html
//  * env(safe-area-inset-*) (MDN, env()): the four inset variables exist; the
//    viewport-fit=cover requirement is general knowledge, not read on that page
//    (NOT VERIFIED here), the tag is included because without it the insets are
//    zero at worst, which is harmless.
//
// Already in the web UI and NOT repeated: <meta name="viewport" content="width=
// device-width, initial-scale=1">, @media (max-width: 900px) and 820px rules,
// pointer events for the map, touchstart/touchend push-to-talk.
//
// UNPROVEN: no phone, no phone browser and no HTTPS listener on this desk; the
// pieces are proven only as strings and by the installability checker below.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_NET_WEB_PHONE_HPP
#define CASCADE_NET_WEB_PHONE_HPP

#include <string>
#include <vector>

namespace cascade::net::phone {

// Where the icons must be served (PNG, 192 and 512 px; the 256 px resource icon
// is too small for the 512 entry, so a 512 px and a 192 px PNG must be made).
inline constexpr const char* kManifestPath = "/manifest.webmanifest";
inline constexpr const char* kIcon192Path = "/icons/foxsdr-192.png";
inline constexpr const char* kIcon512Path = "/icons/foxsdr-512.png";

// The manifest, as JSON text. Content-Type for it: application/manifest+json.
std::string manifestJson();

// Lines to add inside <head> of the index page: the manifest link, theme-color
// and a viewport that allows safe-area insets. Replaces the existing viewport
// meta rather than adding a second one (see the patch note).
const char* headTags();

// An additive stylesheet for narrow screens (served after the existing CSS):
// 44 px touch targets, safe-area padding, no horizontal scroll from wide tables.
const char* phoneCss();

// The CSP with "manifest-src 'self'" added when no manifest-src is present.
// Idempotent. Everything else in the policy is left exactly as given.
std::string cspWithManifest(const std::string& csp);

struct Check {
    bool ok = false;
    std::vector<std::string> problems;  // English, one per missing/wrong thing
};

// Does this origin meet the transport rule: https, or http on localhost or
// 127.0.0.1 with or without a port? `origin` like "https://host:8443".
Check originInstallable(const std::string& origin);

// Does this manifest JSON carry the members Chromium requires (name or
// short_name, a 192 and a 512 icon, start_url, display or display_override,
// prefer_related_applications false/absent)?
Check manifestInstallable(const std::string& json);

}  // namespace cascade::net::phone

#endif  // CASCADE_NET_WEB_PHONE_HPP
