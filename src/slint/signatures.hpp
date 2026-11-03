// signatures.hpp - publish a model only when its content changed. Setting a list model replaces every repeater item behind it, so each
// publisher keeps the signature of what it last sent and asks changed() before it sets anything (docs/engine-stage4.md 6.36, the
// model-churn guard; shared by main.cpp and the plugin kit's publisher).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <map>
#include <string>

namespace foxsdr::slinthost {

struct Signatures {
    std::map<std::string, std::string> last;
    bool changed(const std::string& key, const std::string& sig) {
        auto it = last.find(key);
        if (it != last.end() && it->second == sig) { return false; }
        last[key] = sig;
        return true;
    }
    void forget(const std::string& key) { last.erase(key); }
};

}  // namespace foxsdr::slinthost
