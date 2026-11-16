// s4_outputs.hpp - screen lane S4's engine half: where the host's decoded-text feed (the plugin host's event ring) can be SENT, and the keyword alerts raised from it.
//
// WHAT THIS IS. The host keeps a ring of the lines the decoder plugins print (core/plugin_api.hpp postEvent / hostEvents; every decoded line is posted as kind "text").
// Three consumers of that ring were written as stand-alone models and never wired: the MQTT publisher (core/mqtt_publisher.hpp), the decode webhook (core/decode_webhook.hpp) and
// the keyword alert rules (core/alert_rules.hpp). This class is the wiring: the engine's periodic pass hands it the new events, it sends them where the user told it to.
//
// EVERYTHING IS OFF until the user turns it on (the wirec keys out.* and alerts.*, whose defaults are "0"). Nothing opens a socket, starts a thread or reads the ring while every switch is off.
// A line from a PRIVATE plugin (CASCADE_EVENT_FLAG_PRIVATE) is never sent anywhere. Passwords and the webhook address (which may carry a token) are never published as a fact, never logged and never
// placed in an error sentence; the settings registry keeps them in the Windows per-user protected store (core/secret_vault.hpp).
//
// REACH. MQTT here is the plain-TCP publisher of the model: its user name and password cross the network UNENCRYPTED when the broker is another computer; scope() says so in a sentence the window
// shows. The webhook is the project's bounded client: https to any host, plain http only to this computer (a LAN address over plain http is refused by the client itself; owner decision 19).
// NO DESKTOP NOTIFICATION IS RAISED BY THIS CLASS: it hands each alert to a NotificationSink the caller installs (the engine installs none by default; a test installs a recording stand-in).
//
// THREADS. configure() and ingest() are called from the engine's pass thread; the network work (a webhook post, a notification) runs on a worker of this class, never on the pass thread. The
// MQTT publisher has a worker of its own. Queues are bounded: an overflow drops the oldest and is counted.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/alert_rules.hpp"
#include "core/decode_webhook.hpp"
#include "core/mqtt_publisher.hpp"
#include "core/power_log.hpp"

namespace cascade::engine::s4 {

// One decoded line, as the engine reads it from the host's event ring. `priv` = the plugin is private: the line is never sent.
struct Event {
    std::uint64_t seq = 0;
    std::int64_t unixMs = 0;
    double frequencyHz = 0.0;   // 0 = none
    std::string plugin;
    std::string kind;
    std::string text;
    bool priv = false;
};

struct Config {
    // MQTT
    bool mqtt = false;
    std::string mqttHost = "127.0.0.1";
    int mqttPort = 1883;
    std::string mqttTopic = "foxsdr/decodes";
    std::string mqttUser, mqttPassword;
    bool mqttAlerts = false;        // false: every decoded line; true: only the alerts
    // webhook
    bool hook = false;
    std::string hookUrl;
    bool hookAlerts = false;
    // InfluxDB: the signal level of the selected radio, as line protocol, every `influxEveryS` seconds (UDP datagrams or an HTTP write; no TLS: the answer for a broker or database on another computer is the user's own proxy)
    bool influx = false;
    std::string influxHost = "127.0.0.1";
    int influxPort = 8086;
    bool influxHttp = true;
    std::string influxOrg, influxBucket = "foxsdr", influxToken;
    int influxEveryS = 10;
    // keyword alerts
    bool alerts = false;
    std::string keywords;           // one keyword or phrase per line
    int alertGapS = 30;             // the same keyword again within this many seconds is not raised again
    bool alertNotify = true;        // hand a raised alert to the notification sink
    bool operator==(const Config& o) const {
        return mqtt == o.mqtt && mqttHost == o.mqttHost && mqttPort == o.mqttPort && mqttTopic == o.mqttTopic && mqttUser == o.mqttUser && mqttPassword == o.mqttPassword &&
               mqttAlerts == o.mqttAlerts && hook == o.hook && hookUrl == o.hookUrl && hookAlerts == o.hookAlerts && alerts == o.alerts && keywords == o.keywords &&
               alertGapS == o.alertGapS && alertNotify == o.alertNotify && influx == o.influx && influxHost == o.influxHost && influxPort == o.influxPort && influxHttp == o.influxHttp &&
               influxOrg == o.influxOrg && influxBucket == o.influxBucket && influxToken == o.influxToken && influxEveryS == o.influxEveryS;
    }
};

// Counts and sentences only: never a line of text, a user name, a password or an address.
struct Stats {
    std::uint64_t seen = 0;             // events read (while any output is on)
    std::uint64_t heldPrivate = 0;      // events of a private plugin, never sent
    std::uint64_t mqttOffered = 0, mqttRejected = 0;
    std::uint64_t hookQueued = 0, hookOk = 0, hookFailed = 0, hookDropped = 0;
    std::uint64_t alertsRaised = 0, notified = 0, notifyFailed = 0;
    std::uint64_t influxOk = 0, influxFailed = 0;
    std::string mqttState;              // "", "off", "connecting", "connected"
    std::string mqttProblem, hookProblem, alertProblem, notifyProblem, influxProblem;   // one sentence each, "" = none
};

// What a keyword list comes to: the keywords kept (trimmed, empty lines and repeats dropped, at most kMaxKeywords, each at most 120 bytes) and the problem found, if any.
constexpr std::size_t kMaxKeywords = 64;
constexpr std::size_t kMaxKeywordBytes = 120;
struct KeywordList {
    std::vector<std::string> keywords;
    std::string problem;
};
KeywordList parseKeywords(const std::string& text);

// A webhook address as the settings accept it: http:// or https://, no space or control character, at most 2048 bytes. Empty = refused with a sentence. "" = fine.
std::string checkWebhookUrl(const std::string& url);
// An MQTT host: a name or an address, no space, control character or slash; "" = fine.
std::string checkMqttHost(const std::string& host);
// An MQTT topic name: valid UTF-8, no wildcard, 1 to 200 bytes; "" = fine.
std::string checkMqttTopic(const std::string& topic);
// An InfluxDB host: as an MQTT host. An organisation or bucket: 1 to 64 bytes, no control character or space ("" is allowed for the organisation: version 1 databases have none).
std::string checkInfluxText(const std::string& text, bool mayBeEmpty);
// True when `host` is this computer (127.x, ::1, localhost).
bool isLoopbackHost(const std::string& host);
// The sentence that says who can read what is sent: "this computer only" or the unencrypted warning.
std::string mqttScope(const std::string& host, bool hasPassword);
std::string hookScope(const std::string& url);

class Hub {
public:
    using Poster = std::function<cascade::core::webhook::Result(const std::string& url, const std::string& json)>;
    Hub();
    ~Hub();
    Hub(const Hub&) = delete;
    Hub& operator=(const Hub&) = delete;

    // The sink a raised alert goes to (the real desktop notification, or a stand-in); null = none. The sink is called from the worker thread.
    void setNotifier(std::shared_ptr<cascade::core::alerts::NotificationSink> sink);
    // A stand-in for the webhook's client (tests); empty = the real bounded client.
    void setPoster(Poster p);
    // Applies the settings; a change in any of them reconfigures the parts that changed. Returns false when nothing is configured to run (every switch off).
    void configure(const Config& c);
    // Hands over the new events (any number, in order). Cheap and allocation-light while every switch is off.
    void ingest(const std::vector<Event>& events);
    // The level output: true when it is on and `everyS` seconds have passed since the last one (nowMs: any steady clock, in milliseconds). The engine's pass calls it, and only then reads a level.
    bool levelsDue(std::int64_t nowMs) const;
    // Hands the record to the sender (a worker); one at a time: a record offered while the last is still being sent is dropped and counted.
    void offerLevel(const cascade::core::powerlog::Record& r, std::int64_t nowMs);
    // A stand-in for the Influx sender (tests): empty = the real one (UDP or HTTP to the host named).
    using InfluxSender = std::function<cascade::core::powerlog::SendResult(const cascade::core::powerlog::InfluxTarget&, const std::vector<std::string>&)>;
    void setInfluxSender(InfluxSender f);
    // Whether any output or alert is switched on (the engine skips reading the ring while this is false).
    bool active() const;
    Stats stats() const;
    // The last alerts raised, newest last (title, then the matched keyword, then the text cut to 120 bytes), at most 20.
    std::vector<std::string> recentAlerts() const;
    // Waits (up to `ms`) for the worker to finish what it holds (tests); true when idle.
    bool drain(int ms);
    void stop();

private:
    void workerLoop();
    void post(std::function<void()> job);
    void applyMqtt(const Config& c);
    void applyAlerts(const Config& c);
    void queueHook(const std::string& url, const std::string& json, const Poster& poster);

    mutable std::mutex m_;
    Config cfg_;
    bool configured_ = false;
    Stats st_;
    std::unique_ptr<cascade::core::mqtt::Publisher> mqtt_;
    cascade::core::alerts::AlertEngine alertEngine_;
    std::vector<std::string> recent_;
    std::shared_ptr<cascade::core::alerts::NotificationSink> sink_;
    Poster poster_;
    InfluxSender influxSender_;
    std::int64_t lastInfluxMs_ = -1000000000;
    bool influxBusy_ = false;

    std::mutex qm_;
    std::condition_variable qcv_;
    std::deque<std::function<void()>> jobs_;
    std::size_t running_ = 0;
    std::thread worker_;
    bool workerStop_ = false;
    std::shared_ptr<cascade::core::UploadCancel> cancel_;
};

}  // namespace cascade::engine::s4
