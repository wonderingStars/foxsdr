// s4b_notify_win.hpp - screen lane S4b, G3-37b: the Windows desktop notification behind the NotificationSink interface of core/alert_rules.hpp.
//
// HOW IT SHOWS. A notification is a BALLOON of the notification area (Shell_NotifyIcon with NIF_INFO), which Windows 10 and 11 show as a toast: it needs no application identity, no Start-menu
// shortcut and writes nothing to the registry or the disk (the packaged-toast route would need both). Its icon is the system's own and is put into the notification area for ten seconds and removed.
// It is made by ONE thread of its own (a message-only window), started by the FIRST notify() and never by the constructor, so building one does nothing at all.
//
// WHEN IT IS INSTALLED. Only by a real run of the window (Engine::setDesktopNotifier from foxsdr-slint's main, and never in a bounded run or a test): every test and every scripted run keeps the
// engine's own default, no sink, or hands it a recording stand-in. The real notification is UNVERIFIED until the mouse drive: nothing here was ever shown on a screen.
//
// BOUNDS. At most six notifications a minute (the rest are dropped and counted: an alert flood is not a notification flood); the title is cut to 63 and the text to 255 UTF-16 units on a
// character boundary; the queue holds at most eight.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "core/alert_rules.hpp"

namespace cascade::engine::s4b {

// A notification's words as the notification area takes them: valid text, no control characters but a line break, the title at most 63 and the body at most 255 UTF-16 units, cut on a code point.
struct BalloonText {
    std::u16string title, body;
};
BalloonText balloonText(const std::string& title, const std::string& body);

class DesktopNotifier final : public cascade::core::alerts::NotificationSink {
public:
    DesktopNotifier() = default;
    ~DesktopNotifier() override;
    const char* name() const override { return "desktop"; }
    bool notify(const cascade::core::alerts::Notification& n, std::string* err) override;
    std::uint64_t shown() const { return shown_.load(); }
    std::uint64_t dropped() const { return dropped_.load(); }
    bool threadStarted() const { return started_.load(); }

private:
    void run();

    std::mutex m_;
    std::deque<BalloonText> queue_;
    std::thread thread_;
    std::atomic<bool> started_{false};
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> shown_{0}, dropped_{0};
    std::int64_t windowStartMs_ = 0;
    int inWindow_ = 0;
    void* hwnd_ = nullptr;
};

}  // namespace cascade::engine::s4b
