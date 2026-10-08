// plugin_dir_signature.hpp - a cheap fingerprint of the plugins folder, so a
// rescan that would find exactly what the last one found can be skipped
// (docs/DIAGNOSTICS.md, "A plugin rescan runs on the GUI thread", option C).
//
// WHAT A RESCAN IS FOR. It exists to make the loaded plugins match what is on
// disk: a file added or removed or replaced, a manifest that now retires a
// plugin. When nothing on disk has moved since the last scan completed, the
// rescan changes nothing - and costs the destroy() of every decoder, the unmap
// and map of every module and a re-hash of every installed file, all on the
// thread that draws the window and none of it bounded.
//
// WHAT THE SIGNATURE IS: the directory the scan reads, and for every regular
// file in it - not only the modules - its name, its size and its last-write
// time. All of it comes out of ONE directory listing: on Windows the listing
// hands each entry back with its size and write time, so nothing is opened,
// nothing is hashed and no file is asked about separately; on a POSIX standard
// library the entry's size and time cost one stat each, which is still no open
// and no read. Every file, because the things that decide which modules load
// are not all modules:
//
//   - the modules themselves (a name, a size or a time that moved: a different
//     plugin or a different build of one);
//   - installed.json, the manifest, which holds both what was installed and
//     the cached catalogue policy - the retirement floors - and so decides the
//     blocked set (PluginRepo::blockedPlugins). The catalogue fetch is the only
//     thing that writes a floor, and it writes the manifest;
//   - the "<module>.disabled" files a retirement leaves, so a quarantined module
//     that was put back, or removed, by hand is seen.
//
// ONE EXCEPTION (0.99.72): the plugin store's catalogue cache - catalogue.json,
// catalogue.json.time and the ".part" each is renamed from - is not counted. A
// good catalogue read rewrites it every time, it decides nothing about which
// modules load, and counted it made every fetch look like a changed folder.
//
// WHAT IT CANNOT SEE, stated so nobody assumes otherwise: a file replaced by
// one with the same size and the same last-write time (a copy that preserves
// times, onto a file system whose clock is coarser than the copy), and anything
// the loader reads from OUTSIDE this folder - a vendor runtime on the system
// path that a module needs. The first is why the comparison is on size AND time
// and the application never takes a signature as proof of a SECOND difference;
// the second is why a scan that left any module refused is never skipped
// (AppWindow::pluginScanSigValid_): a refusal is exactly where an outside
// dependency, installed since, would change the answer.
//
// PURE: no state, no thread, no clock. A missing or unreadable folder is
// answered, not thrown.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PLUGIN_DIR_SIGNATURE_HPP
#define CASCADE_CORE_PLUGIN_DIR_SIGNATURE_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace cascade::core {

struct PluginDirSignature {
    struct Entry {
        std::string name;            // the file's own name, not its path
        std::uint64_t bytes = 0;     // its size
        std::int64_t writeTime = 0;  // its last-write time, in the file system's own ticks
        bool operator==(const Entry& o) const {
            return name == o.name && bytes == o.bytes && writeTime == o.writeTime;
        }
    };

    // The folder the listing is of, as given - a scan that moved to another
    // folder is a different signature whatever the two hold.
    std::string dir;
    // False when the folder does not exist (or is not a folder): the normal
    // state of a user who installed no plugins, answered as such and not as a
    // failure - PluginHost::scan treats it as an empty list too.
    bool listed = false;
    // Every regular file, sorted by name. Folders are not listed: nothing
    // loads from one.
    std::vector<Entry> files;

    bool operator==(const PluginDirSignature& o) const {
        return dir == o.dir && listed == o.listed && files == o.files;
    }
    bool operator!=(const PluginDirSignature& o) const { return !(*this == o); }

    // The entry for `name`, or null.
    const Entry* find(const std::string& name) const {
        for (const Entry& e : files) {
            if (e.name == name) { return &e; }
        }
        return nullptr;
    }
};

// One directory listing of `dir`. True with `out` filled in, `listed` false for
// a folder that is not there; FALSE - `out` meaningless - when the folder is
// there and the listing, or any entry's size or time, could not be had. The
// caller reads false as "I do not know", never as "nothing changed".
bool readPluginDirSignature(const std::string& dir, PluginDirSignature& out);

}  // namespace cascade::core

#endif  // CASCADE_CORE_PLUGIN_DIR_SIGNATURE_HPP
