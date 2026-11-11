// The HTTPS terminator. See net/web_tls.hpp for the design and what it refuses; this file is the Schannel (SSPI) and CNG code.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/web_tls.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "core/thread_beats.hpp"
#include "net/web_auth.hpp"
#include "net/web_server.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define SECURITY_WIN32
#define SCHANNEL_USE_BLACKLISTS   // (schannel.h declares SCH_CREDENTIALS and TLS_PARAMETERS only under this)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <ncrypt.h>
#include <security.h>
#include <sspi.h>
#include <subauth.h>
#include <schannel.h>
#endif

namespace cascade::net {

#ifdef _WIN32

namespace {

using Bytes = std::vector<std::uint8_t>;
using SteadyClock = std::chrono::steady_clock;

std::string hexOf(const std::uint8_t* p, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 15]);
    }
    return s;
}

std::string codeText(const char* what, long code) {
    char b[160];
    std::snprintf(b, sizeof(b), "%s failed (0x%08lX)", what, static_cast<unsigned long>(code));
    return b;
}

std::wstring widen(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (char c : s) { w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c))); }
    return w;
}

// --- Sockets ---------------------------------------------------------------------------------------------------------------------------

bool wsaUp() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA d;
        ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
    });
    return ok;
}

bool setNonBlocking(SOCKET s) {
    u_long m = 1;
    return ioctlsocket(s, FIONBIO, &m) == 0;
}

void noDelay(SOCKET s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
}

// 1 ready, 0 timed out, -1 error.
int waitFor(SOCKET s, bool forWrite, int ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    const int r = forWrite ? select(0, nullptr, &set, nullptr, &tv) : select(0, &set, nullptr, nullptr, &tv);
    if (r < 0) { return -1; }
    return r > 0 ? 1 : 0;
}

std::string peerText(const sockaddr_storage& ss) {
    char buf[INET6_ADDRSTRLEN] = {0};
    if (ss.ss_family == AF_INET) {
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(&ss)->sin_addr, buf, sizeof(buf));
    } else if (ss.ss_family == AF_INET6) {
        const sockaddr_in6* a = reinterpret_cast<const sockaddr_in6*>(&ss);
        if (IN6_IS_ADDR_V4MAPPED(&a->sin6_addr)) {
            in_addr v4;
            std::memcpy(&v4, a->sin6_addr.u.Byte + 12, 4);
            inet_ntop(AF_INET, &v4, buf, sizeof(buf));
        } else {
            inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof(buf));
        }
    }
    return buf;
}

// --- The credential: a certificate and Schannel's handle on it -------------------------------------------------------------------------

struct Credential {
    PCCERT_CONTEXT cert = nullptr;
    NCRYPT_PROV_HANDLE prov = 0;
    NCRYPT_KEY_HANDLE key = 0;           // kept open for an in-memory key (the certificate refers to it)
    CredHandle handle{};
    bool haveHandle = false;
    bool selfSigned = false;
    bool exportable = true;
    bool deleteKeyOnRelease = false;   // a key made for this run only (no certificate file): it is removed from the key store again
    std::vector<std::string> names;
    Bytes der;
    std::string sha256Hex;
    std::string sha1Hex;
    std::int64_t notAfter = 0;

    Credential() = default;
    Credential(const Credential&) = delete;
    Credential& operator=(const Credential&) = delete;
    ~Credential() { release(); }

    void release() {
        if (haveHandle) { FreeCredentialsHandle(&handle); haveHandle = false; }
        if (cert != nullptr) { CertFreeCertificateContext(cert); cert = nullptr; }
        if (key != 0) {
            if (deleteKeyOnRelease) { NCryptDeleteKey(key, NCRYPT_SILENT_FLAG); } else { NCryptFreeObject(key); }   // (deleting frees the handle)
            key = 0;
        }
        if (prov != 0) { NCryptFreeObject(prov); prov = 0; }
        names.clear();
        der.clear();
    }
};

std::int64_t fileTimeToUnix(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<std::int64_t>((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

FILETIME addDays(const FILETIME& ft, long long days) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart = static_cast<ULONGLONG>(static_cast<long long>(u.QuadPart) + days * 24LL * 3600LL * 10000000LL);
    FILETIME o;
    o.dwLowDateTime = u.LowPart;
    o.dwHighDateTime = u.HighPart;
    return o;
}

void describe(Credential& c) {
    c.der.assign(c.cert->pbCertEncoded, c.cert->pbCertEncoded + c.cert->cbCertEncoded);
    std::vector<std::uint8_t> d;
    std::string ignored;
    if (sha256(c.der.data(), c.der.size(), d, ignored)) { c.sha256Hex = hexOf(d.data(), d.size()); }
    BYTE sha1[20];
    DWORD n = sizeof(sha1);
    if (CertGetCertificateContextProperty(c.cert, CERT_SHA1_HASH_PROP_ID, sha1, &n)) { c.sha1Hex = hexOf(sha1, n); }
    c.notAfter = fileTimeToUnix(c.cert->pCertInfo->NotAfter);
}

// Reads the key's export policy: false only when it says "not exportable".
bool keyIsExportable(NCRYPT_KEY_HANDLE k) {
    DWORD policy = 0, cb = 0;
    if (NCryptGetProperty(k, NCRYPT_EXPORT_POLICY_PROPERTY, reinterpret_cast<PBYTE>(&policy), sizeof(policy), &cb, 0) != ERROR_SUCCESS) { return true; }
    return policy != 0;
}

struct AltNames {
    std::vector<std::wstring> dns;
    std::vector<Bytes> ips;
    std::vector<std::string> text;
};

AltNames collectNames(const TlsOptions& opt) {
    AltNames a;
    std::set<std::string> seen;
    const auto add = [&](const std::string& raw) {
        std::string n = raw;
        while (!n.empty() && n.back() == '.') { n.pop_back(); }
        std::string key = n;
        for (char& ch : key) {
            if (ch >= 'A' && ch <= 'Z') { ch = static_cast<char>(ch - 'A' + 'a'); }
        }
        if (n.empty() || n.size() > 253 || !seen.insert(key).second) { return; }
        in_addr v4;
        in6_addr v6;
        if (inet_pton(AF_INET, n.c_str(), &v4) == 1) {
            a.ips.emplace_back(reinterpret_cast<const std::uint8_t*>(&v4), reinterpret_cast<const std::uint8_t*>(&v4) + 4);
        } else if (inet_pton(AF_INET6, n.c_str(), &v6) == 1) {
            a.ips.emplace_back(reinterpret_cast<const std::uint8_t*>(&v6), reinterpret_cast<const std::uint8_t*>(&v6) + 16);
        } else {
            for (char c : n) {
                const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
                if (!ok) { return; }
            }
            a.dns.push_back(widen(n));
        }
        a.text.push_back(n);
    };
    if (opt.includeMachineNames) {
        wchar_t host[256];
        DWORD n = 256;
        if (GetComputerNameExW(ComputerNameDnsFullyQualified, host, &n) && n > 0) {
            std::string h;
            for (DWORD i = 0; i < n; ++i) { h.push_back(host[i] < 128 ? static_cast<char>(host[i]) : '?'); }
            if (h.find('?') == std::string::npos) { add(h); }
        }
        add("localhost");
        add("127.0.0.1");
        add("::1");
        for (const std::string& ip : localInterfaceAddresses()) { add(ip); }
    }
    for (const std::string& n : opt.names) { add(n); }
    return a;
}

// Encodes one extension. Returns false on failure.
bool encodeExt(LPCSTR structType, const void* info, LPCSTR oid, bool critical, std::vector<Bytes>& store, std::vector<CERT_EXTENSION>& out) {
    DWORD size = 0;
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, structType, info, 0, nullptr, nullptr, &size)) { return false; }
    Bytes b(size);
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, structType, info, 0, nullptr, b.data(), &size)) { return false; }
    b.resize(size);
    store.push_back(std::move(b));
    CERT_EXTENSION e{};
    e.pszObjId = const_cast<LPSTR>(oid);
    e.fCritical = critical ? TRUE : FALSE;
    e.Value.cbData = static_cast<DWORD>(store.back().size());
    e.Value.pbData = store.back().data();
    out.push_back(e);
    return true;
}

// The key's public point as the certificate carries it: 0x04 | X | Y.
bool keyPublicPoint(NCRYPT_KEY_HANDLE key, Bytes& point) {
    DWORD sz = 0;
    if (NCryptExportKey(key, 0, BCRYPT_ECCPUBLIC_BLOB, nullptr, nullptr, 0, &sz, 0) != ERROR_SUCCESS) { return false; }
    Bytes blob(sz);
    if (NCryptExportKey(key, 0, BCRYPT_ECCPUBLIC_BLOB, nullptr, blob.data(), sz, &sz, 0) != ERROR_SUCCESS) { return false; }
    if (blob.size() < sizeof(BCRYPT_ECCKEY_BLOB)) { return false; }
    const BCRYPT_ECCKEY_BLOB* h = reinterpret_cast<const BCRYPT_ECCKEY_BLOB*>(blob.data());
    if (blob.size() < sizeof(BCRYPT_ECCKEY_BLOB) + 2 * static_cast<std::size_t>(h->cbKey)) { return false; }
    point.assign(1, 0x04);
    point.insert(point.end(), blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB), blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB) + 2 * h->cbKey);
    return true;
}

bool certMatchesKey(PCCERT_CONTEXT cert, NCRYPT_KEY_HANDLE key) {
    Bytes point;
    if (!keyPublicPoint(key, point)) { return false; }
    const CRYPT_BIT_BLOB& pk = cert->pCertInfo->SubjectPublicKeyInfo.PublicKey;
    return pk.cbData == point.size() && std::memcmp(pk.pbData, point.data(), point.size()) == 0;
}

bool readFile(const std::string& path, Bytes& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { return false; }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !out.empty() && out.size() < (1u << 20);
}

bool writeFileAtomic(const std::string& path, const Bytes& data) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { return false; }
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) { return false; }
    }
    return MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// Opens (or, with `create`, makes) the ECDSA P-256 key. `name` empty = a key that lives in memory only.
bool openKey(Credential& c, const std::wstring& name, bool create, std::string& error) {
    SECURITY_STATUS ss = NCryptOpenStorageProvider(&c.prov, MS_KEY_STORAGE_PROVIDER, 0);
    if (ss != ERROR_SUCCESS) { error = codeText("opening the key storage provider", ss); return false; }
    if (!name.empty()) {
        ss = NCryptOpenKey(c.prov, &c.key, name.c_str(), 0, 0);
        if (ss == ERROR_SUCCESS) { return true; }
        if (!create) { error = codeText("opening the stored key", ss); return false; }
    }
    ss = NCryptCreatePersistedKey(c.prov, &c.key, BCRYPT_ECDSA_P256_ALGORITHM, name.empty() ? nullptr : name.c_str(), 0, 0);
    if (ss != ERROR_SUCCESS) { error = codeText("creating the key", ss); return false; }
    DWORD policy = 0;  // NOT EXPORTABLE
    ss = NCryptSetProperty(c.key, NCRYPT_EXPORT_POLICY_PROPERTY, reinterpret_cast<PBYTE>(&policy), sizeof(policy), NCRYPT_PERSIST_FLAG);
    if (ss != ERROR_SUCCESS) { error = codeText("making the key non-exportable", ss); return false; }
    ss = NCryptFinalizeKey(c.key, 0);
    if (ss != ERROR_SUCCESS) { error = codeText("finalising the key", ss); return false; }
    return true;
}

bool makeSelfSignedCert(Credential& c, const TlsOptions& opt, const std::wstring& keyName, std::string& error) {
    const AltNames names = collectNames(opt);

    // SUBJECT
    DWORD subjSize = 0;
    if (!CertStrToNameW(X509_ASN_ENCODING, L"CN=FoxSDR web page", CERT_X500_NAME_STR, nullptr, nullptr, &subjSize, nullptr)) {
        error = codeText("encoding the certificate name", GetLastError());
        return false;
    }
    Bytes subject(subjSize);
    if (!CertStrToNameW(X509_ASN_ENCODING, L"CN=FoxSDR web page", CERT_X500_NAME_STR, nullptr, subject.data(), &subjSize, nullptr)) {
        error = codeText("encoding the certificate name", GetLastError());
        return false;
    }
    CERT_NAME_BLOB subjectBlob{subjSize, subject.data()};

    // EXTENSIONS
    std::vector<Bytes> store;
    store.reserve(8);
    std::vector<CERT_EXTENSION> exts;
    std::vector<CERT_ALT_NAME_ENTRY> entries;
    for (const std::wstring& d : names.dns) {
        CERT_ALT_NAME_ENTRY e{};
        e.dwAltNameChoice = CERT_ALT_NAME_DNS_NAME;
        e.pwszDNSName = const_cast<LPWSTR>(d.c_str());
        entries.push_back(e);
    }
    for (const Bytes& ip : names.ips) {
        CERT_ALT_NAME_ENTRY e{};
        e.dwAltNameChoice = CERT_ALT_NAME_IP_ADDRESS;
        e.IPAddress.cbData = static_cast<DWORD>(ip.size());
        e.IPAddress.pbData = const_cast<BYTE*>(ip.data());
        entries.push_back(e);
    }
    CERT_ALT_NAME_INFO alt{static_cast<DWORD>(entries.size()), entries.data()};
    CERT_BASIC_CONSTRAINTS2_INFO bc{};
    bc.fCA = FALSE;
    BYTE ku = CERT_DIGITAL_SIGNATURE_KEY_USAGE;
    CRYPT_BIT_BLOB kuBlob{1, &ku, 7};
    LPSTR serverAuth = const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH);
    CERT_ENHKEY_USAGE eku{1, &serverAuth};
    bool ok = encodeExt(X509_BASIC_CONSTRAINTS2, &bc, szOID_BASIC_CONSTRAINTS2, true, store, exts) &&
              encodeExt(X509_KEY_USAGE, &kuBlob, szOID_KEY_USAGE, true, store, exts) &&
              encodeExt(X509_ENHANCED_KEY_USAGE, &eku, szOID_ENHANCED_KEY_USAGE, false, store, exts);
    if (ok && !entries.empty()) { ok = encodeExt(X509_ALTERNATE_NAME, &alt, szOID_SUBJECT_ALT_NAME2, false, store, exts); }
    if (!ok) { error = codeText("encoding the certificate extensions", GetLastError()); return false; }
    CERT_EXTENSIONS extensions{static_cast<DWORD>(exts.size()), exts.data()};

    if (!openKey(c, keyName, true, error)) { return false; }

    CRYPT_ALGORITHM_IDENTIFIER sigAlg{};
    sigAlg.pszObjId = const_cast<LPSTR>(szOID_ECDSA_SHA256);
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);

    // The certificate is assembled and signed by hand: CertCreateSelfSignCertificate refuses an ECDSA CNG key (NTE_BAD_FLAGS), while
    // CryptSignAndEncodeCertificate signs with one and returns the ASN.1 signature a certificate carries.
    DWORD spkiSize = 0;
    if (!CryptExportPublicKeyInfoEx(c.key, CERT_NCRYPT_KEY_SPEC, X509_ASN_ENCODING, nullptr, 0, nullptr, nullptr, &spkiSize)) {
        error = codeText("reading the public key", GetLastError());
        return false;
    }
    Bytes spkiBuf(spkiSize);
    PCERT_PUBLIC_KEY_INFO spki = reinterpret_cast<PCERT_PUBLIC_KEY_INFO>(spkiBuf.data());
    if (!CryptExportPublicKeyInfoEx(c.key, CERT_NCRYPT_KEY_SPEC, X509_ASN_ENCODING, nullptr, 0, nullptr, spki, &spkiSize)) {
        error = codeText("reading the public key", GetLastError());
        return false;
    }
    std::uint8_t serial[16];
    {
        std::string rerr;
        if (!randomBytes(serial, sizeof(serial), rerr)) { error = "no random numbers for the serial number: " + rerr; return false; }
        serial[15] &= 0x7F;   // (the encoding is little-endian: the last byte is the most significant, and a serial is positive)
        serial[15] |= 0x01;   // (and non-zero)
    }
    CERT_INFO ci{};
    ci.dwVersion = CERT_V3;
    ci.SerialNumber.cbData = sizeof(serial);
    ci.SerialNumber.pbData = serial;
    ci.SignatureAlgorithm = sigAlg;
    ci.Issuer = subjectBlob;
    ci.Subject = subjectBlob;
    ci.NotBefore = addDays(nowFt, -1);
    ci.NotAfter = addDays(nowFt, 730);
    ci.SubjectPublicKeyInfo = *spki;
    ci.cExtension = static_cast<DWORD>(exts.size());
    ci.rgExtension = exts.data();
    DWORD tbsSize = 0;
    if (!CryptSignAndEncodeCertificate(c.key, CERT_NCRYPT_KEY_SPEC, X509_ASN_ENCODING, X509_CERT_TO_BE_SIGNED, &ci, &sigAlg, nullptr, nullptr, &tbsSize)) {
        error = codeText("signing the certificate", GetLastError());
        return false;
    }
    Bytes der(tbsSize);
    if (!CryptSignAndEncodeCertificate(c.key, CERT_NCRYPT_KEY_SPEC, X509_ASN_ENCODING, X509_CERT_TO_BE_SIGNED, &ci, &sigAlg, nullptr, der.data(), &tbsSize)) {
        error = codeText("signing the certificate", GetLastError());
        return false;
    }
    der.resize(tbsSize);
    c.cert = CertCreateCertificateContext(X509_ASN_ENCODING, der.data(), static_cast<DWORD>(der.size()));
    if (c.cert == nullptr) { error = codeText("making the certificate", GetLastError()); return false; }
    if (!keyName.empty()) {
        // The certificate needs to know which key is its own: the name in the user's key store.
        CRYPT_KEY_PROV_INFO kpi{};
        std::wstring nameCopy = keyName;
        std::wstring provName = MS_KEY_STORAGE_PROVIDER;
        kpi.pwszContainerName = nameCopy.data();
        kpi.pwszProvName = provName.data();
        kpi.dwProvType = 0;
        kpi.dwKeySpec = 0;
        if (!CertSetCertificateContextProperty(c.cert, CERT_KEY_PROV_INFO_PROP_ID, 0, &kpi)) {
            error = codeText("attaching the key to the certificate", GetLastError());
            return false;
        }
    }
    c.selfSigned = true;
    c.exportable = keyIsExportable(c.key);
    c.names = names.text;
    return true;
}

// A self-signed certificate that is kept: reuse the stored one while it is valid for at least 30 days and belongs to the stored key.
bool loadOrMakeSelfSigned(Credential& c, const TlsOptions& opt, std::string& error) {
    if (opt.certFile.empty()) {
        // NO CERTIFICATE FILE: a certificate for this run only. Schannel finds a certificate's key by its NAME in the key store (it cannot
        // use a key that exists only as a handle), so the key gets a random name and is deleted again when the credential is released.
        std::uint8_t r[8];
        std::string rerr;
        if (!randomBytes(r, sizeof(r), rerr)) { error = "no random numbers for the key name: " + rerr; return false; }
        c.deleteKeyOnRelease = true;
        return makeSelfSignedCert(c, opt, widen("FoxSDR-Web-TLS-run-" + hexOf(r, sizeof(r))), error);
    }
    const std::wstring keyName = widen(opt.keyName.empty() ? std::string("FoxSDR-Web-TLS") : opt.keyName);
    Bytes der;
    if (readFile(opt.certFile, der)) {
        PCCERT_CONTEXT stored = CertCreateCertificateContext(X509_ASN_ENCODING, der.data(), static_cast<DWORD>(der.size()));
        if (stored != nullptr) {
            Credential probe;
            std::string ignored;
            const bool haveKey = openKey(probe, keyName, false, ignored);
            FILETIME nowFt;
            GetSystemTimeAsFileTime(&nowFt);
            const FILETIME soon = addDays(nowFt, 30);
            const bool valid = CompareFileTime(&stored->pCertInfo->NotAfter, &soon) > 0 && CompareFileTime(&stored->pCertInfo->NotBefore, &nowFt) <= 0;
            if (haveKey && valid && certMatchesKey(stored, probe.key)) {
                // The certificate needs to know which key is its own: the name in the user's store.
                CRYPT_KEY_PROV_INFO kpi{};
                std::wstring nameCopy = keyName;
                std::wstring provName = MS_KEY_STORAGE_PROVIDER;
                kpi.pwszContainerName = nameCopy.data();
                kpi.pwszProvName = provName.data();
                kpi.dwProvType = 0;
                kpi.dwKeySpec = 0;
                if (CertSetCertificateContextProperty(stored, CERT_KEY_PROV_INFO_PROP_ID, 0, &kpi)) {
                    c.cert = stored;
                    c.prov = probe.prov;
                    c.key = probe.key;
                    probe.prov = 0;
                    probe.key = 0;
                    c.selfSigned = true;
                    c.exportable = keyIsExportable(c.key);
                    // The names are read back from the certificate itself.
                    PCERT_EXTENSION ext = CertFindExtension(szOID_SUBJECT_ALT_NAME2, stored->pCertInfo->cExtension, stored->pCertInfo->rgExtension);
                    PCERT_ALT_NAME_INFO info = nullptr;
                    DWORD infoSize = 0;
                    if (ext != nullptr && CryptDecodeObjectEx(X509_ASN_ENCODING, X509_ALTERNATE_NAME, ext->Value.pbData, ext->Value.cbData,
                                                              CRYPT_DECODE_ALLOC_FLAG, nullptr, &info, &infoSize)) {
                        for (DWORD i = 0; i < info->cAltEntry; ++i) {
                            const CERT_ALT_NAME_ENTRY& e = info->rgAltEntry[i];
                            if (e.dwAltNameChoice == CERT_ALT_NAME_DNS_NAME && e.pwszDNSName != nullptr) {
                                std::string s;
                                for (const wchar_t* w = e.pwszDNSName; *w != 0; ++w) { s.push_back(static_cast<char>(*w)); }
                                c.names.push_back(s);
                            } else if (e.dwAltNameChoice == CERT_ALT_NAME_IP_ADDRESS) {
                                char buf[INET6_ADDRSTRLEN] = {0};
                                if (e.IPAddress.cbData == 4) { inet_ntop(AF_INET, e.IPAddress.pbData, buf, sizeof(buf)); }
                                else if (e.IPAddress.cbData == 16) { inet_ntop(AF_INET6, e.IPAddress.pbData, buf, sizeof(buf)); }
                                if (buf[0] != 0) { c.names.push_back(buf); }
                            }
                        }
                        LocalFree(info);
                    }
                    return true;
                }
            }
            CertFreeCertificateContext(stored);
        }
    }
    // None kept, or not usable: a new certificate. The key is reused when it exists (a new certificate for the same key), made when not.
    if (!makeSelfSignedCert(c, opt, keyName, error)) { return false; }
    if (!writeFileAtomic(opt.certFile, Bytes(c.cert->pbCertEncoded, c.cert->pbCertEncoded + c.cert->cbCertEncoded))) {
        error = "the certificate was made but could not be written to " + opt.certFile;
        return false;
    }
    return true;
}

bool findByThumbprint(Credential& c, const std::string& hex, std::string& error) {
    Bytes hash;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        const auto nib = [](char h) { return h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10; };
        hash.push_back(static_cast<std::uint8_t>(nib(hex[i]) * 16 + nib(hex[i + 1])));
    }
    CRYPT_HASH_BLOB blob{static_cast<DWORD>(hash.size()), hash.data()};
    for (const DWORD where : {static_cast<DWORD>(CERT_SYSTEM_STORE_CURRENT_USER), static_cast<DWORD>(CERT_SYSTEM_STORE_LOCAL_MACHINE)}) {
        HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, where | CERT_STORE_OPEN_EXISTING_FLAG | CERT_STORE_READONLY_FLAG, L"MY");
        if (store == nullptr) { continue; }
        PCCERT_CONTEXT found = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_SHA1_HASH, &blob, nullptr);
        CertCloseStore(store, 0);
        if (found == nullptr) { continue; }
        DWORD n = 0;
        const bool hasKey = CertGetCertificateContextProperty(found, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &n) ||
                            CertGetCertificateContextProperty(found, CERT_KEY_CONTEXT_PROP_ID, nullptr, &n);
        if (!hasKey) {
            CertFreeCertificateContext(found);
            error = "the certificate with that thumbprint has no private key on this computer";
            return false;
        }
        if (CertVerifyTimeValidity(nullptr, found->pCertInfo) != 0) {
            CertFreeCertificateContext(found);
            error = "the certificate with that thumbprint is expired or not yet valid";
            return false;
        }
        c.cert = found;
        c.selfSigned = false;
        // Best effort: is its key exportable?
        HCRYPTPROV_OR_NCRYPT_KEY_HANDLE kh = 0;
        DWORD spec = 0;
        BOOL free = FALSE;
        c.exportable = true;
        if (CryptAcquireCertificatePrivateKey(found, CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG | CRYPT_ACQUIRE_SILENT_FLAG, nullptr, &kh, &spec, &free)) {
            c.exportable = keyIsExportable(kh);
            if (free) { NCryptFreeObject(kh); }
        }
        return true;
    }
    error = "no certificate with that thumbprint was found in the Personal store of the current user or of this computer";
    return false;
}

bool acquireCredential(Credential& c, std::string& error) {
    SCH_CREDENTIALS sc{};
    TLS_PARAMETERS tp{};
    // TLS 1.2 is the floor: SSL and TLS 1.0/1.1 are switched off whatever the machine's registry says; 1.3 is left to Windows.
    tp.grbitDisabledProtocols = SP_PROT_SSL2 | SP_PROT_SSL3 | SP_PROT_TLS1_0 | SP_PROT_TLS1_1;
    sc.dwVersion = SCH_CREDENTIALS_VERSION;
    sc.cCreds = 1;
    sc.paCred = &c.cert;
    sc.cTlsParameters = 1;
    sc.pTlsParameters = &tp;
    sc.dwFlags = SCH_USE_STRONG_CRYPTO | SCH_CRED_NO_SYSTEM_MAPPER;
    TimeStamp expiry;
    const SECURITY_STATUS ss = AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_INBOUND, nullptr, &sc, nullptr,
                                                         nullptr, &c.handle, &expiry);
    if (ss != SEC_E_OK) {
        error = codeText("Schannel refused the certificate", ss);
        return false;
    }
    c.haveHandle = true;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------

class TlsTerminator::Impl {
public:
    ~Impl() { stop(); }

    bool prepare(const TlsOptions& opt, std::string& error) {
        error.clear();
        if (!wsaUp()) { error = "Windows sockets could not be started"; return false; }
        auto fresh = std::make_unique<Credential>();
        bool ok = false;
        if (!opt.thumbprint.empty()) {
            ok = findByThumbprint(*fresh, opt.thumbprint, error);
        } else {
            ok = loadOrMakeSelfSigned(*fresh, opt, error);
        }
        if (!ok) { return false; }
        if (!acquireCredential(*fresh, error)) { return false; }
        describe(*fresh);
        std::lock_guard<std::mutex> lk(mutex_);
        stopLocked();
        cred_ = std::move(fresh);
        return true;
    }

    bool start(const std::string& bindAddress, int port, int backendPort, std::string& error) {
        error.clear();
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (!cred_) { error = "the HTTPS certificate has not been prepared"; return false; }
        }
        stop();
        if (!wsaUp()) { error = "Windows sockets could not be started"; return false; }
        addrinfo hints{};
        hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo* res = nullptr;
        const std::string portText = std::to_string(port);
        if (getaddrinfo(bindAddress.c_str(), portText.c_str(), &hints, &res) != 0 || res == nullptr) {
            error = "\"" + bindAddress + "\" is not an address that can be bound";
            return false;
        }
        SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s == INVALID_SOCKET) {
            freeaddrinfo(res);
            error = codeText("creating the HTTPS socket", WSAGetLastError());
            return false;
        }
        int on = 1;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof(on));
        if (res->ai_family == AF_INET6) {
            int off = 0;
            setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off));
        }
        if (bind(s, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0 || listen(s, 64) != 0) {
            const int e = WSAGetLastError();
            freeaddrinfo(res);
            closesocket(s);
            error = "could not bind " + bindAddress + ":" + portText + " for HTTPS - another program is probably already using that port" +
                    (e == WSAEACCES ? " (access denied)" : "");
            return false;
        }
        freeaddrinfo(res);
        sockaddr_storage ss{};
        int sl = sizeof(ss);
        getsockname(s, reinterpret_cast<sockaddr*>(&ss), &sl);
        const int bound = ss.ss_family == AF_INET ? ntohs(reinterpret_cast<sockaddr_in*>(&ss)->sin_port)
                                                   : ntohs(reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port);
        setNonBlocking(s);
        listen_ = s;
        boundPort_ = bound;
        backendPort_ = backendPort;
        stop_.store(false);
        running_.store(true);
        acceptThread_ = std::thread([this] { acceptLoop(); });
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> lk(mutex_);
        stopLocked();
    }

    bool running() const { return running_.load(); }
    int boundPort() const { return running_.load() ? boundPort_ : -1; }

    TlsTimeouts timeouts_;
    std::size_t maxTotal_ = 64;
    std::size_t maxPerAddress_ = 16;

    std::string peerOf(int port) const {
        std::lock_guard<std::mutex> lk(peerMutex_);
        const auto it = peers_.find(port);
        return it == peers_.end() ? std::string() : it->second;
    }

    std::size_t connections() const { return static_cast<std::size_t>(active_.load()); }

    TlsStats stats() const {
        TlsStats s;
        s.accepted = accepted_.load();
        s.handshakesOk = hsOk_.load();
        s.handshakesFailed = hsFail_.load();
        s.tls13 = tls13_.load();
        s.tls12 = tls12_.load();
        s.droppedHandshakeTimeout = dropHs_.load();
        s.droppedIdle = dropIdle_.load();
        s.droppedWriteStall = dropWrite_.load();
        s.refusedBusy = busy_.load();
        return s;
    }

    std::unique_ptr<Credential> cred_;

private:
    struct Worker {
        std::thread th;
        std::atomic<bool> done{false};
    };

    void stopLocked() {
        if (!running_.exchange(false)) { return; }
        stop_.store(true);
        if (listen_ != INVALID_SOCKET) {
            closesocket(listen_);
            listen_ = INVALID_SOCKET;
        }
        if (acceptThread_.joinable()) { acceptThread_.join(); }  // (the accept loop joins every connection thread before it returns)
        boundPort_ = -1;
    }

    void acceptLoop() {
        cascade::core::beats::Registration beat(cascade::core::beats::kThreadWebTlsAccept);
        std::list<std::unique_ptr<Worker>> workers;
        const SOCKET ls = listen_;
        while (!stop_.load()) {
            beat.wait(cascade::core::beats::kWaitAccept);
            const int w = waitFor(ls, false, 100);
            beat.beat("accept");
            for (auto it = workers.begin(); it != workers.end();) {
                if ((*it)->done.load()) {
                    if ((*it)->th.joinable()) { (*it)->th.join(); }
                    it = workers.erase(it);
                } else {
                    ++it;
                }
            }
            if (w < 0) { break; }
            if (w == 0) { continue; }
            sockaddr_storage peer{};
            int pl = sizeof(peer);
            SOCKET c = accept(ls, reinterpret_cast<sockaddr*>(&peer), &pl);
            if (c == INVALID_SOCKET) { continue; }
            ++accepted_;
            const std::string addr = peerText(peer);
            bool admit = false;
            {
                std::lock_guard<std::mutex> lk(peerMutex_);
                std::size_t& n = perAddress_[addr];
                if (static_cast<std::size_t>(active_.load()) < maxTotal_ && n < maxPerAddress_) {
                    ++n;
                    ++active_;
                    admit = true;
                } else if (n == 0) {
                    perAddress_.erase(addr);
                }
            }
            if (!admit) {
                ++busy_;
                closesocket(c);
                continue;
            }
            auto wk = std::make_unique<Worker>();
            Worker* raw = wk.get();
            wk->th = std::thread([this, c, addr, raw] {
                serve(c, addr);
                {
                    std::lock_guard<std::mutex> lk(peerMutex_);
                    auto it = perAddress_.find(addr);
                    if (it != perAddress_.end() && --it->second == 0) { perAddress_.erase(it); }
                    --active_;
                }
                raw->done.store(true);
            });
            workers.push_back(std::move(wk));
        }
        for (auto& w : workers) {
            if (w->th.joinable()) { w->th.join(); }
        }
    }

    // --- one connection --------------------------------------------------------------------------------------------------------------

    struct Tls {
        CtxtHandle ctx{};
        bool haveCtx = false;
        SecPkgContext_StreamSizes sizes{};
        ~Tls() {
            if (haveCtx) { DeleteSecurityContext(&ctx); }
        }
    };

    // Sends all of [p, p+n) within `ms`, in steps of 100 ms so the stop flag is seen. False: the peer is gone, stalled, or we are stopping.
    bool sendAll(SOCKET s, const std::uint8_t* p, std::size_t n, int ms, bool toClient) {
        const auto deadline = SteadyClock::now() + std::chrono::milliseconds(ms);
        while (n > 0) {
            if (stop_.load()) { return false; }
            const int r = send(s, reinterpret_cast<const char*>(p), static_cast<int>(std::min<std::size_t>(n, 1 << 20)), 0);
            if (r > 0) {
                p += r;
                n -= static_cast<std::size_t>(r);
                continue;
            }
            if (r < 0 && WSAGetLastError() != WSAEWOULDBLOCK) { return false; }
            if (SteadyClock::now() >= deadline) {
                if (toClient) { ++dropWrite_; }
                return false;
            }
            if (waitFor(s, true, 100) < 0) { return false; }
        }
        return true;
    }

    bool handshake(SOCKET s, Tls& t, Bytes& extra) {
        const auto deadline = SteadyClock::now() + std::chrono::milliseconds(timeouts_.handshakeMs);
        Bytes in;
        bool needMore = true;
        const ULONG flags = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY | ASC_REQ_EXTENDED_ERROR |
                            ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM;
        for (;;) {
            if (stop_.load()) { return false; }
            if (SteadyClock::now() >= deadline) { ++dropHs_; return false; }
            if (needMore) {
                const int w = waitFor(s, false, 100);
                if (w < 0) { ++hsFail_; return false; }
                if (w == 0) { continue; }
                std::uint8_t buf[8192];
                const int n = recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
                if (n == 0) { ++hsFail_; return false; }
                if (n < 0) {
                    if (WSAGetLastError() == WSAEWOULDBLOCK) { continue; }
                    ++hsFail_;
                    return false;
                }
                in.insert(in.end(), buf, buf + n);
                if (in.size() > 65536) { ++hsFail_; return false; }
                needMore = false;
            }
            SecBuffer inb[2];
            inb[0].cbBuffer = static_cast<ULONG>(in.size());
            inb[0].BufferType = SECBUFFER_TOKEN;
            inb[0].pvBuffer = in.data();
            inb[1].cbBuffer = 0;
            inb[1].BufferType = SECBUFFER_EMPTY;
            inb[1].pvBuffer = nullptr;
            SecBufferDesc inD{SECBUFFER_VERSION, 2, inb};
            SecBuffer outb[1];
            outb[0].cbBuffer = 0;
            outb[0].BufferType = SECBUFFER_TOKEN;
            outb[0].pvBuffer = nullptr;
            SecBufferDesc outD{SECBUFFER_VERSION, 1, outb};
            ULONG attr = 0;
            TimeStamp ts;
            CtxtHandle fresh{};
            const SECURITY_STATUS ss = AcceptSecurityContext(&cred_->handle, t.haveCtx ? &t.ctx : nullptr, &inD, flags, 0, &fresh, &outD, &attr, &ts);
            if ((ss == SEC_E_OK || ss == SEC_I_CONTINUE_NEEDED) && !t.haveCtx) {
                t.ctx = fresh;
                t.haveCtx = true;
            }
            if (outb[0].pvBuffer != nullptr) {
                if (outb[0].cbBuffer > 0) {
                    sendAll(s, static_cast<const std::uint8_t*>(outb[0].pvBuffer), outb[0].cbBuffer, timeouts_.writeMs, false);
                }
                FreeContextBuffer(outb[0].pvBuffer);
            }
            if (ss == SEC_E_INCOMPLETE_MESSAGE) {
                needMore = true;
                continue;
            }
            if (ss == SEC_I_CONTINUE_NEEDED || ss == SEC_E_OK) {
                Bytes rest;
                if (inb[1].BufferType == SECBUFFER_EXTRA && inb[1].cbBuffer > 0 && inb[1].cbBuffer <= in.size()) {
                    rest.assign(in.end() - inb[1].cbBuffer, in.end());
                }
                if (ss == SEC_E_OK) {
                    extra = std::move(rest);
                    return true;
                }
                in = std::move(rest);
                needMore = in.empty();
                continue;
            }
            ++hsFail_;   // a version Schannel was told to refuse, a garbage hello, a failed key exchange ...
            return false;
        }
    }

    // 0: need more bytes, 1: ok (plain may have grown), 2: the peer closed the TLS session, -1: error / renegotiation.
    int decrypt(Tls& t, Bytes& cipher, Bytes& plain) {
        while (!cipher.empty()) {
            SecBuffer b[4];
            b[0].cbBuffer = static_cast<ULONG>(cipher.size());
            b[0].BufferType = SECBUFFER_DATA;
            b[0].pvBuffer = cipher.data();
            for (int i = 1; i < 4; ++i) {
                b[i].cbBuffer = 0;
                b[i].BufferType = SECBUFFER_EMPTY;
                b[i].pvBuffer = nullptr;
            }
            SecBufferDesc d{SECBUFFER_VERSION, 4, b};
            const SECURITY_STATUS ss = DecryptMessage(&t.ctx, &d, 0, nullptr);
            if (ss == SEC_E_INCOMPLETE_MESSAGE) { return 0; }
            if (ss == SEC_I_CONTEXT_EXPIRED) { return 2; }
            if (ss != SEC_E_OK) { return -1; }   // (SEC_I_RENEGOTIATE included: a renegotiation request is a close)
            Bytes rest;
            for (int i = 1; i < 4; ++i) {
                if (b[i].BufferType == SECBUFFER_DATA && b[i].cbBuffer > 0) {
                    const std::uint8_t* p = static_cast<const std::uint8_t*>(b[i].pvBuffer);
                    plain.insert(plain.end(), p, p + b[i].cbBuffer);
                } else if (b[i].BufferType == SECBUFFER_EXTRA && b[i].cbBuffer > 0) {
                    const std::uint8_t* p = static_cast<const std::uint8_t*>(b[i].pvBuffer);
                    rest.assign(p, p + b[i].cbBuffer);
                }
            }
            cipher = std::move(rest);
        }
        return 1;
    }

    bool encryptAndSend(SOCKET s, Tls& t, const std::uint8_t* p, std::size_t n) {
        const std::size_t maxMsg = t.sizes.cbMaximumMessage;
        Bytes buf;
        while (n > 0) {
            const std::size_t chunk = std::min(n, maxMsg);
            buf.assign(t.sizes.cbHeader + chunk + t.sizes.cbTrailer, 0);
            std::memcpy(buf.data() + t.sizes.cbHeader, p, chunk);
            SecBuffer b[4];
            b[0].cbBuffer = t.sizes.cbHeader;
            b[0].BufferType = SECBUFFER_STREAM_HEADER;
            b[0].pvBuffer = buf.data();
            b[1].cbBuffer = static_cast<ULONG>(chunk);
            b[1].BufferType = SECBUFFER_DATA;
            b[1].pvBuffer = buf.data() + t.sizes.cbHeader;
            b[2].cbBuffer = t.sizes.cbTrailer;
            b[2].BufferType = SECBUFFER_STREAM_TRAILER;
            b[2].pvBuffer = buf.data() + t.sizes.cbHeader + chunk;
            b[3].cbBuffer = 0;
            b[3].BufferType = SECBUFFER_EMPTY;
            b[3].pvBuffer = nullptr;
            SecBufferDesc d{SECBUFFER_VERSION, 4, b};
            if (EncryptMessage(&t.ctx, 0, &d, 0) != SEC_E_OK) { return false; }
            const std::size_t total = static_cast<std::size_t>(b[0].cbBuffer) + b[1].cbBuffer + b[2].cbBuffer;
            if (!sendAll(s, buf.data(), total, timeouts_.writeMs, true)) { return false; }
            p += chunk;
            n -= chunk;
        }
        return true;
    }

    void sendCloseNotify(SOCKET s, Tls& t) {
        DWORD type = SCHANNEL_SHUTDOWN;
        SecBuffer ob{sizeof(type), SECBUFFER_TOKEN, &type};
        SecBufferDesc od{SECBUFFER_VERSION, 1, &ob};
        if (ApplyControlToken(&t.ctx, &od) != SEC_E_OK) { return; }
        SecBuffer out{0, SECBUFFER_TOKEN, nullptr};
        SecBufferDesc outD{SECBUFFER_VERSION, 1, &out};
        ULONG attr = 0;
        TimeStamp ts;
        const ULONG flags = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM;
        if (AcceptSecurityContext(&cred_->handle, &t.ctx, nullptr, flags, 0, &t.ctx, &outD, &attr, &ts) == SEC_E_OK && out.pvBuffer != nullptr) {
            sendAll(s, static_cast<const std::uint8_t*>(out.pvBuffer), out.cbBuffer, 1000, false);
        }
        if (out.pvBuffer != nullptr) { FreeContextBuffer(out.pvBuffer); }
    }

    SOCKET connectBackend(int& localPort) {
        SOCKET b = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (b == INVALID_SOCKET) { return INVALID_SOCKET; }
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<u_short>(backendPort_));
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (connect(b, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
            closesocket(b);
            return INVALID_SOCKET;
        }
        sockaddr_in me{};
        int ml = sizeof(me);
        getsockname(b, reinterpret_cast<sockaddr*>(&me), &ml);
        localPort = ntohs(me.sin_port);
        setNonBlocking(b);
        noDelay(b);
        return b;
    }

    void serve(SOCKET client, const std::string& peer) {
        cascade::core::beats::Registration beat(cascade::core::beats::kThreadWebTlsConn);
        setNonBlocking(client);
        noDelay(client);
        Tls tls;
        Bytes cipher;
        beat.beat("handshake");
        if (!handshake(client, tls, cipher)) {
            closesocket(client);
            return;
        }
        ++hsOk_;
        QueryContextAttributes(&tls.ctx, SECPKG_ATTR_STREAM_SIZES, &tls.sizes);
        SecPkgContext_ConnectionInfo info{};
        if (QueryContextAttributes(&tls.ctx, SECPKG_ATTR_CONNECTION_INFO, &info) == SEC_E_OK) {
            if (info.dwProtocol & SP_PROT_TLS1_3) { ++tls13_; } else if (info.dwProtocol & SP_PROT_TLS1_2) { ++tls12_; }
        }
        int localPort = 0;
        SOCKET be = connectBackend(localPort);
        if (be == INVALID_SOCKET) {
            closesocket(client);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(peerMutex_);
            peers_[localPort] = peer;
        }
        relay(client, be, tls, cipher, beat);
        {
            std::lock_guard<std::mutex> lk(peerMutex_);
            peers_.erase(localPort);
        }
        closesocket(be);
        closesocket(client);
    }

    void relay(SOCKET client, SOCKET be, Tls& tls, Bytes& cipher, cascade::core::beats::Registration& beat) {
        Bytes plain;
        auto last = SteadyClock::now();
        bool peerClosed = false;
        const auto pump = [&]() -> bool {   // false: end the connection
            plain.clear();
            const int r = decrypt(tls, cipher, plain);
            if (r < 0) { return false; }
            if (!plain.empty() && !sendAll(be, plain.data(), plain.size(), timeouts_.writeMs, false)) { return false; }
            if (r == 2) { peerClosed = true; return false; }
            return true;
        };
        if (!cipher.empty() && !pump()) { return; }   // (bytes that arrived with the end of the handshake)
        std::uint8_t buf[16384];
        for (;;) {
            if (stop_.load()) {
                sendCloseNotify(client, tls);
                return;
            }
            beat.beat("relay");
            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(client, &rs);
            FD_SET(be, &rs);
            timeval tv{0, 100000};
            const int sel = select(0, &rs, nullptr, nullptr, &tv);
            if (sel < 0) { return; }
            if (sel == 0) {
                if (SteadyClock::now() - last > std::chrono::milliseconds(timeouts_.idleMs)) {
                    ++dropIdle_;
                    return;
                }
                continue;
            }
            if (FD_ISSET(client, &rs)) {
                const int n = recv(client, reinterpret_cast<char*>(buf), sizeof(buf), 0);
                if (n == 0) { return; }
                if (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK) { return; }
                if (n > 0) {
                    last = SteadyClock::now();
                    cipher.insert(cipher.end(), buf, buf + n);
                    if (cipher.size() > 4 * 16384) { return; }   // more than a few records with no end: not a client
                    if (!pump()) {
                        if (peerClosed) { sendCloseNotify(client, tls); }
                        return;
                    }
                }
            }
            if (FD_ISSET(be, &rs)) {
                const int n = recv(be, reinterpret_cast<char*>(buf), sizeof(buf), 0);
                if (n == 0) {
                    sendCloseNotify(client, tls);
                    return;
                }
                if (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK) { return; }
                if (n > 0) {
                    last = SteadyClock::now();
                    if (!encryptAndSend(client, tls, buf, static_cast<std::size_t>(n))) { return; }
                }
            }
        }
    }

    mutable std::mutex mutex_;           // start / stop / prepare
    SOCKET listen_ = INVALID_SOCKET;
    int boundPort_ = -1;
    int backendPort_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::thread acceptThread_;

    mutable std::mutex peerMutex_;
    std::map<int, std::string> peers_;
    std::map<std::string, std::size_t> perAddress_;
    std::atomic<int> active_{0};

    std::atomic<std::uint64_t> accepted_{0}, hsOk_{0}, hsFail_{0}, tls13_{0}, tls12_{0}, dropHs_{0}, dropIdle_{0}, dropWrite_{0}, busy_{0};
};

TlsTerminator::TlsTerminator() : impl_(std::make_unique<Impl>()) {}
TlsTerminator::~TlsTerminator() = default;
bool TlsTerminator::available() { return true; }
bool TlsTerminator::prepare(const TlsOptions& opt, std::string& error) { return impl_->prepare(opt, error); }
bool TlsTerminator::start(const std::string& a, int p, int b, std::string& e) { return impl_->start(a, p, b, e); }
void TlsTerminator::stop() { impl_->stop(); }
bool TlsTerminator::running() const { return impl_->running(); }
int TlsTerminator::boundPort() const { return impl_->boundPort(); }
void TlsTerminator::setTimeouts(const TlsTimeouts& t) { impl_->timeouts_ = t; }
void TlsTerminator::setMaxConnections(std::size_t total, std::size_t perAddress) {
    impl_->maxTotal_ = total;
    impl_->maxPerAddress_ = perAddress;
}
std::string TlsTerminator::fingerprintSha256() const { return impl_->cred_ ? impl_->cred_->sha256Hex : std::string(); }
std::string TlsTerminator::thumbprintSha1() const { return impl_->cred_ ? impl_->cred_->sha1Hex : std::string(); }
std::vector<std::string> TlsTerminator::certificateNames() const { return impl_->cred_ ? impl_->cred_->names : std::vector<std::string>(); }
std::int64_t TlsTerminator::certificateNotAfter() const { return impl_->cred_ ? impl_->cred_->notAfter : 0; }
bool TlsTerminator::selfSigned() const { return impl_->cred_ && impl_->cred_->selfSigned; }
bool TlsTerminator::keyExportable() const { return !impl_->cred_ || impl_->cred_->exportable; }
std::vector<std::uint8_t> TlsTerminator::certificateDer() const { return impl_->cred_ ? impl_->cred_->der : std::vector<std::uint8_t>(); }
std::string TlsTerminator::peerOf(int p) const { return impl_->peerOf(p); }
std::size_t TlsTerminator::connections() const { return impl_->connections(); }
TlsStats TlsTerminator::stats() const { return impl_->stats(); }

#else  // not Windows: nothing here has been built or run (UNPROVEN on Linux)

class TlsTerminator::Impl {};
TlsTerminator::TlsTerminator() = default;
TlsTerminator::~TlsTerminator() = default;
bool TlsTerminator::available() { return false; }
bool TlsTerminator::prepare(const TlsOptions&, std::string& e) {
    e = "HTTPS for the web page uses Windows' Schannel and is not built on this platform (UNPROVEN on Linux)";
    return false;
}
bool TlsTerminator::start(const std::string&, int, int, std::string& e) {
    e = "HTTPS for the web page uses Windows' Schannel and is not built on this platform (UNPROVEN on Linux)";
    return false;
}
void TlsTerminator::stop() {}
bool TlsTerminator::running() const { return false; }
int TlsTerminator::boundPort() const { return -1; }
void TlsTerminator::setTimeouts(const TlsTimeouts&) {}
void TlsTerminator::setMaxConnections(std::size_t, std::size_t) {}
std::string TlsTerminator::fingerprintSha256() const { return std::string(); }
std::string TlsTerminator::thumbprintSha1() const { return std::string(); }
std::vector<std::string> TlsTerminator::certificateNames() const { return {}; }
std::int64_t TlsTerminator::certificateNotAfter() const { return 0; }
bool TlsTerminator::selfSigned() const { return false; }
bool TlsTerminator::keyExportable() const { return true; }
std::vector<std::uint8_t> TlsTerminator::certificateDer() const { return {}; }
std::string TlsTerminator::peerOf(int) const { return std::string(); }
std::size_t TlsTerminator::connections() const { return 0; }
TlsStats TlsTerminator::stats() const { return TlsStats{}; }

#endif

}  // namespace cascade::net
