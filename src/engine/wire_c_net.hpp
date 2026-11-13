// wire_c_net.hpp - the network listeners of wiring lane C that FOLLOW another program (row H6: RadioInfo from DXLab Commander and N1MM+). Receive only and DISPLAY only: what the logger
// says its radio is doing is shown as facts; nothing here retunes FoxSDR, answers the sender or sends a datagram. OFF by default; listens on the local machine (127.x) unless the
// person switches a wider address on, and a switched-off or unconfigured listener binds nothing. No thread: the control pass reads the waiting datagrams (a non-blocking socket).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/radioinfo_udp.hpp"
#include "net/radioinfo_listener.hpp"

namespace cascade::engine::wirec {

class RadioInfoFollow {
public:
    // Applies the settings: (re)opens or closes the socket. A port of 0 is "not configured" (nothing is opened: a random port is no use to a sender). `error` is "" when fine or
    // switched off. Returns true when the listener's state changed.
    bool configure(bool enabled, const std::string& address, int port, bool allowOther);
    // Reads what is waiting. True when anything a client can see changed (a radio's frequency, mode, transmit state, or going stale).
    bool poll(std::int64_t nowUnix);

    bool running() const { return listener_ && listener_->running(); }
    int boundPort() const { return listener_ ? listener_->port() : 0; }
    std::uint64_t accepted() const { return listener_ ? listener_->accepted() : acceptedBefore_; }
    std::uint64_t refused() const { return listener_ ? listener_->refused() : refusedBefore_; }
    const std::string& error() const { return error_; }
    // The published value of one radio (1..4): "" when never heard or stale. what: freq_hz, tx_hz, mode, split, transmitting, app.
    std::string value(int radio, const std::string& what) const;

private:
    std::string render(std::int64_t nowUnix) const;

    std::unique_ptr<cascade::net::RadioInfoListener> listener_;
    cascade::core::rig::RadioInfoTable table_{30};
    std::string error_;
    std::string lastRender_;
    std::int64_t nowUnix_ = 0;
    std::uint64_t acceptedBefore_ = 0, refusedBefore_ = 0;
};

}  // namespace cascade::engine::wirec
