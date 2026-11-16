// lanes2_folder.cpp - see lanes2_folder.hpp. UNVERIFIED on a real desktop: no test may raise this dialog (it is proved by the mouse drive).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "lanes2_folder.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shobjidl.h>
#endif

namespace foxsdr::slints2 {

std::string chooseFolderDialog() {
#if defined(_WIN32)
    std::string out;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) && dlg != nullptr) {
        DWORD opts = 0;
        if (SUCCEEDED(dlg->GetOptions(&opts))) { dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST); }
        if (SUCCEEDED(dlg->Show(GetActiveWindow()))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item != nullptr) {
                PWSTR w = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &w)) && w != nullptr) {
                    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
                    if (n > 1) {
                        out.resize(static_cast<std::size_t>(n));
                        WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
                        out.resize(static_cast<std::size_t>(n - 1));
                    }
                    CoTaskMemFree(w);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(init)) { CoUninitialize(); }
    return out;
#else
    return std::string();
#endif
}

}  // namespace foxsdr::slints2
