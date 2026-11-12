// websdr_link.hpp - W18 WebSDR LINK-OUT: "open this WebSDR at the tuned frequency".
//
// This builds a URL for the person's own browser. It is NOT a WebSDR client: no
// audio, no protocol, no connection. The re-check found no client protocol and
// no terms for automated access; https://www.websdr.org/faq.html documents only
// the deep link: "?tune=" appended to the site address, example
// http://websdr.ewi.utwente.nl:8901/?tune=198am (198 kHz, AM). The same FAQ says
// its author "takes action" against framing someone else's WebSDR, so the link
// must be opened in the system browser, never embedded.
//
// DOCUMENTED: the "?tune=<kHz><mode>" shape and the suffix "am". NOT VERIFIED:
// suffixes for other modes (usb, lsb, cw, fm) and fractional-kHz handling; for
// any mode other than AM the link carries the frequency alone and the result
// says so (modeSuffixDocumented = false), so the UI can warn that the page may
// open in its default mode.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_WEBSDR_LINK_HPP
#define CASCADE_CORE_WEBSDR_LINK_HPP

#include <string>

namespace cascade::core::websdr {

struct Link {
    bool ok = false;
    std::string url;
    bool modeSuffixDocumented = false;  // true only for AM
    std::string error;                  // English, when !ok
};

// PURE. `baseUrl` is the WebSDR's address as listed (http or https, a host, an
// optional port and path; a query or fragment already on it is dropped).
// `frequencyKHz` must be finite, > 0 and <= 3,000,000. `mode` is a lower-case
// word ("am", "usb", ...), empty for none.
Link buildLink(const std::string& baseUrl, double frequencyKHz, const std::string& mode);

}  // namespace cascade::core::websdr

#endif  // CASCADE_CORE_WEBSDR_LINK_HPP
