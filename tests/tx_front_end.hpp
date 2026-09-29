// tx_front_end.hpp - THE WINDOW'S HALF OF THE KEY, as the window does it, for
// the transmit tests that drive a real Engine (engine stage 4, API 0.2's THE
// KEY): a LOCAL session on the engine's own API host, its heartbeat, the
// state read the page is drawn from, the page's frame turned into commands by
// gui::txKeyCommands (the window's own function, gui/transmit_page.hpp),
// submitted as one batch, the answers drained. AppWindow::pumpTransmitKey is
// this, with the page's controls in place of the arguments.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>

#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "foxsdr_api.h"
#include "gui/transmit_page.hpp"

namespace tx_test {

struct FrontEnd {
    explicit FrontEnd(cascade::engine::Engine& e, const char* name = "transmit test front end")
        : api(cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR)) {
        FoxSessionParams p{};
        p.structSize = sizeof(p);
        p.flags = FOXAPI_SESSION_LOCAL;
        p.grants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TRANSMIT;
        p.clientName = name;
        api->open_session(e.apiHandle(), &p, &s);
        st.structSize = sizeof(st);
    }
    ~FrontEnd() { close(); }
    FrontEnd(const FrontEnd&) = delete;
    FrontEnd& operator=(const FrontEnd&) = delete;

    void close() {
        if (s != nullptr) {
            api->close_session(s);
            s = nullptr;
        }
    }
    void beat() { api->heartbeat(s); }

    // One frame: the heartbeat, the state read (as the page is drawn from
    // it), the page's key submitted. armRequest is the "Allow remote
    // transmit" switch clicked this frame: -1 not clicked, 0 off, 1 on.
    void frame(bool pageLive, bool pttHeld, bool latchPressed, bool doBeat = true, int armRequest = -1) {
        if (doBeat) { beat(); }
        FoxReceiverState now{};
        now.structSize = sizeof(now);
        if (api->read_state(s, &now) == FOXAPI_OK) { st = now; }
        cascade::gui::TxKeyFrame f;
        f.pageLive = pageLive;
        f.pttHeld = pttHeld;
        f.latchPressed = latchPressed;
        f.armRequest = armRequest;
        f.latchedShown = (st.flags & FOXAPI_RX_TX_LATCHED) != 0u;
        f.latchMine = f.latchedShown && (st.flags & FOXAPI_RX_TX_KEY_MINE) != 0u;
        FoxCommand c[cascade::gui::kTxKeyMaxCommands];
        const std::uint32_t n = cascade::gui::txKeyCommands(f, mem, c);
        if (n > 0) { api->submit(s, c, n, nullptr); }
        drain();
    }
    void drain() {
        FoxCommandResult r[16];
        for (;;) {
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = api->poll_results(s, r, 16);
            if (got < 16) { break; }
        }
    }

    const FoxEngineApi* api;
    FoxSession* s = nullptr;
    FoxReceiverState st{};
    cascade::gui::TxKeyMemory mem;
};

}  // namespace tx_test
