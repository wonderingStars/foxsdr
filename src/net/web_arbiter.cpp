// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/web_arbiter.hpp"

#include <algorithm>
#include <random>

namespace cascade::net {
namespace {

std::uint64_t splitmix(std::uint64_t& x) {
    x += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

}  // namespace

const char* arbResultText(ArbResult r) {
    switch (r) {
        case ArbResult::Ok: return "ok";
        case ArbResult::Unknown: return "you are not connected";
        case ArbResult::NotAdmin: return "only the owner may do that";
        case ArbResult::Full: return "too many viewers are connected";
        case ArbResult::ListenerOnly: return "listeners may only listen";
        case ArbResult::Queued: return "you are waiting for control";
        case ArbResult::AlreadyHolder: return "you already have control";
        case ArbResult::QueueFull: return "too many people are waiting for control";
        case ArbResult::NotExclusive: return "control is open to everyone who may tune";
        case ArbResult::NotHolder: return "you do not have control";
        case ArbResult::Denied: return "someone else has control";
    }
    return "unknown";
}

ControlArbiter::ControlArbiter(const ArbiterConfig& cfg) : cfg_(cfg) {
    rng_ = cfg.idSeed != 0 ? cfg.idSeed : (static_cast<std::uint64_t>(std::random_device{}()) << 32) ^ std::random_device{}();
}

ControlArbiter::Viewer* ControlArbiter::find(const std::string& id) {
    for (Viewer& v : viewers_) {
        if (v.id == id) { return &v; }
    }
    return nullptr;
}

std::string ControlArbiter::newIdLocked() {
    static const char* hex = "0123456789abcdef";
    for (;;) {
        std::uint64_t r = splitmix(rng_);
        std::string id(12, '0');
        for (int i = 0; i < 12; ++i) { id[i] = hex[(r >> (i * 4)) & 0xF]; }
        if (find(id) == nullptr) { return id; }
    }
}

void ControlArbiter::removeFromQueueLocked(const std::string& id) {
    queue_.erase(std::remove(queue_.begin(), queue_.end(), id), queue_.end());
}

void ControlArbiter::setHolderLocked(const std::string& id, std::int64_t nowMs) {
    holder_ = id;
    holderSinceMs_ = nowMs;
    if (Viewer* v = find(id)) { v->lastActionMs = nowMs; }
    removeFromQueueLocked(id);
    ++epoch_;
}

void ControlArbiter::promoteLocked(std::int64_t nowMs) {
    if (mode_ != ControlMode::Exclusive || !holder_.empty()) { return; }
    while (!queue_.empty()) {
        const std::string next = queue_.front();
        queue_.pop_front();
        Viewer* v = find(next);
        if (v != nullptr && v->mayTune) {
            setHolderLocked(next, nowMs);
            return;
        }
    }
    ++epoch_;
}

void ControlArbiter::dropLocked(const std::string& id) {
    const bool wasHolder = holder_ == id;
    removeFromQueueLocked(id);
    viewers_.erase(std::remove_if(viewers_.begin(), viewers_.end(), [&](const Viewer& v) { return v.id == id; }), viewers_.end());
    if (wasHolder) { holder_.clear(); }
    ++epoch_;
}

void ControlArbiter::expireLocked(std::int64_t nowMs) {
    // Viewers first: a viewer that timed out cannot keep control or a place in the queue.
    std::vector<std::string> gone;
    for (const Viewer& v : viewers_) {
        if (nowMs - v.lastSeenMs >= cfg_.viewerTimeoutMs) { gone.push_back(v.id); }
    }
    for (const std::string& id : gone) { dropLocked(id); }
    if (mode_ == ControlMode::Exclusive && !holder_.empty()) {
        const Viewer* h = find(holder_);
        const bool idle = h != nullptr && nowMs - h->lastActionMs >= cfg_.holderIdleMs;
        const bool tooLong = cfg_.maxHoldMs > 0 && nowMs - holderSinceMs_ >= cfg_.maxHoldMs;
        if (h == nullptr || !h->mayTune || idle || tooLong) { holder_.clear(); ++epoch_; }
    }
    if (mode_ == ControlMode::Exclusive && holder_.empty() && !queue_.empty()) { promoteLocked(nowMs); }
}

ArbResult ControlArbiter::join(bool isAdmin, bool mayTune, std::int64_t nowMs, std::string& idOut) {
    idOut.clear();
    std::lock_guard<std::mutex> lk(mu_);
    expireLocked(nowMs);
    if (viewers_.size() >= cfg_.maxViewers) { return ArbResult::Full; }
    Viewer v;
    v.id = newIdLocked();
    v.isAdmin = isAdmin;
    v.mayTune = mayTune || isAdmin;  // an admin can always tune
    v.joinedMs = v.lastSeenMs = v.lastActionMs = nowMs;
    idOut = v.id;
    viewers_.push_back(std::move(v));
    ++epoch_;
    return ArbResult::Ok;
}

ArbResult ControlArbiter::leave(const std::string& id, std::int64_t nowMs) {
    std::lock_guard<std::mutex> lk(mu_);
    expireLocked(nowMs);
    if (find(id) == nullptr) { return ArbResult::Unknown; }
    dropLocked(id);
    promoteLocked(nowMs);
    return ArbResult::Ok;
}

ArbResult ControlArbiter::heartbeat(const std::string& id, std::int64_t nowMs) {
    std::lock_guard<std::mutex> lk(mu_);
    expireLocked(nowMs);
    Viewer* v = find(id);
    if (v == nullptr) { return ArbResult::Unknown; }
    v->lastSeenMs = nowMs;
    return ArbResult::Ok;
}

ArbResult ControlArbiter::setRights(const std::string& id, bool isAdmin, bool mayTune, std::int64_t nowMs) {
    std::lock_guard<std::mutex> lk(mu_);
    expireLocked(nowMs);
    Viewer* v = find(id);
    if (v == nullptr) { return ArbResult::Unknown; }
    v->isAdmin = isAdmin;
    v->mayTune = mayTune || isAdmin;
    if (!v->mayTune) {
        removeFromQueueLocked(id);
        if (holder_ == id) { holder_.clear(); }
    }
    ++epoch_;
    promoteLocked(nowMs);
    return ArbResult::Ok;
}

ControlDecision ControlArbiter::mayAct(const std::string& id, std::int64_t nowMs, bool touch) {
    std::lock_guard<std::mutex> lk(mu_);
    expireLocked(nowMs);
    ControlDecision d;
    Viewer* v = find(id);
    if (v == nullptr) { d.why = ArbResult::Unknown; return d; }
    if (!v->mayTune) { d.why = ArbResult::ListenerOnly; return d; }
    if (mode_ == ControlMode::Open) { d.allowed = true; return d; }
    if (holder_ == id) {
        if (touch) { v->lastActionMs = nowMs; }
        d.allowed = true;
        return d;
    }
    if (v->isAdmin) { d.allowed = true; d.viaOverride = true; return d; }
    d.why = ArbResult::Denied;
    return d;
}

ArbResult ControlArbiter::requestControl(const std::string& id, std::int64_t nowMs) {
    std::lock_guard<std::mutex> lk(mu_);
    expireLocked(nowMs);
    Viewer* v = find(id);
    if (v == nullptr) { return ArbResult::Unknown; }
    if (!v->mayTune) { return ArbResult::ListenerOnly; }
    if (mode_ == ControlMode::Open) { return ArbResult::NotExclusive; }
    if (holder_ == id) { return ArbResult::AlreadyHolder; }
    if (holder_.empty()) { setHolderLocked(id, nowMs); return ArbResult::Ok; }
    if (std::find(queue_.begin(), queue_.end(), id) != queue_.end()) { return ArbResult::Queued; }
    if (queue_.size() >= cfg_.maxQueue) { return ArbResult::QueueFull; }
    queue_.push_back(id);
    ++epoch_;
    return ArbResult::Queued;
}

ArbResult ControlArbiter::releaseControl(const std::string& id, std::int64_t nowMs) {
    std::lock_guard<std::mutex> lk(mu_);
    expireLocked(nowMs);
    if (find(id) == nullptr) { return ArbResult::Unknown; }
    if (holder_ != id) { return ArbResult::NotHolder; }
    holder_.clear();
    ++epoch_;
    promoteLocked(nowMs);
    return ArbResult::Ok;
}

ArbResult ControlArbiter::cancelRequest(const std::string& id, std::int64_t nowMs) {
    std::lock_guard<std::mutex> lk(mu_);
    expireLocked(nowMs);
    if (find(id) == nullptr) { return ArbResult::Unknown; }
    if (std::find(queue_.begin(), queue_.end(), id) == queue_.end()) { return ArbResult::NotHolder; }
    removeFromQueueLocked(id);
    ++epoch_;
    return ArbResult::Ok;
}

ArbResult ControlArbiter::setMode(const std::string& adminId, ControlMode mode, std::int64_t nowMs) {
    std::lock_guard<std::mutex> lk(mu_);
    expireLocked(nowMs);
    const Viewer* a = find(adminId);
    if (a == nullptr) { return ArbResult::Unknown; }
    if (!a->isAdmin) { return ArbResult::NotAdmin; }
    if (mode == mode_) { return ArbResult::Ok; }
    mode_ = mode;
    if (mode == ControlMode::Open) {
        holder_.clear();
        queue_.clear();
    }
    ++epoch_;
    return ArbResult::Ok;
}

ArbResult ControlArbiter::grant(const std::string& adminId, const std::string& targetId, std::int64_t nowMs) {
    std::lock_guard<std::mutex> lk(mu_);
    expireLocked(nowMs);
    const Viewer* a = find(adminId);
    if (a == nullptr) { return ArbResult::Unknown; }
    if (!a->isAdmin) { return ArbResult::NotAdmin; }
    const Viewer* t = find(targetId);
    if (t == nullptr) { return ArbResult::Unknown; }
    if (mode_ != ControlMode::Exclusive) { return ArbResult::NotExclusive; }
    if (!t->mayTune) { return ArbResult::ListenerOnly; }
    if (holder_ == targetId) { return ArbResult::AlreadyHolder; }
    setHolderLocked(targetId, nowMs);
    return ArbResult::Ok;
}

ArbResult ControlArbiter::revoke(const std::string& adminId, std::int64_t nowMs) {
    std::lock_guard<std::mutex> lk(mu_);
    expireLocked(nowMs);
    const Viewer* a = find(adminId);
    if (a == nullptr) { return ArbResult::Unknown; }
    if (!a->isAdmin) { return ArbResult::NotAdmin; }
    if (mode_ != ControlMode::Exclusive) { return ArbResult::NotExclusive; }
    if (holder_.empty()) { return ArbResult::NotHolder; }
    holder_.clear();
    ++epoch_;
    promoteLocked(nowMs);  // the next in line (if any) takes over, exactly as after a release
    return ArbResult::Ok;
}

ControlMode ControlArbiter::mode(std::int64_t nowMs) {
    std::lock_guard<std::mutex> lk(mu_);
    expireLocked(nowMs);
    return mode_;
}

std::string ControlArbiter::holder(std::int64_t nowMs) {
    std::lock_guard<std::mutex> lk(mu_);
    expireLocked(nowMs);
    return holder_;
}

std::vector<ViewerInfo> ControlArbiter::viewers(std::int64_t nowMs) {
    std::lock_guard<std::mutex> lk(mu_);
    expireLocked(nowMs);
    std::vector<ViewerInfo> out;
    for (const Viewer& v : viewers_) {
        ViewerInfo i;
        i.id = v.id;
        i.isAdmin = v.isAdmin;
        i.mayTune = v.mayTune;
        i.holder = v.id == holder_;
        const auto q = std::find(queue_.begin(), queue_.end(), v.id);
        i.queuePosition = q == queue_.end() ? 0 : static_cast<std::size_t>(q - queue_.begin()) + 1;
        i.connectedMs = nowMs - v.joinedMs;
        i.idleMs = nowMs - v.lastSeenMs;
        out.push_back(std::move(i));
    }
    return out;
}

std::vector<std::string> ControlArbiter::queue(std::int64_t nowMs) {
    std::lock_guard<std::mutex> lk(mu_);
    expireLocked(nowMs);
    return std::vector<std::string>(queue_.begin(), queue_.end());
}

std::size_t ControlArbiter::viewerCount(std::int64_t nowMs) {
    std::lock_guard<std::mutex> lk(mu_);
    expireLocked(nowMs);
    return viewers_.size();
}

std::uint64_t ControlArbiter::epoch() const {
    std::lock_guard<std::mutex> lk(mu_);
    return epoch_;
}

void ControlArbiter::tick(std::int64_t nowMs) {
    std::lock_guard<std::mutex> lk(mu_);
    expireLocked(nowMs);
}

}  // namespace cascade::net
