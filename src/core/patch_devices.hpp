// patch_devices.hpp - which device a patch Radio opens, and where a patch
// speaker's sound goes, as keys. No I/O, no threads.
//
// RADIO KEYS (Node::device on a Radio):
//   "siggen"           the signal generator - not hardware, so any number of
//                      radios may use it at once
//   "<driver>|<args>"  a real radio: the driver key makeDeviceSource() knows
//                      ("rtlsdr", "hackrf", ..., "soapy") and the open args
//                      the device list gave for it
//
// ONE RADIO, ONE NODE (owner, 2026-09-23: "only allow one device to be used in
// one panel at a time"). A USB radio can be opened by one reader; a second open
// either fails or - worse, for some vendor stacks - succeeds and splits the
// sample stream between two readers. So two Radio nodes naming the same device
// is refused in the plan, and the device list greys it out.
//
// "THE SAME DEVICE" IS DECIDED BY SERIAL WHEN BOTH KEYS CARRY ONE. The same
// dongle can be listed twice, once by its native driver and once through
// SoapySDR, under different keys; the serial is what they share.
//
// OUTPUT KEYS (Node::device on an audio Sink):
//   "" or "wav"        a WAV file in the recordings folder (the default: the
//                      owner's rule is that sound goes to a file unless it is
//                      sent somewhere else)
//   "mp3"              an MP3 file (Windows' own encoder; WAV elsewhere)
//   "speakers"         the system's default sound output
//   "audio:<name>"     a named sound output device
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PATCH_DEVICES_HPP
#define CASCADE_CORE_PATCH_DEVICES_HPP

#include <cctype>
#include <string>

namespace cascade::core::patch {

inline constexpr const char* kGeneratorKey = "siggen";

inline bool isGeneratorKey(const std::string& key) { return key == kGeneratorKey; }

// The driver half of a hardware key ("rtlsdr" of "rtlsdr|serial=..."), or ""
// for the generator or anything that is not a hardware key.
inline std::string deviceDriver(const std::string& key) {
    const auto bar = key.find('|');
    return bar == std::string::npos ? std::string{} : key.substr(0, bar);
}

// The args half of a hardware key.
inline std::string deviceArgs(const std::string& key) {
    const auto bar = key.find('|');
    return bar == std::string::npos ? std::string{} : key.substr(bar + 1);
}

inline std::string makeDeviceKey(const std::string& driver, const std::string& args) {
    return driver + "|" + args;
}

// The value of `name` in "a=1,serial=00000001,b=2", case-insensitive on the
// name, trimmed; "" when absent.
inline std::string argField(const std::string& args, const std::string& name) {
    std::size_t pos = 0;
    while (pos <= args.size()) {
        std::size_t end = args.find(',', pos);
        if (end == std::string::npos) { end = args.size(); }
        const std::string field = args.substr(pos, end - pos);
        const std::size_t eq = field.find('=');
        if (eq != std::string::npos) {
            std::string k = field.substr(0, eq);
            std::string v = field.substr(eq + 1);
            const auto trim = [](std::string& s) {
                std::size_t a = 0;
                while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) { ++a; }
                std::size_t b = s.size();
                while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) { --b; }
                s = s.substr(a, b - a);
            };
            trim(k);
            trim(v);
            bool match = k.size() == name.size();
            for (std::size_t i = 0; match && i < k.size(); ++i) {
                match = std::tolower(static_cast<unsigned char>(k[i])) ==
                        std::tolower(static_cast<unsigned char>(name[i]));
            }
            if (match) { return v; }
        }
        if (end == args.size()) { break; }
        pos = end + 1;
    }
    return {};
}

// Whether two radio keys name the same physical radio. The generator is never
// "the same device" as anything - it is not a device. Empty keys (no radio
// chosen yet) never clash either.
inline bool sameDevice(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty() || isGeneratorKey(a) || isGeneratorKey(b)) { return false; }
    if (a == b) { return true; }
    const std::string sa = argField(deviceArgs(a), "serial");
    const std::string sb = argField(deviceArgs(b), "serial");
    return !sa.empty() && sa == sb;
}

// --- where a speaker's sound goes ------------------------------------------

enum class OutputKind { Wav, Mp3, Speakers, Device };

inline OutputKind outputKind(const std::string& key) {
    if (key == "mp3") { return OutputKind::Mp3; }
    if (key == "speakers") { return OutputKind::Speakers; }
    if (key.rfind("audio:", 0) == 0 && key.size() > 6) { return OutputKind::Device; }
    return OutputKind::Wav;   // "", "wav" and anything unrecognised: a file
}

// The device name of an "audio:<name>" key.
inline std::string outputDeviceName(const std::string& key) {
    return outputKind(key) == OutputKind::Device ? key.substr(6) : std::string{};
}

inline std::string makeOutputDeviceKey(const std::string& name) { return "audio:" + name; }

}  // namespace cascade::core::patch

#endif  // CASCADE_CORE_PATCH_DEVICES_HPP
