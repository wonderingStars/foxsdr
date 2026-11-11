// The KiwiSDR listen-only client. See kiwisdr_client.hpp for what it is, the
// politeness rules and the thread model; kiwisdr_protocol.hpp for the wire
// format and its provenance.
//
// HOW CLOSE STAYS BOUNDED. httplib's WebSocket::read() cannot be woken by a
// flag, and its own close() reads from the same stream the reader is reading,
// so it cannot be called from another thread. Instead the socket handle is
// captured when httplib creates it (set_socket_options runs before connect)
// and close() calls shutdown() on it: the blocked read fails at once, the
// reader falls out of its loop and destroys the client itself. The handle is
// only ever used under Link::m while Link::sockValid is true, and the reader
// clears sockValid BEFORE it destroys the client (which closes the socket), so
// shutdown() can never reach a recycled descriptor.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Must match every other TU that includes httplib.h (see web_server.cpp and
// tests/test_httplib_vendor.cpp): a layout mismatch between TUs is a crash.
#if !defined(_WIN32)
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#endif
#include <httplib.h>

#include "source/kiwisdr_client.hpp"

#include <algorithm>
#include <condition_variable>
#include <ctime>

#include "dsp/spsc_ring.hpp"
#include "source/kiwisdr_protocol.hpp"

namespace cascade::source {

namespace {

std::atomic<std::uint64_t> g_threadsAbandoned{0};

using State = KiwiSdrClient::State;

bool isTerminal(State s) {
    return s != State::Idle && s != State::Connecting && s != State::Playing;
}

bool hostIsSafe(const std::string& h) {
    if (h.empty() || h.size() > 253) { return false; }
    for (char c : h) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '-' || c == '_' || c == ':';
        if (!ok) { return false; }
    }
    return true;
}

}  // namespace

struct KiwiSdrClient::Link {
    explicit Link(const Options& o) : opt(o), ring(kRingSamples) {}

    Options opt;

    // --- state, status and the socket (m) --------------------------------------
    mutable std::mutex m;
    std::condition_variable cv;  // wakes the keep-alive; signals thread exit
    std::atomic<State> state{State::Connecting};
    Status status;
    bool stop = false;      // the threads must end
    bool aborted = false;   // close() was called
    bool sockValid = false;
    socket_t sock = INVALID_SOCKET;
    bool readerDone = true;  // true until the thread exists
    bool keeperDone = true;
    std::string mode;
    double frequencyHz = 0.0;

    // --- sending (sendMutex) -------------------------------------------------------
    std::mutex sendMutex;
    std::unique_ptr<httplib::ws::WebSocketClient> ws;
    bool sendOk = false;

    // --- samples -----------------------------------------------------------------------
    dsp::SpscRing<std::int16_t> ring;
    std::mutex waitM;
    std::condition_variable waitCv;

    // --- numbers ---------------------------------------------------------------------------
    std::atomic<bool> haveSmeter{false};
    std::atomic<std::uint16_t> smeter{0};
    std::atomic<std::uint64_t> gaps{0};
    std::atomic<std::uint64_t> missed{0};
    std::atomic<std::uint64_t> frames{0};
    std::atomic<std::uint64_t> delivered{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> ignored{0};
    std::atomic<std::uint64_t> keepAlives{0};

    // Shuts the socket (if it is still ours) so a blocked read or write fails.
    void shutdownSocketLocked() {
        if (sockValid) {
#if defined(_WIN32)
            ::shutdown(sock, SD_BOTH);
#else
            ::shutdown(sock, SHUT_RDWR);
#endif
        }
    }

    // Records a final state unless one is already recorded (the first wins),
    // stops the threads and shuts the socket.
    void finish(State s, int code, const std::string& detail, const std::string& redirect) {
        {
            std::lock_guard<std::mutex> lk(m);
            const State now = state.load(std::memory_order_relaxed);
            if (!isTerminal(now)) {
                status.state = s;
                status.code = code;
                status.detail = detail;
                status.redirect = redirect;
                state.store(s, std::memory_order_release);
            }
            stop = true;
            shutdownSocketLocked();
        }
        cv.notify_all();
        { std::lock_guard<std::mutex> lk(waitM); }
        waitCv.notify_all();
    }

    bool sendText(const std::string& text) {
        if (text.empty()) { return false; }
        std::lock_guard<std::mutex> lk(sendMutex);
        if (!sendOk || !ws) { return false; }
        return ws->send(text);
    }

    bool stopping() {
        std::lock_guard<std::mutex> lk(m);
        return stop;
    }

    void threadExited(bool reader) {
        {
            std::lock_guard<std::mutex> lk(m);
            (reader ? readerDone : keeperDone) = true;
        }
        cv.notify_all();
    }
};

// --- the reader ---------------------------------------------------------------------------

namespace {

void readerBody(std::shared_ptr<KiwiSdrClient::Link> link) {
    kiwi::AdpcmState adpcm;
    kiwi::ParsedFrame frame;
    bool haveSeq = false;
    std::uint32_t nextSeq = 0;
    auto lastRx = std::chrono::steady_clock::now();
    std::string msg;

    for (;;) {
        msg.clear();
        const httplib::ws::ReadResult rr = link->ws->read(msg);
        if (link->stopping()) { break; }
        if (rr == httplib::ws::Fail) {
            const auto quiet = std::chrono::steady_clock::now() - lastRx;
            const bool silent = quiet >= link->opt.stallWait - std::chrono::milliseconds(50);
            link->finish(State::Dropped, 0,
                         silent ? "the server went silent" : "the server closed the connection", "");
            break;
        }
        lastRx = std::chrono::steady_clock::now();

        const auto* bytes = reinterpret_cast<const std::uint8_t*>(msg.data());
        const kiwi::ParseError pe = kiwi::parseFrame(bytes, msg.size(), adpcm, frame);
        if (pe == kiwi::ParseError::UnknownTag) {
            link->ignored.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (pe != kiwi::ParseError::None) {
            link->finish(State::Malformed, 0,
                         std::string("the server sent a frame this client refuses: ") +
                             kiwi::parseErrorText(pe),
                         "");
            break;
        }

        if (frame.type == kiwi::FrameType::Snd) {
            const kiwi::SndFrame& s = frame.snd;
            if (haveSeq) {
                const std::uint32_t delta = s.sequence - nextSeq;
                if (delta != 0) {
                    link->gaps.fetch_add(1, std::memory_order_relaxed);
                    if (delta < 0x80000000u) {
                        link->missed.fetch_add(delta, std::memory_order_relaxed);
                    }
                }
            }
            haveSeq = true;
            nextSeq = s.sequence + 1u;
            link->frames.fetch_add(1, std::memory_order_relaxed);
            link->smeter.store(s.smeterRaw, std::memory_order_relaxed);
            link->haveSmeter.store(true, std::memory_order_release);

            const std::size_t n = s.samples.size();
            const std::size_t took = n ? link->ring.write(s.samples.data(), n) : 0;
            link->delivered.fetch_add(took, std::memory_order_relaxed);
            if (took < n) { link->dropped.fetch_add(n - took, std::memory_order_relaxed); }

            {
                std::lock_guard<std::mutex> lk(link->m);
                if (link->state.load(std::memory_order_relaxed) == State::Connecting) {
                    link->status.state = State::Playing;
                    link->state.store(State::Playing, std::memory_order_release);
                }
            }
            { std::lock_guard<std::mutex> lk(link->waitM); }
            link->waitCv.notify_all();
        } else if (frame.type == kiwi::FrameType::Msg) {
            const kiwi::ServerMessage& sm = frame.message;
            switch (sm.kind) {
                case kiwi::MessageKind::Info:
                    link->ignored.fetch_add(1, std::memory_order_relaxed);
                    break;
                case kiwi::MessageKind::TooBusy:
                    link->finish(State::RefusedBusy, sm.code,
                                 "the receiver has no free slot (too_busy)", "");
                    break;
                case kiwi::MessageKind::BadPassword:
                    link->finish(State::RefusedPassword, sm.code,
                                 "the receiver refused the password (badp)", "");
                    break;
                case kiwi::MessageKind::Down:
                    link->finish(State::ServerDown, sm.code, "the receiver reports it is down", "");
                    break;
                case kiwi::MessageKind::Redirect:
                    link->finish(State::Redirected, 0, "the receiver redirects elsewhere",
                                 sm.redirectTarget);
                    break;
            }
            if (sm.kind != kiwi::MessageKind::Info) { break; }
        } else {
            link->ignored.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Nothing may send once the client is going. Then drop the socket handle
    // BEFORE the client (and with it the descriptor) is destroyed.
    {
        std::lock_guard<std::mutex> lk(link->m);
        link->shutdownSocketLocked();
    }
    {
        std::lock_guard<std::mutex> lk(link->sendMutex);
        link->sendOk = false;
    }
    {
        std::lock_guard<std::mutex> lk(link->m);
        link->sockValid = false;
    }
    {
        std::lock_guard<std::mutex> lk(link->sendMutex);
        link->ws.reset();
    }
    link->finish(State::Dropped, 0, "the connection ended", "");  // no-op if already final
    link->threadExited(true);
}

void keeperBody(std::shared_ptr<KiwiSdrClient::Link> link) {
    const std::string ka = kiwi::buildKeepAlive();
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(link->m);
            if (link->cv.wait_for(lk, link->opt.keepAliveInterval, [&] { return link->stop; })) {
                break;
            }
        }
        if (link->sendText(ka)) { link->keepAlives.fetch_add(1, std::memory_order_relaxed); }
    }
    link->threadExited(false);
}

}  // namespace

// --- the object ---------------------------------------------------------------------------

KiwiSdrClient::KiwiSdrClient() = default;

KiwiSdrClient::~KiwiSdrClient() { close(); }

std::shared_ptr<KiwiSdrClient::Link> KiwiSdrClient::linkSnapshot() const {
    std::lock_guard<std::mutex> lk(linkMutex_);
    return link_;
}

int KiwiSdrClient::audioRateHz() { return kiwi::assumed::kAudioRateHz; }

std::uint64_t KiwiSdrClient::threadsAbandoned() {
    return g_threadsAbandoned.load(std::memory_order_relaxed);
}

bool KiwiSdrClient::open(const Options& options) {
    auto link = std::make_shared<Link>(options);
    {
        std::lock_guard<std::mutex> lk(threadMutex_);
        if (opened_) { return false; }
        opened_ = true;
    }
    {
        std::lock_guard<std::mutex> lk(linkMutex_);
        link_ = link;
    }
    link->status.state = State::Connecting;
    link->status.detail = "connecting";

    if (!hostIsSafe(options.host) || options.port == 0) {
        link->finish(State::ConnectFailed, 0, "the address is not a usable host and port", "");
        return false;
    }
    const std::uint64_t ts = options.timestamp != 0
                                 ? options.timestamp
                                 : static_cast<std::uint64_t>(std::time(nullptr));
    const std::string hostPart =
        options.host.find(':') != std::string::npos ? "[" + options.host + "]" : options.host;
    const std::string url =
        "ws://" + hostPart + ":" + std::to_string(options.port) + kiwi::buildSoundPath(ts);

    std::unique_ptr<httplib::ws::WebSocketClient> client;
    try {
        client = std::make_unique<httplib::ws::WebSocketClient>(url);
    } catch (...) {
        client.reset();
    }
    if (!client || !client->is_valid()) {
        link->finish(State::ConnectFailed, 0, "the address is not a usable WebSocket address", "");
        return false;
    }

    const auto secs = [](std::chrono::milliseconds ms) {
        return static_cast<time_t>(ms.count() / 1000);
    };
    const auto usecs = [](std::chrono::milliseconds ms) {
        return static_cast<time_t>((ms.count() % 1000) * 1000);
    };
    client->set_connection_timeout(secs(options.connectWait), usecs(options.connectWait));
    client->set_read_timeout(secs(options.stallWait), usecs(options.stallWait));
    client->set_write_timeout(5, 0);
    client->set_websocket_ping_interval(0);  // the receiver has its own keep-alive; no pings
    Link* raw = link.get();
    client->set_socket_options([raw](socket_t s) {
        std::lock_guard<std::mutex> lk(raw->m);
        raw->sock = s;
        raw->sockValid = true;
        if (raw->aborted) {
#if defined(_WIN32)
            ::shutdown(s, SD_BOTH);
#else
            ::shutdown(s, SHUT_RDWR);
#endif
        }
    });

    const httplib::ws::Result res = client->connect();
    if (!res) {
        {
            std::lock_guard<std::mutex> lk(link->m);
            link->sockValid = false;
        }
        bool wasAborted;
        {
            std::lock_guard<std::mutex> lk(link->m);
            wasAborted = link->aborted;
        }
        if (wasAborted) {
            link->finish(State::Closed, 0, "closed by the caller", "");
        } else {
            std::string why = "could not connect";
            if (res.status() > 0) {
                why = "the receiver answered the WebSocket upgrade with HTTP " +
                      std::to_string(res.status());
            } else {
                why += ": " + httplib::to_string(res.error());
            }
            link->finish(State::ConnectFailed, res.status() > 0 ? res.status() : 0, why, "");
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(link->sendMutex);
        link->ws = std::move(client);
        link->sendOk = true;
    }

    // The opening commands, in this order (ASSUMPTION: the outline gives the
    // commands, not their order; none waits for an answer).
    bool sent = link->sendText(kiwi::buildAuth(options.password));
    sent = sent && link->sendText(kiwi::buildIdentUser(options.identUser));
    sent = sent && link->sendText(kiwi::buildAudioRate(kiwi::assumed::kArOkIn, kiwi::assumed::kArOkOut));
    sent = sent && link->sendText(kiwi::buildCompression(options.adpcm));
    if (!sent) {
        // A refused argument (user name, password) or a dead socket: nothing
        // more is sent and nothing runs.
        {
            std::lock_guard<std::mutex> lk(link->sendMutex);
            link->sendOk = false;
        }
        {
            std::lock_guard<std::mutex> lk(link->m);
            link->shutdownSocketLocked();
            link->sockValid = false;
        }
        {
            std::lock_guard<std::mutex> lk(link->sendMutex);
            link->ws.reset();
        }
        link->finish(State::ConnectFailed, 0,
                     "the opening commands could not be sent (check the user name and password)", "");
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(threadMutex_);
        bool abortedNow;
        {
            std::lock_guard<std::mutex> lk2(link->m);
            abortedNow = link->aborted;
            if (!abortedNow) {
                link->readerDone = false;
                link->keeperDone = false;
            }
        }
        if (abortedNow) {
            // close() won the race: the socket is already shut; tidy and report.
            {
                std::lock_guard<std::mutex> lk2(link->sendMutex);
                link->sendOk = false;
            }
            {
                std::lock_guard<std::mutex> lk2(link->m);
                link->sockValid = false;
            }
            {
                std::lock_guard<std::mutex> lk2(link->sendMutex);
                link->ws.reset();
            }
            link->finish(State::Closed, 0, "closed by the caller", "");
            return false;
        }
        reader_ = std::thread(readerBody, link);
        keeper_ = std::thread(keeperBody, link);
    }
    return true;
}

bool KiwiSdrClient::tune(const std::string& mode, int lowCutHz, int highCutHz, double frequencyHz) {
    auto link = linkSnapshot();
    if (!link) { return false; }
    if (isTerminal(link->state.load(std::memory_order_acquire))) { return false; }
    const std::string cmd = kiwi::buildTune(mode, lowCutHz, highCutHz, frequencyHz);
    if (cmd.empty()) { return false; }
    if (!link->sendText(cmd)) { return false; }
    std::lock_guard<std::mutex> lk(link->m);
    link->mode = mode;
    link->frequencyHz = frequencyHz;
    return true;
}

void KiwiSdrClient::close() {
    auto link = linkSnapshot();
    if (!link) { return; }

    {
        std::lock_guard<std::mutex> lk(link->m);
        link->aborted = true;
    }
    link->finish(State::Closed, 0, "closed by the caller", "");

    std::lock_guard<std::mutex> tl(threadMutex_);
    if (!reader_.joinable() && !keeper_.joinable()) { return; }

    bool bothDone;
    {
        std::unique_lock<std::mutex> lk(link->m);
        bothDone = link->cv.wait_for(lk, kJoinWait, [&] { return link->readerDone && link->keeperDone; });
    }
    auto settle = [&](std::thread& t, bool reader) {
        if (!t.joinable()) { return; }
        bool done;
        {
            std::lock_guard<std::mutex> lk(link->m);
            done = reader ? link->readerDone : link->keeperDone;
        }
        if (done) {
            t.join();
        } else {
            // Abandoned: the thread keeps the link (and with it the client and
            // the buffers) alive until it returns.
            g_threadsAbandoned.fetch_add(1, std::memory_order_relaxed);
            t.detach();
        }
    };
    (void)bothDone;
    settle(reader_, true);
    settle(keeper_, false);
}

std::size_t KiwiSdrClient::read(std::int16_t* dst, std::size_t n, std::chrono::milliseconds wait) {
    auto link = linkSnapshot();
    if (!link || n == 0 || dst == nullptr) { return 0; }
    std::size_t got = link->ring.read(dst, n);
    if (got == 0 && wait.count() > 0) {
        std::unique_lock<std::mutex> lk(link->waitM);
        link->waitCv.wait_for(lk, wait, [&] {
            return link->ring.size() > 0 || isTerminal(link->state.load(std::memory_order_acquire));
        });
        lk.unlock();
        got = link->ring.read(dst, n);
    }
    return got;
}

KiwiSdrClient::Status KiwiSdrClient::status() const {
    auto link = linkSnapshot();
    if (!link) { return Status{}; }
    std::lock_guard<std::mutex> lk(link->m);
    Status s = link->status;
    s.state = link->state.load(std::memory_order_relaxed);
    return s;
}

KiwiSdrClient::State KiwiSdrClient::state() const {
    auto link = linkSnapshot();
    return link ? link->state.load(std::memory_order_acquire) : State::Idle;
}

bool KiwiSdrClient::haveSmeter() const {
    auto l = linkSnapshot();
    return l && l->haveSmeter.load(std::memory_order_acquire);
}
std::uint16_t KiwiSdrClient::smeterRaw() const {
    auto l = linkSnapshot();
    return l ? l->smeter.load(std::memory_order_relaxed) : 0;
}
double KiwiSdrClient::rssiDbm() const { return kiwi::rssiDbm(smeterRaw()); }

#define KIWI_COUNTER(name, member)                       \
    std::uint64_t KiwiSdrClient::name() const {          \
        auto l = linkSnapshot();                         \
        return l ? l->member.load(std::memory_order_relaxed) : 0; \
    }
KIWI_COUNTER(sequenceGaps, gaps)
KIWI_COUNTER(missedFrames, missed)
KIWI_COUNTER(audioFrames, frames)
KIWI_COUNTER(samplesDelivered, delivered)
KIWI_COUNTER(droppedSamples, dropped)
KIWI_COUNTER(ignoredFrames, ignored)
KIWI_COUNTER(keepAlivesSent, keepAlives)
#undef KIWI_COUNTER

std::string KiwiSdrClient::mode() const {
    auto l = linkSnapshot();
    if (!l) { return std::string(); }
    std::lock_guard<std::mutex> lk(l->m);
    return l->mode;
}

double KiwiSdrClient::frequencyHz() const {
    auto l = linkSnapshot();
    if (!l) { return 0.0; }
    std::lock_guard<std::mutex> lk(l->m);
    return l->frequencyHz;
}

}  // namespace cascade::source
