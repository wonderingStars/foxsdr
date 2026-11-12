// test_aaronia_source.cpp - the Aaronia RTSA HTTP stream-server client, proven against a STAND-IN HTTP server
// on loopback: the HTTP head and chunk readers, the record parser, and AaroniaSource end to end.
//
// WHAT THIS CAN AND CANNOT PROVE. The stand-in is written from "RTSA HTTP Stream Server Endpoints"
// (src/source/aaronia_protocol.hpp has the provenance and the list of what the document leaves open). It builds
// its HTTP and its records with its own code and literal text, but it cannot show the reading right, and for the
// raw float32 record delimiting it is the SAME reading as the client (the document is ambiguous there).
// UNPROVEN: a real RTSA-Suite / SPECTRAN V6.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "source/aaronia_protocol.hpp"
#include "source/aaronia_source.hpp"
#include "src_test_net.hpp"
#include "test_check.hpp"

using namespace srctest;
using cascade::source::AaroniaSource;
using cascade::source::NetSampleSource;
namespace aa = cascade::source::aaronia;
using nlohmann::json;

namespace {

constexpr double kPi = 3.14159265358979323846;

bool pollRead(Sock s, int ms) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(s, &rf);
    timeval tv{};
    tv.tv_usec = ms * 1000;
#if defined(_WIN32)
    return ::select(0, &rf, nullptr, nullptr, &tv) > 0;
#else
    return ::select(s + 1, &rf, nullptr, nullptr, &tv) > 0;
#endif
}

std::vector<std::complex<float>> readN(AaroniaSource& s, std::size_t n, int ms = 8000) {
    std::vector<std::complex<float>> out;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    std::vector<std::complex<float>> tmp(2048);
    while (out.size() < n && std::chrono::steady_clock::now() < end) {
        const std::size_t g = s.read(tmp.data(), tmp.size());
        out.insert(out.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(g));
        if (s.faulted()) { break; }
    }
    return out;
}

double toneHz(const std::vector<std::complex<float>>& v, double fs) {
    std::complex<double> acc(0, 0);
    for (std::size_t i = 1; i < v.size(); ++i) { acc += std::complex<double>(v[i]) * std::conj(std::complex<double>(v[i - 1])); }
    return std::arg(acc) * fs / (2.0 * kPi);
}

// --- the stand-in server -----------------------------------------------------------------

enum class Quirk {
    None, Unauthorized, NotFound, NotJson, NoName, ControlFails, StreamUnauthorized, StreamRefused, StreamAbrupt,
    StreamSilent, StreamEnds, GapsInTime, OtherPackets, BadRecord, BadChunk, HugeChunk, NonFinite, HtmlGarbage
};

struct Spec {
    Quirk quirk = Quirk::None;
    bool raw = false;               // honour format=float32
    double toneHz = 7000.0;
    double ppm = 1500.0;            // the server's clock is off by this much
    double noise = 0.01;
    int pairsPerRecord = 500;
    double defaultRate = 200000.0;
    double centerHz = 1.0e9;
};

class Server {
public:
    explicit Server(Spec s) : spec_(std::move(s)) {}
    bool start() {
        return srv_.start([this](Sock c, std::size_t, const std::atomic<bool>& run) { serve(c, run); });
    }
    void stop() { srv_.stop(); }
    std::string args(const std::string& extra = "") const {
        return "aaronia=127.0.0.1:" + std::to_string(srv_.port()) + (extra.empty() ? "" : "," + extra);
    }
    std::vector<std::string> controls() {
        std::lock_guard<std::mutex> lk(m_);
        return controls_;
    }
    std::vector<std::string> remoteConfigs() {
        std::lock_guard<std::mutex> lk(m_);
        return remoteConfigs_;
    }
    std::atomic<int> streamRequests{0}, infoRequests{0};
    std::string lastStreamPath;

private:
    struct Req {
        std::string method, path, body;
    };
    static bool readRequest(Sock c, const std::atomic<bool>& run, Req& r) {
        std::string head;
        char ch;
        while (head.size() < 8192) {
            if (!waitRead(c, run)) { return false; }
            if (::recv(c, &ch, 1, 0) <= 0) { return false; }
            head += ch;
            if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0) { break; }
        }
        const std::size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) { return false; }
        r.method = head.substr(0, sp1);
        r.path = head.substr(sp1 + 1, sp2 - sp1 - 1);
        std::size_t cl = 0;
        const std::size_t p = head.find("Content-Length: ");
        if (p != std::string::npos) { cl = static_cast<std::size_t>(std::atoi(head.c_str() + p + 16)); }
        r.body.resize(cl);
        if (cl > 0 && !recvExact(c, run, &r.body[0], cl)) { return false; }
        return true;
    }
    static void respond(Sock c, int status, const std::string& body, const std::string& extra = "") {
        const std::string r = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " X") + "\r\nContent-Type: application/json\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\n" + extra + "Connection: close\r\n\r\n" + body;
        sendExact(c, r.data(), r.size());
    }
    std::string record(double startTime, int pairs, double fs, double& phase, Rng& rng, bool rawMode, std::string* binaryOut, bool int16Raw = false) {
        json j;
        j["startTime"] = startTime;
        j["endTime"] = startTime + pairs / fs;
        j["unit"] = "generic";
        j["payload"] = "iq";
        j["startFrequency"] = spec_.centerHz - fs / 2;
        j["endFrequency"] = spec_.centerHz + fs / 2;
        j["minPower"] = -2;
        j["maxPower"] = 2;
        j["sampleDepth"] = 1;
        j["sampleSize"] = 2;
        const double dphi = 2.0 * kPi * spec_.toneHz / fs;
        std::vector<float> v;
        for (int k = 0; k < pairs; ++k) {
            auto nz = [&]() { return (static_cast<double>(rng.next() & 0xFFFF) / 65535.0 - 0.5) * 3.4 * spec_.noise; };
            v.push_back(static_cast<float>(0.5 * std::cos(phase) + nz()));
            v.push_back(static_cast<float>(0.5 * std::sin(phase) + nz()));
            phase += dphi;
            if (phase > 2 * kPi) { phase -= 2 * kPi; }
        }
        if (spec_.quirk == Quirk::NonFinite && !v.empty()) { v[2] = std::numeric_limits<float>::quiet_NaN(); v[3] = std::numeric_limits<float>::infinity(); }
        if (rawMode) {
            j["samples"] = pairs;
            std::string out = j.dump() + "\n\x1e";
            if (int16Raw) {   // the reference client's "format=int16&scale=32767": value * 32767, little-endian
                std::vector<std::int16_t> w;
                for (float x : v) { w.push_back(static_cast<std::int16_t>(std::lround(std::max(-1.0f, std::min(1.0f, x)) * 32767.0f))); }
                binaryOut->assign(reinterpret_cast<const char*>(w.data()), w.size() * sizeof(std::int16_t));
                return out;
            }
            binaryOut->assign(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(float));
            return out;
        }
        j["samples"] = v;
        return j.dump() + "\n\x1e";
    }
    static void sendChunked(Sock c, const std::string& data, Rng& rng, const std::atomic<bool>& run) {
        std::size_t off = 0;
        while (off < data.size() && run.load()) {
            const std::size_t take = std::min<std::size_t>(1 + rng.next() % 3000, data.size() - off);
            char hdr[32];
            std::snprintf(hdr, sizeof(hdr), "%zx\r\n", take);
            std::string chunk = hdr;
            chunk.append(data, off, take);
            chunk += "\r\n";
            if (!sendExact(c, chunk.data(), chunk.size())) { return; }
            off += take;
        }
    }

    void serve(Sock c, const std::atomic<bool>& run) {
        Req r;
        if (!readRequest(c, run, r)) { return; }
        if (r.method == "GET" && r.path == "/info") {
            ++infoRequests;
            switch (spec_.quirk) {
                case Quirk::Unauthorized: respond(c, 401, "{}", "WWW-Authenticate: Basic realm=\"rtsa\"\r\n"); return;
                case Quirk::NotFound: respond(c, 404, "{}"); return;
                case Quirk::NotJson: respond(c, 200, "<html>router login</html>"); return;
                case Quirk::NoName: respond(c, 200, "{\"title\":\"x\"}"); return;
                case Quirk::HtmlGarbage: { const std::string g = "SSH-2.0-OpenSSH_9.9\r\n"; sendExact(c, g.data(), g.size()); return; }
                default: respond(c, 200, "{\"name\":\"Block_HTTPServer_0\\u0001\",\"title\":\"HTTP Server\",\"uuid\":\"aaf2a8f7\",\"port\":54664,\"mission\":\"\"}"); return;
            }
        }
        if (r.method == "PUT" && r.path == "/control") {
            {
                std::lock_guard<std::mutex> lk(m_);
                controls_.push_back(r.body);
                const json j = json::parse(r.body, nullptr, false);
                if (j.is_object() && j.value("type", "") == "capture") {
                    if (j.contains("frequencySpan")) { rate_ = j["frequencySpan"].get<double>(); }
                    if (j.contains("frequencyCenter")) { spec_.centerHz = j["frequencyCenter"].get<double>(); }
                }
            }
            if (spec_.quirk == Quirk::ControlFails) { respond(c, 500, "{}"); return; }
            respond(c, 200, "{}");
            return;
        }
        if (r.method == "PUT" && r.path == "/remoteconfig") {
            {
                std::lock_guard<std::mutex> lk(m_);
                remoteConfigs_.push_back(r.body);
                const json j = json::parse(r.body, nullptr, false);
                // the demodulator block carries the rate and the centre the stream will have
                if (j.is_object() && j.value("receiverName", "") == "Block_IQDemodulator_1") {
                    for (const auto& g : j["config"]["items"]) {
                        for (const auto& it : g["items"]) {
                            if (it["name"] == "samplerate") { rate_ = it["value"].get<double>(); }
                            if (it["name"] == "centerfreq") { spec_.centerHz = it["value"].get<double>(); }
                        }
                    }
                }
            }
            if (spec_.quirk == Quirk::ControlFails) { respond(c, 500, "{}"); return; }
            respond(c, 200, "{}");
            return;
        }
        if (r.method == "GET" && r.path.rfind("/stream", 0) == 0) {
            ++streamRequests;
            lastStreamPath = r.path;
            const bool int16Mode = spec_.raw && r.path.find("format=int16&scale=32767") != std::string::npos;
            const bool rawMode = spec_.raw && (int16Mode || r.path.find("format=float32") != std::string::npos);
            if (spec_.quirk == Quirk::StreamUnauthorized) { respond(c, 401, "{}"); return; }
            if (spec_.quirk == Quirk::StreamRefused) { respond(c, 503, "{}"); return; }
            const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n";
            sendExact(c, head.data(), head.size());
            if (spec_.quirk == Quirk::StreamSilent) { while (run.load() && pollRead(c, 20) == false) {} return; }
            Rng rng(31337);
            double phase = 0.0;
            double t = 1.7e9;
            int n = 0;
            double rate;
            {
                std::lock_guard<std::mutex> lk(m_);
                rate = rate_ > 0 ? rate_ : spec_.defaultRate;
            }
            const double fs = rate * (1.0 + spec_.ppm * 1e-6);
            while (run.load()) {
                std::string wire;
                for (int b = 0; b < 4; ++b) {
                    ++n;
                    if (spec_.quirk == Quirk::OtherPackets && n % 3 == 0) {
                        json sp;
                        sp["startTime"] = t; sp["endTime"] = t + 0.001; sp["payload"] = "spectra"; sp["unit"] = "dbm";
                        sp["samples"] = std::vector<std::vector<double>>{{-90.0, -80.5}, {-70.0, -60.0}};
                        wire += sp.dump() + "\n\x1e";
                    }
                    std::string bin;
                    wire += record(t, spec_.pairsPerRecord, fs, phase, rng, rawMode, &bin, int16Mode);
                    wire += bin;
                    t += spec_.pairsPerRecord / fs;
                    if (spec_.quirk == Quirk::GapsInTime && n % 5 == 0) { t += 0.01; }  // the server dropped data
                }
                if (spec_.quirk == Quirk::BadRecord && n >= 8) { wire += "this is not json\n\x1e"; }
                if (spec_.quirk == Quirk::BadChunk && n >= 8) {
                    const std::string bad = "zz\r\nxxxx\r\n";
                    sendExact(c, bad.data(), bad.size());
                    return;
                }
                if (spec_.quirk == Quirk::HugeChunk && n >= 8) {
                    const std::string huge = "7FFFFFFF\r\n";
                    sendExact(c, huge.data(), huge.size());
                    return;
                }
                sendChunked(c, wire, rng, run);
                if (spec_.quirk == Quirk::StreamAbrupt && n >= 12) { return; }  // socket closed mid-stream
                if (spec_.quirk == Quirk::StreamEnds && n >= 12) {
                    const std::string last = "0\r\n\r\n";
                    sendExact(c, last.data(), last.size());
                    return;
                }
                if (spec_.quirk == Quirk::BadRecord && n >= 8) { return; }
                if (pollRead(c, 3)) {  // the client closed
                    char tmp[16];
                    if (::recv(c, tmp, sizeof(tmp), 0) <= 0) { return; }
                }
            }
            return;
        }
        respond(c, 404, "{}");
    }

    Spec spec_;
    TcpServer srv_;
    std::mutex m_;
    std::vector<std::string> controls_, remoteConfigs_;
    double rate_ = 0.0;
};

// --- tests -----------------------------------------------------------------------------

void testHttpHead() {
    aa::HttpHead h;
    std::string err;
    CHECK(aa::parseHttpHead("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 12\r\nX-Odd:   spaced   ", h, err));
    CHECK(h.status == 200 && h.reason == "OK" && h.contentLength == 12 && !h.chunked && h.headers["x-odd"] == "spaced");
    CHECK(aa::parseHttpHead("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked", h, err) && h.chunked && h.contentLength == -1);
    CHECK(aa::parseHttpHead("HTTP/1.0 404", h, err) && h.status == 404 && h.reason.empty());  // no reason phrase is allowed
    CHECK(aa::parseHttpHead("HTTP/1.1 204 No Content", h, err) && h.status == 204);
    for (const char* bad : {"", "HTTP/2 200 OK", "SSH-2.0-OpenSSH_9.9", "HTTP/1.1 2x0 OK", "HTTP/1.1 200 OK\r\nNoColonHere",
                            "HTTP/1.1 200 OK\r\n: novalue", "HTTP/1.1 200 OK\r\nBad Name: x", "HTTP/1.1 200 OK\r\nContent-Length: -5",
                            "HTTP/1.1 200 OK\r\nContent-Length: 1e3", "HTTP/1.1 200 OK\r\nContent-Length: 99999999999999",
                            "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip", "HTTP/1.9 200 OK"}) {
        err.clear();
        CHECK(!aa::parseHttpHead(bad, h, err) && !err.empty());
    }
}

void testChunks() {
    const std::string wire = "5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\nTrailer: x\r\n\r\n";
    {
        aa::ChunkDecoder d;
        std::vector<std::uint8_t> out;
        for (char c : wire) { const std::uint8_t b = static_cast<std::uint8_t>(c); d.feed(&b, 1, out); }
        CHECK(d.finished() && d.error().empty() && std::string(out.begin(), out.end()) == "hello world");
    }
    {
        aa::ChunkDecoder d;
        std::vector<std::uint8_t> out;
        d.feed(reinterpret_cast<const std::uint8_t*>(wire.data()), wire.size(), out);
        CHECK(d.finished() && std::string(out.begin(), out.end()) == "hello world");
        // after the end, more bytes are not interpreted
        const std::uint8_t more[] = {'x', 'y'};
        d.feed(more, 2, out);
        CHECK(std::string(out.begin(), out.end()) == "hello world");
    }
    for (const char* bad : {"zz\r\n", "\r\n", "123456789\r\nxx", "5\r\nhelloXX", "G\r\n", "-1\r\n"}) {
        aa::ChunkDecoder d;
        std::vector<std::uint8_t> out;
        d.feed(reinterpret_cast<const std::uint8_t*>(bad), std::strlen(bad), out);
        CHECK(!d.error().empty() && !d.finished());
    }
    {   // larger than the bound
        aa::ChunkDecoder d;
        std::vector<std::uint8_t> out;
        const char* big = "1000001\r\n";
        d.feed(reinterpret_cast<const std::uint8_t*>(big), std::strlen(big), out);
        CHECK(!d.error().empty());
    }
    {   // a size line that never ends
        aa::ChunkDecoder d;
        std::vector<std::uint8_t> out;
        const std::string long1(200, 'a');
        d.feed(reinterpret_cast<const std::uint8_t*>(long1.data()), long1.size(), out);
        CHECK(!d.error().empty());
    }
}

std::string jsonRecord(const std::vector<float>& v, const char* payload = "iq", bool freq = true) {
    json j;
    j["startTime"] = 100.0;
    j["endTime"] = 100.0 + v.size() / 2 / 1000.0;
    j["payload"] = payload;
    j["unit"] = "generic";
    if (freq) { j["startFrequency"] = 900.0e6; j["endFrequency"] = 902.0e6; }
    j["samples"] = v;
    return j.dump() + "\n\x1e";
}

void testRecords() {
    // json: one record, then one fed a byte at a time
    {
        aa::RecordParser p(aa::SampleFormat::Json);
        std::vector<aa::IqRecord> out;
        const std::string w = jsonRecord({0.5f, -0.25f, 0.125f, 1.0f});
        p.feed(reinterpret_cast<const std::uint8_t*>(w.data()), w.size(), out);
        CHECK(p.error().empty() && out.size() == 1 && out[0].samples.size() == 2);
        CHECK(out[0].samples[0] == std::complex<float>(0.5f, -0.25f) && out[0].samples[1] == std::complex<float>(0.125f, 1.0f));
        CHECK(out[0].hasFrequency && out[0].startFrequency == 900.0e6 && out[0].endFrequency == 902.0e6);
        CHECK_NEAR(out[0].rateHz(), 2.0 / 0.002, 1e-6);  // 2 pairs in 2 ms
        aa::RecordParser q(aa::SampleFormat::Json);
        std::vector<aa::IqRecord> out2;
        const std::string two = w + w;
        for (char c : two) { const std::uint8_t b = static_cast<std::uint8_t>(c); q.feed(&b, 1, out2); }
        CHECK(q.error().empty() && out2.size() == 2);
    }
    // strings containing braces and escaped quotes do not end an object early
    {
        aa::RecordParser p(aa::SampleFormat::Json);
        std::vector<aa::IqRecord> out;
        const std::string w = "{\"payload\":\"iq\",\"startTime\":1,\"endTime\":2,\"note\":\"a } b \\\" { c\",\"samples\":[1,2,3,4]}\n\x1e";
        p.feed(reinterpret_cast<const std::uint8_t*>(w.data()), w.size(), out);
        CHECK(p.error().empty() && out.size() == 1 && out[0].samples.size() == 2);
    }
    // non-I/Q packets are skipped and counted
    {
        aa::RecordParser p(aa::SampleFormat::Json);
        std::vector<aa::IqRecord> out;
        const std::string w = jsonRecord({1, 2, 3, 4}, "spectra") + jsonRecord({1, 2}, "iq") + jsonRecord({9, 9, 9, 9}, "histogram");
        p.feed(reinterpret_cast<const std::uint8_t*>(w.data()), w.size(), out);
        CHECK(p.error().empty() && out.size() == 1 && p.otherPackets() == 2);
    }
    // raw float32: JSON, LF, RS, then pairs*8 bytes
    {
        aa::RecordParser p(aa::SampleFormat::Float32);
        std::vector<aa::IqRecord> out;
        const float s[6] = {0.5f, 0.25f, -1.0f, 2.0f, std::numeric_limits<float>::quiet_NaN(), 3.0f};
        std::string w = "{\"payload\":\"iq\",\"startTime\":1,\"endTime\":2,\"samples\":3}\n\x1e";
        w.append(reinterpret_cast<const char*>(s), sizeof(s));
        w += "{\"payload\":\"iq\",\"startTime\":2,\"endTime\":3,\"samples\":0}\x1e";  // an empty packet, no LF
        for (char c : w) { const std::uint8_t b = static_cast<std::uint8_t>(c); p.feed(&b, 1, out); }
        CHECK(p.error().empty() && out.size() == 2 && out[0].samples.size() == 3 && out[1].samples.empty());
        CHECK(out[0].samples[1] == std::complex<float>(-1.0f, 2.0f));
        CHECK(out[0].samples[2] == std::complex<float>(0.0f, 0.0f) && p.nonFiniteSamples() == 1);  // a pair with a NaN becomes 0+0i
    }
    // refusals
    const char* bad[] = {
        "not json at all",
        "{\"payload\":\"iq\",\"samples\":[1,2,3]}",           // odd count
        "{\"payload\":\"iq\",\"samples\":[1,\"x\"]}",         // not a number
        "{\"payload\":\"iq\",\"samples\":5}",                 // a number where json wants an array
        "{\"payload\":\"iq\"}",                               // no samples
        "{\"payload\":\"iq\",\"samples\":[1,2,]}",            // invalid json
        "[1,2,3]",                                            // not an object
    };
    for (const char* b : bad) {
        aa::RecordParser p(aa::SampleFormat::Json);
        std::vector<aa::IqRecord> out;
        p.feed(reinterpret_cast<const std::uint8_t*>(b), std::strlen(b), out);
        const std::string tail = "\n\x1e";
        p.feed(reinterpret_cast<const std::uint8_t*>(tail.data()), tail.size(), out);
        CHECK(!p.error().empty() && out.empty());
        // and nothing more is believed afterwards
        const std::string w = jsonRecord({1, 2});
        p.feed(reinterpret_cast<const std::uint8_t*>(w.data()), w.size(), out);
        CHECK(out.empty());
    }
    {   // raw without the separator, a count that is too large, a negative count, a non-I/Q raw packet
        for (const char* b : {"{\"payload\":\"iq\",\"samples\":1}\nXXXXXXXX", "{\"payload\":\"iq\",\"samples\":99999999}\n\x1e",
                              "{\"payload\":\"iq\",\"samples\":-3}\n\x1e", "{\"payload\":\"spectra\",\"samples\":3}\n\x1e"}) {
            aa::RecordParser p(aa::SampleFormat::Float32);
            std::vector<aa::IqRecord> out;
            p.feed(reinterpret_cast<const std::uint8_t*>(b), std::strlen(b), out);
            CHECK(!p.error().empty() && out.empty());
        }
    }
    {   // a header that never ends is bounded
        aa::RecordParser p(aa::SampleFormat::Json);
        std::vector<aa::IqRecord> out;
        std::string w = "{\"payload\":\"iq\",\"note\":\"";
        std::vector<std::uint8_t> block(1 << 20, 'a');
        p.feed(reinterpret_cast<const std::uint8_t*>(w.data()), w.size(), out);
        for (int i = 0; i < 10 && p.error().empty(); ++i) { p.feed(block.data(), block.size(), out); }
        CHECK(!p.error().empty() && out.empty());
    }
    {   // int16 raw: little-endian pairs, value / 32767, split at every byte
        std::string w = "{\"payload\":\"iq\",\"startTime\":1,\"endTime\":2,\"samples\":3}\n\x1e";
        const std::int16_t v[] = {32767, -32767, 0, 16384, -1, 1};
        w.append(reinterpret_cast<const char*>(v), sizeof v);
        w += "{\"payload\":\"iq\",\"startTime\":2,\"endTime\":3,\"samples\":1}\n\x1e";
        const std::int16_t v2[] = {-32768, 32767};   // -32768 is one step past full scale: not clamped, not wrapped
        w.append(reinterpret_cast<const char*>(v2), sizeof v2);
        aa::RecordParser p(aa::SampleFormat::Int16);
        std::vector<aa::IqRecord> out;
        for (char c : w) { const std::uint8_t b = static_cast<std::uint8_t>(c); p.feed(&b, 1, out); }
        CHECK(p.error().empty() && out.size() == 2);
        if (out.size() == 2 && out[0].samples.size() == 3 && out[1].samples.size() == 1) {
            CHECK(out[0].samples[0] == std::complex<float>(1.0f, -1.0f) && out[0].samples[1] == std::complex<float>(0.0f, 16384.0f / 32767.0f));
            CHECK(out[0].samples[2] == std::complex<float>(-1.0f / 32767.0f, 1.0f / 32767.0f));
            CHECK(out[1].samples[0].real() < -1.0f && out[1].samples[0].real() > -1.001f && out[1].samples[0].imag() == 1.0f);
        } else {
            CHECK(false);
        }
        // a count whose bytes never arrive is not believed, and one too large is refused
        aa::RecordParser q(aa::SampleFormat::Int16);
        std::vector<aa::IqRecord> o2;
        const std::string hdr = "{\"payload\":\"iq\",\"samples\":10}\n\x1e";
        q.feed(reinterpret_cast<const std::uint8_t*>(hdr.data()), hdr.size(), o2);
        CHECK(q.error().empty() && o2.empty());
        aa::RecordParser r(aa::SampleFormat::Int16);
        const std::string huge = "{\"payload\":\"iq\",\"samples\":99999999}\n\x1e";
        r.feed(reinterpret_cast<const std::uint8_t*>(huge.data()), huge.size(), o2);
        CHECK(!r.error().empty());
    }
    // the /remoteconfig bodies of the reference client (hb9fxq/libspectranstream, app/test_req.http): literal text
    {
        const json d = json::parse(aa::demodulatorRequest(10, "Block_IQDemodulator_1", 938000000.0, 2000000.0));
        const json want = json::parse(R"({"request":10,"receiverName":"Block_IQDemodulator_1","config":{"type":"group","items":[{"type":"group","name":"main","items":[
            {"type":"float","name":"centerfreq","value":938000000},{"type":"float","name":"samplerate","value":2000000},{"type":"float","name":"spanfreq","value":2000000}]}]}})");
        CHECK(d == want);
        const json t = json::parse(aa::tuneRequest(11, "Block_Spectran_V6B_1", 103000000.0));
        const json want2 = json::parse(R"({"request":11,"receiverName":"Block_Spectran_V6B_1","config":{"type":"group","items":[{"type":"group","name":"main","items":[
            {"type":"float","name":"centerfreq","value":103000000}]}]}})");
        CHECK(t == want2);
        CHECK(aa::validBlockName("Block_IQDemodulator_1") && aa::validBlockName("A b-c.d"));
        for (const char* badName : {"", "a\"b", "a\\b", "x\ny", "a{b", "caf\xC3\xA9"}) { CHECK(!aa::validBlockName(badName)); }
        CHECK(!aa::validBlockName(std::string(65, 'a')) && aa::validBlockName(std::string(64, 'a')));
    }
    // request bodies
    {
        const json c = json::parse(aa::captureRequest(1.92e9, 2.0e8));
        CHECK(c["type"] == "capture" && c["frequencyCenter"] == 1.92e9 && c["frequencySpan"] == 2.0e8);
        const json s = json::parse(aa::streamingRequest(true));
        CHECK(s["type"] == "streaming" && s["start"] == true);
        CHECK(json::parse(aa::streamingRequest(false))["start"] == false);
    }
}

void testFuzz() {
    Rng rng(0xA110);
    const std::string good = jsonRecord({0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f});
    for (int i = 0; i < 4000; ++i) {
        for (aa::SampleFormat f : {aa::SampleFormat::Json, aa::SampleFormat::Float32, aa::SampleFormat::Int16}) {
            aa::RecordParser p(f);
            std::vector<aa::IqRecord> out;
            std::string w = i % 2 ? good : std::string();
            const std::size_t extra = rng.next() % 300;
            for (std::size_t k = 0; k < extra; ++k) { w += static_cast<char>(rng.next()); }
            if (i % 3 == 0 && w.size() > 3) { w[rng.next() % w.size()] = '{'; }
            std::size_t off = 0;
            while (off < w.size()) {
                const std::size_t n = std::min<std::size_t>(1 + rng.next() % 50, w.size() - off);
                p.feed(reinterpret_cast<const std::uint8_t*>(w.data()) + off, n, out);
                off += n;
            }
            for (const auto& r : out) { CHECK(r.samples.size() <= aa::kMaxPairs); }
        }
        aa::ChunkDecoder d;
        std::vector<std::uint8_t> o;
        std::string c;
        for (int k = 0; k < 40; ++k) { c += static_cast<char>(rng.next() % 2 ? "0123456789abcdef\r\n"[rng.next() % 18] : static_cast<char>(rng.next())); }
        d.feed(reinterpret_cast<const std::uint8_t*>(c.data()), c.size(), o);
        CHECK(o.size() <= c.size());
        aa::HttpHead h;
        std::string err;
        aa::parseHttpHead(std::string("HTTP/1.1 200 OK\r\n") + c, h, err);
    }
}

void testOpenAndStream(bool raw) {
    Spec sp;
    sp.raw = raw;
    Server srv(sp);
    CHECK(srv.start());
    const auto abandoned = NetSampleSource::readersAbandoned();
    AaroniaSource s;
    CHECK(std::string(s.driverKey()) == "aaronia");
    CHECK(s.open(srv.args(std::string("timeout=4000,rate=200000") + (raw ? ",format=float32" : ""))));
    CHECK(s.isOpen() && s.serverName() == "Block_HTTPServer_0?");  // the control byte in the name became a '?'
    CHECK(std::string(s.name()).find("Aaronia RTSA: Block_HTTPServer_0") == 0);
    CHECK(srv.infoRequests.load() == 1);
    CHECK(s.sampleRateHz() == 200000.0 && s.supportedSampleRatesHz() == std::vector<double>{200000.0});
    CHECK(!s.setCenterFrequencyHz(-5) && !s.setCenterFrequencyHz(0) && !s.setCenterFrequencyHz(std::nan("")) && !s.setSampleRateHz(10.0) && !s.setSampleRateHz(2e9));
    CHECK(s.setCenterFrequencyHz(1.92e9) && s.centerFrequencyHz() == 1.92e9);
    CHECK(srv.controls().empty());  // nothing is sent until start
    CHECK(s.start() && s.running());
    {
        const auto c = srv.controls();
        CHECK(c.size() == 2);  // the capture, then the stream start
        if (c.size() == 2) {
            const json a = json::parse(c[0]), b = json::parse(c[1]);
            CHECK(a["type"] == "capture" && a["frequencyCenter"] == 1.92e9 && a["frequencySpan"] == 200000.0);
            CHECK(b["type"] == "streaming" && b["start"] == true);
        }
    }
    const auto got = readN(s, 20000);
    CHECK(got.size() >= 20000);
    CHECK(srv.lastStreamPath.find(raw ? "format=float32" : "format=json") != std::string::npos && srv.lastStreamPath.find("rate_adaption=0") != std::string::npos);
    // the measured rate carries the server's clock error (1500 ppm), and the tone is where that rate puts it
    const double fs = s.sampleRateHz();
    CHECK_NEAR(fs, 200000.0 * (1.0 + 1500e-6), 200.0);
    CHECK_NEAR(toneHz(got, fs), 7000.0, 12.0);
    double mag = 0;
    for (auto& v : got) { mag += std::abs(v); }
    CHECK_NEAR(mag / static_cast<double>(got.size()), 0.5, 0.03);
    CHECK_NEAR(s.centerFrequencyHz(), 1.92e9, 1.0);  // from the packets' frequency fields
    CHECK(s.gapCount() == 0);
    // retuning while running sends a new capture
    CHECK(s.setCenterFrequencyHz(1.5e9));
    CHECK(srv.controls().size() == 3 && json::parse(srv.controls()[2])["frequencyCenter"] == 1.5e9);
    s.stop();
    CHECK(!s.running());
    CHECK(srv.controls().size() == 4 && json::parse(srv.controls()[3])["start"] == false);
    CHECK(s.start());  // a second start opens a second stream
    CHECK(readN(s, 2000).size() >= 2000);
    CHECK(srv.streamRequests.load() == 2);
    s.closeDevice();
    CHECK(!s.isOpen());
    CHECK(NetSampleSource::readersAbandoned() == abandoned);
    srv.stop();
}

void testInt16Stream() {
    Spec sp;
    sp.raw = true;
    Server srv(sp);
    CHECK(srv.start());
    AaroniaSource s;
    CHECK(s.open(srv.args("timeout=4000,rate=200000,format=int16")));
    CHECK(s.start());
    const auto got = readN(s, 20000);
    CHECK(got.size() >= 20000);
    CHECK(srv.lastStreamPath.find("format=int16&scale=32767") != std::string::npos);
    const double fs = s.sampleRateHz();
    CHECK_NEAR(toneHz(got, fs), 7000.0, 12.0);
    double mag = 0;
    for (auto& v : got) { mag += std::abs(v); }
    CHECK_NEAR(mag / static_cast<double>(got.size()), 0.5, 0.03);   // /32767 puts the 0.5 tone back at 0.5
    CHECK(!s.faulted());
    s.closeDevice();
    srv.stop();
}

void testRemoteConfig() {
    Spec sp;
    sp.raw = true;
    Server srv(sp);
    CHECK(srv.start());
    AaroniaSource s;
    CHECK(s.open(srv.args("timeout=4000,format=float32,control=remoteconfig,spectranoffset=-500000")));
    CHECK(s.setCenterFrequencyHz(938000000.0) && s.setSampleRateHz(250000.0));
    CHECK(srv.remoteConfigs().empty() && srv.controls().empty());  // nothing is sent until start
    CHECK(s.start());
    const auto rc = srv.remoteConfigs();
    CHECK(rc.size() == 2 && srv.controls().empty());  // no /control request at all in this mode, not even a start
    if (rc.size() == 2) {
        const json a = json::parse(rc[0]), b = json::parse(rc[1]);
        CHECK(a["request"] == 1 && a["receiverName"] == "Block_IQDemodulator_1");
        const auto& items = a["config"]["items"][0]["items"];
        CHECK(items.size() == 3 && items[0]["name"] == "centerfreq" && items[0]["value"] == 938000000.0 && items[1]["name"] == "samplerate" &&
              items[1]["value"] == 250000.0 && items[2]["name"] == "spanfreq" && items[2]["value"] == 250000.0);
        CHECK(b["request"] == 2 && b["receiverName"] == "Block_Spectran_V6B_1" && b["config"]["items"][0]["items"][0]["value"] == 937500000.0);
    }
    const auto got = readN(s, 20000);
    CHECK(got.size() >= 20000);
    CHECK_NEAR(s.sampleRateHz(), 250000.0 * (1.0 + 1500e-6), 250.0);   // the stream really runs at the rate the demodulator was given
    CHECK_NEAR(toneHz(got, s.sampleRateHz()), 7000.0, 12.0);
    // retuning while running sends numbered requests again
    CHECK(s.setCenterFrequencyHz(940000000.0));
    CHECK(srv.remoteConfigs().size() == 4 && json::parse(srv.remoteConfigs()[2])["request"] == 3);
    s.stop();
    CHECK(srv.controls().empty());  // and no stop either
    s.closeDevice();
    srv.stop();
    {   // a named block, no SPECTRAN block
        Server s2{Spec{}};
        CHECK(s2.start());
        AaroniaSource t;
        CHECK(t.open(s2.args("timeout=3000,control=remoteconfig,demod=Block_IQDemodulator_1,spectran=")));
        CHECK(t.setCenterFrequencyHz(100e6) && t.setSampleRateHz(1e6) && t.start());
        CHECK(s2.remoteConfigs().size() == 1);
        t.closeDevice();
        s2.stop();
    }
    {   // the mission's refusal is said
        Spec sp3;
        sp3.quirk = Quirk::ControlFails;
        Server s3(sp3);
        CHECK(s3.start());
        AaroniaSource t;
        CHECK(t.open(s3.args("timeout=3000,control=remoteconfig")));
        CHECK(t.setCenterFrequencyHz(100e6) && !t.start() && !t.running());
        CHECK(std::string(t.lastError()).find("setting the demodulator") != std::string::npos && std::string(t.lastError()).find("HTTP status 500") != std::string::npos);
        t.closeDevice();
        s3.stop();
    }
    {   // bad block names and offsets never reach the network
        AaroniaSource t;
        CHECK(!t.open("aaronia=127.0.0.1:1,control=remoteconfig,demod=a\"b"));
        CHECK(!t.open("aaronia=127.0.0.1:1,control=remoteconfig,demod="));
        CHECK(!t.open("aaronia=127.0.0.1:1,control=remoteconfig,spectran=x{y"));
        CHECK(!t.open("aaronia=127.0.0.1:1,control=remoteconfig,spectranoffset=abc"));
        CHECK(!t.open("aaronia=127.0.0.1:1,control=remoteconfig,spectranoffset=1e30"));
        CHECK(!t.isOpen());
    }
}

void testOpenRefusals() {
    {
        AaroniaSource s;
        CHECK(!s.open("aaronia=127.0.0.1:0") && !s.open("aaronia=127.0.0.1:70000") && !s.open("aaronia=[::1"));
        CHECK(!s.open("aaronia=127.0.0.1:1,timeout=5") && !s.open("aaronia=127.0.0.1:1,format=int24") && !s.open("aaronia=127.0.0.1:1,control=magic") && !s.open("aaronia=127.0.0.1:1,rate=5"));
        CHECK(!s.start() && !s.setCenterFrequencyHz(1e9) && !s.setSampleRateHz(1e6));
        CHECK(!s.isOpen());
    }
    {
        AaroniaSource s;
        CHECK(!s.open("aaronia=127.0.0.1:" + std::to_string(deadPort()) + ",timeout=400") && !s.isOpen());
        CHECK(std::string(s.lastError()).find("could not reach") != std::string::npos);
    }
    for (Quirk q : {Quirk::Unauthorized, Quirk::NotFound, Quirk::NotJson, Quirk::NoName, Quirk::HtmlGarbage}) {
        Spec sp;
        sp.quirk = q;
        Server srv(sp);
        CHECK(srv.start());
        AaroniaSource s;
        CHECK(!s.open(srv.args("timeout=3000")) && !s.isOpen());
        if (q == Quirk::Unauthorized) { CHECK(std::string(s.lastError()).find("user name and password") != std::string::npos); }
        if (q == Quirk::NotFound || q == Quirk::NotJson || q == Quirk::NoName) { CHECK(std::string(s.lastError()).find("RTSA") != std::string::npos); }
        srv.stop();
    }
    {   // control refused (500): start says what failed and nothing is left running
        Spec sp;
        sp.quirk = Quirk::ControlFails;
        Server srv(sp);
        CHECK(srv.start());
        AaroniaSource s;
        CHECK(s.open(srv.args("timeout=3000")));
        CHECK(s.setCenterFrequencyHz(1e9) && !s.start() && !s.running());
        CHECK(std::string(s.lastError()).find("HTTP status 500") != std::string::npos);
        s.closeDevice();
        srv.stop();
    }
}

void testStreamFaults() {
    struct Case { Quirk q; const char* text; };
    for (const Case& c : {Case{Quirk::StreamUnauthorized, "user name and password"}, Case{Quirk::StreamRefused, "refused the stream"},
                          Case{Quirk::StreamAbrupt, "closed the stream"}, Case{Quirk::StreamSilent, "stopped sending"},
                          Case{Quirk::StreamEnds, "ended the stream"}, Case{Quirk::BadRecord, "malformed"},
                          Case{Quirk::BadChunk, "malformed"}, Case{Quirk::HugeChunk, "larger than this client accepts"}}) {
        Spec sp;
        sp.quirk = c.q;
        Server srv(sp);
        CHECK(srv.start());
        AaroniaSource s;
        CHECK(s.open(srv.args("timeout=1000")));
        s.start();
        const auto t0 = std::chrono::steady_clock::now();
        while (!s.faulted() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(8)) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        CHECK(s.faulted());
        const std::string why = s.faultedWhile() + " " + s.lastError();
        if (!(why.find(c.text) != std::string::npos)) { std::printf("expected '%s' in '%s'\n", c.text, why.c_str()); }
        CHECK(why.find(c.text) != std::string::npos);
        s.closeDevice();
        CHECK(!s.isOpen());
        srv.stop();
    }
    {   // gaps in the packet timestamps are counted; non-I/Q packets skipped; NaN samples zeroed
        for (Quirk q : {Quirk::GapsInTime, Quirk::OtherPackets, Quirk::NonFinite}) {
            Spec sp;
            sp.quirk = q;
            sp.raw = q == Quirk::NonFinite;  // a NaN cannot be written in JSON; only the binary format carries one
            Server srv(sp);
            CHECK(srv.start());
            AaroniaSource s;
            CHECK(s.open(srv.args(q == Quirk::NonFinite ? "timeout=4000,format=float32" : "timeout=4000")));
            CHECK(s.start());
            const auto got = readN(s, 20000);
            CHECK(got.size() >= 20000);
            if (q == Quirk::GapsInTime) { CHECK(s.gapCount() >= 3); }
            if (q == Quirk::OtherPackets) { CHECK(s.otherPackets() >= 3 && s.gapCount() == 0); }
            if (q == Quirk::NonFinite) {
                const std::uint64_t nf = s.nonFiniteSamples();
                CHECK(nf >= 3);
                for (auto& v : got) { CHECK(std::isfinite(v.real()) && std::isfinite(v.imag())); }
            }
            CHECK(!s.faulted());
            s.closeDevice();
            srv.stop();
        }
    }
}

void testCycles() {
    Server srv{Spec{}};
    CHECK(srv.start());
    const auto abandoned = NetSampleSource::readersAbandoned();
    AaroniaSource s;
    CHECK(s.open(srv.args("timeout=4000")));
    CHECK(s.setCenterFrequencyHz(2.4e9));
    for (int i = 0; i < 30; ++i) {
        const bool st = s.start();
        CHECK(st);
        if (!st) { break; }
        if (i % 10 == 0) { CHECK(readN(s, 1000).size() >= 1000); }
        s.stop();
    }
    s.closeDevice();
    for (int i = 0; i < 30; ++i) {
        AaroniaSource t;
        const bool op = t.open(srv.args("timeout=4000"));
        CHECK(op);
        if (!op) { break; }
        if (i % 5 == 0) { CHECK(t.start()); }
        const auto t0 = std::chrono::steady_clock::now();
        t.closeDevice();
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(4000));
    }
    CHECK(NetSampleSource::readersAbandoned() == abandoned);
    srv.stop();
}

void testReadWhenIdle() {
    Server srv{Spec{}};
    CHECK(srv.start());
    AaroniaSource s;
    std::vector<std::complex<float>> tmp(32);
    CHECK(s.read(tmp.data(), tmp.size()) == 0);
    CHECK(s.open(srv.args("timeout=3000")));
    CHECK(s.read(tmp.data(), tmp.size()) == 0);
    s.closeDevice();
    srv.stop();
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testHttpHead();
    testChunks();
    testRecords();
    testFuzz();
    testOpenAndStream(false);
    testOpenAndStream(true);
    testInt16Stream();
    testRemoteConfig();
    testOpenRefusals();
    testStreamFaults();
    testCycles();
    testReadWhenIdle();
    return testSummary("test_aaronia_source");
}
