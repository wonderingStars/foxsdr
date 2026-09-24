// countries.cpp - see countries.hpp.
//
// HOW THE REGION COLUMN WAS MADE, so it can be checked rather than trusted.
//
// Source: ITU Radio Regulations Article 5, Nos. 5.3-5.9, in the transcription
// the US government publishes as 47 CFR 2.104(b) (govinfo.gov, CFR 2021,
// title 47 vol. 1, section 2.104). Its three lines, verbatim in substance:
//   A - the North Pole along 40 E to 40 N; great circle to 60 E on the Tropic
//       of Cancer; along 60 E to the South Pole.
//   B - the North Pole along 10 W to 72 N; great circle to 50 W 40 N; great
//       circle to 20 W 10 S; along 20 W to the South Pole.
//   C - the North Pole by great circle to 65 30' N on the Bering Strait
//       boundary; great circle to 165 E 50 N; great circle to 170 W 10 N;
//       along 10 N to 120 W; along 120 W to the South Pole.
// Region 1 lies between B and A, Region 2 between C and B, Region 3 between
// A and C - plus the named-country clauses quoted in countries.hpp.
//
// Each country's representative point (its geographic centroid) was placed
// against those lines by computing the great-circle arcs, and the named
// clauses applied on top. The borderline cases, checked one by one:
//   Iran        3 - named: "excluding any of the territory of Iran" from 1.
//   Afghanistan 3 - wholly east of 60 E.  Pakistan 3 likewise.
//   Iraq, Kuwait, Saudi Arabia, Bahrain, Qatar, UAE, Oman, Yemen  1 - all
//               west of line A (Oman's easternmost cape is at 59.8 E, south
//               of the Tropic where A runs down 60 E).
//   Russia, Kazakhstan, Mongolia, Türkiye, the Caucasus and Central Asian
//               republics  1 - named, whatever side of A they lie on.
//   Greenland   2 - west of B at every latitude it spans.
//   Iceland     1 - B crosses 65 N near 29 W; Iceland ends at 24.5 W.
//   Cape Verde  1, Azores/Madeira (Portugal) 1, Bermuda 2, Saint Pierre and
//               Miquelon 2 - by their side of B.
//   Hawaii      2 - east of C (C is near 174 W at 20 N); it is part of US,
//               which is Region 2 in this table.
//   Pacific south of 10 N and west of 120 W is Region 3: Kiribati, French
//               Polynesia, Pitcairn, Cook Islands, Samoa, American Samoa,
//               Tokelau, Niue, Tonga, Wallis and Futuna, Tuvalu. Guam, the
//               Northern Marianas, Palau, Micronesia and the Marshall Islands
//               are west of C: Region 3.
//   Antarctica  omitted.
// Territories that STRADDLE a line get the region of most of their land
// and people, and are marked where they occur below: United States Minor
// Outlying Islands (Navassa, Johnston and Midway are Region 2; Wake, Baker,
// Howland, Jarvis, Palmyra and Kingman are Region 3), French Southern
// Territories (Kerguelen and Amsterdam are 3, Crozet and the Scattered
// Islands 1) and Mauritius (Rodrigues, at 63 E, is Region 3).
//
// THE BAND PLAN is the country's own where resources/bandplans carries one
// (uk, us, canada, australia, japan) and its ITU region's otherwise.
//
// THE LANGUAGE is a suggestion the settings page offers as one key when a
// catalogue exists for it; it never switches anything by itself.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/countries.hpp"

#include <iterator>

#include "core/i18n.hpp"

namespace cascade::core {
namespace {

constexpr Country kCountries[] = {
    {"AF", FOX_TR_NOOP("Afghanistan"), 3, "itu-region3", "fa"},
    {"AL", FOX_TR_NOOP("Albania"), 1, "itu-region1", "sq"},
    {"DZ", FOX_TR_NOOP("Algeria"), 1, "itu-region1", "ar"},
    {"AS", FOX_TR_NOOP("American Samoa"), 3, "itu-region3", "en"},
    {"AD", FOX_TR_NOOP("Andorra"), 1, "itu-region1", "ca"},
    {"AO", FOX_TR_NOOP("Angola"), 1, "itu-region1", "pt"},
    {"AI", FOX_TR_NOOP("Anguilla"), 2, "itu-region2", "en"},
    {"AG", FOX_TR_NOOP("Antigua and Barbuda"), 2, "itu-region2", "en"},
    {"AR", FOX_TR_NOOP("Argentina"), 2, "itu-region2", "es"},
    {"AM", FOX_TR_NOOP("Armenia"), 1, "itu-region1", "hy"},
    {"AW", FOX_TR_NOOP("Aruba"), 2, "itu-region2", "nl"},
    {"AU", FOX_TR_NOOP("Australia"), 3, "australia", "en"},
    {"AT", FOX_TR_NOOP("Austria"), 1, "itu-region1", "de"},
    {"AZ", FOX_TR_NOOP("Azerbaijan"), 1, "itu-region1", "az"},
    {"BS", FOX_TR_NOOP("Bahamas"), 2, "itu-region2", "en"},
    {"BH", FOX_TR_NOOP("Bahrain"), 1, "itu-region1", "ar"},
    {"BD", FOX_TR_NOOP("Bangladesh"), 3, "itu-region3", "bn"},
    {"BB", FOX_TR_NOOP("Barbados"), 2, "itu-region2", "en"},
    {"BY", FOX_TR_NOOP("Belarus"), 1, "itu-region1", "ru"},
    {"BE", FOX_TR_NOOP("Belgium"), 1, "itu-region1", "nl"},
    {"BZ", FOX_TR_NOOP("Belize"), 2, "itu-region2", "en"},
    {"BJ", FOX_TR_NOOP("Benin"), 1, "itu-region1", "fr"},
    {"BM", FOX_TR_NOOP("Bermuda"), 2, "itu-region2", "en"},
    {"BT", FOX_TR_NOOP("Bhutan"), 3, "itu-region3", "dz"},
    {"BO", FOX_TR_NOOP("Bolivia"), 2, "itu-region2", "es"},
    {"BQ", FOX_TR_NOOP("Bonaire, Sint Eustatius and Saba"), 2, "itu-region2", "en"},
    {"BA", FOX_TR_NOOP("Bosnia and Herzegovina"), 1, "itu-region1", "bs"},
    {"BW", FOX_TR_NOOP("Botswana"), 1, "itu-region1", "en"},
    {"BV", FOX_TR_NOOP("Bouvet Island"), 1, "itu-region1", "nb"},
    {"BR", FOX_TR_NOOP("Brazil"), 2, "itu-region2", "pt-BR"},
    {"IO", FOX_TR_NOOP("British Indian Ocean Territory"), 3, "itu-region3", "en"},
    {"VG", FOX_TR_NOOP("British Virgin Islands"), 2, "itu-region2", "en"},
    {"BN", FOX_TR_NOOP("Brunei"), 3, "itu-region3", "ms"},
    {"BG", FOX_TR_NOOP("Bulgaria"), 1, "itu-region1", "bg"},
    {"BF", FOX_TR_NOOP("Burkina Faso"), 1, "itu-region1", "fr"},
    {"BI", FOX_TR_NOOP("Burundi"), 1, "itu-region1", "rn"},
    {"CV", FOX_TR_NOOP("Cabo Verde"), 1, "itu-region1", "pt"},
    {"KH", FOX_TR_NOOP("Cambodia"), 3, "itu-region3", "km"},
    {"CM", FOX_TR_NOOP("Cameroon"), 1, "itu-region1", "fr"},
    {"CA", FOX_TR_NOOP("Canada"), 2, "canada", "en"},
    {"KY", FOX_TR_NOOP("Cayman Islands"), 2, "itu-region2", "en"},
    {"CF", FOX_TR_NOOP("Central African Republic"), 1, "itu-region1", "fr"},
    {"TD", FOX_TR_NOOP("Chad"), 1, "itu-region1", "fr"},
    {"CL", FOX_TR_NOOP("Chile"), 2, "itu-region2", "es"},
    {"CN", FOX_TR_NOOP("China"), 3, "itu-region3", "zh-CN"},
    {"CX", FOX_TR_NOOP("Christmas Island"), 3, "itu-region3", "en"},
    {"CC", FOX_TR_NOOP("Cocos (Keeling) Islands"), 3, "itu-region3", "en"},
    {"CO", FOX_TR_NOOP("Colombia"), 2, "itu-region2", "es"},
    {"KM", FOX_TR_NOOP("Comoros"), 1, "itu-region1", "fr"},
    {"CG", FOX_TR_NOOP("Congo"), 1, "itu-region1", "fr"},
    {"CD", FOX_TR_NOOP("Congo (Democratic Republic)"), 1, "itu-region1", "fr"},
    {"CK", FOX_TR_NOOP("Cook Islands"), 3, "itu-region3", "en"},
    {"CR", FOX_TR_NOOP("Costa Rica"), 2, "itu-region2", "es"},
    {"HR", FOX_TR_NOOP("Croatia"), 1, "itu-region1", "hr"},
    {"CU", FOX_TR_NOOP("Cuba"), 2, "itu-region2", "es"},
    {"CW", FOX_TR_NOOP("Curaçao"), 2, "itu-region2", "en"},
    {"CY", FOX_TR_NOOP("Cyprus"), 1, "itu-region1", "el"},
    {"CZ", FOX_TR_NOOP("Czechia"), 1, "itu-region1", "cs"},
    {"CI", FOX_TR_NOOP("Côte d'Ivoire"), 1, "itu-region1", "fr"},
    {"DK", FOX_TR_NOOP("Denmark"), 1, "itu-region1", "da"},
    {"DJ", FOX_TR_NOOP("Djibouti"), 1, "itu-region1", "fr"},
    {"DM", FOX_TR_NOOP("Dominica"), 2, "itu-region2", "en"},
    {"DO", FOX_TR_NOOP("Dominican Republic"), 2, "itu-region2", "es"},
    {"EC", FOX_TR_NOOP("Ecuador"), 2, "itu-region2", "es"},
    {"EG", FOX_TR_NOOP("Egypt"), 1, "itu-region1", "ar"},
    {"SV", FOX_TR_NOOP("El Salvador"), 2, "itu-region2", "es"},
    {"GQ", FOX_TR_NOOP("Equatorial Guinea"), 1, "itu-region1", "es"},
    {"ER", FOX_TR_NOOP("Eritrea"), 1, "itu-region1", "ti"},
    {"EE", FOX_TR_NOOP("Estonia"), 1, "itu-region1", "et"},
    {"SZ", FOX_TR_NOOP("Eswatini"), 1, "itu-region1", "en"},
    {"ET", FOX_TR_NOOP("Ethiopia"), 1, "itu-region1", "am"},
    {"FK", FOX_TR_NOOP("Falkland Islands (Malvinas)"), 2, "itu-region2", "en"},
    {"FO", FOX_TR_NOOP("Faroe Islands"), 1, "itu-region1", "da"},
    {"FJ", FOX_TR_NOOP("Fiji"), 3, "itu-region3", "en"},
    {"FI", FOX_TR_NOOP("Finland"), 1, "itu-region1", "fi"},
    {"FR", FOX_TR_NOOP("France"), 1, "itu-region1", "fr"},
    {"GF", FOX_TR_NOOP("French Guiana"), 2, "itu-region2", "fr"},
    {"PF", FOX_TR_NOOP("French Polynesia"), 3, "itu-region3", "fr"},
    {"TF", FOX_TR_NOOP("French Southern Territories"), 3, "itu-region3", "fr"},  // straddles A - see above
    {"GA", FOX_TR_NOOP("Gabon"), 1, "itu-region1", "fr"},
    {"GM", FOX_TR_NOOP("Gambia"), 1, "itu-region1", "en"},
    {"GE", FOX_TR_NOOP("Georgia"), 1, "itu-region1", "ka"},
    {"DE", FOX_TR_NOOP("Germany"), 1, "itu-region1", "de"},
    {"GH", FOX_TR_NOOP("Ghana"), 1, "itu-region1", "en"},
    {"GI", FOX_TR_NOOP("Gibraltar"), 1, "itu-region1", "en"},
    {"GR", FOX_TR_NOOP("Greece"), 1, "itu-region1", "el"},
    {"GL", FOX_TR_NOOP("Greenland"), 2, "itu-region2", "da"},
    {"GD", FOX_TR_NOOP("Grenada"), 2, "itu-region2", "en"},
    {"GP", FOX_TR_NOOP("Guadeloupe"), 2, "itu-region2", "fr"},
    {"GU", FOX_TR_NOOP("Guam"), 3, "itu-region3", "en"},
    {"GT", FOX_TR_NOOP("Guatemala"), 2, "itu-region2", "es"},
    {"GG", FOX_TR_NOOP("Guernsey"), 1, "itu-region1", "en"},
    {"GN", FOX_TR_NOOP("Guinea"), 1, "itu-region1", "fr"},
    {"GW", FOX_TR_NOOP("Guinea-Bissau"), 1, "itu-region1", "pt"},
    {"GY", FOX_TR_NOOP("Guyana"), 2, "itu-region2", "en"},
    {"HT", FOX_TR_NOOP("Haiti"), 2, "itu-region2", "fr"},
    {"HM", FOX_TR_NOOP("Heard Island and McDonald Islands"), 3, "itu-region3", "en"},
    {"HN", FOX_TR_NOOP("Honduras"), 2, "itu-region2", "es"},
    {"HK", FOX_TR_NOOP("Hong Kong"), 3, "itu-region3", "zh-HK"},
    {"HU", FOX_TR_NOOP("Hungary"), 1, "itu-region1", "hu"},
    {"IS", FOX_TR_NOOP("Iceland"), 1, "itu-region1", "is"},
    {"IN", FOX_TR_NOOP("India"), 3, "itu-region3", "hi"},
    {"ID", FOX_TR_NOOP("Indonesia"), 3, "itu-region3", "id"},
    {"IR", FOX_TR_NOOP("Iran"), 3, "itu-region3", "fa"},
    {"IQ", FOX_TR_NOOP("Iraq"), 1, "itu-region1", "ar"},
    {"IE", FOX_TR_NOOP("Ireland"), 1, "itu-region1", "en"},
    {"IM", FOX_TR_NOOP("Isle of Man"), 1, "itu-region1", "en"},
    {"IL", FOX_TR_NOOP("Israel"), 1, "itu-region1", "he"},
    {"IT", FOX_TR_NOOP("Italy"), 1, "itu-region1", "it"},
    {"JM", FOX_TR_NOOP("Jamaica"), 2, "itu-region2", "en"},
    {"JP", FOX_TR_NOOP("Japan"), 3, "japan", "ja"},
    {"JE", FOX_TR_NOOP("Jersey"), 1, "itu-region1", "en"},
    {"JO", FOX_TR_NOOP("Jordan"), 1, "itu-region1", "ar"},
    {"KZ", FOX_TR_NOOP("Kazakhstan"), 1, "itu-region1", "kk"},
    {"KE", FOX_TR_NOOP("Kenya"), 1, "itu-region1", "en"},
    {"KI", FOX_TR_NOOP("Kiribati"), 3, "itu-region3", "en"},
    {"KW", FOX_TR_NOOP("Kuwait"), 1, "itu-region1", "ar"},
    {"KG", FOX_TR_NOOP("Kyrgyzstan"), 1, "itu-region1", "ru"},
    {"LA", FOX_TR_NOOP("Laos"), 3, "itu-region3", "lo"},
    {"LV", FOX_TR_NOOP("Latvia"), 1, "itu-region1", "lv"},
    {"LB", FOX_TR_NOOP("Lebanon"), 1, "itu-region1", "ar"},
    {"LS", FOX_TR_NOOP("Lesotho"), 1, "itu-region1", "en"},
    {"LR", FOX_TR_NOOP("Liberia"), 1, "itu-region1", "en"},
    {"LY", FOX_TR_NOOP("Libya"), 1, "itu-region1", "ar"},
    {"LI", FOX_TR_NOOP("Liechtenstein"), 1, "itu-region1", "de"},
    {"LT", FOX_TR_NOOP("Lithuania"), 1, "itu-region1", "lt"},
    {"LU", FOX_TR_NOOP("Luxembourg"), 1, "itu-region1", "lb"},
    {"MO", FOX_TR_NOOP("Macao"), 3, "itu-region3", "pt"},
    {"MG", FOX_TR_NOOP("Madagascar"), 1, "itu-region1", "mg"},
    {"MW", FOX_TR_NOOP("Malawi"), 1, "itu-region1", "en"},
    {"MY", FOX_TR_NOOP("Malaysia"), 3, "itu-region3", "ms"},
    {"MV", FOX_TR_NOOP("Maldives"), 3, "itu-region3", "dv"},
    {"ML", FOX_TR_NOOP("Mali"), 1, "itu-region1", "fr"},
    {"MT", FOX_TR_NOOP("Malta"), 1, "itu-region1", "en"},
    {"MH", FOX_TR_NOOP("Marshall Islands"), 3, "itu-region3", "en"},
    {"MQ", FOX_TR_NOOP("Martinique"), 2, "itu-region2", "fr"},
    {"MR", FOX_TR_NOOP("Mauritania"), 1, "itu-region1", "ar"},
    {"MU", FOX_TR_NOOP("Mauritius"), 1, "itu-region1", "en"},  // Rodrigues is Region 3 - see above
    {"YT", FOX_TR_NOOP("Mayotte"), 1, "itu-region1", "fr"},
    {"MX", FOX_TR_NOOP("Mexico"), 2, "itu-region2", "es"},
    {"FM", FOX_TR_NOOP("Micronesia"), 3, "itu-region3", "en"},
    {"MD", FOX_TR_NOOP("Moldova"), 1, "itu-region1", "ro"},
    {"MC", FOX_TR_NOOP("Monaco"), 1, "itu-region1", "fr"},
    {"MN", FOX_TR_NOOP("Mongolia"), 1, "itu-region1", "mn"},
    {"ME", FOX_TR_NOOP("Montenegro"), 1, "itu-region1", "sr"},
    {"MS", FOX_TR_NOOP("Montserrat"), 2, "itu-region2", "en"},
    {"MA", FOX_TR_NOOP("Morocco"), 1, "itu-region1", "ar"},
    {"MZ", FOX_TR_NOOP("Mozambique"), 1, "itu-region1", "pt"},
    {"MM", FOX_TR_NOOP("Myanmar"), 3, "itu-region3", "my"},
    {"NA", FOX_TR_NOOP("Namibia"), 1, "itu-region1", "en"},
    {"NR", FOX_TR_NOOP("Nauru"), 3, "itu-region3", "en"},
    {"NP", FOX_TR_NOOP("Nepal"), 3, "itu-region3", "ne"},
    {"NL", FOX_TR_NOOP("Netherlands"), 1, "itu-region1", "nl"},
    {"NC", FOX_TR_NOOP("New Caledonia"), 3, "itu-region3", "fr"},
    {"NZ", FOX_TR_NOOP("New Zealand"), 3, "itu-region3", "en"},
    {"NI", FOX_TR_NOOP("Nicaragua"), 2, "itu-region2", "es"},
    {"NE", FOX_TR_NOOP("Niger"), 1, "itu-region1", "fr"},
    {"NG", FOX_TR_NOOP("Nigeria"), 1, "itu-region1", "en"},
    {"NU", FOX_TR_NOOP("Niue"), 3, "itu-region3", "en"},
    {"NF", FOX_TR_NOOP("Norfolk Island"), 3, "itu-region3", "en"},
    {"KP", FOX_TR_NOOP("North Korea"), 3, "itu-region3", "ko"},
    {"MK", FOX_TR_NOOP("North Macedonia"), 1, "itu-region1", "mk"},
    {"MP", FOX_TR_NOOP("Northern Mariana Islands"), 3, "itu-region3", "en"},
    {"NO", FOX_TR_NOOP("Norway"), 1, "itu-region1", "nb"},
    {"OM", FOX_TR_NOOP("Oman"), 1, "itu-region1", "ar"},
    {"PK", FOX_TR_NOOP("Pakistan"), 3, "itu-region3", "ur"},
    {"PW", FOX_TR_NOOP("Palau"), 3, "itu-region3", "en"},
    {"PS", FOX_TR_NOOP("Palestine"), 1, "itu-region1", "ar"},
    {"PA", FOX_TR_NOOP("Panama"), 2, "itu-region2", "es"},
    {"PG", FOX_TR_NOOP("Papua New Guinea"), 3, "itu-region3", "en"},
    {"PY", FOX_TR_NOOP("Paraguay"), 2, "itu-region2", "es"},
    {"PE", FOX_TR_NOOP("Peru"), 2, "itu-region2", "es"},
    {"PH", FOX_TR_NOOP("Philippines"), 3, "itu-region3", "fil"},
    {"PN", FOX_TR_NOOP("Pitcairn"), 3, "itu-region3", "en"},
    {"PL", FOX_TR_NOOP("Poland"), 1, "itu-region1", "pl"},
    {"PT", FOX_TR_NOOP("Portugal"), 1, "itu-region1", "pt-PT"},
    {"PR", FOX_TR_NOOP("Puerto Rico"), 2, "itu-region2", "en"},
    {"QA", FOX_TR_NOOP("Qatar"), 1, "itu-region1", "ar"},
    {"RO", FOX_TR_NOOP("Romania"), 1, "itu-region1", "ro"},
    {"RU", FOX_TR_NOOP("Russia"), 1, "itu-region1", "ru"},
    {"RW", FOX_TR_NOOP("Rwanda"), 1, "itu-region1", "rw"},
    {"RE", FOX_TR_NOOP("Réunion"), 1, "itu-region1", "fr"},
    {"BL", FOX_TR_NOOP("Saint Barthélemy"), 2, "itu-region2", "fr"},
    {"SH", FOX_TR_NOOP("Saint Helena, Ascension and Tristan da Cunha"), 1, "itu-region1", "en"},
    {"KN", FOX_TR_NOOP("Saint Kitts and Nevis"), 2, "itu-region2", "en"},
    {"LC", FOX_TR_NOOP("Saint Lucia"), 2, "itu-region2", "en"},
    {"MF", FOX_TR_NOOP("Saint Martin (French part)"), 2, "itu-region2", "fr"},
    {"PM", FOX_TR_NOOP("Saint Pierre and Miquelon"), 2, "itu-region2", "fr"},
    {"VC", FOX_TR_NOOP("Saint Vincent and the Grenadines"), 2, "itu-region2", "en"},
    {"WS", FOX_TR_NOOP("Samoa"), 3, "itu-region3", "en"},
    {"SM", FOX_TR_NOOP("San Marino"), 1, "itu-region1", "it"},
    {"ST", FOX_TR_NOOP("Sao Tome and Principe"), 1, "itu-region1", "pt"},
    {"SA", FOX_TR_NOOP("Saudi Arabia"), 1, "itu-region1", "ar"},
    {"SN", FOX_TR_NOOP("Senegal"), 1, "itu-region1", "fr"},
    {"RS", FOX_TR_NOOP("Serbia"), 1, "itu-region1", "sr"},
    {"SC", FOX_TR_NOOP("Seychelles"), 1, "itu-region1", "en"},
    {"SL", FOX_TR_NOOP("Sierra Leone"), 1, "itu-region1", "en"},
    {"SG", FOX_TR_NOOP("Singapore"), 3, "itu-region3", "en"},
    {"SX", FOX_TR_NOOP("Sint Maarten (Dutch part)"), 2, "itu-region2", "en"},
    {"SK", FOX_TR_NOOP("Slovakia"), 1, "itu-region1", "sk"},
    {"SI", FOX_TR_NOOP("Slovenia"), 1, "itu-region1", "sl"},
    {"SB", FOX_TR_NOOP("Solomon Islands"), 3, "itu-region3", "en"},
    {"SO", FOX_TR_NOOP("Somalia"), 1, "itu-region1", "so"},
    {"ZA", FOX_TR_NOOP("South Africa"), 1, "itu-region1", "en"},
    {"GS", FOX_TR_NOOP("South Georgia and the South Sandwich Islands"), 2, "itu-region2", "en"},
    {"KR", FOX_TR_NOOP("South Korea"), 3, "itu-region3", "ko"},
    {"SS", FOX_TR_NOOP("South Sudan"), 1, "itu-region1", "en"},
    {"ES", FOX_TR_NOOP("Spain"), 1, "itu-region1", "es"},
    {"LK", FOX_TR_NOOP("Sri Lanka"), 3, "itu-region3", "si"},
    {"SD", FOX_TR_NOOP("Sudan"), 1, "itu-region1", "ar"},
    {"SR", FOX_TR_NOOP("Suriname"), 2, "itu-region2", "nl"},
    {"SJ", FOX_TR_NOOP("Svalbard and Jan Mayen"), 1, "itu-region1", "nb"},
    {"SE", FOX_TR_NOOP("Sweden"), 1, "itu-region1", "sv"},
    {"CH", FOX_TR_NOOP("Switzerland"), 1, "itu-region1", "de"},
    {"SY", FOX_TR_NOOP("Syria"), 1, "itu-region1", "ar"},
    {"TW", FOX_TR_NOOP("Taiwan"), 3, "itu-region3", "zh-TW"},
    {"TJ", FOX_TR_NOOP("Tajikistan"), 1, "itu-region1", "tg"},
    {"TZ", FOX_TR_NOOP("Tanzania"), 1, "itu-region1", "sw"},
    {"TH", FOX_TR_NOOP("Thailand"), 3, "itu-region3", "th"},
    {"TL", FOX_TR_NOOP("Timor-Leste"), 3, "itu-region3", "pt"},
    {"TG", FOX_TR_NOOP("Togo"), 1, "itu-region1", "fr"},
    {"TK", FOX_TR_NOOP("Tokelau"), 3, "itu-region3", "en"},
    {"TO", FOX_TR_NOOP("Tonga"), 3, "itu-region3", "en"},
    {"TT", FOX_TR_NOOP("Trinidad and Tobago"), 2, "itu-region2", "en"},
    {"TN", FOX_TR_NOOP("Tunisia"), 1, "itu-region1", "ar"},
    {"TM", FOX_TR_NOOP("Turkmenistan"), 1, "itu-region1", "tk"},
    {"TC", FOX_TR_NOOP("Turks and Caicos Islands"), 2, "itu-region2", "en"},
    {"TV", FOX_TR_NOOP("Tuvalu"), 3, "itu-region3", "en"},
    {"TR", FOX_TR_NOOP("Türkiye"), 1, "itu-region1", "tr"},
    {"VI", FOX_TR_NOOP("US Virgin Islands"), 2, "itu-region2", "en"},
    {"UG", FOX_TR_NOOP("Uganda"), 1, "itu-region1", "en"},
    {"UA", FOX_TR_NOOP("Ukraine"), 1, "itu-region1", "uk"},
    {"AE", FOX_TR_NOOP("United Arab Emirates"), 1, "itu-region1", "ar"},
    {"GB", FOX_TR_NOOP("United Kingdom"), 1, "uk", "en"},
    {"US", FOX_TR_NOOP("United States"), 2, "us", "en"},
    {"UM", FOX_TR_NOOP("United States Minor Outlying Islands"), 3, "itu-region3", "en"},  // straddles C - see above
    {"UY", FOX_TR_NOOP("Uruguay"), 2, "itu-region2", "es"},
    {"UZ", FOX_TR_NOOP("Uzbekistan"), 1, "itu-region1", "uz"},
    {"VU", FOX_TR_NOOP("Vanuatu"), 3, "itu-region3", "bi"},
    {"VA", FOX_TR_NOOP("Vatican City"), 1, "itu-region1", "it"},
    {"VE", FOX_TR_NOOP("Venezuela"), 2, "itu-region2", "es"},
    {"VN", FOX_TR_NOOP("Vietnam"), 3, "itu-region3", "vi"},
    {"WF", FOX_TR_NOOP("Wallis and Futuna"), 3, "itu-region3", "fr"},
    {"EH", FOX_TR_NOOP("Western Sahara"), 1, "itu-region1", "es"},
    {"YE", FOX_TR_NOOP("Yemen"), 1, "itu-region1", "ar"},
    {"ZM", FOX_TR_NOOP("Zambia"), 1, "itu-region1", "en"},
    {"ZW", FOX_TR_NOOP("Zimbabwe"), 1, "itu-region1", "en"},
    {"AX", FOX_TR_NOOP("Åland Islands"), 1, "itu-region1", "sv"},
};

}  // namespace

std::span<const Country> countries() {
    return std::span<const Country>(kCountries, std::size(kCountries));
}

const Country* findCountry(const std::string& code) {
    if (code.empty()) { return nullptr; }
    for (const Country& c : kCountries) {
        if (code == c.code) { return &c; }
    }
    return nullptr;
}

}  // namespace cascade::core
