// radio_handoff.hpp - ONE RADIO, TWO FOXSDR WINDOWS: finding out that the copy holding a busy radio is another FoxSDR, and asking it to let go (field fix A2, 2026-11-19).
//
// WHY. The 42958e install started four more copies of FoxSDR beside one that held its dongle; every new copy failed "the radio is already in use by another program"
// and said nothing a person could act on. FoxSDR has always allowed a second window (core/tester_link.hpp, "multi-instance design"), so the answer is not to refuse the second
// copy but to tell its user WHO has the radio and offer the one thing they want: "Move it here" - the other copy lets that radio go, this one opens it.
//
// THE CHANNEL. A local named pipe, one per running window, named "<prefix>-<pid>" under \\.\pipe\, created with an access list that names the current user's SID alone and
// PIPE_REJECT_REMOTE_CLIENTS. No network socket: no firewall prompt, no other machine, no other user (a copy running as another account is not found, which is the
// "another program" answer and the right one). A request is one line, an answer is one line:
//     HOLDS <key>     ->  YES <window>      | NO
//     RELEASE <key>   ->  RELEASED          | REFUSED <code> | NOTHELD
// <key> is the radio's kind and its serial ("rtlsdr|00000001"), never sent anywhere but this pipe. A pipe's server process id comes from the operating system
// (GetNamedPipeServerProcessId), not from what the answer claims.
//
// WHAT THIS FILE IS NOT. It does not decide what a release means for the radio: the Server calls two functions the engine gives it (holds, release), on the pipe's thread. The
// engine answers them from its own control pass (engine_handoff.cpp). Off Windows every call answers "no holder" and the server does not start.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace cascade::core::handoff {

// The key a radio is known by across windows: "<kind>|<serial>". Empty when the arguments carry no serial (an "index=0" radio cannot be told from another by index, so a
// holder is never claimed for it and the person gets the "another program" sentence).
std::string radioKey(const std::string& kind, const std::string& args);
// The last four characters of a serial, for a sentence on screen (the whole serial is a stable identifier and stays out of what a person is shown beside a model name).
std::string lastFour(const std::string& serial);
// The serial inside a radioKey ("" for an empty key).
std::string serialOfKey(const std::string& key);

enum class Release {
    Released,   // the other window let the radio go
    Refused,    // it would not: code() says why ("recording")
    NotHeld,    // it no longer has that radio (it was closed meanwhile): the radio is free to open
    NoAnswer,   // nothing answered within the time: leave both as they are
};
struct ReleaseResult {
    Release result = Release::NoAnswer;
    std::string code;   // for Refused: "recording"
};

// What the engine tells the pipe's thread. Both run on the pipe thread and must come back within the stated time (release: the engine's own bounded wait).
struct Hooks {
    std::function<bool(const std::string& key)> holds;
    std::function<ReleaseResult(const std::string& key)> release;
};

class Server {
public:
    Server();
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    // Creates the pipe "<prefix>-<this pid>" and serves it on a thread of its own. `windowWord` is what HOLDS answers ("slint", "legacy"). False when the pipe cannot be made
    // (the window carries on without being movable-from) or off Windows.
    bool start(const std::string& prefix, const std::string& windowWord, Hooks hooks);
    void stop();
    bool running() const { return running_.load(); }
    unsigned requestsServed() const { return served_.load(); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{false};
    std::atomic<unsigned> served_{0};
    std::thread thread_;
};

struct Holder {
    std::string pipe;        // the full pipe path
    unsigned long pid = 0;   // from the operating system
    std::string window;      // "slint" or "legacy", as the holder said
};

// The prefix this program's windows use: "FoxSDR-handoff", or "FoxSDR-handoff-<scope>" when FOXSDR_INSTANCE_SCOPE is set together with CASCADE_CONFIG_TEST (a test run
// keeps to its own pipes, as it keeps to its own instance name).
std::string defaultPrefix();

// Asks every other window under this prefix (not this process) whether it holds `key`. The first that says YES is returned. `budgetMs` bounds the whole search.
// `selfPid` 0 means this process; a test that plays both windows in one process names another number so its own server is not skipped.
bool findHolder(const std::string& prefix, const std::string& key, int budgetMs, Holder& out, unsigned long selfPid = 0);
// Asks that window to let the radio go and waits at most `timeoutMs` for the answer.
ReleaseResult requestRelease(const Holder& holder, const std::string& key, int timeoutMs);

// THE SHEET a window shows (the Handover sheet: a title, a sentence and one or two keys). `state` is what fact.radio.handoff says: "ask" (another FoxSDR window holds the radio: two
// keys), "other" (another program holds it: one key, no move), "refused" (the other window would not: `detail` is its reason code), "timeout" (it did not answer), "moved" (THIS window
// has just let a radio go to another). Empty title for a state that shows nothing. Translated.
struct Sheet {
    std::string title, text, goLabel, stayLabel;   // goLabel empty: nothing to decide, one key
    bool shown() const { return !title.empty(); }
};
Sheet sheetFor(const std::string& state, const std::string& model, const std::string& lastFourChars, const std::string& detail);

// The reason code's sentence, for the person who asked (translated).
std::string refusalSentence(const std::string& code, const std::string& radioName);

}  // namespace cascade::core::handoff
