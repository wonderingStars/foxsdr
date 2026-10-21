// See os_crash_record.hpp for what this is, what was measured and what is kept. This
// file is the half that is the same on every platform: the validation, the matching,
// the query text and the bounded lookup. The Windows event log itself is read in
// os_crash_record_win.cpp; elsewhere the source is empty and nothing is ever found.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/os_crash_record.hpp"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>

namespace cascade::core {

namespace {

bool isAlnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool isHexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

unsigned hexValue(char c) {
    if (c >= '0' && c <= '9') { return static_cast<unsigned>(c - '0'); }
    if (c >= 'a' && c <= 'f') { return static_cast<unsigned>(c - 'a') + 10u; }
    return static_cast<unsigned>(c - 'A') + 10u;
}

bool startsWithNoCase(const std::string& s, const char* prefix) {
    for (std::size_t i = 0; prefix[i] != '\0'; ++i) {
        if (i >= s.size()) { return false; }
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[i])));
        if (a != b) { return false; }
    }
    return true;
}

// "0x" (or "0X") optionally or necessarily, then 1..maxDigits hex digits and NOTHING
// else: no sign, no space, no second prefix. False leaves `out` alone.
enum class Prefix { Required, Optional };
bool parseHexNumber(const std::string& text, Prefix prefix, std::size_t maxDigits,
                    std::uint64_t& out) {
    std::size_t at = 0;
    const bool hasPrefix = text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    if (hasPrefix) {
        at = 2;
    } else if (prefix == Prefix::Required) {
        return false;
    }
    const std::size_t digits = text.size() - at;
    if (digits == 0 || digits > maxDigits) { return false; }
    std::uint64_t v = 0;
    for (std::size_t i = at; i < text.size(); ++i) {
        if (!isHexDigit(text[i])) { return false; }
        v = (v << 4) | hexValue(text[i]);
    }
    out = v;
    return true;
}

// The five NAMED fields of the event's data, and nothing else: the scan skips every
// other element without reading its value into anything.
struct Field {
    const char* name;
    std::string value;
    int seen = 0;           // how many elements carried this name
    bool malformed = false; // one of them was not "<Data Name='x'>text</Data>"
};

// Walks the <EventData> block's <Data Name='...'>...</Data> elements. False when
// there is no whole <EventData> block at all.
bool collectFields(const std::string& xml, Field* fields, std::size_t count) {
    static const std::string kOpen = "<EventData>";
    static const std::string kClose = "</EventData>";
    std::size_t pos = xml.find(kOpen);
    if (pos == std::string::npos) { return false; }
    pos += kOpen.size();
    const std::size_t end = xml.find(kClose, pos);
    if (end == std::string::npos) { return false; }
    while (pos < end) {
        const std::size_t open = xml.find("<Data ", pos);
        if (open == std::string::npos || open >= end) { break; }
        std::size_t p = open + 6;
        char quote = '\0';
        if (xml.compare(p, 6, "Name='") == 0) {
            quote = '\'';
        } else if (xml.compare(p, 6, "Name=\"") == 0) {
            quote = '"';
        } else {
            pos = p;  // an element with no Name is no field of ours
            continue;
        }
        p += 6;
        const std::size_t nameEnd = xml.find(quote, p);
        if (nameEnd == std::string::npos || nameEnd >= end) { break; }
        const std::string name = xml.substr(p, nameEnd - p);
        p = nameEnd + 1;
        std::string value;
        bool whole = false;
        if (p < end && xml[p] == '>') {
            // The text runs to the next '<', which must be this element's own close.
            const std::size_t lt = xml.find('<', p + 1);
            if (lt != std::string::npos && lt < end && xml.compare(lt, 7, "</Data>") == 0) {
                value = xml.substr(p + 1, lt - (p + 1));
                whole = true;
                pos = lt + 7;
            } else {
                pos = p + 1;
            }
        } else if (xml.compare(p, 2, "/>") == 0) {
            whole = true;  // an empty element
            pos = p + 2;
        } else {
            pos = p;
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (name != fields[i].name) { continue; }
            ++fields[i].seen;
            if (whole) {
                fields[i].value = std::move(value);
            } else {
                fields[i].malformed = true;
            }
            break;
        }
    }
    return true;
}

std::uint64_t fileTimeNow() {
    // 100 ns ticks since 1601: the system clock's count since 1970 plus the gap.
    using Ticks = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
    constexpr std::int64_t kUnixEpochTicks = 116444736000000000ll;
    const auto since = std::chrono::duration_cast<Ticks>(
        std::chrono::system_clock::now().time_since_epoch());
    return static_cast<std::uint64_t>(since.count() + kUnixEpochTicks);
}

}  // namespace

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------
bool osCrashModuleNameOk(const std::string& s) {
    if (s.empty() || s.size() > kOsCrashModuleNameMax) { return false; }
    // First: a letter, a digit or an underscore. Not a dot (".", "..", ".hidden"), not a dash.
    if (!isAlnum(s[0]) && s[0] != '_') { return false; }
    for (const char c : s) {
        const bool ok = isAlnum(c) || c == '.' || c == '_' || c == '-' || c == '+';
        if (!ok) { return false; }  // a separator, a space, markup, a control or any byte >= 0x80
    }
    // What Windows writes when it could not name the module: nothing worth keeping.
    if (s.size() == 7 && startsWithNoCase(s, "unknown")) { return false; }
    if (startsWithNoCase(s, "StackHash_")) { return false; }
    return true;
}

bool osCrashOffsetOk(const std::string& hexDigits, std::uint64_t& offset) {
    if (hexDigits.empty() || hexDigits.size() > 16) { return false; }
    std::uint64_t v = 0;
    for (const char c : hexDigits) {
        if (!isHexDigit(c)) { return false; }
        v = (v << 4) | hexValue(c);
    }
    // Inside a module the offset is bounded by its image size, a 32-bit field of the PE
    // header. A larger number is a raw address (no module held the fault).
    if (v > 0xFFFFFFFFull) { return false; }
    offset = v;
    return true;
}

// ---------------------------------------------------------------------------
// The parser
// ---------------------------------------------------------------------------
bool parseApplicationErrorEvent(const std::string& xml, const OsCrashQuery& q, OsCrashLocation& out) {
    // Nothing known about the death: nothing can be matched, and a process id alone is
    // not enough (ids are reused).
    if (q.pid == 0 || q.startFileTime == 0) { return false; }

    // THE PROVIDER AND THE ID, in the <System> block ahead of the data. The query asks
    // for exactly these; a source that hands over anything else is not trusted.
    const std::size_t data = xml.find("<EventData>");
    if (data == std::string::npos) { return false; }
    const std::string head = xml.substr(0, data);
    if (head.find("<Provider Name='Application Error'") == std::string::npos &&
        head.find("<Provider Name=\"Application Error\"") == std::string::npos) {
        return false;
    }
    if (head.find("<EventID>1000</EventID>") == std::string::npos) { return false; }

    Field fields[] = {{"ModuleName", {}}, {"ExceptionCode", {}}, {"FaultingOffset", {}},
                      {"ProcessId", {}},  {"ProcessCreationTime", {}}};
    if (!collectFields(xml, fields, sizeof(fields) / sizeof(fields[0]))) { return false; }
    for (const Field& f : fields) {
        if (f.seen != 1 || f.malformed) { return false; }  // missing, twice, or not whole
    }
    const std::string& module = fields[0].value;
    std::uint64_t code = 0, offset = 0, pid = 0, created = 0;

    // WHICH DEATH: the process id AND the creation time AND the code the process ended with.
    if (!parseHexNumber(fields[3].value, Prefix::Required, 8, pid) || pid != q.pid) { return false; }
    if (!parseHexNumber(fields[4].value, Prefix::Required, 16, created) ||
        created != q.startFileTime) {
        return false;
    }
    if (!parseHexNumber(fields[1].value, Prefix::Optional, 8, code) ||
        code != static_cast<std::uint64_t>(q.exitCode)) {
        return false;
    }
    // WHAT IS KEPT: validated, or dropped.
    if (!osCrashModuleNameOk(module)) { return false; }
    if (!osCrashOffsetOk(fields[2].value, offset)) { return false; }

    out.module = module;
    out.offset = offset;
    return true;
}

// ---------------------------------------------------------------------------
// The query
// ---------------------------------------------------------------------------
std::string osCrashEventXPath(std::uint64_t windowMs) {
    return "*[System[Provider[@Name='Application Error'] and EventID=1000 and "
           "TimeCreated[timediff(@SystemTime) <= " +
           std::to_string(windowMs) + "]]]";
}

std::uint64_t osCrashWindowMs(const OsCrashQuery& q, std::uint64_t nowFileTime) {
    constexpr std::uint64_t kTicksPerMs = 10'000ull;
    constexpr std::uint64_t kSlackMs = 1000ull;
    const std::uint64_t end = q.endFileTime != 0 ? q.endFileTime : nowFileTime;
    const std::uint64_t maxTicks = kOsCrashWindowMaxMs * kTicksPerMs;
    std::uint64_t from = end > maxTicks ? end - maxTicks : 0ull;
    if (q.startFileTime > from) { from = q.startFileTime; }
    const std::uint64_t backMs = nowFileTime > from ? (nowFileTime - from) / kTicksPerMs : 0ull;
    return backMs + kSlackMs;
}

// ---------------------------------------------------------------------------
// The bounded lookup
// ---------------------------------------------------------------------------
namespace {

// What the worker thread and the caller share. Held by both through a shared_ptr, so a
// worker that outlives the caller (the source never answered) has something to write to.
struct LookupState {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    bool found = false;
    OsCrashLocation location;
    int attempts = 0;
    std::atomic<bool> cancel{false};
};

}  // namespace

OsCrashLookup findOsCrashRecord(const OsCrashQuery& q, std::chrono::milliseconds budget,
                                const OsEventSource& source,
                                std::chrono::milliseconds retryInterval) {
    OsCrashLookup result;
    if (q.pid == 0 || q.startFileTime == 0 || budget.count() <= 0) { return result; }
    const OsEventSource src = source ? source : windowsApplicationLogSource();
    if (!src) { return result; }  // no such log on this platform

    const std::string xpath = osCrashEventXPath(osCrashWindowMs(q, fileTimeNow()));
    const auto state = std::make_shared<LookupState>();
    try {
        std::thread([state, src, xpath, q, retryInterval] {
            try {
                for (;;) {
                    int examined = 0;
                    {
                        std::lock_guard<std::mutex> lk(state->m);
                        ++state->attempts;
                    }
                    src(xpath,
                        [&](const std::string& eventXml) -> bool {
                            ++examined;
                            OsCrashLocation l;
                            if (parseApplicationErrorEvent(eventXml, q, l)) {
                                std::lock_guard<std::mutex> lk(state->m);
                                state->found = true;
                                state->location = l;
                                return true;
                            }
                            return examined >= kOsCrashMaxEventsExamined;
                        },
                        state->cancel);
                    std::unique_lock<std::mutex> lk(state->m);
                    if (state->found || retryInterval.count() <= 0 || state->cancel.load()) { break; }
                    // Not there (yet): ask again after the interval, or stop as soon as the
                    // caller has given up.
                    if (state->cv.wait_for(lk, retryInterval, [&] { return state->cancel.load(); })) { break; }
                }
            } catch (...) {
                // A log that cannot be read is "not known".
            }
            {
                std::lock_guard<std::mutex> lk(state->m);
                state->done = true;
            }
            state->cv.notify_all();
        }).detach();
    } catch (...) {
        return result;  // no thread: not known
    }

    std::unique_lock<std::mutex> lk(state->m);
    if (!state->cv.wait_for(lk, budget, [&] { return state->done; })) {
        // The source has not finished. It is left behind (the process is about to end,
        // and the worker holds nothing but its own shared state); cancel is raised for a
        // source that listens, and for the worker's own wait between attempts.
        state->cancel.store(true);
        result.timedOut = true;
        result.attempts = state->attempts;
        // The answer may have arrived in the instant between: a found event is a found event.
        if (state->found) {
            result.found = true;
            result.location = state->location;
        }
        lk.unlock();
        state->cv.notify_all();
        return result;
    }
    result.found = state->found;
    result.attempts = state->attempts;
    if (result.found) { result.location = state->location; }
    return result;
}

#if !defined(_WIN32)
// Windows' event log exists on Windows only.
OsEventSource windowsApplicationLogSource() { return OsEventSource(); }
#endif

}  // namespace cascade::core
