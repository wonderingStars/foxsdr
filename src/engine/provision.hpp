// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// HEADLESS PROVISIONING (engine extraction stage 7e): setting up the web server and the CAT listener of
// `foxsdr-engine` from its command line, because a service has no settings panel to do it in.
//
//   foxsdr-engine --web-enable [--web-bind ADDR] [--web-port N] [--web-user NAME]
//                 [--web-password-file FILE | --web-password-file -]
//                 --cat-enable [--cat-bind-all]
//
// WHAT IT IS, AND IS NOT. The flags say what the settings panels would say - and travel the same way: they are
// built into ONE FOXAPI_OP_SERVER_CONFIG (docs/api-0.3-additions.md 8.16, the command the web and CAT panels
// submit) and submitted on a LOCAL session with the ADMIN grant, the only kind of session the engine lets
// configure what listens on the machine. The engine's ServerHost does the rest, as it does for the window:
// the password is hashed on its worker (the record is the panel's, byte for byte the same code), the bind
// policy decides (net/web_policy.hpp - the sentence a refused configuration is given is the policy's own, shown
// as it is), the listeners start and stop on its worker, and what was configured rides the engine's half of the
// settings file. This module owns no policy and no socket; it asks, waits for the answer and reports it.
//
// THE PASSWORD IS NEVER ON THE COMMAND LINE. `--web-password-file FILE` reads it from a file, `-` from standard
// input - the rule of the tester-link flag (src/engine/process_boot.cpp, "a value here would sit in shell
// history indefinitely"), and for the same reason: an argv is visible to every other process on the machine
// (the process list) and kept by shells and service managers. The flag takes a NAME, never the secret. One
// trailing line end is not part of the password (the file of a `echo secret > file`); nothing else is
// trimmed. The plaintext lives in a Secret (below) from the read to the end of the submit, and is overwritten
// there; it is in no log line and in no message.
//
// WHEN. After the settings are restored and BEFORE the receiver starts (EngineService::provision, between
// restore() and start()): the configuration is settled - the password hashed, the listeners tried - before
// anything is receiving, so a refusal ends the run with nothing started and, with a settings file, nothing
// written (the file keeps what it held).
//
// PERSISTENCE. A run with a settings file (`--config PATH` / `--use-config`) keeps what the flags said: the
// engine's half of the file carries the web* and cat* fields, the forced first save of start() writes them, and
// a later run with no provisioning flags restores them. A run WITHOUT one (hermetic) applies the flags for the
// life of the process and writes nothing - the same promise as the rest of a hermetic run - and its first
// stderr line says so. (The alternative, flags that quietly create a settings file, would break the hermetic
// rule and leave a password record on disk that nobody asked to have stored.)
//
// Engine-clean: no gui/, imgui, GLFW or OpenGL.
#pragma once

#include <chrono>
#include <cstdio>
#include <string>

namespace cascade::engine {

class Engine;

// What the command line asked for. Every web value flag needs --web-enable and --cat-bind-all needs --cat-enable
// (the parse refuses the rest), so "any()" is the whole question of whether this run provisions at all.
struct ProvisionRequest {
    bool webEnable = false;
    std::string webBind;       // empty: not given (the stored value, or the default 127.0.0.1)
    int webPort = 0;           // 0: not given (the stored value, or the default 8073)
    std::string webUser;       // empty: not given (the stored value, or the default "admin")
    std::string passwordFile;  // the NAME of the file ("-": standard input); empty: no password asked for
    bool catEnable = false;
    bool catBindAll = false;
    bool any() const { return webEnable || catEnable; }
};

// A secret's plaintext: overwritten when it goes out of scope. The house has no wipe helper - the settings panel
// and the server host overwrite their copies with std::fill after the call (app_window.cpp applyWebSettings,
// server_host.cpp startHash) - so this follows that practice, written as volatile stores because a plain
// fill of a buffer that is about to be freed is exactly what an optimiser is allowed to drop.
struct Secret {
    std::string value;
    Secret() = default;
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;
    ~Secret() { wipe(value); }
    bool empty() const { return value.empty(); }
    static void wipe(std::string& s);
};

// Reads the password from `source`: a file's name, or "-" for standard input. The one trailing line end (LF or
// CR LF) is dropped. False with a sentence in `error` when the source cannot be read, holds no password, is
// larger than a password can be, or is not valid UTF-8 (a password that cannot be sent as the text it is must
// not be quietly changed on the way). The sentence names the flag and the file's name, never a byte of it.
bool readPassword(const std::string& source, Secret& out, std::string& error);
// The same for an open stream (the file or standard input; the caller closes it): the seam the tests use. On
// failure `why` is the reason alone ("holds no password"); readPassword puts the flag and the name before it.
bool readPasswordFrom(std::FILE* in, Secret& out, std::string& why);

// The FOXAPI_OP_SERVER_CONFIG text for `request` (the JSON the panels build: docs/api-0.3-additions.md 8.16;
// strict - see parseServerConfigRequest in engine/server_host.hpp), carrying `password` when there is one.
// The caller overwrites the returned text once it is sent when a password is in it.
std::string serverConfigText(const ProvisionRequest& request, const Secret* password);

// What provisioning came to.
struct ProvisionOutcome {
    bool ok = false;
    // ok == false: the whole refusal, ready for stderr - the engine's sentence for a refused request, the
    // bind policy's for a refused listener, the listener's own for one the machine would not give. No password.
    std::string error;
    // ok: one line for the journal - what listens, where, and whether a password is required.
    std::string summary;
};

// Submits `request` on a LOCAL ADMIN session of `engine` (opened for this and closed before it returns), lets
// the engine's passes run by hand until the configuration has settled (the answer, the password hashed, the
// listeners started or refused, the list published) - the control thread is not running yet - and reads what
// the engine says of each listener the flags asked for. `bound` is the longest the whole thing may take (a
// password hash is the long part). Call it with the engine initialised and the control thread NOT started.
ProvisionOutcome provisionServers(Engine& engine, const ProvisionRequest& request, const Secret* password,
                                  std::chrono::milliseconds bound);

}  // namespace cascade::engine
