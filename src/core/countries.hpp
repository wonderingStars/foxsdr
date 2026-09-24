// countries.hpp - the country a user says they are in, and what follows from
// it: which ITU region's allocations apply, which band plan to show, and
// which language most people there read.
//
// WHY A COUNTRY AND NOT JUST A REGION. Nobody knows which ITU region they
// live in, and the band plan picker in Display asks exactly that. Everybody
// knows their country. So the settings page asks for the country and derives
// the rest - and still leaves the Region picker in Display for anyone who
// wants a different plan than the one their country implies.
//
// THE REGION IS NOT A MATTER OF OPINION. ITU Radio Regulations Article 5
// (Nos. 5.3-5.9) draws Regions 1, 2 and 3 with three boundary lines, A, B
// and C, and names the countries that are Region 1 whatever side of line A
// they lie on (Armenia, Azerbaijan, Russia, Georgia, Kazakhstan, Mongolia,
// Uzbekistan, Kyrgyzstan, Tajikistan, Turkmenistan, Türkiye, Ukraine) and
// the one that is Region 3 wholly (Iran). How the table was derived from
// that text, and the territories that straddle a line, are written down in
// countries.cpp above the table.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_COUNTRIES_HPP
#define CASCADE_CORE_COUNTRIES_HPP

#include <span>
#include <string>

namespace cascade::core {

struct Country {
    const char* code;      // ISO 3166-1 alpha-2, upper case: "BR"
    const char* name;      // English short name, marked FOX_TR_NOOP: draw it with tr()
    int ituRegion;         // 1, 2 or 3
    const char* bandPlan;  // id of a plan in resources/bandplans: "itu-region2", "uk"
    const char* language;  // BCP 47 tag of the language most people there read
};

// Every current ISO 3166-1 country and territory except Antarctica (which no
// ITU region covers and no band plan describes), in English-name order.
std::span<const Country> countries();

// The entry for `code` (exact, upper case), or null - including for "", the
// config's "not chosen".
const Country* findCountry(const std::string& code);

}  // namespace cascade::core

#endif  // CASCADE_CORE_COUNTRIES_HPP
