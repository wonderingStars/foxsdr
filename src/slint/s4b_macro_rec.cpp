// s4b_macro_rec.cpp - see s4b_macro_rec.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "s4b_macro_rec.hpp"

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include "s4b_logic.hpp"

namespace foxsdr::slints4b {

namespace {

const FoxEngineApi* g_real = nullptr;
std::mutex g_m;

double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void noteOne(const FoxCommand& c) {
    const double num[4] = {c.num[0], c.num[1], c.num[2], c.num[3]};
    const long long ival[2] = {static_cast<long long>(c.ival[0]), static_cast<long long>(c.ival[1])};
    std::lock_guard<std::mutex> lk(g_m);
    macroRecorder().note(c.op, c.target, num, ival, nowMs());
}

int32_t FOXAPI_CALL recSubmit(FoxSession* session, const FoxCommand* commands, uint32_t count, FoxSubmitResult* results) {
    bool on;
    {
        std::lock_guard<std::mutex> lk(g_m);
        on = macroRecorder().active();
    }
    if (!on || commands == nullptr || count == 0u) { return g_real->submit(session, commands, count, results); }
    std::vector<FoxSubmitResult> local;
    if (results == nullptr) {
        local.resize(count);
        for (FoxSubmitResult& r : local) { r.structSize = sizeof(FoxSubmitResult); }
        results = local.data();
    }
    const int32_t rc = g_real->submit(session, commands, count, results);
    if (rc >= 0) {
        for (uint32_t i = 0; i < count; ++i) { if (results[i].status == FOXAPI_OK) { noteOne(commands[i]); } }
    }
    return rc;
}

int32_t FOXAPI_CALL recSubmitText(FoxSession* session, const FoxCommand* command, const char* text, uint32_t len, FoxSubmitResult* result) {
    // (no op the recorder keeps carries a long text: nothing to note; the call is passed on unchanged)
    return g_real->submit_text(session, command, text, len, result);
}

}  // namespace

const FoxEngineApi* recordingApi(const FoxEngineApi* real) {
    static FoxEngineApi copy{};
    static std::once_flag once;
    std::call_once(once, [&] {
        copy = *real;
        g_real = real;
        copy.submit = &recSubmit;
        copy.submit_text = &recSubmitText;
    });
    return &copy;
}

void recorderStart() { std::lock_guard<std::mutex> lk(g_m); macroRecorder().start(); }
void recorderStop() { std::lock_guard<std::mutex> lk(g_m); macroRecorder().stop(); }
void recorderClear() { std::lock_guard<std::mutex> lk(g_m); macroRecorder().clear(); }
bool recorderActive() { std::lock_guard<std::mutex> lk(g_m); return macroRecorder().active(); }
std::size_t recorderSteps() { std::lock_guard<std::mutex> lk(g_m); return macroRecorder().size(); }
std::size_t recorderSkipped() { std::lock_guard<std::mutex> lk(g_m); return macroRecorder().skippedOtherRadio(); }
std::string recorderSaveJson(const std::string& name) { std::lock_guard<std::mutex> lk(g_m); return macroRecorder().saveJson(name); }

}  // namespace foxsdr::slints4b
