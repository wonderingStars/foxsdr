// radio_matrix_model.hpp - the MODELS behind a multi-radio overview ("Matrix"): a per-radio summary table, a hover-to-listen policy and a
// follow-a-master relation. No screen, no engine call, no clock of its own: every function takes the time it is given.
//
// STAND-ALONE (lane rfmon): nothing in the engine uses it. The screen draws the table; the audio change a hover decides on goes through the
// engine's EXISTING solo / audio-route call (the screen lane passes the radio this model names to it); the engine's retune path applies the
// frequency a follow relation computes. This header only decides.
//
// WHAT IS DEFINED FROM A SOURCE (opened 2026-11-11):
//  * HOVER-TO-LISTEN: SDR Console's "Receivers" page (www.sdr-radio.com/receivers, (c) SDR-Radio.com Ltd, the maker's own documentation) lists an
//    "Audio preview" option: "while the mouse is in a receiver window the receiver is unmuted"; the same page calls the Matrix the window
//    that "can display as many receivers as required" (up to 24 there; FoxSDR's limit is EIGHT open radios). The page says nothing of a delay or of
//    what happens when the mouse leaves; the DWELL and the GRACE below are this model's own, defaults chosen to keep a pointer sweeping across
//    the grid from flicking the audio (the screen lane may expose them) - UNVERIFIED as to what feels right.
//  * FOLLOW A MASTER: the SDRuno User Manual (v1.21, (c) SDRplay Limited 2017, from wtfda.org's copy of the maker's PDF, section 2.3 and 4.6)
//    defines it for its virtual receivers: VRX #0 is the "master", the only one that can change the hardware LO, and "changing the hardware LO
//    has the effect of changing the tuning frequency of all the active VRX"; an "LO LOCK" (or SHIFT while tuning) lets the master change its own
//    frequency WITHOUT moving the LO, so the others stay. That is a delta-follow with a lock, and that is what FollowSet implements: a follower
//    keeps its OFFSET from its master (the offset is recorded when the link is made); when the master moves by D, the follower moves by D, unless
//    the link is locked. UNVERIFIED: (a) that this is what users of separate physical radios want (SDRuno's followers are virtual receivers
//    inside ONE hardware stream, FoxSDR's would be separate radios), (b) that later SDRuno versions behave the same (only v1.21 was read).
//
// PRIVACY: a summary row holds numbers and enums only: no device serial, no name, no address. The radio's display name is the screen's.
//
// THREADS: plain values; one thread owns one object.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace cascade::core {

inline constexpr int kMatrixMaxRadios = 8;

enum class MatrixRadioState : std::uint8_t { Closed, Opening, Running, Busy, Faulted };

inline const char* matrixRadioStateText(MatrixRadioState s) {
    switch (s) {
        case MatrixRadioState::Closed: return "closed";
        case MatrixRadioState::Opening: return "opening";
        case MatrixRadioState::Running: return "running";
        case MatrixRadioState::Busy: return "busy";
        case MatrixRadioState::Faulted: return "faulted";
    }
    return "?";
}

// One row of the table.
struct RadioSummary {
    int id = -1;                            // 0 .. kMatrixMaxRadios - 1
    MatrixRadioState state = MatrixRadioState::Closed;
    double centreHz = 0.0;
    double sampleRateHz = 0.0;
    double tunedHz = 0.0;                   // the listened-to channel
    int modeCode = 0;                       // the engine's demod enum value; the screen names it
    float levelDb = -200.0f;                // signal level at the tuned channel (S-meter source)
    bool listening = false;                 // this radio's audio is the one heard
    bool muted = false;                     // the user muted it
    int followsId = -1;                     // master this radio follows, -1 none
    std::uint32_t overruns = 0;             // source-ring drops since start
};

// The table: at most one row per id, rows kept sorted by id.
class RadioMatrix {
public:
    // Inserts or replaces a row. Refuses an id outside 0..7. Non-finite numbers are stored as zero / the floor so a bad frame cannot poison a sum.
    bool update(const RadioSummary& in) {
        if (in.id < 0 || in.id >= kMatrixMaxRadios) { return false; }
        RadioSummary r = in;
        if (!std::isfinite(r.centreHz)) { r.centreHz = 0.0; }
        if (!std::isfinite(r.sampleRateHz) || r.sampleRateHz < 0.0) { r.sampleRateHz = 0.0; }
        if (!std::isfinite(r.tunedHz)) { r.tunedHz = 0.0; }
        if (!std::isfinite(r.levelDb)) { r.levelDb = -200.0f; }
        if (r.followsId < -1 || r.followsId >= kMatrixMaxRadios || r.followsId == r.id) { r.followsId = -1; }
        rows_[static_cast<std::size_t>(r.id)] = r;
        have_[static_cast<std::size_t>(r.id)] = true;
        return true;
    }
    // A radio that was closed or unplugged.
    void remove(int id) {
        if (id >= 0 && id < kMatrixMaxRadios) { have_[static_cast<std::size_t>(id)] = false; }
    }
    void clear() { have_.fill(false); }

    std::size_t size() const {
        std::size_t n = 0;
        for (bool h : have_) { n += h ? 1u : 0u; }
        return n;
    }
    // Rows by id, ascending.
    std::vector<RadioSummary> rows() const {
        std::vector<RadioSummary> v;
        for (int i = 0; i < kMatrixMaxRadios; ++i) {
            if (have_[static_cast<std::size_t>(i)]) { v.push_back(rows_[static_cast<std::size_t>(i)]); }
        }
        return v;
    }
    const RadioSummary* find(int id) const {
        return (id >= 0 && id < kMatrixMaxRadios && have_[static_cast<std::size_t>(id)]) ? &rows_[static_cast<std::size_t>(id)] : nullptr;
    }
    std::size_t runningCount() const {
        std::size_t n = 0;
        for (int i = 0; i < kMatrixMaxRadios; ++i) {
            if (have_[static_cast<std::size_t>(i)] && rows_[static_cast<std::size_t>(i)].state == MatrixRadioState::Running) { ++n; }
        }
        return n;
    }
    // The radio whose audio is heard, -1 when none. Where the rows disagree (more than one flagged) the LOWEST id wins and `consistent` is false.
    int listeningId(bool* consistent = nullptr) const {
        int first = -1, count = 0;
        for (int i = 0; i < kMatrixMaxRadios; ++i) {
            if (have_[static_cast<std::size_t>(i)] && rows_[static_cast<std::size_t>(i)].listening) {
                if (first < 0) { first = i; }
                ++count;
            }
        }
        if (consistent != nullptr) { *consistent = count <= 1; }
        return first;
    }

private:
    std::array<RadioSummary, kMatrixMaxRadios> rows_{};
    std::array<bool, kMatrixMaxRadios> have_{};
};

// Grid for `count` cells (1..8): columns and rows, never taller than wide, the last row may be short:
// 1 -> 1x1, 2 -> 2x1, 3 and 4 -> 2x2, 5 and 6 -> 3x2, 7 and 8 -> 4x2. Counts outside 1..8 give 0 x 0.
struct MatrixGrid {
    int cols = 0;
    int rows = 0;
};
inline MatrixGrid matrixGridFor(int count) {
    static constexpr int kCols[kMatrixMaxRadios] = {1, 2, 2, 2, 3, 3, 4, 4};
    if (count < 1 || count > kMatrixMaxRadios) { return {}; }
    const int c = kCols[count - 1];
    return {c, (count + c - 1) / c};
}

// ----- Hover to listen ------------------------------------------------------------------------------------------------------------------

struct HoverListenConfig {
    double dwellS = 0.15;     // the pointer must rest on a cell this long before the audio follows it (0: at once, as SDR Console does)
    double graceS = 0.30;     // after the pointer leaves every cell, the audio returns to the baseline only after this long
    bool skipMuted = true;    // a radio the user muted is not switched to by a hover
};

struct HoverDecision {
    bool changed = false;     // the audio route must change now
    int listenId = -1;        // the radio to route to; -1 means "back to the user's own choice" (the baseline)
};

class HoverListen {
public:
    void configure(const HoverListenConfig& c) {
        cfg_ = c;
        if (!(cfg_.dwellS >= 0.0) || !std::isfinite(cfg_.dwellS)) { cfg_.dwellS = 0.0; }
        if (!(cfg_.graceS >= 0.0) || !std::isfinite(cfg_.graceS)) { cfg_.graceS = 0.0; }
        reset();
    }
    // The radio the user chose by clicking (or the engine's own default): what hovering overrides and what it returns to. -1: nothing.
    void setBaseline(int id) { baseline_ = validId(id) ? id : -1; }
    int baseline() const { return baseline_; }
    // The radio currently routed because of a hover; -1 when the baseline is in force.
    int hovering() const { return active_; }
    void reset() {
        candidate_ = -1;
        candidateSinceS_ = 0.0;
        active_ = -1;
        leftSinceS_ = 0.0;
        left_ = false;
        lastS_ = -std::numeric_limits<double>::infinity();
    }

    // Called every frame with the cell under the pointer (-1: none, or the pointer is dragging / a menu is open) and the muted state of each
    // radio (bit i of mutedMask = radio i is muted). A clock that is not finite or goes backwards is ignored for the frame.
    HoverDecision update(int hoverId, std::uint32_t mutedMask, double nowS) {
        HoverDecision d;
        if (!std::isfinite(nowS) || nowS < lastS_) { return d; }
        lastS_ = nowS;
        if (!validId(hoverId) || (cfg_.skipMuted && ((mutedMask >> hoverId) & 1u) != 0)) { hoverId = -1; }
        if (hoverId >= 0) {
            left_ = false;
            if (hoverId != candidate_) {
                candidate_ = hoverId;
                candidateSinceS_ = nowS;
            }
            if (active_ != hoverId && nowS - candidateSinceS_ >= cfg_.dwellS - kClockSlackS) {
                active_ = hoverId;
                d.changed = true;
                d.listenId = hoverId;
            }
            return d;
        }
        // Not over a (usable) cell.
        candidate_ = -1;
        if (active_ >= 0) {
            if (!left_) { left_ = true; leftSinceS_ = nowS; }
            if (nowS - leftSinceS_ >= cfg_.graceS - kClockSlackS) {
                active_ = -1;
                left_ = false;
                d.changed = true;
                d.listenId = baseline_;      // -1 when there is none: nothing is routed
            }
        }
        return d;
    }

private:
    static bool validId(int id) { return id >= 0 && id < kMatrixMaxRadios; }
    static constexpr double kClockSlackS = 1e-9;   // a clock summed in 50 ms steps must not miss a 150 ms dwell by rounding
    HoverListenConfig cfg_{};
    int baseline_ = -1;
    int candidate_ = -1;
    double candidateSinceS_ = 0.0;
    int active_ = -1;
    double leftSinceS_ = 0.0;
    bool left_ = false;
    double lastS_ = -std::numeric_limits<double>::infinity();
};

// ----- Follow a master ------------------------------------------------------------------------------------------------------------------

enum class FollowError { None, BadId, SameRadio, Cycle, NotFinite, AlreadyFollowing };

inline const char* followErrorText(FollowError e) {
    switch (e) {
        case FollowError::None: return "ok";
        case FollowError::BadId: return "radio id out of range";
        case FollowError::SameRadio: return "a radio cannot follow itself";
        case FollowError::Cycle: return "that link would make radios follow each other in a circle";
        case FollowError::NotFinite: return "frequency is not a number";
        case FollowError::AlreadyFollowing: return "that radio already follows a master";
    }
    return "?";
}

struct FollowMove {
    int radioId = -1;
    double newHz = 0.0;       // the frequency to retune this follower to
    bool clamped = false;     // the follower's tuning range stopped it short of master + offset
};

struct FollowLimits {
    double minHz = 1000.0;
    double maxHz = 6e9;
};

// Up to eight radios, each following at most one master; chains are allowed (a follower may itself be a master), cycles are refused.
class FollowSet {
public:
    // Makes `follower` follow `master`, recording offset = followerHz - masterHz now.
    FollowError link(int follower, int master, double followerHz, double masterHz) {
        if (!validId(follower) || !validId(master)) { return FollowError::BadId; }
        if (follower == master) { return FollowError::SameRadio; }
        if (!std::isfinite(followerHz) || !std::isfinite(masterHz)) { return FollowError::NotFinite; }
        if (master_[idx(follower)] >= 0) { return FollowError::AlreadyFollowing; }
        for (int m = master; m >= 0; m = master_[idx(m)]) {          // walking up from the proposed master must not reach the follower
            if (m == follower) { return FollowError::Cycle; }
        }
        master_[idx(follower)] = master;
        offsetHz_[idx(follower)] = followerHz - masterHz;
        locked_[idx(follower)] = false;
        return FollowError::None;
    }
    void unlink(int follower) {
        if (validId(follower)) { master_[idx(follower)] = -1; locked_[idx(follower)] = false; }
    }
    // A radio that closed: nothing follows it and it follows nothing.
    void radioClosed(int id) {
        if (!validId(id)) { return; }
        master_[idx(id)] = -1;
        locked_[idx(id)] = false;
        for (int i = 0; i < kMatrixMaxRadios; ++i) {
            if (master_[idx(i)] == id) { master_[idx(i)] = -1; locked_[idx(i)] = false; }
        }
    }
    // SDRuno's LO LOCK, per link: a locked follower stays where it is while its master moves.
    void setLocked(int follower, bool locked) {
        if (validId(follower) && master_[idx(follower)] >= 0) { locked_[idx(follower)] = locked; }
    }
    bool locked(int follower) const { return validId(follower) && locked_[idx(follower)]; }
    int masterOf(int follower) const { return validId(follower) ? master_[idx(follower)] : -1; }
    double offsetOf(int follower) const { return validId(follower) ? offsetHz_[idx(follower)] : 0.0; }
    // Changes the stored offset (the user nudged the follower while it was linked, so the new distance is kept).
    void setOffset(int follower, double offsetHz) {
        if (validId(follower) && master_[idx(follower)] >= 0 && std::isfinite(offsetHz)) { offsetHz_[idx(follower)] = offsetHz; }
    }
    std::size_t linkCount() const {
        std::size_t n = 0;
        for (int i = 0; i < kMatrixMaxRadios; ++i) { n += master_[idx(i)] >= 0 ? 1u : 0u; }
        return n;
    }

    // The master moved to newMasterHz: the retunes for everything that follows it, directly or through a chain, in an order a caller can apply
    // front to back (a master is retuned before its own followers). `known[i]` is false for a radio that is not open (skipped). Locked
    // followers are not moved and neither are THEIR followers (their master did not move).
    std::vector<FollowMove> onMasterMoved(int master, double newMasterHz, const std::array<bool, kMatrixMaxRadios>& known, const FollowLimits& lim = {}) const {
        std::vector<FollowMove> out;
        if (!validId(master) || !std::isfinite(newMasterHz)) { return out; }
        std::array<double, kMatrixMaxRadios> hz{};
        std::array<bool, kMatrixMaxRadios> moved{};
        hz[idx(master)] = newMasterHz;
        moved[idx(master)] = true;
        // At most kMatrixMaxRadios passes: each pass resolves one more link depth.
        for (int pass = 0; pass < kMatrixMaxRadios; ++pass) {
            bool any = false;
            for (int f = 0; f < kMatrixMaxRadios; ++f) {
                const int m = master_[idx(f)];
                if (m < 0 || moved[idx(f)] || !moved[idx(m)] || locked_[idx(f)] || !known[idx(f)]) { continue; }
                double want = hz[idx(m)] + offsetHz_[idx(f)];
                bool clamped = false;
                if (want < lim.minHz) { want = lim.minHz; clamped = true; }
                if (want > lim.maxHz) { want = lim.maxHz; clamped = true; }
                hz[idx(f)] = want;
                moved[idx(f)] = true;
                out.push_back({f, want, clamped});
                any = true;
            }
            if (!any) { break; }
        }
        return out;
    }

private:
    static bool validId(int id) { return id >= 0 && id < kMatrixMaxRadios; }
    static std::size_t idx(int id) { return static_cast<std::size_t>(id); }
    std::array<int, kMatrixMaxRadios> master_ = {-1, -1, -1, -1, -1, -1, -1, -1};
    std::array<double, kMatrixMaxRadios> offsetHz_{};
    std::array<bool, kMatrixMaxRadios> locked_{};
};

}  // namespace cascade::core
