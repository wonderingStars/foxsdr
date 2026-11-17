// s4b_macro_edit.cpp - see s4b_macro_edit.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/s4b_macro_edit.hpp"

#include <cmath>
#include <filesystem>

#include <nlohmann/json.hpp>

#include "core/app_commands.hpp"
#include "core/macros.hpp"
#include "foxsdr_api.h"

namespace cascade::engine::s4b {

namespace mc = cascade::core::macros;
using nlohmann::json;

namespace {

mc::KnownCommands vocabulary() {
    mc::KnownCommands k;
    std::size_t n = 0;
    const std::uint32_t* ops = cascade::core::cmd::knownOps(n);
    for (std::size_t i = 0; i < n; ++i) { k.add(cascade::core::cmd::opName(ops[i])); }
    return k;
}

}  // namespace

std::int32_t macroEdit(const std::string& path, const std::string& text, std::string& why) {
    if (text.size() > 200000) { why = "that macro is too large"; return FOXAPI_BAD_ARGUMENT; }
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("action") || !j["action"].is_string()) { why = "the text is JSON with an action: save or delete"; return FOXAPI_BAD_ARGUMENT; }
    const std::string action = j["action"].get<std::string>();
    if (!j.contains("name") || !j["name"].is_string()) { why = "the macro needs a name"; return FOXAPI_BAD_ARGUMENT; }
    const std::string name = j["name"].get<std::string>();
    if (name.empty() || name.size() > mc::kMaxNameLen || mc::detail::hasControl(name)) { why = "the name is 1 to 64 characters without control characters"; return FOXAPI_BAD_ARGUMENT; }

    const mc::KnownCommands known = vocabulary();
    std::vector<mc::Macro> macros;
    {
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::path(path), ec)) {
            mc::LoadResult r = mc::loadMacrosFile(path, known);
            if (!r.error.empty()) { why = "the macro file is refused as it is: " + r.error; return FOXAPI_BAD_ARGUMENT; }
            macros = std::move(r.macros);
        }
    }
    if (action == "delete") {
        if (j.size() != 2) { why = "delete takes only the name"; return FOXAPI_BAD_ARGUMENT; }
        for (auto it = macros.begin(); it != macros.end(); ++it) {
            if (it->name == name) {
                macros.erase(it);
                std::string err;
                if (!mc::saveMacrosFile(path, macros, known, &err)) { why = err.empty() ? std::string("the macro file could not be written") : err; return FOXAPI_FAILED; }
                return FOXAPI_OK;
            }
        }
        why = "there is no macro of that name";
        return FOXAPI_NOT_FOUND;
    }
    if (action != "save") { why = "the action is save or delete"; return FOXAPI_BAD_ARGUMENT; }
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string& k = it.key();
        if (k != "action" && k != "name" && k != "description" && k != "steps") { why = "an unknown key in the macro"; return FOXAPI_BAD_ARGUMENT; }
    }
    mc::Macro m;
    m.name = name;
    if (j.contains("description")) {
        if (!j["description"].is_string() || j["description"].get<std::string>().size() > 300 || mc::detail::hasControl(j["description"].get<std::string>())) { why = "the description is a short text"; return FOXAPI_BAD_ARGUMENT; }
        m.description = j["description"].get<std::string>();
    }
    if (!j.contains("steps") || !j["steps"].is_array() || j["steps"].empty()) { why = "the macro needs at least one step"; return FOXAPI_BAD_ARGUMENT; }
    if (j["steps"].size() > mc::kMaxSteps) { why = "a macro has at most 500 steps"; return FOXAPI_LIMIT; }
    for (const json& s : j["steps"]) {
        if (!s.is_object() || !s.contains("cmd") || !s["cmd"].is_string()) { why = "each step names a command"; return FOXAPI_BAD_ARGUMENT; }
        mc::Step st;
        st.command = s["cmd"].get<std::string>();
        if (s.contains("num")) {
            if (!s["num"].is_array() || s["num"].size() > 4) { why = "num is at most four numbers"; return FOXAPI_BAD_ARGUMENT; }
            for (std::size_t i = 0; i < s["num"].size(); ++i) {
                if (!s["num"][i].is_number() || !std::isfinite(s["num"][i].get<double>())) { why = "num is numbers"; return FOXAPI_BAD_ARGUMENT; }
                st.num[i] = s["num"][i].get<double>();
            }
        }
        if (s.contains("ival")) {
            if (!s["ival"].is_array() || s["ival"].size() > 2) { why = "ival is at most two whole numbers"; return FOXAPI_BAD_ARGUMENT; }
            for (std::size_t i = 0; i < s["ival"].size(); ++i) {
                if (!s["ival"][i].is_number_integer()) { why = "ival is whole numbers"; return FOXAPI_BAD_ARGUMENT; }
                st.ival[i] = s["ival"][i].get<std::int64_t>();
            }
        }
        if (s.contains("delayMs")) {
            if (!s["delayMs"].is_number_integer() || s["delayMs"].get<std::int64_t>() < 0 || s["delayMs"].get<std::int64_t>() > mc::kMaxDelayMs) { why = "delayMs is a whole number of milliseconds up to a day"; return FOXAPI_BAD_ARGUMENT; }
            st.delayMs = s["delayMs"].get<std::int64_t>();
        }
        m.steps.push_back(std::move(st));
    }
    const std::string bad = mc::validateMacro(m, known);
    if (!bad.empty()) { why = bad; return FOXAPI_BAD_ARGUMENT; }
    bool replaced = false;
    for (mc::Macro& old : macros) {
        if (old.name == name) { old = m; replaced = true; break; }
    }
    if (!replaced) {
        if (macros.size() >= mc::kMaxMacros) { why = "there are 200 macros already"; return FOXAPI_LIMIT; }
        macros.push_back(m);
    }
    std::string err;
    if (!mc::saveMacrosFile(path, macros, known, &err)) { why = err.empty() ? std::string("the macro file could not be written") : err; return FOXAPI_FAILED; }
    return FOXAPI_OK;
}

}  // namespace cascade::engine::s4b
