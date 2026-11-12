// wire_b_needed_libs.cpp - the table and the re-check of wire_b_needed_libs.hpp. See that header for the rule and for what the check does NOT say.
//
// WHAT WAS READ. The two WiNRADiO file names come from the oss-libs lane's source headers (g39ddc_api.hpp / g33ddc_api.hpp: "G39DDCAPI.dll", "G33DDCAPI.dll", read there from
// the maker's SDK documentation pages winradio.com/home/g39ddc_sdk-doc.htm and g33ddc_sdk-doc.htm, which state no licence and ask for no agreement). THIS lane read no publisher
// page: every official address is therefore unverified and is shown to nobody, no minimum version is stated, and every licence line says so.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/wire_b_needed_libs.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#include "source/soapy_modules.hpp"
#include "source/vendor_lib.hpp"

namespace cascade::engine::wireb {

namespace {

std::string lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

const char* kindWord(NeededKind k) {
    switch (k) {
        case NeededKind::VendorLibrary: return "vendor-library";
        case NeededKind::SoapyModule: return "soapy-module";
        case NeededKind::RunningProgram: return "running-program";
        case NeededKind::RemoteSoftware: return "remote-software";
        case NeededKind::UserFile: return "user-file";
    }
    return "vendor-library";
}

std::vector<NeededLibrary> buildTable() {
    std::vector<NeededLibrary> t;
    const std::string unread = "The maker's licence for the library was not read by this lane (the SDK page it was named from states none).";
    {
        NeededLibrary r;
        r.libraryId = "winradio-g39ddc";
        r.sourceKind = "g39ddc";
        r.name = "WiNRADiO G39DDC library (G39DDCAPI.dll)";
        r.fileNames = {"G39DDCAPI.dll"};
        r.why = "Lets FoxSDR open a WiNRADiO G39DDC receiver through the maker's own library.";
        r.installKind = "copy-in";
        r.folderNote = "Any folder you choose (give it to FoxSDR as lib=<folder or file>), or a folder on the PATH.";
        r.steps = {"Get the 64-bit G39DDCAPI.dll from WiNRADiO (where from: unverified, see the link registry).",
                   "Put it in a folder of your own, or in a folder that is on the PATH.",
                   "If you chose a folder of your own, give it to FoxSDR as lib=<that folder> when you open the radio.",
                   "Press Check again: FoxSDR looks for the file and reads its headers (it does not run it)."};
        r.licence = unread;
        t.push_back(r);
    }
    {
        NeededLibrary r;
        r.libraryId = "winradio-g33ddc";
        r.sourceKind = "g33ddc";
        r.name = "WiNRADiO G33DDC library (G33DDCAPI.dll)";
        r.fileNames = {"G33DDCAPI.dll"};
        r.why = "Lets FoxSDR open a WiNRADiO G33DDC receiver through the maker's own library.";
        r.installKind = "copy-in";
        r.folderNote = "Any folder you choose (give it to FoxSDR as lib=<folder or file>), or a folder on the PATH.";
        r.steps = {"Get the 64-bit G33DDCAPI.dll from WiNRADiO (where from: unverified, see the link registry).",
                   "Put it in a folder of your own, or in a folder that is on the PATH.",
                   "If you chose a folder of your own, give it to FoxSDR as lib=<that folder> when you open the radio.",
                   "Press Check again: FoxSDR looks for the file and reads its headers (it does not run it)."};
        r.licence = unread;
        t.push_back(r);
    }
    {
        NeededLibrary r;
        r.libraryId = "soapy-bladerf";
        r.name = "SoapySDR module for the bladeRF (SoapyBladeRF)";
        r.kind = NeededKind::SoapyModule;
        r.fileNames = {"bladerf"};
        r.why = "Lets the SoapySDR source list and open a Nuand bladeRF.";
        r.installKind = "installer";
        r.folderNote = "The module folder of your PothosSDR or radioconda install (FoxSDR adds it to its search path by itself).";
        r.steps = {"Install Nuand's bladeRF driver and the SoapySDR module for it (SoapyBladeRF), for example through PothosSDR or radioconda (that those carry it: unverified).",
                   "Unplug and re-plug the radio.",
                   "Press Check again: FoxSDR looks in the module folders of the installs it knows for a file with 'bladerf' in its name."};
        r.licence = "The module and the driver carry their own licences; this lane read none.";
        t.push_back(r);
    }
    {
        NeededLibrary r;
        r.libraryId = "soapy-plutosdr";
        r.name = "SoapySDR module for the ADALM-Pluto (SoapyPlutoSDR), optional";
        r.kind = NeededKind::SoapyModule;
        r.fileNames = {"pluto"};
        r.why = "Only for opening a Pluto through the SoapySDR source: FoxSDR's own Pluto source talks to the radio's network address and needs nothing installed (an AntSDR uses that source).";
        r.installKind = "installer";
        r.folderNote = "The module folder of your PothosSDR or radioconda install (FoxSDR adds it to its search path by itself).";
        r.steps = {"Install the SoapySDR module for the Pluto (SoapyPlutoSDR), for example through PothosSDR or radioconda (that those carry it: unverified).",
                   "Unplug and re-plug the radio.",
                   "Press Check again: FoxSDR looks in the module folders of the installs it knows for a file with 'pluto' in its name."};
        r.licence = "The module carries its own licence; this lane read none.";
        t.push_back(r);
    }
    {
        NeededLibrary r;
        r.libraryId = "sdrplay-sdrconnect";
        r.sourceKind = "sdrconnect";
        r.name = "SDRplay SDRconnect (the program that serves the radio)";
        r.kind = NeededKind::RunningProgram;
        r.why = "FoxSDR is a client of SDRconnect's network server: the radio is opened by SDRconnect, FoxSDR reads its I/Q.";
        r.installKind = "running program";
        r.folderNote = "Not a file FoxSDR looks for: the program must be RUNNING with its server on, on this PC or another one.";
        r.steps = {"Install SDRplay's SDRconnect (where from: unverified, see the link registry) and start it with its server enabled (how: unverified).",
                   "Give FoxSDR its address as sdrconnect=host:port (the default port FoxSDR tries, 5454, is NOT verified).",
                   "Press Check again: FoxSDR cannot see a program on disk; it can only report that it could not connect when you open the radio."};
        r.licence = "SDRplay's own terms; this lane read none.";
        t.push_back(r);
    }
    {
        NeededLibrary r;
        r.libraryId = "soapyremote-server";
        r.sourceKind = "soapyremote";
        r.name = "SoapySDR remote server (SoapySDRServer) on the machine the radio is plugged into";
        r.kind = NeededKind::RemoteSoftware;
        r.why = "FoxSDR is a client of that server and reads the radio's I/Q over the network.";
        r.installKind = "software on another machine";
        r.folderNote = "Nothing to install on this PC.";
        r.steps = {"On the machine with the radio, install SoapyRemote and run its server (how: unverified).",
                   "Give FoxSDR the server's address as soapyremote=host[:port].",
                   "Press Check again: FoxSDR cannot see another machine; it reports the refusal when you open the radio."};
        r.licence = "SoapyRemote is Boost-licensed (its LICENSE_1_0.txt was opened by the lane that wrote the client); nothing of it is in FoxSDR.";
        t.push_back(r);
    }
    {
        NeededLibrary r;
        r.libraryId = "aaronia-rtsa-suite";
        r.sourceKind = "aaronia";
        r.name = "Aaronia RTSA-Suite with its HTTP stream server block";
        r.kind = NeededKind::RunningProgram;
        r.why = "FoxSDR is a client of the RTSA-Suite stream server and reads the SPECTRAN's I/Q.";
        r.installKind = "running program";
        r.folderNote = "Not a file FoxSDR looks for: the suite must be RUNNING with the server block on (the document's example port is 54664).";
        r.steps = {"Install Aaronia's RTSA-Suite (where from: unverified, see the link registry) and start its HTTP server block (how: unverified).",
                   "Give FoxSDR its address as aaronia=host[:port]. A server that asks for a login is refused: FoxSDR stores no password.",
                   "Press Check again: FoxSDR cannot see a program on disk; it reports the refusal when you open the radio."};
        r.licence = "Aaronia's own terms; this lane read none.";
        t.push_back(r);
    }
    {
        NeededLibrary r;
        r.libraryId = "icom-icr8600-firmware";
        r.sourceKind = "icr8600";
        r.name = "Icom IC-R8600 firmware file (only when the radio waits for it)";
        r.kind = NeededKind::UserFile;
        r.why = "The radio's I/Q port can need Icom's firmware image before it streams; FoxSDR ships none and asks for the file you have.";
        r.installKind = "your own file";
        r.folderNote = "Wherever you keep it: give it as firmware=<path>.";
        r.steps = {"Get the firmware file from Icom (where from: unverified).",
                   "Give its path to FoxSDR as firmware=<path> when you open the radio.",
                   "Press Check again: FoxSDR cannot judge a firmware file by name; it reports the radio's answer when you open it."};
        r.licence = "Icom's own terms; this lane read none.";
        t.push_back(r);
    }
    return t;
}

Json answer(const std::string& state, const std::string& sentence, const std::string& lookedIn, const std::string& foundIn = std::string(), const std::string& file = std::string()) {
    Json j = Json::object();
    j["state"] = state;
    j["sentence"] = sentence;
    j["looked_in"] = lookedIn;
    j["found_in"] = foundIn;
    j["file"] = file;
    return j;
}

std::string folderOf(const std::string& path) {
    const std::size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? std::string() : path.substr(0, cut);
}

std::string fileOf(const std::string& path) {
    const std::size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

}  // namespace

const std::vector<NeededLibrary>& neededLibraries() {
    static const std::vector<NeededLibrary> table = buildTable();
    return table;
}

const NeededLibrary* findNeededLibrary(const std::string& id) {
    for (const NeededLibrary& r : neededLibraries()) { if (r.libraryId == id) { return &r; } }
    return nullptr;
}

static std::filesystem::path asPath(const std::string& p) { return std::filesystem::path(std::u8string(p.begin(), p.end())); }

NeededEnv defaultNeededEnv() {
    NeededEnv e;
    e.getenv = [](const char* name) -> std::string {
#ifdef _WIN32
        char* buf = nullptr;
        std::size_t n = 0;
        if (_dupenv_s(&buf, &n, name) != 0 || buf == nullptr) { return std::string(); }
        std::string v(buf);
        std::free(buf);
        return v;
#else
        const char* v = std::getenv(name);
        return v != nullptr ? std::string(v) : std::string();
#endif
    };
    e.isDirectory = [](const std::string& p) {
        std::error_code ec;
        return std::filesystem::is_directory(asPath(p), ec);
    };
    e.listEntries = [](const std::string& p) {
        std::vector<std::string> out;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(asPath(p), ec), end; !ec && it != end; it.increment(ec)) { { const auto u = it->path().filename().u8string(); out.emplace_back(u.begin(), u.end()); } }
        return out;
    };
    return e;
}

Json recheckNeeded(const NeededLibrary& row, const std::string& where, const NeededEnv& env) {
    switch (row.kind) {
        case NeededKind::RunningProgram:
        case NeededKind::RemoteSoftware:
        case NeededKind::UserFile:
            if (!where.empty()) { return answer("cannot-check", "There is no file to judge for this item.", ""); }
            return answer("cannot-check",
                          row.kind == NeededKind::RunningProgram ? "FoxSDR cannot see a program that is running; opening the radio says whether it could connect."
                          : row.kind == NeededKind::RemoteSoftware ? "FoxSDR cannot see software on another machine; opening the radio says whether it could connect."
                                                                      : "FoxSDR cannot judge a firmware file by name; opening the radio says what the radio answered.",
                          "");
        case NeededKind::VendorLibrary: {
            const std::string file = row.fileNames.empty() ? std::string() : row.fileNames.front();
            const std::string lookedIn = where.empty() ? std::string("the folders on the PATH") : where;
            std::string why;
            const std::string path = cascade::source::vendorlib::findLibrary(where, file, why);
            if (path.empty()) { return answer("not-found", "Not found: " + file + " is not in " + lookedIn + (why.empty() ? std::string(".") : " (" + why + ")."), lookedIn); }
            const cascade::source::vendorlib::Check ck = cascade::source::vendorlib::checkFile(path);
            if (ck.ok()) { return answer("found", file + " is there and its headers are those of a 64-bit library for this machine (it is loaded only when you open the radio; no version is read).", lookedIn, folderOf(path), fileOf(path)); }
            if (ck.verdict == cascade::source::vendorlib::Verdict::Wrong32Bit) { return answer("wrong-32-bit", ck.sentence, lookedIn, folderOf(path), fileOf(path)); }
            return answer("unusable", ck.sentence, lookedIn, folderOf(path), fileOf(path));
        }
        case NeededKind::SoapyModule: {
            if (!where.empty()) { return answer("cannot-check", "A SoapySDR module is found in the module folder of an install, not by a file you choose.", ""); }
            std::string frag = row.fileNames.empty() ? std::string() : lower(row.fileNames.front());
            std::string looked;
            for (const auto& cand : cascade::source::candidateVendorRoots(env.getenv)) {
                const std::string base = cand.second + "/lib/SoapySDR";
                if (!env.isDirectory(base)) { continue; }
                for (const std::string& m : env.listEntries(base)) {
                    if (lower(m).rfind("modules", 0) != 0) { continue; }
                    const std::string dir = base + "/" + m;
                    if (!env.isDirectory(dir)) { continue; }
                    looked += (looked.empty() ? "" : ", ") + cand.first;
                    for (const std::string& f : env.listEntries(dir)) {
                        if (lower(f).find(frag) != std::string::npos && !env.isDirectory(dir + "/" + f)) {
                            return answer("found", "A SoapySDR module for it is in the " + cand.first + " install (" + f + "); whether it loads is known when SoapySDR lists devices.", cand.first, dir, f);
                        }
                    }
                }
            }
            return answer("not-found", looked.empty() ? std::string("No PothosSDR or radioconda install with a SoapySDR module folder was found.")
                                                      : "None of the module folders of " + looked + " holds a file with '" + frag + "' in its name.",
                          looked.empty() ? std::string("the known PothosSDR and radioconda folders") : looked);
        }
    }
    return answer("cannot-check", "", "");
}

Json neededToJson(const NeededLibrary& row, const Json& lastCheck) {
    Json j = Json::object();
    j["library_id"] = row.libraryId;
    j["source_kind"] = row.sourceKind;
    j["name"] = row.name;
    j["kind"] = kindWord(row.kind);
    j["file_names"] = row.fileNames;
    j["min_version"] = row.minVersion;
    j["min_version_note"] = row.minVersion.empty() ? "no document read by this lane states one" : "";
    j["bits64"] = row.bits64;
    j["why"] = row.why;
    j["official_page"] = row.pageStatus == "verified" ? row.officialPage : std::string();   // an unverified address is shown to nobody
    j["page_status"] = row.pageStatus;
    j["install_kind"] = row.installKind;
    j["folder_note"] = row.folderNote;
    j["steps"] = row.steps;
    j["licence"] = row.licence;
    j["not_supplied"] = row.notSupplied;
    j["fetch_allowed"] = row.fetchAllowed;
    j["check"] = lastCheck;
    return j;
}

}  // namespace cascade::engine::wireb
