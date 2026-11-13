// needed_library.cpp - the version reader, the state and kind words, the default environment, the ONE TABLE of what the user must have and the checks of its rows.
// See needed_library.hpp.
//
// WHAT WAS READ. The two WiNRADiO file names come from the oss-libs lane's source headers (g39ddc_api.hpp / g33ddc_api.hpp: "G39DDCAPI.dll", "G33DDCAPI.dll", read there from the
// maker's SDK documentation pages, which state no licence and ask for no agreement). The lane that wrote those eight rows read no publisher page: every address of them is
// unverified and shown to nobody, no minimum version is stated, and every licence line says so. The ftdi-d2xx row is the SDR-IQ bridge's (sdriq_d2xx.cpp).
//
// WHAT A CHECK MAY DO. A "Check again" never loads vendor code, EXCEPT for the one row whose bridge already has a silent load check (ftdi-d2xx: checkFtd2xx): every other
// file row reads the file's headers only (vendorlib::checkFile). Nothing is downloaded, opened as a web page, copied or written, and no Windows dialog can be raised.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/needed_library.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <vector>

#include "source/sdriq_d2xx.hpp"
#include "source/soapy_modules.hpp"
#include "source/vendor_lib.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#pragma comment(lib, "version.lib")
#endif

namespace cascade::source::needed {

const char* stateWord(State s) {
    switch (s) {
        case State::Working: return "working";
        case State::NotFound: return "not-found";
        case State::TooOld: return "too-old";
        case State::Wrong32Bit: return "wrong-32-bit";
        case State::WouldNotLoad: return "would-not-load";
        case State::NotALibrary: return "not-a-library";
        case State::CannotCheck: return "cannot-check";
    }
    return "unknown";
}

const char* kindWord(Kind k) {
    switch (k) {
        case Kind::LibraryFile: return "library-file";
        case Kind::SoapyModule: return "soapy-module";
        case Kind::RunningProgram: return "running-program";
        case Kind::RemoteSoftware: return "remote-software";
        case Kind::UserFile: return "user-file";
    }
    return "library-file";
}

std::string fileVersion(const std::string& utf8Path) {
#ifdef _WIN32
    const std::filesystem::path p{std::u8string(utf8Path.begin(), utf8Path.end())};
    const std::wstring w = p.wstring();
    DWORD handle = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(w.c_str(), &handle);
    if (size == 0 || size > 1024 * 1024) { return {}; }
    std::vector<char> buf(size);
    if (!::GetFileVersionInfoW(w.c_str(), 0, size, buf.data())) { return {}; }
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!::VerQueryValueW(buf.data(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &len) || ffi == nullptr || len < sizeof(VS_FIXEDFILEINFO)) { return {}; }
    return std::to_string(HIWORD(ffi->dwFileVersionMS)) + "." + std::to_string(LOWORD(ffi->dwFileVersionMS)) + "." +
           std::to_string(HIWORD(ffi->dwFileVersionLS)) + "." + std::to_string(LOWORD(ffi->dwFileVersionLS));
#else
    (void)utf8Path;
    return {};
#endif
}

namespace {

std::string lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

std::filesystem::path asPath(const std::string& p) { return std::filesystem::path(std::u8string(p.begin(), p.end())); }

std::string folderOf(const std::string& path) {
    const std::size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? std::string() : path.substr(0, cut);
}

std::string fileOf(const std::string& path) {
    const std::size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

Check answer(State s, const std::string& sentence, const std::string& lookedIn, const std::string& depth, const std::string& foundFolder = std::string(),
             const std::string& file = std::string()) {
    Check c;
    c.state = s;
    c.sentence = sentence;
    c.lookedIn = lookedIn;
    c.depth = depth;
    c.foundFolder = foundFolder;
    c.fileName = file;
    return c;
}

// ---- the checks of the table's rows ------------------------------------------------------------------------------------------------------------------------------

// A program that is running, software on another machine, a firmware file the user keeps: nothing on this PC to look at.
Check checkNothingToLook(const Info& row, const std::string& where, const Env&) {
    if (!where.empty()) { return answer(State::CannotCheck, "There is no file to judge for this item.", "", ""); }
    return answer(State::CannotCheck,
                  row.kind == Kind::RunningProgram ? "FoxSDR cannot see a program that is running; opening the radio says whether it could connect."
                  : row.kind == Kind::RemoteSoftware ? "FoxSDR cannot see software on another machine; opening the radio says whether it could connect."
                                                      : "FoxSDR cannot judge a firmware file by name; opening the radio says what the radio answered.",
                  "", "");
}

// A library file, HEADERS ONLY: found by name in the folder or file the user gave, or on the PATH; its headers must be those of a 64-bit library for this machine. It is
// never loaded (the loading happens when the radio is opened, and the source says then why it refused), so no version is read and "too old" / "would not load" are not
// answered: state Working here means found and well formed (depth "headers").
Check checkLibraryHeaders(const Info& row, const std::string& where, const Env&) {
    const std::string file = row.fileNames.empty() ? std::string() : row.fileNames.front();
    const std::string lookedIn = where.empty() ? std::string("the folders on the PATH") : where;
    std::string why;
    const std::string path = vendorlib::findLibrary(where, file, why);
    if (path.empty()) {
        return answer(State::NotFound, "Not found: " + file + " is not in " + lookedIn + (why.empty() ? std::string(".") : " (" + why + ")."), lookedIn, "headers");
    }
    const vendorlib::Check ck = vendorlib::checkFile(path);
    if (ck.ok()) {
        return answer(State::Working,
                      file + " is there and its headers are those of a 64-bit library for this machine (it is loaded only when you open the radio; no version is read).",
                      lookedIn, "headers", folderOf(path), fileOf(path));
    }
    if (ck.verdict == vendorlib::Verdict::Wrong32Bit) { return answer(State::Wrong32Bit, ck.sentence, lookedIn, "headers", folderOf(path), fileOf(path)); }
    return answer(State::NotALibrary, ck.sentence, lookedIn, "headers", folderOf(path), fileOf(path));
}

// A SoapySDR module: a file with a fragment in its name in the module folder of a PothosSDR or radioconda install. Lists folders and file names only.
Check checkSoapyModule(const Info& row, const std::string& where, const Env& env) {
    if (!where.empty()) { return answer(State::CannotCheck, "A SoapySDR module is found in the module folder of an install, not by a file you choose.", "", ""); }
    const std::string frag = row.fileNames.empty() ? std::string() : lower(row.fileNames.front());
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
                    return answer(State::Working, "A SoapySDR module for it is in the " + cand.first + " install (" + f + "); whether it loads is known when SoapySDR lists devices.",
                                  cand.first, "headers", dir, f);
                }
            }
        }
    }
    return answer(State::NotFound,
                  looked.empty() ? std::string("No PothosSDR or radioconda install with a SoapySDR module folder was found.")
                                 : "None of the module folders of " + looked + " holds a file with '" + frag + "' in its name.",
                  looked.empty() ? std::string("the known PothosSDR and radioconda folders") : looked, "headers");
}

std::vector<Info> buildTable() {
    std::vector<Info> t;
    const std::string unread = "The maker's licence for the library was not read by this lane (the SDK page it was named from states none).";
    {
        Info r;
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
        r.check = &checkLibraryHeaders;
        t.push_back(r);
    }
    {
        Info r;
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
        r.check = &checkLibraryHeaders;
        t.push_back(r);
    }
    {
        Info r;
        r.libraryId = "soapy-bladerf";
        r.name = "SoapySDR module for the bladeRF (SoapyBladeRF)";
        r.kind = Kind::SoapyModule;
        r.fileNames = {"bladerf"};
        r.why = "Lets the SoapySDR source list and open a Nuand bladeRF.";
        r.installKind = "installer";
        r.folderNote = "The module folder of your PothosSDR or radioconda install (FoxSDR adds it to its search path by itself).";
        r.steps = {"Install Nuand's bladeRF driver and the SoapySDR module for it (SoapyBladeRF), for example through PothosSDR or radioconda (that those carry it: unverified).",
                   "Unplug and re-plug the radio.",
                   "Press Check again: FoxSDR looks in the module folders of the installs it knows for a file with 'bladerf' in its name."};
        r.licence = "The module and the driver carry their own licences; this lane read none.";
        r.check = &checkSoapyModule;
        t.push_back(r);
    }
    {
        Info r;
        r.libraryId = "soapy-plutosdr";
        r.name = "SoapySDR module for the ADALM-Pluto (SoapyPlutoSDR), optional";
        r.kind = Kind::SoapyModule;
        r.fileNames = {"pluto"};
        r.why = "Only for opening a Pluto through the SoapySDR source: FoxSDR's own Pluto source talks to the radio's network address and needs nothing installed (an AntSDR uses that source).";
        r.installKind = "installer";
        r.folderNote = "The module folder of your PothosSDR or radioconda install (FoxSDR adds it to its search path by itself).";
        r.steps = {"Install the SoapySDR module for the Pluto (SoapyPlutoSDR), for example through PothosSDR or radioconda (that those carry it: unverified).",
                   "Unplug and re-plug the radio.",
                   "Press Check again: FoxSDR looks in the module folders of the installs it knows for a file with 'pluto' in its name."};
        r.licence = "The module carries its own licence; this lane read none.";
        r.check = &checkSoapyModule;
        t.push_back(r);
    }
    {
        Info r;
        r.libraryId = "sdrplay-sdrconnect";
        r.sourceKind = "sdrconnect";
        r.name = "SDRplay SDRconnect (the program that serves the radio)";
        r.kind = Kind::RunningProgram;
        r.why = "FoxSDR is a client of SDRconnect's network server: the radio is opened by SDRconnect, FoxSDR reads its I/Q.";
        r.installKind = "running program";
        r.folderNote = "Not a file FoxSDR looks for: the program must be RUNNING with its server on, on this PC or another one.";
        r.steps = {"Install SDRplay's SDRconnect (where from: unverified, see the link registry) and start it with its server enabled (how: unverified).",
                   "Give FoxSDR its address as sdrconnect=host:port (the default port FoxSDR tries, 5454, is NOT verified).",
                   "Press Check again: FoxSDR cannot see a program on disk; it can only report that it could not connect when you open the radio."};
        r.licence = "SDRplay's own terms; this lane read none.";
        r.check = &checkNothingToLook;
        t.push_back(r);
    }
    {
        Info r;
        r.libraryId = "soapyremote-server";
        r.sourceKind = "soapyremote";
        r.name = "SoapySDR remote server (SoapySDRServer) on the machine the radio is plugged into";
        r.kind = Kind::RemoteSoftware;
        r.why = "FoxSDR is a client of that server and reads the radio's I/Q over the network.";
        r.installKind = "software on another machine";
        r.folderNote = "Nothing to install on this PC.";
        r.steps = {"On the machine with the radio, install SoapyRemote and run its server (how: unverified).",
                   "Give FoxSDR the server's address as soapyremote=host[:port].",
                   "Press Check again: FoxSDR cannot see another machine; it reports the refusal when you open the radio."};
        r.licence = "SoapyRemote is Boost-licensed (its LICENSE_1_0.txt was opened by the lane that wrote the client); nothing of it is in FoxSDR.";
        r.check = &checkNothingToLook;
        t.push_back(r);
    }
    {
        Info r;
        r.libraryId = "aaronia-rtsa-suite";
        r.sourceKind = "aaronia";
        r.name = "Aaronia RTSA-Suite with its HTTP stream server block";
        r.kind = Kind::RunningProgram;
        r.why = "FoxSDR is a client of the RTSA-Suite stream server and reads the SPECTRAN's I/Q.";
        r.installKind = "running program";
        r.folderNote = "Not a file FoxSDR looks for: the suite must be RUNNING with the server block on (the document's example port is 54664).";
        r.steps = {"Install Aaronia's RTSA-Suite (where from: unverified, see the link registry) and start its HTTP server block (how: unverified).",
                   "Give FoxSDR its address as aaronia=host[:port]. A server that asks for a login is refused: FoxSDR stores no password.",
                   "Press Check again: FoxSDR cannot see a program on disk; it reports the refusal when you open the radio."};
        r.licence = "Aaronia's own terms; this lane read none.";
        r.check = &checkNothingToLook;
        t.push_back(r);
    }
    {
        Info r;
        r.libraryId = "icom-icr8600-firmware";
        r.sourceKind = "icr8600";
        r.name = "Icom IC-R8600 firmware file (only when the radio waits for it)";
        r.kind = Kind::UserFile;
        r.why = "The radio's I/Q port can need Icom's firmware image before it streams; FoxSDR ships none and asks for the file you have.";
        r.installKind = "your own file";
        r.folderNote = "Wherever you keep it: give it as firmware=<path>.";
        r.steps = {"Get the firmware file from Icom (where from: unverified).",
                   "Give its path to FoxSDR as firmware=<path> when you open the radio.",
                   "Press Check again: FoxSDR cannot judge a firmware file by name; it reports the radio's answer when you open it."};
        r.licence = "Icom's own terms; this lane read none.";
        r.check = &checkNothingToLook;
        t.push_back(r);
    }
    {
        Info r = ftd2xxNeededInfo();   // the SDR-IQ bridge's own row (kind, source key, page status and check are set there)
        t.push_back(r);
    }
    return t;
}

}  // namespace

Env defaultEnv() {
    Env e;
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
        for (std::filesystem::directory_iterator it(asPath(p), ec), end; !ec && it != end; it.increment(ec)) {
            const auto u = it->path().filename().u8string();
            out.emplace_back(u.begin(), u.end());
        }
        return out;
    };
    return e;
}

const std::vector<Info>& neededTable() {
    static const std::vector<Info> table = buildTable();
    return table;
}

const Info* findNeeded(const std::string& libraryId) {
    for (const Info& r : neededTable()) {
        if (r.libraryId == libraryId) { return &r; }
    }
    return nullptr;
}

Check runCheck(const Info& row, const std::string& where, const Env& env) {
    if (row.check == nullptr) { return answer(State::CannotCheck, "FoxSDR cannot check this item on this PC.", "", ""); }
    try {
        return row.check(row, where, env);
    } catch (...) {
        return answer(State::CannotCheck, "The check could not be made.", "", "");
    }
}

}  // namespace cascade::source::needed
