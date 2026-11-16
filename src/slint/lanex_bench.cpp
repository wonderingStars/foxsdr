// lanex_bench.cpp - see lanex_bench.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "lanex_bench.hpp"

#include <cstdio>
#include <exception>
#include <utility>

#include "core/i18n.hpp"
#include "core/utf8_text.hpp"

namespace foxsdr::slintlanex {

BenchJob::BenchJob(std::function<BenchItem(std::size_t)> run, std::size_t count) : run_(std::move(run)), count_(count) {}

BenchJob::~BenchJob() {
    cancel();
    if (th_.joinable()) { th_.join(); }
}

bool BenchJob::start() {
    if (state_.load() == BenchState::Running) { return false; }
    if (th_.joinable()) { th_.join(); }   // the previous run has ended (its state is not Running): reap it
    {
        std::lock_guard<std::mutex> lk(mu_);
        results_.clear();
    }
    done_.store(0);
    cancel_.store(false);
    state_.store(BenchState::Running);
    th_ = std::thread([this] { work(); });
    return true;
}

void BenchJob::cancel() { cancel_.store(true); }

void BenchJob::work() {
    for (std::size_t i = 0; i < count_; ++i) {
        if (cancel_.load()) {
            state_.store(BenchState::Cancelled);
            return;
        }
        BenchItem it;
        try {
            it = run_ ? run_(i) : BenchItem{};
        } catch (const std::exception& e) {
            it.ok = false;
            it.error = e.what();
        } catch (...) {
            it.ok = false;
            it.error = "unknown error";
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            results_.push_back(std::move(it));
        }
        done_.store(i + 1);
    }
    state_.store(cancel_.load() && done_.load() < count_ ? BenchState::Cancelled : BenchState::Done);
}

std::vector<BenchItem> BenchJob::results() const {
    std::lock_guard<std::mutex> lk(mu_);
    return results_;
}

NumberWriter plainNumbers() {
    return [](double v, int decimals) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.*f", decimals < 0 ? 0 : decimals, v);
        return std::string(buf);
    };
}

std::string itemText(const BenchItem& it, const NumberWriter& num) {
    if (!it.ok) { return cascade::core::formatText(cascade::i18n::tr("failed: %s"), it.error.empty() ? "?" : it.error.c_str()); }
    return cascade::core::formatText(cascade::i18n::tr("%s million samples a second, %s times real time"), num(it.samplesPerSec / 1e6, 1).c_str(), num(it.realTimeMultiple, 1).c_str());
}

std::string progressText(std::size_t done, std::size_t total) {
    return cascade::core::formatText(cascade::i18n::tr("%d of %d"), static_cast<int>(done), static_cast<int>(total));
}

std::string summaryText(const std::vector<BenchItem>& items, const NumberWriter& num) {
    if (items.empty()) { return std::string(); }
    int failed = 0;
    const BenchItem* slowest = nullptr;
    for (const BenchItem& it : items) {
        if (!it.ok) { ++failed; continue; }
        if (!slowest || it.realTimeMultiple < slowest->realTimeMultiple) { slowest = &it; }
    }
    std::string s = cascade::core::formatText(cascade::i18n::tr("%d items timed, %d failed."), static_cast<int>(items.size()), failed);
    if (slowest) {
        s += ' ';
        s += cascade::core::formatText(cascade::i18n::tr("The slowest is %s at %s times real time."), (slowest->group + " " + slowest->name).c_str(), num(slowest->realTimeMultiple, 1).c_str());
    }
    return s;
}

}  // namespace foxsdr::slintlanex
