// Tests for the command-line parser (core/cli_options.hpp) and the status-bar clock (core/status_clock.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/cli_options.hpp"
#include "core/status_clock.hpp"
#include "test_check.hpp"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#endif

namespace {
namespace cli = cascade::core::cli;
namespace sc = cascade::core::statusclock;
using Args = std::vector<std::string>;

bool hasError(const cli::ParseResult& r, const std::string& fragment) {
    for (const std::string& e : r.errors) { if (e.find(fragment) != std::string::npos) { return true; } }
    return false;
}

void testFrequencies() {
    struct Row { const char* text; double hz; };
    const Row good[] = {
        {"7074000", 7074000}, {"7074000Hz", 7074000}, {"7074000hz", 7074000}, {"7074k", 7074000}, {"7074kHz", 7074000}, {"7074K", 7074000}, {"7.074M", 7074000}, {"7.074MHz", 7074000}, {"7.074m", 7074000},
        {"145.5M", 145500000}, {"145500k", 145500000}, {"1.296G", 1296000000}, {"1.296GHz", 1296000000}, {"2.4M", 2400000}, {"0.5M", 500000}, {".5M", 500000}, {"1", 1}, {"1Hz", 1},
        {"10000000000000", 1e13}, {"123456789.123456789", 123456789.123456789}, {"00007", 7}, {"1.", 1},
    };
    int n = 0;
    for (const Row& r : good) {
        const auto p = cli::parse({"--freq", r.text});
        if (!p.ok || !p.options.frequencyHz || std::fabs(*p.options.frequencyHz - r.hz) > (std::string(r.text) == "123456789.123456789" ? 1e-6 : 0.0)) { std::printf("  '%s': ok=%d hz=%.9g (want %.9g)\n", r.text, p.ok, p.options.frequencyHz ? *p.options.frequencyHz : -1.0, r.hz); }
        CHECK(p.ok && p.options.frequencyHz && std::fabs(*p.options.frequencyHz - r.hz) <= (std::string(r.text) == "123456789.123456789" ? 1e-6 : 0.0));
        ++n;
    }
    const char* bad[] = {"", "abc", "M", "-5", "+5", "1e6", "1E6", "1,5M", "1.2.3", "1 M", " 7M", "7M ", "7.074 MHz", "0", "0Hz", "0.5", "0.9999", "10000000000001", "99999999999999999", "1.1234567890123", "inf", "nan", "NaN",
                         "0x10", "7mm", "7x", "7kk", "7MHzz", "7,074,000", "7_074_000", "\xD9\xA7M", "1/2", "7M\n", "-0", "--5"};
    for (const char* b : bad) {
        const auto p = cli::parse({"--freq", b});
        if (p.ok) { std::printf("  NOT refused: '%s'\n", b); }
        CHECK(!p.ok && !p.options.frequencyHz && hasError(p, "--freq"));
        ++n;
    }
    // the same values through the other two frequency options
    const auto p = cli::parse({"--iq-file", "x.cu8", "--iq-rate", "2.4M", "--iq-center", "433.92M"});
    CHECK(p.ok && *p.options.iqSampleRateHz == 2400000 && *p.options.iqCenterHz == 433920000);
    CHECK(!cli::parse({"--iq-file", "x.cu8", "--iq-rate", "fast"}).ok && !cli::parse({"--iq-file", "x.cu8", "--iq-center", "0"}).ok);
    std::printf("frequencies: %d rows (22 accepted with units Hz k M G, case-insensitive; the rest refused: signs, exponents, separators, spaces, zero, over 10 THz, nan, hex)\n", n);
}

void testOptions() {
    // forms
    auto p = cli::parse({"--freq", "7.074M", "--mode", "usb", "--radio", "B200 #1", "--start"});
    CHECK(p.ok && *p.options.frequencyHz == 7074000 && *p.options.mode == "USB" && *p.options.radio == "B200 #1" && p.options.startRunning && !p.options.iqFile && !p.options.help);
    p = cli::parse({"--freq=7.074M", "--mode=Cw", "--radio=rtl|0"});
    CHECK(p.ok && *p.options.frequencyHz == 7074000 && *p.options.mode == "CW" && *p.options.radio == "rtl|0");
    p = cli::parse({"-f", "145.5M", "-m", "NFM", "-r", "x", "-s", "-p", "C:\\Users\\me\\prof"});
    CHECK(p.ok && *p.options.frequencyHz == 145500000 && *p.options.mode == "NFM" && p.options.startRunning && *p.options.profileDir == "C:\\Users\\me\\prof");
    p = cli::parse({"-f7.074M", "-mUSB"});
    CHECK(p.ok && *p.options.frequencyHz == 7074000 && *p.options.mode == "USB");
    p = cli::parse({"--freq=7.074M=3"});                                           // an '=' inside the value stays in the value
    CHECK(!p.ok);
    // every mode name, any case, canonical spelling out
    for (const char* m : {"NFM", "WFM", "AM", "DSB", "USB", "LSB", "CW", "RAW"}) {
        std::string lower = m; for (char& c : lower) { c = static_cast<char>(c - 'A' + 'a'); }
        CHECK(*cli::parse({"-m", lower}).options.mode == m && *cli::parse({"-m", m}).options.mode == m);
    }
    p = cli::parse({"--mode", "fm"});
    CHECK(!p.ok && hasError(p, "is not a mode") && hasError(p, "NFM, WFM"));
    // an empty command line
    p = cli::parse({});
    CHECK(p.ok && !p.options.frequencyHz && !p.options.mode && !p.options.radio && !p.options.startRunning && !p.options.iqFile && !p.options.profileDir && !p.options.help);
    // help
    p = cli::parse({"--help"}); CHECK(p.ok && p.options.help);
    p = cli::parse({"-h"}); CHECK(p.ok && p.options.help);
    CHECK(!cli::parse({"--help=1"}).ok);
    // errors are all reported, not just the first
    p = cli::parse({"--freq", "x", "--mode", "y", "--bogus", "--radio"});
    CHECK(!p.ok && p.errors.size() == 4);
    // unknown option, with a suggestion when close
    p = cli::parse({"--frq", "7M"});
    CHECK(!p.ok && hasError(p, "unknown option --frq") && hasError(p, "did you mean --freq"));
    p = cli::parse({"--zzzzzzzz"}); CHECK(!p.ok && hasError(p, "unknown option") && !hasError(p, "did you mean"));
    CHECK(!cli::parse({"-x"}).ok && !cli::parse({"-xyz"}).ok);
    // duplicates
    for (const Args& a : {Args{"-f", "1M", "--freq", "2M"}, Args{"-m", "AM", "-m", "AM"}, Args{"-s", "--start"}, Args{"-r", "a", "-r", "b"}, Args{"-p", "a", "-p", "b"}, Args{"-h", "-h"}}) {
        const auto d = cli::parse(a);
        CHECK(!d.ok && hasError(d, "more than once"));
    }
    // missing values
    for (const char* o : {"--freq", "--mode", "--radio", "--iq-file", "--profile", "--iq-rate", "--iq-center", "-f", "-m", "-r", "-i", "-p"}) {
        const auto d = cli::parse({o});
        CHECK(!d.ok && hasError(d, "needs a value"));
    }
    // a value that looks like an option is still the value (and is then validated)
    p = cli::parse({"--freq", "--start"}); CHECK(!p.ok && hasError(p, "--freq '--start'") && !p.options.startRunning);
    // flags take no value
    CHECK(!cli::parse({"--start=yes"}).ok);
    // radio text
    CHECK(!cli::parse({"--radio", ""}).ok);
    CHECK(!cli::parse({"--radio", std::string("a\x01" "b")}).ok && !cli::parse({"--radio", std::string("a\0b", 3)}).ok && !cli::parse({"--radio", "a\nb"}).ok);
    CHECK(cli::parse({"--radio", std::string(128, 'x')}).ok && !cli::parse({"--radio", std::string(129, 'x')}).ok);
    CHECK(cli::parse({"--radio", "caf\xC3\xA9 \xE2\x82\xAC"}).ok);
    // the end of options
    p = cli::parse({"--", "-weird.wav"});
    CHECK(p.ok && *p.options.iqFile == "-weird.wav");
    p = cli::parse({"-f", "1M", "--", "a.wav"}); CHECK(p.ok && *p.options.iqFile == "a.wav" && *p.options.frequencyHz == 1e6);
    // limits
    Args many(65, "-s");
    CHECK(!cli::parse(many).ok);
    CHECK(!cli::parse({"--radio", std::string(5000, 'x')}).ok);
    CHECK(!cli::parse({""}).ok);                                                   // an empty argument
    std::printf("options: long, =, short, attached short forms; every mode in any case; help; all errors reported; unknown-option suggestion; duplicates, missing values, flags with values; radio text; --; limits\n");
}

void testIqFile() {
    // positional
    auto p = cli::parse({"C:\\rec\\cap_20260115_143059Z.wav"});
    CHECK(p.ok && *p.options.iqFile == "C:\\rec\\cap_20260115_143059Z.wav");
    p = cli::parse({"-i", "x.sigmf-meta"}); CHECK(p.ok && *p.options.iqFile == "x.sigmf-meta");
    for (const char* ext : {".wav", ".WAV", ".rf64", ".cu8", ".CS16", ".cf32", ".sigmf-meta", ".sigmf-data"}) { CHECK(cli::parse({std::string("a") + ext}).ok); }
    for (const char* name : {"a.mp3", "a", "a.", ".wav", "a.wav.txt", "dir.wav\\file", "a.flac"}) {
        const auto d = cli::parse({"--iq-file", name});
        if (std::string(name) == ".wav") { CHECK(!d.ok); }                           // a dot file with no stem has no extension
        else { CHECK(!d.ok); }
        CHECK(hasError(d, "not an I/Q file"));
    }
    // two files, or a file twice
    p = cli::parse({"a.wav", "b.wav"}); CHECK(!p.ok && hasError(p, "more than one file"));
    p = cli::parse({"a.wav", "--iq-file", "b.wav"}); CHECK(!p.ok && hasError(p, "as well as --iq-file"));
    p = cli::parse({"--iq-file", "a.wav", "--iq-file", "b.wav"}); CHECK(!p.ok && hasError(p, "more than once"));
    // a file and a radio together
    p = cli::parse({"--iq-file", "a.wav", "--radio", "B200"}); CHECK(!p.ok && hasError(p, "cannot be used together"));
    p = cli::parse({"a.wav", "-r", "B200"}); CHECK(!p.ok && hasError(p, "cannot be used together"));
    // rate and centre only for raw files
    CHECK(cli::parse({"a.cu8", "--iq-rate", "2.4M", "--iq-center", "100M"}).ok);
    CHECK(cli::parse({"a.cf32", "--iq-rate", "2.4M"}).ok && cli::parse({"a.cs16", "--iq-center", "100M"}).ok);
    p = cli::parse({"a.wav", "--iq-rate", "48k"}); CHECK(!p.ok && hasError(p, "only apply to raw files"));
    p = cli::parse({"a.sigmf-meta", "--iq-center", "1M"}); CHECK(!p.ok && hasError(p, "only apply to raw files"));
    p = cli::parse({"--iq-rate", "48k"}); CHECK(!p.ok && hasError(p, "need an I/Q file"));
    p = cli::parse({"--iq-center", "1M", "-f", "1M"}); CHECK(!p.ok && hasError(p, "need an I/Q file"));
    // path characters
    for (const char* b : {"a<b.wav", "a>b.wav", "a\"b.wav", "a|b.wav", "a?b.wav", "a*b.wav", "a:b.wav", "C:\\a:b.wav", "::.wav", "1:\\a.wav"}) {
        const auto d = cli::parse({"--iq-file", b});
        if (d.ok) { std::printf("  path NOT refused: %s\n", b); }
        CHECK(!d.ok);
    }
    for (const char* g : {"C:\\Users\\me\\a.wav", "c:/rec/a.wav", "\\\\server\\share\\a.wav", "/home/me/a.wav", "rel/a.wav", ".\\a.wav", "..\\a.wav", "a b.wav", "caf\xC3\xA9.wav", "a'b.wav", "a;b&c.wav"}) { CHECK(cli::parse({"--iq-file", g}).ok); }
    CHECK(!cli::parse({"--iq-file", std::string(1025, 'a') + ".wav"}).ok);
    CHECK(!cli::parse({"--iq-file", std::string("a\0b.wav", 7)}).ok);
    // existence callbacks
    cli::ParseConfig cfg;
    cfg.fileExists = [](const std::string& f) { return f == "present.wav"; };
    cfg.dirExists = [](const std::string& d) { return d == "C:\\prof"; };
    CHECK(cli::parse({"present.wav"}, cfg).ok);
    p = cli::parse({"absent.wav"}, cfg); CHECK(!p.ok && hasError(p, "does not exist"));
    CHECK(cli::parse({"-p", "C:\\prof"}, cfg).ok);
    p = cli::parse({"-p", "C:\\nope"}, cfg); CHECK(!p.ok && hasError(p, "profile folder"));
    CHECK(cli::parse({"absent.wav"}).ok);                                            // no callback: syntax only
    // profile text
    CHECK(!cli::parse({"-p", ""}).ok && !cli::parse({"-p", "a|b"}).ok && !cli::parse({"-p", std::string(1025, 'x')}).ok);
    // a full realistic line
    p = cli::parse({"--profile", "D:\\FoxProfiles\\lab", "--iq-file", "D:\\samples\\iq_145.5MHz_2.4Msps.cu8", "--iq-rate", "2.4M", "--iq-center", "145.5M", "--mode", "nfm", "--freq", "145.5M", "--start"});
    CHECK(p.ok && *p.options.mode == "NFM" && *p.options.iqSampleRateHz == 2400000 && p.options.startRunning);
    // parseMain skips argv[0] and survives null entries
    const char* argv[] = {"foxsdr.exe", "-f", "7M", nullptr};
    p = cli::parseMain(3, argv); CHECK(p.ok && *p.options.frequencyHz == 7e6);
    const char* argv2[] = {"foxsdr.exe", nullptr}; p = cli::parseMain(2, argv2); CHECK(!p.ok);   // a null argument reads as an empty one
    p = cli::parseMain(1, argv); CHECK(p.ok);
    // custom mode and extension lists
    cli::ParseConfig c2; c2.modes = {"SAM"}; c2.iqExtensions = {".iq"};
    CHECK(cli::parse({"-m", "sam"}, c2).ok && !cli::parse({"-m", "USB"}, c2).ok && cli::parse({"a.iq"}, c2).ok && !cli::parse({"a.wav"}, c2).ok);
    std::printf("I/Q file and profile: positional and -i, 8 extensions, conflicts with --radio, raw-only rate and centre, Windows path characters, existence callbacks, a realistic full line\n");
}

void testPassThrough() {
    // the options the start-up code already owns (engine/process_boot.cpp: --config PATH, --use-config, --frames N) are skipped, not errors
    cli::ParseConfig cfg;
    cfg.passThrough = {{"--config", true}, {"--use-config", false}, {"--frames", true}};
    auto p = cli::parse({"--config", "x.json", "--use-config", "--frames=10", "-f", "7M"}, cfg);
    CHECK(p.ok && *p.options.frequencyHz == 7e6);
    CHECK(p.passedThrough == (Args{"--config", "x.json", "--use-config", "--frames=10"}));
    // without the pass-through list the same words are unknown options
    p = cli::parse({"--use-config"});
    CHECK(!p.ok && hasError(p, "unknown option --use-config") && p.passedThrough.empty());
    // a pass-through option with a missing value does not swallow what is not there, and an option after it still parses
    p = cli::parse({"--frames"}, cfg); CHECK(p.ok && p.passedThrough == (Args{"--frames"}));
    p = cli::parse({"--config", "a.json", "--bogus"}, cfg); CHECK(!p.ok && hasError(p, "unknown option --bogus") && p.passedThrough.size() == 2);
    // a pass-through option's value is not read as a file name or an option
    p = cli::parse({"--config", "settings.wav", "-s"}, cfg); CHECK(p.ok && !p.options.iqFile && p.options.startRunning);
    std::printf("pass-through: the start-up's own options are skipped and reported, unknown ones still refused, a value is not mistaken for a file\n");
}

void testHelp() {
    const std::string h = cli::helpText();
    CHECK(h.rfind("Usage: foxsdr [options] [I/Q file]\n", 0) == 0);
    for (const cli::OptionDoc& o : cli::optionTable()) {
        CHECK(h.find(o.longName) != std::string::npos);
        if (o.shortName[0] != '\0') { CHECK(h.find(o.shortName) != std::string::npos); }
        CHECK(h.find(o.description) != std::string::npos);
    }
    CHECK(cli::helpText("x.exe").rfind("Usage: x.exe", 0) == 0);
    // every documented option is accepted by the parser (the table and the parser cannot drift apart)
    int accepted = 0;
    for (const cli::OptionDoc& o : cli::optionTable()) {
        Args a = {o.longName};
        const std::string name = o.longName;
        if (o.argument[0] != '\0') {
            if (name == "--freq" || name == "--iq-rate" || name == "--iq-center") { a.push_back("1M"); }
            else if (name == "--mode") { a.push_back("USB"); }
            else if (name == "--iq-file") { a.push_back("a.cu8"); }
            else { a.push_back("value"); }
        }
        if (name == "--iq-rate" || name == "--iq-center") { a.insert(a.begin(), {"--iq-file", "a.cu8"}); }
        const auto p = cli::parse(a);
        if (!p.ok) { std::printf("  documented option not accepted: %s\n", o.longName); }
        CHECK(p.ok);
        if (p.ok) { ++accepted; }
        if (o.shortName[0] != '\0' && name != "--iq-rate") {
            Args b = {o.shortName};
            if (o.argument[0] != '\0') { b.push_back(name == "--freq" ? "1M" : (name == "--mode" ? "USB" : (name == "--iq-file" ? "a.cu8" : "value"))); }
            CHECK(cli::parse(b).ok);
        }
    }
    CHECK(accepted == static_cast<int>(cli::optionTable().size()));
    std::printf("help: every option of the table appears in the text and is accepted by the parser (%d of %zu)\n", accepted, cli::optionTable().size());
}

void testFuzz() {
    std::mt19937 g(2026);
    const char* pieces[] = {"--freq", "-f", "--mode", "-m", "--radio", "-r", "--start", "-s", "--iq-file", "-i", "--iq-rate", "--iq-center", "--profile", "-p", "--help", "--", "-", "7.074M", "1e6", "USB", "x.cu8", "a.wav", "C:\\a", "a:b",
                            "", "=", "--freq=7M", "--mode=am", "\xFF\xFE", "9999999999999999", "0", ".", "1.2.3", "M", "k", "caf\xC3\xA9", "--iq-file=a.sigmf-meta"};
    int ok = 0, bad = 0;
    for (int i = 0; i < 200000; ++i) {
        Args a;
        const int n = static_cast<int>(g() % 8);
        for (int k = 0; k < n; ++k) {
            if (g() % 6 == 0) { std::string r; const int len = static_cast<int>(g() % 10); for (int j = 0; j < len; ++j) { r.push_back(static_cast<char>(g() % 256)); } a.push_back(r); }
            else { a.push_back(pieces[g() % (sizeof(pieces) / sizeof(pieces[0]))]); }
        }
        const auto p = cli::parse(a);
        if (!p.ok) { ++bad; CHECK(!p.errors.empty()); continue; }
        ++ok;
        const cli::Options& o = p.options;
        bool inv = true;
        if (o.frequencyHz && !(*o.frequencyHz >= 1.0 && *o.frequencyHz <= 1e13)) { inv = false; }
        if (o.iqSampleRateHz && !(*o.iqSampleRateHz >= 1.0 && *o.iqSampleRateHz <= 1e13)) { inv = false; }
        if (o.iqCenterHz && !(*o.iqCenterHz >= 1.0 && *o.iqCenterHz <= 1e13)) { inv = false; }
        if (o.radio && (o.radio->empty() || o.radio->size() > 128)) { inv = false; }
        if (o.radio) { for (unsigned char c : *o.radio) { if (c < 32 || c == 127) { inv = false; } } }
        if (o.iqFile && o.radio) { inv = false; }
        if (o.iqFile) { const std::string e = cli::detail::extensionOf(*o.iqFile); if (e != ".wav" && e != ".rf64" && e != ".cu8" && e != ".cs16" && e != ".cf32" && e != ".sigmf-meta" && e != ".sigmf-data") { inv = false; } }
        if ((o.iqSampleRateHz || o.iqCenterHz) && !o.iqFile) { inv = false; }
        if (o.mode) { bool known = false; for (const char* m : {"NFM", "WFM", "AM", "DSB", "USB", "LSB", "CW", "RAW"}) { if (*o.mode == m) { known = true; } } if (!known) { inv = false; } }
        if (!inv) { std::printf("  invariant broken by an accepted command line\n"); }
        CHECK(inv);
    }
    CHECK(ok > 5000 && bad > 5000);
    std::printf("fuzz: 200000 random command lines (hostile bytes, option fragments): %d accepted (every accepted one satisfies all option invariants), %d refused with at least one message, no crash\n", ok, bad);
}

void testClock() {
    constexpr std::int64_t kWed = 1794415314123LL;     // 2026-11-11T16:41:54.123Z
    sc::ClockStyle style;
    CHECK(sc::format(kWed, 0, style, true) == "16:41:54 UTC");
    CHECK(sc::format(kWed, 0, style, false) == "16:41:54 UTC");
    CHECK(sc::format(kWed, 60, style) == "17:41:54 UTC+01:00");
    CHECK(sc::format(kWed, -300, style) == "11:41:54 UTC-05:00");
    CHECK(sc::format(kWed, 330, style) == "22:11:54 UTC+05:30");        // India
    CHECK(sc::format(kWed, 345, style) == "22:26:54 UTC+05:45");        // Nepal
    CHECK(sc::format(kWed, -210, style) == "13:11:54 UTC-03:30");       // Newfoundland
    CHECK(sc::format(kWed, 840, style) == "06:41:54 UTC+14:00");        // the extremes in use
    CHECK(sc::format(kWed, -720, style) == "04:41:54 UTC-12:00");
    CHECK(sc::format(kWed, 15 * 60, style) == "16:41:54 UTC");          // out of range reads as 0
    CHECK(sc::format(kWed, -15 * 60, style) == "16:41:54 UTC");
    CHECK(sc::format(kWed, 0, style, false) == "16:41:54 UTC");         // a zero offset named UTC even when asked for "local"
    style.showSeconds = false; CHECK(sc::format(kWed, 0, style, true) == "16:41 UTC");
    style.showZone = false; CHECK(sc::format(kWed, 0, style, true) == "16:41");
    style.showDate = true; CHECK(sc::format(kWed, 0, style, true) == "2026-11-11 16:41");
    style.showSeconds = true; CHECK(sc::format(kWed, 120, style) == "2026-11-11 18:41:54");
    // the date follows the offset across midnight and the year
    constexpr std::int64_t kNye = 1798759800000LL;   // 2026-12-31T23:30:00Z
    CHECK(sc::format(kNye, 60, style) == "2027-01-01 00:30:00");
    CHECK(sc::format(kNye, -60, style) == "2026-12-31 22:30:00");
    // 12-hour
    sc::ClockStyle h12; h12.hour12 = true; h12.showZone = false;
    CHECK(sc::format(1794355200000LL, 0, h12) == "12:00:00 AM");      // 2026-11-11T00:00:00Z
    CHECK(sc::format(1794355200000LL + 3600000, 0, h12) == "01:00:00 AM");
    CHECK(sc::format(1794355200000LL + 12 * 3600000, 0, h12) == "12:00:00 PM");
    CHECK(sc::format(1794355200000LL + 13 * 3600000 + 5 * 60000, 0, h12) == "01:05:00 PM");
    CHECK(sc::format(1794355200000LL + 23 * 3600000 + 59 * 60000 + 59000, 0, h12) == "11:59:59 PM");
    // before 1970 and at the epoch
    sc::ClockStyle dt; dt.showDate = true; dt.showZone = true;
    CHECK(sc::format(0, 0, dt, true) == "1970-01-01 00:00:00 UTC");
    CHECK(sc::format(-1, 0, dt, true) == "1969-12-31 23:59:59 UTC");
    // both clocks
    const sc::DualClock d = sc::dual(kWed, 60);
    CHECK(d.utc == "16:41:54 UTC" && d.local == "17:41:54 UTC+01:00");
    CHECK(sc::dual(kWed, 0).local == "16:41:54 UTC");
    // zone names
    CHECK(sc::zoneName(0, true) == "UTC" && sc::zoneName(0, false) == "UTC" && sc::zoneName(60, false) == "UTC+01:00" && sc::zoneName(-570, false) == "UTC-09:30" && sc::zoneName(60, true) == "UTC");
    // ticks on the second
    CHECK(sc::msToNextSecond(0) == 1000 && sc::msToNextSecond(1) == 999 && sc::msToNextSecond(999) == 1 && sc::msToNextSecond(1000) == 1000 && sc::msToNextSecond(kWed) == 877 && sc::msToNextSecond(-1) == 1 && sc::msToNextSecond(-1000) == 1000);
    // a timer that sleeps msToNextSecond each time lands on the second every time: 5000 simulated ticks starting at an arbitrary millisecond
    std::int64_t t = kWed; int offBeat = 0;
    for (int i = 0; i < 5000; ++i) { t += sc::msToNextSecond(t); if (t % 1000 != 0) { ++offBeat; } }
    CHECK(offBeat == 0);
    // the clock text over 100000 consecutive seconds matches an independent count (seconds field cycles 0..59, minutes 0..59, hours 0..23)
    int bad = 0; int hh = 16, mm = 41, ss = 54;
    sc::ClockStyle plain; plain.showZone = false;
    for (int i = 0; i < 100000; ++i) {
        char buf[16]; std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hh, mm, ss);
        if (sc::format(kWed + i * 1000LL, 0, plain) != buf) { ++bad; }
        if (++ss == 60) { ss = 0; if (++mm == 60) { mm = 0; if (++hh == 24) { hh = 0; } } }
    }
    CHECK(bad == 0);
    std::printf("clock: UTC and 9 offsets (incl. +05:30, +05:45, -03:30), midnight and year rollover, 12-hour, before the epoch, dual clock, 5000 timer ticks stay on the second, 100000 consecutive seconds match an independent count\n");
}

void testSystemOffset() {
    // with the C library's time zone forced by the TZ variable (the Windows C runtime knows the simple POSIX form, US daylight rules)
    const std::int64_t jan = 1767225600000LL + 12 * 3600000LL;   // 2026-01-01T12:00:00Z
    const std::int64_t jul = 1782907200000LL;                    // 2026-07-01T12:00:00Z
#if defined(_WIN32)
    const char* old = std::getenv("TZ");
    const std::string saved = old != nullptr ? old : "";
    _putenv("TZ=UTC0"); _tzset();
    CHECK(sc::systemUtcOffsetMinutes(jan) == 0 && sc::systemUtcOffsetMinutes(jul) == 0);
    _putenv("TZ=EST5EDT"); _tzset();
    const int winter = sc::systemUtcOffsetMinutes(jan), summer = sc::systemUtcOffsetMinutes(jul);
    CHECK(winter == -300);
    CHECK(summer == -240);                                       // daylight saving followed at that instant, not the current one
    _putenv("TZ=JST-9"); _tzset();
    CHECK(sc::systemUtcOffsetMinutes(jan) == 540);
    _putenv("TZ=IST-5:30"); _tzset();
    CHECK(sc::systemUtcOffsetMinutes(jan) == 330);              // a half-hour zone
    _putenv(saved.empty() ? "TZ=" : (std::string("TZ=") + saved).c_str()); _tzset();
#else
    SKIP_LINUX("system offset test uses the Windows C runtime's TZ handling");
#endif
    // the machine's own zone, printed for a manual comparison with the operating system's own answer (not asserted)
    const std::int64_t nowMs = 1794415314123LL;
    std::printf("system offset on this PC at 2026-11-11T16:41:54Z: %d minutes; at 2026-07-01T12:00:00Z: %d minutes\n", sc::systemUtcOffsetMinutes(nowMs), sc::systemUtcOffsetMinutes(jul));
}

}  // namespace

int main() {
    testFrequencies();
    testOptions();
    testIqFile();
    testPassThrough();
    testHelp();
    testFuzz();
    testClock();
    testSystemOffset();
    return testSummary("test_cli_clock");
}
