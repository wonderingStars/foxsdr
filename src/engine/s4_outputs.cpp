// s4_outputs.cpp - see s4_outputs.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/s4_outputs.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/decode_json.hpp"
#include "core/server_ports.hpp"

namespace cascade::engine::s4 {

namespace {

namespace al = cascade::core::alerts;
namespace dj = cascade::core::decodejson;

constexpr std::size_t kMaxJobs = 200;       // jobs waiting for the worker; an overflow drops the oldest
constexpr std::size_t kMaxHookQueue = 100;  // webhook posts waiting
constexpr std::size_t kRecent = 20;

std::string trimmed(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) { ++a; }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) { --b; }
    return s.substr(a, b - a);
}

bool hasControlByte(const std::string& s) {
    for (const unsigned char c : s) {
        if (c < 0x20u || c == 0x7Fu) { return true; }
    }
    return false;
}

std::string lowerOf(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

std::string cutUtf8(const std::string& s, std::size_t max) {
    if (s.size() <= max) { return s; }
    std::size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0u) == 0x80u) { --n; }
    return s.substr(0, n);
}

// The JSON of one alert, for a webhook or an MQTT message: the rule, the time, what matched and the text (never a user name or an address).
std::string alertJson(const al::Alert& a) {
    dj::DecodeEvent e;
    e.plugin = a.event.decoder;
    e.unixMs = a.timeMs;
    e.frequencyHz = a.event.freqHz > 0.0 ? a.event.freqHz : std::nan("");
    e.text = a.body;
    e.fields.push_back({"alert", a.rule, 0.0, false});
    e.fields.push_back({"matched", a.matchedBy, 0.0, false});
    e.fields.push_back({"severity", al::severityName(a.severity), 0.0, false});
    return dj::toJson(e);
}

}  // namespace

KeywordList parseKeywords(const std::string& text) {
    KeywordList out;
    std::size_t from = 0;
    while (from <= text.size()) {
        std::size_t to = text.find('\n', from);
        if (to == std::string::npos) { to = text.size(); }
        const std::string line = trimmed(text.substr(from, to - from));
        from = to + 1;
        if (line.empty()) { continue; }
        if (hasControlByte(line)) { return KeywordList{{}, "a keyword has a control character"}; }
        if (line.size() > kMaxKeywordBytes) { return KeywordList{{}, "a keyword is longer than 120 bytes"}; }
        const std::string low = lowerOf(line);
        bool dup = false;
        for (const std::string& k : out.keywords) { if (lowerOf(k) == low) { dup = true; break; } }
        if (dup) { continue; }
        if (out.keywords.size() >= kMaxKeywords) { return KeywordList{{}, "more than 64 keywords"}; }
        out.keywords.push_back(line);
    }
    return out;
}

std::string checkWebhookUrl(const std::string& url) {
    if (url.empty()) { return std::string(); }
    const bool http = url.rfind("http://", 0) == 0, https = url.rfind("https://", 0) == 0;
    if (!http && !https) { return "the address must start with http:// or https://"; }
    if (url.size() > 2048) { return "the address is longer than 2048 bytes"; }
    if (hasControlByte(url) || url.find(' ') != std::string::npos) { return "the address has a space or a control character"; }
    return std::string();
}

std::string checkMqttHost(const std::string& host) {
    if (host.empty()) { return "a host is needed"; }
    if (host.size() > 253) { return "the host is longer than 253 bytes"; }
    if (hasControlByte(host) || host.find_first_of(" /\\@") != std::string::npos) { return "the host has a space, a slash or a control character"; }
    return std::string();
}

std::string checkMqttTopic(const std::string& topic) {
    if (topic.empty() || topic.size() > 200) { return "the topic is 1 to 200 bytes"; }
    if (!cascade::core::mqtt::validTopicName(topic)) { return "the topic is not valid text, or has a # or + in it"; }
    return std::string();
}

std::string checkInfluxText(const std::string& text, bool mayBeEmpty) {
    if (text.empty()) { return mayBeEmpty ? std::string() : std::string("this is needed"); }
    if (text.size() > 64) { return "at most 64 bytes"; }
    if (hasControlByte(text) || text.find(' ') != std::string::npos) { return "no spaces or control characters"; }
    return std::string();
}

// "This computer only" is decided from the ADDRESS, never from how a name begins (L4, security review 1): a host that merely STARTS with "127." ("127.evil.example") used to
// be labelled "this computer only" while a password went out in clear text to another machine. Only the name `localhost`, `::1` and a parsed dotted-quad inside 127.0.0.0/8
// count. (Any other name is "another computer", even one that would resolve to this machine: the label is made without a lookup, which has no time bound.)
bool isLoopbackHost(const std::string& host) {
    const std::string h = lowerOf(host);
    return h == "[::1]" || cascade::net::isLoopbackAddress(h);
}

std::string mqttScope(const std::string& host, bool hasPassword) {
    if (isLoopbackHost(host)) { return "this computer only"; }
    return hasPassword ? "another computer: the user name and password are sent without encryption"
                       : "another computer: what is sent is not encrypted";
}

std::string hookScope(const std::string& url) {
    if (url.empty()) { return std::string(); }
    if (url.rfind("https://", 0) == 0) { return "sent to another address over an encrypted link"; }
    std::string host = url.substr(7);
    host = host.substr(0, host.find_first_of("/:?#"));
    return isLoopbackHost(host) ? "this computer only" : "refused: a plain http address is only allowed on this computer";
}

Hub::Hub() : cancel_(std::make_shared<cascade::core::UploadCancel>()) {}

Hub::~Hub() { stop(); }

void Hub::setNotifier(std::shared_ptr<al::NotificationSink> sink) {
    std::lock_guard<std::mutex> lk(m_);
    sink_ = std::move(sink);
}

void Hub::setPoster(Poster p) {
    std::lock_guard<std::mutex> lk(m_);
    poster_ = std::move(p);
}

bool Hub::active() const {
    std::lock_guard<std::mutex> lk(m_);
    return cfg_.mqtt || cfg_.hook || cfg_.alerts;
}

void Hub::setInfluxSender(InfluxSender f) {
    std::lock_guard<std::mutex> lk(m_);
    influxSender_ = std::move(f);
}

bool Hub::levelsDue(std::int64_t nowMs) const {
    std::lock_guard<std::mutex> lk(m_);
    return cfg_.influx && st_.influxProblem.rfind("settings:", 0) != 0 && nowMs - lastInfluxMs_ >= static_cast<std::int64_t>(std::clamp(cfg_.influxEveryS, 1, 3600)) * 1000;
}

void Hub::offerLevel(const cascade::core::powerlog::Record& r, std::int64_t nowMs) {
    Config c;
    InfluxSender sender;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!cfg_.influx) { return; }
        lastInfluxMs_ = nowMs;
        if (influxBusy_) { ++st_.hookDropped; return; }   // the last record is still being sent: this one is dropped and counted with the others
        influxBusy_ = true;
        c = cfg_;
        sender = influxSender_;
    }
    post([this, c, sender, r] {
        cascade::core::powerlog::InfluxTarget t;
        t.enabled = true;
        t.http = c.influxHttp;
        t.host = c.influxHost;
        t.port = c.influxPort;
        t.org = c.influxOrg;
        t.bucket = c.influxBucket;
        t.token = c.influxToken;
        const std::vector<std::string> lines{cascade::core::powerlog::influxLine(r)};
        cascade::core::powerlog::SendResult res;
        if (sender) { res = sender(t, lines); }
        else { res = c.influxHttp ? cascade::core::powerlog::sendHttp(t, lines) : cascade::core::powerlog::sendUdp(t, lines); }
        std::lock_guard<std::mutex> lk(m_);
        influxBusy_ = false;
        if (res.ok) { ++st_.influxOk; st_.influxProblem.clear(); }
        else { ++st_.influxFailed; st_.influxProblem = res.error.empty() ? "no answer" : res.error; }   // the sender's sentence never holds the token
    });
}

Stats Hub::stats() const {
    std::lock_guard<std::mutex> lk(m_);
    Stats s = st_;
    if (mqtt_) {
        const cascade::core::mqtt::Stats ms = mqtt_->stats();
        s.mqttOffered = ms.offered;
        s.mqttRejected = ms.rejected;
        s.mqttState = mqtt_->connected() ? "connected" : "connecting";
        const std::string e = mqtt_->lastError();
        if (!e.empty()) { s.mqttProblem = e; }
    } else {
        s.mqttState = cfg_.mqtt ? "connecting" : "off";
    }
    return s;
}

std::vector<std::string> Hub::recentAlerts() const {
    std::lock_guard<std::mutex> lk(m_);
    return recent_;
}

void Hub::applyMqtt(const Config& c) {
    // (m_ held)
    if (mqtt_) {
        mqtt_->stop();
        mqtt_.reset();
    }
    st_.mqttProblem.clear();
    if (!c.mqtt) { return; }
    std::string why = checkMqttHost(c.mqttHost);
    if (why.empty()) { why = checkMqttTopic(c.mqttTopic); }
    if (!why.empty()) { st_.mqttProblem = why; return; }
    cascade::core::mqtt::Options o;
    o.enabled = true;
    o.host = c.mqttHost;
    o.port = c.mqttPort;
    o.username = c.mqttUser;
    o.password = c.mqttUser.empty() ? std::string() : c.mqttPassword;
    auto pub = std::make_unique<cascade::core::mqtt::Publisher>(o);
    if (!pub->start()) {
        st_.mqttProblem = pub->lastError().empty() ? "the connection could not be started" : pub->lastError();
        return;
    }
    mqtt_ = std::move(pub);
}

void Hub::applyAlerts(const Config& c) {
    // (m_ held)
    st_.alertProblem.clear();
    if (!c.alerts) {
        std::string ignore;
        (void)alertEngine_.setRules({}, &ignore);
        return;
    }
    const KeywordList k = parseKeywords(c.keywords);
    if (!k.problem.empty()) { st_.alertProblem = k.problem; (void)alertEngine_.setRules({}, nullptr); return; }
    al::Rule r;
    r.name = "keywords";
    r.severity = al::Severity::Notice;
    r.keywords = k.keywords;
    r.perKeyIntervalMs = static_cast<std::int64_t>(std::clamp(c.alertGapS, 0, 3600)) * 1000;
    r.maxPerHour = 120;
    std::string why;
    if (!alertEngine_.setRules(k.keywords.empty() ? std::vector<al::Rule>{} : std::vector<al::Rule>{r}, &why)) { st_.alertProblem = why; }
}

void Hub::configure(const Config& c) {
    bool startWorker = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (configured_ && c == cfg_) { return; }
        const Config old = cfg_;
        const bool first = !configured_;
        configured_ = true;
        cfg_ = c;
        if (first || old.mqtt != c.mqtt || old.mqttHost != c.mqttHost || old.mqttPort != c.mqttPort || old.mqttTopic != c.mqttTopic || old.mqttUser != c.mqttUser ||
            old.mqttPassword != c.mqttPassword) {
            applyMqtt(c);
        }
        if (first || old.alerts != c.alerts || old.keywords != c.keywords || old.alertGapS != c.alertGapS) { applyAlerts(c); }
        st_.influxProblem.clear();
        if (c.influx) {
            std::string why = checkMqttHost(c.influxHost);
            if (why.empty()) { why = checkInfluxText(c.influxBucket, false); }
            if (why.empty()) { why = checkInfluxText(c.influxOrg, true); }
            if (why.empty() && (c.influxPort < 1 || c.influxPort > 65535)) { why = "the port is 1 to 65535"; }
            if (!why.empty()) { st_.influxProblem = "settings: " + why; }
        }
        st_.hookProblem.clear();
        if (c.hook) {
            const std::string why = checkWebhookUrl(c.hookUrl);
            if (!why.empty()) { st_.hookProblem = why; }
            else if (c.hookUrl.empty()) { st_.hookProblem = "an address is needed"; }
        }
        startWorker = c.hook || c.influx || (c.alerts && c.alertNotify);
    }
    if (startWorker) {
        std::lock_guard<std::mutex> lk(qm_);
        if (!worker_.joinable()) {
            workerStop_ = false;
            worker_ = std::thread([this] { workerLoop(); });
        }
    }
}

void Hub::post(std::function<void()> job) {
    std::lock_guard<std::mutex> lk(qm_);
    if (!worker_.joinable()) { return; }
    if (jobs_.size() >= kMaxJobs) {
        jobs_.pop_front();
        std::lock_guard<std::mutex> lk2(m_);
        ++st_.hookDropped;
    }
    jobs_.push_back(std::move(job));
    qcv_.notify_one();
}

void Hub::workerLoop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk(qm_);
            qcv_.wait(lk, [this] { return workerStop_ || !jobs_.empty(); });
            if (workerStop_) { return; }
            job = std::move(jobs_.front());
            jobs_.pop_front();
            ++running_;
        }
        try { job(); } catch (...) {   // a job that throws must not end the worker: it is counted and said in the alert row's sentence
            std::lock_guard<std::mutex> lk(m_);
            ++st_.notifyFailed;
            st_.notifyProblem = "an output job failed unexpectedly";
        }
        {
            std::lock_guard<std::mutex> lk(qm_);
            --running_;
        }
        qcv_.notify_all();
    }
}

bool Hub::drain(int ms) {
    std::unique_lock<std::mutex> lk(qm_);
    return qcv_.wait_for(lk, std::chrono::milliseconds(ms), [this] { return jobs_.empty() && running_ == 0; });
}

void Hub::stop() {
    {
        std::lock_guard<std::mutex> lk(qm_);
        workerStop_ = true;
        cancel_->cancel();
    }
    qcv_.notify_all();
    if (worker_.joinable()) { worker_.join(); }
    std::lock_guard<std::mutex> lk(m_);
    if (mqtt_) { mqtt_->stop(); mqtt_.reset(); }
    cfg_ = Config{};
    configured_ = false;
    influxBusy_ = false;
}

void Hub::ingest(const std::vector<Event>& events) {
    if (events.empty()) { return; }
    Config c;
    std::shared_ptr<al::NotificationSink> sink;
    Poster poster;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!(cfg_.mqtt || cfg_.hook || cfg_.alerts)) { return; }
        c = cfg_;
        sink = sink_;
        poster = poster_;
    }
    for (const Event& ev : events) {
        {
            std::lock_guard<std::mutex> lk(m_);
            ++st_.seen;
            if (ev.priv) { ++st_.heldPrivate; }
        }
        if (ev.priv) { continue; }   // a private plugin's line never leaves the program
        dj::DecodeEvent je;
        je.plugin = ev.plugin;
        je.unixMs = ev.unixMs;
        je.frequencyHz = ev.frequencyHz > 0.0 ? ev.frequencyHz : std::nan("");
        je.text = ev.text;
        const std::string line = dj::toJson(je);
        if (c.mqtt && !c.mqttAlerts) {
            std::lock_guard<std::mutex> lk(m_);
            if (mqtt_) { (void)mqtt_->publish(c.mqttTopic, line, 0, false); }
        }
        if (c.hook && !c.hookAlerts && !c.hookUrl.empty() && checkWebhookUrl(c.hookUrl).empty()) { queueHook(c.hookUrl, line, poster); }
        if (c.alerts) {
            cascade::core::decodedb::DecodeEvent de;
            de.timeMs = ev.unixMs;
            de.freqHz = ev.frequencyHz > 0.0 ? ev.frequencyHz : 0.0;
            de.decoder = ev.plugin;
            de.kind = ev.kind;
            de.text = ev.text;
            for (const al::Alert& a : alertEngine_.process(de)) {
                std::string recent = a.matchedBy + ": " + cutUtf8(a.body, 120);
                {
                    std::lock_guard<std::mutex> lk(m_);
                    ++st_.alertsRaised;
                    recent_.push_back(std::move(recent));
                    if (recent_.size() > kRecent) { recent_.erase(recent_.begin()); }
                }
                if (c.mqtt && c.mqttAlerts) {
                    std::lock_guard<std::mutex> lk(m_);
                    if (mqtt_) { (void)mqtt_->publish(c.mqttTopic, alertJson(a), 0, false); }
                }
                if (c.hook && c.hookAlerts && !c.hookUrl.empty() && checkWebhookUrl(c.hookUrl).empty()) { queueHook(c.hookUrl, alertJson(a), poster); }
                if (c.alertNotify && sink) {
                    const al::Notification n = al::toNotification(a);
                    post([this, sink, n] {
                        std::string why;
                        bool ok = false;
                        try { ok = sink->notify(n, &why); } catch (...) { why = "the notification sink threw"; }
                        std::lock_guard<std::mutex> lk(m_);
                        if (ok) { ++st_.notified; }
                        else { ++st_.notifyFailed; st_.notifyProblem = why.empty() ? "the notification was refused" : why; }
                    });
                }
            }
        }
    }
}

void Hub::queueHook(const std::string& url, const std::string& json, const Poster& poster) {
    {
        std::lock_guard<std::mutex> lk(qm_);
        std::size_t waiting = 0;
        waiting = jobs_.size();
        if (waiting >= kMaxHookQueue) {
            std::lock_guard<std::mutex> lk2(m_);
            ++st_.hookDropped;
            return;
        }
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        ++st_.hookQueued;
    }
    const std::shared_ptr<cascade::core::UploadCancel> cancel = cancel_;
    post([this, url, json, poster, cancel] {
        cascade::core::webhook::Result r;
        if (poster) { r = poster(url, json); }
        else {
            cascade::core::webhook::Options o;
            o.enabled = true;
            o.url = url;
            r = cascade::core::webhook::send(o, json, cancel);
        }
        std::lock_guard<std::mutex> lk(m_);
        if (r.ok) { ++st_.hookOk; st_.hookProblem.clear(); }
        else { ++st_.hookFailed; st_.hookProblem = r.error.empty() ? "no answer" : r.error; }   // the client's sentence never holds the address
    });
}

}  // namespace cascade::engine::s4
