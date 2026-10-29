// A short, bounded, DETERMINISTIC mutation pass over one fuzz target, as an ordinary test (plugin containment P2, docs/engine-stage4.md 6.29):
//
//   fuzz_smoke <target> <iterations> <seed> <file-or-directory>...
//
// WHY THIS EXISTS. fuzz_corpus_replay feeds a target the inputs somebody saved; libFuzzer finds new ones but needs Clang and a build of its own.
// This sits between: it takes the same corpus as seeds and, from a fixed random seed, makes `iterations` mutants of them - bit flips, bytes set
// to the values parsers trip on, inserted and deleted runs, a field overwritten with 0 / 1 / 0x7FFFFFFF / 0xFFFFFFFF, two seeds spliced, a cut -
// and hands each to the target. No coverage feedback, so it is a smoke test and not a fuzzer: it finds the shallow things (a count nobody
// checked, a type nobody tested) on every platform in the normal suite, in seconds, and the same run finds the same things every time. The
// long run is libFuzzer (docs/SANITIZERS-AND-FUZZING.md "Real fuzzing"); this is the label `fuzz_smoke`.
//
// A failing run is reproducible with the same arguments: the target either faults (the process dies at iteration N) or violates a property
// (FUZZ PROPERTY VIOLATED, abort). Progress is written to stderr every 1000 iterations, and FUZZ_SMOKE_SAVE=<file> keeps the input about to
// be run in that file (overwritten each time: slow, for chasing a failure), so the input that killed it is on disk.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

extern "C" {
#define CASCADE_FUZZ_TARGET(name) int cascade_fuzz_##name(const std::uint8_t*, std::size_t);
#include "fuzz_targets.inc"
#undef CASCADE_FUZZ_TARGET
}

namespace {

using Entry = int (*)(const std::uint8_t*, std::size_t);
struct Target {
    const char* name;
    Entry entry;
};
const Target kTargets[] = {
#define CASCADE_FUZZ_TARGET(name) {#name, &cascade_fuzz_##name},
#include "fuzz_targets.inc"
#undef CASCADE_FUZZ_TARGET
};

// xorshift64*: the same stream on every platform and compiler.
struct Rng {
    std::uint64_t s;
    std::uint64_t next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 2685821657736338717ull;
    }
    std::size_t below(std::size_t n) { return n == 0 ? 0 : static_cast<std::size_t>(next() % n); }
};

using Bytes = std::vector<std::uint8_t>;

void collect(const fs::path& p, std::vector<fs::path>& out) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
        std::vector<fs::path> here;
        for (const fs::directory_entry& e : fs::recursive_directory_iterator(p, ec)) {
            if (e.is_regular_file(ec)) { here.push_back(e.path()); }
        }
        std::sort(here.begin(), here.end());
        out.insert(out.end(), here.begin(), here.end());
    } else if (fs::is_regular_file(p, ec)) {
        out.push_back(p);
    }
}

const std::uint32_t kInteresting[] = {0u,          1u,          2u,          7u,          8u,          63u,         64u,         65u,
                                      127u,        128u,        255u,        256u,        1000u,       4095u,       4096u,       4097u,
                                      65535u,      65536u,      0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFEu, 0xFFFFFFFFu, 0x20000000u, 0x00FFFFFFu};

void mutate(Bytes& b, const std::vector<Bytes>& seeds, Rng& rng) {
    const unsigned rounds = 1 + static_cast<unsigned>(rng.below(4));
    for (unsigned r = 0; r < rounds; ++r) {
        switch (rng.below(10)) {
            case 0:   // flip a bit
                if (!b.empty()) { b[rng.below(b.size())] ^= static_cast<std::uint8_t>(1u << rng.below(8)); }
                break;
            case 1:   // set a byte to something a parser trips on
                if (!b.empty()) {
                    static const std::uint8_t v[] = {0x00, 0x01, 0x7F, 0x80, 0xFF, '\n', '"', '{', '}', '[', ']', '\\', ':', ',', '0', '9', '-'};
                    b[rng.below(b.size())] = v[rng.below(sizeof(v))];
                }
                break;
            case 2: {   // overwrite a 32-bit field with an interesting number
                if (b.size() >= 4) {
                    const std::size_t at = rng.below(b.size() - 3);
                    const std::uint32_t v = kInteresting[rng.below(sizeof(kInteresting) / sizeof(kInteresting[0]))];
                    for (int i = 0; i < 4; ++i) { b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i)); }
                }
                break;
            }
            case 3: {   // insert a run of random bytes
                const std::size_t at = rng.below(b.size() + 1);
                const std::size_t n = 1 + rng.below(16);
                Bytes run(n);
                for (auto& x : run) { x = static_cast<std::uint8_t>(rng.next()); }
                b.insert(b.begin() + static_cast<std::ptrdiff_t>(at), run.begin(), run.end());
                break;
            }
            case 4:   // delete a run
                if (!b.empty()) {
                    const std::size_t at = rng.below(b.size());
                    const std::size_t n = std::min<std::size_t>(1 + rng.below(32), b.size() - at);
                    b.erase(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + n));
                }
                break;
            case 5:   // duplicate a run
                if (!b.empty() && b.size() < (1u << 20)) {
                    const std::size_t at = rng.below(b.size());
                    const std::size_t n = std::min<std::size_t>(1 + rng.below(64), b.size() - at);
                    const Bytes run(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + n));
                    b.insert(b.begin() + static_cast<std::ptrdiff_t>(at), run.begin(), run.end());
                }
                break;
            case 6: {   // splice with another seed
                const Bytes& other = seeds[rng.below(seeds.size())];
                if (!other.empty() && !b.empty()) {
                    const std::size_t from = rng.below(other.size());
                    const std::size_t n = std::min<std::size_t>(1 + rng.below(128), other.size() - from);
                    const std::size_t at = rng.below(b.size() + 1);
                    b.insert(b.begin() + static_cast<std::ptrdiff_t>(at), other.begin() + static_cast<std::ptrdiff_t>(from),
                             other.begin() + static_cast<std::ptrdiff_t>(from + n));
                }
                break;
            }
            case 7:   // cut
                if (!b.empty()) { b.resize(rng.below(b.size())); }
                break;
            case 8:   // arithmetic on a byte
                if (!b.empty()) { b[rng.below(b.size())] = static_cast<std::uint8_t>(b[rng.below(b.size())] + static_cast<std::uint8_t>(rng.below(9)) - 4); }
                break;
            default:   // overwrite a run with one byte
                if (!b.empty()) {
                    const std::size_t at = rng.below(b.size());
                    const std::size_t n = std::min<std::size_t>(1 + rng.below(24), b.size() - at);
                    std::fill(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + n), static_cast<std::uint8_t>(rng.next()));
                }
                break;
        }
        if (b.size() > (1u << 20)) { b.resize(1u << 20); }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s <target> <iterations> <seed> <file-or-directory>...\n", argv[0]);
        return 2;
    }
    const Target* target = nullptr;
    for (const Target& t : kTargets) {
        if (std::strcmp(t.name, argv[1]) == 0) { target = &t; }
    }
    if (target == nullptr) {
        std::fprintf(stderr, "no fuzz target named \"%s\"\n", argv[1]);
        return 2;
    }
    const std::uint64_t iterations = std::strtoull(argv[2], nullptr, 10);
    Rng rng{std::strtoull(argv[3], nullptr, 10) * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull};

    std::vector<fs::path> files;
    for (int i = 4; i < argc; ++i) { collect(fs::path(argv[i]), files); }
    std::vector<Bytes> seeds;
    for (const fs::path& p : files) {
        std::ifstream f(p, std::ios::binary);
        const std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        seeds.emplace_back(data.begin(), data.end());
    }
    if (seeds.empty()) {
        std::fprintf(stderr, "FAIL %s: no corpus files found under the given paths\n", target->name);
        return 2;
    }
    const char* save = std::getenv("FUZZ_SMOKE_SAVE");

    const auto t0 = std::chrono::steady_clock::now();
    std::uint64_t bytes = 0;
    for (std::uint64_t i = 0; i < iterations; ++i) {
        Bytes input = seeds[rng.below(seeds.size())];
        if (i >= seeds.size()) { mutate(input, seeds, rng); }   // the first pass over the seeds runs them as they are
        Bytes exact(input);   // a buffer of exactly its size: an over-read must reach the allocator's red zone
        bytes += exact.size();
        if (save != nullptr) {
            std::ofstream out(save, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(exact.data()), static_cast<std::streamsize>(exact.size()));
        }
        if (i % 1000 == 0 && i != 0) { std::fprintf(stderr, "fuzz_smoke_%s: iteration %llu\n", target->name, static_cast<unsigned long long>(i)); }
        target->entry(exact.data(), exact.size());
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("fuzz_smoke_%s: %llu inputs from %zu seeds (%llu bytes), %.1f s, no fault\n", target->name, static_cast<unsigned long long>(iterations),
                seeds.size(), static_cast<unsigned long long>(bytes), secs);
    return 0;
}
