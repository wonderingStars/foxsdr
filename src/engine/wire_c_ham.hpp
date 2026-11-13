// wire_c_ham.hpp - the QSO LOG of wiring lane C (rows H1 Cabrillo export, H3 ADIF export; the models are core/qso_log, cabrillo, adif_qso). The engine had no QSO store: this is one,
// and only a LOG - FoxSDR is a receiver and never sends anything, so nothing here keys, calls or answers: a QSO is a contact the PERSON typed in (a listening log, a contest
// log kept while listening, a log they made on their own transmitter and want to export).
//
// PERSONAL DATA, ON PURPOSE AND ONLY HERE. The callsigns and exchange values are what the person typed into their own log; they live in qso-log.json beside the settings (the
// file is the person's log and is exported only to a path they name). They are never a setting, never in the usage report, a crash report, the diagnostics log or a fact (the
// facts carry COUNTS and the model's problem sentences, which name a QSO by number only).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/cabrillo.hpp"
#include "core/qso_log.hpp"

namespace cascade::engine::wirec {

inline constexpr std::size_t kMaxQsos = 20000;

class QsoBook {
public:
    explicit QsoBook(std::filesystem::path file) : file_(std::move(file)) { load(); }

    const std::string& error() const { return error_; }
    std::size_t size() const { return qsos_.size(); }
    const std::vector<cascade::core::ham::Qso>& qsos() const { return qsos_; }
    const cascade::core::ham::CabrilloHeader& header() const { return header_; }

    // One QSO as compact JSON: {"t":unix (0 or absent = now),"f":kHz,"m":"CW","c":"K1ABC","me":"G4XYZ","s":["599","12"],"r":["599","14"]}. False with `why`.
    bool add(const std::string& json, std::int64_t nowUnix, std::string& why);
    bool remove(std::size_t oneBasedIndex, std::string& why);
    bool clear();
    // The Cabrillo header as JSON (any of contest, callsign, location, assisted, band, mode, operator, overlay, power, station, time, transmitter, score, club, email, grid, name,
    // address[], operators[], soapbox[]); text values are what the person typed, cut and cleaned by the model when written.
    bool setHeader(const std::string& json, std::string& why);

    // Cabrillo: writes the file; `problems` carries the model's sentences (a QSO is named by its number). False when nothing was written.
    bool exportCabrillo(const std::string& path, std::string& problems, int& lines, int& skipped, std::string& why) const;
    // ADIF 3: one header and one record per QSO that can be written; `skipped` counts the rest.
    bool exportAdif(const std::string& path, int& written, int& skipped, std::string& why) const;

private:
    void load();
    bool save();

    std::filesystem::path file_;
    std::vector<cascade::core::ham::Qso> qsos_;
    cascade::core::ham::CabrilloHeader header_;
    std::string error_;
};

}  // namespace cascade::engine::wirec
