# PS5LM roadmap

The goal: llama.cpp running natively on a jailbroken PS5, able to load any GGUF model, with **Qwen 3.8 27B** as the first model to run on the console.

The plan goes CPU first, GPU second. The CPU path needs nothing but the payload SDK and gets Qwen 3.8 talking on the console soonest. The GPU path is where the speed is, and it is the part nobody has done with llama.cpp yet.

## The model and the memory

Qwen 3.8's smallest open model is the 27B (dense, hybrid Gated DeltaNet and gated attention, GGUF architecture `qwen35`). The PS5 has 16 GB of GDDR6 shared by the CPU, the GPU and the system, so only the low-bit quants fit ([unsloth/Qwen3.8-27B-GGUF](https://huggingface.co/unsloth/Qwen3.8-27B-GGUF)):

| Quant | Size | Role |
|---|---|---|
| UD-IQ1_M | 6.27 GiB | fallback if memory is tight |
| **UD-IQ2_XXS** | 6.77 GiB | first light candidate |
| UD-IQ2_S | 7.80 GiB | |
| **UD-Q2_K_XL** | 9.15 GiB | main target: K-quants are the cheapest to decode on CPU and GPU |
| UD-IQ3_XXS | 10.18 GiB | stretch, if a process can hold ~11 GiB |
| UD-Q4_K_M | 15.33 GiB | does not fit |

Only one layer in four keeps a KV cache (the others are Gated DeltaNet with a fixed state), so long contexts stay cheap. The vision encoder (`mmproj`, 0.86 GiB) is left out until the text model runs.

Decoding is bound by memory bandwidth: tokens per second ≈ effective bandwidth / bytes read per token (about the model size). The GDDR6 peaks at 448 GB/s (576 GB/s on the Pro), so the GPU ceiling for Q2_K_XL is around 45 tok/s. What the CPU can pull is what Phase 0 measures.

## Phase 0: toolchain and console facts

- [x] ps5-payload-dev SDK v0.43, pinned by checksum (`scripts/setup-sdk.sh`)
- [x] macOS host toolchain: Homebrew `llvm@21` and `lld` (`scripts/env.sh`)
- [x] `probes/memprobe` builds
- [x] `scripts/host-relapse.sh` serves the exploit page from macOS or Linux; `scripts/console-setup.sh` sends ftpsrv, klogsrv and shsrv after it
- [x] Console jailbroken (Relapse, firmware 13.60 or lower), with ftpsrv (2121), shsrv (2323) and klogsrv (3232)
- [x] memprobe and threadprobe on the console: results in [CONSOLE.md](CONSOLE.md)
  - direct, flexible and malloc ceilings for one payload process
  - the largest single allocation (llama.cpp's CPU backend puts all weights in one buffer)
  - read bandwidth with 1 to 8 threads, on malloc and on direct memory
  - AVX2 FMA GFLOPS
  - file read and mmap speed on a GGUF in `/data/PS5LM/models`

**Done when** we know how many GiB one process can hold and how fast it can read them.

## Phase 1: llama.cpp on the CPU, first light

- [x] ggml and libllama cross-compile with no source changes
- [x] `llama-cli`, `llama-server` and `llama-bench` link ([PORTING.md](PORTING.md) lists the three build fixes)
- [x] Smoke test: Qwen3.5 0.8B generates text on the PS5 CPU, 14.2 tok/s on two threads
- [x] Safe CPU defaults for the PS5 (`patches/0001`): threads = allowed CPUs minus two, no busy-waiting
- [x] `llama-server` on the console with a chat page in the PS5's own browser (`scripts/ps5-chat.sh`, `ps5/webui`); phones and laptops use the same page on port 8081
- [ ] `llama-bench` numbers for the small model
- [ ] **First light: Qwen3.8-27B generates text on the PS5.** UD-Q2_K_XL if Phase 0 says it fits, UD-IQ2_XXS if not. A video of it, posted with the repo.

Expect 1 to 3 tok/s here. Slow, but it is Qwen 3.8 running on a PS5, and the claim we want first.

**Done when** Qwen 3.8 answers a prompt on the console.

## Phase 1.5: the launcher and the model library

An Ollama-like way to get models and chat, without a PC in the loop. One payload, `ps5lm.elf` (v0.1):

- [x] A small web server (port 8082) with the **model library** page, opened in the PS5's own browser on start (`sceSystemServiceLaunchWebBrowser`). The same page works from a phone or laptop on the network.
- [x] The library: a catalog of models measured on the console, plus whatever is already in `/data/PS5LM/models`, with files over the payload limit (1.6 GB locked) refused
- [x] **Download** from Hugging Face straight to `/data/PS5LM/models`, with progress and resume: cpp-httplib over mbedTLS, 16 connections in parallel (the kernel caps one connection at 0.9 MB/s), about 12.5 MB/s
- [x] **Run**: llama-server runs inside the same process (`llama_server()`), and switching models stops it (`llama_server_terminate()`) and starts it again, with the model locked in memory
- [x] **Chat** in a plain page (llama.cpp's own UI renders blank in the PS5 browser), on the TV or from any device on the network, and the OpenAI-compatible API on port 8081

**Done when** a model can be picked, downloaded and chatted with using only the DualSense. Done in v0.1.0 (2026-10-02); v0.1.1 restyled the pages. Payload Manager installs it from `https://ps5lm.cobanov.dev/payloads.json`, and a listing in the default mirror is requested (itsPLK/ps5-payloads-mirror#19).

## Phase 2: fit and speed on the CPU

- [ ] Weights in direct memory if malloc's ceiling is too low: a ggml CPU buffer type on `sceKernelAllocateDirectMemory`, as a patch in `patches/`. Needs the native app: a payload's budget has no direct memory ([CONSOLE.md](CONSOLE.md)); the recipe is in [RESEARCH.md](RESEARCH.md)
- [ ] Loading: mmap or `--no-mmap`, from `/data` or a USB drive, whichever Phase 0 shows is faster
- [ ] Threads: the best `-t` for the cores a payload is allowed, with pinning (`patches/0002` makes `-C` and `--prio` work, #7)
- [ ] Qwen 3.8's native MTP block for speculative decoding
- [ ] Context budget: 8k to 32k tokens

**Done when** Q2_K_XL runs at the best speed the CPU allows, and the numbers are in `docs/CONSOLE.md`.

## Phase 3: the GPU through Vulkan

The route is ggml's Vulkan backend on [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)
and [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa). On the current Pro, a tiny
native RADV dispatch and five bounded FP32 ggml operator cases passed with clean
native closure. Full model compatibility, residency and inference performance
remain unverified. The target is a small resident dense model first, then a
smaller fully resident MoE, not the earlier 27B/performance targets.

- [x] Build RADV locally on Mac with `scripts/build-native-radv.sh`; pinned archive/provenance in [PORTING.md](PORTING.md).
- [x] Native app shell with standalone Kstuff Lite / ShadowMountPlus, no etaHEN; PPSA99040 queries and clean close passed.
- [x] PPSA99041 tiny GPU test: all 64 integer outputs correct, fence and cleanup complete, clean shell-managed closure. Evidence in [CONSOLE.md](CONSOLE.md).
- [x] ggml-vulkan and libllama cross-compiled from the pinned source export; 1,567 host-generated SPIR-V modules validated and the bounded native package linked with audited imports.
- [ ] Bounded `test-backend-ops` coverage for the selected dense and MoE graphs matches the CPU.
  - Initial five FP32 cases (`ADD`, `MUL`, `RMS_NORM`, `MUL_MAT`, `MUL_MAT_ID`) passed on the native GPU: 320 outputs, zero mismatches, independently observed clean closure.
  - Expanded suite: Q4_K matvec/matmul/MoE and Q6_K matmul passed all 200 outputs. Mixed-precision attention decode failed one of 128 outputs; prefill was not run. Clean native closure passed.
  - Attention diagnostic captured all 128 values and `fp16_f32acc` tuning. Fused-FP16 host emulation reproduces every GPU output exactly, attributing this case's mismatch to mixed-precision arithmetic.
  - Controlled FP32 scalar decode comparison passed on hardware: identical FP16 cache values, CPU reference and dispatch tuning, unchanged tolerance, zero mismatches, maximum error `1.49011612e-7`, clean native closure.
  - Attention-scoped FP32 causal prefill passed on hardware: four queries, all 512 outputs checked, zero mismatches, maximum error `1.49011612e-7`, actual `fp32` shader with `device_fp16=true`, clean closure and services responsive. Targeted tiny operators pass; full selected-model coverage and resident-model validation remain open. Reuse upstream app/server work while retaining the audited runtime.
- [x] `scripts/ps5-native-test.sh`: deploy, launch and watch a native title from the Mac, so a GPU test needs no Play by hand ([MAC_DEV_LOOP.md](MAC_DEV_LOOP.md#native-titles)). Verified on the console 2026-10-15.
- [ ] Conservative prompt batches and bounded GPU submissions; record completion/timeouts before increasing load.
- [x] Establish model, context and scratch residency with a small dense model. Qwen3.5 0.8B Q4_K_M ran on the GPU with an explicit 1.5 GiB budget (peak 1085 MiB) and matched the CPU reference: 22 identical greedy tokens, then a near tie. At first the LM head ran on the CPU (see the next item, now fixed). The reported 12 GiB direct budget is still not a validated safe allocation limit.
- [x] Run the LM head on the GPU. The cause was not the storage-buffer range (RADV reports 4 GiB) but ggml's op-size limit forced to 240 MiB, which refused llama's 485 MiB load-time probe; fixed with a 512 MiB op-size limit plus ggml splitting dispatches at the title's 65535-group guard. Qwen3.5 0.8B: 2 splits, same tokens, generation 7.2 → 140 tok/s ([PORTING.md](PORTING.md#lm-head-actual-cause-and-fix-2026-10-15-evening)).
- [x] Small resident MoE: Granite 3.1 1B-A400M Q4_K_M passed on the GPU (near tie at token 1, then 24 tokens equal to the CPU given that token), 160 tok/s, direct peak 991 MiB of 1536 ([CONSOLE.md](CONSOLE.md)).
- [ ] Run a smaller fully resident MoE after the dense baseline; SSD expert streaming remains deferred.
- [ ] Larger resident MoE models, climbing the memory in written steps ([PORTING.md](PORTING.md#larger-moe-models-the-climb-pins-and-budgets-2026-10-15-late)). Step 1, Granite 3.1 3B-A800M Q4_K_M in a 3072 MiB budget: passed (20 tokens equal to the CPU, then a near tie), through the API too, 149 tok/s generation. Step 2, OLMoE-1B-7B-0125 Q4_K_M (4.2 GB) in 6144 MiB: passed (25 tokens equal), 232 tok/s generation through the API, and repeated identically. Step 3, Qwen3-30B-A3B-Instruct-2507 IQ2_M (9.2 GiB) in 10560 MiB: loaded and ran fully resident (peak 9723 MiB, closed cleanly) but failed the pass rule at token 7 (CPU gap 1.415; the CPU forced with the GPU's token reproduces the rest), so the climb stopped at 6144 MiB ([CONSOLE.md](CONSOLE.md)).
- [ ] Record repeatable prompt-processing and generation benchmarks without extrapolating external results.

**Done when** selected resident models produce correct output through the LAN API and repeatable local benchmarks are recorded.

### Next on the GPU (2026-10-15; steps 0 to 3 done)

Steps 0 and 1 are done; nothing for steps 2 and 3 has been run. Each step is one bounded
`ps5-native-test.sh` run, read before the next, as the dense baseline was.

**0. LM head on the GPU: done (2026-10-15).** The cause was ggml's op-size
limit, which the title had forced to 240 MiB: llama's load-time probe for the
output layer is a 512-row matmul with a 485 MiB result. Raised to 512 MiB (it
sizes no allocation), and ggml-vk now sees the title's 65535-group dispatch
limit so it splits the 248320-row mat-vec. Qwen3.5 0.8B: 2 splits, same tokens,
generation 7.2 → 140 tok/s. See PORTING.md "LM head: actual cause and fix".

**1. Smaller fully resident MoE: done (2026-10-15).** Granite 3.1 1B-A400M
Q4_K_M (`bartowski/granite-3.1-1b-a400m-instruct-GGUF@940d2e1f`, 821,847,360
bytes, SHA-256 `3a2ec1c2…42eb`) passed on the GPU and repeated identically:
near tie at token 1, then 24 tokens equal to the CPU given the same token,
2 splits, 160 tok/s generation, direct-memory peak 991 MiB of a 1536 MiB cap.
See PORTING.md "Small MoE". Next size up, still under the 2 GiB per-file limit:
the 3B-A800M Q4_K_M at `be9a36f042806cb586bc65556c527079782b78e0`,
2,016,888,384 bytes,
`48e0edcd578fd4462f26127f04c651d0e650741110185297741089aea01a82b3`; it needs a
larger written budget (weights ~2.1 GiB with transients, so a total near
2.75 GiB) before its first load. One model per launch: the `abhi245y/PS5LM`
fork reports ~5 GiB of GPU memory held after an in-app unload, so never unload
and reload within a run.

**2. LAN API: done (2026-10-15, night).** Build modes `server` and
`server-qwen` run llama.cpp's server in PPSA99041 for a bounded window (600 s
or `POST /ps5lm/shutdown`), then stop, free and close through the verified
route. Through the API, Granite's temperature-0 completion is identical to the
model mode's GPU tokens; chat, streaming and repeated requests work; an
unchanged repeat launch matched. See PORTING.md "LAN API" and CONSOLE.md.
The plan as it was written:

Port upstream's `llama_server()` integration (`ps5/app/main.cpp`
in `cobanov/PS5LM`) into a new build mode of PPSA99041, not its build: keep our
native imports, close path, ledger, watchdog-free serving loop with an explicit
`/quit` that calls `sceShellCoreUtilExitApp`, and the closed environment.
Needs: `compat_app.c`'s socket/resolver pieces that the server really imports
(checked by the validator, no wildcard stubs), a bind on the LAN address only,
and the fork's `0005-listener-survives-network-drops.patch` (plus `0004` for
Anthropic-style system messages) moved into `patches/` after review. A server
outlives the 300 s watchdog, so this mode needs a different liveness rule
(health endpoint plus a bounded idle shutdown) written down before the first run.

**3. Benchmarks: done through the API (2026-10-15, night).**
`scripts/ps5-api-bench.py` (128- and 512-token prompts, 128 generated tokens,
temperature 0, one warm-up and three repetitions, llama-server's own timings):
Granite 3.1 1B-A400M at about 1370–1400 tok/s prompt and 210 tok/s generation,
and an unchanged repeat launch within 0.5 %. See CONSOLE.md "LAN API
benchmarks". Qwen3.5 0.8B through the same server: 1744 / 1884 tok/s prompt (128 / 512
tokens), 214 / 211 tok/s generation, tokens identical to its model mode.
The in-title variant below was not needed for that and is not done.

**Next proposals** (none started): (a) find what holds generation at
~210 tok/s for both models: count and time the ~13 submissions per token, then
try ggml-vk's fusion and graph optimisation, which this title switches off, one
at a time with the token check; (b) prompt processing with larger ubatches
(64-512) once each submit's time is known to stay far below the 10 s
device-lost limit; (c) the next model size step under AGENTS.md's rules,
with a written budget. The plan as it was written:

In the title, not over the network first: fixed prompts of
128 and 512 tokens and 64 generated tokens, three repeats after one warm-up,
greedy, reporting prompt and generation tok/s with min/median/max and the memory
peaks, in one `PS5LM_COMPUTE_RESULT`. Then the same through the LAN API from the
Mac. Compare only like with like (same model file, context, batch, threads), and
never extrapolate other consoles' numbers (upstream's 9–21 tok/s 27B result,
the fork's 124 tok/s on a Slim) to this Pro.

## Phase 4: the app

- [ ] A chat screen on the TV, with the DualSense and the system keyboard
- [ ] Model picker over `/data/PS5LM/models` and USB drives
- [ ] `llama-server` always on, so the PS5 serves an OpenAI-compatible API to the home network
- [ ] Releases: payload ELFs and the app folder, with an install guide

**Done when** someone who is not a developer can install it and chat.

## Later

- Qwen3.8-Flash-Next (180B total, 6B active) with experts streamed from the SSD
- Vision input for the 27B (`mmproj`)
- Upstream the small portability fixes to llama.cpp

## Risks

| Risk | Plan B |
|---|---|
| A payload process gets far less than 7 GiB (**confirmed: about 6 GiB, shared with the system**) | Move to the native app shell (Phase 3's) sooner, where direct memory is available; smaller models meanwhile |
| The CPU is too slow for a 27B | Still first light; Phase 3 is the fix |
| ggml-vulkan needs a feature the driver lacks | Its feature switches (`GGML_VK_DISABLE_*`), or fix it in the driver with its author |
| Someone else gets there first | Keep Phase 1 short and publish as soon as it talks |
| A firmware update closes the exploit | Keep the console on 13.60 or lower, automatic updates off |
