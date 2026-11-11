// adif_qso.hpp - an ADIF 3.1.4 (ADI) record for one contest/logged QSO (hamops lane), and the optional ADI header. Pure text.
//
// Separate from the listening-log (SWL) writer on branch feat/signal-blocks (src/core/adif.hpp): that one is not in this lane's base, writes
// listener reports and has a different record shape. This file writes the QSO fields a logger exchanges: CALL, QSO_DATE, TIME_ON, BAND, FREQ,
// MODE (+SUBMODE), RST_SENT/RST_RCVD, STX/SRX, STX_STRING/SRX_STRING, CONTEST_ID, STATION_CALLSIGN. The ADIF broadcaster (net/adif_broadcast.hpp)
// sends any ADI record text, so records from either writer can go out.
//
// SOURCE: ADIF 3.1.4, https://adif.org/314/ADIF_314.htm (opened 2026-11-11): data specifier <F:L:T>D with L the data length; a record is data
// specifiers then <EOR>; header optional, ends <EOH>, may carry <ADIF_VER:5>3.1.4; no field name more than once per record; QSO_DATE YYYYMMDD,
// TIME_ON HHMM or HHMMSS (UTC); FREQ in megahertz; MODE enumeration with SUBMODE (SSB: LSB, USB); STX / SRX contest serial numbers (integer >= 0);
// STX_STRING / SRX_STRING "Contest QSO transmitted/received information" (the page says to use Cabrillo format for information without a
// dedicated field); CONTEST_ID "use enumeration values for interoperability" (this writer passes the Cabrillo contest id through; the enumeration
// itself was not read: UNVERIFIED that every id here is an ADIF enumeration value).
// ADI is ASCII: the IntlString field types are not used, and non-ASCII bytes in text become '?' so length == bytes.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "cabrillo.hpp"
#include "qso_log.hpp"

namespace cascade::core::ham {

namespace adif_detail {
inline std::string asciiOnly(const std::string& s, std::size_t maxLen = 200) {
    std::string r;
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        r.push_back((u >= 32 && u < 127) ? c : (u == '\t' || u == '\n' || u == '\r' ? ' ' : '?'));
        if (r.size() >= maxLen) { break; }
    }
    return r;
}
inline void field(std::string& out, const char* name, const std::string& value) {
    if (value.empty()) { return; }
    out += '<';
    out += name;
    out += ':';
    out += std::to_string(value.size());
    out += '>';
    out += value;
    out += ' ';
}
}  // namespace adif_detail

// The ADI file header. `programVersion` is plain text.
inline std::string adifHeader(const std::string& programId, const std::string& programVersion) {
    std::string h = "FoxSDR ADIF export\r\n";
    adif_detail::field(h, "ADIF_VER", "3.1.4");
    adif_detail::field(h, "PROGRAMID", adif_detail::asciiOnly(programId, 40));
    adif_detail::field(h, "PROGRAMVERSION", adif_detail::asciiOnly(programVersion, 40));
    h += "<EOH>\r\n";
    return h;
}

struct AdifQsoOptions {
    std::string contestId;     // CONTEST_ID, empty = omit
    const ContestDef* contest = nullptr;   // when set, sent/received tokens become STX/SRX and STX_STRING/SRX_STRING
};

// One ADI record, "" when the QSO cannot be written (bad call, no time, no usable frequency). The record ends with <EOR> and a CR LF.
inline std::string adifQsoRecord(const Qso& q, const AdifQsoOptions& o = AdifQsoOptions()) {
    using adif_detail::field;
    std::string call;
    if (!normaliseCall(q.call, call) || q.unixTime < 0 || !(q.freqKHz > 0.0)) { return std::string(); }
    std::string mine;
    const bool haveMine = !q.myCall.empty() && normaliseCall(q.myCall, mine);
    const UtcParts t = utcFromUnix(q.unixTime);
    char buf[32];
    std::string rec;
    if (haveMine) { field(rec, "STATION_CALLSIGN", mine); }
    field(rec, "CALL", call);
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d", t.year, t.month, t.day);
    field(rec, "QSO_DATE", buf);
    std::snprintf(buf, sizeof(buf), "%02d%02d%02d", t.hour, t.minute, t.second);
    field(rec, "TIME_ON", buf);
    const double mhz = q.freqKHz / 1000.0;
    field(rec, "BAND", adifBand(mhz));
    std::snprintf(buf, sizeof(buf), "%.6f", mhz);
    field(rec, "FREQ", buf);
    const std::string m = upperAscii(q.mode);
    if (m == "USB" || m == "LSB") { field(rec, "MODE", "SSB"); field(rec, "SUBMODE", m); }
    else if (m == "SSB" || m == "CW" || m == "AM" || m == "FM" || m == "RTTY") { field(rec, "MODE", m); }
    else if (m == "NFM") { field(rec, "MODE", "FM"); }
    else if (!m.empty()) { field(rec, "MODE", adif_detail::asciiOnly(m, 20)); }
    if (o.contest != nullptr && q.sent.size() == o.contest->exch.size() && q.rcvd.size() == o.contest->exch.size()) {
        std::string sentStr, rcvdStr;
        for (std::size_t i = 0; i < o.contest->exch.size(); ++i) {
            const std::string key = o.contest->exch[i].key;
            const std::string a = adif_detail::asciiOnly(upperAscii(q.sent[i]), 12);
            const std::string b = adif_detail::asciiOnly(upperAscii(q.rcvd[i]), 12);
            if (key == "RST") { field(rec, "RST_SENT", a); field(rec, "RST_RCVD", b); continue; }
            if (key == "NR") {
                if (a.find_first_not_of("0123456789") == std::string::npos && b.find_first_not_of("0123456789") == std::string::npos) {
                    field(rec, "STX", std::to_string(std::atoi(a.c_str())));
                    field(rec, "SRX", std::to_string(std::atoi(b.c_str())));
                }
            }
            sentStr += (sentStr.empty() ? "" : " ") + a;
            rcvdStr += (rcvdStr.empty() ? "" : " ") + b;
        }
        field(rec, "STX_STRING", sentStr);
        field(rec, "SRX_STRING", rcvdStr);
    }
    if (!o.contestId.empty()) { field(rec, "CONTEST_ID", adif_detail::asciiOnly(o.contestId, 40)); }
    rec += "<EOR>\r\n";
    return rec;
}

}  // namespace cascade::core::ham
