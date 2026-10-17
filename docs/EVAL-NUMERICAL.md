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
| 9B 64K | (below) | this session |
| 27B IQ2_XXS 4K/8K | **CPU-validated** (1.46–1.86 ≤ 4.9, overlap ≥ 7) | ladder |
| 27B IQ2_XXS 16K | Metal-validated in the ladder; revalidated this session (gate vs the 8K CPU reference: 32/32 identical, top-1 equal; needle_50 vs Metal: 23 tokens identical, max diff 2.19, overlap 8). **No 16K CPU long-prompt completion exists — partially validated, CPU at needle_50 only via Metal.** | run 20261016-191900 |
| 27B IQ2_XXS 32K | (below) | this session |
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

## Offline comparisons (CPU reference vs the stored GPU rows)

(filled when the CPU references land: 9B 32K, 9B 64K, 27B 32K; per needle: tokens,
first divergence, top-1, top-2 margins, overlap, max/mean/relative diffs, nonfinite
count; tolerances from the Metal stand-ins)

## Offload placement and CPU fallbacks

Every window: full offload confirmed in the server summary (9B 33/33, 27B 33/33 layers
incl. output; `splits_batch` 2 as always), no op fell back (the server runs with op
offload disabled; an unsupported op would fail the graph). No Vulkan errors, no
direct-memory denials, memory returned to zero after every close.

## Attention/recurrence anomalies

None observed: the qwen35 recurrence state does not grow with context (checked on the
host during the ladder); logit margins at depth track the shallower rungs; all per-position
diffs are finite and bounded by the same tolerances; needles retrieved at every depth.
