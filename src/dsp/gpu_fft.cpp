// GpuSpectrumEngine: see gpu_fft.hpp for the contract and gpu_fft_shader.hpp for the HLSL.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/gpu_fft.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "dsp/fft.hpp"
#include "dsp/gpu_fft_shader.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#endif

namespace cascade::dsp {

namespace {

using Clock = std::chrono::steady_clock;

bool isPow2(std::size_t n) { return n != 0 && (n & (n - 1)) == 0; }

std::string hex32(long v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08lX", static_cast<unsigned long>(v));
    return b;
}

// The CPU path: SpectrumEstimator::process with alpha = 1, frame by frame. Same operations in the same order, so it is bit-identical.
void cpuFrames(ComplexFFT& fft, const float* window, std::vector<std::complex<float>>& windowed, std::vector<std::complex<float>>& spectrum,
               const std::complex<float>* frames, std::size_t nFrames, float* outDb) {
    const std::size_t n = fft.size();
    const float invN2 = 1.0f / (static_cast<float>(n) * static_cast<float>(n));
    const std::size_t half = n / 2;
    for (std::size_t f = 0; f < nFrames; ++f) {
        const std::complex<float>* in = frames + f * n;
        float* out = outDb + f * n;
        for (std::size_t i = 0; i < n; ++i) { windowed[i] = in[i] * window[i]; }
        fft.forward(windowed.data(), spectrum.data());
        for (std::size_t i = 0; i < n; ++i) {
            std::size_t k = i + half;
            if (k >= n) { k -= n; }
            const float p = std::norm(spectrum[k]) * invN2;
            out[i] = 10.0f * std::log10(p > 1e-20f ? p : 1e-20f);
        }
    }
}

}  // namespace

const char* gpuFftShaderSource() { return gpufft_detail::kShaderSource; }

void gpuFftCpuReference(const std::complex<float>* frames, std::size_t nFrames, std::size_t fftSize, const float* window, float* outDb) {
    ComplexFFT fft(fftSize);
    std::vector<std::complex<float>> a(fftSize), b(fftSize);
    cpuFrames(fft, window, a, b, frames, nFrames, outDb);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Windows: the Direct3D 11 side
// ---------------------------------------------------------------------------------------------------------------------------------
#ifdef _WIN32
namespace {

using Microsoft::WRL::ComPtr;

struct Api {
    HMODULE d3d11 = nullptr;
    HMODULE dxgi = nullptr;
    HMODULE compiler = nullptr;
    PFN_D3D11_CREATE_DEVICE createDevice = nullptr;
    HRESULT(WINAPI* createFactory)(REFIID, void**) = nullptr;
    pD3DCompile compile = nullptr;
    std::string error;  // why something is missing
};

HMODULE loadSystem(const wchar_t* name) {
    // System32 only (never the current directory): a planted DLL of the same name is not picked up.
    return LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

const Api& api() {
    static const Api a = [] {
        Api r;
        r.d3d11 = loadSystem(L"d3d11.dll");
        r.dxgi = loadSystem(L"dxgi.dll");
        r.compiler = loadSystem(L"d3dcompiler_47.dll");
        if (r.d3d11 == nullptr) { r.error = "d3d11.dll not available"; }
        else if (r.dxgi == nullptr) { r.error = "dxgi.dll not available"; }
        else if (r.compiler == nullptr) { r.error = "d3dcompiler_47.dll not available"; }
        if (r.error.empty()) {
            r.createDevice = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(r.d3d11, "D3D11CreateDevice"));
            r.createFactory = reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(GetProcAddress(r.dxgi, "CreateDXGIFactory1"));
            r.compile = reinterpret_cast<pD3DCompile>(GetProcAddress(r.compiler, "D3DCompile"));
            if (r.createDevice == nullptr || r.createFactory == nullptr || r.compile == nullptr) {
                r.error = "Direct3D entry points missing";
            }
        }
        return r;
    }();
    return a;
}

std::string wideToUtf8(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) { return {}; }
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// Every adapter DXGI lists, with the COM pointer when `keep` is non-null.
std::vector<GpuAdapterInfo> listAdapters(std::vector<ComPtr<IDXGIAdapter1>>* keep) {
    std::vector<GpuAdapterInfo> out;
    const Api& a = api();
    if (!a.error.empty() || a.createFactory == nullptr) { return out; }
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(a.createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf())))) { return out; }
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> ad;
        if (factory->EnumAdapters1(i, ad.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) { break; }
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(ad->GetDesc1(&d))) { continue; }
        GpuAdapterInfo info;
        info.index = static_cast<int>(i);
        info.name = wideToUtf8(d.Description);
        info.vendorId = d.VendorId;
        info.deviceId = d.DeviceId;
        info.dedicatedVideoBytes = static_cast<std::uint64_t>(d.DedicatedVideoMemory);
        // The software rasteriser: the DXGI flag, or the Microsoft vendor id with the Basic Render / WARP device id.
        info.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || (d.VendorId == 0x1414 && d.DeviceId == 0x8c);
        out.push_back(info);
        if (keep != nullptr) { keep->push_back(ad); }
    }
    return out;
}

enum class Kernel { First2, First4, Stage4 };

struct Pass {
    Kernel kernel = Kernel::Stage4;
    ComPtr<ID3D11Buffer> cb;
};

struct GpuSlotRes {
    ComPtr<ID3D11Buffer> staging;
    ComPtr<ID3D11Query> done;
    ComPtr<ID3D11Query> disjoint;
    ComPtr<ID3D11Query> tsBegin;
    ComPtr<ID3D11Query> tsEnd;
};

struct CbData {
    UINT n, ns, log2ns, pad0;
    float invN2, p1, p2, p3;
};

}  // namespace
#endif // _WIN32 (the Direct3D helpers above)

#ifdef _WIN32
namespace {
// Everything that dies with the device.
struct GpuState {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    GpuAdapterInfo adapter;
    ComPtr<ID3D11ComputeShader> csFirst2, csFirst4, csStage4, csPower;
    ComPtr<ID3D11Buffer> input, tw, win, bufA, bufB, out;
    ComPtr<ID3D11ShaderResourceView> inputSrv, twSrv, winSrv, aSrv, bSrv;
    ComPtr<ID3D11UnorderedAccessView> aUav, bUav, outUav;
    std::vector<Pass> passes;
    ComPtr<ID3D11Buffer> powerCb;
    std::vector<GpuSlotRes> slots;
    bool finalIsB = false;  // where the last FFT pass left the result
};
}  // namespace
#else
namespace {
struct GpuState {};
}  // namespace
#endif

std::vector<GpuAdapterInfo> enumerateGpuAdapters() {
#ifdef _WIN32
    return listAdapters(nullptr);
#else
    return {};
#endif
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Impl (every method is called with `mu` held)
// ---------------------------------------------------------------------------------------------------------------------------------
struct GpuSpectrumEngine::Impl {
    struct Slot {
        std::uint64_t ticket = 0;
        std::size_t frames = 0;
        int idx = 0;
        GpuFftBackend backend = GpuFftBackend::Cpu;
        bool accounted = false;
    };

    mutable std::mutex mu;
    GpuFftConfig cfg;
    std::size_t n = 0;
    std::size_t maxBatch = 1;
    std::vector<float> window;
    GpuFftMode mode = GpuFftMode::Cpu;

    ComplexFFT cpuFft;
    std::vector<std::complex<float>> cpuA, cpuB;
    std::vector<std::vector<float>> cpuOut;  // per ring index

    std::deque<Slot> ring;
    std::vector<bool> used;
    std::uint64_t nextTicket = 1;

    GpuFftStats st;
    std::string lastError;
    std::string shaderLog;
    GpuAdapterInfo adapterInfo;

    std::unique_ptr<GpuState> gpu;
    int recoverStreak = 0;     // device losses since the last batch that finished on the GPU
    int createFailures = 0;    // bring-up failures since the last success
    bool wasLost = false;
    bool forcedLost = false;
    int forceCreateFail = 0;
    Clock::time_point nextRetry{};
    Clock::time_point windowStart = Clock::now();

    explicit Impl(const GpuFftConfig& c) : cfg(c), n(c.fftSize), mode(c.mode), cpuFft(c.fftSize) {}

    // ---- CPU path
    void cpuCompute(const std::complex<float>* frames, std::size_t nFrames, float* out) {
        cpuFrames(cpuFft, window.data(), cpuA, cpuB, frames, nFrames, out);
    }

    // ---- budget
    void refreshWindow() {
        const auto now = Clock::now();
        if (now - windowStart >= std::chrono::seconds(1)) {
            windowStart = now;
            st.gpuSecondsThisWindow = 0.0;
        }
    }

    bool gpuAllowedFor() const { return mode == GpuFftMode::Gpu || (mode == GpuFftMode::Auto && n >= cfg.autoMinFftSize); }

    int freeIndex() const {
        for (std::size_t i = 0; i < used.size(); ++i) {
            if (!used[i]) { return static_cast<int>(i); }
        }
        return -1;
    }

#ifdef _WIN32
    bool failHr(const char* what, HRESULT hr) {
        lastError = std::string(what) + " failed (" + hex32(static_cast<long>(hr)) + ")";
        return false;
    }

    ComPtr<ID3D11ComputeShader> compileShader(GpuState& g, const std::string& src, const char* entry) {
        ComPtr<ID3DBlob> code, err;
        const HRESULT hr = api().compile(src.data(), src.size(), "gpu_fft.hlsl", nullptr, nullptr, entry, "cs_5_0",
                                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.GetAddressOf(), err.GetAddressOf());
        if (err != nullptr && err->GetBufferSize() > 0) {
            shaderLog.append(entry).append(": ").append(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize()).append("\n");
        }
        if (FAILED(hr) || code == nullptr) {
            lastError = std::string("shader compile failed for ") + entry + " (" + hex32(static_cast<long>(hr)) + "); see shaderLog()";
            return nullptr;
        }
        ComPtr<ID3D11ComputeShader> cs;
        const HRESULT h2 = g.dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, cs.GetAddressOf());
        if (FAILED(h2)) {
            failHr("CreateComputeShader", h2);
            return nullptr;
        }
        return cs;
    }

    ComPtr<ID3D11Buffer> makeBuffer(GpuState& g, UINT bytes, UINT stride, D3D11_USAGE usage, UINT bind, UINT cpuAccess, const void* init, const char* what) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = bytes;
        d.Usage = usage;
        d.BindFlags = bind;
        d.CPUAccessFlags = cpuAccess;
        d.MiscFlags = stride != 0 ? D3D11_RESOURCE_MISC_BUFFER_STRUCTURED : 0;
        d.StructureByteStride = stride;
        D3D11_SUBRESOURCE_DATA sd{};
        sd.pSysMem = init;
        ComPtr<ID3D11Buffer> b;
        const HRESULT hr = g.dev->CreateBuffer(&d, init != nullptr ? &sd : nullptr, b.GetAddressOf());
        if (FAILED(hr)) { failHr(what, hr); return nullptr; }
        return b;
    }

    ComPtr<ID3D11ShaderResourceView> makeSrv(GpuState& g, ID3D11Buffer* b, UINT count) {
        D3D11_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        d.Buffer.FirstElement = 0;
        d.Buffer.NumElements = count;
        ComPtr<ID3D11ShaderResourceView> v;
        const HRESULT hr = g.dev->CreateShaderResourceView(b, &d, v.GetAddressOf());
        if (FAILED(hr)) { failHr("CreateShaderResourceView", hr); return nullptr; }
        return v;
    }

    ComPtr<ID3D11UnorderedAccessView> makeUav(GpuState& g, ID3D11Buffer* b, UINT count) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        d.Buffer.FirstElement = 0;
        d.Buffer.NumElements = count;
        ComPtr<ID3D11UnorderedAccessView> v;
        const HRESULT hr = g.dev->CreateUnorderedAccessView(b, &d, v.GetAddressOf());
        if (FAILED(hr)) { failHr("CreateUnorderedAccessView", hr); return nullptr; }
        return v;
    }

    ComPtr<ID3D11Buffer> makeCb(GpuState& g, UINT ns, UINT log2ns) {
        CbData c{};
        c.n = static_cast<UINT>(n);
        c.ns = ns;
        c.log2ns = log2ns;
        c.invN2 = 1.0f / (static_cast<float>(n) * static_cast<float>(n));
        return makeBuffer(g, sizeof(CbData), 0, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, &c, "CreateBuffer(constants)");
    }

    // Brings the whole GPU path up. On any failure lastError says why and nothing is left half-built.
    bool initGpu() {
        if (forceCreateFail > 0) {
            --forceCreateFail;
            lastError = "GPU bring-up refused (test hook)";
            return false;
        }
        const Api& a = api();
        if (!a.error.empty()) { lastError = a.error; return false; }
        std::vector<ComPtr<IDXGIAdapter1>> ads;
        const std::vector<GpuAdapterInfo> infos = listAdapters(&ads);
        if (infos.empty()) { lastError = "no display adapter listed by DXGI"; return false; }
        int pick = -1;
        if (cfg.adapterIndex >= 0) {
            for (std::size_t i = 0; i < infos.size(); ++i) {
                if (infos[i].index == cfg.adapterIndex) { pick = static_cast<int>(i); }
            }
            if (pick < 0) { lastError = "adapter index " + std::to_string(cfg.adapterIndex) + " not found"; return false; }
            if (infos[static_cast<std::size_t>(pick)].software) { lastError = "refusing a software adapter"; return false; }
        } else {
            for (std::size_t i = 0; i < infos.size(); ++i) {
                if (infos[i].software) { continue; }
                if (pick < 0 || infos[i].dedicatedVideoBytes > infos[static_cast<std::size_t>(pick)].dedicatedVideoBytes) { pick = static_cast<int>(i); }
            }
            if (pick < 0) { lastError = "no hardware adapter (only a software adapter is present)"; return false; }
        }

        auto g = std::make_unique<GpuState>();
        g->adapter = infos[static_cast<std::size_t>(pick)];
        const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
        D3D_FEATURE_LEVEL got{};
        HRESULT hr = a.createDevice(ads[static_cast<std::size_t>(pick)].Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_SINGLETHREADED,
                                    &want, 1, D3D11_SDK_VERSION, g->dev.GetAddressOf(), &got, g->ctx.GetAddressOf());
        if (FAILED(hr)) { return failHr("D3D11CreateDevice", hr); }
        if (cfg.lowPriority) {
            ComPtr<IDXGIDevice> dxgiDev;
            if (SUCCEEDED(g->dev.As(&dxgiDev))) { dxgiDev->SetGPUThreadPriority(-7); }  // lowest; failure is harmless
        }

        shaderLog.clear();
        const std::string src = cfg.shaderSourceOverride.empty() ? std::string(gpufft_detail::kShaderSource) : cfg.shaderSourceOverride;
        g->csFirst2 = compileShader(*g, src, "CSFirst2");
        if (g->csFirst2 == nullptr) { return false; }
        g->csFirst4 = compileShader(*g, src, "CSFirst4");
        if (g->csFirst4 == nullptr) { return false; }
        g->csStage4 = compileShader(*g, src, "CSStage4");
        if (g->csStage4 == nullptr) { return false; }
        g->csPower = compileShader(*g, src, "CSPower");
        if (g->csPower == nullptr) { return false; }

        const UINT total = static_cast<UINT>(maxBatch * n);
        // Twiddles in double, rounded once.
        std::vector<float> tw(2 * n);
        for (std::size_t m = 0; m < n; ++m) {
            const double ang = -6.283185307179586476925286766559 * static_cast<double>(m) / static_cast<double>(n);
            tw[2 * m] = static_cast<float>(std::cos(ang));
            tw[2 * m + 1] = static_cast<float>(std::sin(ang));
        }
        g->input = makeBuffer(*g, total * 8, 8, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE, nullptr, "CreateBuffer(input)");
        g->tw = makeBuffer(*g, static_cast<UINT>(n * 8), 8, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, tw.data(), "CreateBuffer(twiddles)");
        g->win = makeBuffer(*g, static_cast<UINT>(n * 4), 4, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, window.data(), "CreateBuffer(window)");
        g->bufA = makeBuffer(*g, total * 8, 8, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, nullptr, "CreateBuffer(A)");
        g->bufB = makeBuffer(*g, total * 8, 8, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, nullptr, "CreateBuffer(B)");
        g->out = makeBuffer(*g, total * 4, 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, nullptr, "CreateBuffer(out)");
        if (!g->input || !g->tw || !g->win || !g->bufA || !g->bufB || !g->out) { return false; }
        g->inputSrv = makeSrv(*g, g->input.Get(), total);
        g->twSrv = makeSrv(*g, g->tw.Get(), static_cast<UINT>(n));
        g->winSrv = makeSrv(*g, g->win.Get(), static_cast<UINT>(n));
        g->aSrv = makeSrv(*g, g->bufA.Get(), total);
        g->bSrv = makeSrv(*g, g->bufB.Get(), total);
        g->aUav = makeUav(*g, g->bufA.Get(), total);
        g->bUav = makeUav(*g, g->bufB.Get(), total);
        g->outUav = makeUav(*g, g->out.Get(), total);
        if (!g->inputSrv || !g->twSrv || !g->winSrv || !g->aSrv || !g->bSrv || !g->aUav || !g->bUav || !g->outUav) { return false; }

        // The pass list. Ns = the sub-transform length finished so far.
        int log2n = 0;
        while ((std::size_t{1} << log2n) < n) { ++log2n; }
        UINT ns = 1;
        UINT log2ns = 0;
        {
            Pass p;
            p.kernel = (log2n & 1) != 0 ? Kernel::First2 : Kernel::First4;
            p.cb = makeCb(*g, 1, 0);
            if (!p.cb) { return false; }
            g->passes.push_back(p);
            if ((log2n & 1) != 0) { ns = 2; log2ns = 1; } else { ns = 4; log2ns = 2; }
        }
        while (ns < n) {
            Pass p;
            p.kernel = Kernel::Stage4;
            p.cb = makeCb(*g, ns, log2ns);
            if (!p.cb) { return false; }
            g->passes.push_back(p);
            ns *= 4;
            log2ns += 2;
        }
        g->finalIsB = (g->passes.size() % 2) == 0;  // pass 0 writes A, pass 1 writes B, ...
        g->powerCb = makeCb(*g, 1, 0);
        if (!g->powerCb) { return false; }

        g->slots.resize(static_cast<std::size_t>(cfg.ringSlots));
        for (auto& s : g->slots) {
            s.staging = makeBuffer(*g, total * 4, 4, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, nullptr, "CreateBuffer(staging)");
            if (!s.staging) { return false; }
            auto q = [&](D3D11_QUERY type, ComPtr<ID3D11Query>& out) {
                D3D11_QUERY_DESC qd{};
                qd.Query = type;
                const HRESULT h = g->dev->CreateQuery(&qd, out.GetAddressOf());
                if (FAILED(h)) { failHr("CreateQuery", h); return false; }
                return true;
            };
            if (!q(D3D11_QUERY_EVENT, s.done) || !q(D3D11_QUERY_TIMESTAMP_DISJOINT, s.disjoint) || !q(D3D11_QUERY_TIMESTAMP, s.tsBegin) ||
                !q(D3D11_QUERY_TIMESTAMP, s.tsEnd)) {
                return false;
            }
        }
        adapterInfo = g->adapter;
        gpu = std::move(g);
        return true;
    }

    // Issue one batch. False on failure (lastError set); nothing is left in flight then.
    bool gpuIssue(int idx, const std::complex<float>* frames, std::size_t nFrames) {
        GpuState& g = *gpu;
        GpuSlotRes& sr = g.slots[static_cast<std::size_t>(idx)];
        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT hr = g.ctx->Map(g.input.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        if (FAILED(hr)) { return failHr("Map(input)", hr); }
        std::memcpy(m.pData, frames, nFrames * n * sizeof(std::complex<float>));
        g.ctx->Unmap(g.input.Get(), 0);

        g.ctx->Begin(sr.disjoint.Get());
        g.ctx->End(sr.tsBegin.Get());
        ID3D11ShaderResourceView* nullSrv[4] = {nullptr, nullptr, nullptr, nullptr};
        ID3D11UnorderedAccessView* nullUav[2] = {nullptr, nullptr};
        const std::size_t groupSize = 256;
        for (std::size_t i = 0; i < g.passes.size(); ++i) {
            const Pass& p = g.passes[i];
            ID3D11ComputeShader* cs = p.kernel == Kernel::First2 ? g.csFirst2.Get() : (p.kernel == Kernel::First4 ? g.csFirst4.Get() : g.csStage4.Get());
            const bool dstIsA = (i % 2) == 0;
            ID3D11ShaderResourceView* srcSrv = i == 0 ? nullptr : (dstIsA ? g.bSrv.Get() : g.aSrv.Get());
            ID3D11ShaderResourceView* srvs[4] = {g.inputSrv.Get(), g.twSrv.Get(), g.winSrv.Get(), srcSrv};
            ID3D11UnorderedAccessView* uavs[2] = {dstIsA ? g.aUav.Get() : g.bUav.Get(), nullptr};
            ID3D11Buffer* cb = p.cb.Get();
            g.ctx->CSSetShader(cs, nullptr, 0);
            g.ctx->CSSetConstantBuffers(0, 1, &cb);
            g.ctx->CSSetShaderResources(0, 4, srvs);
            g.ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
            const std::size_t work = p.kernel == Kernel::First2 ? n / 2 : n / 4;
            g.ctx->Dispatch(static_cast<UINT>((work + groupSize - 1) / groupSize), static_cast<UINT>(nFrames), 1);
            g.ctx->CSSetShaderResources(0, 4, nullSrv);
            g.ctx->CSSetUnorderedAccessViews(0, 2, nullUav, nullptr);
        }
        {
            ID3D11ShaderResourceView* srvs[4] = {nullptr, nullptr, nullptr, g.finalIsB ? g.bSrv.Get() : g.aSrv.Get()};
            ID3D11UnorderedAccessView* uavs[2] = {nullptr, g.outUav.Get()};
            ID3D11Buffer* cb = g.powerCb.Get();
            g.ctx->CSSetShader(g.csPower.Get(), nullptr, 0);
            g.ctx->CSSetConstantBuffers(0, 1, &cb);
            g.ctx->CSSetShaderResources(0, 4, srvs);
            g.ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
            g.ctx->Dispatch(static_cast<UINT>((n + groupSize - 1) / groupSize), static_cast<UINT>(nFrames), 1);
            g.ctx->CSSetShaderResources(0, 4, nullSrv);
            g.ctx->CSSetUnorderedAccessViews(0, 2, nullUav, nullptr);
        }
        g.ctx->End(sr.tsEnd.Get());
        g.ctx->End(sr.disjoint.Get());
        D3D11_BOX box{};
        box.left = 0;
        box.right = static_cast<UINT>(nFrames * n * sizeof(float));
        box.top = 0;
        box.bottom = 1;
        box.front = 0;
        box.back = 1;
        g.ctx->CopySubresourceRegion(sr.staging.Get(), 0, 0, 0, 0, g.out.Get(), 0, &box);
        g.ctx->End(sr.done.Get());
        g.ctx->Flush();
        return true;
    }

    bool slotDone(const Slot& s) {
        GpuSlotRes& sr = gpu->slots[static_cast<std::size_t>(s.idx)];
        BOOL b = FALSE;
        const HRESULT hr = gpu->ctx->GetData(sr.done.Get(), &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        return hr == S_OK && b != FALSE;
    }

    void account(Slot& s) {
        if (s.accounted || !gpu) { return; }
        s.accounted = true;
        GpuSlotRes& sr = gpu->slots[static_cast<std::size_t>(s.idx)];
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 t0 = 0, t1 = 0;
        if (gpu->ctx->GetData(sr.disjoint.Get(), &dj, sizeof dj, 0) != S_OK) { return; }
        if (gpu->ctx->GetData(sr.tsBegin.Get(), &t0, sizeof t0, 0) != S_OK) { return; }
        if (gpu->ctx->GetData(sr.tsEnd.Get(), &t1, sizeof t1, 0) != S_OK) { return; }
        if (dj.Disjoint || dj.Frequency == 0 || t1 < t0) { return; }
        const double sec = static_cast<double>(t1 - t0) / static_cast<double>(dj.Frequency);
        st.gpuSecondsLastBatch = sec;
        st.gpuSecondsTotal += sec;
        refreshWindow();
        st.gpuSecondsThisWindow += sec;
    }

    long removedReason() {
        if (forcedLost) { return static_cast<long>(DXGI_ERROR_DEVICE_REMOVED); }
        if (!gpu) { return 0; }
        return static_cast<long>(gpu->dev->GetDeviceRemovedReason());
    }
#else
    bool initGpu() {
        lastError = "Direct3D 11 is not available on this platform";
        return false;
    }
    bool gpuIssue(int, const std::complex<float>*, std::size_t) { return false; }
    bool slotDone(const Slot&) { return false; }
    void account(Slot&) {}
    long removedReason() { return 0; }
#endif

    // ---- loss handling
    void handleLost(long reason) {
        ++st.deviceLost;
        lastError = "Direct3D device lost (" + hex32(reason) + "); CPU path in use";
        std::deque<Slot> keep;
        for (auto& s : ring) {
            if (s.backend == GpuFftBackend::Gpu) {
                ++st.droppedLost;
                st.droppedFrames += s.frames;
                used[static_cast<std::size_t>(s.idx)] = false;
            } else {
                keep.push_back(s);
            }
        }
        ring.swap(keep);
        if (gpu) {
#ifdef _WIN32
            if (gpu->ctx) { gpu->ctx->ClearState(); }
#endif
            gpu.reset();
        }
        forcedLost = false;
        wasLost = true;
        ++recoverStreak;
        nextRetry = Clock::now() + std::chrono::milliseconds(cfg.retryCooldownMs);
        st.gpuActive = false;
    }

    // True when the device is fine (or absent); false when it was found lost and handled.
    bool checkDevice() {
        if (!gpu) { return true; }
        const long r = removedReason();
        if (r < 0) { handleLost(r); return false; }
        return true;
    }

    // Bring the GPU up if wanted, absent and allowed to retry.
    void ensureGpu() {
        if (mode == GpuFftMode::Cpu || gpu) { return; }
        if (recoverStreak > cfg.maxRecoveries || createFailures > cfg.maxRecoveries) { return; }
        if (Clock::now() < nextRetry) { return; }
        if (initGpu()) {
            createFailures = 0;
            if (wasLost) { ++st.recoveries; wasLost = false; }
            st.gpuActive = true;
        } else {
            gpu.reset();
            ++createFailures;
            nextRetry = Clock::now() + std::chrono::milliseconds(cfg.retryCooldownMs);
            st.gpuActive = false;
        }
    }
};

// ---------------------------------------------------------------------------------------------------------------------------------
// Public class
// ---------------------------------------------------------------------------------------------------------------------------------
GpuSpectrumEngine::GpuSpectrumEngine(const GpuFftConfig& cfg) {
    if (!isPow2(cfg.fftSize) || cfg.fftSize < kGpuFftMinSize || cfg.fftSize > kGpuFftMaxSize) {
        throw std::invalid_argument("GpuSpectrumEngine: fftSize must be a power of two in [32, 524288]");
    }
    if (!cfg.customWindow.empty() && cfg.customWindow.size() != cfg.fftSize) {
        throw std::invalid_argument("GpuSpectrumEngine: customWindow size must equal fftSize");
    }
    if (cfg.ringSlots < 1 || cfg.ringSlots > 16) { throw std::invalid_argument("GpuSpectrumEngine: ringSlots must be 1..16"); }
    if (!(cfg.gpuBudgetFraction > 0.0 && cfg.gpuBudgetFraction <= 1.0)) {
        throw std::invalid_argument("GpuSpectrumEngine: gpuBudgetFraction must be in (0, 1]");
    }
    if (cfg.maxBatch < 1) { throw std::invalid_argument("GpuSpectrumEngine: maxBatch must be >= 1"); }
    impl_ = std::make_unique<Impl>(cfg);
    Impl& m = *impl_;
    m.maxBatch = std::min<std::size_t>(cfg.maxBatch, std::max<std::size_t>(1, (std::size_t{1} << 23) / cfg.fftSize));
    m.window = cfg.customWindow.empty() ? makeWindow(cfg.window, cfg.fftSize) : cfg.customWindow;
    m.cpuA.resize(cfg.fftSize);
    m.cpuB.resize(cfg.fftSize);
    m.cpuOut.resize(static_cast<std::size_t>(cfg.ringSlots));
    m.used.assign(static_cast<std::size_t>(cfg.ringSlots), false);
    std::lock_guard<std::mutex> lk(m.mu);
    m.ensureGpu();
}

GpuSpectrumEngine::~GpuSpectrumEngine() {
    if (!impl_) { return; }
    std::lock_guard<std::mutex> lk(impl_->mu);
#ifdef _WIN32
    if (impl_->gpu && impl_->gpu->ctx) {
        impl_->gpu->ctx->ClearState();
        impl_->gpu->ctx->Flush();
    }
#endif
    impl_->gpu.reset();
}

std::size_t GpuSpectrumEngine::fftSize() const noexcept { return impl_->n; }
std::size_t GpuSpectrumEngine::maxBatch() const noexcept { return impl_->maxBatch; }

void GpuSpectrumEngine::setMode(GpuFftMode mode) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    Impl& m = *impl_;
    m.mode = mode;
    if (mode != GpuFftMode::Cpu) {
        m.recoverStreak = 0;
        m.createFailures = 0;
        m.nextRetry = Clock::time_point{};
        m.ensureGpu();
    }
}

GpuFftMode GpuSpectrumEngine::mode() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->mode;
}

GpuFftBackend GpuSpectrumEngine::backend() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return (impl_->gpu && impl_->gpuAllowedFor()) ? GpuFftBackend::Gpu : GpuFftBackend::Cpu;
}

std::uint64_t GpuSpectrumEngine::submit(const std::complex<float>* frames, std::size_t nFrames) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    Impl& m = *impl_;
    if (frames == nullptr || nFrames == 0 || nFrames > m.maxBatch) {
        ++m.st.rejected;
        return 0;
    }
    if (m.ring.size() >= m.used.size()) {
        ++m.st.droppedRingFull;
        m.st.droppedFrames += nFrames;
        return 0;
    }
    bool useGpu = false;
    if (m.mode != GpuFftMode::Cpu) {
        m.ensureGpu();
        if (m.gpu && !m.checkDevice()) { m.ensureGpu(); }  // lost just now: in-flight batches are dropped; a zero cool-down rebuilds at once
        if (m.gpu && m.gpuAllowedFor()) {
            // Settle what has already finished so the budget sees it.
            for (auto& s : m.ring) {
                if (s.backend == GpuFftBackend::Gpu && !s.accounted && m.slotDone(s)) { m.account(s); }
            }
            m.refreshWindow();
            if (m.st.gpuSecondsThisWindow >= m.cfg.gpuBudgetFraction) {
                ++m.st.budgetFallbacks;
            } else {
                useGpu = true;
            }
        }
    }
    const int idx = m.freeIndex();
    if (idx < 0) {
        ++m.st.droppedRingFull;
        m.st.droppedFrames += nFrames;
        return 0;
    }
    const std::uint64_t ticket = m.nextTicket++;
    Impl::Slot slot;
    slot.ticket = ticket;
    slot.frames = nFrames;
    slot.idx = idx;
    if (useGpu) {
        if (m.gpuIssue(idx, frames, nFrames)) {
            slot.backend = GpuFftBackend::Gpu;
            m.used[static_cast<std::size_t>(idx)] = true;
            m.ring.push_back(slot);
            ++m.st.submitted;
            if (!m.checkDevice()) {
                // Lost while issuing: this batch was in flight too and is gone.
                return 0;
            }
            return ticket;
        }
        // Issue failed: lastError says why; compute this batch on the CPU instead.
        m.checkDevice();
    }
    auto& buf = m.cpuOut[static_cast<std::size_t>(idx)];
    if (buf.size() < m.maxBatch * m.n) { buf.resize(m.maxBatch * m.n); }
    m.cpuCompute(frames, nFrames, buf.data());
    slot.backend = GpuFftBackend::Cpu;
    slot.accounted = true;
    m.used[static_cast<std::size_t>(idx)] = true;
    m.ring.push_back(slot);
    ++m.st.submitted;
    return ticket;
}

bool GpuSpectrumEngine::collect(GpuFftBatch& info, float* outDb, std::size_t capacityFloats) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    Impl& m = *impl_;
    if (m.ring.empty()) { return false; }
    Impl::Slot& s = m.ring.front();
    const std::size_t floats = s.frames * m.n;
    if (outDb == nullptr || capacityFloats < floats) { return false; }
    if (s.backend == GpuFftBackend::Gpu) {
#ifdef _WIN32
        if (!m.gpu) { return false; }
        if (!m.slotDone(s)) {
            m.checkDevice();
            return false;
        }
        m.account(s);
        ID3D11Buffer* staging = m.gpu->slots[static_cast<std::size_t>(s.idx)].staging.Get();
        D3D11_MAPPED_SUBRESOURCE mp{};
        const HRESULT hr = m.gpu->ctx->Map(staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mp);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) { return false; }
        if (FAILED(hr)) {
            m.failHr("Map(staging)", hr);
            m.checkDevice();
            return false;
        }
        std::memcpy(outDb, mp.pData, floats * sizeof(float));
        m.gpu->ctx->Unmap(staging, 0);
        ++m.st.completedGpu;
        m.recoverStreak = 0;
#else
        return false;
#endif
    } else {
        std::memcpy(outDb, m.cpuOut[static_cast<std::size_t>(s.idx)].data(), floats * sizeof(float));
        ++m.st.completedCpu;
    }
    info.ticket = s.ticket;
    info.frames = s.frames;
    info.backend = s.backend;
    m.used[static_cast<std::size_t>(s.idx)] = false;
    m.ring.pop_front();
    return true;
}

std::size_t GpuSpectrumEngine::process(const std::complex<float>* frames, std::size_t nFrames, float* outDb, int timeoutMs) {
    const std::uint64_t t = submit(frames, nFrames);
    if (t == 0) { return 0; }
    const auto end = Clock::now() + std::chrono::milliseconds(timeoutMs);
    GpuFftBatch b;
    const std::size_t cap = nFrames * fftSize();
    while (Clock::now() < end) {
        if (collect(b, outDb, cap)) {
            if (b.ticket == t) { return b.frames; }
            continue;  // an older batch still in the ring: hand it away and keep going
        }
        {
            std::lock_guard<std::mutex> lk(impl_->mu);
            if (impl_->ring.empty()) { return 0; }  // our batch was dropped (device lost)
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    return 0;
}

GpuFftStats GpuSpectrumEngine::stats() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    GpuFftStats s = impl_->st;
    s.gpuActive = impl_->gpu != nullptr;
    return s;
}

std::string GpuSpectrumEngine::lastError() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->lastError;
}

std::string GpuSpectrumEngine::shaderLog() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->shaderLog;
}

GpuAdapterInfo GpuSpectrumEngine::adapter() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->gpu ? impl_->adapterInfo : GpuAdapterInfo{};
}

void GpuSpectrumEngine::forceDeviceLostForTest() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->forcedLost = true;
}

void GpuSpectrumEngine::forceCreateFailureForTest(int n) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->forceCreateFail = n;
}

}  // namespace cascade::dsp
