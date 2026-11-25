// consent_file.hpp - THE CONSENT FIELDS OF config.json, AS TWO RUNNING COPIES SHARE THEM (0.99.115, brief 83, review 4 item D1).
//
// THE RULE. Rule R says nothing noted while a data switch was OFF is ever sent. Two copies of the program on one profile broke it: a switch turned OFF in one never reached the other,
// which went on collecting and sending, and whose next save wrote the switch back ON with the old identifier. The answer is small and has four parts:
//
//   1. A copy that does not hold the settings lock (a second or later copy) is the SECOND role: it keeps both data families off for its whole run (the engine's side of that is in
//      engine_consent.cpp) and it never writes the consent fields (below) from what it holds in memory.
//   2. NO copy writes the consent fields from its memory as part of an ordinary save. The writer's worker, under one process-wide mutex, reads what is on disk NOW and lays the disk's
//      consent fields over the document it is about to write (overlay): OFF on disk always wins over ON in memory, and an identifier that a click deleted cannot come back.
//   3. The consent fields are written ONLY by a click, at once, as a MERGE into what is on disk (read, change those keys, write: mergeKeys), under the same mutex.
//   4. The copy that holds the lock watches the file (Watch: its time and size first, the fields only when those moved), adopts a change at once - an OFF runs its own OFF edge - and
//      every send is checked against the disk first (sendVetoed, asked from postObserverVetoes, the one place a report's bytes are handed to the network).
//
// WHAT IS A CONSENT FIELD: the Usage switch, the install identifier and the counters that live under it (usageKeys), the pending usage record, the Diagnostics switch, the update-check
// switch (switchKeys), and (second role only) the beta tester's running report and its retry queue, which belong to the Usage family.
//
// WHAT THIS DOES NOT DO: there is no lock BETWEEN processes around a write, so a click in one copy that lands in the few milliseconds between another copy's read and its atomic
// replace can be lost. A holder adopts whatever the disk holds at its next look, so a lost OFF is noticed only by the person; the copy without the lock re-checks its own click a few
// times after it (engine_consent.cpp) and writes it again if it was lost.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_CONSENT_FILE_HPP
#define CASCADE_CORE_CONSENT_FILE_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace cascade::core::consent {

struct Fields {
    bool ok = false;               // the file was there and could be read
    bool usageEnabled = false;
    std::string installId;
    bool diagnostics = true;
    bool updateCheck = true;
};

// The keys of the Usage family that a click on the Usage switch writes (and a copy without the lock leaves to the disk).
const std::vector<std::string>& usageKeys();
// The two switches the interface's half carries.
const std::vector<std::string>& switchKeys();
// The beta tester's report, which follows the Usage switch (second role only).
const std::vector<std::string>& testerKeys();

// One mutex for every read-modify-write and every guarded write of the file in this process.
std::mutex& ioMutex();

// The fields as the disk holds them now (ConfigStore::load: the file is not changed). false when the file is missing or damaged.
bool readFields(const std::string& path, Fields& out);

enum class Role { Holder, Second };

// `text` is the document an ordinary save is about to write (ConfigStore::serialize). Returns it with the disk's consent fields laid over it:
//   Holder: telemetryEnabled, telemetryInstallId, diagnosticsEnabled, updateCheckEnabled come from the disk; telemetryPending is kept only when the disk says Usage is ON under the
//           same identifier the document carries, else it is emptied (OFF on disk wins, and a record built under an identifier a click deleted is never written back).
//   Second: every usageKeys, switchKeys and testerKeys key comes from the disk.
// A key the disk does not have is left as the document has it. A file that cannot be read leaves the document as it is.
std::string overlay(const std::string& text, const std::string& path, Role role);

// overlay + ConfigStore::writeFile, under ioMutex. The writer's worker calls this.
bool writeGuarded(const std::string& path, const std::string& text, Role role, std::string& error);

// The function the settings writer's worker runs (core/config_writer.hpp Writer): ConfigStore::writeFile while `role` holds -1 (a hermetic run), else writeGuarded as the holder (0) or as a
// second copy (1). The role is shared by pointer because a write abandoned at the quit can outlive the service that made it.
std::function<bool(const std::string& path, const std::string& text, std::string& error)> guardedWriter(std::shared_ptr<std::atomic<int>> role);

// A click's write: load the file, copy `keys` from the document `sourceText` into it, write it back (ConfigStore::serialize + writeFile), under ioMutex. A file that is not there is
// written from `sourceText` whole. A damaged file is not written over (false, `error` says so).
bool mergeKeys(const std::string& path, const std::string& sourceText, const std::vector<std::string>& keys, std::string& error);

// Thread-safe: the file's time and size are looked at first; the fields are parsed again only when those moved (or `force`).
class Watch {
public:
    explicit Watch(std::string path) : path_(std::move(path)) {}
    // Fills `out` with the latest fields. Returns true when they were parsed afresh by this call (the file moved since the last call).
    bool read(Fields& out, bool force = false);

private:
    std::string path_;
    std::mutex m_;
    bool have_ = false;
    std::uint64_t size_ = 0;
    std::int64_t time_ = 0;
    Fields cached_;
};

// THE SEND CHECK. The engine that holds the lock installs one; postObserverVetoes (core/crash_upload.cpp) asks it for every destination, so a report is refused when the disk says its
// switch is OFF whatever the memory says. `allow` returns false to refuse. One owner at a time (the last installed); clearSendHook(owner) removes it only if it is still the owner's.
void setSendHook(const void* owner, std::function<bool(const std::string& url)> allow);
void clearSendHook(const void* owner);
bool sendVetoed(const std::string& url);

}  // namespace cascade::core::consent

#endif  // CASCADE_CORE_CONSENT_FILE_HPP
