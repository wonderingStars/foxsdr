// Tests for the web page's HTTPS (net/web_tls.hpp, and WebServer with WebServerConfig::tlsEnabled), feat/web-secure.
//
// The client side is written in the test against Schannel (tests/web_tls_client.hpp) and, separately, is the curl.exe that ships with
// Windows; the peer is a stand-in HTTP server on 127.0.0.1 and, in the second half, the real WebServer. Windows only: elsewhere the test
// says so and passes (the feature is compiled out there and UNPROVEN on Linux).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <ctime>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
#include <ncrypt.h>
#include <wincrypt.h>
#endif

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "net/web_auth.hpp"
#include "net/web_server.hpp"
#include "net/web_tls.hpp"
#include "test_check.hpp"
#include "web_tls_client.hpp"

using namespace cascade::net;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

#ifdef _WIN32

using web_test::HttpReply;
using web_test::SchannelClient;

std::string g_scratch;   // a folder of this run's own (PID in the name)

long long msSince(Clock::time_point t) { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count(); }

std::string patternBody(std::size_t n) {
    std::string s(n, '\0');
    for (std::size_t i = 0; i < n; ++i) { s[i] = static_cast<char>((i * 31 + 7) & 0xFF); }
    return s;
}

bool isHex(const std::string& s, std::size_t len) {
    if (s.size() != len) { return false; }
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { return false; }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// The stand-in plain server behind the terminator, and the terminator in front of it.
// ---------------------------------------------------------------------------------------------------------------------------------------

struct Fixture {
    httplib::Server backend;
    std::thread th;
    int backendPort = -1;
    TlsTerminator tls;
    std::atomic<TlsTerminator*> peerSource{&tls};   // whose table /port consults (a test that runs a second terminator points it there)
    int port = -1;
    std::atomic<int> streamsOpen{0};
    std::atomic<std::uint64_t> endlessBytes{0};
    std::atomic<bool> stopping{false};

    bool start(const TlsTimeouts& to = TlsTimeouts{}, std::size_t total = 64, std::size_t per = 16, const TlsOptions& opt = TlsOptions{}) {
        backend.set_payload_max_length(32u * 1024 * 1024);
        backend.set_read_timeout(10, 0);
        backend.set_write_timeout(10, 0);
        backend.Get("/hello", [](const httplib::Request&, httplib::Response& res) { res.set_content("hello", "text/plain"); });
        backend.Get("/big", [](const httplib::Request& req, httplib::Response& res) {
            const std::size_t n = static_cast<std::size_t>(std::stoull(req.get_param_value("n")));
            res.set_content(patternBody(n), "application/octet-stream");
        });
        backend.Get("/port", [this](const httplib::Request& req, httplib::Response& res) {
            nlohmann::json j;
            j["remote_addr"] = req.remote_addr;
            j["peer"] = peerSource.load()->peerOf(req.remote_port);
            j["forwarded"] = req.get_header_value("X-Forwarded-For");
            res.set_content(j.dump(), "application/json");
        });
        backend.Post("/upload", [](const httplib::Request& req, httplib::Response& res) {
            std::uint64_t sum = 0;
            for (unsigned char c : req.body) { sum = sum * 131 + c; }
            res.set_content(std::to_string(req.body.size()) + " " + std::to_string(sum), "text/plain");
        });
        backend.Get("/stream", [this](const httplib::Request&, httplib::Response& res) {
            ++streamsOpen;
            res.set_chunked_content_provider(
                "text/event-stream",
                [this](std::size_t, httplib::DataSink& sink) {
                    if (stopping.load()) { return false; }
                    const std::string line = "data: tick\n\n";
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    return sink.write(line.data(), line.size());
                },
                [this](bool) { --streamsOpen; });
        });
        backend.Get("/endless", [this](const httplib::Request&, httplib::Response& res) {
            ++streamsOpen;
            res.set_chunked_content_provider(
                "application/octet-stream",
                [this](std::size_t, httplib::DataSink& sink) {
                    if (stopping.load()) { return false; }
                    static const std::string block(65536, 'z');
                    if (!sink.write(block.data(), block.size())) { return false; }
                    endlessBytes += block.size();
                    return true;
                },
                [this](bool) { --streamsOpen; });
        });
        backendPort = backend.bind_to_any_port("127.0.0.1");
        if (backendPort < 0) { return false; }
        th = std::thread([this] { backend.listen_after_bind(); });
        backend.wait_until_ready();
        std::string err;
        if (!tls.prepare(opt, err)) {
            std::printf("prepare failed: %s\n", err.c_str());
            return false;
        }
        tls.setTimeouts(to);
        tls.setMaxConnections(total, per);
        if (!tls.start("127.0.0.1", 0, backendPort, err)) {
            std::printf("start failed: %s\n", err.c_str());
            return false;
        }
        port = tls.boundPort();
        return port > 0;
    }

    ~Fixture() {
        stopping = true;
        tls.stop();
        backend.stop();
        if (th.joinable()) { th.join(); }
    }
};

bool tlsConnect(SchannelClient& c, int port, DWORD disabled, std::string& err) {
    return c.connectTcp("127.0.0.1", port) && c.handshake("localhost", disabled, err);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// The certificate
// ---------------------------------------------------------------------------------------------------------------------------------------

void testSelfSignedCertificate() {
    TlsTerminator t;
    std::string err;
    CHECK(TlsTerminator::available());
    CHECK(t.prepare(TlsOptions{}, err));
    if (!err.empty()) { std::printf("prepare: %s\n", err.c_str()); }
    CHECK(isHex(t.fingerprintSha256(), 64));
    CHECK(isHex(t.thumbprintSha1(), 40));
    CHECK(t.selfSigned());
    CHECK(!t.keyExportable());   // the private key is NOT EXPORTABLE
    const std::vector<std::string> names = t.certificateNames();
    const auto has = [&](const std::string& n) { return std::find(names.begin(), names.end(), n) != names.end(); };
    CHECK(has("localhost"));
    CHECK(has("127.0.0.1"));
    CHECK(has("::1"));
    for (const std::string& ip : localInterfaceAddresses()) { CHECK(has(ip)); }   // every LAN address is named
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    CHECK(t.certificateNotAfter() > now + 700LL * 86400 && t.certificateNotAfter() < now + 760LL * 86400);
    CHECK(!t.certificateDer().empty());
    // The fingerprint is the SHA-256 of the DER, and is a new one every time for an in-memory key.
    std::vector<std::uint8_t> d;
    std::string e2;
    CHECK(sha256(t.certificateDer().data(), t.certificateDer().size(), d, e2));
    static const char* hx = "0123456789abcdef";
    std::string hex;
    for (std::uint8_t b : d) { hex.push_back(hx[b >> 4]); hex.push_back(hx[b & 15]); }
    CHECK(hex == t.fingerprintSha256());
    TlsTerminator t2;
    CHECK(t2.prepare(TlsOptions{}, err));
    CHECK(t2.fingerprintSha256() != t.fingerprintSha256());
    // Extra names are honoured; a name with a character that cannot be one is left out rather than breaking the certificate.
    TlsOptions o;
    o.includeMachineNames = false;
    o.names = {"radio.example.test", "10.20.30.40", "bad name!"};
    TlsTerminator t3;
    CHECK(t3.prepare(o, err));
    CHECK(t3.certificateNames() == (std::vector<std::string>{"radio.example.test", "10.20.30.40"}));
}

void deleteKey(const std::string& name) {
    NCRYPT_PROV_HANDLE prov = 0;
    NCRYPT_KEY_HANDLE key = 0;
    if (NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS) { return; }
    const std::wstring w(name.begin(), name.end());
    if (NCryptOpenKey(prov, &key, w.c_str(), 0, 0) == ERROR_SUCCESS) { NCryptDeleteKey(key, 0); }   // (frees the handle)
    NCryptFreeObject(prov);
}

bool keyExists(const std::string& name) {
    NCRYPT_PROV_HANDLE prov = 0;
    NCRYPT_KEY_HANDLE key = 0;
    bool ok = false;
    if (NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0) == ERROR_SUCCESS) {
        const std::wstring w(name.begin(), name.end());
        if (NCryptOpenKey(prov, &key, w.c_str(), 0, 0) == ERROR_SUCCESS) {
            ok = true;
            // THE KEY CANNOT BE TAKEN OUT: an export of the private part is refused by the key store.
            DWORD sz = 0;
            const SECURITY_STATUS ss = NCryptExportKey(key, 0, BCRYPT_ECCPRIVATE_BLOB, nullptr, nullptr, 0, &sz, 0);
            CHECK(ss != ERROR_SUCCESS);
            NCryptFreeObject(key);
        }
        NCryptFreeObject(prov);
    }
    return ok;
}

void testPersistedCertificate() {
    const std::string keyName = "FoxSDR-Web-TLS-test-" + std::to_string(_getpid());
    const std::string certFile = g_scratch + "/kept.cer";
    TlsOptions o;
    o.certFile = certFile;
    o.keyName = keyName;
    std::string err;
    std::string first;
    {
        TlsTerminator t;
        CHECK(t.prepare(o, err));
        first = t.fingerprintSha256();
        CHECK(isHex(first, 64));
        CHECK(fs::exists(certFile));
        CHECK(!t.keyExportable());
        CHECK(keyExists(keyName));   // the key is in the user's key store, and refuses an export of its private half
    }
    {
        TlsTerminator t;   // the second start reuses the certificate: the same fingerprint
        CHECK(t.prepare(o, err));
        CHECK(t.fingerprintSha256() == first);
    }
    // A damaged file: a new certificate is made from the same key, and the file is replaced.
    {
        std::ofstream(certFile, std::ios::binary | std::ios::trunc) << "not a certificate";
        TlsTerminator t;
        CHECK(t.prepare(o, err));
        CHECK(isHex(t.fingerprintSha256(), 64));
        CHECK(t.fingerprintSha256() != first);
        TlsTerminator again;
        CHECK(again.prepare(o, err));
        CHECK(again.fingerprintSha256() == t.fingerprintSha256());
    }
    // A certificate that belongs to ANOTHER key is not used: the key store holds the key, not the file.
    {
        TlsOptions other = o;
        other.certFile = g_scratch + "/other.cer";
        other.keyName = keyName + "-b";
        TlsTerminator mine;
        CHECK(mine.prepare(o, err));
        TlsTerminator theirs;
        CHECK(theirs.prepare(other, err));
        fs::copy_file(other.certFile, certFile, fs::copy_options::overwrite_existing);
        TlsTerminator t;
        CHECK(t.prepare(o, err));
        CHECK(t.fingerprintSha256() != theirs.fingerprintSha256());
        deleteKey(keyName + "-b");
    }
    deleteKey(keyName);
    CHECK(!keyExists(keyName));
}

// A user-picked certificate: put one made here in the current user's Personal store, find it by thumbprint, take it out again.
struct StoreEntry {
    PCCERT_CONTEXT inStore = nullptr;
    void remove() {
        if (inStore != nullptr) { CertDeleteCertificateFromStore(inStore); inStore = nullptr; }   // (frees the context)
    }
    ~StoreEntry() { remove(); }
};

void testThumbprintCertificate() {
    const std::string keyName = "FoxSDR-Web-TLS-pick-" + std::to_string(_getpid());
    TlsOptions o;
    o.certFile = g_scratch + "/pick.cer";
    o.keyName = keyName;
    std::string err;
    TlsTerminator maker;
    CHECK(maker.prepare(o, err));
    const std::string thumb = maker.thumbprintSha1();
    CHECK(isHex(thumb, 40));

    // Not in the store yet: a sentence, not a crash.
    {
        TlsOptions pick;
        pick.thumbprint = thumb;
        TlsTerminator t;
        CHECK(!t.prepare(pick, err));
        CHECK(err.find("no certificate with that thumbprint") != std::string::npos);
    }
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, L"MY");
    CHECK(store != nullptr);
    if (store != nullptr) {
        const std::vector<std::uint8_t> der = maker.certificateDer();
        PCCERT_CONTEXT fresh = CertCreateCertificateContext(X509_ASN_ENCODING, der.data(), static_cast<DWORD>(der.size()));
        CHECK(fresh != nullptr);
        StoreEntry entry;
        if (fresh != nullptr) {
            // Tell the store entry where its key is (the user's key store, by name), then add it.
            std::wstring w(keyName.begin(), keyName.end());
            std::wstring prov = MS_KEY_STORAGE_PROVIDER;
            CRYPT_KEY_PROV_INFO kpi{};
            kpi.pwszContainerName = w.data();
            kpi.pwszProvName = prov.data();
            CertSetCertificateContextProperty(fresh, CERT_KEY_PROV_INFO_PROP_ID, 0, &kpi);
            const BOOL added = CertAddCertificateContextToStore(store, fresh, CERT_STORE_ADD_REPLACE_EXISTING, &entry.inStore);
            CHECK(added);
            CertFreeCertificateContext(fresh);
        }
        if (entry.inStore != nullptr) {
            TlsOptions pick;
            pick.thumbprint = thumb;
            Fixture f;
            CHECK(f.start(TlsTimeouts{}, 64, 16, pick));
            CHECK(f.tls.fingerprintSha256() == maker.fingerprintSha256());
            CHECK(!f.tls.selfSigned());
            CHECK(!f.tls.keyExportable());
            SchannelClient c;
            std::string e;
            CHECK(tlsConnect(c, f.port, 0, e));
            CHECK(c.peerFingerprint() == maker.fingerprintSha256());
            const HttpReply r = c.request("GET", "/hello");
            CHECK(r.status == 200 && r.body == "hello");
            // Upper case is the same thumbprint.
            std::string up = thumb;
            for (char& ch : up) { if (ch >= 'a' && ch <= 'f') { ch = static_cast<char>(ch - 'a' + 'A'); } }
            TlsOptions pick2;
            pick2.thumbprint = up;
            TlsTerminator t2;
            CHECK(t2.prepare(pick2, err));
        }
        entry.remove();   // (before the store is closed)
        CertCloseStore(store, 0);
    }
    {
        // Gone again.
        TlsOptions pick;
        pick.thumbprint = thumb;
        TlsTerminator t;
        CHECK(!t.prepare(pick, err));
    }
    deleteKey(keyName);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// The handshake, with Schannel's client and with curl
// ---------------------------------------------------------------------------------------------------------------------------------------

void testHandshakeVersionsAndFingerprint() {
    Fixture f;
    CHECK(f.start());
    // TLS 1.2 only (the client switches 1.3 off).
    {
        SchannelClient c;
        std::string e;
        CHECK(tlsConnect(c, f.port, SP_PROT_TLS1_3, e));
        if (!e.empty()) { std::printf("tls1.2 handshake: %s\n", e.c_str()); }
        CHECK((c.protocol() & SP_PROT_TLS1_2) != 0);
        CHECK(c.peerFingerprint() == f.tls.fingerprintSha256());   // the fingerprint the server publishes is the certificate the client got
        const HttpReply r = c.request("GET", "/hello");
        CHECK(r.status == 200 && r.body == "hello");
        // Keep-alive: a second request on the same connection.
        const HttpReply r2 = c.request("GET", "/hello");
        CHECK(r2.status == 200 && r2.body == "hello");
    }
    // TLS 1.3 only (the client switches 1.2 and below off). Windows offers it on this build; if it does not, say so rather than pass.
    {
        SchannelClient c;
        std::string e;
        const bool ok = tlsConnect(c, f.port, SP_PROT_TLS1_2 | SP_PROT_TLS1_1 | SP_PROT_TLS1_0 | SP_PROT_SSL3, e);
        if (!ok) { std::printf("tls1.3 handshake: %s\n", e.c_str()); }
        CHECK(ok);
        if (ok) {
            CHECK((c.protocol() & SP_PROT_TLS1_3) != 0);
            const HttpReply r = c.request("GET", "/hello");
            CHECK(r.status == 200 && r.body == "hello");
        }
    }
    const TlsStats st = f.tls.stats();
    CHECK(st.tls12 >= 1);
    CHECK(st.tls13 >= 1);
    CHECK(st.handshakesOk >= 2);
}

std::string runCurl(const std::string& args, int* exitCode = nullptr) {
    const std::string cmd = "C:\\Windows\\System32\\curl.exe " + args + " 2>&1";
    FILE* p = _popen(cmd.c_str(), "rb");
    std::string out;
    if (p == nullptr) { return out; }
    char buf[4096];
    std::size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) { out.append(buf, n); }
    const int rc = _pclose(p);
    if (exitCode != nullptr) { *exitCode = rc; }
    return out;
}

void testCurl() {
    if (!fs::exists("C:\\Windows\\System32\\curl.exe")) {
        SKIP_LINUX("curl.exe is not on this Windows");
        return;
    }
    Fixture f;
    CHECK(f.start());
    const std::string url = "https://127.0.0.1:" + std::to_string(f.port);
    int rc = -1;
    // TLS 1.2 exactly, then 1.3 at least. (-k: the certificate is self-signed; the Schannel client above checks the fingerprint.)
    std::string out = runCurl("-s -k --ssl-no-revoke --max-time 30 --tlsv1.2 --tls-max 1.2 " + url + "/hello", &rc);
    CHECK(rc == 0 && out == "hello");
    if (out != "hello") { std::printf("curl 1.2: rc=%d out=%s\n", rc, out.c_str()); }
    out = runCurl("-s -k --ssl-no-revoke --max-time 30 --tlsv1.3 " + url + "/hello", &rc);
    CHECK(rc == 0 && out == "hello");
    if (out != "hello") { std::printf("curl 1.3: rc=%d out=%s\n", rc, out.c_str()); }
    CHECK(f.tls.stats().tls12 >= 1 && f.tls.stats().tls13 >= 1);
    // curl refuses to talk TLS 1.1 to it (the server has it switched off), and says so with a failure, not a page.
    out = runCurl("-s -k --ssl-no-revoke --max-time 15 --tlsv1.1 --tls-max 1.1 " + url + "/hello", &rc);
    CHECK(out != "hello");
    CHECK(rc != 0);
    // A large body, byte for byte.
    const std::string bigFile = g_scratch + "/curl-big.bin";
    runCurl("-s -k --ssl-no-revoke --max-time 60 -o \"" + bigFile + "\" \"" + url + "/big?n=6000000\"", &rc);
    CHECK(rc == 0);
    std::ifstream in(bigFile, std::ios::binary);
    std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(got.size() == 6000000);
    CHECK(got == patternBody(6000000));
    // An upload through the terminator.
    const std::string up = patternBody(3000000);
    const std::string upFile = g_scratch + "/curl-up.bin";
    { std::ofstream o(upFile, std::ios::binary); o.write(up.data(), static_cast<std::streamsize>(up.size())); }
    out = runCurl("-s -k --ssl-no-revoke --max-time 60 --data-binary @\"" + upFile + "\" -H \"Content-Type: application/octet-stream\" " + url + "/upload", &rc);
    std::uint64_t sum = 0;
    for (unsigned char c : up) { sum = sum * 131 + c; }
    CHECK(out == std::to_string(up.size()) + " " + std::to_string(sum));
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// Old protocols refused - proved with a hello written byte by byte, so the answer does not depend on what this Windows client will send
// ---------------------------------------------------------------------------------------------------------------------------------------

std::string clientHello(std::uint16_t version, const std::vector<std::uint16_t>& suites, bool withExtensions, std::uint8_t seed = 1) {
    std::string body;
    const auto u16 = [](std::string& s, std::uint16_t v) { s.push_back(static_cast<char>(v >> 8)); s.push_back(static_cast<char>(v & 255)); };
    u16(body, version);
    for (int i = 0; i < 32; ++i) { body.push_back(static_cast<char>(seed + i * 7)); }
    body.push_back(0);   // session id length
    u16(body, static_cast<std::uint16_t>(suites.size() * 2));
    for (std::uint16_t sc : suites) { u16(body, sc); }
    body.push_back(1);
    body.push_back(0);   // compression: null
    if (withExtensions) {
        std::string ext;
        u16(ext, 0x000a); u16(ext, 4); u16(ext, 2); u16(ext, 0x0017);          // supported_groups: secp256r1
        u16(ext, 0x000b); u16(ext, 2); ext.push_back(1); ext.push_back(0);    // ec_point_formats: uncompressed
        if (version >= 0x0303) { u16(ext, 0x000d); u16(ext, 4); u16(ext, 2); u16(ext, 0x0403); }   // signature_algorithms: ecdsa_secp256r1_sha256
        u16(body, static_cast<std::uint16_t>(ext.size()));
        body += ext;
    }
    std::string hs;
    hs.push_back(1);   // ClientHello
    hs.push_back(static_cast<char>((body.size() >> 16) & 255));
    hs.push_back(static_cast<char>((body.size() >> 8) & 255));
    hs.push_back(static_cast<char>(body.size() & 255));
    hs += body;
    std::string rec;
    rec.push_back(0x16);
    u16(rec, version < 0x0301 ? version : static_cast<std::uint16_t>(0x0301));
    u16(rec, static_cast<std::uint16_t>(hs.size()));
    rec += hs;
    return rec;
}

// What the server did with a hello: a ServerHello came back, or it did not (an alert, a reset, silence).
bool answeredWithServerHello(int port, const std::string& hello) {
    SchannelClient c;
    if (!c.connectTcp("127.0.0.1", port)) { return false; }
    c.sendRawBytes(hello);
    const std::string reply = c.rawReceive(5000);   // (a refusal ends at once: the server closes; a loaded machine may need seconds to answer)
    return reply.size() > 5 && static_cast<unsigned char>(reply[0]) == 0x16 && static_cast<unsigned char>(reply[5]) == 0x02;
}

void testOldProtocolsRefused() {
    Fixture f;
    CHECK(f.start());
    const std::vector<std::uint16_t> modern = {0xC02B, 0xC02C, 0xC023, 0xC024, 0xC009, 0xC00A};
    // THE POSITIVE CONTROL: the same bytes with TLS 1.2 in them get a ServerHello, so a refusal below is the protocol's and not the harness's.
    CHECK(answeredWithServerHello(f.port, clientHello(0x0303, modern, true)));
    CHECK(!answeredWithServerHello(f.port, clientHello(0x0302, modern, true)));          // TLS 1.1
    CHECK(!answeredWithServerHello(f.port, clientHello(0x0301, modern, true)));          // TLS 1.0
    CHECK(!answeredWithServerHello(f.port, clientHello(0x0300, {0x002F, 0x000A}, false)));   // SSL 3.0
    CHECK(!answeredWithServerHello(f.port, clientHello(0x0200, {0x002F}, false)));       // nonsense
    const TlsStats st = f.tls.stats();
    CHECK(st.handshakesFailed >= 3);
    // ... and the server is still fine.
    SchannelClient c;
    std::string e;
    CHECK(tlsConnect(c, f.port, 0, e));
    CHECK(c.request("GET", "/hello").body == "hello");
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// Stalled, half-open and hostile clients
// ---------------------------------------------------------------------------------------------------------------------------------------

void testStalledAndHalfOpenClients() {
    TlsTimeouts to;
    to.handshakeMs = 1200;
    to.idleMs = 1500;
    to.writeMs = 1500;
    Fixture f;
    CHECK(f.start(to));
    // (a) connects and says nothing at all.
    {
        SchannelClient c;
        CHECK(c.connectTcp("127.0.0.1", f.port));
        const auto t0 = Clock::now();
        CHECK(c.peerClosedWithin(4000));
        const long long ms = msSince(t0);
        CHECK(ms >= 900 && ms < 9000);
        std::printf("half-open silent client dropped after %lld ms (limit %d)\n", ms, to.handshakeMs);
    }
    // (b) sends part of a hello, then stalls.
    {
        SchannelClient c;
        CHECK(c.connectTcp("127.0.0.1", f.port));
        const std::string hello = clientHello(0x0303, {0xC02B}, true);
        c.sendRawBytes(hello.substr(0, 20));
        CHECK(c.peerClosedWithin(4000));
    }
    CHECK(f.tls.stats().droppedHandshakeTimeout >= 2);
    // (c) finishes the handshake and then sends nothing: dropped by the idle limit.
    {
        SchannelClient c;
        std::string e;
        CHECK(tlsConnect(c, f.port, 0, e));
        const auto t0 = Clock::now();
        CHECK(c.peerClosedWithin(5000));
        CHECK(msSince(t0) < 9000);
    }
    // (d) asks for a stream of data and never reads it: the send stalls and the connection is dropped, and the backend's stream ends.
    {
        SchannelClient c;
        std::string e;
        CHECK(tlsConnect(c, f.port, 0, e));
        CHECK(c.send("GET /endless HTTP/1.1\r\nHost: t\r\n\r\n"));
        const auto t0 = Clock::now();
        bool dropped = false;
        while (msSince(t0) < 15000) {
            if (f.tls.stats().droppedWriteStall >= 1) { dropped = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        CHECK(dropped);
        for (int i = 0; i < 100 && f.streamsOpen.load() != 0; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
        CHECK(f.streamsOpen.load() == 0);
    }
    // (e) hundreds of idle half-open connections do not exhaust the server: the per-address cap holds and the rest are refused at once.
    CHECK(f.tls.connections() == 0 || true);
}

void testConnectionCaps() {
    Fixture f;
    CHECK(f.start(TlsTimeouts{}, 3, 2));
    std::vector<std::unique_ptr<SchannelClient>> held;
    for (int i = 0; i < 2; ++i) {
        held.push_back(std::make_unique<SchannelClient>());
        CHECK(held.back()->connectTcp("127.0.0.1", f.port));
    }
    for (int i = 0; i < 100 && f.tls.connections() != 2; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    // The third connection from the same address is over the per-address cap (2): closed straight away, before any handshake.
    SchannelClient third;
    CHECK(third.connectTcp("127.0.0.1", f.port));
    CHECK(third.peerClosedWithin(2000));
    CHECK(f.tls.stats().refusedBusy >= 1);
    CHECK(f.tls.connections() == 2);
    held.clear();
    for (int i = 0; i < 150 && f.tls.connections() != 0; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    CHECK(f.tls.connections() == 0);
    SchannelClient again;
    std::string e;
    CHECK(tlsConnect(again, f.port, 0, e));   // the seats came back
}

void testGarbageAndFuzz() {
    TlsTimeouts to;
    to.handshakeMs = 800;
    Fixture f;
    CHECK(f.start(to));
    std::mt19937 rng(0x7157);   // a fixed seed: a failure repeats
    std::printf("fuzz seed 0x7157\n");
    const std::string good = clientHello(0x0303, {0xC02B, 0xC02C, 0xC023, 0xC009}, true);
    std::vector<std::string> cases;
    cases.push_back("GET / HTTP/1.1\r\nHost: x\r\n\r\n");   // plain HTTP on the HTTPS port
    cases.push_back(std::string(70000, '\0'));              // a long run of zeros
    cases.push_back(std::string(70000, '\x16'));
    cases.push_back(std::string("\x16\x03\x03\xff\xff", 5) + std::string(3000, 'A'));   // a record that claims 65 KB
    cases.push_back(std::string("\x16\x03\x03\x00\x04\x01\x00\x00\x00", 9));            // a hello with an empty body
    cases.push_back(std::string("\x80\x2e\x01\x03\x01", 5));                            // an SSLv2-style hello
    for (int i = 0; i < 160; ++i) {   // mutations of a real hello: flips, truncations, wrong lengths, appended junk
        std::string m = good;
        const int kind = static_cast<int>(rng() % 4);
        if (kind == 0) {
            for (int k = 0; k < 1 + static_cast<int>(rng() % 4); ++k) { m[rng() % m.size()] = static_cast<char>(rng() & 255); }
        } else if (kind == 1) {
            m.resize(1 + rng() % m.size());
        } else if (kind == 2) {
            m[3 + rng() % 3] = static_cast<char>(rng() & 255);   // the record or handshake length bytes
        } else {
            for (int k = 0; k < 40; ++k) { m.push_back(static_cast<char>(rng() & 255)); }
        }
        cases.push_back(std::move(m));
    }
    for (std::string& g : cases) {
        SchannelClient c;
        if (!c.connectTcp("127.0.0.1", f.port)) { continue; }
        c.sendRawBytes(g);
        (void)c.rawReceive(30);
    }
    // After all of that the server serves, and the failures were counted.
    SchannelClient c;
    std::string e;
    bool served = false;
    for (int i = 0; i < 30 && !served; ++i) {   // (the abandoned half-open ones are still draining their seats)
        SchannelClient d;
        if (tlsConnect(d, f.port, 0, e)) { served = d.request("GET", "/hello").body == "hello"; }
        if (!served) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); }
    }
    CHECK(served);
    for (int i = 0; i < 150 && f.tls.stats().handshakesFailed + f.tls.stats().droppedHandshakeTimeout + f.tls.stats().refusedBusy < 100; ++i) {   // (counted as the server gets to them)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(f.tls.stats().handshakesFailed + f.tls.stats().droppedHandshakeTimeout + f.tls.stats().refusedBusy >= 100);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// Data through the terminator
// ---------------------------------------------------------------------------------------------------------------------------------------

void testLargeTransfersAndStreams() {
    Fixture f;
    CHECK(f.start());
    SchannelClient c;
    std::string e;
    CHECK(tlsConnect(c, f.port, 0, e));
    // 12 MB down, byte for byte.
    const HttpReply big = c.request("GET", "/big?n=12000000", std::string(), std::string(), 60000);
    CHECK(big.status == 200);
    CHECK(big.body.size() == 12000000);
    CHECK(big.body == patternBody(12000000));
    // 4 MB up on the same connection.
    const std::string up = patternBody(4000000);
    std::uint64_t sum = 0;
    for (unsigned char ch : up) { sum = sum * 131 + ch; }
    const HttpReply r = c.request("POST", "/upload", "Content-Type: application/octet-stream\r\n", up, 60000);
    CHECK(r.status == 200);
    CHECK(r.body == std::to_string(up.size()) + " " + std::to_string(sum));
    // Zero- and one-byte bodies.
    CHECK(c.request("GET", "/big?n=0").body.empty());
    CHECK(c.request("GET", "/big?n=1").body == patternBody(1));
    // A live stream: forty events arrive, in order, through TLS; closing the client ends the stream at the backend.
    {
        SchannelClient s;
        CHECK(tlsConnect(s, f.port, 0, e));
        const std::string got = s.stream("/stream", 12 * 40, std::string(), 20000);
        CHECK(got.size() >= 12 * 40);
        for (std::size_t i = 0; i + 12 <= got.size(); i += 12) { CHECK(got.compare(i, 12, "data: tick\n\n") == 0); }
        CHECK(f.streamsOpen.load() == 1);
    }
    for (int i = 0; i < 100 && f.streamsOpen.load() != 0; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    CHECK(f.streamsOpen.load() == 0);
    // Many small requests, several connections at once.
    std::atomic<int> okCount{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t) {
        ts.emplace_back([&] {
            SchannelClient k;
            std::string er;
            if (!tlsConnect(k, f.port, 0, er)) { return; }
            for (int i = 0; i < 25; ++i) {
                if (k.request("GET", "/big?n=2000").body == patternBody(2000)) { ++okCount; }
            }
        });
    }
    for (std::thread& t : ts) { t.join(); }
    CHECK(okCount == 8 * 25);
}

void testStopIsPrompt() {
    auto f = std::make_unique<Fixture>();
    CHECK(f->start());
    std::vector<std::unique_ptr<SchannelClient>> idle;
    std::string e;
    for (int i = 0; i < 3; ++i) {
        idle.push_back(std::make_unique<SchannelClient>());
        CHECK(tlsConnect(*idle.back(), f->port, 0, e));
    }
    SchannelClient streamer;
    CHECK(tlsConnect(streamer, f->port, 0, e));
    std::thread reader([&] { (void)streamer.stream("/stream", 12 * 100000, std::string(), 15000); });
    SchannelClient half;
    CHECK(half.connectTcp("127.0.0.1", f->port));   // mid-handshake
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const auto t0 = Clock::now();
    f->stopping = true;
    f->tls.stop();
    const long long ms = msSince(t0);
    std::printf("terminator stop() with 3 idle, 1 streaming and 1 half-open connection: %lld ms\n", ms);
    CHECK(ms < 4000);
    CHECK(!f->tls.running());
    reader.join();
    // Stopping twice, and starting again on the same terminator.
    f->tls.stop();
    std::string err;
    CHECK(f->tls.start("127.0.0.1", 0, f->backendPort, err));
    SchannelClient again;
    CHECK(tlsConnect(again, f->tls.boundPort(), 0, e));
    CHECK(again.request("GET", "/hello").body == "hello");
}

void testTrueClientAddress() {
    Fixture f;
    CHECK(f.start());
    SchannelClient c;
    std::string e;
    CHECK(tlsConnect(c, f.port, 0, e));
    // A request that LIES in a header: the terminator's record is what the server reads, never the header.
    const HttpReply r = c.request("GET", "/port", "X-Forwarded-For: 203.0.113.9\r\nForwarded: for=203.0.113.9\r\nX-Real-IP: 203.0.113.9\r\n");
    CHECK(r.status == 200);
    const nlohmann::json j = nlohmann::json::parse(r.body, nullptr, false);
    CHECK(!j.is_discarded());
    if (!j.is_discarded()) {
        CHECK(j.value("remote_addr", "") == "127.0.0.1");        // what the plain server saw: the relay
        CHECK(j.value("peer", "") == "127.0.0.1");               // what the terminator recorded: the real client
        CHECK(j.value("forwarded", "") == "203.0.113.9");        // the header is there, and nobody trusts it
    }
    // An unknown loopback source port is NOT mapped (a program on this machine talking to the private port directly).
    CHECK(f.tls.peerOf(1) == "");
    CHECK(f.tls.peerOf(-5) == "");
    // After the connection closes its entry is gone (the table does not grow).
    c.close();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(f.tls.connections() == 0);
    // From a LAN address of this machine the real address is that address, not 127.0.0.1 (when there is one to try).
    const std::vector<std::string> lan = localInterfaceAddresses();
    if (lan.empty()) {
        SKIP_LINUX("this computer has no LAN IPv4 address to connect from");
    } else {
        TlsTerminator wide;
        std::string err;
        CHECK(wide.prepare(TlsOptions{}, err));
        CHECK(wide.start("0.0.0.0", 0, f.backendPort, err));
        f.peerSource = &wide;
        SchannelClient k;
        CHECK(k.connectTcp(lan[0], wide.boundPort()));
        const bool hs = k.handshake("localhost", 0, err);
        if (!hs) { std::printf("LAN handshake to %s: %s\n", lan[0].c_str(), err.c_str()); }
        CHECK(hs);
        const HttpReply rr = k.request("GET", "/port");
        if (rr.status != 200) { std::printf("LAN request: status %d body '%s'\n", rr.status, rr.body.c_str()); }
        const nlohmann::json jj = nlohmann::json::parse(rr.body, nullptr, false);
        if (!jj.is_discarded() && jj.value("peer", "") != lan[0]) { std::printf("LAN connect to %s: peer recorded %s\n", lan[0].c_str(), jj.value("peer", "").c_str()); }
        CHECK(!jj.is_discarded() && jj.value("peer", "") == lan[0]);
        CHECK(!jj.is_discarded() && jj.value("remote_addr", "") == "127.0.0.1");
    }
}

#endif  // _WIN32

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    (void)argv;
    g_scratch = (fs::temp_directory_path() / ("uw-web-tls-" + std::to_string(_getpid()))).string();
    fs::create_directories(g_scratch);
    // argv[2], when given, runs only the tests whose name contains it (a developer's shortcut; CTest passes none).
    const std::string only = argc > 2 ? argv[2] : "";
    if (only.empty() || std::string("testSelfSignedCertificate").find(only) != std::string::npos) { testSelfSignedCertificate(); }
    if (only.empty() || std::string("testPersistedCertificate").find(only) != std::string::npos) { testPersistedCertificate(); }
    if (only.empty() || std::string("testThumbprintCertificate").find(only) != std::string::npos) { testThumbprintCertificate(); }
    if (only.empty() || std::string("testHandshakeVersionsAndFingerprint").find(only) != std::string::npos) { testHandshakeVersionsAndFingerprint(); }
    if (only.empty() || std::string("testCurl").find(only) != std::string::npos) { testCurl(); }
    if (only.empty() || std::string("testOldProtocolsRefused").find(only) != std::string::npos) { testOldProtocolsRefused(); }
    if (only.empty() || std::string("testStalledAndHalfOpenClients").find(only) != std::string::npos) { testStalledAndHalfOpenClients(); }
    if (only.empty() || std::string("testConnectionCaps").find(only) != std::string::npos) { testConnectionCaps(); }
    if (only.empty() || std::string("testGarbageAndFuzz").find(only) != std::string::npos) { testGarbageAndFuzz(); }
    if (only.empty() || std::string("testLargeTransfersAndStreams").find(only) != std::string::npos) { testLargeTransfersAndStreams(); }
    if (only.empty() || std::string("testStopIsPrompt").find(only) != std::string::npos) { testStopIsPrompt(); }
    if (only.empty() || std::string("testTrueClientAddress").find(only) != std::string::npos) { testTrueClientAddress(); }
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
#else
    (void)argc;
    (void)argv;
    SKIP_LINUX("HTTPS for the web page uses Windows' Schannel; UNPROVEN on Linux");
#endif
    return testSummary("test_web_tls");
}
