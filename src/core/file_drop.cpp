// file_drop.cpp - see file_drop.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/file_drop.hpp"

#include <atomic>
#include <cstring>

#include "core/utf8_text.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <ole2.h>
#include <shellapi.h>
#include <commctrl.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#endif

namespace cascade::core {

namespace {

std::string utf16ToUtf8(const unsigned char* p, std::size_t units) {
    std::string out;
    for (std::size_t i = 0; i < units; ++i) {
        unsigned int u = static_cast<unsigned int>(p[2 * i]) | (static_cast<unsigned int>(p[2 * i + 1]) << 8);
        if (u >= 0xD800 && u <= 0xDBFF) {
            unsigned int lo = 0;
            if (i + 1 < units) { lo = static_cast<unsigned int>(p[2 * i + 2]) | (static_cast<unsigned int>(p[2 * i + 3]) << 8); }
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            } else {
                u = 0xFFFD;
            }
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            u = 0xFFFD;
        }
        utf8Append(out, u);
    }
    return out;
}

std::string narrowToUtf8(const unsigned char* p, std::size_t n) {
    if (n == 0) { return std::string(); }
#if defined(_WIN32)
    const int wn = ::MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(p), static_cast<int>(n), nullptr, 0);
    if (wn <= 0) { return std::string(reinterpret_cast<const char*>(p), n); }
    std::wstring w(static_cast<std::size_t>(wn), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(p), static_cast<int>(n), w.data(), wn);
    return utf16ToUtf8(reinterpret_cast<const unsigned char*>(w.data()), w.size());
#else
    return std::string(reinterpret_cast<const char*>(p), n);
#endif
}

}  // namespace

std::vector<std::string> pathsFromDropBlock(const unsigned char* block, std::size_t bytes, std::size_t maxFiles, std::size_t* total, bool* complete) {
    std::vector<std::string> out;
    if (total != nullptr) { *total = 0; }
    if (complete != nullptr) { *complete = true; }
    if (block == nullptr || bytes < 20) { return out; }
    std::uint32_t offset = 0;
    std::uint32_t wide = 0;
    std::memcpy(&offset, block, 4);
    std::memcpy(&wide, block + 16, 4);
    if (offset < 20 || offset > bytes) { return out; }
    const unsigned char* p = block + offset;
    const std::size_t avail = bytes - offset;
    std::size_t count = 0;
    bool whole = true;
    if (wide != 0) {
        const std::size_t units = avail / 2;
        std::size_t i = 0;
        while (i < units) {
            std::size_t j = i;
            while (j < units && !(p[2 * j] == 0 && p[2 * j + 1] == 0)) { ++j; }
            if (j == i) { break; }       // the empty string that ends the list
            if (j >= units) { whole = false; break; }   // ran off the end without its NUL
            if (out.size() < maxFiles) { out.push_back(utf16ToUtf8(p + 2 * i, j - i)); }
            ++count;
            i = j + 1;
        }
    } else {
        std::size_t i = 0;
        while (i < avail) {
            std::size_t j = i;
            while (j < avail && p[j] != 0) { ++j; }
            if (j == i) { break; }
            if (j >= avail) { whole = false; break; }
            if (out.size() < maxFiles) { out.push_back(narrowToUtf8(p + i, j - i)); }
            ++count;
            i = j + 1;
        }
    }
    if (total != nullptr) { *total = count; }
    if (complete != nullptr) { *complete = whole; }
    return out;
}

#if defined(_WIN32)

namespace {

constexpr UINT_PTR kSubclassId = 0x46445250;  // "FDRP"

DropEvent eventFromBlock(const unsigned char* block, std::size_t bytes, std::size_t maxFiles) {
    DropEvent ev;
    ev.paths = pathsFromDropBlock(block, bytes, maxFiles, &ev.total, &ev.complete);
    return ev;
}

}  // namespace

struct FileDrop::Impl {
    HWND hwnd = nullptr;
    DropCallback cb;
    DropOptions options;
    DropOptions::Mode mode = DropOptions::Mode::Ole;
    DWORD threadId = 0;
    bool attached = false;
    bool oleInit = false;      // OleInitialize succeeded (S_OK or S_FALSE): one OleUninitialize owed
    bool oleRegistered = false;
    bool subclassed = false;
    std::size_t delivered = 0;
    std::size_t refused = 0;
    IDropTarget* target = nullptr;  // our reference, released at detach

    void deliver(const DropEvent& ev) {
        DropCallback local = cb;   // a callback that destroys this object must not pull its own std::function out from under itself
        if (!local) { return; }
        ++delivered;
        local(ev);
    }
};

namespace {

class DropTargetObj final : public IDropTarget {
public:
    explicit DropTargetObj(FileDrop::Impl* owner) : owner_(owner) {}

    void disown() { owner_.store(nullptr); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) { return E_POINTER; }
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(++ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const long r = --ref_;
        if (r == 0) { delete this; }
        return static_cast<ULONG>(r);
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        hasFiles_ = holdsFiles(data);
        FileDrop::Impl* o = owner_.load();
        if (!hasFiles_ && o != nullptr) { ++o->refused; }
        if (effect != nullptr) { *effect = (hasFiles_ && o != nullptr) ? DROPEFFECT_COPY : DROPEFFECT_NONE; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        if (effect != nullptr) { *effect = (hasFiles_ && owner_.load() != nullptr) ? DROPEFFECT_COPY : DROPEFFECT_NONE; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        hasFiles_ = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        hasFiles_ = false;
        if (effect != nullptr) { *effect = DROPEFFECT_NONE; }
        FileDrop::Impl* o = owner_.load();
        if (o == nullptr || data == nullptr) { return S_OK; }
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM sm{};
        if (FAILED(data->GetData(&fe, &sm))) {
            ++o->refused;
            return S_OK;
        }
        DropEvent ev;
        if (sm.tymed == TYMED_HGLOBAL && sm.hGlobal != nullptr) {
            const SIZE_T size = ::GlobalSize(sm.hGlobal);
            const void* mem = ::GlobalLock(sm.hGlobal);
            if (mem != nullptr) {
                ev = eventFromBlock(static_cast<const unsigned char*>(mem), static_cast<std::size_t>(size), o->options.maxFiles);
                ::GlobalUnlock(sm.hGlobal);
            }
        }
        ::ReleaseStgMedium(&sm);
        if (effect != nullptr) { *effect = DROPEFFECT_COPY; }
        if (ev.total == 0) { return S_OK; }
        o->deliver(ev);   // last: the callback may destroy the owner
        return S_OK;
    }

private:
    static bool holdsFiles(IDataObject* data) {
        if (data == nullptr) { return false; }
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        return data->QueryGetData(&fe) == S_OK;
    }

    std::atomic<FileDrop::Impl*> owner_;
    std::atomic<long> ref_{1};
    bool hasFiles_ = false;
};

LRESULT CALLBACK subclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    FileDrop::Impl* impl = reinterpret_cast<FileDrop::Impl*>(ref);
    if (msg == WM_DROPFILES && impl != nullptr && impl->mode == DropOptions::Mode::DropFiles) {
        HDROP drop = reinterpret_cast<HDROP>(wp);
        DropEvent ev;
        const void* mem = ::GlobalLock(reinterpret_cast<HGLOBAL>(drop));
        if (mem != nullptr) {
            ev = eventFromBlock(static_cast<const unsigned char*>(mem), static_cast<std::size_t>(::GlobalSize(reinterpret_cast<HGLOBAL>(drop))),
                                impl->options.maxFiles);
            ::GlobalUnlock(reinterpret_cast<HGLOBAL>(drop));
        }
        ::DragFinish(drop);
        if (ev.total > 0) { impl->deliver(ev); }
        return 0;
    }
    if (msg == WM_DESTROY && impl != nullptr && impl->mode == DropOptions::Mode::Ole && impl->oleRegistered) {
        // The window is going: an OLE target must be revoked before the window dies or OLE keeps a reference to it for good.
        ::RevokeDragDrop(hwnd);
        impl->oleRegistered = false;
        if (impl->target != nullptr) {
            static_cast<DropTargetObj*>(impl->target)->disown();
            impl->target->Release();
            impl->target = nullptr;
        }
    }
    if (msg == WM_NCDESTROY) {
        ::RemoveWindowSubclass(hwnd, subclassProc, id);
        if (impl != nullptr) {
            impl->subclassed = false;
            impl->attached = false;
            impl->hwnd = nullptr;
            impl->cb = nullptr;
        }
    }
    return ::DefSubclassProc(hwnd, msg, wp, lp);
}

void say(std::string* out, const char* text) {
    if (out != nullptr) { *out = text; }
}

}  // namespace

FileDrop::FileDrop() : impl_(std::make_unique<Impl>()) {}

std::unique_ptr<FileDrop> FileDrop::attach(void* hwndIn, DropCallback callback, const DropOptions& options, std::string* refusal) {
    HWND hwnd = static_cast<HWND>(hwndIn);
    if (hwnd == nullptr || !::IsWindow(hwnd)) { say(refusal, "there is no window to take files"); return nullptr; }
    if (!callback) { say(refusal, "no callback was given for dropped files"); return nullptr; }
    if (::GetWindowThreadProcessId(hwnd, nullptr) != ::GetCurrentThreadId()) {
        say(refusal, "files can only be accepted from the thread that owns the window");
        return nullptr;
    }
    std::unique_ptr<FileDrop> fd(new FileDrop());
    Impl& m = *fd->impl_;
    m.hwnd = hwnd;
    m.cb = std::move(callback);
    m.options = options;
    if (m.options.maxFiles == 0) { m.options.maxFiles = 1; }
    m.threadId = ::GetCurrentThreadId();

    bool wantOle = options.mode != DropOptions::Mode::DropFiles;
    if (wantOle) {
        const HRESULT hr = ::OleInitialize(nullptr);
        if (hr == S_OK || hr == S_FALSE) {
            m.oleInit = true;
        } else if (options.mode == DropOptions::Mode::Ole) {
            say(refusal, "OLE cannot be used on this thread (it already joined the multithreaded apartment)");
            return nullptr;
        } else {
            wantOle = false;   // Auto: this thread cannot do OLE; the older door still works
        }
    }
    if (wantOle) {
        if (m.options.replaceExisting) { ::RevokeDragDrop(hwnd); }
        m.target = new DropTargetObj(&m);
        const HRESULT hr = ::RegisterDragDrop(hwnd, m.target);
        if (FAILED(hr)) {
            static_cast<DropTargetObj*>(m.target)->disown();
            m.target->Release();
            m.target = nullptr;
            if (m.oleInit) { ::OleUninitialize(); m.oleInit = false; }
            say(refusal, hr == DRAGDROP_E_ALREADYREGISTERED
                             ? "the window already has a drop target of another owner (switch its own drag-and-drop off, or ask to replace it)"
                             : "the window refused to be a drop target");
            return nullptr;
        }
        m.oleRegistered = true;
        m.mode = DropOptions::Mode::Ole;
        // Only to learn that the window is being destroyed (the revoke above must come before it).
        m.subclassed = ::SetWindowSubclass(hwnd, subclassProc, kSubclassId, reinterpret_cast<DWORD_PTR>(&m)) != FALSE;
    } else {
        m.mode = DropOptions::Mode::DropFiles;
        m.subclassed = ::SetWindowSubclass(hwnd, subclassProc, kSubclassId, reinterpret_cast<DWORD_PTR>(&m)) != FALSE;
        if (!m.subclassed) {
            say(refusal, "the window could not be subclassed to receive dropped files");
            return nullptr;
        }
        ::DragAcceptFiles(hwnd, TRUE);
    }
    m.attached = true;
    return fd;
}

void FileDrop::detach() {
    Impl& m = *impl_;
    if (m.hwnd != nullptr && ::IsWindow(m.hwnd) && ::GetWindowThreadProcessId(m.hwnd, nullptr) == ::GetCurrentThreadId()) {
        if (m.mode == DropOptions::Mode::DropFiles) { ::DragAcceptFiles(m.hwnd, FALSE); }
        if (m.oleRegistered) { ::RevokeDragDrop(m.hwnd); }
        if (m.subclassed) { ::RemoveWindowSubclass(m.hwnd, subclassProc, kSubclassId); }
    }
    m.oleRegistered = false;
    m.subclassed = false;
    if (m.target != nullptr) {
        static_cast<DropTargetObj*>(m.target)->disown();
        m.target->Release();
        m.target = nullptr;
    }
    if (m.oleInit) {
        ::OleUninitialize();
        m.oleInit = false;
    }
    m.cb = nullptr;
    m.attached = false;
    m.hwnd = nullptr;
}

FileDrop::~FileDrop() {
    if (impl_) { detach(); }
}

DropOptions::Mode FileDrop::mode() const { return impl_->mode; }
bool FileDrop::attached() const { return impl_->attached; }
std::size_t FileDrop::dropsDelivered() const { return impl_->delivered; }
std::size_t FileDrop::dragsRefused() const { return impl_->refused; }
void* FileDrop::oleTargetForTest() const { return impl_->mode == DropOptions::Mode::Ole ? impl_->target : nullptr; }

#else  // not Windows: no drop door; the refusal says so

struct FileDrop::Impl {};
FileDrop::FileDrop() : impl_(std::make_unique<Impl>()) {}
std::unique_ptr<FileDrop> FileDrop::attach(void*, DropCallback, const DropOptions&, std::string* refusal) {
    if (refusal != nullptr) { *refusal = "dropping files on a window is built for Windows only"; }
    return nullptr;
}
void FileDrop::detach() {}
FileDrop::~FileDrop() = default;
DropOptions::Mode FileDrop::mode() const { return DropOptions::Mode::Auto; }
bool FileDrop::attached() const { return false; }
std::size_t FileDrop::dropsDelivered() const { return 0; }
std::size_t FileDrop::dragsRefused() const { return 0; }
void* FileDrop::oleTargetForTest() const { return nullptr; }

#endif

}  // namespace cascade::core
