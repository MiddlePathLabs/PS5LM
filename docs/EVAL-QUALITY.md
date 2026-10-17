# Model quality and long-context comprehension on the PS5 (2026-10-16 evening)

Part of the long-context evaluation (Phases B–D); the numerical GPU-vs-CPU validation is in
[EVAL-NUMERICAL.md](EVAL-NUMERICAL.md), the 64K Q8_0 KV study in
[EVAL-64K-FEASIBILITY.md](EVAL-64K-FEASIBILITY.md).

**Question asked:** does Qwen3.8 27B, heavily quantized (UD-IQ2_XXS 6.8 GiB or UD-Q2_K_XL
9.2 GiB), produce meaningfully better answers than Qwen3.5 9B Q4_K_M, and is the long
context practically useful?

## 1. Method

- **Phase C (2K, all three models):** 48 fixed tasks — 12 coding (the model's final
  ```python block executed against pinned asserts), 12 reasoning, 8 instruction-following,
  8 structured output, 8 grounded document QA. `testdata/quality-tasks.json`,
  `scripts/ps5-quality-bench.py`. Temperature 0, each model's native chat template with
  thinking disabled (the repo's established nothink prompt), identical fixed output limits
  (coding 420 tokens, reasoning 350, the rest 120–250). Same tasks, same graders, no LLM
  judge. Replies preserved per task in the result JSONs.
- **Phase B (long context):** 9 deterministic comprehension cases per length
  (`testdata/lc-comprehension/corpus.json`, `scripts/ps5-lc-fixtures.py` +
  `scripts/ps5-lc-comprehension.py`): a three-fact sum across 15 %/50 %/85 % depths, a
  revision-supersedes contradiction, a two-step cross-reference, a three-document
  synthesis requiring citations, a "not enough information" refusal, near-identical
  identifiers (MERIDIAN-7 vs -7B), one entity with two values (winter/summer), and the
  same fact placed early vs late (position sensitivity). ~16K/32K/64K input tokens
  (n_ctx minus 320), 128-token answers, temperature 0, deterministic graders with exact
  reference answers and citation requirements.
- Windows used the `-q` entries (validated rung caps, longer serving windows; see
  PORTING.md "Evaluation builds"). Every window closed cleanly; per-request wall times
  and title counters were recorded for the stall watch (Phase D, section 5).

## 2. Phase C: 2K quality (the common comparison)

| | Qwen3.5 9B Q4_K_M | 27B UD-IQ2_XXS | 27B UD-Q2_K_XL |
|---|---|---|---|
| Tasks passed | **44/48 (91.7 %)** | 43/48 (89.6 %) | **45/48 (93.8 %)** |
| Coding | 12/12 | 10/12 | 12/12 |
| Reasoning | 8/12 | 9/12 | 9/12 |
| Instruction | 8/8 | 8/8 | 8/8 |
| Structured (JSON) | 8/8 | 8/8 | 8/8 |
| Grounded | 8/8 | 8/8 | 8/8 |
| Invalid JSON | 0 | 0 | 0 |
| Median wall / p95 | 1.61 s / 5.33 s | 3.32 s / 11.7 s | 3.39 s / 12.1 s |
| Generation tok/s (median) | 67.5 | 31.1 | 29.9 |
| Peak memory | 6267 MiB | 8159 MiB | 10268 MiB (cap 10624) |
| Time for the whole suite | 100 s | 220 s | 223 s |

Evidence: `test-results/ps5/quality/20261016-184256` (9B), `…-185242` (IQ2_XXS),
`…-185704` (Q2_K_XL); windows `PPSA99041/20261016-183504`, `-184837`, `-185255`.

**Reading the failures.** 10 of the 11 reasoning misses across the three models are
350-token-budget truncations: the model writes a long derivation and never reaches the
required `Answer:` line (predicted_n = 350/350). Counting "expected value appears anywhere
in the reply" as a secondary metric: 9B res-06/res-11 and Q2_K_XL res-05 contain the right
value; res-02 (date arithmetic) and res-10 (dice probability) were unfinished in all
models that missed them. The genuine solution errors are: IQ2_XXS code-01 (answered in
prose, no code block — a formatting failure at 2.1-bit weights) and code-04 (wrong
`roman()` for subtractive forms); res-11 (sequence rule) derailed all three models into
self-correction loops (9B and Q2_K_XL recovered the value, IQ2_XXS did not).

**Paired:** no task was failed by the 9B alone; IQ2_XXS alone failed code-01/code-04;
Q2_K_XL alone failed res-05 (truncation). res-11 failed everywhere. Differences of ±2
tasks at n=48 are within noise (binomial 95 % CI at 44/48 is roughly ±7 percentage
points); the three models are statistically indistinguishable on this suite.

## 3. Phase B: comprehension by context length

| Model | 16K | 32K | 64K |
|---|---|---|---|
| Qwen3.5 9B Q4_K_M | 9/9 | 8/9 | 8/9 |
| 27B UD-IQ2_XXS | 9/9 | 8/9 | not attempted (memory) |
| 27B UD-Q2_K_XL | not attempted (2 GiB more for no measured gain) | — | — |

Evidence: `test-results/ps5/lc-comprehension/20261016-190808` (9B 16K), `…-193536`
(27B 16K), `…-195401` (9B 32K), `…-204303` (9B 64K), `…-214112` (27B 32K).

At 16K both models solved every case: the three-depth sum, the revision-supersedes
question (4.2 bar, not the superseded 4.8), the two-step cross-reference through
CFR-T-0189, the three-siding synthesis with all three citations, a clean refusal with no
invented warranty, the near-identical identifier, the conditioned value (summer 58 %, not
winter 71 %), and both position probes (TOL-7749 at 15 %, TOL-3316 at 85 %).

At 32K each model missed exactly one case, and not the same one: the 9B answered the
three-transformer sum correctly (1335 kVA) but cited only 1 of the 3 required documents
(all three at 16K) — a citation-completeness loss at depth; the 27B spent its 128-token
answer budget quoting documents and never reached the required Answer line (same
truncation mode as its 2K reasoning misses). At 64K the 9B again missed one case, a
different one again: the siding synthesis quoted all three documents but totalled 30
instead of 40 wagons, while the three-depth sum passed *with* all citations. Position
probes and refusals never failed at any length on either model.

**Reading:** comprehension degradation from 16K to 64K is real but small and non-monotonic
case-by-case (one case per length, different each time; n = 1 per cell). Nothing suggests
catastrophic loss-of-context; the reliable degradation is in cost: 9B prompt throughput
422 → 412 → 280 tok/s and generation at depth 60 → 57 → 49 tok/s from 16K to 64K, wall
time per question 39 s → 80 s → 235 s.

## 4. Latency and memory (windows as run)

| Window | pp at depth (tok/s) | tg at depth (tok/s) | wall per question | peak MiB (cap) |
|---|---|---|---|---|
| 9B 2K suite | — | 67.5 (median) | 1.6 s median | 6267 (7680) |
| 27B XXS 2K suite | — | 31.1 | 3.3 s | 8159 (9216) |
| 27B XL 2K suite | — | 29.9 | 3.4 s | 10268 (10624) |
| 9B 16K | 422 | 60 | 39 s | 6771 (7296) |
| 9B 32K | 412 | 57 | 80 s | 7325 (7808) |
| 9B 64K | 280 | 49 | 235 s | see window record (9088 cap) |
| 27B XXS 16K | 149 | 28 | 110 s | 9153 (9728) |
| 27B XXS 32K | 119 | 26 | 274 s | 10237 (10624) |

## 5. Phase D: server stability

Eight windows, 1 269 requests, 0 errors; every window ended by `shutdown_request`,
1.48–1.50 s from stop to freed, Vulkan memory fully released every time
(`vk_live_after_free` 0). No request exceeded 10× its run's median wall. The one
historical 9B stall (2026-10-16 09:47, one request hung ~100 s, then a clean
147-response run; cause never identified) **did not recur** in 1 423 monitored requests
across every model and depth, including two 60+-request depth windows on the 9B itself.
One incident this session was self-inflicted and fixed (a window collision: run 2's bench
reached run 1's still-open server because the shutdown probe raced the wrapper lock —
quarantined, `ps5-eval-window.sh` now forbids it).

## 6. Decision

1. **Is the 9B better than the 27B IQ2_XXS for our use?** At 2K they are statistically
   tied on quality (44 vs 43 of 48; the difference is far inside noise), and the 27B's
   two coding misses (prose instead of code, wrong roman numerals) are exactly the
   failure mode expected of 2.1-bit weights. The 9B is 2.2× faster, uses 1.9 GiB less,
   and reaches 64K. **Yes.**
2. **Does Q2_K_XL justify its ~2 GiB?** It is the only configuration with a measurable
   edge (45/48; coding 12/12 vs the XXS's 10/12) at unchanged speed. But it runs at the
   device-memory ceiling (10268 of 10624 MiB, 356 MiB margin), is confined to 2K, and
   its reasoning misses are the same truncations as everyone else's. As a default, no;
   as a coding-accuracy option at short context, defensible.
3. **Most reliable for coding / reasoning / instruction following:** the 9B and Q2_K_XL
   tie on coding (12/12); all three tie on instruction following, structured output and
   grounded QA (perfect); reasoning is truncation-dominated for all. The 9B is the
   all-rounder, and it is also the fastest and cheapest.
4. **Does comprehension deteriorate with context?** Mildly and unevenly: 9/9 → 8/9 → 8/9
   for the 9B with a *different* case failing at each length (citation completeness at
   32K, one value at 64K); the 27B went 9/9 → 8/9 on a truncation, not comprehension.
   Retrieval at 15 %/85 % and refusal never failed. The decisive cost of depth is time
   and memory, not correctness.
5. **Best practical configuration:** **Qwen3.5 9B Q4_K_M, 32K context** as the serving
   default (57 tok/s at depth, 7.3 GiB peak, all comprehension but one case); 16K
   (60 tok/s, 6.8 GiB) when margin matters. 64K is validated and works (49 tok/s at
   depth, ~4 min prefill per question) — keep it for retrieval-style work, not
   conversation.
6. **Is 27B at 64K worth investigating with quantized KV?** The arithmetic fits
   (Q8_0 KV: 10366 MiB predicted of 10624, 258 MiB margin; the Vulkan path runs on-GPU —
   see EVAL-64K-FEASIBILITY.md), but the 27B buys no measured quality over the 9B at any
   tested length while costing 2× latency and the entire memory margin. Not recommended
   as a default; the bounded experiment is written up if wanted anyway.

**Recommendation.** Default model: **Qwen3.5 9B Q4_K_M**. Quality-oriented alternative:
**Qwen3.8 27B UD-Q2_K_XL at 2K** (coding accuracy where the memory is affordable). Largest
context with meaningful validation: **64K on the 9B** — needles 3/3 at 65.3K, comprehension
8/9, bounded waits, and token-exact CPU agreement at 50 %/90 % depth (see
EVAL-NUMERICAL.md; the 27B is token-exact to 16K and log-prob-tolerant but not
token-exact at 32K/90 %).

