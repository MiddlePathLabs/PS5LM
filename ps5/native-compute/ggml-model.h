/* SPDX-License-Identifier: GPL-3.0-or-later
 * Fixed parameters of the first resident dense-model run. Shared by the native
 * title (GPU) and the host CPU reference, so both see the same prompt and limits.
 */
#ifndef PS5LM_GGML_MODEL_H
#define PS5LM_GGML_MODEL_H
#define PS5LM_GGML_WORKLOAD "model-dense-greedy"
/* unsloth/Qwen3.5-0.8B-GGUF @ 6ab461498e2023f6e3c1baea90a8f0fe38ab64d0 */
#define PS5LM_MODEL_FILE "Qwen3.5-0.8B-Q4_K_M.gguf"
#define PS5LM_MODEL_PATH "/data/PS5LM/models/" PS5LM_MODEL_FILE
#define PS5LM_MODEL_BYTES 532517120ull
/* Qwen chat format with thinking switched off, tokenized with special tokens. */
#define PS5LM_MODEL_PROMPT "<|im_start|>user\nIn a short paragraph, explain how a GPU differs " \
    "from a CPU.<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n"
#define PS5LM_MODEL_PREDICT 32u
#define PS5LM_MODEL_CTX 1024u
#define PS5LM_MODEL_BATCH 32u
#define PS5LM_MODEL_THREADS 2
/* Budget, MiB. Weights = everything the model load adds (resident buffers plus
 * any loader read or staging buffer the heap keeps); context = KV cache,
 * recurrent state, outputs and compute buffers. The ledger caps the total. */
#define PS5LM_MODEL_WEIGHTS_MIB 1152u
/* Context was 256: run 2 measured 271 MiB, 67 MiB of it Vulkan as predicted. */
#define PS5LM_MODEL_CONTEXT_MIB 384u
#define PS5LM_MODEL_TOTAL_MIB 1536u
#define PS5LM_MODEL_GPU_ALLOCATION_MIB 256u
/* Pass rule (docs/PORTING.md): first 8 tokens identical, or the first
 * difference falls where the CPU's top two logits are within this gap. */
#define PS5LM_MODEL_PREFIX 8u
#define PS5LM_MODEL_TIE_GAP 0.5f
#endif
