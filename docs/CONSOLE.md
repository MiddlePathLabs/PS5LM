# Console facts

Unless explicitly labeled **Pro** below, these are historical measurements from
the earlier Slim, not the current test target. Payload ceiling/freezing results
remain cautionary evidence, not a validated safe budget for the Pro.

## Current Pro discovery and native capabilities (2026-10-14–15)

- CFI-7019 B01X, firmware 12.70 (`0x12700001`), host `192.168.1.50`.
- Payload Manager 0.5.2 on 8084; FTP 2121; kernel logs 3232.
- `hello` and bounded `discovery` passed on hardware. Discovery reported affinity
  `0xea00`, 16 CPUs online, zero direct-memory budget, and 8 GiB configured /
  approximately 7.97 GiB available flexible memory.
- Only an 8 MiB allocation/touch/verify/free test was exercised. The flexible
  pool is shared; its reported availability is not an allocation recommendation.
- A native RADV dispatch over 64 integers and clean shell-managed closure passed.
  SSD bandwidth, model residency and inference performance have **not** been
  measured on this Pro. Native budgets remain queries, not safe allocation limits.
- Evidence: `test-results/ps5/hello/20261014-213555/` and
  `test-results/ps5/discovery/20261014-213630/` (local, ignored).
- On 2026-10-15, pinned Kstuff Lite v1.11 / ShadowMountPlus 1.7-beta5fix1
  registered PPSA99040. Its first native launch (PID 132) crashed with SIGSEGV
  at RIP zero before any capabilities result; no native budgets were obtained.
  Logs: `test-results/ps5/native-launch/20261015-090646/`.
- The repaired PPSA99040 retry (PID 135) reported success: 12 GiB direct memory,
  448 MiB configured / 442 MiB available flexible memory, CPU affinity `0x1fff`
  (13 logical CPUs allowed), 16 CPUs online, CPU model `100-000000735`.
  No test buffers were allocated; reported memory is not a safe allocation limit.
  An `exit(0)` call was followed by SIGSYS (12) at `libkernel` + `0x3ac`;
  the process terminated through crash handling, not a verified clean exit.
  Retry logs/results: `test-results/ps5/native-capabilities-retry/20261015-124122/`.
- The shell-exit retry (PID 138) repeated the successful budget queries and was
  removed by ShellCore (`Kill for LoadExec => 0`) without an observed target crash.
  The shell then tried to execute path `exit`; replacement PID 139 failed with
  `0x80aa001a` / `EINVAL` and SIGSYS (12). Both PIDs were absent afterward and
  loader services remained responsive. This is **not** a clean lifecycle pass.
  Evidence: `test-results/ps5/native-capabilities-shell-exit/20261015-135534/`.
- Read-only inspection of this console's SystemService module identified the
  no-argument `sceShellCoreUtilExitApp` close wrapper and its separate
  `libSceShellCoreUtil` export library. The direct-close launch passed as PID 140:
  query result success, close API return zero, PID-specific shell exit and process
  removal, app unload and runtime-mount release. No abort, crash or exit-spawn
  failure was observed in the captured run; console services answered afterward.
  This satisfies the native capabilities/clean-close gate, not GPU correctness.
  Evidence: `test-results/ps5/native-capabilities-direct-close/20261015-141650/`.
- PPSA99041 (**PS5LM Native Compute**) was separately authorized for one tiny GPU
  test, staged with raw readback verification and registered through the existing
  loader. Its first launch (PID 141) failed at `runtime_setup` (`rc=2`) before any
  Vulkan submission or output verification; no GPU computation was observed.
  Tracked startup direct-memory peak was 18 MiB, with no denials/ledger errors.
  The title closed cleanly through the shell, with runtime mounts released and
  services responsive. The output file was absent, but loader logs confirm the
  sandbox `/data` mapping existed.
  Evidence: `test-results/ps5/native-compute/20261015-144615/`.
  The diagnostic retry (PID 142) identified `lstat("/data/PS5LM")` returning
  `-1` / `EPERM` (errno 1). It again stopped before Vulkan submission and
  closed cleanly, with no observed abort/crash and services answering afterward.
  The native ABI/policy reason for that rejection is not yet established.
  Evidence: `test-results/ps5/native-compute-setup-diagnostics/20261015-145941/`.
  Read-only firmware inspection confirmed the expected lstat binding and syscall
  190. A setup-only no-follow open/fstat/close repair passed local build and 14
  host filesystem/error smokes. The repaired launch passed as PID 143: runtime
  setup succeeded, the Vulkan fence completed, all 64 outputs matched and
  application cleanup completed. No later ABORT or crash was observed; native
  closure, process removal and runtime-mount release passed.
  The buffer allocation was 256 bytes (host-coherent memory type 5), with tracked
  direct-memory peak 36.5 MiB and zero denials/ledger errors. Reported total time
  was 73.892 ms for this one tiny dispatch, not an inference benchmark.
  Driver output warned of invalid PCI bus info used for device UUID generation.
  Services answered afterward; no larger-memory or timeout-recovery claim follows.
  Evidence: `test-results/ps5/native-compute-metadata/20261015-160502/`.
- The native llama/ggml-vulkan integration now builds locally. Its bounded
  five-case FP32 operator executable replaced PPSA99041's integer probe using
  staged/raw-verified deployment; the old executable is preserved locally.
  Metadata, runtime, registration and loader instances are unchanged.
  Local validation covered 277 native imports, 1,567 SPIR-V modules and 320 CPU
  values checked against scalar formulas. The user-selected native run then
  **passed all five FP32 operators on the GPU**, PID 144 / app ID `0xe018`:
  320/320 outputs within tolerance, zero mismatches. Maximum absolute error
  across the cases was `2.68220901e-7`. Fence/explicit backend cleanup completed;
  static direct-memory peak was 45.875 MiB, with zero denials/ledger errors.
  The 45.625 MiB live charge at the marker is not total residency/leak evidence.
  Whole-run time was 819.647 ms, including CPU references, not inference speed.
  Native close returned zero; PID-specific shell exit/removal, EndAppMount and
  loader runtime release passed. PID 144 was absent afterward; no later ABORT,
  fatal signal, crash-report start or replacement-spawn failure was observed.
  Services answered and local log capture was stopped.
  Evidence: `test-results/ps5/native-ggml-ops/20261015-175624/result.json`.
  Quantized operators, attention and full model graphs remain unverified.
  The model directory was rechecked: only the 1,280,835,840-byte incomplete
  `Qwen3.5-2B-Q4_K_M.gguf.part` and its `.pieces` file were present.
- The expanded PPSA99041 suite **failed its numerical gate but closed cleanly**,
  PID 145 / app ID `0x18`. Q4_K matvec/matmul, Q6_K matmul and Q4_K two-expert
  selection passed all 200 outputs (maximum absolute error `1.90734863e-6`).
  ATTN_DECODE checked 128 outputs and had one mismatch, first at index 77;
  its case-wide maximum absolute error was `0.000507116318`. ATTN_PREFILL
  was not run because the suite stopped on failure. The reported total was
  328 checked values, one mismatch, rc=6; this is not an 840-value pass.
  Fence and explicit cleanup completed, tracked direct-memory peak 38.875 MiB,
  zero allocation denials/ledger errors. Native close returned zero and
  PID-specific shell exit/removal, EndAppMount and runtime release passed.
  Services answered, the title was unmounted, and local capture was stopped.
  Evidence: `test-results/ps5/native-ggml-quant-attn/20261015-181614/result.json`.
  Source inspection found FP16 intermediates remain in scalar Vulkan attention
  despite requested FP32 accumulation. Their role in the mismatch is a hypothesis,
  not proven attribution to llama.cpp or RADV; exact failing values were not logged.
  Tolerances and deployed code are unchanged; no rerun or model upload occurred.
- The attention-only diagnostic ran as PID 146 / app ID `0x2018`, recorded all
  128 exact CPU/GPU float values, and repeated the one numerical mismatch.
  At index 77 the reference was 0.3407879173755646, GPU 0.34033203125;
  error 0.0004558861255645752 exceeded allowance 0.00044078791737556456.
  Captured variant `fp16_f32acc`, Br=2/Bc=32, D_split=8, workgroup 128,
  subgroup 32, GQA=2, split_k=1, scalar path. Host emulation with fused FP16
  intermediate arithmetic reproduced every GPU output exactly; all 128 were
  FP16-representable. Tolerance is unchanged. This identifies mixed-precision
  rounding for this case, not evidence of arbitrary driver corruption.
  Native close/process removal/mount release passed; services answered. The user
  confirmed the temporary black screen cleared. Capture was stopped.
  Tracked direct-memory peak 36.625 MiB; zero allocation denials/ledger errors.
  Evidence: `test-results/ps5/native-ggml-attn-diagnostic/20261015-183433/result.json`.
  Next is a separate full-FP32 scalar comparison with the same FP16 cache data;
  no upstream merge or model execution has occurred.
- The separate full-FP32 scalar attention comparison **passed on hardware**,
  PID 147 / app ID `0x4018`: actual variant `fp32`, SPIR-V size 82,152,
  all 128 outputs within unchanged tolerance, zero mismatches; maximum
  absolute error `1.4901161193847656e-7`. CPU reference bits, dispatch tuning
  and FP16 K/V cache data are unchanged from the mixed-precision diagnostic.
  At index 77 the GPU is now 0.3407878279685974 versus reference
  0.3407879173755646, error `8.940696716308594e-8`.
  Fence/explicit cleanup and independent native process/mount release passed.
  Services answered; log capture stopped. Tracked peak 36.625 MiB, zero
  denials/ledger errors. Whole-run diagnostic time 9.744221 s is not throughput.
  Evidence: `test-results/ps5/native-ggml-attn-fp32/20261015-184307/result.json`.
  The global disable-F16 switch is a controlled comparison, not yet a validated
  model-wide precision/performance policy. Prefill and full models remain unproven.
- The attention-scoped causal-prefill workload **passed**, PID 148 / app ID
  `0x6018`: four queries, two Q heads / one KV head, 32 cached tokens,
  all 512 outputs within unchanged tolerance, zero mismatches, maximum absolute
  error `1.49011612e-7`. Actual shader `fp32`, device_fp16=true, GQA ratio 2,
  split_k=1, mask enabled: FP32 scalar attention selection works without
  globally disabling FP16 paths. This is not a new matmul performance result.
  Fence/explicit cleanup and independent native closure/mount release passed.
  Tracked peak 36.625 MiB, zero allocation denials/ledger errors. All services
  answered afterward; capture stopped. Reported total 888.960 ms is correctness
  timing, including CPU reference work, not inference throughput.
  Evidence: `test-results/ps5/native-ggml-prefill-scoped-fp32/20261015-185248/result.json`.
  Full model graphs, residency/allocation policy and model execution are still
  unverified; these small operator checks do not establish a model memory budget.
- `scripts/ps5-native-test.sh` launched PPSA99041 itself for the first time
  (PID 150, app ID `0x8018`): the launcher started it in 0.7 s, the prefill
  workload passed 512/512 again and the title closed and unmounted.
  Evidence: `test-results/ps5/PPSA99041/20261015-190216/`.
- **First model on the GPU** (Qwen3.5 0.8B Q4_K_M, pinned in
  [PORTING.md](PORTING.md#dense-model-baseline-decisions-pin-and-budget)).
  Run 1 (PID 152): the 300 s watchdog fired while llama.cpp was still creating
  tensors; unbuffered writes to `/data` from the title cost tens of milliseconds
  each and its debug log is thousands of lines. Closed cleanly.
  `test-results/ps5/PPSA99041/20261015-192544/`.
  Run 2 (PID 154): loaded in 0.81 s, then stopped itself at the context budget
  (271 MiB against a 256 MiB cap). Closed cleanly.
  `test-results/ps5/PPSA99041/20261015-193419/`.
  Run 3 (PID 156) **passed**: 32 greedy tokens, the first 22 identical to the CPU
  reference; the first difference is at a near tie (CPU top-two gap 0.244).
  Text: "A GPU (Graphics Processing Unit) is specialized hardware designed to
  perform parallel calculations, such as rendering 3D graphics, playing video
  games, or processing massive datasets". No NaN or repetition. All 24 layers and
  the output norm on the GPU (`Vulkan0` 298.47 MiB); the tied LM head (198.93 MiB
  Q6_K) stays in pinned host memory and runs on the CPU, because RADV here
  reports a `maxStorageBufferRange` below that tensor's size (3 graph splits).
  Memory: wrapped direct memory peaked at 1085 MiB (cap 1536), the model load
  added 764 MiB, the context 271 MiB (198 MiB of it llama's graph-scheduler
  bookkeeping), Vulkan memory peaked at 565 MiB and was all freed. Load 0.82 s;
  prompt 26 tokens at 69 tok/s; generation 7.2 tok/s with the LM head on two CPU
  threads. One run, not a benchmark. Closed and unmounted, no signal or abort.
  `test-results/ps5/PPSA99041/20261015-193802/`.
  An unchanged repeat (PID 158) gave the same 32 tokens and the same memory
  figures, and closed cleanly: `test-results/ps5/PPSA99041/20261015-194035/`.
  Correction (from the runs below): the 3-split placement was not caused by
  `maxStorageBufferRange`, and the LM head sat in the plain `CPU` buffer, not
  `Vulkan_Host` (that one held the input embedding). Cause and fix:
  [PORTING.md](PORTING.md#lm-head-actual-cause-and-fix-2026-10-15-evening).
- **LM head on the GPU.** Device limits as RADV reports them here
  (`PS5LM_VK_LIMITS`): `maxStorageBufferRange` 4294967295,
  `maxMemoryAllocationSize` and `maxBufferSize` 4294967292, no
  `VK_EXT_shader_64bit_indexing`, shaderInt64 and buffer device address yes,
  UMA (integrated) yes; `maxComputeWorkGroupCount` x 2^32-1, y and z 65535.
  Run A (PID 160, eboot `ece5b335…bed4`): with ggml's op-size limit at 512 MiB
  the output matrix went to `Vulkan0` (model buffer 497.39 MiB, 2 splits), then
  the title's dispatch guard (65535 groups per dimension) stopped the first
  decode: ggml-vk had dispatched the 248320-row LM-head mat-vec unsplit because
  RADV allows 2^32-1 in x. Safe stop, closed and unmounted.
  `test-results/ps5/PPSA99041/20261015-195314/`.
  Run B (PID 162, eboot `cd39599d…6f82`, ggml told x ≤ 65535 so it splits into
  z slices) **passed**: the same 32 tokens as before (first 22 equal to the CPU
  reference, near tie at 22), 2 graph splits, LM head on `Vulkan0`, no CPU model
  buffer. Generation 140.4 tok/s (llama's own eval timer 154 tok/s) against 7.2
  before; prompt 98.7 tok/s against 69. Memory: load +764 MiB (cap 1152),
  context +267 MiB (cap 384), wrapped direct memory peak 1081 MiB (cap 1536),
  Vulkan peak 760 MiB (cap 1408), all freed. One run, not a benchmark.
  `test-results/ps5/PPSA99041/20261015-195528/`. The unchanged repeat (PID 164)
  gave identical tokens and memory, 139.4 tok/s:
  `test-results/ps5/PPSA99041/20261015-195622/`.
- **First MoE on the GPU** (Granite 3.1 1B-A400M Q4_K_M, 32 experts / 8 active,
  pinned in [PORTING.md](PORTING.md#small-moe-decision-pin-and-budget-2026-10-15-evening)).
  Run C (PID 166, eboot `f0786ea6…6b40`) failed safely on a build bug: the
  MoE flag reached `main.c` (which checked the Granite file) but not the loader,
  which loaded Qwen3.5 again (2 splits, 152 tok/s eval) and compared it with the
  Granite reference (`model_prompt_mismatch`). Closed and unmounted.
  `test-results/ps5/PPSA99041/20261015-200115/`.
  Run D (PID 168, eboot `0c91055b…890a`) **passed** by the near-tie rule: the
  first difference is at token 1, where the CPU's top two logits are 0.213
  apart (the Metal stand-in diverged there too). A host check that forces the
  GPU's token 1 into the CPU run gives the GPU's next 24 tokens exactly, the next
  difference again at a near tie (0.127):
  `test-results/local/moe-forced-continuation/20261015-200354/`. Text: "A
  Graphics Processing Unit (GPU) and a Central Processing Unit (CPU) are two
  distinct types of processors designed for different purposes, despite their
  shared function of executing". 25/25 offloaded, 2 splits; `Vulkan0` 782.12 MiB
  (all experts and the LM head), input embedding 39.38 MiB in `Vulkan_Host`.
  Generation 160.6 tok/s (llama's eval timer 164), prompt 22 tokens at 45.5 tok/s,
  load 9.1 s (file read). Memory: load +826 MiB (cap 1152), context +93 MiB (cap
  384), wrapped direct memory peak 991 MiB (cap 1536), Vulkan peak 876 MiB (cap
  1408), all freed. One run, not a benchmark.
  `test-results/ps5/PPSA99041/20261015-200238/`. The unchanged repeat (PID 170)
  gave identical tokens and memory, 161.2 tok/s:
  `test-results/ps5/PPSA99041/20261015-200310/`.

The remaining sections describe the earlier Slim. Payload numbers came from
`probes/memprobe` and `probes/threadprobe`, run through shsrv.

## Setup

- PS5 Slim (CFI-2015), firmware 13.60
- Relapse through WebKit Autoloader v0.5.2 and Payload Manager (PLK)
- Autoload: kstuff-lite 1.11, ShadowMountPlus 1.7beta2, etaHEN 2.5B, ftpsrv 0.21.1, shsrv 0.20, klogsrv 0.9
- FTP upload from a Mac on the same network: about 61 MB/s

The Relapse kernel stage sometimes hangs and the console then shuts down, or sticks on "preparing to turn off" until the power button is held. That is the exploit's race, not anything PS5LM runs.

## Memory, for one payload process

| What | Value |
|---|---|
| Flexible memory configured / free | 8.00 / 7.97 GiB |
| malloc ceiling (256 MiB steps, pages touched) | 6.25 GiB |
| Largest single block (`posix_memalign`) | 6.00 GiB |
| Direct memory | none: size 0, allocation fails with `0x80020023` |

The flexible pool is shared with the system: holding about 6 GiB froze the home screen until the process ended. Plan on 4 to 5 GiB for model, context and buffers in a payload. Bigger models (Qwen 3.8 27B) need a process with direct memory, that is a native app.

Why a payload has no direct memory: elfldr starts every payload as a new process with budget 0 (`sys_budget_set(0)` before it executes SceSpZeroConf, `elfldr.c` in ps5-payload-dev/elfldr), the budget of a system app. `0x80020023` is `0x80020000 + EAGAIN`: the pool is empty, the call is not refused. etaHEN's jailbreak request changes a process's rights (uid, sandbox, authid), not its budget, and kstuff patches no budgets; every homebrew project using more than 6 GiB is a native app.

## CPU, for one payload process

- AVX, AVX2, FMA, F16C, BMI2; no AVX-512. 16 logical CPUs online.
- **Affinity mask `0xea00`: only CPUs 9, 11, 13, 14 and 15**, shared with system services. Sony priority 700 (normal) for the main thread and new threads.
- 1, 2 and 4 threads busy for a second each: fine.
- Busy-waiting on all five (llama.cpp's defaults: one thread per physical core, `--poll 50`) starved the system: services stopped answering and the console shut down twice. `patches/0001-ps5-safe-cpu-defaults.patch` makes the default thread count the allowed CPUs minus two and turns polling off.
- `--poll 0` only ends the spinning between graphs. ggml's barrier spins at every operation inside a graph whatever the poll value, so during generation every worker runs flat out: the thread count is the real load setting.
- Only CPUs 14 and 15 are the two halves of one core (SMT siblings are 2k and 2k+1, as PS5SX2 and PS5CEMU-HAR lay them out). 9, 11 and 13 share their cores with threads outside the payload.
- llama.cpp's `-C`, `--cpu-strict` and `--prio` did nothing here: ggml took the PS5 for an unsupported platform. `patches/0002-ps5-thread-affinity-and-priority.patch` maps them to `scePthreadSetaffinity` and `scePthreadSetprio`, only ever lowers priority, and leaves the main thread unpinned. Not measured yet (#7).

## llama.cpp

First run, Qwen3.5 0.8B Q4_K_M, `-t 2 --poll 0 -lm none`, 512 context:

| | |
|---|---|
| Load | 4.4 s |
| Prompt | 38.5 tok/s |
| Generation | 14.2 tok/s |

`llama-server` loads Granite 4.2 3B (2.1 GiB) with mmap, so mapping a model file works in a payload.

Chat through `llama-server` and `ps5/webui` in the PS5's browser (browser open, `-c 4096 -np 1 -lm none`, 3 threads from the safe defaults):

| Model | Prompt | Generation |
|---|---|---|
| Qwen3.5 0.8B Q4_K_M, thinking off | 67 tok/s | 13.1 tok/s |
| Granite 4.2 3B Q4_K_M | 0.6 tok/s | 0.2 tok/s |

llama.cpp's own web UI renders blank in the PS5 browser; `ps5/webui` (plain ES5) works.

## Payload memory gets paged out

Models over ~1 GB ran 10 to 65 times slower than their size suggests (Granite 4.2 3B at 0.2 tok/s, Qwen3.5 2B at 0.9 tok/s). The reason is paging, not the architecture: while the PS5 browser is in front, the system pages a background payload's memory out to disk. An idle ps5lm was found holding 6 MB of a 600 MB model; the next request paged it back in at 0.3 tok/s.

Locking the weights (`-lm mlock`) fixes it: Qwen3.5 2B went from 0.9 to **8.5 to 9.2 tok/s** and stayed at 1.4 GB resident. Locked pages come out of the pool the home screen needs, though: 1.4 GB locked was fine, **2.7 GB (Qwen3.5 4B) froze the console**. ps5lm locks every model and refuses files over 1.6 GB.

## Downloads

- Sony's `libSceHttp2` cannot send a request from a payload (`sceHttp2SendRequest` fails); plain sockets, DNS and TCP 443 work. ps5lm uses cpp-httplib over mbedTLS 3.6.7 with the Mozilla CA list.
- The kernel caps `SO_RCVBUF` at 64 KB, whatever is asked for. With the Hugging Face CDN ~150 ms away, one connection tops out at 0.9 MB/s.
- 16 connections fetching 8 MiB pieces in parallel: **about 12.5 MB/s** (Qwen3.5 4B, 2.6 GB, in 217 s).

At exit, Sony's libc prints `[SceLibc] A heap error is detected` (SceLibcInternalHeap) after all work is done. After `main`, the SDK's start code unloads the modules the payload loaded and calls Sony's `exit()`, which runs the C++ static destructors while other threads may still run; the tools now leave through `_Exit` instead (`ps5/compat/exit.cpp`). Not yet checked on the console (#3).

## shsrv quirks

- Arguments are split on spaces with no quoting: put prompts in a file (`-f`).
- Programs need an absolute path; a relative one, and very long command lines, end with exit code 3 ("command not found").
- `kill` takes no `-9`; Payload Manager's `/process_kill?pid=N` works.
- `browse <URL>` opens the PS5's web browser.
