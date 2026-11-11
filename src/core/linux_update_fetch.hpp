// linux_update_fetch.hpp - the ranged download behind UpdateHost::fetch, written once as a template
// over the cpp-httplib client type: the Linux host instantiates it with httplib::SSLClient
// (https, certificate checking left on); the test instantiates it with the plain httplib::Client
// against a stand-in server on 127.0.0.1, which is how resume, restart and cap are proven on Windows.
//
// THE CALLER INCLUDES <httplib.h> FIRST (with CPPHTTPLIB_OPENSSL_SUPPORT where it wants SSLClient).
//
// RULES. No redirect is followed (the client's default is off here and the caller must not turn it on).
// `Range: bytes=N-` is sent when resuming. A 206 whose Content-Range begins at N appends; a 200 means
// the server ignored the range, so the file restarts from byte 0 (resumed=false); a 416 deletes the
// partial (it cannot be right) and fails so the next attempt starts clean; any other status fails.
// More than maxBytes in all stops the transfer. `cancel` is polled between chunks.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_LINUX_UPDATE_FETCH_HPP
#define CASCADE_CORE_LINUX_UPDATE_FETCH_HPP

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/linux_update.hpp"

namespace cascade::core::linuxupdate {

template <class Client>
FetchResult rangeFetch(Client& cli, const std::string& target, const std::string& partialPath,
                       std::uint64_t resumeFrom, std::uint64_t maxBytes, std::atomic<bool>* cancel) {
    FetchResult fr;
    httplib::Headers headers{{"User-Agent", "foxsdr-linux-updater/1.0"}};
    if (resumeFrom > 0) { headers.emplace("Range", "bytes=" + std::to_string(resumeFrom) + "-"); }

    std::ofstream out;
    std::uint64_t total = 0;
    bool accepted = false, overLimit = false, writeFailed = false, cancelled = false;
    std::uint64_t status = 0;

    auto onResponse = [&](const httplib::Response& r) {
        status = static_cast<std::uint64_t>(r.status);
        if (r.status == 206 && resumeFrom > 0) {
            const std::string cr = r.get_header_value("Content-Range");  // "bytes N-M/T"
            const std::string want = "bytes " + std::to_string(resumeFrom) + "-";
            if (cr.rfind(want, 0) != 0) { return false; }  // not the bytes we asked for
            out.open(partialPath, std::ios::binary | std::ios::app);
            total = resumeFrom;
            fr.resumed = true;
            accepted = true;
        } else if (r.status == 200) {
            out.open(partialPath, std::ios::binary | std::ios::trunc);  // the whole file again
            total = 0;
            fr.resumed = false;
            accepted = true;
        } else {
            return true;  // body ignored; the status is judged after the call
        }
        const std::string len = r.get_header_value("Content-Length");
        if (!len.empty() && total + std::strtoull(len.c_str(), nullptr, 10) > maxBytes) {
            overLimit = true;
            return false;
        }
        if (!out) { writeFailed = true; return false; }
        return true;
    };
    auto onBody = [&](const char* data, std::size_t n) {
        if (!accepted) { return true; }
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) { cancelled = true; return false; }
        total += n;
        if (total > maxBytes) { overLimit = true; return false; }
        out.write(data, static_cast<std::streamsize>(n));
        if (!out) { writeFailed = true; return false; }
        return true;
    };

    httplib::Result res = cli.Get(target, headers, onResponse, onBody);
    if (out.is_open()) { out.close(); }
    if (cancelled || (cancel != nullptr && cancel->load(std::memory_order_relaxed))) {
        fr.status = FetchResult::Status::Cancelled;
        return fr;
    }
    if (overLimit) { fr.error = "refusing a download larger than " + std::to_string(maxBytes) + " bytes"; return fr; }
    if (writeFailed) { fr.error = "could not write the downloaded data"; return fr; }
    if (!res) { fr.error = "request failed: " + httplib::to_string(res.error()); return fr; }
    if (status == 416) {
        std::error_code ec;
        std::filesystem::remove(partialPath, ec);  // the partial cannot be right: start clean next time
        fr.error = "the server refused the resume range; the partial file was discarded";
        return fr;
    }
    if (!accepted) { fr.error = "the server answered " + std::to_string(status); return fr; }
    std::error_code ec;
    fr.totalBytes = std::filesystem::file_size(partialPath, ec);
    fr.status = FetchResult::Status::Ok;
    return fr;
}

}  // namespace cascade::core::linuxupdate

#endif  // CASCADE_CORE_LINUX_UPDATE_FETCH_HPP
