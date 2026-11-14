// The SDRplay diagnostic's dialog (window-gaps-2, rows 1460, 1461, 1464 to 1470; src/slint/sdrplaydiag_logic.hpp) and the pointer ledger's line (pointer_ledger.hpp). A STAND-IN child stands for the
// probe, so no SDRplay radio, service or vendor API is touched. What this holds equal: the key is live unless a probe runs or FoxSDR holds an RSP; ticking the bias box only ASKS; Cancel on the
// question leaves it off; "Yes" is the one road the bias tee reaches the probe, and it does not outlive the dialog; Run is refused while the question is open; a finished child's file is read once;
// a start that fails says so; and over a long random sequence of presses the probe is never started with the bias tee on unless the last thing said about it was yes.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "sdrplaydiag_logic.hpp"
#include "pointer_ledger.hpp"
#include "test_check.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace foxsdr::slintdiag;

namespace {

struct Spy {
    int starts = 0;
    bool lastBias = false;
    bool failNext = false;
    int pollsToRun = 3;
    std::string lastPath;
};

class SpyRunner final : public ProbeRunner {
public:
    SpyRunner(Spy& s, int polls) : spy_(s), polls_(polls) {}
    bool start(const std::string& outPath, bool bias, std::string& error) override {
        ++spy_.starts;
        spy_.lastBias = bias;
        spy_.lastPath = outPath;
        if (spy_.failNext) { spy_.failNext = false; error = "Windows would not start it"; return false; }
        std::ofstream f(outPath, std::ios::binary | std::ios::trunc);
        f << "report bias=" << (bias ? "on" : "off") << "\n";
        return true;
    }
    bool running() override { return polls_-- > 0; }
    int exitCode() const override { return 0; }
private:
    Spy& spy_;
    int polls_;
};

std::filesystem::path scratch() {
    const std::filesystem::path p = std::filesystem::temp_directory_path() / "foxsdr-test-sdrplaydiag";
    std::filesystem::create_directories(p);
    return p;
}

SdrPlayDiag make(Spy& spy) {
    return SdrPlayDiag([&spy] { return std::unique_ptr<ProbeRunner>(new SpyRunner(spy, spy.pollsToRun)); }, [] { return scratch().string(); });
}

void flow() {
    Spy spy;
    SdrPlayDiag d = make(spy);
    SdrPlayDiag::View v = d.view(false);
    CHECK(v.keyEnabled && !v.dialogShown && !v.biasAsk && v.status.empty());
    CHECK(v.intro.find("For SDRplay owners") == 0 && v.warning.find("This takes the SDRplay radio") == 0 && v.biasQuestion.find("The bias tee puts DC power on the antenna socket") == 0);
    // FoxSDR itself holds an RSP: the key is dead and says why
    v = d.view(true);
    CHECK(!v.keyEnabled && v.status == "FoxSDR has the SDRplay radio open - choose another source first.");

    // the key opens the dialog; nothing is asked and nothing started
    d.press();
    v = d.view(false);
    CHECK(v.dialogShown && !v.biasAsk && !v.biasOn && spy.starts == 0);
    // ticking the bias box only ASKS
    d.tickBias(true);
    v = d.view(false);
    CHECK(v.biasAsk && !v.biasOn && spy.starts == 0);
    d.run();   // refused while the question is open
    CHECK(spy.starts == 0 && d.view(false).dialogShown);
    // Cancel on the question: off, back to the dialog
    d.biasCancel();
    v = d.view(false);
    CHECK(!v.biasAsk && !v.biasOn && v.dialogShown);
    // ask again and say yes: the box shows ticked
    d.tickBias(true);
    d.biasYes();
    v = d.view(false);
    CHECK(!v.biasAsk && v.biasOn);
    // unticking it takes it back
    d.tickBias(false);
    CHECK(!d.view(false).biasOn);
    d.tickBias(true);
    d.biasYes();
    // Run: the dialog closes, the child is started WITH the bias tee, the key is dead while it runs
    d.run();
    v = d.view(false);
    CHECK(spy.starts == 1 && spy.lastBias && d.lastStartedWithBias() && !v.dialogShown && !v.keyEnabled && d.running());
    CHECK(v.status == "SDRplay diagnostic running - this takes a minute or two...");
    d.press();   // a press while it runs does nothing
    CHECK(!d.view(false).dialogShown);
    for (int i = 0; i < 2; ++i) { d.poll(); CHECK(d.running()); }
    for (int i = 0; i < 4; ++i) { d.poll(); }
    CHECK(!d.running() && d.report() == "report bias=on\n");
    CHECK(d.view(false).status.find("SDRplay diagnostic finished: ") == 0 && d.view(false).keyEnabled);

    // a second diagnostic: the bias tee does not outlive the first dialog
    d.press();
    CHECK(!d.view(false).biasOn);
    d.run();
    CHECK(spy.starts == 2 && !spy.lastBias && !d.lastStartedWithBias());
    for (int i = 0; i < 6; ++i) { d.poll(); }
    CHECK(d.report() == "report bias=off\n");

    // Cancel on the dialog starts nothing; run() with no dialog starts nothing
    d.press();
    d.cancel();
    d.run();
    CHECK(spy.starts == 2 && !d.view(false).dialogShown);
    d.biasYes();
    d.biasCancel();
    d.tickBias(true);
    CHECK(!d.view(false).biasAsk && !d.view(false).biasOn);   // no dialog, no question
}

void failure() {
    Spy spy;
    SdrPlayDiag d = make(spy);
    spy.failNext = true;
    d.press();
    d.run();
    const SdrPlayDiag::View v = d.view(false);
    CHECK(spy.starts == 1 && !d.running() && v.keyEnabled && v.status == "SDRplay diagnostic could not start: Windows would not start it");
    // a child that ends and wrote nothing
    class Silent final : public ProbeRunner {
    public:
        bool start(const std::string& outPath, bool, std::string&) override { std::error_code ec; std::filesystem::remove(outPath, ec); return true; }   // no file: a child that wrote nothing
        bool running() override { return false; }
        int exitCode() const override { return 3; }
    };
    SdrPlayDiag e([] { return std::unique_ptr<ProbeRunner>(new Silent()); }, [] { return scratch().string(); });
    e.press();
    e.run();
    CHECK(e.running());
    e.poll();
    CHECK(!e.running() && e.report().empty() && e.view(false).status == "SDRplay diagnostic could not start: no file was written (exit 3)");
}

// A long random walk over every press: the child is started with the bias tee on only when the last answer to the question was yes, in the dialog that ran it.
void walk() {
    Spy spy;
    SdrPlayDiag d = make(spy);
    std::srand(12345);
    bool yesInThisDialog = false;
    bool dialog = false;
    for (int i = 0; i < 4000; ++i) {
        const int a = std::rand() % 8;
        const SdrPlayDiag::View before = d.view(false);
        switch (a) {
            case 0: d.press(); if (!d.running()) { dialog = true; yesInThisDialog = false; } break;
            case 1: d.tickBias(true); break;
            case 2: d.tickBias(false); if (dialog && !before.biasAsk) { yesInThisDialog = false; } break;
            case 3: if (before.biasAsk) { yesInThisDialog = true; } d.biasYes(); break;
            case 4: if (before.biasAsk) { yesInThisDialog = false; } d.biasCancel(); break;
            case 5: { const int starts = spy.starts; d.run(); if (spy.starts != starts) { CHECK(spy.lastBias == yesInThisDialog); dialog = false; } break; }
            case 6: d.cancel(); dialog = false; yesInThisDialog = false; break;
            default: d.poll(); break;
        }
        dialog = d.view(false).dialogShown;
        if (!dialog) { yesInThisDialog = false; }
        CHECK(!(d.view(false).biasOn && !yesInThisDialog));
    }
    CHECK(spy.starts > 20);
}

void ledger() {
    using namespace foxsdr::slintgaps;
    LedgerInputs in;
    in.slintX = 100; in.slintY = 200; in.slintW = 1600; in.slintH = 900; in.scale = 1.25f;
    const std::string line = formatPointerLedger(340, 560, 100, 200, 1600, 900, 92, 170, 1616, 939, in);
    CHECK(line == "os cursor 340,560 | client origin 100,200 client 1600x900 | window 92,170 1616x939 | toolkit window 100,200 1600x900 scale 1.25 | cursor in client 240,360");
    CHECK(pointerLedgerLine(LedgerInputs{}).empty());   // no window: nothing to say
}

}  // namespace

int main() {
    flow();
    failure();
    walk();
    ledger();
    return testSummary("test_slint_host_sdrplaydiag");
}
