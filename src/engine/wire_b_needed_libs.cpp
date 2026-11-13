// wire_b_needed_libs.cpp - the JSON of the one needed-library table. See wire_b_needed_libs.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/wire_b_needed_libs.hpp"

namespace cascade::engine::wireb {

namespace nl = cascade::source::needed;

Json checkToJson(const nl::Check& c) {
    Json j = Json::object();
    j["state"] = nl::stateWord(c.state);
    j["sentence"] = c.sentence;
    j["looked_in"] = c.lookedIn;
    j["found_in"] = c.foundFolder;
    j["file"] = c.fileName;
    j["version"] = c.version;
    j["missing"] = c.missing;
    j["depth"] = c.depth;
    return j;
}

Json neededToJson(const nl::Info& row, const Json& lastCheck) {
    Json j = Json::object();
    j["library_id"] = row.libraryId;
    j["source_kind"] = row.sourceKind;
    j["name"] = row.name;
    j["kind"] = nl::kindWord(row.kind);
    j["file_names"] = row.fileNames;
    j["min_version"] = row.minVersion;
    j["min_version_note"] = row.minVersion.empty() ? "no document read by this lane states one" : "";
    j["bits64"] = row.needs64Bit;
    j["why"] = row.why;
    j["publisher"] = row.publisher;
    j["official_page"] = row.pageStatus == "verified" ? row.officialAddress : std::string();   // an unverified address is shown to nobody
    j["page_status"] = row.pageStatus;
    j["install_kind"] = row.installKind;
    j["folder_note"] = row.folderNote;
    j["steps"] = row.steps;
    j["licence"] = row.licence;
    j["not_supplied"] = row.notSuppliedLine;
    j["fetch_allowed"] = row.fetchAllowed ? std::string("yes") : row.fetchNote;
    j["check"] = lastCheck;
    return j;
}

}  // namespace cascade::engine::wireb
