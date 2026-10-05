// Fuzz target: the update service's answer (https://.../api/update).
//
// The application asks once at start-up and acts on the reply by downloading
// and offering to run an installer, so parseUpdateManifest is the gate between
// a web server's JSON and "run this file". It is called from a worker thread
// whose result the GUI thread collects with future::get() - an exception
// leaving the parser is not caught anywhere on the way, so the property that
// matters most here is that NO input makes it throw.
//
// Properties: a document that is refused changes nothing a caller could act on
// (the UpdateInfo is empty) and says why; an accepted one names a well-formed
// version, a 64-digit hash, and a URL on the allowed host.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/updater.hpp"
#include "fuzz_common.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    fuzz::Bytes in(data, size);
    // The running version is chosen by the input too, as its first line.
    const std::size_t cut = in.u8() % 24;
    const std::string current = in.str(cut);
    const std::string text = in.rest();

    cascade::core::UpdateInfo info;
    std::string error;
    if (!cascade::core::parseUpdateManifest(text, current, info, error)) {
        FUZZ_REQUIRE(!error.empty());
        FUZZ_REQUIRE(info.version.empty() || !info.newer);
        return 0;
    }
    FUZZ_REQUIRE(cascade::core::wellFormedVersion(info.version));
    FUZZ_REQUIRE(info.sha256.size() == 64);
    FUZZ_REQUIRE(info.url.rfind("https://", 0) == 0);
    FUZZ_REQUIRE(!info.critical || info.newer);
    return 0;
}
