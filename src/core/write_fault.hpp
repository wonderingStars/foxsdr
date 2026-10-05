// write_fault.hpp - a test seam for a file write that fails part way (0.99.65).
//
// WHAT IT IS FOR. The settings, the bookmark list, the markers, a screenshot and an
// exported list are each written through a standard stream; a full disk, a volume
// that went read-only or a device that was pulled shows up as that stream going bad
// after the bytes were handed to it. There is no unprivileged way to make a real
// volume fill up under a test, so the writers call writeFaultPoint() immediately
// after they have written and flushed, and a test installs a hook that sets the
// stream's badbit - which is exactly the state a failed write leaves it in - to
// prove what each writer then does: the temporary file is removed, the old file is
// untouched, the reason is returned, and the caller says it.
//
// A plain function pointer, null in every shipped build: the cost there is one
// relaxed atomic load per file written. It is never set by the application.
//
// `writer` names which writer is asking ("config", "bookmarks", "markers", "export",
// "bmp"), so a test fails the one it is about and leaves the rest alone.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_WRITE_FAULT_HPP
#define CASCADE_CORE_WRITE_FAULT_HPP

#include <atomic>
#include <ostream>

namespace cascade::core {

using WriteFaultHook = void (*)(const char* writer, std::ostream& out);

namespace detail {
inline std::atomic<WriteFaultHook>& writeFaultSlot() {
    static std::atomic<WriteFaultHook> slot{nullptr};
    return slot;
}
}  // namespace detail

// Tests only. Pass nullptr to remove it.
inline void setWriteFaultHookForTest(WriteFaultHook hook) {
    detail::writeFaultSlot().store(hook, std::memory_order_relaxed);
}

// Called by a writer after it has written and flushed `out`, before it looks at the
// stream's state.
inline void writeFaultPoint(const char* writer, std::ostream& out) {
    if (const WriteFaultHook hook = detail::writeFaultSlot().load(std::memory_order_relaxed)) {
        hook(writer, out);
    }
}

}  // namespace cascade::core

#endif  // CASCADE_CORE_WRITE_FAULT_HPP
