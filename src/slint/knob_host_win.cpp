// knob_host_win.cpp - the real listing and opening of the knob host (knob_host.hpp), over the HID layer of src/input. Windows only; elsewhere the listing is empty.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "knob_host.hpp"

#ifdef _WIN32
#include "input/hid_win.hpp"
#endif

namespace foxsdr::slintknobs {

#ifdef _WIN32
namespace {
struct Held {
    cascade::input::HidInputDevice dev;
};
std::vector<cascade::input::HidDeviceInfo>& lastListing() {
    static std::vector<cascade::input::HidDeviceInfo> v;
    return v;
}
}  // namespace

KnobHost::Env realEnv() {
    KnobHost::Env e;
    e.list = [] {
        std::vector<FoundDevice> out;
        auto& keep = lastListing();
        keep.clear();
        const cascade::input::HidEnumeration en = cascade::input::enumerateHid();   // reads each interface's attributes; reads no report
        for (const auto& d : en.supported) {
            if (!d.plausible) { continue; }
            FoundDevice f;
            f.id = d.stableId;
            f.name = d.name;
            f.preset = d.kind == cascade::input::HidKind::PowerMate ? 1 : 0;
            f.token = keep.size();
            keep.push_back(d);
            out.push_back(std::move(f));
        }
        return out;
    };
    e.open = [](const FoundDevice& f, cascade::input::EventQueue& q, std::string& error) -> std::shared_ptr<void> {
        const auto& keep = lastListing();
        if (f.token >= keep.size()) { error = "The device is no longer listed."; return nullptr; }
        auto h = std::make_shared<Held>();
        if (!h->dev.open(keep[f.token], q, error)) { return nullptr; }
        return h;
    };
    e.reports = [](const std::shared_ptr<void>& h) { return h ? std::static_pointer_cast<Held>(h)->dev.reports() : std::uint64_t{0}; };
    return e;
}
#else
KnobHost::Env realEnv() {
    KnobHost::Env e;
    e.list = [] { return std::vector<FoundDevice>(); };
    return e;
}
#endif

}  // namespace foxsdr::slintknobs
