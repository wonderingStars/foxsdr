# GPU FFT (lane `gpufft`, branch `feat/gpufft`)

Row: "GPU FFT (SDR Console CUDA/OpenCL)", FEATURE-GAPS.md "Spectrum and waterfall", survey row S19 ("XL, needs CUDA/OpenCL SDKs, not worth building"). The owner said all of it before the release.

Verdict first: **built, proven correct against the CPU FFT, OFF by default (`GpuFftMode::Cpu`), wired to nothing.** On this PC the GPU path is *not* better at small sizes (1,024 to 32,768): the single-frame round trip is slower than the CPU, and the pipelined throughput lost or tied in the medians. It **is** much cheaper for the CPU at every size measured and clearly faster from 65,536 up. So the survey's doubt holds for the sizes the receiver uses today (1,024) and fails for the large analysis sizes. The Auto threshold is 65,536; the default stays Cpu.

## What was built (new files only)

| File | Content |
| --- | --- |
| `src/dsp/gpu_fft.hpp` | interface: `GpuFftMode {Auto, Cpu, Gpu}`, `GpuFftConfig`, `GpuSpectrumEngine` (`submit`, `collect`, `process`, `setMode`, `stats`, `lastError`, `shaderLog`, `adapter`), `enumerateGpuAdapters()`, `gpuFftCpuReference()` |
| `src/dsp/gpu_fft.cpp` | Direct3D 11 device, buffers, passes, ring, budget, loss recovery, and the CPU backend |
| `src/dsp/gpu_fft_shader.hpp` | the HLSL source text (compiled at run time, `cs_5_0`) |
| `tests/test_gpu_fft.cpp` | correctness and behaviour tests |
| `tests/test_gpu_fft_bench.cpp` | raw timings (quick by default; `GPUFFT_BENCH_FULL=1` for the long runs; `GPUFFT_BENCH_SIZES=8192,32768`, `GPUFFT_BENCH_NOLOWPRIO=1`) |

No existing file was edited (the test list is a glob; the `src/dsp/*.cpp` glob picks up `gpu_fft.cpp`). Nothing is linked: `d3d11.dll`, `dxgi.dll` and `d3dcompiler_47.dll` are loaded from System32 with `LoadLibraryExW(..., LOAD_LIBRARY_SEARCH_SYSTEM32)` at the first use, so a machine without them (or a non-Windows build, which compiles a stub) just runs the CPU path. No download, no SDK, no driver or system setting touched. CUDA and OpenCL are not used.

### Step 0 (reading only)

- `src/dsp/fft.*`: `ComplexFFT`, pffft single precision, ordered output, sizes 2^a 3^b 5^c with a >= 5, unscaled forward transform.
- `src/dsp/spectrum.*`: `SpectrumEstimator::process`: window, forward FFT, `|X|^2/N^2`, fftshift, EMA, `10 log10`, floor 1e-20 (-200 dB). The GPU contract is this with alpha = 1 (one frame, no averaging; averaging stays with the caller).
- `feat/signal-blocks-3` `src/dsp/spectrum_block.hpp` (read with `git show`): sizes 1,024 to 524,288 powers of two, five windows (rectangular, Hann, Hamming, Blackman-Harris, flat-top), average / peak / min hold, max-per-column reduction. The GPU engine takes the four `dsp::WindowType` windows or any window as a vector (`customWindow`), so the flat-top table of `spectrum_block.hpp` can be passed in; sizes here are powers of two from 32 to 524,288. The holds and column reduction are cheap per-bin work that stay on the CPU after `collect()`.
- How the feed calls the FFT today: `core/pipeline.hpp` owns one `SpectrumEstimator estimator_` touched only by the DSP thread (fixed `kFftSize = 1024` in `core/receiver_tables.hpp`).
- GPUs on this PC (DXGI, read-only enumeration, printed by `test_gpu_fft`):
  - adapter 0: NVIDIA GeForce RTX 4090, vendor 0x10DE, device 0x2684, 24,142 MB dedicated, hardware (the one used);
  - adapter 1: Microsoft Basic Render Driver (vendor 0x1414, device 0x008C), software; never selected, refused by index too (tested).
- Interface presented: complex float frames in (N per frame, back to back, 1 to `maxBatch()` frames), power dB out per frame (N floats, fftshifted), batched, asynchronous (`submit` / `collect`), plus a synchronous `process`.
- Break-even size: measured below; Auto switches to the GPU from `autoMinFftSize` = 65,536.

### Algorithm

Stockham autosort FFT (no bit-reversal pass), decimation in time, radix-4 passes with one radix-2 pass first when log2 N is odd. Thread j of a pass with sub-transform length Ns reads `v[r] = src[j + r N/R]`, multiplies by the table twiddle `Tw[r (j mod Ns) N/(Ns R)]`, applies the radix-R butterfly and writes `dst[(j - j mod Ns) R + (j mod Ns) + r Ns]`. The window is multiplied in the first pass; one more pass does power, fftshift and `10 log10`. Twiddles are computed in double on the CPU and rounded once. Ping-pong buffers; one dispatch per pass covers the whole batch (frame number = dispatch Y). The radix-4 butterfly is the 4-point DFT `a0 = v0+v2, a1 = v0-v2, b0 = v1+v3, b1 = -i (v1-v3); y = (a0+b0, a1+b1, a0-b0, a1-b1)`.

No textbook or paper was opened for this: the pass structure is the standard Stockham form written from knowledge and **verified numerically** (below) against pffft and a direct double-precision DFT at both parities of log2 N. The same text is in the header comment of `gpu_fft_shader.hpp`.

### Safety and behaviour

- OFF by default: `GpuFftConfig::mode = Cpu`; a Cpu engine never creates a device (tested).
- Headless: `D3D11CreateDevice` on a DXGI adapter, no swap chain, no window, no UI; `SINGLETHREADED` device behind one mutex.
- GPU shared with the desktop: `SetGPUThreadPriority(-7)` (lowest); a per-second budget (`gpuBudgetFraction`, default 25%) measured with timestamp queries: when spent, the batch runs on the CPU instead (counted, tested); one hardware adapter by most dedicated memory; software adapters refused. Tests and the bench use small workloads (a few hundred milliseconds per run, at most a few batches in flight) and never create a window.
- Asynchronous, never stalls the audio path: `submit` issues and returns; `collect` returns only batches whose event query has completed and maps the staging buffer with `DO_NOT_WAIT`. A full ring (`ringSlots`, default 4) drops the new batch and counts it (`droppedRingFull`, `droppedFrames`).
- Device lost / TDR: `GetDeviceRemovedReason` is checked before and after every issue and on every failed map. On a loss: in-flight GPU batches are dropped and counted (`droppedLost`), every GPU object is released, `lastError` says why, the CPU path takes over in the same call, and after `retryCooldownMs` the GPU is rebuilt (`recoveries`), up to `maxRecoveries` losses in a row (reset by a batch that finishes on the GPU or by `setMode`).
- Shader compile at run time, error text kept (`shaderLog()`, `lastError()`), never abort; a missing DLL, no hardware adapter, a bad adapter index or a failed device creation leave the engine on the CPU with the reason.
- Nothing personal is stored: adapter name and error text only; no stderr or stdout printing in the engine.

## Tests (real output)

`ctest -C Release -R "^test_gpu_fft$"`:

```
Start 197: test_gpu_fft
1/1 Test #197: test_gpu_fft .....................   Passed    7.37 sec
```

and the run's own line `test_gpu_fft: 170 checks, 0 failed` (eight consecutive runs, all 170/0 after the last test fix). Coverage: size / window / ring / budget / batch validation; default off and no device; CPU backend bit-identical to `SpectrumEstimator` (alpha 1) for four windows; -200 dB floor on CPU and GPU; direct double DFT at 32, 64, 256, 2,048 (both parities of log2 N); the CPU path at 1,024 / 8,192 / 65,536 / 524,288 with noise, off-grid tones with a 50 ppm sample-rate error, and tones; every window and a custom window; tone level (0 dBFS rectangular) and fftshift orientation; ticket order, backend flag, ring-full drops, rejected submits, a too-small collect buffer; runtime switch Cpu -> Gpu -> Auto (below and above the threshold) -> Cpu; refused bring-up with fallback and re-arm; non-compiling shader and a shader without the entry points; bad and software adapters; the budget; forced device loss with in-flight drop, CPU answer, cool-down and recovery; repeated losses giving up and `setMode` re-arming; a producer/consumer thread pair with 150 batches in order and every peak on the right bin.

### Correctness against the CPU FFT (printed by the test; error = max |P_gpu - P_cpu| / P_peak, linear power; dB column = max |dB difference| over bins above -80 dB of the frame's own peak)

| N | frames | measured max rel-to-peak | measured max dB diff | pinned (test fails above) |
| --- | --- | --- | --- | --- |
| 1,024 | 4 | 5.99e-07 | 0.00283 | 2e-6 / 0.01 |
| 8,192 | 4 | 1.39e-06 | 0.000206 | 4e-6 / 0.01 |
| 65,536 | 3 | 1.26e-06 | 0.000221 | 4e-6 / 0.01 |
| 524,288 | 1 | 2.20e-07 | 5.34e-05 | 1e-6 / 0.01 |

Against the direct double DFT: n = 32: 1.92e-07 / 0.00094 dB; n = 64: 7.38e-08 / 0.0016; n = 256: 1.7e-07 / 0.0011; n = 2,048: 2.61e-08 / 0.00015 (pinned 1e-6 / 0.02). The first pins (3e-7) were too tight for float rounding and were loosened to the table above after the first run printed these numbers; no expectation was weakened below what the measurements justify (the error is float rounding of two different FFTs).

## Timing: GPU against CPU on this PC

The machine is shared and was at 100% CPU the whole time (Get-Counter read 100 during the runs) with other lanes also using the GPU, so wall times are noisy. Raw lines from `GPUFFT_BENCH_FULL=1 test_gpu_fft_bench` (log `D:/foxsdr-build/gpufft-bench-full1.log`), three timings each. CPU = one frame at a time through the CPU backend (wall us, thread-CPU us); GPU round trip = one frame submit to result in hand (us, spin wait); GPU pipelined = batches of 8 (4 at 262,144 and up) in flight, frames per second, thread-CPU us per frame (polling by sleeping), and GPU compute us per frame from the timestamp queries.

```
N=1024 CPU per frame (wall us, thread-CPU us) x3: (7.6, 3.2) (8.6, 5.4) (9.9, 8.0)
N=1024 GPU 1-frame round trip (us) x3: 450.5 122.7 205.5
N=1024 GPU pipelined batch=8: frames/s x3: 42779 36933 31551; thread-CPU us/frame x3: 2.4 1.1 2.6; GPU-compute us/frame x3: 2.6 2.7 2.6
N=4096 CPU per frame (wall us, thread-CPU us) x3: (41.3, 29.3) (47.9, 39.4) (67.2, 48.1)
N=4096 GPU 1-frame round trip (us) x3: 48703.2 44913.4 15855.4
N=4096 GPU pipelined batch=8: frames/s x3: 23388 6861 27643; thread-CPU us/frame x3: 4.0 7.5 5.3; GPU-compute us/frame x3: 6.1 6.4 5.8
N=8192 CPU per frame (wall us, thread-CPU us) x3: (195.7, 68.4) (169.4, 92.6) (312.0, 92.6)
N=8192 GPU 1-frame round trip (us) x3: 79769.3 88911.8 83637.9
N=8192 GPU pipelined batch=8: frames/s x3: 2096 1882 2036; thread-CPU us/frame x3: 24.8 21.9 20.4; GPU-compute us/frame x3: 30.1 32.4 39.5
N=16384 CPU per frame (wall us, thread-CPU us) x3: (196.0, 171.5) (204.3, 183.0) (198.4, 179.8)
N=16384 GPU 1-frame round trip (us) x3: 4613.0 3012.9 1611.4
N=16384 GPU pipelined batch=8: frames/s x3: 5469 6101 7077; thread-CPU us/frame x3: 20.9 17.1 16.2; GPU-compute us/frame x3: 23.0 21.6 22.4
N=32768 CPU per frame (wall us, thread-CPU us) x3: (413.9, 280.2) (469.8, 318.1) (833.4, 347.2)
N=32768 GPU 1-frame round trip (us) x3: 89780.3 84867.8 66620.7
N=32768 GPU pipelined batch=8: frames/s x3: 1831 1953 1909; thread-CPU us/frame x3: 62.5 63.5 43.6; GPU-compute us/frame x3: 99.3 85.3 73.7
N=65536 CPU per frame (wall us, thread-CPU us) x3: (1861.4, 582.9) (1288.7, 816.0) (1433.2, 850.6)
N=65536 GPU 1-frame round trip (us) x3: 50953.2 69403.8 86968.3
N=65536 GPU pipelined batch=8: frames/s x3: 1496 1431 2033; thread-CPU us/frame x3: 103.9 111.6 97.2; GPU-compute us/frame x3: 134.9 122.9 100.5
N=131072 CPU per frame (wall us, thread-CPU us) x3: (3201.0, 1732.4) (3026.3, 2299.6) (3289.5, 2124.5)
N=131072 GPU 1-frame round trip (us) x3: 81784.5 81338.3 71667.5
N=131072 GPU pipelined batch=8: frames/s x3: 1492 2486 2856; thread-CPU us/frame x3: 160.4 204.9 215.0; GPU-compute us/frame x3: 210.0 182.1 175.1
N=262144 CPU per frame (wall us, thread-CPU us) x3: (5982.8, 4917.8) (6534.1, 5638.6) (6890.7, 5539.8)
N=262144 GPU 1-frame round trip (us) x3: 6173.4 5491.7 6499.2
N=262144 GPU pipelined batch=4: frames/s x3: 1844 1679 1630; thread-CPU us/frame x3: 417.7 403.0 325.5; GPU-compute us/frame x3: 369.4 389.2 341.4
N=524288 CPU per frame (wall us, thread-CPU us) x3: (9971.8, 7760.8) (10145.1, 8074.7) (11509.2, 9899.8)
N=524288 GPU 1-frame round trip (us) x3: 4076.1 4209.1 4087.5
N=524288 GPU pipelined batch=4: frames/s x3: 962 918 1105; thread-CPU us/frame x3: 876.5 702.0 837.7; GPU-compute us/frame x3: 725.4 788.7 709.6
```

Medians of the three (N, CPU us/frame wall, CPU thread us/frame, GPU thread-CPU us/frame, CPU frames/s, GPU frames/s, GPU/CPU throughput):

```
1024,   8.6,    5.4,    2.4,   116696, 36933, 0.32x
4096,   47.9,   39.4,   5.3,   20869,  23388, 1.12x
8192,   195.7,  68.4,   21.9,  5110,   2036,  0.40x
16384,  198.4,  179.8,  17.1,  5041,   6101,  1.21x
32768,  469.8,  318.1,  62.5,  2128,   1909,  0.90x
65536,  1433.2, 850.6,  103.9, 698,    1496,  2.14x
131072, 3201.0, 1732.4, 204.9, 312,    2486,  7.96x
262144, 6534.1, 5638.6, 403.0, 153,    1679,  10.97x
524288, 10145.1, 8074.7, 837.7, 99,    962,   9.76x
```

Four further runs, not saved as files (numbers read from the console): two more full runs of the same binary (the environment switches did not exist yet in that binary, so both are plain low-priority repeats), then, after the switches were added, 8,192 / 32,768 / 65,536 once with the default low GPU priority and once with `GPUFFT_BENCH_NOLOWPRIO=1`. They gave the same picture as the table: single-frame round trips from 0.2 ms to 120 ms with no stable pattern, pipelined 1.9k to 9k frames/s at 8,192 and 2.2k to 5.5k at 65,536, GPU compute 10 to 20 us per frame at 8,192 and about 100 us at 65,536. Low against normal GPU priority: at 8,192 the normal-priority run was faster (round trips 0.18 to 1.7 ms against 3.5 to 11.5 ms; pipelined 9.1k / 8.4k / 2.4k against 4.9k / 6.2k / 2.2k frames/s); at 32,768 and 65,536 there was no consistent difference. One run each is inconclusive; the engine keeps the low priority because starving the display is the worse failure.

Reading of the numbers:

- GPU compute time per frame (timestamp queries, not affected by CPU contention) is 2.6 us at 1,024 against 8.6 us on the CPU, 6 us against 48 at 4,096, about 22 against 198 at 16,384, about 100 to 135 against 1,433 at 65,536 and about 700 against 10,100 at 524,288: 3x to 14x faster. The wall-clock gain is eaten at small sizes by submit / map / scheduling latency on a machine that is at 100% CPU and shared with other GPU users, so the GPU's wall throughput at 1,024 to 32,768 is 0.3x to 1.2x of the CPU (no reliable win), and the single-frame latency is far worse than the CPU's up to 131,072 (CPU 8.6 us to 3.2 ms against GPU 0.1 to 90 ms). From 262,144 the single-frame latency is also lower on the GPU (about 6 ms against 6.5 ms at 262,144, about 4.1 ms against 10.1 ms at 524,288).
- First size where the GPU won pipelined throughput in all three timings: 65,536 (1.5k to 2.0k frames/s against 0.5k to 0.8k). That is the Auto threshold. It is a measurement on a loaded PC, not a law: on a quiet machine it would be lower.
- **CPU load saved** (thread-CPU time per frame, median, share of one core to analyse every sample with no overlap, frames/s = fs / N; the path that cannot keep up is marked):

```
fs=2.4 MS/s  N=1024  frames/s=2344: CPU path 1.3%,  GPU path 0.6%
fs=2.4 MS/s  N=4096  frames/s=586:  CPU path 2.3%,  GPU path 0.3%
fs=2.4 MS/s  N=65536 frames/s=37:   CPU path 3.1%,  GPU path 0.4%
fs=10.0 MS/s N=1024  frames/s=9766: CPU path 5.3%,  GPU path 2.4%
fs=10.0 MS/s N=4096  frames/s=2441: CPU path 9.6%,  GPU path 1.3%
fs=10.0 MS/s N=65536 frames/s=153:  CPU path 13.0%, GPU path 1.6%
```

(The GPU path's remaining CPU cost is the two buffer copies, the API calls and the polling wakeups.) At today's 1,024-point spectrum the saving is about 1 percentage point of one core at 10 MS/s (5.3% to 2.4%); the larger sizes save 8 to 11 points of one core at 10 MS/s. Neither is a reason by itself to switch the default.
- Honest verdict: not faster or fairer at the small sizes the receiver uses now; worth having behind the switch for large spectrum sizes and for a busy CPU; the default stays Cpu.

## Break-it (remove the behaviour, see red, restore, green)

Script `D:/foxsdr-build/gpufft-breakit.sh` (and `gpufft-bkC.sh` for row C after the test fix); each row: edit, rebuild `test_gpu_fft`, run, `git checkout`, rebuild.

| Mutation | Result |
| --- | --- |
| A: radix-4 stage uses `Tw[t]` instead of `Tw[2*t]` | exit 1, 26 failed (direct DFT and CPU comparisons, e.g. `er.maxRelToPeak < 1e-6`) |
| B: device-loss check never fires | exit 1, 13 failed (`st.deviceLost == 1`, `st.droppedLost == 1`, `st.droppedFrames == 1`, ...) |
| C: ring-full drop counter removed | exit 1, 2 failed (`e.stats().droppedRingFull == 1`, lines 328 and 364) |
| D: per-second GPU budget ignored | exit 1, 3 failed (`budgetFallbacks >= 1`, `completedCpu >= 1`, CPU-equal result) |
| E: software-adapter refusal removed | exit 1, 2 failed (`e2.backend() == Cpu`, error text mentions software) |
| Restored | exit 0, `test_gpu_fft: 170 checks, 0 failed` |

The first run of row C (ring guard replaced by `if (false)`) did not go red on the ring test because a second guard (`freeIndex() < 0`) also drops; it failed in the repeated-loss test instead, which exposed that test as racy (a collected GPU batch resets the loss streak). The test was fixed (no collect between losses), repeated eight times green, and row C redone on the counter.

## Gates

- Own tests: `test_gpu_fft` passes (170 checks, 0 failed); `test_gpu_fft_bench` passes (54 checks, 0 failed, full run).
- Headless set: see "Gates run" at the end of this record.

## Wiring and screen hand-off (nothing is wired)

Engine (the spectrum path owner): in `core/pipeline.hpp` / `pipeline.cpp`, where `SpectrumEstimator estimator_` is fed, add a `GpuSpectrumEngine` built from the same size and window with `mode` from the setting below; feed `submit()` with up to `maxBatch()` frames from the same ring the estimator reads, apply the EMA / holds / column reduction on the `collect()`ed dB frames on the DSP thread (the engine returns single-frame dB, alpha = 1), and keep the CPU estimator alive for a size change. Both paths agree to the pinned errors above. Log `lastError()` and `stats()` (counts only) at a low rate; never log adapter names into reports unless the owner wants it (a hardware model name is not personal data but is not needed).

Plugin ABI / API: none needed. A fact for the engine API: `gpu_fft.mode` (0 Cpu, 1 Auto, 2 Gpu), `gpu_fft.backend` (read-only, 0 Cpu / 1 Gpu), `gpu_fft.stats` (read-only counters).

Screen (English strings; none translated here):

| Control | Label | Op / fact | Range | Default |
| --- | --- | --- | --- | --- |
| Choice | "Spectrum FFT on" | `gpu_fft.mode` | "CPU" / "Auto" / "GPU" | "CPU" |
| Read-out | "Spectrum FFT running on" | `gpu_fft.backend` | "CPU" / "GPU: " + adapter name | "CPU" |
| Read-out (when it fell back) | "GPU spectrum unavailable: " + reason | `GpuSpectrumEngine::lastError()` | text | empty |
| Read-out | "GPU spectrum frames dropped" | `stats().droppedRingFull + droppedLost` | integer | 0 |
| Number (advanced) | "Share of each second the GPU may be used" | `GpuFftConfig::gpuBudgetFraction` | 5% to 100% | 25% |
| Number (advanced) | "Smallest size that Auto sends to the GPU" | `GpuFftConfig::autoMinFftSize` | 1,024 to 524,288 (power of two) | 65,536 |

## UNVERIFIED

- A real TDR / device removal was never provoked (it would blank the owner's display). The loss path was exercised through `forceDeviceLostForTest()`, which makes the next device check see `DXGI_ERROR_DEVICE_REMOVED` and then runs the genuine handling (drop, release, fallback, rebuild); the Direct3D call that reports a real removal (`GetDeviceRemovedReason`) is therefore exercised only in its healthy case.
- Only one hardware GPU (RTX 4090) was available: no AMD, Intel or older-driver behaviour, no machine without a GPU other than through the forced-failure hook and the software-adapter refusal.
- The bench numbers are from a PC at 100% CPU shared with about 40 other jobs; round-trip latencies in particular vary by two orders of magnitude between runs. The break-even is a measurement on that PC, not a constant.
- The low against normal GPU priority comparison is one run of three sizes on a noisy machine; inconclusive (at 8,192 normal priority looked faster). The earlier repeats ran a binary without the switch.
- The Stockham pass structure was written from knowledge, no textbook or paper was opened; it is proven only numerically (above).
- No wiring: nothing in the receiver uses the engine, so end-to-end behaviour in the running program (audio never stalling with the GPU path on) is by design, not observed.

## Sources opened

None. No web page or document was opened for this lane; the Direct3D 11 API usage (`D3D11CreateDevice`, structured buffers, queries, `D3DCompile`, `IDXGIDevice::SetGPUThreadPriority`) is from the Windows SDK headers installed on this PC (`d3d11.h`, `dxgi.h`, `d3dcompiler.h`, 10.0.26100.0), which compiled and behaved as used. Licences: nothing third-party copied.

## Owner decisions

- Accept the default Cpu with Auto from 65,536 (the numbers say the GPU is not a win at the sizes in use today)? Or hold the screen entry back until the spectrum size choice from `spectrum_block.hpp` is wired, since the GPU only pays off at the large sizes.
- Whether the budget default (25% of each second) and the lowest GPU thread priority are right for the owner's machine: the display adapter and the compute adapter are the same card here.

## Gates run

(filled in below at the end of the lane)
