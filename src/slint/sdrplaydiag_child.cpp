// sdrplaydiag_child.cpp - the real ProbeRunner: the program's own executable again with --sdrplay-probe (source::SdrPlayProbeChild). The product links it; no test does.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "sdrplaydiag_logic.hpp"

#include "source/sdrplay_probe.hpp"

namespace foxsdr::slintdiag {

namespace {
class ChildRunner final : public ProbeRunner {
public:
    bool start(const std::string& outPath, bool biasTeeOn, std::string& error) override { return child_.start(outPath, biasTeeOn, error); }
    bool running() override { return child_.running(); }
    int exitCode() const override { return child_.exitCode(); }
private:
    cascade::source::SdrPlayProbeChild child_;
};
}  // namespace

std::unique_ptr<ProbeRunner> makeProbeChildRunner() { return std::make_unique<ChildRunner>(); }

}  // namespace foxsdr::slintdiag
