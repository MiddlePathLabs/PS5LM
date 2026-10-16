# Models

Models worth running on a PS5, from what is popular on Ollama in October 2026, with the GGUF sizes on Hugging Face. Every architecture below loads in the pinned llama.cpp (`b11327`).

The console has 16 GB shared by everything. How much one PS5LM process can hold is what `probes/memprobe` measures. Until then: ProsperoAI already keeps a 6.37 GiB model resident on this console, so up to about 6 GiB is safe, and anything above 8 GiB waits for the probe.

## Ran on this console's GPU (PS5 Pro, native title, fully resident)

Correct against the host CPU reference and through the LAN API; numbers are
llama-server's medians for a 128-token prompt and 128 generated tokens
([CONSOLE.md](CONSOLE.md), pins in [PORTING.md](PORTING.md)).

| Model | Quant | File | Architecture | Budget | Prompt / generation tok/s |
|---|---|---|---|---|---|
| Qwen3.5 0.8B | Q4_K_M | 0.50 GiB | qwen35 (dense) | 1536 MiB | 1744 / 214 |
| Granite 3.1 1B-A400M | Q4_K_M | 0.77 GiB | granitemoe | 1536 MiB | 1370 / 212 |
| Granite 3.1 3B-A800M | Q4_K_M | 1.88 GiB | granitemoe | 3072 MiB | 711 / 150 |
| OLMoE 1B-7B 0125 | Q4_K_M | 3.92 GiB | olmoe | 6144 MiB | 615 / 232 |

Vision (model mode, title clock; [PORTING.md](PORTING.md) "Vision"): Qwen3-VL
4B Instruct Q4_K_M (2.33 GiB, `qwen3vl`) + its F16 mmproj (0.78 GiB) in a
6144 MiB budget: one 448 x 448 image (196 tokens) encoded in 231 ms,
embedding within 0.8 % (relative L2) of the CPU's, text identical to the CPU,
114.8 tok/s generation; the mmproj loads in 0.47 s (`read(2)`, patch 0005). Through the API (server mode): one image request's
prompt (encode + 224 tokens) in 397 ms, 113 tok/s generation; text 913 / 1171
tok/s prompt (128 / 512 tokens), 116 / 112 tok/s generation. Qwen3-VL 8B
Instruct Q4_K_M (4.68 GiB) + F16 mmproj (1.08 GiB) in an 8192 MiB budget:
encode 291 ms, embedding within 0.6 % of the CPU's, text identical, 78.7 tok/s;
through the API 624 ms image prompt, 79 tok/s, text 517 / 624 tok/s prompt.

Qwen3-30B-A3B-Instruct-2507 IQ2_M (9.19 GiB, `qwen3moe`) loaded and ran fully
resident in a 10560 MiB budget (peak 9723 MiB, 37 tok/s on the title's clock)
but failed the CPU-comparison rule at token 7; see [CONSOLE.md](CONSOLE.md).

## Measured on this console

Every model other PS5 ports report, run on this build the same way: the model mode against
the host CPU reference (pass rule in [PORTING.md](PORTING.md)), then `scripts/ps5-api-bench.py`
in server mode (llama-server's medians, 128 generated tokens; prompt tok/s at 128 / 512-token
prompts). Load time and peak direct memory are from the server run. Their numbers come from
other consoles (mostly a base PS5 or a Slim) and other builds: they stand beside ours, not as
ours. Evidence under `test-results/ps5/`.

| Model | File | Pass / fail | Load | Peak | Prompt tok/s (128 / 512) | Generation tok/s | Evidence | Their number (source) |
|---|---|---|---|---|---|---|---|---|
| Qwen3.5 0.8B | Q4_K_M 0.50 GiB | pass | 1.3 s | 1410 MiB | 3127 / 5255 | 252.2 | `PPSA99041/20261016-090943/`, `api-bench/20261016-091017/` | 207 tok/s (upstream v0.9.0, Slim) |
| Qwen3.5 9B | Q4_K_M 5.29 GiB (`unsloth`) | pass (32/32 identical) | 4.6 s | 6277 MiB | 514 / 609 | 67.6 | `PPSA99041/20261016-094546/` (model), `20261016-100128/` (server), `api-bench/20261016-100138/` | 55 tok/s (upstream v0.9.0, Slim) |
| Qwen3.8 27B | UD-IQ2_XXS 6.77 GiB (`unsloth`) | pass by the rule (20 identical, then a 0.15 tie) | 6.6 s | 8194 MiB | 178 / 200 | 31.0 | `PPSA99041/20261016-102525/` (model), `20261016-102708/` (server), `api-bench/20261016-102731/` | 20.7 tok/s at 64k context (abhi fork, Slim); upstream ~20 for UD-Q2_K_XL |
| Llama 3.2 3B Instruct | Q4_K_M 1.88 GiB (`bartowski`) | pass (32/32 identical) | 2.3 s | 2751 MiB | 1152 / 1555 | 139.4 | `PPSA99041/20261016-103013/` (model), `20261016-103154/` (server), `api-bench/20261016-103210/` | 124 tok/s at 64k context (abhi fork, Slim 13.60) |
| Mistral 7B Instruct v0.3 | Q4_0 3.83 GiB (`QuantFactory`, their file) | pass (32/32 identical, server only) | 3.4 s | 4492 MiB | 569 / 737 | 90.8 | `PPSA99041/20261016-110045/`, `api-bench/20261016-110107/` | 72.5 tok/s (ProsperoAI, base PS5 FW 6.02) |
| MiniCPM5 2B | Q8_0 2.50 GiB (`openbmb`) | pass by the rule (20 identical, then a 0.31 tie; server only) | 2.6 s | 2938 MiB | 1408 / 2214 | 120.3 | `PPSA99041/20261016-110208/`, `api-bench/20261016-110223/` | 109.1 tok/s (ProsperoAI, base PS5) |
| Granite 4.0 H-Tiny | Q4_K_M 3.94 GiB (`ibm-granite`) | pass (32/32 identical, server only) | 3.9 s | 4594 MiB | 1025 / 1901 | 141.4 | `PPSA99041/20261016-110300/`, `api-bench/20261016-110319/` | 105.0 tok/s (ProsperoAI, base PS5) |
<!-- measured-rows -->

## Fits for sure (up to 6 GiB)

| Model | Ollama name | Quant | Size | Architecture | Notes |
|---|---|---|---|---|---|
| Granite 4.2 3B | `granite4.2:3b` | Q4_K_M | 2.09 GiB | granite | the smoke test model: small and fast |
| Nemotron 3 Nano 4B | | Q4_K_M | 2.64 GiB | nemotron_h | hybrid Mamba, 1M context |
| Gemma 4 E4B | | Q4_0 | 4.28 GiB | gemma4 | |
| Granite 4.2 8B | `granite4.2:8b` | Q4_K_M | 4.98 GiB | granite | |
| Qwen3.5 9B | | Q4_K_M | 5.29 GiB | qwen35 | the model ProsperoAI runs: a direct comparison |
| Ornith 1.5 9B | `ornith-1.5:9b` | Q4_K_M | 5.38 GiB | qwen35 | among Ollama's most pulled |
| Gemma 4 12B | | Q3_K_M / IQ4_XS | 5.30 / 5.94 GiB | gemma4 | |

## Fits if the probe allows (6 to 11 GiB)

| Model | Ollama name | Quant | Size | Architecture | Notes |
|---|---|---|---|---|---|
| **Qwen 3.8 27B** | `qwen3.8` | UD-IQ2_XXS / UD-Q2_K_XL | 6.77 / 9.15 GiB | qwen35 | the headline target |
| Muse Glimmer 30B | `muse-glimmer` | IQ2_XXS | 8.31 GiB | muse-glimmer | |
| Qwen 3.6 27B | `qwen3.6:27b` | UD-IQ2_XXS | 8.74 GiB | qwen35 | Ollama's most pulled model |
| Qwen 3.6 35B-A3B | `qwen3.6:35b` | UD-IQ2_XXS | 10.02 GiB | qwen35moe | MoE, 3B active |
| gpt-oss 20B | `gpt-oss:20b` | MXFP4 | 10.7 GiB | gpt-oss | MoE, 3.6B active; every quant is about this size |

## Does not fit

- Nemotron 3.5 Lightning 30B-A3B: the smallest GGUF is 18 GiB.
- Ornith 1.5 35B-A3B: only a 20 GiB Q4_K_M is published.
- Qwen 3.8 Flash-Next (180B), and the cloud-only models (MiniMax M3, GLM 5.x, DeepSeek V4).

## Speed

Decoding reads the active weights once per token, so tokens per second ≈ memory bandwidth / active bytes per token. A dense 27B at 2 to 3 bits reads 7 to 9 GiB per token; a MoE with 3B active reads around 1 GiB. On the CPU the MoE models (Qwen 3.6 35B-A3B, gpt-oss 20B) should be several times faster than the dense 27Bs, at a similar quality. Real numbers come with `llama-bench` on the console.
