# Qwen3.8 27B UD-IQ2_XXS at 64K with Q8_0 KV: offline feasibility analysis

Status: **analysis only; no 64K 27B run was made.** Written 2026-10-16 evening, after the
32K F16 rung passed (peak 10172 MiB of the 10624 MiB device-memory ceiling,
`test-results/ps5/PPSA99041/20261016-171917/`). This document is the bounded proposal the
evaluation brief asked for; launching it needs the user's go.

## 1. Why Q8_0 KV

F16 KV at 64K does not fit (documented in PORTING.md "Long context": expected peak
12286 MiB > 10624 MiB). Q8_0 halves the KV bytes with the smallest quality step of the
quantized options; Q4_0 is the fallback only if Q8_0's arithmetic fails.

## 2. Exact sizes from the architecture and ggml block layout

27B `qwen35` attention: 16 of 64 layers have a KV cache, 4 KV heads, head size 256, K and V
both cached. Per token: 16 × 4 × 256 × 2 = 32768 elements. ggml Q8_0 stores 32 elements per
block as 2 bytes of scale + 32 bytes of data = 34 bytes per 32 elements (block overhead
included); F16 stores 2 bytes per element.

| Quantity | F16 @32K | F16 @64K | Q8_0 @32K | Q8_0 @64K |
|---|---|---|---|---|
| KV bytes/token | 65536 B (64 KiB) | 65536 B | 34816 B (34 KiB) | 34816 B |
| KV total | 2048 MiB | 4096 MiB | 1088 MiB | 2176 MiB |
| Expected context (server) | 2971 MiB (measured) | 5084 MiB | ~2211 MiB | ~3164 MiB (cap 3456) |
| Expected peak | 10172 MiB (measured) | 12286 MiB — **does not fit** | ~9418 MiB | **10366 MiB** (cap 10624) |
| Margin to the 10624 cap | 452 MiB | −1662 MiB | 1206 MiB | **258 MiB** |
| RADV device memory left (of ~11.4 GiB usable) | ~1.44 GiB | −0.7 GiB | ~2.2 GiB | **~1.25 GiB** |

Context model: measured 32K context 2971 MiB, swap the KV term 2048 → 2176 MiB, add the
per-token extras (2 KiB/token × 32768 extra tokens ≈ 64 MiB) → 3163 ≈ 3164 MiB; peak
likewise 10172 + 194 ≈ 10366. The graph/scheduler scratch grows with `n_kv` (attention mask
`n_kv × ubatch` F16, counted on device and host): at ubatch 512 that is 64 MiB at 64K —
already inside the 2 KiB/token term; the compute buffer grows ~70 MiB from 32K to 64K
(569 → ~640 MiB, from the CPU measurements in PORTING.md). Keeping ≥1 GiB of device memory
free holds: 11616 − 10366 ≈ 1.25 GiB. The margin to the cap (258 MiB) is the tightest of
any passing run so far (32K F16 had 452 MiB) but of the same order.

## 3. Does the quantized-KV Flash Attention actually run on our GPU? (pinned source inspection)

All references are `third_party/llama.cpp` @ `552f18f`, `ggml/src/ggml-vulkan/`:

1. **`FLASH_ATTN_EXT` support** (`ggml_backend_vk_device_supports_op`): K/V types Q8_0 are
   accepted (`fa_kv_ok`); requirements: head sizes % 8 == 0 (256 ✓), query F32 (✓ with FA),
   and for the non-coopmat2 path `subgroup_shuffle && subgroup_vote` — both already in use
   on this device, since today's F16 FA runs the same scalar path. No CPU fallback exists in
   the title (the server runs `--no-op-offload`; an unsupported op would fail the graph, not
   silently move).
2. **Shader path**: the scalar FA shader (`flash_attn.comp` + `flash_attn_dequant.glsl`)
   dequantizes Q8_0 in-shader (`FA_DEQUANT4_Q8_0`, `block_q8_0_packed16` SSBO views,
   spec-constant-specialized so unused type paths fold away). There is **no separate
   dequantized copy** of K or V in device memory — the 2176 MiB KV is the only copy. The
   integer-dot "mmq" fast path needs `integer_dot_product` hardware we do not expose, so
   the scalar shader's plain F32 arithmetic after dequant is what runs — the same shader
   family our scoped-FP32 attention patch (`patches/ggml-vulkan/0004`) covers.
3. **KV write**: F32 → Q8_0 cache writes use the `cpy_f32_q8_0` compute pipeline (GPU);
   reads back through `cpy_quant_f32` where the graph needs F32.
4. **Hadamard rotation**: with quantized K (and V), llama.cpp enables `attn_rot_k/v`
   (head 256 % 64 == 0): each new KV cell is dequantized, rotated by a fixed Hadamard
   matrix (a small `mul_mat`), roped, rotated back and re-quantized — all GPU ops already
   exercised elsewhere. The Hadamard matrices are host-resident (256×256 each, ~512 KiB);
   the per-token FLOP cost is ~2 × 2 × 256 × 256 × 4 heads × 16 layers ≈ 16 MFLOP/token of
   prefill — under 0.1 % of the model's per-token cost, but it does add graph nodes and
   submit pressure at 65K-token prefills.
5. **Scheduler/watchdog interplay**: unchanged — the same 2 s fence bound and ~10 s
   device-lost watchdog apply; the risk is the longer graphs below, not new failure modes.

## 4. Performance estimate (not a measurement)

- The 32K F16 rung needed prompt chunk 64 (client lowered from 128) with graphs ≤ 0.697 s.
  At 64K the attention work per chunk doubles: chunk 64 F16 would predict ~1.4 s graphs —
  under the 1.8 s abort but over the 1.6 s target. In-shader Q8_0 dequant adds an estimated
  20–50 % to the attention portion (fewer bytes streamed, more ALU per element; direction
  plausible, magnitude unmeasured).
- **Expected outcome: chunk 64, graphs ~1.4–2.0 s** — the rung may legitimately abort on
  its own wait/graph rules. If it does, that is a "does not fit our latency rules" result,
  not a memory failure.
- Prefill throughput ≈ 90–119 tok/s → 9–12 min per 65K prefill; probe + 3 needles +
  comprehension cases ≈ 45–70 min. The existing `c64k-kvq8` entry's window (2400 s) is too
  small for a full window; use ≥ 4200 s.

## 5. Proposed bounded experiment (for approval; NOT run here)

Entry `qwen38-27b-iq2xxs-c64k-kvq8` with two documented corrections before its first run:
prompt chunk **64** (not 256) and window **4200 s**. Sequence, one window:

1. **Short-context gate first**: the 512-token benchmark prompt against a host CPU
   reference built with the same Q8_0 KV (`--gate-depth0`); a failure ends the rung with no
   long request. This is where quantization-KV correctness is decided — GPU-vs-CPU on the
   *same quantized cache*, before any depth.
2. Half-length probe (32K, chunk 64): longest wait/graph measured; the existing prediction
   logic lowers the chunk or stops.
3. Only if both pass: needles at 10/50/90 %, `--logits-all`, compared against a CPU Q8_0-KV
   reference (needle_50 at minimum; the 27B CPU cost is ~2.9 h per 32K needle, ~5.5 h per
   64K needle — accept needle_50 only and label the rest Metal-supplemented if needed).
4. Memory ledger watched live (`/ps5lm/stats` direct peak, denials) with the same
   1.5 s wait / 1.8 s graph aborts; any denial or unexplained peak ends the run.

**Stop conditions** (beyond the standard ones): first direct-memory denial; predicted
context above 3400 MiB or peak above 10500 MiB at the half probe; the short-context Q8_0
gate failing the token rule; a measured wait above 1.5 s. On any of these, the 27B stays a
32K model and this document is the record.

**Explicitly not proposed**: Q4_0 KV, any new Vulkan patch, or raising the 10624 MiB cap.
