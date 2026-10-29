// Fuzz target: a file in the plugins folder, judged BEFORE any of it runs (plugin containment P2, docs/engine-stage4.md 6.29; the check is P1's,
// core/plugin_module_check.hpp, 6.28 rule 16).
//
// The load-time validation reads a stranger's bytes - a PE's DOS and NT headers, its section table and export directory, an ELF's program
// headers and dynamic symbols - and decides whether to hand them to the operating system's loader at all. It is the first code to touch a file
// that may be a truncation, a corruption, a different processor's build, or a planted nothing; if IT faults, the loader is never reached and
// the application is lost to a file that was only ever going to be refused. Every structure's offset and count is the file's own claim.
//
// THE PROPERTIES: whatever the bytes, for every image format and every machine a host could be: the verdict is one of the closed words, an
// image the check accepts is at least as long as the magic it was recognised by, and a refusal carries a sentence for a person. The same bytes
// read back through the file reader (the product's path) get the verdict the byte check gives. A file NAME that declares an ABI never breaks the
// reader of names.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "core/plugin_module_check.hpp"
#include "fuzz_common.hpp"

using cascade::core::ImageFormat;
using cascade::core::ModuleCheck;
using cascade::core::ModuleVerdict;

namespace {

void requireWellFormed(const ModuleCheck& r, std::size_t size) {
    const char* word = cascade::core::moduleVerdictWord(r.verdict);
    FUZZ_REQUIRE(word != nullptr && word[0] != '\0');
    if (r.ok()) {
        FUZZ_REQUIRE(size >= 4);   // an image was recognised by a magic of at least four bytes
    } else {
        FUZZ_REQUIRE(!r.detail.empty());
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::uint32_t machines[] = {0x8664u, 0xAA64u, 0x14Cu, 62u, 183u, 3u, 0u, 0xFFFFu};
    for (const ImageFormat fmt : {ImageFormat::Pe, ImageFormat::Elf, ImageFormat::MachO}) {
        for (const std::uint32_t machine : machines) { requireWellFormed(cascade::core::checkModuleBytesFor(fmt, machine, data, size), size); }
    }
    const ModuleCheck own = cascade::core::checkModuleBytes(data, size);
    requireWellFormed(own, size);

    // the same bytes as a file (a name that declares no ABI): the product's path agrees with the byte check
    const std::string path = fuzz::writeScratch("module.bin", data, size);
    const ModuleCheck viaFile = cascade::core::checkModuleFile(path);
    if (size > 0) { FUZZ_REQUIRE(viaFile.verdict == own.verdict); }
    requireWellFormed(viaFile, size);

    // and the bytes as a file NAME
    (void)cascade::core::abiDeclaredByFileName(fuzz::asString(data, size < 300 ? size : 300));
    return 0;
}
