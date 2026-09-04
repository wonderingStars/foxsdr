// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/module_census.hpp"

#include <cstdio>

namespace cascade::gui {

using cascade::core::LoadedPlugin;

bool moduleProvides(const LoadedPlugin& p, std::uint32_t capMask) {
    // A record that did not load carries no tables at all - the host nulls
    // every borrowed pointer on the way out - so asking about them would
    // answer "no" for a reason that has nothing to do with the question. The
    // caller wants the capability WORD for those, and censusModules reads it.
    if (!p.loaded) { return false; }

    // ONE ENTRY PER BIT THIS ABI DEFINES, so the mapping is total over
    // CASCADE_CAP_ALL_KNOWN and a bit cannot be silently answered "no". A new
    // capability added to plugin_abi.h without a line here would be a module
    // this census could never see, which is the failure mode the whole file is
    // about.
    struct Entry {
        std::uint32_t bit;
        const void* table;
    };
    const Entry table[] = {
        {CASCADE_CAP_DECODER, p.decoder},
        {CASCADE_CAP_IQ_DECODER, p.iqDecoder},
        {CASCADE_CAP_IMAGE_DECODER, p.imageDecoder},
        {CASCADE_CAP_TRACK_SOURCE, p.trackSource},
        {CASCADE_CAP_PANEL, p.panel},
        {CASCADE_CAP_HOST_CLIENT, p.hostClient},
        {CASCADE_CAP_PRESET, p.preset},
        {CASCADE_CAP_BASEMAP, p.basemap},
        {CASCADE_CAP_TRACK_INFO, p.trackInfo},
    };
    for (const Entry& e : table) {
        if ((capMask & e.bit) != 0u && e.table != nullptr) { return true; }
    }
    return false;
}

ModuleCensus censusModules(const std::vector<LoadedPlugin>& plugins, std::uint32_t capMask,
                           const ModuleStoppedFn& isStopped) {
    ModuleCensus c;
    for (const LoadedPlugin& p : plugins) {
        const std::string file = cascade::core::pluginKey(p);
        const std::string named = p.name.empty() ? file : p.name;
        if (!p.loaded) {
            // THE CAPABILITY WORD, not the table, on a record that did not
            // load: every borrowed pointer is nulled on the way out and the
            // bits are what survive.
            if ((p.capabilities & capMask) != 0u) {
                ++c.refused;
                if (c.refusedName.empty()) { c.refusedName = named; }
            } else if (p.capabilities == 0u) {
                ++c.unread;
            }
            continue;
        }
        if (!moduleProvides(p, capMask)) { continue; }
        // A NULL PREDICATE IS NOT "NOTHING IS STOPPED". A caller with no stop
        // set to consult would otherwise have every stopped module counted as
        // live, which is the same class of wrong answer as the one this file
        // exists to remove; it is a programming error, and the empty function
        // is the only value it can take, so treat it as "cannot say" and
        // count nothing rather than counting it live.
        if (!isStopped) { continue; }
        if (isStopped(file)) {
            ++c.stopped;
            if (c.stoppedName.empty()) { c.stoppedName = named; }
            continue;
        }
        ++c.live;
    }
    return c;
}

// The clause every note ends with when a file in the folder was refused before
// the host could read what it was. Saying "nothing here decodes anything" while
// such a file sits in the folder would be a firmer claim than the evidence
// supports.
namespace {

void appendUnreadClause(std::string& s, int unread) {
    if (unread <= 0) { return; }
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  " %d file%s in the plugin folder %s refused before the host could "
                  "read what %s - the Fitted modules window prints why.",
                  unread, unread == 1 ? "" : "s", unread == 1 ? "was" : "were",
                  unread == 1 ? "it was" : "they were");
    s += buf;
}

}  // namespace

std::string trackSourceAbsenceNote(const ModuleCensus& c, const char* subject,
                                   const char* installRemedy) {
    char buf[320];
    // STOPPED FIRST, and ahead of every other answer, exactly as
    // gui::fittedState orders them: the user's own choice is not a fault, and
    // it is the one state where the module they need is already on the disk.
    if (c.stopped > 0) {
        std::string s;
        if (c.stopped == 1 && !c.stoppedName.empty()) {
            std::snprintf(buf, sizeof(buf),
                          "Nothing is publishing %s: \"%s\" is a track source and you "
                          "stopped it.",
                          subject, c.stoppedName.c_str());
            s = buf;
        } else {
            std::snprintf(buf, sizeof(buf),
                          "Nothing is publishing %s: %d fitted track sources are "
                          "stopped.",
                          subject, c.stopped);
            s = buf;
        }
        s += " A stopped module stays fitted and is given no track source until you "
             "start it again - the key is on its own plate in the Fitted modules window.";
        return s;
    }
    if (c.refused > 0) {
        std::string s;
        if (c.refused == 1 && !c.refusedName.empty()) {
            // REFUSED, which is the word the Fitted modules window letters this
            // state in, rather than a second one meaning the same thing.
            std::snprintf(buf, sizeof(buf),
                          "Nothing is publishing %s: \"%s\" is a track source and the "
                          "host refused it.",
                          subject, c.refusedName.c_str());
            s = buf;
        } else {
            std::snprintf(buf, sizeof(buf),
                          "Nothing is publishing %s: %d fitted track sources were "
                          "refused.",
                          subject, c.refused);
            s = buf;
        }
        s += " It is fitted already, so there is nothing to fetch: the Fitted modules "
             "window prints the host's own reason for refusing it, verbatim.";
        return s;
    }
    if (c.live > 0) {
        std::snprintf(buf, sizeof(buf),
                      "Nothing is publishing %s: a fitted track source did not start - "
                      "the host asked it for one and was given none. Nothing needs "
                      "fetching; the module itself failed.",
                      subject);
        return buf;
    }
    std::snprintf(buf, sizeof(buf),
                  "No fitted module publishes tracks of any kind, so there are no %s "
                  "here. %s",
                  subject, installRemedy);
    std::string s = buf;
    appendUnreadClause(s, c.unread);
    return s;
}

std::string decoderAbsenceNote(const ModuleCensus& c) {
    char buf[400];
    // THE SAME FOUR ANSWERS IN THE SAME ORDER as the track-source note above,
    // because they are answers to the same question about the same records.
    // Only the last of them mentions fitting anything.
    if (c.stopped > 0) {
        std::string s;
        if (c.stopped == 1 && !c.stoppedName.empty()) {
            // "carries a decoder", never "decodes ADS-B": the descriptor
            // declares the capability and says nothing whatever about what the
            // module decodes. See the header.
            std::snprintf(buf, sizeof(buf),
                          "Nothing is decoding: \"%s\" carries a decoder and you "
                          "stopped it.",
                          c.stoppedName.c_str());
            s = buf;
        } else {
            // Reached with two or more, and - only in theory - with one whose
            // descriptor carried no name. A loaded record always has one, since
            // MissingName is a refusal, so the singular is written out for
            // correctness rather than for a state anyone will see.
            std::snprintf(buf, sizeof(buf),
                          "Nothing is decoding: %d fitted decoder%s %s stopped.", c.stopped,
                          c.stopped == 1 ? "" : "s", c.stopped == 1 ? "is" : "are");
            s = buf;
        }
        s += " A stopped module stays fitted and is fed no signal until you start it "
             "again - the Fitted modules window letters it STOPPED BY YOU and the key "
             "is on its plate.";
        return s;
    }
    if (c.refused > 0) {
        std::string s;
        if (c.refused == 1 && !c.refusedName.empty()) {
            std::snprintf(buf, sizeof(buf),
                          "Nothing is decoding: \"%s\" carries a decoder and the host "
                          "refused it.",
                          c.refusedName.c_str());
            s = buf;
        } else {
            std::snprintf(buf, sizeof(buf),
                          "Nothing is decoding: %d fitted decoder%s %s refused.", c.refused,
                          c.refused == 1 ? "" : "s", c.refused == 1 ? "was" : "were");
            s = buf;
        }
        s += " It is on the disk already, so there is nothing to fetch: the Fitted "
             "modules window letters it REFUSED and prints the host's own reason, "
             "verbatim.";
        return s;
    }
    if (c.live > 0) {
        // THE RUNNER'S LIST IS BUILT, NOT OBSERVED. PluginRunner::status() is
        // filled only by rebuild(), so a fitted decoder that is loaded, not
        // stopped and absent from that list means the list predates the module.
        // Nothing is wrong with the module and nothing needs fetching.
        return "Nothing is decoding: a fitted decoder is not in the receiver's decoder "
               "list. That list is built when the receiver's source or rate changes and "
               "after a rescan of the plugin folder, so nothing here needs fetching.";
    }
    std::string s =
        "No fitted module carries a decoder of any kind, so there is nothing here to "
        "decode with. The Plugin store row above is where one is fitted from.";
    appendUnreadClause(s, c.unread);
    return s;
}

}  // namespace cascade::gui
