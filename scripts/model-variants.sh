# Measured models (docs/PORTING.md "Measured models"): one entry per model, read by
# build-native-compute.sh (modes m-NAME, server-m-NAME) and build-host-model-reference.sh
# (PS5LM_MODEL_VARIANT=m-NAME). Each entry is the model's pin and its budget; write the
# budget in docs/PORTING.md before the first run of an entry.
#
#   ps5lm_model_variant NAME   sets mv_repo, mv_revision, mv_file, mv_bytes, mv_sha256 and
#                              mv_flags (the -D flags)

# Prompts: the same question as every other model, in each model's chat format. The title
# tokenizes with add_special, so a model whose GGUF adds BOS gets no BOS in its prompt.
mv_prompt_qwen_nothink='<|im_start|>user\nIn a short paragraph, explain how a GPU differs from a CPU.<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n'
mv_prompt_llama3='<|start_header_id|>user<|end_header_id|>\n\nIn a short paragraph, explain how a GPU differs from a CPU.<|eot_id|><|start_header_id|>assistant<|end_header_id|>\n\n'
mv_prompt_mistral='[INST] In a short paragraph, explain how a GPU differs from a CPU. [/INST]'
mv_prompt_minicpm5='<s><|im_start|>user\nIn a short paragraph, explain how a GPU differs from a CPU.<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n'
mv_prompt_granite4='<|start_of_role|>user<|end_of_role|>In a short paragraph, explain how a GPU differs from a CPU.<|end_of_text|>\n<|start_of_role|>assistant<|end_of_role|>'

ps5lm_model_variant() {
    local name=$1 workload prompt weights context total submit=16384 watchdog=300 load=180
    local alloc_mib='' buffer=''
    case "$name" in
        qwen35-9b)
            mv_repo=unsloth/Qwen3.5-9B-GGUF; mv_revision=3885219b6810b007914f3a7950a8d1b469d598a5
            mv_file=Qwen3.5-9B-Q4_K_M.gguf; mv_bytes=5680522464
            mv_sha256=03b74727a860a56338e042c4420bb3f04b2fec5734175f4cb9fa853daf52b7e8
            # Output Q6_K 795.7 MiB (one tensor), token_embd Q4_K 545.6 MiB (Vulkan_Host).
            prompt=$mv_prompt_qwen_nothink; weights=6144; context=1536; total=7680
            submit=24576; alloc_mib=832; buffer=872415232 ;;
        qwen38-27b-iq2xxs)
            mv_repo=unsloth/Qwen3.8-27B-GGUF; mv_revision=4ca720788d1e01f1bff70c033e0d0028fd02e502
            mv_file=Qwen3.8-27B-UD-IQ2_XXS.gguf; mv_bytes=7266070528
            mv_sha256=e792d8fb3142fe6d9171876d6da0f71f05a71028718debc72dbec93ff645e67d
            # Output Q3_K 521.0 MiB (one tensor), token_embd Q2_K 397.9 MiB (Vulkan_Host).
            # A new step (docs/PORTING.md "Measured models"): total 9216 MiB.
            prompt=$mv_prompt_qwen_nothink; weights=7424; context=1280; total=9216
            submit=32768; watchdog=480; load=300; alloc_mib=544; buffer=570425344 ;;
        qwen38-27b-q2kxl)
            mv_repo=unsloth/Qwen3.8-27B-GGUF; mv_revision=4ca720788d1e01f1bff70c033e0d0028fd02e502
            mv_file=Qwen3.8-27B-UD-Q2_K_XL.gguf; mv_bytes=9828981664
            mv_sha256=fd4730dd8aad070517978752b63d530aeb1740d2283cab9fa24f1e404032ddb0
            # Output Q4_K 682.0 MiB (one tensor), token_embd Q2_K 397.9 MiB (Vulkan_Host).
            # A new step (docs/PORTING.md "Measured models"): total 10624 MiB, the device-memory
            # rule's limit (1 GiB of RADV's ~11.4 GiB kept free).
            prompt=$mv_prompt_qwen_nothink; weights=9472; context=1088; total=10624
            submit=40960; watchdog=480; load=300; alloc_mib=704; buffer=738197504 ;;
        llama32-3b)
            # The abhi fork's model (its README: Llama 3.2 3B Instruct Q4_K_M, 1.9 GiB; no repo named).
            mv_repo=bartowski/Llama-3.2-3B-Instruct-GGUF; mv_revision=5ab33fa94d1d04e903623ae72c95d1696f09f9e8
            mv_file=Llama-3.2-3B-Instruct-Q4_K_M.gguf; mv_bytes=2019377696
            mv_sha256=6c1a2b41161032677be168d354123594c0e6e67d2b9227c84f296ad037c728ff
            # Tied embeddings: token_embd Q6_K 308.2 MiB is the output too (GPU copy + Vulkan_Host).
            prompt=$mv_prompt_llama3; weights=2816; context=1024; total=4096
            submit=12288; alloc_mib=336 ;;
        mistral7b)
            # ProsperoAI's model and file (its README links QuantFactory, Q4_0).
            mv_repo=QuantFactory/Mistral-7B-Instruct-v0.3-GGUF; mv_revision=09c7caf61e654ef9ed1ac5d3b0e56d56fabf1c7f
            mv_file=Mistral-7B-Instruct-v0.3.Q4_0.gguf; mv_bytes=4113289152
            mv_sha256=19c3cf872246d48e7a9784d6def7a694a6c9056cfbeb98f7061d43cd0a7c8b7f
            # Untied: output Q6_K 105 MiB, token_embd Q4_0 72 MiB.
            prompt=$mv_prompt_mistral; weights=4608; context=1024; total=6144; submit=16384 ;;
        minicpm5-2b)
            # ProsperoAI's model (its README links openbmb's repo, Q8_0).
            mv_repo=openbmb/MiniCPM5-2B-GGUF; mv_revision=2079a22f3beaa4e306449978533478fe0522f4b3
            mv_file=MiniCPM5-2B-Q8_0.gguf; mv_bytes=2679710688
            mv_sha256=c5415f8989bf88a8288f1b55a3cc371af53c07b0faa220a63bd7a990cfaba078
            # Untied: output and token_embd Q8_0 270.9 MiB each; BOS is in the template, not added.
            prompt=$mv_prompt_minicpm5; weights=3328; context=1024; total=4608
            submit=12288; alloc_mib=288 ;;
        granite4-htiny)
            # ProsperoAI's model (its README links ibm-granite's repo, Q4_K_M).
            mv_repo=ibm-granite/granite-4.0-h-tiny-GGUF; mv_revision=08d5a8a9741dd5c1a95d2d39e25253226aa1464e
            mv_file=granite-4.0-h-tiny-Q4_K_M.gguf; mv_bytes=4230976352
            mv_sha256=5a38b08c441ae1adbafb1d2b8a7167e0d48734d83af68b268cefea1eec553dcd
            # granitehybrid: Mamba-2 with attention in 4 of 40 layers, 64 experts (6 used).
            prompt=$mv_prompt_granite4; weights=4608; context=1024; total=6144; submit=16384 ;;
        *) echo "Unknown measured model: $name" >&2; return 2 ;;
    esac
    workload="model-$name-greedy"
    mv_flags=(-DPS5LM_MODEL_CUSTOM "-DPS5LM_GGML_WORKLOAD=\"$workload\"" "-DPS5LM_SERVER_WORKLOAD_NAME=\"server-$name\""
        "-DPS5LM_MODEL_FILE=\"$mv_file\"" "-DPS5LM_MODEL_BYTES=${mv_bytes}ull"
        "-DPS5LM_MODEL_PROMPT=\"$prompt\""
        "-DPS5LM_MODEL_WEIGHTS_MIB=${weights}u" "-DPS5LM_MODEL_CONTEXT_MIB=${context}u"
        "-DPS5LM_MODEL_TOTAL_MIB=${total}u" "-DPS5LM_MODEL_SUBMIT_LIMIT=${submit}u"
        "-DPS5LM_MODEL_WATCHDOG_S=${watchdog}u" "-DPS5LM_MODEL_SERVER_LOAD_S=${load}u")
    if [ -n "$alloc_mib" ]; then
        # ggml allocates up to the guard's cap less 16 MiB (as for the other models).
        mv_flags+=("-DPS5LM_MODEL_GPU_ALLOCATION_MIB=${alloc_mib}u"
            "-DPS5LM_MODEL_VK_MAX_ALLOCATION=\"$(( (alloc_mib - 16) << 20 ))\"")
    fi
    if [ -n "$buffer" ]; then
        mv_flags+=("-DPS5LM_MODEL_VK_MAX_BUFFER=\"$buffer\"")
    fi
}
