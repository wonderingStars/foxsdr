// A RECEIVER: everything that is ONE radio of the engine (multi-radio engine, stage 1; docs/multi-radio-records/engine-stages-1-3.md).
//
// The engine used to hold one receiver as about 180 loose data members: its Pipeline, its device and the open / fault / retry machinery around it, the
// settings it mirrors, its recorders, its health counters and its published state. This header is where they live now. Engine holds
// `receivers_`, an array of kMaxReceivers slots; slot 0 always exists and is the receiver every client has always used. The other slots stay empty
// until stage 4 (RADIO_ADD).
//
// THE MOVE IS PURE. Each member was moved with its declaration, its initialiser and its comment unchanged, and Engine keeps a reference of the same
// name bound to slot 0 (`float& volume_ = receivers_[0]->volume_;`), so every function of the engine, the friend classes and the tests read and write
// exactly what they did. The data has one home (here); the engine's own code is converted to name a slot only where a command or a read is
// addressed to one (stage 3 onwards). Do not add a member to Engine for a thing that is one radio's: add it here.
//
// THE ORDER OF THE MEMBERS IS THE ORDER OF DESTRUCTION: the Pipeline (and the threads under it) is declared first, so it is destroyed last, after the
// open future and the other state that reads it, exactly as it was in Engine.
//
// Nothing in this header locks or starts anything: it is data. The threads are the Pipeline's.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/audio_open.hpp"
#include "core/noise_floor_tracker.hpp"
#include "core/pipeline.hpp"
#include "core/receiver_snapshot.hpp"
#include "core/recorder.hpp"
#include "core/record_finish.hpp"
#include "core/retune_coalescer.hpp"
#include "engine/engine_types.hpp"
#include "engine/record_start.hpp"
#include "source/soapy_source.hpp"

namespace cascade::engine {

// THE LIMIT OF THIS BUILD: how many receivers the engine can hold, and (the owner's rule, stage 0 measurement 2026-11-10) how many radios may be open
// in total, patch radios included. ONE constant: every structure that is per radio is sized by it, `FoxCommand.target` is a number, the facts are keyed
// by number and a client reads `fact.radio.max`. Nothing may assume four.
inline constexpr std::size_t kMaxReceivers = 8;

struct Receiver {
    explicit Receiver(cascade::core::Pipeline::Config cfg) : pipeline_(cfg) {}
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    // The slot table for a new engine: slot 0 built, the rest empty.
    static std::array<std::unique_ptr<Receiver>, kMaxReceivers> makeSlots(cascade::core::Pipeline::Config cfg) {
        std::array<std::unique_ptr<Receiver>, kMaxReceivers> slots;
        slots[0] = std::make_unique<Receiver>(cfg);
        return slots;
    }


    // ---- pipeline ----
    // DSP pipeline plus the two live display widgets it feeds. The views are
    // held by unique_ptr for two reasons: the forward declarations above, and
    // the waterfall's GL texture, whose deletion needs the creating GL context
    // current - run() tears the view down explicitly before destroying the
    // context, because AppWindow itself outlives it (destroyed in main()).
    cascade::core::Pipeline pipeline_;
};  // struct Receiver

}  // namespace cascade::engine
