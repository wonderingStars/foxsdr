// sdrplaydiag_logic.hpp - THE SDRPLAY DIAGNOSTIC'S DIALOG LOGIC (window-gaps-2, rows 1460, 1461, 1464 to 1470, 4614): the classic window's AppWindow::drawSdrPlayProbeDialog / startSdrPlayProbe
// (gui/app_window_sdrplay.cpp) without a window. One probe at a time, in a child process (source/sdrplay_probe.hpp) so a vendor call that hangs cannot take this process's SDRplay session.
//
// THE BIAS TEE puts DC power on the antenna socket, so it reaches the probe by ONE road: the separate confirmation ("Yes, switch it on"), which is the only thing that sets biasOn. Ticking the box
// only ASKS; a Cancel leaves it off; Run starts the child with whatever the confirmation left. Nothing in this class runs the probe itself: the child is behind ProbeRunner, so a test hands it a
// stand-in and no SDRplay radio, service or API is ever touched by a test.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace foxsdr::slintdiag {

class ProbeRunner {
public:
    virtual ~ProbeRunner() = default;
    virtual bool start(const std::string& outPath, bool biasTeeOn, std::string& error) = 0;
    virtual bool running() = 0;
    virtual int exitCode() const = 0;
};

// The real one: the program's own executable again with --sdrplay-probe (source::SdrPlayProbeChild).
std::unique_ptr<ProbeRunner> makeProbeChildRunner();   // defined in sdrplaydiag_child.cpp: the product only, a test never links it
// A STAND-IN for the child (a bounded run and every test): start() writes a small report file at once, running() stays true for `polls` looks, the exit code is 0. It never touches an SDRplay radio,
// the service or the vendor API.
std::unique_ptr<ProbeRunner> makeStandInRunner(int polls);

class SdrPlayDiag {
public:
    using RunnerFactory = std::function<std::unique_ptr<ProbeRunner>()>;
    // reportsDir: where the report file goes (the diagnostics crash folder); "" falls back to the temp folder.
    SdrPlayDiag(RunnerFactory factory, std::function<std::string()> reportsDir);

    // What the window draws.
    struct View {
        bool keyEnabled = true;            // dead while a probe runs or FoxSDR itself holds an RSP
        std::string intro;                 // "For SDRplay owners: ..."
        std::string status;                // the line under the key
        bool dialogShown = false;
        std::string warning;               // "This takes the SDRplay radio for a minute or two..."
        bool biasAsk = false;
        std::string biasQuestion;
        bool biasOn = false;               // the box shows ticked once the question was answered yes
    };
    View view(bool sdrplayOpen) const;

    void openDialog();               // Run SDRplay diagnostic: opens the dialog, bias off, no question
    void tickBias(bool want);     // the checkbox: ticking only ASKS
    void biasYes();               // Yes, switch it on
    void biasCancel();            // Cancel on the question: off, back to the dialog
    void run();                   // Run the diagnostic: closes the dialog and starts the child with what the confirmation left
    void cancel();                // Cancel on the dialog
    void poll();                  // once a tick: a finished child's file is read once

    bool running() const { return child_ != nullptr; }
    const std::string& report() const { return report_; }       // the finished file's text
    const std::string& reportPath() const { return path_; }
    int startsCalled() const { return starts_; }
    bool lastStartedWithBias() const { return lastBias_; }

private:
    RunnerFactory factory_;
    std::function<std::string()> reportsDir_;
    std::unique_ptr<ProbeRunner> child_;
    bool dialog_ = false, biasAsk_ = false, biasOn_ = false, lastBias_ = false;
    int starts_ = 0;
    std::string status_, report_, path_;
};

}  // namespace foxsdr::slintdiag
