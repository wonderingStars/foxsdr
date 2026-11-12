// engine_wire_c.cpp - the engine's side of wiring lane C: the op block 0x8A00-0x8AFF, its facts, its once-a-pass pump.
// The settings registry and the state are in wire_c.{hpp,cpp}; each feature of the lane adds its own file next to this one and one `case` here.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>

#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "engine/engine.hpp"
#include "engine/wire_c.hpp"

namespace cascade::engine {

namespace wc = cascade::engine::wirec;

wc::State& Engine::wireC() {
    std::lock_guard<std::mutex> lk(wireCMutex_);   // (the first caller may be the control pass or the fact builder: whichever asks first builds it)
    if (!wireC_) {
        const std::filesystem::path folder = wireCFolderForTest_.empty()
            ? std::filesystem::path(cascade::core::ConfigStore::defaultPath()).parent_path()
            : std::filesystem::path(wireCFolderForTest_);
        wireC_ = std::make_unique<wc::State>(folder);
    }
    return *wireC_;
}

std::int32_t Engine::applyWireC(const FoxCommand& c, const std::string& text, FoxCommandResult& res, std::string& why) {
    (void)res;
    switch (c.op) {
        case FOXAPP_OP_WIREC_SETTING: {
            const std::size_t sep = text.find('\x1F');
            if (sep == std::string::npos || sep == 0) {
                why = "the text is \"<key>\\x1F<value>\"";
                return FOXAPI_BAD_ARGUMENT;
            }
            switch (wireC().settings().set(text.substr(0, sep), text.substr(sep + 1), why)) {
                case wc::SetResult::Changed: ++wireCVersion_; return FOXAPI_OK;
                case wc::SetResult::NoChange: return FOXAPI_NO_CHANGE;
                case wc::SetResult::NotFound: return FOXAPI_NOT_FOUND;
                case wc::SetResult::BadValue: return FOXAPI_BAD_ARGUMENT;
                case wc::SetResult::OutOfRange: return FOXAPI_OUT_OF_RANGE;
                case wc::SetResult::SaveFailed: return FOXAPI_FAILED;
            }
            return FOXAPI_FAILED;
        }
        default:
            break;
    }
    why = "not an operation of this block";
    return FOXAPI_UNSUPPORTED;
}

void Engine::wireCFacts(const std::function<void(const std::string&, const std::string&)>& fact) {
    wc::State& s = wireC();
    s.settings().each([&fact](const std::string& key, const std::string& value) { fact("wirec." + key, value); });
    fact("wirec.version", std::to_string(wireCVersion()));
    fact("wirec.error", s.settings().error());
}

std::uint64_t Engine::wireCVersion() {
    return wireCVersion_ + wireC().settings().version();
}

void Engine::pumpWireC() {
    // (nothing runs on the pass yet: each feature adds its call here)
}

}  // namespace cascade::engine
