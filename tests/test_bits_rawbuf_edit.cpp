// Bit-view library: the raw-slice ring and re-decode chain (rawbuf.hpp), the code tables, the user table
// parser and the binary editor model (bitedit.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <cstdio>
#include <thread>

#include "bits_test_util.hpp"
#include "core/bits/bitedit.hpp"
#include "core/bits/rawbuf.hpp"
#include "test_check.hpp"

using namespace cascade::bits;
using bitstest::Rng;

namespace {
Bits B(const char* s) { return fromString(s); }

RawSlice sliceOf(const Bits& data, LineCode lc, Rng& rng, double jitter = 0.1, double bias = 0.0) {
    RawSlice s;
    s.runs = bitstest::chipsToRuns(encodeLineCode(data, lc), 400.0, 0.01, jitter, rng);
    for (auto& r : s.runs) r.duration += r.level ? -bias : bias;
    s.label = "burst";
    return s;
}
}  // namespace

int main() {
    Rng rng(0xBEEF);

    // ---- the ring: bounded by count, by total runs, refuses an oversized slice, ids rise, thread safe ---------------------
    {
        RawRing ring({4, 1000});
        uint64_t last = 0;
        for (int i = 0; i < 10; ++i) {
            RawSlice s;
            s.runs.assign(10, LevelRun{1, 100});
            const uint64_t id = ring.push(std::move(s));
            CHECK(id > last);
            last = id;
        }
        CHECK(ring.size() == 4 && ring.evicted() == 6);
        RawSlice got;
        CHECK(!ring.get(1, &got) && ring.get(last, &got) && got.id == last);
        // the oldest four ids survive: 7..10
        const auto snap = ring.snapshot();
        CHECK(snap.size() == 4 && snap[0].id == 7 && snap[3].id == 10);
        // total-run bound: a 600-run slice pushes out older ones; one of 1001 is refused whole
        RawSlice big;
        big.runs.assign(600, LevelRun{0, 5});
        CHECK(ring.push(big) != 0);
        CHECK(ring.used() <= 1000);
        RawSlice huge;
        huge.runs.assign(2000, LevelRun{0, 5});
        const size_t before = ring.size();
        CHECK(ring.push(huge) == 0 && ring.refused() == 1 && ring.size() == before);
        // label cut at 32
        RawSlice lab;
        lab.runs.assign(2, LevelRun{1, 1});
        lab.label = std::string(80, 'x');
        const uint64_t lid = ring.push(lab);
        CHECK(ring.get(lid, &got) && got.label.size() == 32);
        ring.clear();
        CHECK(ring.size() == 0 && ring.used() == 0);
        // concurrency: four threads push and snapshot; the bounds hold and every id is unique
        RawRing shared({16, 4000});
        std::atomic<bool> ok{true};
        std::vector<std::thread> th;
        for (int t = 0; t < 4; ++t)
            th.emplace_back([&] {
                for (int i = 0; i < 500; ++i) {
                    RawSlice s;
                    s.runs.assign(1 + (i % 50), LevelRun{1, 10});
                    shared.push(std::move(s));
                    const auto sn = shared.snapshot();
                    if (sn.size() > 16) ok = false;
                    for (size_t k = 1; k < sn.size(); ++k)
                        if (sn[k].id <= sn[k - 1].id) ok = false;
                }
            });
        for (auto& x : th) x.join();
        CHECK(ok.load() && shared.size() <= 16 && shared.used() <= 4000);
    }

    // ---- re-decode with different settings after the fact ----------------------------------------------------------------------
    {
        const Bits sync = B("1110 1001 0110 1100");
        Bits data = sync;
        const Bits payload = bitstest::randomBits(rng, 48);
        data.insert(data.end(), payload.begin(), payload.end());
        RawSlice s = sliceOf(data, LineCode::ManchesterIeee, rng, 0.1, 80.0);
        // wrong settings first: NRZ-L reads it as noise, no sync found
        ChainSettings wrong;
        wrong.stages = {LineCode::NrzL};
        wrong.sync = sync;
        auto r1 = redecode(s, wrong);
        CHECK(r1.ok && r1.syncHits.empty());
        // the right line code finds it, from the SAME stored slice
        ChainSettings right;
        right.stages = {LineCode::ManchesterIeee};
        right.sync = sync;
        right.framing = FramingKind::Fixed;
        right.fixedBits = 48;
        auto r2 = redecode(s, right);
        CHECK(r2.ok && r2.syncHits.size() == 1 && r2.frames.size() == 1 && r2.frames[0].bits == payload && r2.violations == 0);
        CHECK(std::fabs(r2.chipUs - 400.0 * 1.01) < 15.0);
        // inverted line: found only with invert (or with both polarities)
        RawSlice si = s;
        for (auto& r : si.runs) r.level ^= 1;
        auto r3 = redecode(si, right);
        CHECK(r3.ok && r3.frames.empty());
        ChainSettings inv = right;
        inv.invert = true;
        CHECK(redecode(si, inv).frames.size() == 1);
        ChainSettings both = right;
        both.syncBothPolarities = true;
        CHECK(redecode(si, both).syncHits.size() == 1);
        // a fixed chip period is honoured, and a wrong one reads nonsense
        ChainSettings fixedT = right;
        fixedT.chipUs = 404.0;
        CHECK(redecode(s, fixedT).frames.size() == 1);
        fixedT.chipUs = 900.0;
        CHECK(redecode(s, fixedT).frames.empty());
        // whitening applied after the line decode: whiten the data with PN9, the chain removes it
        Bits white = whitenBits(data, kPn9, kPn9Seed);
        RawSlice sw = sliceOf(white, LineCode::ManchesterIeee, rng, 0.05);
        ChainSettings wset = right;
        wset.whiten.kind = WhitenKind::AdditiveBits;
        wset.whiten.lfsr = kPn9;
        wset.whiten.seed = kPn9Seed;
        auto r4 = redecode(sw, wset);
        CHECK(r4.frames.size() == 1 && r4.frames[0].bits == payload);
        CHECK(redecode(sw, right).frames.empty());
        // the length framing and the flag framing paths
        Bits lenFrame = sync;
        for (int k = 7; k >= 0; --k) lenFrame.push_back(static_cast<uint8_t>((6 >> k) & 1u));  // length 6 bytes = 48 bits counting itself
        const Bits body = bitstest::randomBits(rng, 40);
        lenFrame.insert(lenFrame.end(), body.begin(), body.end());
        ChainSettings byLen = right;
        byLen.framing = FramingKind::Length;
        auto r5 = redecode(sliceOf(lenFrame, LineCode::ManchesterIeee, rng), byLen);
        CHECK(r5.frames.size() == 1 && r5.frames[0].bits.size() == 48);
        ChainSettings byFlag;
        byFlag.stages = {LineCode::ManchesterIeee};
        byFlag.framing = FramingKind::Flag;
        const Bits fl = stuffFrame(B("1111 1111 1010 0000 0000 1111 1111 1"), byFlag.flag);
        auto r6 = redecode(sliceOf(fl, LineCode::ManchesterIeee, rng), byFlag);
        CHECK(r6.frames.size() == 1 && r6.frames[0].bits == B("1111 1111 1010 0000 0000 1111 1111 1"));
        // no sync, no framing: the whole stream is one frame
        ChainSettings plain;
        plain.stages = {LineCode::ManchesterIeee};
        CHECK(redecode(s, plain).frames.size() == 1);
        // failure paths: empty slice, no stages, too many stages, too few runs, impossible phase
        CHECK(!redecode(RawSlice{}, plain).ok);
        ChainSettings none;
        none.stages = {};
        CHECK(!redecode(s, none).ok && !redecode(s, none).problem.empty());
        ChainSettings four;
        four.stages = {LineCode::NrzL, LineCode::NrzL, LineCode::NrzL, LineCode::NrzL};
        CHECK(!redecode(s, four).ok);
        RawSlice two;
        two.runs = {{1, 100}, {0, 100}};
        CHECK(!redecode(two, plain).ok || redecode(two, plain).frames.size() <= 1);
        ChainSettings ph = plain;
        ph.phase = 1;
        CHECK(redecode(s, ph).ok);  // a Manchester phase of 1 is valid
        // chips given directly
        RawSlice chipSlice;
        chipSlice.chips = encodeLineCode(data, LineCode::ManchesterIeee);
        chipSlice.chipUs = 400;
        CHECK(redecode(chipSlice, right).frames.size() == 1);
        // redecodeAll over a ring, and the sweep
        RawRing ring;
        for (int i = 0; i < 5; ++i) ring.push(sliceOf(data, LineCode::ManchesterIeee, rng));
        ring.push(RawSlice{});  // an empty slice: counts as failed
        const auto all = redecodeAll(ring, right);
        CHECK(all.slicesOk == 5 && all.slicesFailed == 1 && all.frames.size() == 5 && all.sliceIds.size() == 5);
        const auto rows = sweepSettings(s, right, {LineCode::NrzL, LineCode::ManchesterIeee, LineCode::BiphaseMark, LineCode::Miller});
        CHECK(!rows.empty() && rows[0].code == LineCode::ManchesterIeee && !rows[0].invert && rows[0].syncHits == 1);
    }

    // ---- built-in code tables ---------------------------------------------------------------------------------------------------
    {
        // ASCII-7
        const Bits hello = fromBytes({'H' & 0x7F, 'i' & 0x7F});
        Bits h7;
        for (uint8_t c : {'H', 'i'}) for (int i = 6; i >= 0; --i) h7.push_back(static_cast<uint8_t>((c >> i) & 1u));
        CHECK(decodeText(h7, tableAscii7()).text == "Hi");
        (void)hello;
        CHECK(tableAscii7().planes[0][10] == "<LF>" && tableAscii7().planes[0][127] == "<DEL>" && tableAscii7().planes[0][0x20] == " ");
        CHECK(decodeText(fromBytes({'O', 'K', 0x01}), tableAscii8()).text == "OK.");
        CHECK(tableIta5().name == "ITA5" && tableIta5().planes[0]['A'] == "A");
        // SIXBIT: code + 0x20
        auto six = [](std::initializer_list<unsigned> v) { Bits b; for (unsigned x : v) for (int i = 5; i >= 0; --i) b.push_back(static_cast<uint8_t>((x >> i) & 1u)); return b; };
        CHECK(decodeText(six({'H' - 0x20, 'I' - 0x20}), tableSixbit()).text == "HI");
        // AIS 6-bit: 0 = '@', 1 = 'A', 32 = ' ', 48 = '0', 63 = '?'
        CHECK(decodeText(six({0, 1, 32, 48, 63}), tableAis6()).text == "@A 0?");
        // ITA2: the sequence from the opened table; shifts switch planes and are not printed
        auto five = [](std::initializer_list<unsigned> v) { Bits b; for (unsigned x : v) for (int i = 4; i >= 0; --i) b.push_back(static_cast<uint8_t>((x >> i) & 1u)); return b; };
        // LTRS state: R=0x0A, Y=0x15, FIGS=0x1B, 3=0x01, 4=0x0A, LTRS=0x1F, E=0x01
        CHECK(decodeText(five({0x0A, 0x15, 0x1B, 0x01, 0x0A, 0x1F, 0x01}), tableIta2()).text == "RY34E");
        CHECK(decodeText(five({0x1B, 0x09, 0x05}), tableIta2(true)).text == "$<BEL>");
        CHECK(decodeText(five({0x1B, 0x09}), tableIta2(false)).text == "<ENQ>");
        // the whole letters plane: A..Z each exactly once
        std::string letters;
        for (unsigned c = 0; c < 32; ++c) if (tableIta2().planes[0][c].size() == 1 && std::isalpha(static_cast<unsigned char>(tableIta2().planes[0][c][0]))) letters += tableIta2().planes[0][c];
        std::sort(letters.begin(), letters.end());
        CHECK(letters == "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
        // unknown symbols are counted and shown as '?'; count limits the read; a symbol that does not fit is not read
        const auto dt = decodeText(five({0x1B, 0x1B, 0x1B}), tableIta2());
        CHECK(dt.text.empty() && dt.symbols == 3);
        CodeTable sparse = tableAscii7();
        sparse.planes[0]['A'].clear();
        const auto du = decodeText(h7, sparse);
        CHECK(du.unknown == 0);
        Bits aA;
        for (int i = 6; i >= 0; --i) aA.push_back(static_cast<uint8_t>(('A' >> i) & 1u));
        CHECK(decodeText(aA, sparse).text == "?" && decodeText(aA, sparse).unknown == 1);
        CHECK(decodeText(h7, tableAscii7(), 0, 1).text == "H");
        CHECK(decodeText(h7, tableAscii7(), 7).text == "i");
        CHECK(decodeText(B("101"), tableAscii7()).text.empty());
        CHECK(decodeText({}, tableAscii7()).text.empty());
        CHECK(decodeText(h7, CodeTable{}).text.empty());
        // LSB-first reading
        Bits lsb;
        for (int i = 0; i < 7; ++i) lsb.push_back(static_cast<uint8_t>(('H' >> i) & 1u));
        CHECK(decodeText(lsb, tableAscii7(), 0, 0, false).text == "H");
    }

    // ---- the user table file: a table with up to four planes, refusals with line numbers ----------------------------------------------
    {
        const char* good =
            "foxsdr-bittable 1\nname mine\nbits 5\n"
            "plane A  # first plane\nshift B = 27\nshift C = 28\n1 = x\n2 = <space>\n"
            "plane B\nshift A = 31\n1 = 1\n0b10 = 0x\n"
            "plane C\nshift A = 31\n1 = !\n";
        const TableParse p = parseTable(good);
        CHECK(p.ok && p.table.name == "mine" && p.table.bits == 5 && p.table.planes.size() == 3);
        auto five = [](std::initializer_list<unsigned> v) { Bits b; for (unsigned x : v) for (int i = 4; i >= 0; --i) b.push_back(static_cast<uint8_t>((x >> i) & 1u)); return b; };
        CHECK(decodeText(five({1, 2, 27, 1, 2, 31, 1, 28, 1}), p.table).text == "x 10xx!");
        struct Bad { const char* text; int line; const char* part; };
        const Bad bad[] = {
            {"", 0, "empty"},
            {"nope\n", 1, "first statement"},
            {"foxsdr-bittable 2\n", 1, "first statement"},
            {"foxsdr-bittable 1\nbits 9\n", 2, "1..8"},
            {"foxsdr-bittable 1\nplane A\n", 2, "'bits' must come before"},
            {"foxsdr-bittable 1\nbits 5\n1 = a\n", 3, "inside a plane"},
            {"foxsdr-bittable 1\nbits 5\nplane A\nplane A\n", 4, "already defined"},
            {"foxsdr-bittable 1\nbits 5\nplane A\nplane B\nplane C\nplane D\nplane E\n", 7, "at most four"},
            {"foxsdr-bittable 1\nbits 5\nplane A\n32 = a\n", 4, "inside 0..31"},
            {"foxsdr-bittable 1\nbits 5\nplane A\n1 = a\n1 = b\n", 5, "already defined"},
            {"foxsdr-bittable 1\nbits 5\nplane A\n1 = abcdefghij\n", 4, "at most 8"},
            {"foxsdr-bittable 1\nbits 5\nplane A\n1 a\n", 4, "expected"},
            {"foxsdr-bittable 1\nbits 5\nplane A\nshift Z = 3\n", 5 - 1, "unknown plane"},
            {"foxsdr-bittable 1\nbits 5\nplane A\nshift B 3\n", 4, "expected 'shift"},
            {"foxsdr-bittable 1\nbits 5\n", 0, "no plane"},
            {"foxsdr-bittable 1\nbits 5\nplane A\nname a b\n", 4, "one word"},
            {"foxsdr-bittable 1\nbits 5\nplane A\n-1 = a\n", 4, "expected"},
        };
        for (const auto& b : bad) {
            const TableParse t = parseTable(b.text);
            bool found = false;
            for (const auto& e : t.errors) found |= e.line == b.line && e.message.find(b.part) != std::string::npos;
            if (!found) {
                std::printf("  table refusal not as expected:\n%s  wanted line %d '%s'; got:", b.text, b.line, b.part);
                for (const auto& e : t.errors) std::printf(" [%d: %s]", e.line, e.message.c_str());
                std::printf("\n");
            }
            CHECK(!t.ok && found);
        }
        CHECK(!parseTable(std::string(9 * 1024, ' ')).ok);
        std::string manyEntries = "foxsdr-bittable 1\nbits 8\nplane A\n";
        for (int i = 0; i < 300; ++i) manyEntries += std::to_string(i % 256) + " = a\n";
        CHECK(!parseTable(manyEntries).ok);  // duplicates and the 400-line limit
        // the shift target may be defined after the shift line
        CHECK(parseTable("foxsdr-bittable 1\nbits 2\nplane A\nshift B = 3\nplane B\n").ok);
    }

    // ---- remapping and the editor -----------------------------------------------------------------------------------------------------
    {
        std::array<uint8_t, 256> id{}, swap01{};
        for (size_t i = 0; i < 256; ++i) { id[i] = static_cast<uint8_t>(i); swap01[i] = static_cast<uint8_t>(i); }
        swap01[0] = 1;
        swap01[1] = 0;
        CHECK(remapSymbols(B("00 01 10 11"), 2, id) == B("00011011"));
        CHECK(remapSymbols(B("00 01 10 11"), 2, swap01) == B("01001011"));
        CHECK(remapSymbols(B("00 01 1"), 2, swap01) == B("01001"));   // a short tail is left alone
        CHECK(remapSymbols(B("0001"), 0, swap01) == B("0001") && remapSymbols(B("0001"), 9, swap01) == B("0001"));

        BitEditor e(B("1011 0010 1110 0101"));
        CHECK(e.size() == 16 && !e.dirty() && !e.canUndo());
        CHECK(e.flipBit(0) && toString(e.bits()) == "0011001011100101" && e.dirty());
        CHECK(e.setBit(0, 1) && toString(e.bits()) == "1011001011100101");
        CHECK(e.setByte(1, 0xFF) && toString(e.bits()) == "1011001011111111");
        CHECK(e.insertBits(8, B("000")) && e.size() == 19);
        CHECK(e.eraseBits(8, 3) && e.size() == 16);
        CHECK(e.invertRange(0, 4) && toString(e.bits()).substr(0, 4) == "0100");
        CHECK(e.reverseRange(0, 4) && toString(e.bits()).substr(0, 4) == "0010");
        CHECK(e.xorRange(0, 16, B("11")) && toString(e.bits()).substr(0, 4) == "1101");
        CHECK(e.remapRange(0, 4, 2, swap01) && toString(e.bits()).substr(0, 4) == "1100");
        // undo everything, exactly back to the start, then redo everything, exactly back to the end
        const std::string end = toString(e.bits());
        size_t undone = 0;
        while (e.undo()) ++undone;
        CHECK(undone == e.historySize() + undone && toString(e.bits()) == "1011001011100101" && !e.dirty());
        CHECK(!e.undo() && e.canRedo());
        size_t redone = 0;
        while (e.redo()) ++redone;
        CHECK(redone == undone && toString(e.bits()) == end && !e.canRedo());
        // an edit after undo clears the redo branch
        e.undo();
        CHECK(e.canRedo());
        CHECK(e.flipBit(5));
        CHECK(!e.canRedo());
        // Undo of a LENGTH-CHANGING edit on its own (an overlapping later edit would hide a wrong range): insert, erase, replace by a
        // longer and by a shorter run, each undone and redone against the exact text.
        {
            const std::string base = "1011001011100101";
            BitEditor u(B(base.c_str()));
            CHECK(u.insertBits(5, B("0000")) && u.size() == 20);
            CHECK(u.undo() && toString(u.bits()) == base);
            CHECK(u.redo() && toString(u.bits()) == base.substr(0, 5) + "0000" + base.substr(5));
            CHECK(u.undo() && u.eraseBits(2, 6) && u.size() == 10 && toString(u.bits()) == base.substr(0, 2) + base.substr(8));
            CHECK(u.undo() && toString(u.bits()) == base);
            CHECK(u.redo() && u.size() == 10);
            CHECK(u.undo() && toString(u.bits()) == base);
            CHECK(u.replaceAll(B("11")) && u.size() == 2 && u.undo() && toString(u.bits()) == base);
            CHECK(u.replaceAll(Bits(40, 1)) && u.size() == 40 && u.undo() && toString(u.bits()) == base && u.redo() && u.size() == 40);
        }
        // refusals change nothing
        const std::string snap = toString(e.bits());
        CHECK(!e.setBit(99, 1) && !e.flipBit(99) && !e.setByte(10, 1) && !e.insertBits(99, B("1")) && !e.eraseBits(10, 99) && !e.eraseBits(99, 0) &&
              !e.invertRange(10, 99) && !e.reverseRange(99, 1) && !e.xorRange(0, 4, {}) && !e.xorRange(0, 99, B("1")) && !e.remapRange(0, 4, 9, swap01));
        CHECK(toString(e.bits()) == snap);
        // a no-op edit leaves no history entry
        const size_t hs = e.historySize();
        CHECK(e.setBit(0, e.bits()[0]) && e.historySize() == hs);
        CHECK(e.eraseBits(0, 0) && e.historySize() == hs);
        // dirty tracking across markSaved
        BitEditor d(B("1111"));
        d.markSaved();
        CHECK(!d.dirty());
        d.flipBit(0);
        CHECK(d.dirty());
        d.undo();
        CHECK(!d.dirty());
        d.flipBit(0);
        d.markSaved();
        d.undo();
        CHECK(d.dirty());
        d.redo();
        CHECK(!d.dirty());
        // saved state on a redo branch that gets dropped can never be reached again
        d.undo();
        d.markSaved();      // saved at the undone state (revision 0)
        d.redo();
        d.undo();
        d.flipBit(3);       // new branch
        d.undo();
        CHECK(!d.dirty());  // the saved state (the state before any edit) is reachable again
        // history bound: 300 edits keep 256 and the buffer bound refuses growth
        BitEditor h(B("0"));
        for (int i = 0; i < 300; ++i) h.flipBit(0);
        CHECK(h.historySize() == BitEditor::kMaxEdits);
        BitEditor g;
        CHECK(g.insertBits(0, Bits(BitEditor::kMaxBits, 1)));
        CHECK(!g.insertBits(0, B("1")) && g.size() == BitEditor::kMaxBits);
        BitEditor lost(B("1"));
        lost.markSaved();
        for (int i = 0; i < 300; ++i) lost.flipBit(0);
        lost.markSaved();
        for (int i = 0; i < 300; ++i) lost.flipBit(0);
        for (int i = 0; i < 300; ++i) lost.undo();
        CHECK(lost.dirty());  // the saved state fell out of the 256-edit history
        // the initial buffer is truncated at the bound
        CHECK(BitEditor(Bits(BitEditor::kMaxBits + 5, 0)).size() == BitEditor::kMaxBits);
        // byte view
        BitEditor v(fromBytes({'A', 'b', 0x00, 0xFF, '1', '2', '3', '4', '5'}));
        const std::string view = v.byteView(8);
        CHECK(view == "000000  41 62 00 FF 31 32 33 34  Ab..1234\n000008  35                       5\n");
        CHECK(BitEditor().byteView().empty());
    }
    return testSummary("test_bits_rawbuf_edit");
}
