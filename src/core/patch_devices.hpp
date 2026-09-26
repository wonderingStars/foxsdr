// patch_devices.hpp - which device a patch Radio opens, and where a patch
// speaker's sound goes, as keys. No I/O, no threads.
//
// RADIO KEYS (Node::device on a Radio):
//   "siggen"           the signal generator - not hardware, so any number of
//                      radios may use it at once
//   "<driver>|<args>"  a real radio: the driver key makeDeviceSource() knows
//                      ("rtlsdr", "hackrf", ..., "soapy") and the open args
//                      the device list gave for it
//   "iqfile|path=<p>"  an I/Q RECORDING (0.99.40): a 2-channel WAV that
//                      source/iq_file_source.hpp plays on a loop, paced to
//                      real time by the patch radio's reader. EVERYTHING after
//                      "path=" is the path, byte for byte - it is never read
//                      through argField's "a=1,b=2" grammar, because a Windows
//                      file name may hold a comma, and even "serial=".
//                      Its rate is the file's (deviceSetsItsOwnRate); its
//                      centre is the node's frequency, which names the air
//                      frequency the recording is baseband around - nothing
//                      is tuned. The same file on two Radios is the SAME
//                      device, by the rule below, exactly as for hardware.
//
// ONE RADIO, ONE NODE (owner, 2026-09-23: "only allow one device to be used in
// one panel at a time"). A USB radio can be opened by one reader; a second open
// either fails or - worse, for some vendor stacks - succeeds and splits the
// sample stream between two readers. So two Radio nodes naming the same device
// is refused in the plan, and the device list greys it out.
//
// "THE SAME DEVICE" IS DECIDED BY SERIAL WHEN BOTH KEYS CARRY ONE. The same
// dongle can be listed twice, once by its native driver and once through
// SoapySDR, under different keys; the serial is what they share. When either
// side has no serial, a native row and a SoapySDR row of the same family are
// taken to be the same dongle (see sameDevice).
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
#include <cstdio>
#include <string>

namespace cascade::core::patch {

inline constexpr const char* kGeneratorKey = "siggen";

inline bool isGeneratorKey(const std::string& key) { return key == kGeneratorKey; }

// --- an I/Q recording (0.99.40) ----------------------------------------------

inline constexpr const char* kIqFileDriver = "iqfile";
inline constexpr const char* kIqFilePrefix = "iqfile|path=";

// Whether `key` names a recording - and a file: "iqfile|path=" alone does not.
inline bool isIqFileKey(const std::string& key) {
    const std::string prefix(kIqFilePrefix);
    return key.size() > prefix.size() && key.compare(0, prefix.size(), prefix) == 0;
}

inline std::string makeIqFileKey(const std::string& path) { return kIqFilePrefix + path; }

// The path a recording key names, whole; "" for any other key.
inline std::string iqFilePath(const std::string& key) {
    return isIqFileKey(key) ? key.substr(std::string(kIqFilePrefix).size()) : std::string{};
}

// A path as the file system tells files apart: on Windows the case and the
// direction of the slashes do not matter, so "C:\Rec\A.wav" and "c:/rec/a.WAV"
// are one file. Elsewhere a path is exactly itself.
inline std::string iqFileIdentity(const std::string& path) {
#if defined(_WIN32)
    std::string out = path;
    for (char& c : out) {
        if (c == '/') { c = '\\'; }
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
#else
    return path;
#endif
}

// WHOSE RATE A RADIO RUNS AT. A radio's rate is a setting it is asked for; a
// recording's is a property of the file (IqFileSource refuses a new one), so
// the node's rate follows the file and never the other way round.
inline bool deviceSetsItsOwnRate(const std::string& key) { return isIqFileKey(key); }

// What makes a patch radio a different thing to OPEN: its device and, for a
// radio, the rate it is asked for - changing either reopens it. A recording's
// rate is its own, so it is left out: the node taking the file's rate after
// opening must not reopen the file, and then again, for ever.
inline std::string radioOpenIdentity(const std::string& device, double rateHz) {
    if (deviceSetsItsOwnRate(device)) { return device; }
    char buf[48];
    std::snprintf(buf, sizeof(buf), "@%.17g", rateHz);
    return device + buf;
}

// The key a radio's up- or down-converter is remembered under
// (core/freq_converter.hpp): a recording shares the receiver's one for I/Q
// files ("file", converterRadioKey's), every other radio is its own key.
inline std::string converterKeyForDevice(const std::string& key) {
    return isIqFileKey(key) ? std::string("file") : key;
}

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

// The native driver key for a SoapySDR module's driver name - "rtlsdr" for
// SoapyRTLSDR's "rtlsdr", "mirisdr" for SoapyMiri's "miri", "rx888" for
// SoapySDDC's "sddc" - or "" for a module no native driver shares hardware
// with (a B200, a LimeSDR). `soapyDriver` is lower case. The Pluto is absent
// on purpose: SoapyPlutoSDR addresses a board by URI, not by a USB identity.
// THE ONE TABLE: gui::nativeKeyForSoapyDriver answers from it too.
inline std::string nativeFamilyForSoapyDriver(const std::string& soapyDriver) {
    if (soapyDriver == "rtlsdr" || soapyDriver == "hackrf" || soapyDriver == "airspy" ||
        soapyDriver == "airspyhf" || soapyDriver == "sdrplay") {
        return soapyDriver;
    }
    if (soapyDriver == "miri") { return "mirisdr"; }
    if (soapyDriver == "sddc") { return "rx888"; }
    return std::string();
}

namespace detail {

// Whether one key is a native row and the other a SoapySDR row whose module
// drives the same family of hardware ("rtlsdr|..." and "soapy|driver=rtlsdr").
inline bool sameFamilyAcrossStacks(const std::string& a, const std::string& b) {
    const std::string da = deviceDriver(a);
    const std::string db = deviceDriver(b);
    const bool soapyA = da == "soapy";
    const bool soapyB = db == "soapy";
    if (soapyA == soapyB) { return false; }
    const std::string& native = soapyA ? db : da;
    std::string module = argField(deviceArgs(soapyA ? a : b), "driver");
    for (char& c : module) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    const std::string family = nativeFamilyForSoapyDriver(module);
    return !family.empty() && family == native;
}

}  // namespace detail

// Whether two radio keys name the same physical radio. The generator is never
// "the same device" as anything - it is not a device. Empty keys (no radio
// chosen yet) never clash either.
//
// Two serials decide. WITHOUT a serial on both sides, a native row and a
// SoapySDR row of the same family are the SAME radio: a dongle with no serial
// in its USB descriptor is listed natively as "rtlsdr|index=0" and by
// SoapyRTLSDR with a blank serial, and nothing in either key can show they
// differ. Through 0.99.34 that pair counted as two radios, and two patch
// Radio nodes could open one dongle through two driver stacks. Treating it as
// one costs nothing real: the native list already has a row for every dongle
// of that family.
inline bool sameDevice(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty() || isGeneratorKey(a) || isGeneratorKey(b)) { return false; }
    // A RECORDING IS DECIDED BY ITS FILE (0.99.40), and first: a recording is
    // never a radio, and its path is not argument text - a file named
    // "x,serial=1.wav" must not match a dongle with serial 1 below.
    if (isIqFileKey(a) || isIqFileKey(b)) {
        return isIqFileKey(a) && isIqFileKey(b) &&
               iqFileIdentity(iqFilePath(a)) == iqFileIdentity(iqFilePath(b));
    }
    if (a == b) { return true; }
    const std::string sa = argField(deviceArgs(a), "serial");
    const std::string sb = argField(deviceArgs(b), "serial");
    if (!sa.empty() && !sb.empty()) { return sa == sb; }
    return detail::sameFamilyAcrossStacks(a, b);
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
