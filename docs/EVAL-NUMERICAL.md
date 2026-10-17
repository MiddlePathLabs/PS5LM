# GPU-vs-CPU numerical validation at depth (2026-10-16 evening)

Phase A of the long-context evaluation. Question: are the PS5 GPU's long-context results
numerically correct against the same quantized model on the CPU, at depths beyond the
8K/16K already validated during the ladder?

**Method** (established in PORTING.md "Long context", unchanged): identical pinned GGUF,
identical deterministic prompts (token ids verified equal host↔console), temperature 0,
F16 KV, the model's greedy continuation compared by the model-mode rule (first 8 tokens
identical, or first difference where the CPU's top two are within 0.5), plus top-10
log-probability comparisons at the first generated token of each needle (10 % early,
50 % middle, 90 % late). Host CPU reference = the pinned llama.cpp on this Mac, CPU
backend, 8 threads (only the rounding order of the CPU's own attention differs from the
title's 2-thread split — tolerated by the same margins as every earlier rung). Metal
(host Metal rehearsal) is a stand-in only, always labeled. Tolerance = 3 × the Metal−CPU
difference on the same prompt (PORTING rule), set before each console run.

**Distinguish two questions.** (1) *GPU correctness against the same quantized model* is
what this file measures. (2) *Quality loss from quantization* (e.g. IQ2_XXS vs Q4_K_M) is
a model-quality question measured in [EVAL-QUALITY.md](EVAL-QUALITY.md) — passing (1)
says nothing about (2).

## Validation status by configuration

| Config | GPU vs CPU at depth | Basis |
|---|---|---|
| 9B 4K/8K/16K | **CPU-validated** (ladder: 0.367–1.009 ≤ tol, top-1 equal, tokens identical) | ladder runs 20261016-13xxxx |
| 9B 32K | **CPU-validated, all depths** (below) | this session |
| 9B 64K | **CPU-validated at 50 %/90 % depth** (needle_10 Metal-supplemented; below) | this session |
| 27B IQ2_XXS 4K/8K | **CPU-validated** (1.46–1.86 ≤ 4.9, overlap ≥ 7) | ladder |
| 27B IQ2_XXS 16K | Metal-validated in the ladder; revalidated this session (gate vs the 8K CPU reference: 32/32 identical, top-1 equal; needle_50 vs Metal: 23 tokens identical, max diff 2.19, overlap 8). **No 16K CPU long-prompt completion exists — partially validated, CPU at needle_50 only via Metal.** | run 20261016-191900 |
| 27B IQ2_XXS 32K | **log-probs within tolerance; token rule FAILS at 90 % depth** (first-token flip, CPU margin 1.87; below) | this session |
| 27B Q2_K_XL | 2K only: API tokens identical to CPU (ladder); no long-context claims | ladder |

## 27B IQ2_XXS 16K (window 20261016-191900, eboot `9c6d9f0a…`)

- Short gate (512-token prompt vs the 8K CPU reference): rule **identical** (all 32
  tokens), top-1 equal, 8/10 overlap, max top-10 log-prob diff 1.85, mean 0.73.
- needle_50 (16.2K prompt) vs the Metal stand-in: 23 tokens **identical**, top-1 equal,
  8/10 overlap, max diff 2.19 ≤ 4.9, mean 0.61, no nonfinite values. GPU top-2 margin
  0.63 vs CPU-side 1.30.
- Needles 10/50/90 all retrieved; longest real graph 1.053 s (chunk 128), longest fence
  wait 0.240 s; peak 9153 MiB; clean close, Vulkan freed.

## 9B 32K (window 20261016-193701) — CPU-validated at early, middle and late depth

GPU rows from the window (`--logits-all`, stored), CPU reference
`test-results/local/longctx-reference/m-qwen35-9b-c32k-cpu-full.json` (prompt ids verified
equal), Metal stand-in for the tolerance. **Tolerance 3.3, min-common 8** (3 × the
Metal−CPU envelope: Metal vs CPU max diff 0.377/0.998/0.615/1.069 across the four
positions, overlap ≥ 8).

| position | token rule | top-1 | overlap | GPU−CPU max / mean | CPU / GPU top-2 margin | nonfinite |
|---|---|---|---|---|---|---|
| depth0 (512) | identical | equal | 10/10 | 0.472 / 0.181 | 9.05 / 8.89 | 0 |
| needle 10 % | identical | equal | 10/10 | 1.565 / 0.501 | 8.15 / 9.18 | 0 |
| needle 50 % | identical | equal | 8/10 | 0.818 / 0.276 | 12.59 / 11.98 | 0 |
| needle 90 % (128 tokens) | identical | equal | 8/10 | 0.925 / 0.637 | 8.17 / 9.04 | 0 |

Every greedy continuation is token-identical to the CPU — including the full 128-token
ignore-EOS continuation at 90 % depth. The GPU−CPU differences sit inside the Metal−CPU
envelope × 3 at every position (the largest, 1.565 at 10 %, is 1.6 × that position's
Metal−CPU 0.998). Needles 10/50/90 all retrieved; probe lowered chunk 256→128; longest
graph 0.804 s, wait 0.187 s; pp 412 tok/s at 32.6K, tg 56.9 at depth; context 1665 MiB
as predicted, peak 7325 MiB; 97 requests, 0 errors, clean close, Vulkan freed.

The sections below were completed offline once the Mac CPU references finished (the
32K/64K prefills take hours); the GPU rows were captured in the windows above.

## Offload placement and CPU fallbacks

Every window: full offload confirmed in the server summary (9B 33/33, 27B 33/33 layers
incl. output; `splits_batch` 2 as always), no op fell back (the server runs with op
offload disabled; an unsupported op would fail the graph). No Vulkan errors, no
direct-memory denials, memory returned to zero after every close.

## Attention/recurrence anomalies

None observed: the qwen35 recurrence state does not grow with context (checked on the
host during the ladder); logit margins at depth track the shallower rungs; all per-position
diffs are finite and bounded by the same tolerances; needles retrieved at every depth.

## 9B 64K (window 20261016-195421) — CPU-validated at 50 %/90 % depth (needle_10 Metal-supplemented)

GPU rows `--logits-all` from the window, CPU reference `m-qwen35-9b-c64k-cpu-full.json`
(65.3K-token prefills at ~8 tok/s on the Mac), Metal stand-in for the tolerance.

| position | token rule | top-1 | overlap | GPU−CPU max / mean | nonfinite | tol (3× Metal) | pass |
|---|---|---|---|---|---|---|---|
| depth0_512 | identical@None | True | 10/10 | 0.4659 / 0.1857 | 0 | 1.2 | PASS |
| needle_50 | identical@None | True | 8/10 | 0.846 / 0.2677 | 0 | 1.8 | PASS |
| needle_90 | identical@None | True | 9/10 | 1.2933 / 0.8206 | 0 | 6.9 | PASS |

Needles 3/3 at 65.3K; chunk lowered to 64; graphs <= 0.432 s, waits <= 0.100 s;
pp 279.5 tok/s at 65.3K, tg 43.8-49 at depth; clean close.

## 27B IQ2_XXS 32K (window 20261016-204333) — DEVIATIONS, see rows

Gate vs the 8K CPU reference: identical 32/32 tokens, top-1 equal, max diff 1.86 <= 4.9.
GPU rows vs the CPU reference (needle_50 and needle_90 completions; needle_10 CPU was not
run — pair-2 budget — and stays Metal-supplemented), Metal stand-in for the tolerance.

| position | token rule | top-1 | overlap | GPU−CPU max / mean | nonfinite | tol (3× Metal) | pass |
|---|---|---|---|---|---|---|---|
| depth0_512 | identical@None | True | 7/10 | 1.8587 / 0.5768 | 0 | 4.7 | PASS |
| needle_50 | identical@None | True | 6/10 | 4.1523 / 2.1766 | 0 | 13.4 | PASS |
| needle_90 | fail@0 | False | 7/10 | 2.4036 / 1.1207 | 0 | 5.7 | FAIL |

Needles 3/3; chunk 64; graphs <= 0.701 s, waits <= 0.245 s; peak 10237 MiB of 10624;
pp 119.2 tok/s at 32.6K, tg 25-26.5 at depth; clean close.

### The 27B 32K needle_90 divergence, in full

Position 0 of the 90 % needle (the deepest tested), top-2 log-probabilities:

| backend | token A | token B | margin |
|---|---|---|---|
| CPU | `41` at −0.168 | `760` at −2.034 | 1.866 |
| Metal (another GPU) | `41` at −0.364 | `760` at −1.259 | 0.895 |
| PS5 GPU | `760` at −0.648 | `41` at −0.773 | 0.125 |

All three backends put the same two tokens on top; they disagree on their order. The CPU
continues with the passcode token itself, the PS5 with `760` ("The secret passcode…");
both continuations retrieve the needle (coherent — which is exactly why the rule does not
accept coherence as evidence). The absolute disagreements (GPU−CPU max 2.40 over the CPU
top-10) sit inside the Metal−CPU envelope at this rung (tolerance 5.7), and the same
order-flip risk is visible on Metal (margin 0.90 vs the CPU's 1.87). Reading: at 32K depth
the 2.1-bit model's top-2 separation collapses (CPU 1.87 → 0.13 on the PS5), so backend
rounding flips the argmax. **GPU-vs-CPU on this quantized model at this depth is
borderline: within log-prob tolerance everywhere, token-exact at 0 %/16K and at 32K 50 %,
token-divergent at 32K 90 %.** This is distinct from quantization quality itself (the
collapse exists on the CPU too, vs the F16 original we never measured — see
EVAL-QUALITY.md). The 9B Q4_K_M never showed this at any depth up to 64K.
