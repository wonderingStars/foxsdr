// omnirig_com.cpp - the Windows COM bridge to OmniRig (screen lane S5b, row G3-18a). See cat_link.hpp.
//
// READ-ONLY BY CONSTRUCTION: this class implements only property GETs on OmniRig's IRigX (Status, Freq, FreqA, FreqB, Mode, Vfo, Split, Tx); putFreq and putMode return false without a COM call, and no
// SendCustomCommand, PortBits or Tx writer exists here. OmniRig is other people's software the person installs; nothing is downloaded. The program is started by CoCreateInstance only when the person chose OmniRig and
// the link's own thread connects; NO TEST calls this function (tests hand the link a stand-in port). UNVERIFIED against a real OmniRig: the property names are those of its type library (OmniRig.ridl, MIT, opened
// 2026-11-11 by the lane that wrote core/omnirig_client.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/cat_link.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#endif

namespace cascade::engine::wirec {

#if defined(_WIN32)
namespace {

namespace rig = cascade::core::rig;

class ComInit {
public:
    ComInit() : ok_(SUCCEEDED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~ComInit() { if (ok_) { ::CoUninitialize(); } }
    ComInit(const ComInit&) = delete;
    ComInit& operator=(const ComInit&) = delete;

private:
    bool ok_;
};

// One IDispatch property read by name; false when anything about it fails.
bool getProperty(IDispatch* d, const wchar_t* name, VARIANT& out) {
    if (d == nullptr) { return false; }
    DISPID id = 0;
    LPOLESTR n = const_cast<LPOLESTR>(name);
    if (FAILED(d->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id))) { return false; }
    DISPPARAMS none{};
    VariantInit(&out);
    return SUCCEEDED(d->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &none, &out, nullptr, nullptr));
}

class OmniRigCom final : public rig::OmniRigRigPort {
public:
    OmniRigCom(IDispatch* app, IDispatch* rigX) : app_(app), rig_(rigX) {}
    ~OmniRigCom() override {
        if (rig_ != nullptr) { rig_->Release(); }
        if (app_ != nullptr) { app_->Release(); }
    }
    std::int32_t status() override { return getLong(L"Status", rig::ST_NOTCONFIGURED); }
    std::int32_t freq() override { return getLong(L"Freq", 0); }
    std::int32_t freqA() override { return getLong(L"FreqA", 0); }
    std::int32_t freqB() override { return getLong(L"FreqB", 0); }
    std::uint32_t mode() override { return static_cast<std::uint32_t>(getLong(L"Mode", 0)); }
    std::uint32_t vfo() override { return static_cast<std::uint32_t>(getLong(L"Vfo", 0)); }
    std::uint32_t split() override { return static_cast<std::uint32_t>(getLong(L"Split", 0)); }
    std::uint32_t txState() override { return static_cast<std::uint32_t>(getLong(L"Tx", static_cast<long>(rig::PM_RX))); }
    // The only two writes of the port: never made from here.
    bool putFreq(std::int32_t) override { return false; }
    bool putMode(std::uint32_t) override { return false; }

private:
    long getLong(const wchar_t* name, long fallback) {
        VARIANT v;
        if (!getProperty(rig_, name, v)) { return fallback; }
        VARIANT l;
        VariantInit(&l);
        long out = fallback;
        if (SUCCEEDED(VariantChangeType(&l, &v, 0, VT_I4))) { out = l.lVal; }
        VariantClear(&l);
        VariantClear(&v);
        return out;
    }
    IDispatch* app_;
    IDispatch* rig_;
};

// COM must be initialised on the thread that uses the objects: the link's own thread calls this and keeps the port, so one ComInit lives as long as the thread (a static thread_local).
thread_local ComInit* g_comInit = nullptr;

}  // namespace

std::unique_ptr<cascade::core::rig::OmniRigRigPort> openOmniRigCom(int rigNumber, std::string& error) {
    if (g_comInit == nullptr) { g_comInit = new ComInit(); }   // released with the process: one per link thread, a few bytes
    CLSID clsid{};
    if (FAILED(::CLSIDFromProgID(L"OmniRig.OmniRigX", &clsid))) {
        error = CatLink::omniRigNeededSentence();   // a registry lookup only: nothing is started
        return nullptr;
    }
    IDispatch* app = nullptr;
    if (FAILED(::CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, IID_IDispatch, reinterpret_cast<void**>(&app))) || app == nullptr) {
        error = "OmniRig is installed but would not start. Start OmniRig yourself, check that its window opens, then choose OmniRig here again.";
        return nullptr;
    }
    VARIANT v;
    if (!getProperty(app, rigNumber == 2 ? L"Rig2" : L"Rig1", v) || v.vt != VT_DISPATCH || v.pdispVal == nullptr) {
        app->Release();
        VariantClear(&v);
        error = "OmniRig did not give a rig object for that slot.";
        return nullptr;
    }
    IDispatch* rigX = v.pdispVal;
    rigX->AddRef();
    VariantClear(&v);
    return std::unique_ptr<cascade::core::rig::OmniRigRigPort>(new OmniRigCom(app, rigX));
}

#else

std::unique_ptr<cascade::core::rig::OmniRigRigPort> openOmniRigCom(int, std::string& error) {
    error = "OmniRig runs on Windows only.";
    return nullptr;
}

#endif

}  // namespace cascade::engine::wirec
