// test_engine_api_hotplug.cpp - HOT-PLUG DETECTION IN THE ENGINE (engine serial lane A, row R10): core/sys_device_watch.hpp wired by engine_hotplug.cpp.
//   A  a burst of arrivals and a removal of USB radio interfaces (the same radio under both classes) is ONE settled action: fact.hotplug.bursts 1, .arrivals 3, .removals 1; the facts hold counts only;
//   B  an interface that is not a radio class (a monitor, a drive) is not counted and starts no burst;
//   C  an arrival brings a waiting busy retry forward (attempts back to 0, the last failure long ago); a removal alone does not;
//   D  starting twice makes one watcher; shutdown stops it (fact.hotplug.on 0) and a stopped engine ignores a late event;
//   E  nothing of the device's identity is kept: the serial number in an injected path appears in no fact, and the two source files print nothing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "wire1_rig.hpp"

namespace {

const std::string kRtlWinusb = R"(\\?\USB#VID_0BDA&PID_2838#SECRET123#{dee824ef-729b-4a0e-9c14-b7117d33a817})";
const std::string kRtlUsb = R"(\\?\USB#VID_0BDA&PID_2838#SECRET123#{a5dcbf10-6530-11d2-901f-00c04fb951ed})";
const std::string kMonitor = R"(\\?\DISPLAY#ABC1234#5&1a2b3c#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7})";

std::string read(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_hotplug\n");
    engine_api_test::hermetic("hotplug");
    Access::installHooks();
    Rig r;
    r.pump(10);
    CHECK_EQ(r.fact("hotplug.on"), std::string("0"));
    CHECK_EQ(r.fact("hotplug.bursts"), std::string("0"));
    Access::hotplugStart(r.eng, 300);
    Access::hotplugStart(r.eng, 300);   // D: twice is one
    CHECK(Access::hotplugRunning(r.eng));
    r.pump(3);
    CHECK_EQ(r.fact("hotplug.on"), std::string("1"));

    std::printf("A: a burst is one action\n");
    CHECK(Access::hotplugInject(r.eng, true, kRtlWinusb));
    CHECK(Access::hotplugInject(r.eng, true, kRtlUsb));
    CHECK(Access::hotplugInject(r.eng, true, kRtlUsb));
    CHECK(Access::hotplugInject(r.eng, false, kRtlWinusb));
    CHECK(r.until([&] { return r.fact("hotplug.bursts") == "1"; }, 8000));
    CHECK_EQ(r.fact("hotplug.bursts"), std::string("1"));
    CHECK_EQ(r.fact("hotplug.arrivals"), std::string("3"));
    CHECK_EQ(r.fact("hotplug.removals"), std::string("1"));

    std::printf("B: not a radio class\n");
    CHECK(Access::hotplugInject(r.eng, true, kMonitor));
    CHECK(Access::hotplugInject(r.eng, false, kMonitor));
    r.until([] { return false; }, 900);
    CHECK_EQ(r.fact("hotplug.bursts"), std::string("1"));
    CHECK_EQ(r.fact("hotplug.arrivals"), std::string("3"));

    std::printf("C: an arrival brings a busy retry forward\n");
    Access::armBusyRetry(r.eng, 3, 5.0);
    CHECK(Access::hotplugInject(r.eng, false, kRtlUsb));            // a removal alone
    CHECK(r.until([&] { return r.fact("hotplug.bursts") == "2"; }, 8000));
    CHECK_EQ(Access::retryAttempts(r.eng), 3);                      // untouched
    CHECK_EQ(Access::retryLastFail(r.eng), 5.0);
    CHECK(Access::hotplugInject(r.eng, true, kRtlWinusb));          // an arrival
    CHECK(r.until([&] { return r.fact("hotplug.bursts") == "3"; }, 8000));
    CHECK_EQ(Access::retryAttempts(r.eng), 0);
    CHECK(Access::retryLastFail(r.eng) < -1.0e8);

    std::printf("E: counts only\n");
    for (const char* key : {"hotplug.on", "hotplug.bursts", "hotplug.arrivals", "hotplug.removals", "radio.count", "radio.1.state", "radio.1.device", "radio.1.label"}) {
        CHECK(r.fact(key).find("SECRET123") == std::string::npos);
        CHECK(r.fact(key).find("0BDA") == std::string::npos);
    }
    const char* files[] = {"src/engine/engine_hotplug.cpp", "src/core/sys_device_watch.cpp"};
    for (const char* f : files) {
        const std::string text = read(std::string(CASCADE_SOURCE_DIR) + "/" + f);
        CHECK(!text.empty());
        for (const char* banned : {"printf(", "std::cout", "std::cerr", "stderr", "stdout", "puts("}) {
            std::size_t at = 0;
            bool found = false;
            while ((at = text.find(banned, at)) != std::string::npos) {
                // diagLogf( contains "printf(" only as "diagLogf(": the check is for the C stdio spellings at a word start
                const bool wordStart = at == 0 || !(std::isalnum(static_cast<unsigned char>(text[at - 1])) || text[at - 1] == '_');
                if (wordStart) { found = true; }
                at += 1;
            }
            CHECK(!found);
        }
    }

    std::printf("D: shutdown stops it\n");
    r.eng.shutdownQuiesce();
    CHECK(!Access::hotplugRunning(r.eng));
    CHECK(!Access::hotplugInject(r.eng, true, kRtlUsb));            // nobody listens
    return check::finish("test_engine_api_hotplug");
}
