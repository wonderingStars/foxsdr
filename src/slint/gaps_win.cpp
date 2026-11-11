// gaps_win.cpp - see gaps_win.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gaps_win.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#endif

namespace foxsdr::slintgaps {

#if defined(_WIN32)
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) { return std::wstring(); }
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(std::max(n, 0)), L'\0');
    if (n > 0) { ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n); }
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) { return std::string(); }
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(std::max(n, 0)), '\0');
    if (n > 0) { ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr); }
    return s;
}

struct FindCtx { DWORD pid; HWND found; };
BOOL CALLBACK enumProc(HWND h, LPARAM lp) {
    FindCtx* c = reinterpret_cast<FindCtx*>(lp);
    DWORD pid = 0;
    ::GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid || ::GetWindow(h, GW_OWNER) != nullptr) { return TRUE; }
    wchar_t cls[64] = {};
    ::GetClassNameW(h, cls, 63);
    if (std::wstring(cls) == L"ConsoleWindowClass" || std::wstring(cls) == L"IME") { return TRUE; }
    c->found = h;   // a hidden-desktop window is not "visible" to every test: the process's first top-level window with no owner is the one
    return FALSE;
}

// An IDropTarget that takes the first file of a CF_HDROP and hands its path to the callback.
class FileDrop final : public IDropTarget {
public:
    explicit FileDrop(std::function<void(const std::string&)> cb) : cb_(std::move(cb)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) { *out = static_cast<IDropTarget*>(this); AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(::InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = ::InterlockedDecrement(&refs_);
        if (n == 0) { delete this; }
        return static_cast<ULONG>(n);
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* d, DWORD, POINTL, DWORD* effect) override { *effect = hasFiles(d) ? DROPEFFECT_COPY : DROPEFFECT_NONE; return S_OK; }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override { *effect = DROPEFFECT_COPY; return S_OK; }
    HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* d, DWORD, POINTL, DWORD* effect) override {
        *effect = DROPEFFECT_NONE;
        FORMATETC fe = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM sm = {};
        if (d == nullptr || FAILED(d->GetData(&fe, &sm))) { return S_OK; }
        HDROP drop = reinterpret_cast<HDROP>(sm.hGlobal);
        const UINT n = ::DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
        if (n > 0) {
            const UINT len = ::DragQueryFileW(drop, 0, nullptr, 0);
            std::wstring w(len + 1, L'\0');
            ::DragQueryFileW(drop, 0, w.data(), len + 1);
            w.resize(len);
            if (cb_) { cb_(narrow(w)); }
            *effect = DROPEFFECT_COPY;
        }
        ::ReleaseStgMedium(&sm);
        return S_OK;
    }

private:
    static bool hasFiles(IDataObject* d) {
        FORMATETC fe = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        return d != nullptr && d->QueryGetData(&fe) == S_OK;
    }
    std::function<void(const std::string&)> cb_;
    LONG refs_ = 1;
};
}  // namespace

bool openUrl(const std::string& url) {
    if (url.empty()) { return false; }
    const HINSTANCE h = ::ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(h) > 32;
}

void* findMainWindow() {
    FindCtx c{::GetCurrentProcessId(), nullptr};
    ::EnumWindows(&enumProc, reinterpret_cast<LPARAM>(&c));
    return c.found;
}

bool installFileDrop(void* window, std::function<void(const std::string&)> onDrop) {
    HWND h = static_cast<HWND>(window);
    if (h == nullptr || !::IsWindow(h)) { return false; }
    // the window backend registers its own drop target on its window; ours replaces it (and runs on the thread that owns the window, which is this one)
    (void)::OleInitialize(nullptr);
    ::RevokeDragDrop(h);
    FileDrop* t = new FileDrop(std::move(onDrop));
    const HRESULT hr = ::RegisterDragDrop(h, t);
    t->Release();
    return SUCCEEDED(hr);
}

bool setClipboardText(const std::string& utf8) {
    const std::wstring w = widen(utf8);
    if (!::OpenClipboard(nullptr)) { return false; }
    ::EmptyClipboard();
    const std::size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    HGLOBAL g = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (g != nullptr) {
        void* p = ::GlobalLock(g);
        if (p != nullptr) {
            std::memcpy(p, w.c_str(), bytes);
            ::GlobalUnlock(g);
            ok = ::SetClipboardData(CF_UNICODETEXT, g) != nullptr;
        }
        if (!ok) { ::GlobalFree(g); }
    }
    ::CloseClipboard();
    return ok;
}

std::string clipboardText() {
    std::string out;
    if (!::OpenClipboard(nullptr)) { return out; }
    if (HANDLE h = ::GetClipboardData(CF_UNICODETEXT); h != nullptr) {
        if (const wchar_t* p = static_cast<const wchar_t*>(::GlobalLock(h)); p != nullptr) { out = narrow(p); ::GlobalUnlock(h); }
    }
    ::CloseClipboard();
    return out;
}
#else
bool openUrl(const std::string&) { return false; }
void* findMainWindow() { return nullptr; }
bool installFileDrop(void*, std::function<void(const std::string&)>) { return false; }
bool setClipboardText(const std::string&) { return false; }
std::string clipboardText() { return std::string(); }
#endif

}  // namespace foxsdr::slintgaps
