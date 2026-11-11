// HLSL source text of the GPU spectrum engine (compiled at run time by d3dcompiler_47, cs_5_0). See gpu_fft.hpp.
//
// Layout: every frame is N consecutive float2 (re, im) in a StructuredBuffer; the dispatch's Y index is the frame number.
//
// Stockham autosort, decimation in time, as an iteration over Ns = the length of the sub-transforms already done (1, then R, R^2 ...):
//   thread j reads  v[r] = src[j + r*N/R]            r = 0 .. R-1
//   k = j mod Ns;  v[r] *= exp(-2*pi*i * r*k / (Ns*R))  (a table lookup: Tw[r*k*N/(Ns*R)], Tw[m] = exp(-2*pi*i*m/N))
//   v = DFT_R(v)
//   dst[(j - k)*R + k + r*Ns] = v[r]                  (j - k = (j / Ns)*Ns, so this is (j/Ns)*Ns*R + k + r*Ns)
// After the last pass dst holds the natural-order transform; no bit reversal pass exists.
// DFT_4 (forward, e^-i): a0 = v0+v2, a1 = v0-v2, b0 = v1+v3, b1 = -i*(v1-v3);  y0 = a0+b0, y1 = a1+b1, y2 = a0-b0, y3 = a1-b1.
//
// Entry points: CSFirst2 / CSFirst4 (first pass, Ns = 1: input * window, no twiddles), CSStage4 (later radix-4 passes), CSPower.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

namespace cascade::dsp::gpufft_detail {

inline constexpr char kShaderSource[] = R"HLSL(
cbuffer Params : register(b0) {
    uint N;        // frame length (power of two)
    uint Ns;       // sub-transform length done so far (power of two)
    uint log2Ns;
    uint pad0;
    float invN2;   // 1 / N^2
    float3 pad1;
};

StructuredBuffer<float2> Input : register(t0);
StructuredBuffer<float2> Tw : register(t1);
StructuredBuffer<float> Win : register(t2);
StructuredBuffer<float2> Src : register(t3);
RWStructuredBuffer<float2> Dst : register(u0);
RWStructuredBuffer<float> OutDb : register(u1);

float2 cmul(float2 a, float2 b) { return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }

void dft4(inout float2 v0, inout float2 v1, inout float2 v2, inout float2 v3) {
    float2 a0 = v0 + v2;
    float2 a1 = v0 - v2;
    float2 b0 = v1 + v3;
    float2 d = v1 - v3;
    float2 b1 = float2(d.y, -d.x);   // -i * d
    v0 = a0 + b0;
    v1 = a1 + b1;
    v2 = a0 - b0;
    v3 = a1 - b1;
}

[numthreads(256, 1, 1)]
void CSFirst2(uint3 id : SV_DispatchThreadID) {
    uint j = id.x;
    uint hN = N >> 1;
    if (j >= hN) { return; }
    uint base = id.y * N;
    float2 v0 = Input[base + j] * Win[j];
    float2 v1 = Input[base + j + hN] * Win[j + hN];
    Dst[base + 2 * j] = v0 + v1;
    Dst[base + 2 * j + 1] = v0 - v1;
}

[numthreads(256, 1, 1)]
void CSFirst4(uint3 id : SV_DispatchThreadID) {
    uint j = id.x;
    uint q = N >> 2;
    if (j >= q) { return; }
    uint base = id.y * N;
    float2 v0 = Input[base + j] * Win[j];
    float2 v1 = Input[base + j + q] * Win[j + q];
    float2 v2 = Input[base + j + 2 * q] * Win[j + 2 * q];
    float2 v3 = Input[base + j + 3 * q] * Win[j + 3 * q];
    dft4(v0, v1, v2, v3);
    uint o = base + 4 * j;
    Dst[o] = v0;
    Dst[o + 1] = v1;
    Dst[o + 2] = v2;
    Dst[o + 3] = v3;
}

[numthreads(256, 1, 1)]
void CSStage4(uint3 id : SV_DispatchThreadID) {
    uint j = id.x;
    uint q = N >> 2;
    if (j >= q) { return; }
    uint base = id.y * N;
    uint k = j & (Ns - 1);
    uint step = N / (Ns * 4);
    uint t = k * step;
    float2 v0 = Src[base + j];
    float2 v1 = cmul(Src[base + j + q], Tw[t]);
    float2 v2 = cmul(Src[base + j + 2 * q], Tw[2 * t]);
    float2 v3 = cmul(Src[base + j + 3 * q], Tw[3 * t]);
    dft4(v0, v1, v2, v3);
    uint o = base + (j - k) * 4 + k;
    Dst[o] = v0;
    Dst[o + Ns] = v1;
    Dst[o + 2 * Ns] = v2;
    Dst[o + 3 * Ns] = v3;
}

[numthreads(256, 1, 1)]
void CSPower(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    if (i >= N) { return; }
    uint base = id.y * N;
    uint k = (i + (N >> 1)) & (N - 1);
    float2 x = Src[base + k];
    float p = (x.x * x.x + x.y * x.y) * invN2;
    OutDb[base + i] = 10.0f * log10(max(p, 1e-20f));
}
)HLSL";

}  // namespace cascade::dsp::gpufft_detail
