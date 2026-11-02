// test_process_resources.cpp - the `resources:` line (core/process_resources.hpp,
// 0.99.103): what this process holds of what the system hands out in limited
// numbers, read for real on this machine, and the line's fixed shape.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <string>

#include "core/diag_log.hpp"
#include "core/process_resources.hpp"
#include "test_check.hpp"

using cascade::core::ProcessResources;
using cascade::core::processResourcesLine;
using cascade::core::readProcessResources;

int main() {
    std::printf("test_process_resources\n");

    // --- the line's shape, from known numbers ------------------------------------
    {
        ProcessResources r;
        r.gdiObjects = 412;
        r.userObjects = 97;
        r.handles = 1234;
        r.memoryBytes = 812LL << 20;
        r.videoUsedBytes = 356LL << 20;
        r.videoBudgetBytes = 3900LL << 20;
        CHECK(processResourcesLine(r, "at start-up") ==
              "resources: gdi 412, user 97, handles 1234, memory 812 MB, video 356 of 3900 MB - at "
              "start-up");
        // No phrase: no dash.
        CHECK(processResourcesLine(r, nullptr) ==
              "resources: gdi 412, user 97, handles 1234, memory 812 MB, video 356 of 3900 MB");
        CHECK(processResourcesLine(r, "") ==
              "resources: gdi 412, user 97, handles 1234, memory 812 MB, video 356 of 3900 MB");
        // Rounded to the nearest megabyte, not truncated: 1.6 MB is 2.
        r.memoryBytes = (1LL << 20) + (1LL << 19) + 1;
        CHECK(processResourcesLine(r, nullptr).find("memory 2 MB") != std::string::npos);
        r.memoryBytes = (1LL << 20) + (1LL << 19) - 1;
        CHECK(processResourcesLine(r, nullptr).find("memory 1 MB") != std::string::npos);
        r.memoryBytes = 0;
        CHECK(processResourcesLine(r, nullptr).find("memory 0 MB") != std::string::npos);
    }

    // --- what could not be read is a question mark, in place ------------------------
    {
        const ProcessResources none;
        CHECK(processResourcesLine(none, "ten minutes on") ==
              "resources: gdi ?, user ?, handles ?, memory ? MB, video ? of ? MB - ten minutes on");
        ProcessResources some;
        some.handles = 5;
        some.videoUsedBytes = 7LL << 20;
        CHECK(processResourcesLine(some, nullptr) ==
              "resources: gdi ?, user ?, handles 5, memory ? MB, video 7 of ? MB");
    }

    // --- read for real: what this process certainly has -------------------------------
    {
        const auto t0 = std::chrono::steady_clock::now();
        const ProcessResources r = readProcessResources();
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const std::string line = processResourcesLine(r, "in the test");
        std::printf("    %s (%.1f ms to read)\n", line.c_str(), ms);
        // Any process has open handles and memory.
        CHECK(r.handles > 0);
        CHECK(r.memoryBytes > 0);
#if defined(_WIN32)
        // The GUI object counts answer for every process, a console one included -
        // zero is an answer, -1 is not.
        CHECK(r.gdiObjects >= 0);
        CHECK(r.userObjects >= 0);
        // Video memory is read through DXGI when the machine has it; a budget is
        // never smaller than what is in use, and a read never comes back negative
        // other than as "unknown".
        CHECK(r.videoUsedBytes >= -1 && r.videoBudgetBytes >= -1);
        if (r.videoUsedBytes >= 0) { CHECK(r.videoBudgetBytes >= r.videoUsedBytes); }
#endif
        // A read is made on the window's thread at a tear-off: it has to be quick.
        // Generous, so a loaded machine cannot fail it; the figure is printed above.
        CHECK(ms < 500.0);
        // The line fits the log's line.
        CHECK(line.size() < cascade::core::DiagLog::kLineBytes);
        // And reading twice gives the same shape - nothing is consumed by a read.
        const ProcessResources again = readProcessResources();
        CHECK((again.handles > 0) == (r.handles > 0));
        CHECK((again.videoUsedBytes >= 0) == (r.videoUsedBytes >= 0));
    }

    return testSummary("test_process_resources");
}
