// test_file_drop.cpp - files dropped on a window (core/file_drop.hpp), on a hidden test window. Run it on the hidden desktop.
//
//   [A] pathsFromDropBlock: wide and narrow lists, non-ASCII, a surrogate pair, a cap, a cut-off block, a bad offset, a fuzz run.
//   [B] the OLE door, by direct IDropTarget calls with a hand-made data object: a drop of files, a drag that holds no files (refused,
//       and a Drop of it delivers nothing), many files and the cap, a folder, a path over 259 characters (and the classifier reading
//       it), a file that vanishes after the drop.
//   [C] the WM_DROPFILES door with a REAL message and a real HDROP (shell32's own DragFinish frees it).
//   [D] lifecycle: unregister on detach (the window can take another target), the window destroyed first, the window destroyed
//       with the registration alive, a drop after detach delivers nothing, a callback that destroys the registration, a window that
//       already has another target (refused, then replaced on request), the wrong thread, no window, no callback, a thread that is
//       a multithreaded apartment (Auto falls back to WM_DROPFILES, Ole is refused).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "core/file_drop.hpp"
#include "core/file_kind.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <objbase.h>
#include <ole2.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;
using cascade::core::DropEvent;
using cascade::core::DropOptions;
using cascade::core::FileDrop;
using cascade::core::FileKind;
using cascade::core::pathsFromDropBlock;

namespace {

// A DROPFILES block: the 20-byte header, then the list. `wide`: UTF-16 from the UTF-8 given; else the bytes as they are.
std::vector<unsigned char> block(const std::vector<std::string>& utf8, bool wide = true, bool terminate = true, std::uint32_t offset = 20) {
    std::vector<unsigned char> b(20, 0);
    std::memcpy(b.data(), &offset, 4);
    const std::uint32_t fWide = wide ? 1u : 0u;
    std::memcpy(b.data() + 16, &fWide, 4);
    for (const auto& s : utf8) {
        if (wide) {
#if defined(_WIN32)
            const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            std::wstring w(static_cast<std::size_t>(n), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
            for (wchar_t c : w) {
                b.push_back(static_cast<unsigned char>(c & 0xFF));
                b.push_back(static_cast<unsigned char>((c >> 8) & 0xFF));
            }
#endif
            b.push_back(0);
            b.push_back(0);
        } else {
            b.insert(b.end(), s.begin(), s.end());
            b.push_back(0);
        }
    }
    if (terminate) {
        b.push_back(0);
        if (wide) { b.push_back(0); }
    }
    return b;
}

void testBlock() {
    {
        std::size_t total = 0;
        bool complete = false;
        const auto p = pathsFromDropBlock(block({"C:\\a\\one.wav", "C:\\b\\two.csv"}).data(), block({"C:\\a\\one.wav", "C:\\b\\two.csv"}).size(), 4096, &total, &complete);
        CHECK(p.size() == 2 && p[0] == "C:\\a\\one.wav" && p[1] == "C:\\b\\two.csv");
        CHECK(total == 2 && complete);
    }
    {   // non-ASCII and a surrogate pair
        const std::string name = "C:\\\xCE\xB1\xCE\xB2\\\xF0\x9F\x93\xBB.wav";
        const auto b = block({name});
        const auto p = pathsFromDropBlock(b.data(), b.size());
        CHECK(p.size() == 1 && p[0] == name);
    }
    {   // narrow (ASCII bytes survive any code page)
        const auto b = block({"C:\\x.wav", "C:\\y.wav"}, false);
        const auto p = pathsFromDropBlock(b.data(), b.size());
        CHECK(p.size() == 2 && p[1] == "C:\\y.wav");
    }
    {   // the cap
        std::vector<std::string> many;
        for (int i = 0; i < 1000; ++i) { many.push_back("C:\\f" + std::to_string(i)); }
        const auto b = block(many);
        std::size_t total = 0;
        const auto p = pathsFromDropBlock(b.data(), b.size(), 10, &total);
        CHECK(p.size() == 10 && total == 1000);
        const auto none = pathsFromDropBlock(b.data(), b.size(), 0, &total);
        CHECK(none.empty() && total == 1000);
    }
    {   // cut off: a path with no NUL; the whole ones before it are kept, the cut one is not
        auto b = block({"C:\\whole.wav", "C:\\cut.wav"}, true, false);
        b.resize(b.size() - 4);   // drop the last path's NUL and a character
        bool complete = true;
        std::size_t total = 0;
        const auto p = pathsFromDropBlock(b.data(), b.size(), 4096, &total, &complete);
        CHECK(p.size() == 1 && p[0] == "C:\\whole.wav" && total == 1 && !complete);
    }
    {   // not a list
        std::size_t total = 9;
        CHECK(pathsFromDropBlock(nullptr, 100, 4096, &total).empty() && total == 0);
        const unsigned char tiny[10] = {};
        CHECK(pathsFromDropBlock(tiny, 10).empty());
        CHECK(pathsFromDropBlock(block({"C:\\a"}, true, true, 4).data(), 40).empty());                       // offset inside the header
        CHECK(pathsFromDropBlock(block({"C:\\a"}, true, true, 5000).data(), block({"C:\\a"}).size()).empty());  // offset past the end
        const auto emptyList = block({});
        CHECK(pathsFromDropBlock(emptyList.data(), emptyList.size()).empty());
    }
    {   // FUZZ: random blocks and mutated good ones
        std::mt19937 rng(99);
        const auto good = block({"C:\\one.wav", "C:\\\xCE\xB1.csv", "D:\\two"});
        for (int i = 0; i < 60000; ++i) {
            std::vector<unsigned char> b = good;
            if (i % 3 == 0) {
                b.resize(rng() % 120);
                for (auto& x : b) { x = static_cast<unsigned char>(rng()); }
            } else {
                for (int f = 0, n = 1 + static_cast<int>(rng() % 5); f < n; ++f) { b[rng() % b.size()] = static_cast<unsigned char>(rng()); }
                b.resize(rng() % (b.size() + 1));
            }
            std::size_t total = 0;
            const auto p = pathsFromDropBlock(b.data(), b.size(), 8, &total);
            if (p.size() > total) { CHECK(false); }
        }
        CHECK(true);
    }
}

#if defined(_WIN32)

// ---- a minimal data object holding either CF_HDROP or CF_TEXT -------------------------------------------------------------------
class FakeData final : public IDataObject {
public:
    FakeData(std::vector<unsigned char> hdrop, bool text) : hdrop_(std::move(hdrop)), text_(text) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDataObject) { *ppv = static_cast<IDataObject*>(this); AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override { return --ref_; }   // stack-owned
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* fe, STGMEDIUM* sm) override {
        if (!text_ && fe->cfFormat == CF_HDROP && (fe->tymed & TYMED_HGLOBAL)) {
            HGLOBAL g = ::GlobalAlloc(GMEM_MOVEABLE, hdrop_.size());
            void* m = ::GlobalLock(g);
            std::memcpy(m, hdrop_.data(), hdrop_.size());
            ::GlobalUnlock(g);
            sm->tymed = TYMED_HGLOBAL;
            sm->hGlobal = g;
            sm->pUnkForRelease = nullptr;
            return S_OK;
        }
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* fe) override {
        if (!text_ && fe->cfFormat == CF_HDROP && (fe->tymed & TYMED_HGLOBAL)) { return S_OK; }
        if (text_ && fe->cfFormat == CF_TEXT) { return S_OK; }
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return E_NOTIMPL; }

private:
    std::vector<unsigned char> hdrop_;
    bool text_;
    ULONG ref_ = 1;
};

// Another owner's drop target (what a window library registers for itself).
class ForeignTarget final : public IDropTarget {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) { *ppv = static_cast<IDropTarget*>(this); AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override { return --ref_; }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject*, DWORD, POINTL, DWORD*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject*, DWORD, POINTL, DWORD*) override { return S_OK; }
    ULONG ref_ = 1;
};

LRESULT CALLBACK testProc(HWND h, UINT m, WPARAM w, LPARAM l) { return ::DefWindowProcW(h, m, w, l); }

HWND makeWindow() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = testProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FoxSdrFileDropTest";
        ::RegisterClassW(&wc);
        registered = true;
    }
    return ::CreateWindowExW(0, L"FoxSdrFileDropTest", L"t", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
}

void pump() {
    MSG m;
    while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        ::TranslateMessage(&m);
        ::DispatchMessageW(&m);
    }
}

IDropTarget* ole(const FileDrop& d) { return static_cast<IDropTarget*>(d.oleTargetForTest()); }

DWORD enter(IDropTarget* t, IDataObject* d) {
    DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
    POINTL pt{5, 5};
    t->DragEnter(d, 0, pt, &effect);
    return effect;
}
DWORD drop(IDropTarget* t, IDataObject* d) {
    DWORD effect = DROPEFFECT_COPY;
    POINTL pt{5, 5};
    t->Drop(d, 0, pt, &effect);
    return effect;
}

void testOle() {
    HWND w = makeWindow();
    CHECK(w != nullptr);
    std::vector<DropEvent> got;
    std::string refusal;
    auto fd = FileDrop::attach(w, [&](const DropEvent& e) { got.push_back(e); }, {}, &refusal);
    CHECK(fd != nullptr);
    if (!fd) { std::printf("refusal: %s\n", refusal.c_str()); ::DestroyWindow(w); return; }
    CHECK(fd->mode() == DropOptions::Mode::Ole && fd->attached());
    IDropTarget* t = ole(*fd);
    CHECK(t != nullptr);
    if (t == nullptr) { return; }

    {   // one drag of two files
        FakeData d(block({"C:\\rec\\a.wav", "C:\\rec\\\xCE\xB1.csv"}), false);
        CHECK(enter(t, &d) == DROPEFFECT_COPY);
        DWORD effect = DROPEFFECT_COPY;
        POINTL pt{1, 1};
        CHECK(t->DragOver(0, pt, &effect) == S_OK && effect == DROPEFFECT_COPY);
        CHECK(drop(t, &d) == DROPEFFECT_COPY);
        CHECK(got.size() == 1 && got[0].paths.size() == 2 && got[0].paths[1] == "C:\\rec\\\xCE\xB1.csv" && got[0].complete);
        CHECK(fd->dropsDelivered() == 1);
    }
    {   // a drag with no files in it: the "no" cursor, counted, and a Drop of it delivers nothing
        FakeData d({}, true);
        CHECK(enter(t, &d) == DROPEFFECT_NONE);
        DWORD effect = DROPEFFECT_COPY;
        POINTL pt{1, 1};
        t->DragOver(0, pt, &effect);
        CHECK(effect == DROPEFFECT_NONE);
        CHECK(drop(t, &d) == DROPEFFECT_NONE);
        CHECK(got.size() == 1 && fd->dragsRefused() >= 1);
        CHECK(t->DragLeave() == S_OK);
    }
    {   // an empty list of files delivers nothing
        FakeData d(block({}), false);
        enter(t, &d);
        drop(t, &d);
        CHECK(got.size() == 1);
    }
    {   // many files: all of 3000 arrive, in order
        std::vector<std::string> many;
        for (int i = 0; i < 3000; ++i) { many.push_back("C:\\drop\\file" + std::to_string(i) + ".wav"); }
        FakeData d(block(many), false);
        enter(t, &d);
        drop(t, &d);
        CHECK(got.size() == 2 && got[1].paths.size() == 3000 && got[1].total == 3000);
        CHECK(got.size() == 2 && got[1].paths[2999] == "C:\\drop\\file2999.wav" && got[1].paths[0] == "C:\\drop\\file0.wav");
    }
    {   // more than the cap: 10000 files, 4096 delivered, the total says 10000
        std::vector<std::string> many;
        for (int i = 0; i < 10000; ++i) { many.push_back("C:\\d\\" + std::to_string(i)); }
        FakeData d(block(many), false);
        enter(t, &d);
        drop(t, &d);
        CHECK(got.size() == 3 && got[2].paths.size() == 4096 && got[2].total == 10000);
    }
    {   // a cut-off block is delivered with complete=false (what was whole), not dropped silently
        auto b = block({"C:\\ok.wav", "C:\\cutoff.wav"}, true, false);
        b.resize(b.size() - 4);
        FakeData d(b, false);
        enter(t, &d);
        drop(t, &d);
        CHECK(got.size() == 4 && !got[3].complete && got[3].paths.size() == 1);
    }

    // a folder, a path over 259 characters, a file that vanishes: delivered as they are, named by the classifier
    const fs::path root = fs::temp_directory_path() / ("foxsdr-test-file-drop-" + std::to_string(::GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "folder");
    const auto u8s = [](const fs::path& p) { const auto s = p.u8string(); return std::string(reinterpret_cast<const char*>(s.data()), s.size()); };
    {
        FakeData d(block({u8s(root / "folder")}), false);
        enter(t, &d);
        drop(t, &d);
        CHECK(got.size() == 5 && got[4].paths.size() == 1);
        CHECK(got.size() == 5 && cascade::core::classifyFile(got[4].paths[0]).kind == FileKind::Folder);
    }
    {
        fs::path deep = root;
        for (int i = 0; i < 5; ++i) { deep /= std::string(60, 'e'); }
        const std::string longFile = u8s(deep) + "\\r.wav";
        CHECK(longFile.size() > 259);
        fs::create_directories(cascade::core::fsPathFromUtf8(u8s(deep)), ec);
        {
            std::vector<unsigned char> hdr(44, 0);
            std::memcpy(hdr.data(), "RIFF", 4); std::memcpy(hdr.data() + 8, "WAVEfmt ", 8);
            hdr[16] = 16; hdr[20] = 1; hdr[22] = 2; hdr[24] = 0x80; hdr[25] = 0xBB; hdr[34] = 16;
            std::memcpy(hdr.data() + 36, "data", 4);
            std::ofstream out(cascade::core::fsPathFromUtf8(longFile), std::ios::binary);
            out.write(reinterpret_cast<const char*>(hdr.data()), 44);
        }
        FakeData d(block({longFile}), false);
        enter(t, &d);
        drop(t, &d);
        CHECK(got.size() == 6 && got[5].paths.size() == 1 && got[5].paths[0] == longFile);
        CHECK(got.size() == 6 && cascade::core::classifyFile(got[5].paths[0]).kind == FileKind::IqRecording);
        fs::remove_all(fs::path(std::wstring(LR"(\\?\)") + (root / std::string(60, 'e')).wstring()), ec);
    }
    {
        const fs::path f = root / "gone.wav";
        { std::ofstream(f, std::ios::binary) << "RIFF"; }
        FakeData d(block({u8s(f)}), false);
        enter(t, &d);
        fs::remove(f, ec);   // vanishes after the drag started, before the drop is read
        drop(t, &d);
        CHECK(got.size() == 7 && cascade::core::classifyFile(got[6].paths[0]).kind == FileKind::Missing);
    }
    fs::remove_all(root, ec);

    // unregister on detach: the window can take another target now, and a Drop of ours delivers nothing
    const std::size_t before = got.size();
    fd->detach();
    CHECK(!fd->attached());
    ForeignTarget other;
    CHECK(::RegisterDragDrop(w, &other) == S_OK);   // would be DRAGDROP_E_ALREADYREGISTERED if we had not revoked
    ::RevokeDragDrop(w);
    {
        FakeData d(block({"C:\\late.wav"}), false);
        drop(t, &d);   // the object may outlive the registration (OLE holds references mid-drag): inert
        CHECK(got.size() == before);
    }
    fd.reset();
    ::DestroyWindow(w);
    pump();
}

void testDropFiles() {
    HWND w = makeWindow();
    std::vector<DropEvent> got;
    DropOptions o;
    o.mode = DropOptions::Mode::DropFiles;
    std::string refusal;
    auto fd = FileDrop::attach(w, [&](const DropEvent& e) { got.push_back(e); }, o, &refusal);
    CHECK(fd != nullptr);
    if (!fd) { std::printf("refusal: %s\n", refusal.c_str()); return; }
    CHECK(fd->mode() == DropOptions::Mode::DropFiles && fd->oleTargetForTest() == nullptr);
    CHECK((::GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_ACCEPTFILES) != 0);   // DragAcceptFiles ran
    {
        const auto b = block({"C:\\x\\one.wav", "C:\\x\\\xCE\xB2.csv"});
        HGLOBAL g = ::GlobalAlloc(GHND, b.size());
        void* m = ::GlobalLock(g);
        std::memcpy(m, b.data(), b.size());
        ::GlobalUnlock(g);
        ::SendMessageW(w, WM_DROPFILES, reinterpret_cast<WPARAM>(g), 0);   // a real message through the real window procedure
        CHECK(got.size() == 1 && got[0].paths.size() == 2 && got[0].paths[1] == "C:\\x\\\xCE\xB2.csv");
    }
    {   // posted rather than sent: arrives through the queue on this thread
        const auto b = block({"C:\\x\\two.wav"});
        HGLOBAL g = ::GlobalAlloc(GHND, b.size());
        void* m = ::GlobalLock(g);
        std::memcpy(m, b.data(), b.size());
        ::GlobalUnlock(g);
        ::PostMessageW(w, WM_DROPFILES, reinterpret_cast<WPARAM>(g), 0);
        pump();
        CHECK(got.size() == 2 && got[1].paths[0] == "C:\\x\\two.wav");
    }
    {   // many files, over the cap
        std::vector<std::string> many;
        for (int i = 0; i < 6000; ++i) { many.push_back("C:\\q\\" + std::to_string(i)); }
        const auto b = block(many);
        HGLOBAL g = ::GlobalAlloc(GHND, b.size());
        void* m = ::GlobalLock(g);
        std::memcpy(m, b.data(), b.size());
        ::GlobalUnlock(g);
        ::SendMessageW(w, WM_DROPFILES, reinterpret_cast<WPARAM>(g), 0);
        CHECK(got.size() == 3 && got[2].paths.size() == 4096 && got[2].total == 6000);
    }
    {   // a drop with nothing in it: delivers nothing
        const auto b = block({});
        HGLOBAL g = ::GlobalAlloc(GHND, b.size());
        void* m = ::GlobalLock(g);
        std::memcpy(m, b.data(), b.size());
        ::GlobalUnlock(g);
        ::SendMessageW(w, WM_DROPFILES, reinterpret_cast<WPARAM>(g), 0);
        CHECK(got.size() == 3);
    }
    // detach: the window stops accepting and the subclass is gone
    fd->detach();
    CHECK((::GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_ACCEPTFILES) == 0);
    {
        const auto b = block({"C:\\x\\three.wav"});
        HGLOBAL g = ::GlobalAlloc(GHND, b.size());
        void* m = ::GlobalLock(g);
        std::memcpy(m, b.data(), b.size());
        ::GlobalUnlock(g);
        ::SendMessageW(w, WM_DROPFILES, reinterpret_cast<WPARAM>(g), 0);   // nobody handles it now; the default procedure ignores it
        ::GlobalFree(g);
        CHECK(got.size() == 3);
    }
    fd.reset();
    ::DestroyWindow(w);
    pump();
}

void testLifecycle() {
    {   // the window destroyed FIRST (OLE mode): the registration is revoked by the module, nothing crashes, the object says detached
        HWND w = makeWindow();
        auto fd = FileDrop::attach(w, [](const DropEvent&) {});
        CHECK(fd != nullptr && fd->attached());
        ::DestroyWindow(w);
        pump();
        CHECK(fd != nullptr && !fd->attached());
        fd.reset();   // must not throw or fault
        CHECK(true);
    }
    {   // the window destroyed first (WM_DROPFILES mode)
        HWND w = makeWindow();
        DropOptions o;
        o.mode = DropOptions::Mode::DropFiles;
        auto fd = FileDrop::attach(w, [](const DropEvent&) {}, o);
        CHECK(fd != nullptr);
        ::DestroyWindow(w);
        pump();
        CHECK(fd != nullptr && !fd->attached());
        fd.reset();
        CHECK(true);
    }
    {   // a window with another owner's target: refused with the reason, and replaced when asked
        HWND w = makeWindow();
        ForeignTarget other;
        CHECK(::RegisterDragDrop(w, &other) == S_OK);
        std::string refusal;
        auto fd = FileDrop::attach(w, [](const DropEvent&) {}, {}, &refusal);
        CHECK(fd == nullptr);
        CHECK(refusal.find("already") != std::string::npos);
        DropOptions o;
        o.replaceExisting = true;
        auto fd2 = FileDrop::attach(w, [](const DropEvent&) {}, o, &refusal);
        CHECK(fd2 != nullptr && fd2->mode() == DropOptions::Mode::Ole);
        fd2.reset();
        ::DestroyWindow(w);
        pump();
    }
    {   // a callback that destroys the registration it was called by
        HWND w = makeWindow();
        std::unique_ptr<FileDrop> fd;
        int calls = 0;
        fd = FileDrop::attach(w, [&](const DropEvent&) { ++calls; fd.reset(); });
        CHECK(fd != nullptr);
        IDropTarget* t = ole(*fd);
        FakeData d(block({"C:\\a.wav"}), false);
        enter(t, &d);
        drop(t, &d);
        CHECK(calls == 1 && fd == nullptr);
        ::DestroyWindow(w);
        pump();
    }
    {   // no window, a dead window, no callback
        std::string refusal;
        CHECK(FileDrop::attach(nullptr, [](const DropEvent&) {}, {}, &refusal) == nullptr && !refusal.empty());
        HWND w = makeWindow();
        ::DestroyWindow(w);
        CHECK(FileDrop::attach(w, [](const DropEvent&) {}, {}, &refusal) == nullptr && !refusal.empty());
        HWND w2 = makeWindow();
        CHECK(FileDrop::attach(w2, cascade::core::DropCallback(), {}, &refusal) == nullptr);
        ::DestroyWindow(w2);
    }
    {   // the wrong thread: refused with a fact
        HWND w = makeWindow();
        std::string refusal;
        std::unique_ptr<FileDrop> fromOther;
        std::thread([&] { fromOther = FileDrop::attach(w, [](const DropEvent&) {}, {}, &refusal); }).join();
        CHECK(fromOther == nullptr && refusal.find("thread") != std::string::npos);
        ::DestroyWindow(w);
    }
    {   // a multithreaded-apartment thread cannot do OLE: Auto falls back to WM_DROPFILES, Ole is refused
        std::string refusalOle;
        DropOptions::Mode autoMode = DropOptions::Mode::Ole;
        bool autoOk = false, oleRefused = false;
        std::thread([&] {
            ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            HWND w = makeWindow();
            {
                auto fd = FileDrop::attach(w, [](const DropEvent&) {});
                autoOk = fd != nullptr;
                if (fd) { autoMode = fd->mode(); }
            }
            {
                DropOptions o;
                o.mode = DropOptions::Mode::Ole;
                oleRefused = FileDrop::attach(w, [](const DropEvent&) {}, o, &refusalOle) == nullptr;
            }
            ::DestroyWindow(w);
            ::CoUninitialize();
        }).join();
        CHECK(autoOk && autoMode == DropOptions::Mode::DropFiles);
        CHECK(oleRefused && !refusalOle.empty());
    }
}

#endif  // _WIN32

}  // namespace

int main() {
    testBlock();
#if defined(_WIN32)
    ::OleInitialize(nullptr);   // this test's own thread is an STA, as the window's thread is
    testOle();
    testDropFiles();
    testLifecycle();
    ::OleUninitialize();
#endif
    return testSummary("test_file_drop");
}
