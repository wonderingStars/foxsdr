// wire_a_records.cpp - WIRING LANE wire-a, the files that belong to a recording: the analyser project's hash and verify (PROJECT), the attachments of a record (ATTACH) and the annotations of a SigMF
// recording (SIGMF_ANNOTATE). The ops are documented in core/wire_a_ops.hpp. Every one runs as the lane's background job (wire_a_jobs.cpp): they read or write a user's files, some of them whole recordings, and the
// engine's pass does no disk work. Their answers are in fact.job.result as JSON; their messages are words and never a path.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <random>
#include <string>

#include <nlohmann/json.hpp>

#include "core/analyser_project.hpp"
#include "core/app_commands.hpp"
#include "core/file_read.hpp"
#include "core/record_attachments.hpp"
#include "engine/engine.hpp"
#include "engine/wire_a.hpp"
#include "source/sigmf_annotations.hpp"

namespace cascade::engine {

namespace {

namespace an = cascade::core::analyser;
namespace at = cascade::core::attach;
namespace sa = cascade::source::sigmfann;
using nlohmann::json;

std::string str(const json& j, const char* key) { return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string(); }
bool number(const json& j, const char* key, double& out) {
    if (!j.contains(key) || !j[key].is_number() || !std::isfinite(j[key].get<double>())) { return false; }
    out = j[key].get<double>();
    return true;
}

const char* kindWord(at::Kind k) { return k == at::Kind::Screenshot ? "screenshot" : (k == at::Kind::Clip ? "clip" : "note"); }
const char* stateWord(at::FileState s) {
    switch (s) {
        case at::FileState::Ok: return "ok";
        case at::FileState::Missing: return "missing";
        case at::FileState::SizeChanged: return "sizeChanged";
        case at::FileState::HashChanged: return "hashChanged";
        default: return "readError";
    }
}
const char* refWord(an::RefCheck s) {
    switch (s) {
        case an::RefCheck::Match: return "match";
        case an::RefCheck::Missing: return "missing";
        case an::RefCheck::SizeDiffers: return "sizeDiffers";
        case an::RefCheck::HashDiffers: return "hashDiffers";
        case an::RefCheck::NotHashed: return "notHashed";
        case an::RefCheck::Cancelled: return "cancelled";
        default: return "readError";
    }
}

WireA::JobResult fail(const std::string& why) {
    WireA::JobResult r;
    r.message = why;
    return r;
}

// An error text of the models never repeats a user's text, but it can name a file: leave it out when it names the path given.
std::string clean(const std::string& err, const std::string& path) { return err.find(path) == std::string::npos ? err : std::string(); }

json attachList(const at::AttachmentStore& s, const std::vector<at::VerifyEntry>* verified) {
    json arr = json::array();
    for (const at::Item& i : s.items()) {
        json o = {{"id", i.id}, {"kind", kindWord(i.kind)}, {"title", i.title}, {"sizeBytes", i.sizeBytes}};
        if (i.kind == at::Kind::Note) { o["text"] = i.text; }
        if (verified != nullptr) {
            for (const at::VerifyEntry& v : *verified) {
                if (v.id == i.id) { o["state"] = stateWord(v.state); }
            }
        }
        arr.push_back(std::move(o));
    }
    return {{"items", arr}};
}
}  // namespace

WireA::Result WireA::recordsOp(const FoxCommand& c, const std::string& text, Result res) {
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { return refused(res, FOXAPI_BAD_ARGUMENT, "the text is not a JSON object"); }
    std::string why;
    const std::string action = str(j, "action");

    if (c.op == FOXAPP_OP_PROJECT) {
        if (action == "hash") {
            const std::string rec = str(j, "recording");
            if (!fullPath(rec, why)) { return refused(res, FOXAPI_BAD_ARGUMENT, why); }
            return startJob(res, "recording hash", [rec](JobCtl& ctl) -> JobResult {
                std::error_code ec;
                if (!std::filesystem::is_regular_file(std::filesystem::path(rec), ec)) { return fail("the recording is not a file that can be read"); }
                const std::uint64_t total = std::filesystem::file_size(std::filesystem::path(rec), ec);
                std::string hex, err;
                std::uint64_t bytes = 0;
                const bool ok = an::sha256File(std::filesystem::path(rec), &hex, &bytes, &err, &ctl.cancel,
                                               [&ctl, total](std::uint64_t done) { ctl.progress.store(total == 0 ? 1.0 : static_cast<double>(done) / static_cast<double>(total)); });
                JobResult r;
                r.ok = ok;
                r.message = ok ? "hashed" : (err == "cancelled" ? "cancelled" : "the recording could not be read");
                if (ok) { r.json = json{{"sha256", hex}, {"sizeBytes", bytes}}.dump(); }
                return r;
            });
        }
        if (action == "verify") {
            const std::string proj = str(j, "project"), rec = str(j, "recording");
            if (!fullPath(proj, why) || (!rec.empty() && !fullPath(rec, why))) { return refused(res, FOXAPI_BAD_ARGUMENT, why); }
            return startJob(res, "project check", [proj, rec](JobCtl& ctl) -> JobResult {
                an::Project p;
                std::string err;
                if (!an::loadFile(std::filesystem::path(proj), &p, &err)) { return fail("the project file was refused: " + clean(err, proj)); }
                const std::string where = rec.empty() ? p.recording.path : rec;
                const an::RefCheck s = an::verifyReference(p, std::filesystem::path(where), &ctl.cancel);
                JobResult r;
                r.ok = true;
                r.message = s == an::RefCheck::Match ? "this is the recording the project was made on" : "this is not the recording the project was made on";
                r.json = json{{"state", refWord(s)}, {"tags", p.tags.size()}}.dump();
                return r;
            });
        }
        return refused(res, FOXAPI_BAD_ARGUMENT, "the action is hash or verify");
    }

    if (c.op == FOXAPP_OP_ATTACH) {
        const std::string folder = str(j, "folder");
        if (!fullPath(folder, why)) { return refused(res, FOXAPI_BAD_ARGUMENT, why); }
        if (action != "add" && action != "edit" && action != "remove" && action != "verify" && action != "list") { return refused(res, FOXAPI_BAD_ARGUMENT, "the action is add, edit, remove, verify or list"); }
        const std::string kind = str(j, "kind"), path = str(j, "path"), title = str(j, "title"), body = str(j, "text");
        double id = 0.0;
        const bool hasText = j.contains("text");
        if (action == "add") {
            if (kind != "screenshot" && kind != "clip" && kind != "note") { return refused(res, FOXAPI_BAD_ARGUMENT, "the kind is screenshot, clip or note"); }
            if (kind != "note" && !fullPath(path, why)) { return refused(res, FOXAPI_BAD_ARGUMENT, why); }
        }
        if ((action == "edit" || action == "remove") && !number(j, "id", id)) { return refused(res, FOXAPI_BAD_ARGUMENT, "the id is missing"); }
        const std::int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        return startJob(res, "attachments", [folder, action, kind, path, title, body, hasText, id, nowMs](JobCtl&) -> JobResult {
            at::AttachmentStore store;
            std::string err;
            if (!store.open(std::filesystem::path(folder), &err)) { return fail(clean(err, folder).empty() ? "the record folder is not usable" : err); }
            std::vector<at::VerifyEntry> verified;
            if (action == "add") {
                const std::uint32_t nid = kind == "note" ? store.addNote(title, body, nowMs, &err)
                                                          : store.addFile(std::filesystem::path(path), kind == "screenshot" ? at::Kind::Screenshot : at::Kind::Clip, title, nowMs, &err);
                if (nid == 0) { return fail(clean(err, path).empty() ? "the attachment was refused" : err); }
            } else if (action == "edit") {
                const std::string t = body;
                if (!store.edit(static_cast<std::uint32_t>(id), title, hasText ? &t : nullptr, &err)) { return fail(err); }
            } else if (action == "remove") {
                if (!store.remove(static_cast<std::uint32_t>(id), &err)) { return fail(err); }
            } else if (action == "verify") {
                verified = store.verify();
            }
            JobResult r;
            r.ok = true;
            r.message = action == "list" ? "listed" : (action == "verify" ? "checked" : "done");
            r.json = attachList(store, action == "verify" ? &verified : nullptr).dump();
            return r;
        });
    }

    // SIGMF_ANNOTATE
    const std::string meta = str(j, "meta");
    if (!fullPath(meta, why)) { return refused(res, FOXAPI_BAD_ARGUMENT, why); }
    if (action != "add" && action != "remove" && action != "list") { return refused(res, FOXAPI_BAD_ARGUMENT, "the action is add, remove or list"); }
    double start = 0, end = 0, lo = 0, hi = 0, rate = 0, total = 0, index = 0;
    if (action == "add") {
        if (!number(j, "startSec", start) || !number(j, "endSec", end) || !number(j, "loHz", lo) || !number(j, "hiHz", hi) || !number(j, "sampleRateHz", rate)) {
            return refused(res, FOXAPI_BAD_ARGUMENT, "an add needs startSec, endSec, loHz, hiHz and sampleRateHz");
        }
        (void)number(j, "totalSamples", total);
        if (total < 0.0 || total > 9.0e15) { return refused(res, FOXAPI_OUT_OF_RANGE, "totalSamples is 0 or a sample count"); }
    }
    if (action == "remove" && (!number(j, "index", index) || index < 0.0 || index > 1.0e9)) { return refused(res, FOXAPI_BAD_ARGUMENT, "a remove needs the index of the annotation"); }
    const std::string label = str(j, "label"), comment = str(j, "comment");
    return startJob(res, "annotations", [meta, action, start, end, lo, hi, rate, total, index, label, comment](JobCtl&) -> JobResult {
        std::string err;
        const std::filesystem::path p(meta);
        if (action == "add") {
            sa::Annotation a;
            if (!sa::fromBox(start, end, lo, hi, rate, label, comment, "FoxSDR", &a)) { return fail("the box is not a time and frequency range"); }
            unsigned char b[16];
            std::random_device rd;
            for (unsigned char& x : b) { x = static_cast<unsigned char>(rd() & 0xFF); }
            a.uuid = sa::uuid4FromBytes(b);
            if (!sa::addToFile(p, a, static_cast<std::uint64_t>(total), &err)) { return fail("the annotation was refused: " + clean(err, meta)); }
        } else if (action == "remove") {
            if (!sa::removeFromFile(p, static_cast<std::size_t>(index), &err)) { return fail("the annotation was refused: " + clean(err, meta)); }
        }
        std::string text;
        std::error_code ec;
        const std::uintmax_t size = std::filesystem::file_size(p, ec);
        if (ec || size > sa::kMaxFileBytes) { return fail("the metadata file could not be read"); }
        if (cascade::core::readTextFile(p, text) != cascade::core::ReadResult::Ok) { return fail("the metadata file could not be read"); }
        std::vector<sa::Annotation> list;
        if (!sa::readAnnotations(text, &list, &err)) { return fail("the metadata file was refused: " + clean(err, meta)); }
        JobResult r;
        r.ok = true;
        r.message = action == "list" ? "listed" : "done";
        r.json = json{{"annotations", list.size()}}.dump();
        return r;
    });
}

}  // namespace cascade::engine
