// engine_macros.cpp - MACROS in the engine (engine wiring W2; docs/api-0.3-additions.md 9.34; the model is core/macros.hpp).
//
// A macro is a named list of application commands with delays, kept in a JSON file ("macros.json" in the per-user folder: the same folder as
// the configuration). Three ops, all the application's own (so none is a REMOTE session's: "the application's own commands are for sessions on
// this machine"):
//   FOXAPP_OP_MACRO_RUN     text: the macro's name. OK once started; NOT_FOUND no such macro; BUSY one is running already; BAD_ARGUMENT the file
//                           has an error (the message says which macro and step) or the name is empty
//   FOXAPP_OP_MACRO_STOP    stops the run (NO_CHANGE when none is running)
//   FOXAPP_OP_MACRO_RELOAD  reads the file again (the engine reads it the first time anything asks); BAD_ARGUMENT with the file's error, in which
//                           case NO macro is available - a file with one bad step is refused whole, never half-run
//
// THE STEPS ARE THE ENGINE'S OWN COMMANDS, applied by Engine::applyCommand on the control pass, the very path a session's command takes after its
// grant check. A macro step therefore has the authority of the person at this machine, bounded by the model's allow-list made by subtraction
// (core/macros.hpp MacroPolicy): nothing that transmits or keys a transmitter, grants a permission, installs or removes a plugin, edits the
// server or interface configuration, uploads a report or checks for updates, runs a test hook - and not another macro op (a macro starts another
// macro only through the model's RUN_MACRO pseudo-step, which carries the loop protection). A step acts on the first radio.
//
// THE PLAYER holds no thread: pumpMacros() runs once per control pass with the engine's clock and applies the steps that are due, in order. A
// step that the engine refuses stops the run with the refusal's words (a macro does not skip over a failure and carry on).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "core/diag_log.hpp"
#include "core/file_read.hpp"
#include "core/macros.hpp"
#include "engine/engine.hpp"

namespace cascade::engine {

namespace mc = cascade::core::macros;
namespace cmd = cascade::core::cmd;

namespace {

// The command vocabulary a macro file may name: every op the application knows by name (cmd::knownOps / cmd::opName), and the model removes
// the denied prefixes. The name -> op lookup the player's steps need is the same table read the other way.
mc::KnownCommands vocabulary() {
    mc::KnownCommands k;
    std::size_t n = 0;
    const std::uint32_t* ops = cmd::knownOps(n);
    for (std::size_t i = 0; i < n; ++i) { k.add(cmd::opName(ops[i])); }
    return k;
}

bool opByName(const std::string& name, std::uint32_t& op) {
    std::size_t n = 0;
    const std::uint32_t* ops = cmd::knownOps(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (name == cmd::opName(ops[i])) {
            op = ops[i];
            return true;
        }
    }
    return false;
}

}  // namespace

std::string Engine::macrosFilePath() const {
    if (!macrosPathForTest_.empty()) { return macrosPathForTest_; }
    return (std::filesystem::path(cascade::core::ConfigStore::defaultPath()).parent_path() / "macros.json").string();
}

std::int64_t Engine::macroNowMs() const {
    const std::int64_t t = macroClockMs_.load(std::memory_order_relaxed);
    if (t >= 0) { return t; }   // a test's clock
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Engine::loadMacros() {
    macrosLoaded_ = true;
    macros_.clear();
    macrosError_.clear();
    std::string text;
    switch (cascade::core::readTextFile(std::filesystem::path(macrosFilePath()), text)) {
        case cascade::core::ReadResult::Ok:
            break;
        case cascade::core::ReadResult::CannotOpen:
            ++macroVersion_;
            return;   // no file is no macros, and not an error
        default:
            macrosError_ = "the macro file cannot be read";
            ++macroVersion_;
            return;
    }
    mc::LoadResult r = mc::parseMacros(text, vocabulary());
    if (!r.error.empty()) {
        macrosError_ = r.error;
        cascade::core::diagWarnf("macros: the macro file was refused: %s", macrosError_.c_str());
    } else {
        macros_ = std::move(r.macros);
    }
    ++macroVersion_;
}

std::int32_t Engine::applyMacroOp(const FoxCommand& c, const std::string& text, std::string& why) {
    if (!macrosLoaded_ || c.op == FOXAPP_OP_MACRO_RELOAD) {
        if (macroPlayer_.running()) {
            why = "a macro is running; stop it first";
            return FOXAPI_BUSY;
        }
        loadMacros();
    }
    switch (c.op) {
        case FOXAPP_OP_MACRO_RELOAD:
            if (!macrosError_.empty()) {
                why = macrosError_;
                return FOXAPI_BAD_ARGUMENT;
            }
            return FOXAPI_OK;
        case FOXAPP_OP_MACRO_RUN: {
            if (!macrosError_.empty()) {
                why = macrosError_;
                return FOXAPI_BAD_ARGUMENT;
            }
            const std::string name = text.empty() ? std::string(c.text) : text;
            if (name.empty()) {
                why = "no macro named";
                return FOXAPI_BAD_ARGUMENT;
            }
            if (macroPlayer_.running()) {
                why = "a macro is running already";
                return FOXAPI_BUSY;
            }
            const std::string set = mc::validateSet(macros_, vocabulary());
            if (!set.empty()) {
                why = set;
                return FOXAPI_BAD_ARGUMENT;
            }
            std::string err;
            if (!macroPlayer_.start(macros_, name, macroNowMs(), &err)) {
                why = err;
                return FOXAPI_NOT_FOUND;
            }
            macroName_ = name;
            macroLast_ = "running";
            macroStepsApplied_ = 0;
            ++macroVersion_;
            cascade::core::diagLogf("macros: started '%s'", name.c_str());
            return FOXAPI_OK;
        }
        case FOXAPP_OP_MACRO_STOP:
            if (!macroPlayer_.running()) {
                why = "no macro is running";
                return FOXAPI_NO_CHANGE;
            }
            macroPlayer_.cancel();
            macroLast_ = "stopped";
            macroName_.clear();
            ++macroVersion_;
            return FOXAPI_OK;
        default:
            break;
    }
    why = "not a macro op";
    return FOXAPI_UNSUPPORTED;
}

void Engine::pumpMacros() {
    if (!macroPlayer_.running()) { return; }
    for (const mc::DueStep& d : macroPlayer_.poll(macroNowMs())) {
        if (d.step.command == mc::kWait) { continue; }
        std::uint32_t op = 0;
        if (!opByName(d.step.command, op)) {
            macroPlayer_.cancel();
            macroLast_ = "stopped: step " + std::to_string(d.index + 1) + " of '" + d.macro + "' names a command this build does not have";
            break;
        }
        FoxCommand c = cmd::make(op);
        for (int i = 0; i < 4; ++i) { c.num[i] = d.step.num[i]; }
        for (int i = 0; i < 2; ++i) { c.ival[i] = d.step.ival[i]; }
        std::snprintf(c.text, sizeof(c.text), "%s", d.step.text.c_str());
        const FoxCommandResult r = applyCommand(c, std::string());
        ++macroStepsApplied_;
        ++macroVersion_;   // the step count is a fact
        // A refusal ends the run: a macro does not step over a failure and carry on. NO_CHANGE (already so) is not one.
        if (r.status != FOXAPI_OK && r.status != FOXAPI_NO_CHANGE) {
            macroPlayer_.cancel();
            macroLast_ = "stopped: step " + std::to_string(d.index + 1) + " of '" + d.macro + "' was refused: " + r.message;
            cascade::core::diagWarnf("macros: '%s' stopped at step %zu: %s", d.macro.c_str(), d.index + 1, r.message);
            break;
        }
    }
    if (!macroPlayer_.running()) {
        if (macroLast_ == "running") { macroLast_ = macroPlayer_.stopReason().empty() ? "finished" : "stopped: " + macroPlayer_.stopReason(); }
        macroName_.clear();
        ++macroVersion_;
    }
}

void Engine::macroFacts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    std::string names;
    for (const mc::Macro& m : macros_) {
        if (!names.empty()) { names += '\n'; }
        names += m.name;
    }
    fact("macro.names", names);
    fact("macro.running", macroName_);
    fact("macro.last", macroLast_);
    fact("macro.error", macrosError_);
    fact("macro.steps", std::to_string(macroStepsApplied_));
}

}  // namespace cascade::engine
