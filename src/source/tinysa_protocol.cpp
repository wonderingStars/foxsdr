// tinysa_protocol.cpp - see tinysa_protocol.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/tinysa_protocol.hpp"

#include <cmath>

namespace cascade::source::tinysa {

double levelOffsetDb(Model m) {
    switch (m) {
        case Model::TinySa: return 128.0;
        case Model::TinySaUltra: return 174.0;
        case Model::Unknown: break;
    }
    return std::nan("");
}

std::string scanrawCommand(double startHz, double stopHz, std::size_t points, int option,
                           std::string* whyNot) {
    auto fail = [&](const char* why) {
        if (whyNot != nullptr) { *whyNot = why; }
        return std::string{};
    };
    if (!std::isfinite(startHz) || !std::isfinite(stopHz)) { return fail("frequency is not a number"); }
    if (startHz < 0.0 || stopHz > core::kSweepMaxFrequencyHz) { return fail("frequency out of range"); }
    if (!(startHz < stopHz)) { return fail("start must be below stop"); }
    if (points < 2 || points > core::kSweepMaxPointsPerLine) { return fail("points out of range"); }
    if (option < 0 || option > 3) { return fail("option must be 0..3"); }
    const auto s = static_cast<unsigned long long>(std::llround(startHz));
    const auto e = static_cast<unsigned long long>(std::llround(stopHz));
    if (!(s < e)) { return fail("start and stop round to the same Hz"); }
    std::string cmd = "scanraw " + std::to_string(s) + " " + std::to_string(e) + " " +
                      std::to_string(points) + " " + std::to_string(option);
    cmd += kCommandTerminator;
    return cmd;
}

ScanrawParser::ScanrawParser(std::size_t expectedPoints) : expected_(expectedPoints) {
    raw_.reserve(expected_ < core::kSweepMaxPointsPerLine ? expected_ : core::kSweepMaxPointsPerLine);
}

void ScanrawParser::reset() {
    st_ = St::Seek;
    status_ = FrameStatus::NeedMore;
    raw_.clear();
    discarded_ = 0;
}

FrameStatus ScanrawParser::feed(const std::uint8_t* data, std::size_t n) {
    for (std::size_t i = 0; i < n && st_ != St::Finished; ++i) {
        const std::uint8_t b = data[i];
        switch (st_) {
            case St::Seek:
                if (b == '{') { st_ = St::PointOrEnd; } else { ++discarded_; }
                break;
            case St::PointOrEnd:
                if (b == 'x') {
                    if (raw_.size() >= expected_) {
                        st_ = St::Finished;
                        status_ = FrameStatus::BadLength;   // more points than asked
                    } else {
                        st_ = St::Lsb;
                    }
                } else if (b == '}') {
                    st_ = St::Finished;
                    status_ = raw_.size() == expected_ ? FrameStatus::Done : FrameStatus::BadLength;
                } else {
                    st_ = St::Finished;
                    status_ = FrameStatus::BadFraming;
                }
                break;
            case St::Lsb: lsb_ = b; st_ = St::Msb; break;
            case St::Msb:
                raw_.push_back(static_cast<std::uint16_t>(lsb_ | (static_cast<unsigned>(b) << 8)));
                st_ = St::PointOrEnd;
                break;
            case St::Finished: break;
        }
    }
    return status_;
}

FrameStatus ScanrawParser::finish() {
    if (st_ == St::Finished) { return status_; }
    st_ = St::Finished;
    status_ = FrameStatus::Truncated;
    return status_;
}

bool decodeScanraw(const std::vector<std::uint16_t>& raw, Model model, double startHz,
                   double stopHz, core::SweepLine& out, std::string* whyNot) {
    auto fail = [&](const char* why) {
        if (whyNot != nullptr) { *whyNot = why; }
        return false;
    };
    const double off = levelOffsetDb(model);
    if (std::isnan(off)) { return fail("model not named"); }
    if (raw.size() < 2) { return fail("fewer than two points"); }
    core::SweepLine line;
    line.startHz = startHz;
    line.stopHz = stopHz;
    line.db.reserve(raw.size());
    for (std::uint16_t r : raw) {
        const double v = static_cast<double>(r) / 32.0 - off;
        if (v > kPlausibleMaxDb) { return fail("level above the sanity bound (wrong model or garbage)"); }
        line.db.push_back(static_cast<float>(v));
    }
    const std::string problem = core::sweepLineProblem(line);
    if (!problem.empty()) { return fail("line refused"); }
    out = std::move(line);
    return true;
}

SweepResult sweepOnce(SerialLink& link, const SweepRequest& req, core::SweepLine& out,
                      std::string* why) {
    auto note = [&](const char* s) { if (why != nullptr) { *why = s; } };
    std::string reason;
    const std::string cmd = scanrawCommand(req.startHz, req.stopHz, req.points, kOptionUnbuffered, &reason);
    if (cmd.empty()) { if (why != nullptr) { *why = reason; } return SweepResult::BadRequest; }
    if (std::isnan(levelOffsetDb(req.model))) { note("model not named"); return SweepResult::BadRequest; }
    if (!link.write(reinterpret_cast<const std::uint8_t*>(cmd.data()), cmd.size())) {
        note("write failed");
        return SweepResult::WriteFailed;
    }
    ScanrawParser parser(req.points);
    std::vector<std::uint8_t> buf(4096);
    int idle = 0;
    // Bound on total reads too, so a device that trickles one byte forever ends.
    const std::size_t maxReads = 2 * (req.points * 3 + 4096) + 1000;
    for (std::size_t reads = 0; reads < maxReads; ++reads) {
        const long n = link.read(buf.data(), buf.size());
        if (n < 0) { note("link failed"); return SweepResult::LinkFailed; }
        if (n == 0) {
            if (++idle > req.maxIdleReads) { break; }
            continue;
        }
        idle = 0;
        const FrameStatus st = parser.feed(buf.data(), static_cast<std::size_t>(n));
        if (st != FrameStatus::NeedMore) { break; }
    }
    switch (parser.status()) {
        case FrameStatus::Done: break;
        case FrameStatus::BadFraming: note("garbage inside the frame"); return SweepResult::Malformed;
        case FrameStatus::BadLength: note("frame length is not the points asked"); return SweepResult::Malformed;
        case FrameStatus::NeedMore:
        case FrameStatus::Truncated:
            parser.finish();
            note(parser.pointsSeen() == 0 && parser.discardedBytes() == 0 ? "no reply" : "reply cut short");
            return SweepResult::TimedOut;
    }
    core::SweepLine line;
    if (!decodeScanraw(parser.raw(), req.model, req.startHz, req.stopHz, line, &reason)) {
        if (why != nullptr) { *why = reason; }
        return SweepResult::Refused;
    }
    line.timeS = req.timeS;
    out = std::move(line);
    return SweepResult::Ok;
}

}  // namespace cascade::source::tinysa
