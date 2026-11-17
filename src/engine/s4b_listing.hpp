// s4b_listing.hpp - screen lane S4b, G3-30: the public receiver listing (Receiverbook), settings to the document net/receiver_listing.hpp describes.
//
// WHAT THIS DOES AND DOES NOT DO. A receiver directory does not receive anything FROM FoxSDR: it READS a document, status.json, from the web page's own address, and the person registers the page's
// address with the directory in their own browser (net/receiver_listing.hpp says so; the directory's side is UNVERIFIED and nothing here contacts it). FoxSDR therefore never sends the listing
// anywhere. What it does is give the web page (net/web_server.cpp, GET /status.json) the document the person's own words make, when they turn it on, and show them the EXACT text it will serve before
// anyone can read it. The page answers 404, as if the route did not exist, while the listing is off or its words are not valid; and the page itself is reachable only as far as the person has made it
// reachable (this computer only, unless they changed the web page's address).
//
// WHAT IS IN IT: the name, the location text, optionally a latitude and longitude (both or neither), a list of bands (a name, a centre frequency and a sample rate each), a listener limit and the program's
// version. NOTHING ELSE: no e-mail address, no altitude, no serial number, no radio model, no address of this computer, nothing from the receiver's live state.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>

#include "net/receiver_listing.hpp"

namespace cascade::engine::s4b {

// The settings as typed (every value is text: an empty one is "not given").
struct ListingText {
    bool enabled = false;
    std::string name, location;
    std::string latitude, longitude;      // degrees as text; both or neither
    std::string maxClients = "1";
    std::string bands;                    // "name;centre MHz;sample rate MHz", one per line or separated by '|' (at most 32)
    std::string version;
};

// Builds the listing from the typed words. False with a sentence in `problem` for anything the model would refuse (the words are never repaired or cut). An unticked listing is always built (it publishes nothing).
bool buildListing(const ListingText& in, cascade::net::ReceiverListing& out, std::string& problem);

}  // namespace cascade::engine::s4b
