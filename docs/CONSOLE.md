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
- **LAN API: llama.cpp's server in the title** (build mode `server`, Granite
  3.1 1B-A400M, eboot `2303fcb0…2eaf`; settings and budget in
  [PORTING.md](PORTING.md#lan-api-server-mode-lifetime-and-budget-2026-10-15-night)).
  Run 1 (PID 172) **passed**: model loaded in 8.5 s, listening on
  `0.0.0.0:8081` 9.0 s after launch. Through the API
  (`scripts/ps5-api-check.py`, 9/9): `/tokenize` gives the model mode's 22 prompt
  tokens; a temperature-0 `/completion` of 32 tokens is **identical to the model
  mode's GPU tokens** (so the same near tie at token 1 against the CPU);
  `/v1/completions` gives the same text; a chat request, a streaming chat
  request (27 chunks) and the same completion again (identical) all succeed and
  `/health` stays `ok`. The window was ended with `POST /ps5lm/shutdown` after
  17 s of serving; stop to freed 0.1 s; 23 responses, 0 errors; closed and
  unmounted, port 8081 closed afterwards. Memory: load +846 MiB (cap 1152),
  context +136 MiB at 2048 tokens (cap 384), wrapped direct memory peak
  1084 MiB (cap 1536), Vulkan peak 920 MiB, Vulkan 0 after the server returned;
  18,358 queue submissions (the model mode's 8192 cap would have stopped it).
  `test-results/ps5/PPSA99041/20261015-203449/`,
  `test-results/ps5/api-check/20261015-203452/`. Run 2 (PID 174), launched again
  with nothing changed: identical tokens, memory and counts,
  `…/PPSA99041/20261015-203625/`, `…/api-check/20261015-203628/`. Benchmarks
  from the same windows: see "LAN API benchmarks" below.
- **LAN API with Qwen3.5 0.8B** (build mode `server-qwen`, eboot `2b469f29…672d`;
  from this build on the title also reads llama's placement lines in server
  mode and closes unless every layer and the output are on the GPU). Run 3
  (PID 176) **passed**: 25/25 offloaded, 2 graph splits; the API's 32 tokens
  are **identical to the model mode's GPU tokens** (first 22 equal to the CPU
  reference, as before); chat and streaming (thinking switched off), a second
  identical completion, `/health` ok; 23 responses, 0 errors. Memory: load
  +782 MiB, context +253 MiB (cap 384; `qwen35`'s large scheduler buffer),
  direct peak 1126 MiB (cap 1536), Vulkan peak 746 MiB, 0 after the server
  returned. `test-results/ps5/PPSA99041/20261015-204216/`,
  `test-results/ps5/api-check/20261015-204219/`.
- **Granite again with the final build** (eboot `de39308b…ec34`, the placement
  check included). Run 4 (PID 178) **passed**: 25/25 offloaded, 2 splits,
  identical tokens, memory and counts to runs 1 and 2.
  `…/PPSA99041/20261015-204410/`, `…/api-check/20261015-204415/`.
- **The window ends by itself.** Run 5 (PID 180, same build, no shutdown
  request; one completion early on): the server stopped at the end of its
  600 s window (`stop_reason: window`, served 600.03 s), freed in 0.1 s, and
  the title closed 609 s after launch, well inside its 840 s watchdog; port
  8081 closed afterwards. `test-results/ps5/PPSA99041/20261015-204458/`.

**LAN API benchmarks** (`scripts/ps5-api-bench.py`, from the Mac over the LAN
during the server windows above). Granite 3.1 1B-A400M Q4_K_M, context 2048,
batch = ubatch = 32, two CPU threads, one slot; prompts are the first 128 and
512 tokens of a fixed passage, sent as token ids; 128 generated tokens
(`ignore_eos`), temperature 0, prompt cache off; one warm-up and three
measured requests per size. Rates are llama-server's own `timings`; wall time is
the whole HTTP request.

| Run | Prompt | Prompt tok/s, median (min–max) | Generation tok/s, median (min–max) | Wall s, median |
|---|---|---|---|---|
| 1 (`api-bench/20261015-203521`) | 128 | 1368 (1352–1370) | 212.3 (211.6–214.7) | 0.73 |
| 1 | 512 | 1394 (1380–1400) | 210.0 (208.9–210.8) | 1.06 |
| 2, repeat (`api-bench/20261015-203639`) | 128 | 1374 (1371–1385) | 211.9 (211.8–215.0) | 0.71 |
| 2 | 512 | 1397 (1387–1399) | 209.1 (209.1–211.9) | 1.04 |
| 4, final build (`api-bench/20261015-204434`) | 128 | 1370 (1362–1391) | 212.5 (212.0–214.6) | 0.71 |
| 4 | 512 | 1396 (1390–1407) | 210.6 (209.2–211.2) | 1.02 |

Qwen3.5 0.8B Q4_K_M, same settings and script (run 3, `api-bench/20261015-204241`):

| Prompt | Prompt tok/s, median (min–max) | Generation tok/s, median (min–max) | Wall s, median |
|---|---|---|---|
| 128 | 1744 (1736–1773) | 214.0 (212.1–214.4) | 0.68 |
| 512 | 1884 (1863–1898) | 210.9 (210.0–211.1) | 0.92 |

The repeat's medians are within 0.5 % of the first run's (the limit set was
about 5 %); run 4, a rebuild that only added the placement check, is within
0.7 % of both. The generation rate is higher than the model mode's 160 tok/s
because that figure was the title's own wall clock around each token,
including a full-vocabulary argmax and a log line per token on the CPU; the
server's is llama's decode timer with sampling. Prompt processing at 512 tokens
is 16 batches of 32; larger batches were not tried. These are this console's
numbers only, for this model and these settings.

**Larger MoE models: the memory climb** (pins, budgets and the rule in
[PORTING.md](PORTING.md#larger-moe-models-the-climb-pins-and-budgets-2026-10-15-late)).
Same settings, pass rule and API check as above; each model is one model-mode
run and one server-mode run with the benchmark.

- **Step 1, Granite 3.1 3B-A800M Q4_K_M (total cap 3072 MiB): pass.** Model mode
  (PID 183, eboot `e2601805…6ac2`): the first 20 greedy tokens equal the CPU
  reference; the first difference is at token 20, where the CPU's top two
  logits are 0.105 apart; text "A GPU (Graphics Processing Unit) and a CPU
  (Central Processing Unit) are both essential components in computing
  systems, but they serve distinct purposes and have different architectures".
  33/33 offloaded, 2 splits (`Vulkan0` 1921.79 MiB, input embedding 59.07 MiB in
  `Vulkan_Host`). Load 21.3 s (about 95 MB/s), weights +1986 MiB (cap 2304),
  context +123 MiB (cap 384), direct peak 2181 MiB (cap 3072), Vulkan peak
  2052 MiB, 0 after free; 2626 submissions; generation 117.6 tok/s on the
  title's clock, prompt 44 tok/s. `test-results/ps5/PPSA99041/20261015-210939/`.
  Server mode (PID 185, eboot `e8ffd463…7c36`): API check 9/9, the completion
  identical to the model mode's GPU tokens; load 21.5 s, weights +2005 MiB,
  context +184 MiB at 2048 tokens, direct peak 2294 MiB, Vulkan peak 2113 MiB,
  0 after free, 23 responses, 0 errors, closed after `/ps5lm/shutdown`.
  `…/PPSA99041/20261015-211035/`, `…/api-check/20261015-211038/`.
  Benchmark (`…/api-bench/20261015-211113/`), medians (min–max): 128-token
  prompt 711 (707–721) tok/s, generation 149.5 (148.7–149.7) tok/s; 512-token
  prompt 709 (707–713) tok/s, generation 146.4 (146.2–147.5) tok/s.
- **Step 2, OLMoE-1B-7B-0125-Instruct Q4_K_M (total cap 6144 MiB): pass.**
  Model mode (PID 187, eboot `eaec7392…9326`): the first 25 greedy tokens equal
  the CPU reference; the first difference is at token 25 (CPU gap 0.241); text
  "A GPU (Graphics Processing Unit) and a CPU (Central Processing Unit) are both
  essential components in computers, but they serve distinct purposes and are
  designed with different". 17/17 offloaded, 2 splits (`Vulkan0` 3961.35 MiB,
  input embedding 55.27 MiB in `Vulkan_Host`). Load 45.6 s (about 92 MB/s),
  weights +4022 MiB (cap 4608), context +166 MiB (cap 512), direct peak
  4257 MiB (cap 6144), Vulkan peak 4152 MiB, 0 after free; 4437 submissions;
  generation 175.6 tok/s on the title's clock, prompt 56 tok/s. One llama
  warning: the GGUF's own "special_eos_id is not in special_eog_ids" note,
  printed on the CPU too. `test-results/ps5/PPSA99041/20261015-212150/`.
  Server mode (PID 189, eboot `1f4e572d…7065`): API check 9/9, completion
  identical to the model mode's; load 45.6 s, weights +4042 MiB, context
  +292 MiB at 2048 tokens, direct peak 4432 MiB, Vulkan peak 4278 MiB, 0 after
  free, 23 responses, 0 errors. `…/PPSA99041/20261015-212307/`,
  `…/api-check/20261015-212310/`. Benchmark (`…/api-bench/20261015-212407/`):
  128-token prompt 614.5 (613–624) tok/s, generation 232.4 (230.8–232.7) tok/s;
  512-token prompt 570.9 (568–573) tok/s, generation 223.1 (222.4–225.8) tok/s.
- **Step 3, Qwen3-30B-A3B-Instruct-2507 IQ2_M (total cap 10560 MiB): failed
  by the pass rule; the climb ends at 6144 MiB.** Model mode (PID 191, eboot
  `45f84bab…eae5`): the first 7 tokens equal the CPU reference; at token 7 the
  GPU chose " differs" where the CPU chose " and" with a top-two gap of 1.415
  (the GPU's own gap there: 0.154). The rule allows a first difference before
  token 8 only where the CPU's gap is within 0.5, so the title reported
  failure (exit 1). Text coherent: "A GPU (Graphics Processing Unit) differs
  from a CPU (Central Processing Unit) in its design and purpose. While a CPU is
  optimized for handling a wide range"; no NaN, no repeats. Everything else
  worked: 49/49 offloaded, 2 splits (`Vulkan0` 9279.82 MiB, input embedding
  127.51 MiB in `Vulkan_Host`), load 106.8 s (about 92 MB/s), weights +9447 MiB
  (cap 9728), context +205 MiB (cap 768), direct peak 9723 MiB (cap 10560;
  about 1.9 GiB of RADV's ~11.4 GiB left), Vulkan peak 9524 MiB, 0 after free,
  10,341 submissions, generation 37.4 tok/s and prompt 21.8 tok/s on the
  title's clock; closed and unmounted, console answering.
  `test-results/ps5/PPSA99041/20261015-214345/`.
  Offline diagnosis: with the GPU's first 8 tokens forced, the CPU produces all
  24 remaining GPU tokens exactly
  (`test-results/local/qwen3moe-forced-continuation/20261015-214738/`); the same
  title code on Metal (M2 Pro) stays on the CPU's path for 27 tokens but its
  logits differ from the CPU's by as much as the PS5's do (top-two gap shifts
  up to 1.39) (`test-results/local/model-metal-smoke/qwen3moe-20261015-2146/`).
  So this looks like 2-bit rounding crossing a decision boundary rather than a
  wrong kernel, but the rule was fixed before the run and is not changed after
  it. No server-mode run at this size.
- **Repeat of the largest passing model** (OLMoE model mode, eboot
  `eaec7392…9326` unchanged, PID 193): pass, identical 32 tokens, identical
  memory figures and submission count, generation 175.2 tok/s.
  `test-results/ps5/PPSA99041/20261015-214817/`.

**Performance work** (2026-10-15/16 night; method, knobs and budget in
[PORTING.md](PORTING.md#performance-measured-tuning-2026-10-15-night)). All
runs passed with exit 0 and `"closed": true`; the console answered after each.

- **Run 1, tuning window, Qwen3.5 0.8B** (`server-qwen`, eboot
  `821103cd…eebb`: counters, runtime knobs, 512-token context, defaults equal
  to the old settings): API check 9/9 identical to the model mode; 15
  configurations through `scripts/ps5-api-tune.py`, every one with check tokens
  identical to the model mode's and identical benchmark outputs. Baseline
  1731/1867 tok/s prompt (128/512), 212.0/211.6 generation, matching the
  earlier record. Context +506 MiB (cap 512), direct peak 1505 MiB (cap 1536),
  Vulkan peak 828 MiB, 0 after free, 404 responses, 0 errors.
  `test-results/ps5/PPSA99041/20261015-233747/`, `…/api-check/20261015-233751/`,
  `…/api-tune/20261015-233814/`.
- **Run 2, tuning window, OLMoE-1B-7B** (`server-olmoe`, eboot
  `b37f71fb…64c8`, same build): 9/9, 15 configurations all identical. Baseline
  615/571 prompt, 231.5/221.7 generation. Load 45.6 s; context +359 MiB (cap
  768), direct peak 4522 MiB (cap 6144), Vulkan peak 4345 MiB, 0 after free.
  Storage probe: `read()` of the file's first regions 1415 (1 MiB) / 2830
  (4 MiB) / 2237 (16 MiB) / 2289 (64 MiB) MB/s, four threads 3207 MB/s.
  `…/PPSA99041/20261015-234129/`, `…/api-check/20261015-234134/`,
  `…/api-tune/20261015-234228/`.
- **Run 3, model mode Qwen3.5 0.8B, kept defaults** (eboot `8647174d…a058e`:
  fusion and graph optimisation on, 1000 nodes per submit, `read(2)` loader):
  pass, rule `prefix` (first 22 equal, the 0.244 tie), 32 tokens identical to
  the earlier model-mode run `20261015-195528`; 25/25, 2 splits; weights
  +764 MiB, context +267 MiB, direct peak 1093 MiB; load 1.26 s (file cached);
  generation 150.4 tok/s on the title's clock (was 140.4); 1010 submissions
  (was 1143). `…/PPSA99041/20261015-235201/`.
- **Run 4, model mode OLMoE, kept defaults** (eboot `6ca3ae4f…e898`): pass,
  rule `prefix` (first 25 equal), 32 tokens identical to `20261015-212150`;
  17/17, 2 splits; **load 3.61 s (was 45.6 s)**, weights +4022 MiB, context
  +166 MiB, direct peak 4250 MiB; generation 221.4 tok/s on the title's clock
  (was 175.6). `…/PPSA99041/20261015-235237/`.
- **Run 5, server Qwen3.5 0.8B, kept defaults** (eboot `ab264294…b9`, chunk
  512): 9/9 identical to the model mode; benchmark 3146.6/5264.0 prompt,
  254.5/252.7 generation; submit cadences around the defaults (25 and
  100 MFLOP splits, 500 and 2000 nodes) all identical and none faster. Context
  +506 MiB (cap 768), direct peak 1410 MiB (cap 2048), Vulkan peak 828 MiB,
  0 after free, 183 responses, 0 errors. Storage probe on the cached file:
  `read()` 4.9–5.6 GB/s, `fread` 4.3 GB/s. `…/PPSA99041/20261015-235322/`,
  `…/api-check/20261015-235327/`, `…/api-bench/20261015-235335/`,
  `…/api-tune/20261015-235340/`.
- **Run 6, server OLMoE, kept defaults** (eboot `134e1f51…a365`): 9/9
  identical; benchmark 1429.0/2628.4 prompt, 326.9/309.2 generation; cadences
  none faster. Load 3.58 s; context +359 MiB, direct peak 4501 MiB, Vulkan
  peak 4345 MiB, 0 after free. Storage probe: `read()` 1657 (1 MiB) / 2834
  (4 MiB) / 2356 (16 MiB) / 2514 (64 MiB) MB/s, four threads 3712 MB/s,
  **`fread` of an uncached region 92.6 MB/s**, cached re-read 4870 MB/s.
  `…/PPSA99041/20261015-235759/`, `…/api-check/20261015-235804/`,
  `…/api-bench/20261015-235816/`, `…/api-tune/20261015-235821/`.

- **Run 7, server Granite 3.1 1B-A400M, kept defaults + MMVQ knob** (eboot
  `05a10f87…2213`): 9/9, completion identical to the model mode's GPU tokens
  (`20261015-200238`); benchmark 3328.1/6532.0 prompt, 344.6/335.7
  generation; MMVQ off changed nothing (535 dispatches either way). Load
  0.98 s (was 6.0 s); context +170 MiB, direct peak 1132 MiB, 0 Vulkan after
  free. `…/PPSA99041/20261016-000917/`, `…/api-check/20261016-000922/`,
  `…/api-bench/20261016-000932/`, `…/api-tune/20261016-000936/`.
- **Run 8, server Granite 3.1 3B-A800M** (eboot `51faea5e…940f`): 9/9; the
  completion is now identical to the CPU reference for all 32 tokens (rule
  `identical`; the old model mode split at the 0.105 tie at token 20);
  benchmark 1762.3/3320.8 prompt, 236.7/228.1 generation; MMVQ off: no change.
  Load 2.11 s (was 21.5 s); context +243 MiB (cap 384), direct peak 2380 MiB
  (cap 3072). `…/PPSA99041/20261016-001031/`, `…/api-check/20261016-001046/`,
  `…/api-bench/20261016-001047/`, `…/api-tune/20261016-001053/`.
- **Run 9, server OLMoE, current code** (eboot `1f7ac7c9…618b`): 9/9
  identical; defaults 1427.7/2656.0 prompt, 329.3/309.9 generation (run 6
  within 1 %); MMVQ off: no change (326 dispatches either way).
  `…/PPSA99041/20261016-001255/`, `…/api-check/20261016-001300/`,
  `…/api-tune/20261016-001311/`.
- **Run 10, server Qwen3.5 0.8B, final code** (eboot `9d64ad93…383a`): 9/9
  identical; benchmark 3156.5/5271.2 prompt, 253.5/251.3 generation (run 5
  within 0.6 %); load 1.09 s; direct peak 1407 MiB (cap 2048).
  `…/PPSA99041/20261016-001630/`, `…/api-check/20261016-001641/`,
  `…/api-bench/20261016-001644/`.
- **Runs 11 and 12, model mode, final code** (Qwen eboot `d8c6410f…56b1`,
  OLMoE `354b6fb4…cc00`): both pass with rule `prefix` (22 and 25 equal
  tokens), tokens, memory figures and submission counts identical to runs 3
  and 4; loads 0.84 s and 3.59 s. `…/PPSA99041/20261016-001703/`,
  `…/PPSA99041/20261016-001732/`. Console answering afterwards, port 8081
  closed.

| Model (API, 128/512-token prompt) | Prompt tok/s before → after | Generation tok/s before → after | Load before → after |
|---|---|---|---|
| Qwen3.5 0.8B Q4_K_M | 1744 / 1884 → 3147 / 5264 | 214.0 / 210.9 → 254.5 / 252.7 | 0.8 s → 0.8–1.3 s (cached file) |
| OLMoE-1B-7B-0125 Q4_K_M | 615 / 571 → 1429 / 2628 | 232.4 / 223.1 → 326.9 / 309.2 | 45.6 s → 3.6 s |
| Granite 3.1 1B-A400M Q4_K_M | 1370 / 1396 → 3328 / 6532 | 212.5 / 210.6 → 344.6 / 335.7 | 6.0 s → 1.0 s |
| Granite 3.1 3B-A800M Q4_K_M | 711 / 709 → 1762 / 3321 | 149.5 / 146.4 → 236.7 / 228.1 | 21.5 s → 2.1 s |

**SSD streaming, 2026-10-16** (PORTING.md "SSD streaming"; at most 10 runs,
one used):

- **Run 1, stream probe** (`stream-probe`, eboot `e734fdfc…aaf7`, total cap
  1024 MiB, no model load): **exit 5, title not closed: stop condition.**
  The title started (PID 219, `flex` 403,701,760 B available, direct
  12,863,930,368 B, 16 KiB pages, `vm.stats` sysctls unreadable in a title),
  printed its first line and then never returned from `mmap(PROT_READ,
  MAP_SHARED)` of the 3.9 GiB OLMoE file / `mincore()` over that mapping. In
  the kernel log, ShellCore's `VM Stats` wire count rose by ~100 MiB every
  ~5 s (451 → 1212 MiB), the kernel's memory stats then showed the whole
  system pool wired (`total 79598, free 476 … wire 78455` pages of 16 KiB:
  ~1.2 GiB), ShellCore and the UI were swapped out (`swap out` 93.6 → 152 MiB,
  their RSS fell), and `FMEM allocation timeout` repeated 16 times. The
  watchdog (300 s) wrote its result (rc 124) and the exit request returned
  `close_rc 0`, but no `ProcessTerm` followed before the script's 300 s
  timeout. Afterwards FTP, klog, Payload Manager and ShadowMountPlus ports
  accepted connections, but `games/info` did not answer within 10 s and the
  kernel log sent nothing in 12 s. **The console needs a person** (check
  whether PPSA99041 is still open; reboot if the system is still starved).
  Evidence: `test-results/ps5/PPSA99041/20261016-003554/` (`klog.log`,
  `native-model.out`, `result.json`).
  Reading: a file mapping in a title is not lazily paged page cache on this
  firmware: its pages are wired from the system's small shared pool
  (consistent with the payload era, where an mmap'd model counted against the
  payload's 8 GiB flexible budget). **Never mmap a model file in a title.**
  The script's `-t` should exceed the title's watchdog by a minute, so a
  watchdog close can still be observed (here both were 300 s).

**Recovery check, 2026-10-16 07:54** (by the user after the hang above:
reboot, Kstuff Lite then ShadowMountPlus reloaded): the OLMoE model mode
(`354b6fb4…cc00`) passed and closed, `…/PPSA99041/20261016-075431/`.

**Vision, 2026-10-16** (PORTING.md "Vision"; at most 10 runs):

- **Run 1, Qwen3-VL 4B model mode** (`vl4b`, eboot `d09eab01…35cd`, total
  cap 6144 MiB): **fail by the embedding tolerance, closed, exit 1.** Text:
  first 10 tokens equal to the CPU's, the difference at the 0.0175 tie (rule
  `prefix`), 44 tokens, answer HELLO right, shapes right; 37/37 on the GPU, 2
  splits; the encoder graph in 1 split (766 nodes, nothing on the CPU).
  Image embedding against the CPU: relative L2 0.070 (tolerance 0.03),
  cosine 0.99766 (0.9995), 31 of 196 tokens below 0.995, max abs 1.10. The
  trace pass (one more encode) put the first tensor over tolerance at layer
  0's `ffn_out` (0.034; Metal 0.001), with every matmul output 5–30x worse
  than Metal's and the error at 0.03–0.11 through the layers. Memory as
  budgeted: text weights +2716 MiB (cap 3072), mmproj +1000 MiB (1152),
  context +227 MiB (1024), direct peak 4031 MiB (6144), Vulkan peak 3676 MiB,
  0 after free. Load 2.6 s, mmproj 9.0 s, encode 229 ms, image-embedding
  decode 379 ms (7 batches of 32), generation 114.9 tok/s (title clock).
  `…/PPSA99041/20261016-081904/`.
  Cause: ggml-vk runs F16-weight matmuls at the default precision with its
  FP16-accumulating pipelines (`ggml_vk_get_mul_mat_mat_f16acc`; fp16 device,
  coopmat off in the title). Fix: `patches/0004-ps5-clip-f32-matmul-accumulation.patch`.
- **Run 2, same with the patch** (eboot `915c2e95…f641`): **pass, exit 0,
  closed.** Embedding: relative L2 0.0082, cosine 0.999967, max abs 0.623
  (4.1 % of the largest reference value 15.08), one token (189, white
  background) at 0.976, every other above 0.995; per slice 0.0065 (main),
  0.0020, 0.0091, 0.0102 (deepstack 5, 11, 17); better than the Mac's Metal
  (0.0106). Text: all 47 tokens identical to the CPU's (rule `identical`),
  "The image displays the word "HELLO" in bold black letters above three
  geometric shapes—a red circle, a blue square, and a green triangle—arranged
  horizontally. The word written in the image is "HELLO"." Memory identical
  to run 1. Encode 231 ms (196 tokens, 784 patches), image decode 380 ms,
  text 316 ms, generation 114.8 tok/s, mmproj load 9.0 s (clip reads through
  `ifstream`, i.e. `fread`), text load 2.6 s. `…/PPSA99041/20261016-082235/`.

- **Run 3, Qwen3-VL 4B server mode** (`server-vl4b`, eboot `e5e4411b…7912`):
  **pass, exit 0, closed**, 22 responses, 0 errors, stopped by
  `/ps5lm/shutdown`. `scripts/ps5-api-vision.py`: an OpenAI chat request with
  the test image as a base64 PNG data URL, temperature 0, prompt cache off:
  the 44 token ids equal the model mode's GPU tokens (and the CPU's), answer
  HELLO, 224 prompt tokens. One warm-up + three requests of 64 tokens: image
  prompt (encode + 224-token prompt decode in one 512-token batch) 396.8 ms
  median (396.1–398.9), generation 113.2 tok/s (112.9–113.9), wall 0.97 s.
  The unchanged text benchmark on the same server: prompt 912.9 / 1171.3
  tok/s (128 / 512 tokens), generation 115.7 / 112.0 tok/s. Memory: text
  weights +2719 MiB, context +466 MiB (cap 1024), mmproj +990 MiB (cap 1152),
  direct peak 4382 MiB (cap 6144), Vulkan peak 3915 MiB, 0 after free. Load
  2.6 s, mmproj 9.0 s, ready 12.0 s after launch; the storage probe's
  `fread` ran at 92.7 MB/s again (clip loads through `ifstream`, so `fread`).
  `…/PPSA99041/20261016-083436/`, `…/api-vision/20261016-083500/`,
  `…/api-bench/20261016-083512/`.

- **Run 4, Qwen3-VL 8B model mode** (`vl8b`, eboot `bc9cae82…9004`, the new
  8192 MiB step): **pass, exit 0, closed.** Embedding: relative L2 0.0056,
  cosine 0.999984, max abs 0.054 (1.1 % of the largest reference value 5.03),
  no token below 0.995 (lowest 0.99954); per slice 0.0095, 0.0028, 0.0043,
  0.0062. Text: all 47 tokens identical to the CPU's, "The image displays the
  word "HELLO" in bold black letters above a row of three colored shapes: a
  red circle, a blue square, and a green triangle. The word written in the
  image is "HELLO"." 37/37, 2 splits; encoder 1 split, 853 nodes. Memory:
  text weights +4826 MiB (cap 5376), mmproj +1312 MiB (1408), context
  +228 MiB (1024), direct peak 6458 MiB (8192), Vulkan peak 6097 MiB, 0 after
  free. Load 4.6 s, mmproj 12.5 s, encode 291 ms, image decode 550 ms,
  generation 78.7 tok/s. `…/PPSA99041/20261016-085419/`.
- **Run 5, Qwen3-VL 8B server mode** (`server-vl8b`, eboot `9d63f6fb…e480`):
  **pass, exit 0, closed**, 22 responses, 0 errors. Image request: 47 token
  ids equal to the model mode's and the CPU's, answer HELLO; image prompt
  623.5 ms median (615.6–628.4), generation 79.2 tok/s (78.8–80.2), wall
  1.44 s. Text benchmark: prompt 516.5 / 623.9 tok/s, generation 79.8 / 78.5.
  Memory: weights +4829 MiB, context +514 MiB, mmproj +1312 MiB, direct peak
  6875 MiB (cap 8192), Vulkan peak 6383 MiB, 0 after free; ready 17.4 s after
  launch. `…/PPSA99041/20261016-085508/`, `…/api-vision/20261016-085537/`,
  `…/api-bench/20261016-085545/`.

- **Run 6, Qwen3-VL 8B model mode, unchanged repeat** (eboot `bc9cae82…9004`,
  left deployed): **pass, exit 0, closed**; embedding statistics, the 47
  tokens, memory (peak 6458 MiB) and 5342 submissions identical to run 4;
  encode 289 ms, 80.2 tok/s. `…/PPSA99041/20261016-085658/`. Console
  answering afterwards (FTP, klog, Payload Manager, ShadowMountPlus), port
  8081 closed, PPSA99041 unmounted.

| Vision (one 448 x 448 image, 196 image tokens) | Embedding rel. L2 vs CPU | Tokens vs CPU | Encode (model mode) | Image prompt via API | Generation model / API | Peak / cap |
|---|---|---|---|---|---|---|
| Qwen3-VL 4B Q4_K_M + F16 mmproj | 0.0082 | identical (47) | 231 ms | 397 ms | 114.8 / 113.2 tok/s | 4382 / 6144 MiB |
| Qwen3-VL 8B Q4_K_M + F16 mmproj | 0.0056 | identical (47) | 291 ms | 624 ms | 78.7 / 79.2 tok/s | 6875 / 8192 MiB |


- **2026-10-16, server-qwen with upstream v0.9.0's fixes** (eboot `bc8f8c3a…67fa`,
  left deployed): this build has the patched RADV (freed buffers wait for the GPU), the
  C-locale `localeconv`, and the Anthropic and tojson server patches
  ([PORTING.md](PORTING.md#fixes-from-upstream-v090-2026-10-16)).
  - **Result:** pass, exit 0, closed, PID 113; 127 responses, 0 errors.
  - **api-check 13/13:** the `temperature: 0.7` request answered, the Anthropic request with a
    mid-conversation system message answered, and the soak passed 100/100 (median 62 ms).
  - **Speed and memory:** tg 252.2/250.5 tok/s, pp 3127/5255, within 1% of before. Peak
    1409.6 MiB of 2048.
  - **Evidence:** `…/PPSA99041/20261016-090943/`. All four services answered afterwards.
- **2026-10-16, vl4b model mode with the mmproj read through `read(2)`** (eboot
  `6edb7318…e416`, patch 0005, patched RADV): pass, exit 0, closed. Mmproj load 8955 → 471
  ms; embedding, tokens (47/47, HELLO), memory (peak 4223 MB) and submissions equal to the
  earlier pass; 114.5 tok/s. Evidence: `…/PPSA99041/20261016-092200/`.

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
