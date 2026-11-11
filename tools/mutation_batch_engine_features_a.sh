#!/bin/bash
# The break-it checks of the engine features lane A (docs/multi-radio-records/engine-features-a.md): each line mutates ONE guarantee and names the tests that must go red.
M=C:/Users/steve/foxsdr-wt/mr-engine1/tools/mutation_check.sh
# WebSocket layer: an unmasked client frame accepted
$M M01 src/net/ws_frame.cpp 's|if ((b1 & 0x80) == 0) {|if (false \&\& (b1 \& 0x80) == 0) {|' "test_ws_frame test_tci_server" "test_ws_frame test_tci_server"
# TCI: a transmitter op on the TCI session's profile
$M M02 src/engine/fox_engine_api.cpp 's|    FOXAPI_OP_SET_BANDWIDTH, FOXAPI_OP_SET_SQUELCH,|    FOXAPI_OP_SET_BANDWIDTH, FOXAPI_OP_SET_SQUELCH, FOXAPI_OP_TX_PTT,|' "test_tci_engine" "test_tci_engine"
# TCI: TRX true answered true
$M M03 src/net/tci_protocol.cpp 's|replies.push_back(name + ":" + std::to_string(indexOf(r)) + ",false;");|replies.push_back(name + ":" + std::to_string(indexOf(r)) + ",true;");|' "test_tci_protocol test_tci_server test_tci_engine" "test_tci_protocol test_tci_server test_tci_engine"
# TCI: the bind scope ignored (always every interface)
$M M04 src/net/tci_server.cpp 's|bindAll ? ::htonl(INADDR_ANY) : ::htonl(INADDR_LOOPBACK)|::htonl(INADDR_ANY)|' "test_tci_server" "test_tci_server"
# TCI: a client that never reads is never dropped
$M M05 src/net/tci_server.hpp 's|kTciStallMs = 3000;|kTciStallMs = 3000000;|; s|kMaxTciOutBytes = 1u << 20;|kMaxTciOutBytes = 1u << 30;|' "test_tci_server" "test_tci_server"
# TCI: the client cap not enforced
$M M06 src/net/tci_server.cpp 's|>= kMaxTciClients) {|>= 1000) {|' "test_tci_server test_tci_engine" "test_tci_server"
# TCI: the exclusive bind replaced by the sharing one
$M M07 src/net/tci_server.cpp 's|SO_EXCLUSIVEADDRUSE,|SO_REUSEADDR,|' "test_tci_server" "test_tci_server"
# TCI: a pass-through that echoes nothing the client set (the forced echo dropped)
$M M08 src/net/tci_protocol.cpp 's#if (f != forced_.end()) { g |= f->second; forced_.erase(f); }#if (f != forced_.end()) { forced_.erase(f); }#' "test_tci_protocol test_tci_server" "test_tci_protocol"
# UDP: the packet size ignored
$M M09 src/net/udp_audio.cpp 's|const std::size_t pf = static_cast<std::size_t>(packetFrames_);|const std::size_t pf = 480;|' "test_udp_audio test_tci_engine" "test_udp_audio"
# schedule: a start more than 30 s late is still started
$M M10 src/core/schedule.hpp 's|kGraceS = 30;|kGraceS = 100000;|' "test_schedule test_engine_api_schedule" "test_schedule"
# schedule: the handled occurrence is not remembered (a restart or a clock set back replays it)
$M M11 src/core/schedule.cpp 's|^        e.lastStartUnix = latest;|        |' "test_schedule test_engine_api_schedule" "test_schedule"
# schedule: the clock set back no longer ends a recording
$M M12 src/core/schedule.cpp 's|} else if (nowMs < r.startedMs - kBackStepMs) {|} else if (false) {|' "test_schedule test_engine_api_schedule" "test_schedule"
# engine: the entry's radio ignored (everything happens on radio 1)
$M M13 src/engine/engine_schedule.cpp 's|const std::size_t slot = static_cast<std::size_t>(e.radio) - 1u;|const std::size_t slot = 0;|; s|const std::uint32_t radio = static_cast<std::uint32_t>(e.radio);|const std::uint32_t radio = 1;|' "test_engine_api_schedule" "test_engine_api_schedule:two"
# engine: the tuning is not put back
$M M14 src/engine/engine_schedule.cpp 's|if (rt.restore && rt.tuned) {|if (false) {|' "test_engine_api_schedule" "test_engine_api_schedule:one"
# engine: overlap on a radio not refused
$M M15 src/engine/engine_schedule.cpp 's|if (other.enabled && e.enabled && sched::overlaps(e, other, nowS)) {|if (false) {|' "test_engine_api_schedule" "test_engine_api_schedule:rules"
# engine: a refused start (radio stopped / busy) is not checked
$M M16 src/engine/engine_schedule.cpp 's|    if (rx().deviceOpenPending_) { return who + " is still opening its device"; }|    if (false) { return who; }|; s|    if ((p.rx.flags & FOXAPI_RX_RUNNING) == 0u) {|    if (false) {|' "test_engine_api_schedule" "test_engine_api_schedule:refusals"
# plugin hand-off: a span that ends before it starts accepted
$M M17 src/core/plugin_api.cpp 's|if (x.losUnixMs <= x.aosUnixMs) { return CASCADE_API_BAD_ARGUMENT; }||' "test_plugin_schedule_hint" "test_plugin_schedule_hint"
# plugin hand-off: the sixteen-per-plugin limit not kept
$M M18 src/core/plugin_api.cpp 's|if (st.hints.size() >= CASCADE_MAX_SCHEDULE_HINTS_PER_PLUGIN) { return CASCADE_API_LIMIT; }||' "test_plugin_schedule_hint" "test_plugin_schedule_hint"
echo MUTATIONS_DONE > D:/foxsdr-build/mr-engine1/lane/mut-all.done
