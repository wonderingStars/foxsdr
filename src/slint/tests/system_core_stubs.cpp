// system_core_stubs.cpp - the few core symbols test_slint_host_system.cpp would otherwise have to link the whole of the engine's diagnostics for.
//
// system_logic.cpp quotes core's pure rules (the feature-request and problem-report validators, the pasted-report detector, the tester-code and
// country tables), and those units sit beside the senders (feature_request.cpp posts through crash_upload.cpp's transport) and the sentinel (the
// pasted report's class words). The test does not send and does not run a sentinel; it needs these five symbols to LINK, no more:
//   sentinelClassId / sentinelClassReason  copied from core/sentinel.cpp (a six-way switch over the header's own constants), so a pasted report is
//                                          still recognised with the class it would carry;
//   UploadCancel::cancel, postBounded      the transport: this one never reaches a network (attempted = false).
// The product links the real ones (cascade_engine). The file is not named test_slint_*.cpp so the directory's glob does not register it as a test.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/crash_upload.hpp"
#include "core/sentinel.hpp"

namespace cascade::core {

const char* sentinelClassId(SentinelClass c) {
    switch (c) {
        case SentinelClass::Crash: return "crash";
        case SentinelClass::Frozen: return "frozen";
        case SentinelClass::Startup: return "startup";
        case SentinelClass::Outside: return "outside";
        case SentinelClass::Session: return "session";
        case SentinelClass::None: break;
    }
    return "none";
}

const char* sentinelClassReason(SentinelClass c) {
    switch (c) {
        case SentinelClass::Crash: return kSentinelReasonCrash;
        case SentinelClass::Frozen: return kSentinelReasonFrozen;
        case SentinelClass::Startup: return kSentinelReasonStartup;
        case SentinelClass::Outside: return kSentinelReasonOutside;
        case SentinelClass::Session: return kSentinelReasonSession;
        case SentinelClass::None: break;
    }
    return "";
}

void UploadCancel::cancel() { cancelled_.store(true, std::memory_order_release); }

RawPostResult postBounded(const std::string&, const std::string&, const std::shared_ptr<UploadCancel>&, bool) { return RawPostResult{}; }

}  // namespace cascade::core
