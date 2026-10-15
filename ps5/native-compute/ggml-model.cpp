/* SPDX-License-Identifier: GPL-3.0-or-later
 * First resident dense-model run: load one GGUF with every layer on the GPU,
 * decode a fixed prompt greedily and compare the tokens with a CPU reference.
 *
 * The same file builds twice. Natively it is the PPSA99041 workload and checks
 * its tokens against model-reference.h. With PS5LM_HOST_REFERENCE it is a host
 * program on the same llama.cpp pin, CPU backend only, that writes that header.
 */
#include "ggml.h"
#include "ggml-backend.h"
#include "llama.h"
#include "ggml-model.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#if defined(PS5LM_HOST_REFERENCE)
[[maybe_unused]] static uint64_t direct_live() { return 0; }
static uint64_t direct_peak() { return 0; }
static unsigned long long vk_peak() { return 0; }
static unsigned long long vk_live() { return 0; }
static unsigned vk_allocations() { return 0; }
[[maybe_unused]] static unsigned vk_errors() { return 0; }
static unsigned submissions() { return 0; }
static void flush_output() { std::fflush(stdout); }
static void ps5lm_report_allocations() {}
static void phase(const char *) {}
[[noreturn]] static void fatal(const char *stage)
{
    std::fprintf(stderr, "fatal: %s\n", stage);
    std::exit(1);
}
#else
#include "model-reference.h"
extern "C" [[noreturn]] void ps5lm_ops_fatal(const char *stage);
extern "C" void ps5lm_ops_checked(unsigned count, unsigned errors);
extern "C" uint64_t ps5lm_direct_live_bytes(void);
extern "C" uint64_t ps5lm_direct_peak_bytes(void);
extern "C" int sceKernelDebugOutText(int channel, const char *text);
extern "C" unsigned long long ps5lm_vk_memory_peak_bytes(void);
extern "C" unsigned long long ps5lm_vk_memory_live_bytes(void);
extern "C" unsigned ps5lm_vk_memory_allocations(void);
extern "C" unsigned ps5lm_vk_memory_errors(void);
extern "C" unsigned ps5lm_submission_count(void);
extern "C" void ps5lm_flush_output(void);
extern "C" void ps5lm_report_allocations(void);
extern "C" uint64_t ps5lm_elapsed_ms(void);
static void flush_output() { ps5lm_flush_output(); }
/* Timed progress in the kernel log, which is fast, before the slow file flush. */
static void phase(const char *name)
{
    char line[160];
    std::snprintf(line, sizeof line, "PS5LM_MODEL_PHASE {\"phase\":\"%s\",\"ms\":%llu,\"direct_live\":%llu}\n",
                  name, (unsigned long long)ps5lm_elapsed_ms(), (unsigned long long)ps5lm_direct_live_bytes());
    sceKernelDebugOutText(0, line);
}
static unsigned long long vk_peak() { return ps5lm_vk_memory_peak_bytes(); }
static unsigned long long vk_live() { return ps5lm_vk_memory_live_bytes(); }
static unsigned vk_allocations() { return ps5lm_vk_memory_allocations(); }
static unsigned vk_errors() { return ps5lm_vk_memory_errors(); }
static unsigned submissions() { return ps5lm_submission_count(); }
static uint64_t direct_live() { return ps5lm_direct_live_bytes(); }
static uint64_t direct_peak() { return ps5lm_direct_peak_bytes(); }
static void klog(const char *line) { sceKernelDebugOutText(0, line); }
static void checked(unsigned count, unsigned errors) { ps5lm_ops_checked(count, errors); }
[[noreturn]] static void fatal(const char *stage) { ps5lm_ops_fatal(stage); }
#endif

static constexpr uint64_t MiB = 1048576;

/* What the llama.cpp log says about placement, kept alongside the full log. */
static struct {
    int offloaded = -1, offload_total = -1;
    int splits_batch = -1, splits_single = -1;
    unsigned warnings = 0, errors = 0;
} placement;

static bool dropping;  // a dropped line's GGML_LOG_LEVEL_CONT pieces go too

static void log_line(ggml_log_level level, const char *text, void *)
{
    // Debug output and the metadata dump would cost minutes of file writes.
    if (level == GGML_LOG_LEVEL_CONT ? dropping :
        (dropping = level == GGML_LOG_LEVEL_DEBUG || std::strstr(text, "llama_model_loader: - kv") != nullptr))
        return;
    std::fputs(text, stdout);
    if (level == GGML_LOG_LEVEL_WARN) ++placement.warnings;
    if (level == GGML_LOG_LEVEL_ERROR) ++placement.errors;
    if (const char *at = std::strstr(text, "offloaded "))
        std::sscanf(at, "offloaded %d/%d", &placement.offloaded, &placement.offload_total);
    // "graph: nodes = N, splits = S" or, when batch and single differ, "splits = S / T".
    if (const char *at = std::strstr(text, ", splits = ")) {
        if (std::sscanf(at, ", splits = %d / %d", &placement.splits_batch, &placement.splits_single) == 1)
            placement.splits_single = placement.splits_batch;
    }
}

static double ms_since(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

static std::string json_text(const std::string &text, size_t limit)
{
    std::string out;
    for (size_t i = 0; i < text.size() && out.size() < limit; ++i) {
        const unsigned char c = (unsigned char)text[i];
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c == '\n') out += "\\n";
        else if (c < 0x20 || c == 0x7f) { char hex[8]; std::snprintf(hex, sizeof hex, "\\u%04x", c); out += hex; }
        else out += char(c);
    }
    return out;
}

static std::string piece(const llama_vocab *vocab, llama_token token)
{
    char buffer[256];
    const int n = llama_token_to_piece(vocab, token, buffer, sizeof buffer, 0, true);
    return n > 0 ? std::string(buffer, size_t(n)) : std::string();
}

struct step {
    llama_token top1, top2;
    float logit1, logit2;
    unsigned nonfinite;
};

static step rank(const float *logits, int n_vocab)
{
    step s{-1, -1, -INFINITY, -INFINITY, 0};
    for (int i = 0; i < n_vocab; ++i) {
        const float v = logits[i];
        if (!std::isfinite(v)) { ++s.nonfinite; continue; }
        if (v > s.logit1) { s.top2 = s.top1; s.logit2 = s.logit1; s.top1 = i; s.logit1 = v; }
        else if (v > s.logit2) { s.top2 = i; s.logit2 = v; }
    }
    return s;
}

static bool decode(llama_context *ctx, llama_batch &batch, const llama_token *tokens, int count, int pos)
{
    batch.n_tokens = count;
    for (int i = 0; i < count; ++i) {
        batch.token[i] = tokens[i];
        batch.pos[i] = pos + i;
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = 0;
        batch.logits[i] = i == count - 1;
    }
    return llama_decode(ctx, batch) == 0;
}

struct run {
    std::vector<step> steps;
    std::string text;
    int prompt_tokens = 0;
    unsigned nonfinite = 0;
    bool eog = false;
    double load_ms = 0, context_ms = 0, prompt_ms = 0, generate_ms = 0;
    uint64_t live_start = 0, live_model = 0, live_context = 0, model_bytes = 0;
    int n_layer = 0;
    unsigned long long vk_peak = 0;
    unsigned submissions = 0;
};

static int generate(run &r)
{
    llama_log_set(log_line, nullptr);
    llama_backend_init();
    for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        std::printf("PS5LM_MODEL_DEVICE {\"index\":%zu,\"name\":\"%s\",\"type\":%d}\n", i,
                    ggml_backend_dev_name(dev), int(ggml_backend_dev_type(dev)));
    }
    r.live_start = direct_live();
    phase("load_begin");
    flush_output();
    llama_model_params mp = llama_model_default_params();
#if defined(PS5LM_HOST_REFERENCE)
    mp.n_gpu_layers = 0;
#else
    mp.n_gpu_layers = 999;                 // all repeating layers and the output layer
#endif
    mp.load_mode = LLAMA_LOAD_MODE_NONE;   // read into accounted buffers, no mmap
    mp.lazy_mode = LLAMA_LAZY_MODE_OFF;
    mp.use_extra_bufts = false;            // no CPU weight repacking
    mp.check_tensors = false;
    mp.load_mtp = false;
    auto t = std::chrono::steady_clock::now();
#if defined(PS5LM_HOST_REFERENCE)
    extern const char *host_model_path;
    llama_model *model = llama_model_load_from_file(host_model_path, mp);
#elif defined(PS5LM_SMOKE_MODEL_PATH)
    llama_model *model = llama_model_load_from_file(PS5LM_SMOKE_MODEL_PATH, mp); // host smoke only
#else
    llama_model *model = llama_model_load_from_file(PS5LM_MODEL_PATH, mp);
#endif
    r.load_ms = ms_since(t);
    phase("load_end");
    if (!model) fatal("model_load");
    r.live_model = direct_live();
    r.model_bytes = llama_model_size(model);
    r.n_layer = llama_model_n_layer(model);
    std::printf("PS5LM_MODEL_LOADED {\"load_ms\":%.1f,\"model_size_bytes\":%llu,\"n_layer\":%d,"
                "\"direct_live_bytes\":%llu,\"direct_peak_bytes\":%llu,\"vk_live_bytes\":%llu,"
                "\"vk_allocations\":%u,\"submissions\":%u}\n", r.load_ms,
                (unsigned long long)r.model_bytes, r.n_layer,
                (unsigned long long)r.live_model, (unsigned long long)direct_peak(), vk_live(),
                vk_allocations(), submissions());
    if (r.live_model - r.live_start > PS5LM_MODEL_WEIGHTS_MIB * MiB) fatal("model_weights_budget");
#if !defined(PS5LM_HOST_REFERENCE)
    if (placement.offloaded < 0 || placement.offloaded != placement.offload_total ||
        placement.offload_total != r.n_layer + 1)
        fatal("model_not_fully_offloaded");
#endif

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = PS5LM_MODEL_CTX;
    cp.n_batch = PS5LM_MODEL_BATCH;
    cp.n_ubatch = PS5LM_MODEL_BATCH;
    cp.n_seq_max = 1;
    cp.n_threads = PS5LM_MODEL_THREADS;
    cp.n_threads_batch = PS5LM_MODEL_THREADS;
    cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
    cp.type_k = GGML_TYPE_F16;
    cp.type_v = GGML_TYPE_F16;
    cp.offload_kqv = true;
    cp.op_offload = false;                 // never stream CPU weights through the GPU
    cp.no_perf = false;
    t = std::chrono::steady_clock::now();
    llama_context *ctx = llama_init_from_model(model, cp);
    r.context_ms = ms_since(t);
    phase("context_end");
    ps5lm_report_allocations();
    if (!ctx) fatal("model_context");
    r.live_context = direct_live();
    std::printf("PS5LM_MODEL_CONTEXT {\"context_ms\":%.1f,\"n_ctx\":%u,\"direct_live_bytes\":%llu,"
                "\"direct_peak_bytes\":%llu,\"vk_live_bytes\":%llu,\"vk_allocations\":%u}\n",
                r.context_ms, llama_n_ctx(ctx), (unsigned long long)r.live_context,
                (unsigned long long)direct_peak(), vk_live(), vk_allocations());
    if (r.live_context - r.live_model > PS5LM_MODEL_CONTEXT_MIB * MiB) fatal("model_context_budget");
    flush_output();

    const llama_vocab *vocab = llama_model_get_vocab(model);
    const int n_vocab = llama_vocab_n_tokens(vocab);
    const char *prompt = PS5LM_MODEL_PROMPT;
    llama_token tokens[256];
    r.prompt_tokens = llama_tokenize(vocab, prompt, int(std::strlen(prompt)), tokens, 256, true, true);
    if (r.prompt_tokens <= 0 || r.prompt_tokens > 256) fatal("model_tokenize");
    std::printf("PS5LM_MODEL_PROMPT {\"tokens\":%d,\"ids\":[", r.prompt_tokens);
    for (int i = 0; i < r.prompt_tokens; ++i) std::printf("%s%d", i ? "," : "", tokens[i]);
    std::printf("]}\n");

    llama_batch batch = llama_batch_init(PS5LM_MODEL_BATCH, 0, 1);
    t = std::chrono::steady_clock::now();
    int pos = 0;
    while (pos < r.prompt_tokens) {
        const int n = std::min<int>(PS5LM_MODEL_BATCH, r.prompt_tokens - pos);
        if (!decode(ctx, batch, tokens + pos, n, pos)) fatal("model_prompt_decode");
        pos += n;
    }
    r.prompt_ms = ms_since(t);
    phase("prompt_end");
    t = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < PS5LM_MODEL_PREDICT; ++i) {
        const float *logits = llama_get_logits_ith(ctx, -1);
        if (!logits) fatal("model_logits");
        const step s = rank(logits, n_vocab);
        r.nonfinite += s.nonfinite;
        if (s.top1 < 0 || s.top2 < 0) fatal("model_no_finite_logits");
        r.steps.push_back(s);
        const std::string text = piece(vocab, s.top1);
        r.text += text;
        std::printf("PS5LM_MODEL_TOKEN {\"index\":%u,\"token\":%d,\"logit\":%.5f,\"second\":%d,"
                    "\"second_logit\":%.5f,\"nonfinite\":%u,\"text\":\"%s\"}\n",
                    i, s.top1, s.logit1, s.top2, s.logit2, s.nonfinite, json_text(text, 32).c_str());
        if (llama_vocab_is_eog(vocab, s.top1)) { r.eog = true; break; }
        if (i + 1 == PS5LM_MODEL_PREDICT) break;
        if (!decode(ctx, batch, &s.top1, 1, pos)) fatal("model_token_decode");
        ++pos;
    }
    r.generate_ms = ms_since(t);
    phase("generate_end");
    ps5lm_report_allocations();
    r.vk_peak = vk_peak();
    r.submissions = submissions();
    llama_perf_context_print(ctx);
    llama_batch_free(batch);
    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    phase("freed");
    return 0;
}

static unsigned longest_repeat(const std::vector<step> &steps)
{
    unsigned best = 0, run = 0;
    for (size_t i = 0; i < steps.size(); ++i) {
        run = i && steps[i].top1 == steps[i - 1].top1 ? run + 1 : 1;
        if (run > best) best = run;
    }
    return best;
}

#if defined(PS5LM_HOST_REFERENCE)
const char *host_model_path;

int main(int argc, char **argv)
{
    if (argc != 3) { std::fprintf(stderr, "usage: %s MODEL.gguf OUT.h\n", argv[0]); return 64; }
    host_model_path = argv[1];
    run r;
    generate(r);
    if (r.nonfinite || r.steps.size() != PS5LM_MODEL_PREDICT) fatal("reference_incomplete");
    FILE *out = std::fopen(argv[2], "w");
    if (!out) fatal("reference_write");
    std::fprintf(out, "/* Generated by the host CPU reference (ggml-model.cpp); do not edit.\n"
                      " * Text: %s */\n", json_text(r.text, 400).c_str());
    std::fprintf(out, "#define PS5LM_REFERENCE_PROMPT_TOKENS %d\n", r.prompt_tokens);
    std::fprintf(out, "static const int ps5lm_reference_tokens[%u] = {", PS5LM_MODEL_PREDICT);
    for (size_t i = 0; i < r.steps.size(); ++i) std::fprintf(out, "%s%d", i ? "," : "", r.steps[i].top1);
    std::fprintf(out, "};\nstatic const float ps5lm_reference_gaps[%u] = {", PS5LM_MODEL_PREDICT);
    for (size_t i = 0; i < r.steps.size(); ++i)
        std::fprintf(out, "%s%.9gf", i ? "," : "", double(r.steps[i].logit1 - r.steps[i].logit2));
    std::fprintf(out, "};\n");
    std::fclose(out);
    std::printf("PS5LM_MODEL_REFERENCE {\"tokens\":%zu,\"longest_repeat\":%u,\"prompt_ms\":%.1f,"
                "\"generate_ms\":%.1f,\"text\":\"%s\"}\n", r.steps.size(), longest_repeat(r.steps),
                r.prompt_ms, r.generate_ms, json_text(r.text, 400).c_str());
    return 0;
}
#else
static_assert(sizeof(ps5lm_reference_tokens) / sizeof(ps5lm_reference_tokens[0]) == PS5LM_MODEL_PREDICT,
              "Reference length");

extern "C" int ps5lm_run_ggml_ops(void)
{
    try {
        run r;
        generate(r);
        if (r.prompt_tokens != PS5LM_REFERENCE_PROMPT_TOKENS) fatal("model_prompt_mismatch");
        const unsigned n = unsigned(r.steps.size());
        int first_diff = -1;
        for (unsigned i = 0; i < n && first_diff < 0; ++i)
            if (r.steps[i].top1 != ps5lm_reference_tokens[i]) first_diff = int(i);
        if (first_diff < 0 && n < PS5LM_MODEL_PREDICT) first_diff = int(n); // stopped early
        const unsigned repeat = longest_repeat(r.steps);
        const char *rule = "fail";
        if (first_diff < 0) rule = "identical";
        else if (unsigned(first_diff) >= PS5LM_MODEL_PREFIX) rule = "prefix";
        else if (unsigned(first_diff) < n && ps5lm_reference_gaps[first_diff] <= PS5LM_MODEL_TIE_GAP) rule = "near_tie";
        const bool sane = !r.nonfinite && repeat < 8 && placement.errors == 0 && vk_errors() == 0;
        const bool pass = std::strcmp(rule, "fail") != 0 && sane;
        unsigned matched = 0;
        while (matched < n && r.steps[matched].top1 == ps5lm_reference_tokens[matched]) ++matched;
        const double tg = r.generate_ms > 0 && n > 1 ? (n - 1) * 1000.0 / r.generate_ms : 0;
        const double pp = r.prompt_ms > 0 ? r.prompt_tokens * 1000.0 / r.prompt_ms : 0;
        std::string ids;
        for (unsigned i = 0; i < n; ++i) ids += (i ? "," : "") + std::to_string(r.steps[i].top1);
        char line[1536];
        std::snprintf(line, sizeof line,
            "PS5LM_MODEL_SUMMARY {\"pass\":%s,\"rule\":\"%s\",\"generated\":%u,\"matched_prefix\":%u,"
            "\"first_diff\":%d,\"reference_gap_at_diff\":%.6f,\"nonfinite\":%u,\"longest_repeat\":%u,"
            "\"eog\":%s,\"offloaded\":%d,\"offload_total\":%d,\"splits_batch\":%d,\"splits_single\":%d,"
            "\"log_warnings\":%u,\"log_errors\":%u,\"model_size_bytes\":%llu,"
            "\"weights_bytes\":%llu,\"context_bytes\":%llu,\"direct_peak_bytes\":%llu,"
            "\"vk_peak_bytes\":%llu,\"vk_live_after_free\":%llu,\"vk_errors\":%u,\"submissions\":%u,"
            "\"load_ms\":%.1f,\"context_ms\":%.1f,\"prompt_ms\":%.1f,\"generate_ms\":%.1f,"
            "\"prompt_tok_s\":%.2f,\"gen_tok_s\":%.2f,\"tokens\":[%s]}\n",
            pass ? "true" : "false", rule, n, matched, first_diff,
            first_diff >= 0 && unsigned(first_diff) < n ? double(ps5lm_reference_gaps[first_diff]) : -1.0,
            r.nonfinite, repeat, r.eog ? "true" : "false", placement.offloaded, placement.offload_total,
            placement.splits_batch, placement.splits_single, placement.warnings, placement.errors,
            (unsigned long long)r.model_bytes, (unsigned long long)(r.live_model - r.live_start),
            (unsigned long long)(r.live_context - r.live_model), (unsigned long long)direct_peak(),
            r.vk_peak, vk_live(), vk_errors(), r.submissions,
            r.load_ms, r.context_ms, r.prompt_ms, r.generate_ms, pp, tg, ids.c_str());
        std::fputs(line, stdout);
        klog(line);
        std::printf("PS5LM_MODEL_TEXT {\"text\":\"%s\"}\n", json_text(r.text, 1024).c_str());
        flush_output();
        phase("flushed");
        checked(PS5LM_MODEL_PREDICT, pass ? 0 : 1);
        return pass ? 0 : 6;
    } catch (const std::exception &) {
        fatal("model_exception");
    } catch (...) {
        fatal("model_unknown_exception");
    }
}
#endif
