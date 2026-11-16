// send_flags.hpp - the flags every send() on a TCP socket passes.
//
// On POSIX a send() to a socket whose peer has gone raises SIGPIPE, and the default action ends
// the whole process: a radio server, a rotator, a CAT client or a broker that closes at the wrong
// moment would take the application with it (found on Linux, 2026-11-16: four radio-source tests
// died with SIGPIPE). MSG_NOSIGNAL turns that into an ordinary EPIPE failure return, which every
// caller already handles. Windows has no SIGPIPE and no such flag, so there it is 0.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#if !defined(_WIN32)
#include <sys/socket.h>
#endif

namespace cascade::net {

#if defined(MSG_NOSIGNAL)
inline constexpr int kSendFlags = MSG_NOSIGNAL;
#else
inline constexpr int kSendFlags = 0;
#endif

}  // namespace cascade::net
