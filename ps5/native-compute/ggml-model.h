/* SPDX-License-Identifier: GPL-3.0-or-later
 * Fixed parameters of the first resident dense-model run. Shared by the native
 * title (GPU) and the host CPU reference, so both see the same prompt and limits.
 */
#ifndef PS5LM_GGML_MODEL_H
#define PS5LM_GGML_MODEL_H
#if defined(PS5LM_MODEL_QWEN3MOE)
/* Step 3 of the MoE climb (build modes "qwen3moe", "server-qwen3moe"; docs/PORTING.md
 * "Larger MoE models"). bartowski/Qwen_Qwen3-30B-A3B-Instruct-2507-GGUF
 * @ 6c6e8692f43e4ca663f7ece8229a1361090d3a4c */
#define PS5LM_GGML_WORKLOAD "model-qwen3moe-greedy"
#define PS5LM_MODEL_FILE "Qwen_Qwen3-30B-A3B-Instruct-2507-IQ2_M.gguf"
#define PS5LM_MODEL_BYTES 9870270464ull
/* Qwen chat format; the 2507 Instruct model does not think. */
#define PS5LM_MODEL_PROMPT "<|im_start|>user\nIn a short paragraph, explain how a GPU differs " \
    "from a CPU.<|im_end|>\n<|im_start|>assistant\n"
#define PS5LM_MODEL_WEIGHTS_MIB 9728u
#define PS5LM_MODEL_CONTEXT_MIB 768u
#define PS5LM_MODEL_TOTAL_MIB 10560u
/* About one queue submission per MiB uploaded, plus decoding. */
#define PS5LM_MODEL_SUBMIT_LIMIT 24576u
#define PS5LM_MODEL_WATCHDOG_S 480u
#define PS5LM_MODEL_SERVER_LOAD_S 300u
#elif defined(PS5LM_MODEL_OLMOE)
/* Step 2 of the MoE climb (build modes "olmoe", "server-olmoe").
 * allenai/OLMoE-1B-7B-0125-Instruct-GGUF @ 2ac1d27317927518c9ef7bd99f91f3f1e4ee288d */
#define PS5LM_GGML_WORKLOAD "model-olmoe-greedy"
#define PS5LM_MODEL_FILE "OLMoE-1B-7B-0125-Instruct-Q4_K_M.gguf"
#define PS5LM_MODEL_BYTES 4213512192ull
/* OLMoE's chat template: BOS (<|endoftext|>, not added by the tokenizer), user turn. */
#define PS5LM_MODEL_PROMPT "<|endoftext|><|user|>\nIn a short paragraph, explain how a GPU differs " \
    "from a CPU.\n<|assistant|>\n"
#if defined(PS5LM_STREAM_PROBE)
/* The streaming probe loads no model: ~140 MiB of heap, capped at 1024 MiB. */
#undef PS5LM_GGML_WORKLOAD
#define PS5LM_GGML_WORKLOAD "stream-probe"
#define PS5LM_MODEL_TOTAL_MIB 1024u
#endif
#define PS5LM_MODEL_WEIGHTS_MIB 4608u
/* 512 until the performance work: the server's context for 512-token physical
 * batches needs about 60 MiB more (docs/PORTING.md "Performance"). */
#define PS5LM_MODEL_CONTEXT_MIB 768u
#ifndef PS5LM_MODEL_TOTAL_MIB
#define PS5LM_MODEL_TOTAL_MIB 6144u
#endif
#define PS5LM_MODEL_SUBMIT_LIMIT 16384u
#define PS5LM_MODEL_WATCHDOG_S 300u
#define PS5LM_MODEL_SERVER_LOAD_S 180u
#elif defined(PS5LM_MODEL_GRANITE3B)
/* Step 1 of the MoE climb (build modes "moe3b", "server-moe3b").
 * bartowski/granite-3.1-3b-a800m-instruct-GGUF @ be9a36f042806cb586bc65556c527079782b78e0 */
#define PS5LM_GGML_WORKLOAD "model-moe3b-greedy"
#define PS5LM_MODEL_FILE "granite-3.1-3b-a800m-instruct-Q4_K_M.gguf"
#define PS5LM_MODEL_BYTES 2016888384ull
#define PS5LM_MODEL_PROMPT "<|start_of_role|>user<|end_of_role|>In a short paragraph, explain how " \
    "a GPU differs from a CPU.<|end_of_text|>\n<|start_of_role|>assistant<|end_of_role|>"
#define PS5LM_MODEL_WEIGHTS_MIB 2304u
#define PS5LM_MODEL_CONTEXT_MIB 384u
#define PS5LM_MODEL_TOTAL_MIB 3072u
#define PS5LM_MODEL_SUBMIT_LIMIT 12288u
#define PS5LM_MODEL_WATCHDOG_S 300u
#define PS5LM_MODEL_SERVER_LOAD_S 120u
#elif defined(PS5LM_MODEL_MOE)
/* Small resident MoE (build mode "moe", docs/PORTING.md "Small MoE").
 * bartowski/granite-3.1-1b-a400m-instruct-GGUF @ 940d2e1f9f65330615c7c8e980e6c5ac73d3360c */
#define PS5LM_GGML_WORKLOAD "model-moe-greedy"
#define PS5LM_MODEL_FILE "granite-3.1-1b-a400m-instruct-Q4_K_M.gguf"
#define PS5LM_MODEL_BYTES 821847360ull
/* Granite 3.x chat format, user turn only, tokenized with special tokens. */
#define PS5LM_MODEL_PROMPT "<|start_of_role|>user<|end_of_role|>In a short paragraph, explain how " \
    "a GPU differs from a CPU.<|end_of_text|>\n<|start_of_role|>assistant<|end_of_role|>"
#else
#define PS5LM_GGML_WORKLOAD "model-dense-greedy"
/* unsloth/Qwen3.5-0.8B-GGUF @ 6ab461498e2023f6e3c1baea90a8f0fe38ab64d0 */
#define PS5LM_MODEL_FILE "Qwen3.5-0.8B-Q4_K_M.gguf"
#define PS5LM_MODEL_BYTES 532517120ull
/* Qwen chat format with thinking switched off, tokenized with special tokens. */
#define PS5LM_MODEL_PROMPT "<|im_start|>user\nIn a short paragraph, explain how a GPU differs " \
    "from a CPU.<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n"
#endif
#define PS5LM_MODEL_PATH "/data/PS5LM/models/" PS5LM_MODEL_FILE
#define PS5LM_MODEL_PREDICT 32u
#define PS5LM_MODEL_CTX 1024u
#define PS5LM_MODEL_BATCH 32u
#define PS5LM_MODEL_THREADS 2
/* Budget, MiB. Weights = everything the model load adds (resident buffers plus
 * any loader read or staging buffer the heap keeps); context = KV cache,
 * recurrent state, outputs and compute buffers. The ledger caps the total. */
#ifndef PS5LM_MODEL_WEIGHTS_MIB /* the first two models' budget; larger ones set their own above */
#define PS5LM_MODEL_WEIGHTS_MIB 1152u
/* Context was 256: run 2 measured 271 MiB, 67 MiB of it Vulkan as predicted.
 * 384 until the performance work; the server's context for 512-token physical
 * batches measured 506 MiB for Qwen3.5 (its scheduler block grows with the
 * batch), so 768, and the total 2048 (was 1536; peak 1505 measured)
 * (docs/PORTING.md "Performance"). */
#define PS5LM_MODEL_CONTEXT_MIB 768u
#define PS5LM_MODEL_TOTAL_MIB 2048u
#define PS5LM_MODEL_SUBMIT_LIMIT 8192u
#define PS5LM_MODEL_WATCHDOG_S 300u
#define PS5LM_MODEL_SERVER_LOAD_S 120u
#endif
#define PS5LM_MODEL_GPU_ALLOCATION_MIB 256u
/* Pass rule (docs/PORTING.md): first 8 tokens identical, or the first
 * difference falls where the CPU's top two logits are within this gap. */
#define PS5LM_MODEL_PREFIX 8u
#define PS5LM_MODEL_TIE_GAP 0.5f
#endif
