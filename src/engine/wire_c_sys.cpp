// wire_c_sys.cpp - see wire_c_sys.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/wire_c_sys.hpp"

#include <exception>
#include <string>

namespace cascade::engine::wirec {

namespace sys = cascade::core::sys;

// ---- the first-run wizard ----
std::string SysOps::wizardStepName() const {
    switch (wizard_.step()) {
        case sys::WizardStep::Welcome: return "welcome";
        case sys::WizardStep::Radio: return "radio";
        case sys::WizardStep::Preset: return "preset";
        case sys::WizardStep::Privacy: return "privacy";
        case sys::WizardStep::Done: return "done";
    }
    return "welcome";
}

bool SysOps::wizardVerb(const std::string& verb, std::string& why) {
    if (verb == "next") {
        if (!wizard_.next()) { why = "make this step's choice first"; return false; }
        return true;
    }
    if (verb == "back") {
        if (!wizard_.back()) { why = "this is the first step"; return false; }
        return true;
    }
    if (verb == "skip") {
        wizard_.skip();
        return true;
    }
    if (verb.rfind("radio:", 0) == 0) {
        const std::vector<sys::RadioChoiceInfo>& all = sys::radioChoices();
        int n = -1;
        try { n = std::stoi(verb.substr(6)); } catch (const std::exception&) { n = -1; }
        if (n < 0 || static_cast<std::size_t>(n) >= all.size()) { why = "no such radio choice"; return false; }
        if (!wizard_.chooseRadio(all[static_cast<std::size_t>(n)].family)) { why = "that radio cannot be chosen now"; return false; }
        return true;
    }
    if (verb.rfind("preset:", 0) == 0) {
        if (!wizard_.choosePreset(verb.substr(7))) { why = "that set of stations is not offered for this radio"; return false; }
        return true;
    }
    if (verb == "telemetry:1" || verb == "telemetry:0") {
        wizard_.chooseTelemetry(verb == "telemetry:1");
        telemetryChosen_ = true;
        return true;
    }
    why = "unknown step";
    return false;
}

std::string SysOps::radioLines() const {
    std::string out;
    for (const sys::RadioChoiceInfo& r : sys::radioChoices()) { out += std::string(out.empty() ? "" : "\n") + r.label; }
    return out;
}

std::string SysOps::presetLines() const {
    std::string out;
    for (const sys::StarterPreset& p : wizard_.offered()) { out += std::string(out.empty() ? "" : "\n") + p.id + "\t" + p.title; }
    return out;
}

}  // namespace cascade::engine::wirec
