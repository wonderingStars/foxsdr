// core/file_drop.hpp - files dragged from Explorer onto a window: the paths, handed to a callback on the window's own thread.
// No Slint, no ImGui, no GLFW: it takes a window handle and nothing else.
//
// WHY THIS EXISTS (rows P17 and W2). The classic window gets drops from its window library. The Slint window's library does not
// hand them over, so the window needs ONE call that says "this window takes files" and a callback that receives the paths. What is
// done with them (core/file_kind.hpp names what each one is) is the window's business.
//
// THE CHOICE, AND WHY. Windows offers two doors, and BOTH are here, because neither alone is right everywhere:
//   * OLE (IDropTarget + RegisterDragDrop) - the DEFAULT. It needs no change to the window procedure (a subclass of somebody else's
//     window procedure is a lifetime hazard: the owner may subclass or destroy in an order we cannot see), it can REFUSE a drag that
//     holds no files while the pointer is still over the window (the "no" cursor, not a silent swallow), and it hands the whole
//     CF_HDROP block, which has no 260-character limit. Its costs: the thread must be an STA with OLE initialised (this module calls
//     OleInitialize; if the thread already joined the multithreaded apartment OLE cannot be used there), and a window can hold ONE
//     drop target - a window library that registered its own already (winit does, unless drag-and-drop is switched off) makes
//     RegisterDragDrop answer DRAGDROP_E_ALREADYREGISTERED. That is a REFUSAL FACT, not a fallback: WM_DROPFILES never arrives at a
//     window whose OLE target belongs to somebody else, so falling back would "succeed" and receive nothing. Options::replaceExisting
//     revokes the other target first (same thread only), for a window whose owner has been told.
//   * WM_DROPFILES (DragAcceptFiles + a window subclass) - chosen when OLE is unavailable on the thread (the MTA case) or by asking
//     for it. The subclass is installed with SetWindowSubclass (a chain, removed by id, safe whatever order others subclass).
//
// THREADING. attach() and the destructor run on the thread that owns the window (the UI thread); the callback runs on that thread
// too (a drop is a message to that thread). attach() from any other thread is refused with a fact.
//
// BOUNDS. At most Options::maxFiles paths are delivered (the rest are counted, never allocated); a hostile or giant drop block is
// parsed with every offset checked (pathsFromDropBlock, fuzzed); the callback never runs after detach()/destruction.
//
// PRIVACY. Nothing here logs a path.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_FILE_DROP_HPP
#define CASCADE_CORE_FILE_DROP_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cascade::core {

// Every path in a CF_HDROP block (a DROPFILES header, then a list of paths each ending in NUL, the list in a further NUL), UTF-8, in
// order, at most `maxFiles` of them. `*total` (when given) is the number of paths the block holds, `*complete` false when the block
// is cut off (a path that runs off the end without its NUL, an offset past the end). Wide lists are UTF-16 (a lone surrogate becomes
// U+FFFD), narrow ones the system code page. A block that is not a list gives nothing. Pure; fuzzed.
std::vector<std::string> pathsFromDropBlock(const unsigned char* block, std::size_t bytes, std::size_t maxFiles = 4096,
                                            std::size_t* total = nullptr, bool* complete = nullptr);

struct DropEvent {
    std::vector<std::string> paths;  // UTF-8, at most Options::maxFiles
    std::size_t total = 0;           // how many the drop held (may exceed paths.size())
    bool complete = true;            // false when the drop block was cut off
};
using DropCallback = std::function<void(const DropEvent&)>;

struct DropOptions {
    enum class Mode : std::uint8_t { Auto, Ole, DropFiles };
    Mode mode = Mode::Auto;          // Auto: OLE, and WM_DROPFILES where OLE cannot be used on this thread
    std::size_t maxFiles = 4096;
    bool replaceExisting = false;    // OLE: revoke a drop target somebody else registered first (same thread only)
};

// One window's file-drop registration. Move-less; destroy it (on the window's thread) to unregister.
class FileDrop {
public:
    // Registers `hwnd` (a Windows HWND passed as a pointer so this header needs no windows.h). Returns null with `*refusal` set (one
    // sentence, no path) when it cannot: no window, wrong thread, no OLE on the thread, a target already registered, a non-Windows
    // build. On success `callback` is called once per drop.
    static std::unique_ptr<FileDrop> attach(void* hwnd, DropCallback callback, const DropOptions& options = {}, std::string* refusal = nullptr);

    ~FileDrop();
    FileDrop(const FileDrop&) = delete;
    FileDrop& operator=(const FileDrop&) = delete;

    // Unregisters and silences the callback. Idempotent. After the window is destroyed this is a no-op that never throws.
    void detach();

    DropOptions::Mode mode() const;  // the door in use: Ole or DropFiles (never Auto)
    bool attached() const;
    std::size_t dropsDelivered() const;  // callbacks made so far
    std::size_t dragsRefused() const;    // OLE drags that held no files and were told "no"

    // Tests: the OLE target (an IDropTarget*), null in DropFiles mode.
    void* oleTargetForTest() const;

    struct Impl;

private:
    FileDrop();
    std::unique_ptr<Impl> impl_;
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_FILE_DROP_HPP
