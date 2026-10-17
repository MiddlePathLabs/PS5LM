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

# Long-context server builds (docs/PORTING.md "Long context"): NAME-cNk[-kvq8|-kvq4] is the
# entry NAME with a larger server context, its own context and total caps (written in
# PORTING.md before the first run of each), the KV cache type, the serving window and the
# prompt chunk the server decodes at a time. Server builds only (server-m-NAME-cNk).
ps5lm_long_context() {
    lc_kv=f16; lc_pbatch=512; lc_window=600
    case "$1" in
        # MiB:                          base               ctx    context total  window chunk
        # Host rehearsal only (never built for the console): 6 KV layers, 24 KiB per token.
        qwen35-0.8b-c32k)  set -- qwen35-0.8b 32768 2048 3072 600 512 ;;
        qwen35-9b-c4k)     set -- qwen35-9b 4096 1024 6784 900 512 ;;
        qwen35-9b-c8k)     set -- qwen35-9b 8192 1152 6912 900 512 ;;
        qwen35-9b-c16k)    set -- qwen35-9b 16384 1536 7296 900 512 ;;
        qwen35-9b-c32k)    set -- qwen35-9b 32768 2048 7808 1200 256 ;;
        qwen35-9b-c64k)    set -- qwen35-9b 65536 3328 9088 2400 128 ;;
        llama32-3b-c4k)    set -- llama32-3b 4096 896 3328 600 512 ;;
        llama32-3b-c8k)    set -- llama32-3b 8192 1408 3968 600 512 ;;
        llama32-3b-c16k)   set -- llama32-3b 16384 2432 4992 900 512 ;;
        llama32-3b-c32k)   set -- llama32-3b 32768 4480 7040 1200 256 ;;
        qwen38-27b-iq2xxs-c4k)  set -- qwen38-27b-iq2xxs 4096 1536 8832 900 256 ;;
        qwen38-27b-iq2xxs-c8k)  set -- qwen38-27b-iq2xxs 8192 1792 9088 900 256 ;;
        qwen38-27b-iq2xxs-c16k) set -- qwen38-27b-iq2xxs 16384 2432 9728 1200 128 ;;
        qwen38-27b-iq2xxs-c32k) set -- qwen38-27b-iq2xxs 32768 3328 10624 1800 128 ;;
        qwen38-27b-iq2xxs-c64k-kvq8) set -- qwen38-27b-iq2xxs 65536 3456 10624 2400 256; lc_kv=q8_0 ;;
        qwen38-27b-iq2xxs-c64k-kvq4) set -- qwen38-27b-iq2xxs 65536 2688 9984 2400 256; lc_kv=q4_0 ;;
        # Evaluation windows (docs/PORTING.md "Evaluation builds"): the rung's validated caps
        # with a longer serving window for the quality/comprehension suites; ctx 2048 for the
        # 2K suites. The runner shuts the server down when done, so the window is only a bound.
        qwen35-9b-c2k-q)          set -- qwen35-9b 2048 1024 7680 1500 512 ;;
        qwen35-9b-c16k-q)         set -- qwen35-9b 16384 1536 7296 2100 512 ;;
        qwen35-9b-c32k-q)         set -- qwen35-9b 32768 2048 7808 2700 256 ;;
        qwen35-9b-c64k-q)         set -- qwen35-9b 65536 3328 9088 3600 128 ;;
        qwen38-27b-iq2xxs-c2k-q)  set -- qwen38-27b-iq2xxs 2048 1280 9216 1800 256 ;;
        qwen38-27b-iq2xxs-c16k-q) set -- qwen38-27b-iq2xxs 16384 2432 9728 2400 128 ;;
        qwen38-27b-iq2xxs-c32k-q) set -- qwen38-27b-iq2xxs 32768 3328 10624 3900 128 ;;
        qwen38-27b-q2kxl-c2k-q)   set -- qwen38-27b-q2kxl 2048 1280 10624 1800 256 ;;
        *) echo "Unknown long-context entry: $1" >&2; return 2 ;;
    esac
    lc_base=$1; lc_ctx=$2; lc_context=$3; lc_total=$4; lc_window=$5; lc_pbatch=$6
}

ps5lm_model_variant() {
    local name=$1 workload prompt weights context total submit=16384 watchdog=300 load=180
    local alloc_mib='' buffer='' base=$1
    lc_ctx=''
    case "$name" in
        *-c[0-9]*k*) ps5lm_long_context "$name" || return 2; base=$lc_base ;;
    esac
    case "$base" in
        qwen35-0.8b)
            # The first dense model (build modes model/server-qwen), as an entry for the host
            # rehearsals of long contexts (docs/PORTING.md "Long context"); caps as server-qwen.
            mv_repo=unsloth/Qwen3.5-0.8B-GGUF; mv_revision=6ab461498e2023f6e3c1baea90a8f0fe38ab64d0
            mv_file=Qwen3.5-0.8B-Q4_K_M.gguf; mv_bytes=532517120
            mv_sha256=bd258782e35f7f458f8aced1adc053e6e92e89bc735ba3be89d38a06121dc517
            prompt=$mv_prompt_qwen_nothink; weights=1152; context=768; total=2048; submit=8192 ;;
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
    if [ -n "$lc_ctx" ]; then
        # The base entry's pin, prompt, weights cap and allocation caps; this context's caps.
        context=$lc_context; total=$lc_total
    fi
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
    if [ -n "$lc_ctx" ]; then
        local kv_ggml
        case "$lc_kv" in f16) kv_ggml=GGML_TYPE_F16 ;; q8_0) kv_ggml=GGML_TYPE_Q8_0 ;; q4_0) kv_ggml=GGML_TYPE_Q4_0 ;; esac
        mv_flags+=("-DPS5LM_SERVER_CTX=$lc_ctx" "-DPS5LM_SERVER_KV=\"$lc_kv\"" "-DPS5LM_SERVER_KV_GGML=$kv_ggml"
            "-DPS5LM_SERVER_WINDOW_S=${lc_window}u" "-DPS5LM_SERVER_PROMPT_BATCH=$lc_pbatch")
    fi
}
