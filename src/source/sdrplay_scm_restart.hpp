// sdrplay_scm_restart.hpp - restart the SDRplay Windows service through the Service Control
// Manager (survey row W4c), with the decision logic separated from the Windows calls.
//
// CALLED BY NOTHING in this lane. The tree already has an elevated cmd.exe route
// (sdrplay_service.hpp, SdrPlayServiceRestart); this is the direct route for a process that
// already holds the right to control the service, and it says plainly when it does not
// (SdrPlayScmResult::AccessDenied, with the Windows error) instead of prompting. It is never
// run against the real service in a test: tests inject ScmInterface.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <chrono>
#include <string>

namespace cascade::source {

enum class ScmState { Stopped, StartPending, StopPending, Running, Other };

inline constexpr unsigned long kScmErrorAccessDenied = 5;            // ERROR_ACCESS_DENIED
inline constexpr unsigned long kScmErrorServiceDoesNotExist = 1060;  // ERROR_SERVICE_DOES_NOT_EXIST

// The few Service Control Manager operations a restart needs. The real one wraps
// OpenSCManagerW / OpenServiceW / ControlService / StartServiceW / QueryServiceStatusEx.
// Each call answers 0 or a Windows error number.
class ScmInterface {
public:
    virtual ~ScmInterface() = default;
    virtual unsigned long open(const std::string& serviceName) = 0;
    virtual unsigned long queryState(ScmState& state) = 0;
    virtual unsigned long requestStop() = 0;
    virtual unsigned long requestStart() = 0;
    virtual void sleepFor(std::chrono::milliseconds ms) = 0;  // injected so tests do not wait
    virtual void close() = 0;
};

enum class SdrPlayScmResult {
    Restarted,     // stopped (or was already stopped) and is Running again
    AccessDenied,  // the process lacks the right to control the service
    NotInstalled,  // the service does not exist
    StopTimedOut,
    StartTimedOut,
    OtherError     // `error` holds the Windows number
};

struct SdrPlayScmOutcome {
    SdrPlayScmResult result = SdrPlayScmResult::OtherError;
    unsigned long error = 0;
    std::string sentence;  // for the screen
};

struct SdrPlayScmLimits {
    std::chrono::milliseconds step{250};
    std::chrono::milliseconds stopWait{20000};
    std::chrono::milliseconds startWait{20000};
};

// The decision logic. Always closes what it opened. Never throws.
SdrPlayScmOutcome restartSdrPlayServiceVia(ScmInterface& scm, const std::string& serviceName,
                                           const SdrPlayScmLimits& limits = SdrPlayScmLimits{});

// The real interface (Windows; off Windows every call answers 50, ERROR_NOT_SUPPORTED).
// Constructing it touches nothing; open() is the first Windows call.
class WindowsScm : public ScmInterface {
public:
    WindowsScm();
    ~WindowsScm() override;
    unsigned long open(const std::string& serviceName) override;
    unsigned long queryState(ScmState& state) override;
    unsigned long requestStop() override;
    unsigned long requestStart() override;
    void sleepFor(std::chrono::milliseconds ms) override;
    void close() override;

private:
    void* manager_ = nullptr;
    void* service_ = nullptr;
};

// restartSdrPlayServiceVia() over a WindowsScm. NOT TO BE CALLED FROM A TEST.
SdrPlayScmOutcome restartSdrPlayServiceNow(const std::string& serviceName);

}  // namespace cascade::source
