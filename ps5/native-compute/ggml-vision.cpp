/* SPDX-License-Identifier: GPL-3.0-or-later
 * Vision model mode (build modes "vl4b", "vl8b"; docs/PORTING.md "Vision"):
 * load one Qwen-VL text model and its mmproj with everything on the GPU,
 * encode one fixed image, compare the image embedding with the host CPU's,
 * then decode the fixed prompt greedily and compare the tokens.
 *
 * The same file builds three ways:
 *  - natively, the PPSA99041 workload: compares against the CPU reference that
 *    the build embeds (model-reference.h, vision-reference.h and the raw
 *    embedding/trace files through .incbin);
 *  - with PS5LM_HOST_REFERENCE on the Mac, CPU backend only: writes those files;
 *  - with PS5LM_HOST_REFERENCE and --gpu on the Mac's Metal build: the same
 *    comparison as the title, a check of this runner (not of the PS5 GPU).
 *
 * Image embedding check: max abs error, error relative to the largest
 * reference value, relative L2 error, global and smallest per-token cosine
 * similarity, per slice (main embedding and each deepstack slice). If it is
 * outside the tolerance, the title encodes once more with a trace of the
 * encoder's intermediate tensors (layer 0 ops, every layer's output) and
 * compares strided samples of each against the CPU's, to find the first op
 * that diverges.
 */
#include "ggml.h"
#include "ggml-backend.h"
#include "llama.h"
#include "mtmd.h"
#include "mtmd-helper.h"
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

#if !defined(PS5LM_MODEL_VISION)
#error "ggml-vision.cpp needs a vision model variant (PS5LM_MODEL_QWEN3VL4B or PS5LM_MODEL_QWEN3VL8B)"
#endif

#if defined(PS5LM_HOST_REFERENCE)
static uint64_t direct_live() { return 0; }
static uint64_t direct_peak() { return 0; }
static unsigned long long vk_peak() { return 0; }
static unsigned long long vk_live() { return 0; }
static unsigned vk_allocations() { return 0; }
static unsigned submissions() { return 0; }
static void flush_output() { std::fflush(stdout); }
static void ps5lm_report_allocations() {}
static void phase(const char *) {}
static void klog(const char *) {}
[[noreturn]] static void fatal(const char *stage)
{
    std::fprintf(stderr, "fatal: %s\n", stage);
    std::fflush(nullptr);
    std::exit(1);
}
static bool host_gpu;            // --gpu: the Metal stand-in, compared with a CPU reference
static const char *host_model_path, *host_mmproj_path;
static std::vector<unsigned char> host_png;
#else
#include "model-reference.h"
#include "vision-reference.h"
#include "vision-image.h"
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
[[noreturn]] static void fatal(const char *stage) { ps5lm_ops_fatal(stage); }

/* The CPU reference's image embedding and trace samples, raw little-endian
 * floats, embedded by the build (paths from build-native-compute.sh). */
#define PS5LM_STR2(x) #x
#define PS5LM_STR(x) PS5LM_STR2(x)
__asm__(".section .rodata\n.balign 64\n"
        "ps5lm_vision_embd_ref:\n.incbin \"" PS5LM_VISION_EMBD_BIN "\"\n"
        "ps5lm_vision_embd_ref_end:\n.balign 64\n"
        "ps5lm_vision_trace_ref:\n.incbin \"" PS5LM_VISION_TRACE_BIN "\"\n"
        "ps5lm_vision_trace_ref_end:\n.previous\n");
extern "C" const float ps5lm_vision_embd_ref[], ps5lm_vision_embd_ref_end[];
extern "C" const float ps5lm_vision_trace_ref[], ps5lm_vision_trace_ref_end[];
#endif

static constexpr uint64_t MiB = 1048576;

/* What the llama.cpp log says about placement (as ggml-model.cpp reads it). */
static struct {
    int offloaded = -1, offload_total = -1;
    int splits_batch = -1, splits_single = -1;
    unsigned warnings = 0, errors = 0;
    double host_model_mib = 0;
    unsigned moved = 0;
    unsigned clip_unsupported = 0;   // "unsupported operators by the backend" (encoder ops on the CPU)
    int clip_splits = -1, clip_nodes = -1;
    double clip_compute_mib = 0;
} placement;

static bool dropping;

static void log_line(ggml_log_level level, const char *text, void *)
{
    const bool moved = level != GGML_LOG_LEVEL_CONT &&
                       std::strstr(text, "cannot be used with preferred buffer type") != nullptr;
    if (moved) ++placement.moved;
    if (level == GGML_LOG_LEVEL_CONT ? dropping :
        (dropping = !moved && (level == GGML_LOG_LEVEL_DEBUG ||
                               std::strstr(text, "llama_model_loader: - kv") != nullptr)))
        return;
    std::fputs(text, stdout);
    if (level == GGML_LOG_LEVEL_WARN) ++placement.warnings;
    if (level == GGML_LOG_LEVEL_ERROR) ++placement.errors;
    if (const char *at = std::strstr(text, "offloaded "))
        std::sscanf(at, "offloaded %d/%d", &placement.offloaded, &placement.offload_total);
    if (const char *at = std::strstr(text, "Vulkan_Host model buffer size ="))
        std::sscanf(at, "Vulkan_Host model buffer size = %lf", &placement.host_model_mib);
    if (const char *at = std::strstr(text, ", splits = ")) {
        if (std::sscanf(at, ", splits = %d / %d", &placement.splits_batch, &placement.splits_single) == 1)
            placement.splits_single = placement.splits_batch;
    }
    if (std::strstr(text, "unsupported operators by the backend")) ++placement.clip_unsupported;
    if (const char *at = std::strstr(text, "graph splits = "))
        std::sscanf(at, "graph splits = %d, nodes = %d", &placement.clip_splits, &placement.clip_nodes);
    if (const char *at = std::strstr(text, "compute buffer size =")) {
        double mib = 0;
        if (std::strstr(text, "warmup") || std::strstr(text, "reserve_compute_meta"))
            if (std::sscanf(at, "compute buffer size = %lf", &mib) == 1) placement.clip_compute_mib += mib;
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

/* ---- Encoder trace: strided samples of named intermediate tensors ---- */
static constexpr size_t TRACE_STRIDE = 61;
static bool trace_on;
struct trace_item {
    std::string name;
    int64_t elements;
    std::vector<float> samples;
    double sumsq;
};
static std::vector<trace_item> trace_items;

static bool traced_name(const char *name)
{
    static const char *const layer0[] = {"patch_bias", "inp_pos_emb", "Qcur_rope-0", "Kcur_rope-0",
                                         "kqv_out-0", "attn_out-0", "ffn_inp-0", "ffn_out-0"};
    for (const char *n : layer0)
        if (std::strcmp(name, n) == 0) return true;
    return std::strncmp(name, "layer_out-", 10) == 0;
}

static bool trace_cb(ggml_tensor *t, bool ask, void *)
{
    if (!trace_on || !t || t->type != GGML_TYPE_F32 || !traced_name(t->name) || std::strchr(t->name, ' '))
        return false;  // not views such as "layer_out-5 (reshaped)": their memory may be reused by then
    if (ask) return true;
    if (!ggml_is_contiguous(t)) return true;
    const int64_t n = ggml_nelements(t);
    std::vector<float> all(static_cast<size_t>(n));
    ggml_backend_tensor_get(t, all.data(), 0, ggml_nbytes(t));
    trace_item item{t->name, n, {}, 0.0};
    for (int64_t i = 0; i < n; ++i) item.sumsq += double(all[size_t(i)]) * all[size_t(i)];
    for (int64_t i = 0; i < n; i += int64_t(TRACE_STRIDE)) item.samples.push_back(all[size_t(i)]);
    trace_items.push_back(std::move(item));
    return true;
}

/* ---- Embedding comparison ---- */
struct embd_cmp {
    double max_abs = 0, ref_absmax = 0, rel_l2 = 0, cos = 0, cos_min_token = 2;
    int worst_token = -1;
    unsigned nonfinite = 0, tokens_below = 0;
    double slice_rel_l2[8] = {0};
    int slices = 0;
};

static embd_cmp compare_embd(const float *gpu, const float *ref, int n_tokens, int n_embd_inp, int n_embd)
{
    embd_cmp c;
    double dd = 0, rr = 0, gg = 0, gr = 0;
    c.slices = std::min(8, n_embd > 0 ? n_embd_inp / n_embd : 1);
    std::vector<double> sd(size_t(c.slices), 0), sr(size_t(c.slices), 0);
    for (int t = 0; t < n_tokens; ++t) {
        double tgg = 0, trr = 0, tgr = 0;
        for (int j = 0; j < n_embd_inp; ++j) {
            const size_t i = size_t(t) * size_t(n_embd_inp) + size_t(j);
            const double g = gpu[i], r = ref[i];
            if (!std::isfinite(g)) { ++c.nonfinite; continue; }
            const double d = g - r;
            c.max_abs = std::max(c.max_abs, std::fabs(d));
            c.ref_absmax = std::max(c.ref_absmax, std::fabs(r));
            dd += d * d; rr += r * r; gg += g * g; gr += g * r;
            tgg += g * g; trr += r * r; tgr += g * r;
            const int s = std::min(c.slices - 1, j / std::max(1, n_embd));
            sd[size_t(s)] += d * d; sr[size_t(s)] += r * r;
        }
        const double tc = tgg > 0 && trr > 0 ? tgr / std::sqrt(tgg * trr) : 0;
        if (tc < c.cos_min_token) { c.cos_min_token = tc; c.worst_token = t; }
        if (tc < PS5LM_VISION_TOL_TOKEN_COS) ++c.tokens_below;
    }
    c.rel_l2 = rr > 0 ? std::sqrt(dd / rr) : 0;
    c.cos = gg > 0 && rr > 0 ? gr / std::sqrt(gg * rr) : 0;
    for (int s = 0; s < c.slices; ++s) c.slice_rel_l2[s] = sr[size_t(s)] > 0 ? std::sqrt(sd[size_t(s)] / sr[size_t(s)]) : 0;
    return c;
}

static std::string cmp_json(const embd_cmp &c)
{
    char buf[768];
    std::string slices;
    for (int s = 0; s < c.slices; ++s) {
        char x[32];
        std::snprintf(x, sizeof x, "%s%.6g", s ? "," : "", c.slice_rel_l2[s]);
        slices += x;
    }
    std::snprintf(buf, sizeof buf, "{\"max_abs\":%.6g,\"ref_absmax\":%.6g,\"max_abs_rel_to_ref_max\":%.6g,"
                  "\"rel_l2\":%.6g,\"cosine\":%.9f,\"cosine_min_token\":%.9f,\"worst_token\":%d,"
                  "\"tokens_below_token_cos\":%u,\"nonfinite\":%u,\"slice_rel_l2\":[%s]}",
                  c.max_abs, c.ref_absmax, c.ref_absmax > 0 ? c.max_abs / c.ref_absmax : 0.0, c.rel_l2, c.cos,
                  c.cos_min_token, c.worst_token, c.tokens_below, c.nonfinite, slices.c_str());
    return buf;
}

static bool embd_within(const embd_cmp &c)
{
    return c.nonfinite == 0 && c.rel_l2 <= PS5LM_VISION_TOL_REL_L2 && c.cos >= PS5LM_VISION_TOL_COS &&
           c.tokens_below <= PS5LM_VISION_TOL_TOKENS_BELOW;
}

/* ---- The run ---- */
struct run {
    std::vector<step> steps;
    std::string text;
    std::vector<llama_token> prompt_ids;   // text tokens, in order (image tokens excluded)
    int prompt_tokens = 0, image_tokens = 0, image_nx = 0, image_ny = 0, n_embd_inp = 0, n_embd = 0;
    int image_pos = 0;
    unsigned nonfinite = 0;
    bool eog = false;
    std::vector<float> embd;
    double load_ms = 0, mmproj_ms = 0, context_ms = 0, encode_ms = 0, image_decode_ms = 0, text_ms = 0,
           generate_ms = 0;
    uint64_t live_start = 0, live_model = 0, live_mmproj = 0, live_context = 0, model_bytes = 0;
    int n_layer = 0;
    unsigned long long vk_peak = 0;
    unsigned submissions = 0;
    int trace_pass = 0;
};

static llama_model *g_model;
static llama_context *g_ctx;
static mtmd_context *g_mctx;
static mtmd_input_chunks *g_chunks;

static int generate(run &r)
{
    llama_log_set(log_line, nullptr);
    mtmd_helper_log_set(log_line, nullptr);
    llama_backend_init();
    for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        std::printf("PS5LM_MODEL_DEVICE {\"index\":%zu,\"name\":\"%s\",\"type\":%d}\n", i,
                    ggml_backend_dev_name(dev), int(ggml_backend_dev_type(dev)));
    }
#if defined(PS5LM_HOST_REFERENCE)
    const bool gpu = host_gpu;
    const char *model_path = host_model_path, *mmproj_path = host_mmproj_path;
    const unsigned char *png = host_png.data();
    const size_t png_bytes = host_png.size();
    const int threads = gpu ? PS5LM_MODEL_THREADS : 8;   // CPU results do not depend on the thread count
#else
    const bool gpu = true;
    const char *model_path = PS5LM_MODEL_PATH, *mmproj_path = PS5LM_MMPROJ_PATH;
    const unsigned char *png = ps5lm_vision_png;
    const size_t png_bytes = sizeof ps5lm_vision_png;
    const int threads = PS5LM_MODEL_THREADS;
#endif
    /* 1. Text model, all layers and the output on the GPU, no mmap. */
    r.live_start = direct_live();
    phase("load_begin");
    flush_output();
    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = gpu ? 999 : 0;
    mp.load_mode = LLAMA_LOAD_MODE_NONE;
    mp.lazy_mode = LLAMA_LAZY_MODE_OFF;
    mp.use_extra_bufts = false;
    mp.check_tensors = false;
    mp.load_mtp = false;
    auto t = std::chrono::steady_clock::now();
    g_model = llama_model_load_from_file(model_path, mp);
    r.load_ms = ms_since(t);
    phase("load_end");
    if (!g_model) fatal("model_load");
    r.live_model = direct_live();
    r.model_bytes = llama_model_size(g_model);
    r.n_layer = llama_model_n_layer(g_model);
    r.n_embd = llama_model_n_embd(g_model);
    r.n_embd_inp = llama_model_n_embd_inp(g_model);
    std::printf("PS5LM_MODEL_LOADED {\"load_ms\":%.1f,\"model_size_bytes\":%llu,\"n_layer\":%d,\"n_embd\":%d,"
                "\"n_embd_inp\":%d,\"direct_live_bytes\":%llu,\"direct_peak_bytes\":%llu,\"vk_live_bytes\":%llu,"
                "\"vk_allocations\":%u,\"submissions\":%u}\n", r.load_ms, (unsigned long long)r.model_bytes,
                r.n_layer, r.n_embd, r.n_embd_inp, (unsigned long long)r.live_model,
                (unsigned long long)direct_peak(), vk_live(), vk_allocations(), submissions());
    if (r.live_model - r.live_start > PS5LM_MODEL_WEIGHTS_MIB * MiB) fatal("model_weights_budget");
#if !defined(PS5LM_HOST_REFERENCE)
    if (placement.offloaded < 0 || placement.offloaded != placement.offload_total ||
        placement.offload_total != r.n_layer + 1)
        fatal("model_not_fully_offloaded");
#endif
    flush_output();

    /* 2. The mmproj (vision encoder + merger), on the GPU, image tokens capped. */
    mtmd_context_params vp = mtmd_context_params_default();
    vp.use_gpu = gpu;
    vp.print_timings = true;
    vp.n_threads = threads;
    vp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
    vp.warmup = true;                          // reserves the compute buffers at the capped image size
    vp.image_min_tokens = PS5LM_VISION_MIN_TOKENS;
    vp.image_max_tokens = PS5LM_VISION_MAX_TOKENS;
    vp.cb_eval = trace_cb;
    vp.cb_eval_user_data = nullptr;
    vp.batch_max_tokens = PS5LM_VISION_MAX_TOKENS;
    t = std::chrono::steady_clock::now();
    phase("mmproj_begin");
    g_mctx = mtmd_init_from_file(mmproj_path, g_model, vp);
    r.mmproj_ms = ms_since(t);
    phase("mmproj_end");
    ps5lm_report_allocations();
    if (!g_mctx) fatal("mmproj_load");
    if (!mtmd_support_vision(g_mctx)) fatal("mmproj_no_vision");
    r.live_mmproj = direct_live();
    std::printf("PS5LM_VISION_MMPROJ {\"mmproj_ms\":%.1f,\"direct_live_bytes\":%llu,\"direct_peak_bytes\":%llu,"
                "\"vk_live_bytes\":%llu,\"mrope\":%s,\"clip_splits\":%d,\"clip_nodes\":%d,"
                "\"clip_unsupported_warnings\":%u}\n", r.mmproj_ms, (unsigned long long)r.live_mmproj,
                (unsigned long long)direct_peak(), vk_live(), mtmd_decode_use_mrope(g_mctx) ? "true" : "false",
                placement.clip_splits, placement.clip_nodes, placement.clip_unsupported);
    if (r.live_mmproj - r.live_model > PS5LM_VISION_MMPROJ_MIB * MiB) fatal("mmproj_budget");
    flush_output();

    /* 3. One context, the model mode's settings. */
    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = PS5LM_MODEL_CTX;
    cp.n_batch = PS5LM_MODEL_BATCH;
    cp.n_ubatch = PS5LM_MODEL_BATCH;
    cp.n_seq_max = 1;
    cp.n_threads = threads;
    cp.n_threads_batch = threads;
    cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
    cp.type_k = GGML_TYPE_F16;
    cp.type_v = GGML_TYPE_F16;
    cp.offload_kqv = true;
    cp.op_offload = false;
    cp.no_perf = false;
    t = std::chrono::steady_clock::now();
    g_ctx = llama_init_from_model(g_model, cp);
    r.context_ms = ms_since(t);
    phase("context_end");
    ps5lm_report_allocations();
    if (!g_ctx) fatal("model_context");
    r.live_context = direct_live();
    std::printf("PS5LM_MODEL_CONTEXT {\"context_ms\":%.1f,\"n_ctx\":%u,\"direct_live_bytes\":%llu,"
                "\"direct_peak_bytes\":%llu,\"vk_live_bytes\":%llu,\"vk_allocations\":%u}\n",
                r.context_ms, llama_n_ctx(g_ctx), (unsigned long long)r.live_context,
                (unsigned long long)direct_peak(), vk_live(), vk_allocations());
    if (r.live_context - r.live_mmproj > PS5LM_MODEL_CONTEXT_MIB * MiB) fatal("model_context_budget");
    flush_output();

    /* 4. The image and the prompt, as chunks: text, image, text. */
    mtmd_helper_bitmap_wrapper bw = mtmd_helper_bitmap_init_from_buf(g_mctx, png, png_bytes, false,
                                                                     mtmd_helper_init_opt_default());
    if (!bw.bitmap) fatal("vision_image_decode");
    r.image_nx = int(mtmd_bitmap_get_nx(bw.bitmap));
    r.image_ny = int(mtmd_bitmap_get_ny(bw.bitmap));
    if (r.image_nx != PS5LM_VISION_IMAGE_SIDE || r.image_ny != PS5LM_VISION_IMAGE_SIDE) fatal("vision_image_size");
    g_chunks = mtmd_input_chunks_init();
    mtmd_input_text text{PS5LM_VISION_PROMPT, std::strlen(PS5LM_VISION_PROMPT), true, true};
    const mtmd_bitmap *bitmaps[] = {bw.bitmap};
    if (mtmd_tokenize(g_mctx, g_chunks, &text, bitmaps, 1) != 0) fatal("vision_tokenize");
    mtmd_bitmap_free(bw.bitmap);
    const size_t n_chunks = mtmd_input_chunks_size(g_chunks);
    const mtmd_input_chunk *image = nullptr;
    for (size_t i = 0; i < n_chunks; ++i) {
        const mtmd_input_chunk *c = mtmd_input_chunks_get(g_chunks, i);
        if (mtmd_input_chunk_get_type(c) == MTMD_INPUT_CHUNK_TYPE_IMAGE) {
            if (image) fatal("vision_two_images");
            image = c;
            r.image_tokens = int(mtmd_input_chunk_get_n_tokens(c));
            r.image_pos = int(mtmd_input_chunk_get_n_pos(c));
        } else {
            size_t n = 0;
            const llama_token *ids = mtmd_input_chunk_get_tokens_text(c, &n);
            r.prompt_ids.insert(r.prompt_ids.end(), ids, ids + n);
        }
    }
    if (!image || r.image_tokens <= 0 || r.image_tokens > PS5LM_VISION_MAX_TOKENS) fatal("vision_image_tokens");
    r.prompt_tokens = int(r.prompt_ids.size());
    std::printf("PS5LM_VISION_PROMPT {\"chunks\":%zu,\"text_tokens\":%d,\"image_tokens\":%d,\"image_pos\":%d,"
                "\"image\":[%d,%d],\"ids\":[", n_chunks, r.prompt_tokens, r.image_tokens, r.image_pos,
                r.image_nx, r.image_ny);
    for (size_t i = 0; i < r.prompt_ids.size(); ++i) std::printf("%s%d", i ? "," : "", r.prompt_ids[i]);
    std::printf("]}\n");
    flush_output();

    /* 5. Encode the image (timed, no trace), keep the embedding. */
    phase("encode_begin");
    t = std::chrono::steady_clock::now();
    if (mtmd_encode_chunk(g_mctx, image) != 0) fatal("vision_encode");
    r.encode_ms = ms_since(t);
    phase("encode_end");
    const size_t n_floats = size_t(r.image_tokens) * size_t(r.n_embd_inp);
    const float *out = mtmd_get_output_embd(g_mctx);
    if (!out) fatal("vision_no_embedding");
    r.embd.assign(out, out + n_floats);
    return 0;
}

/* After the embedding check: decode the prompt (text, image embedding, text), then 64 greedy tokens. */
static void decode_and_generate(run &r)
{
    const llama_vocab *vocab = llama_model_get_vocab(g_model);
    const int n_vocab = llama_vocab_n_tokens(vocab);
    llama_pos n_past = 0;
    const size_t n_chunks = mtmd_input_chunks_size(g_chunks);
    auto t = std::chrono::steady_clock::now();
    for (size_t i = 0; i < n_chunks; ++i) {
        const mtmd_input_chunk *c = mtmd_input_chunks_get(g_chunks, i);
        const bool last = i + 1 == n_chunks;
        llama_pos next = n_past;
        if (mtmd_input_chunk_get_type(c) == MTMD_INPUT_CHUNK_TYPE_IMAGE) {
            const auto ti = std::chrono::steady_clock::now();
            if (mtmd_helper_decode_image_chunk(g_mctx, g_ctx, c, r.embd.data(), n_past, 0, PS5LM_MODEL_BATCH,
                                               &next, nullptr, nullptr) != 0)
                fatal("vision_image_decode_batch");
            r.image_decode_ms += ms_since(ti);
        } else {
            if (mtmd_helper_eval_chunk_single(g_mctx, g_ctx, c, n_past, 0, PS5LM_MODEL_BATCH, last, &next) != 0)
                fatal("vision_text_decode");
        }
        n_past = next;
    }
    r.text_ms = ms_since(t) - r.image_decode_ms;
    phase("prompt_end");
    llama_batch batch = llama_batch_init(1, 0, 1);
    t = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < PS5LM_MODEL_PREDICT; ++i) {
        const float *logits = llama_get_logits_ith(g_ctx, -1);
        if (!logits) fatal("model_logits");
        const step s = rank(logits, n_vocab);
        r.nonfinite += s.nonfinite;
        if (s.top1 < 0 || s.top2 < 0) fatal("model_no_finite_logits");
        r.steps.push_back(s);
        const std::string piece_text = piece(vocab, s.top1);
        r.text += piece_text;
        std::printf("PS5LM_MODEL_TOKEN {\"index\":%u,\"token\":%d,\"logit\":%.5f,\"second\":%d,"
                    "\"second_logit\":%.5f,\"nonfinite\":%u,\"text\":\"%s\"}\n",
                    i, s.top1, s.logit1, s.top2, s.logit2, s.nonfinite, json_text(piece_text, 32).c_str());
        if (llama_vocab_is_eog(vocab, s.top1)) { r.eog = true; break; }
        if (i + 1 == PS5LM_MODEL_PREDICT) break;
        batch.n_tokens = 1;
        batch.token[0] = s.top1;
        batch.pos[0] = n_past;
        batch.n_seq_id[0] = 1;
        batch.seq_id[0][0] = 0;
        batch.logits[0] = 1;
        if (llama_decode(g_ctx, batch) != 0) fatal("model_token_decode");
        ++n_past;
    }
    r.generate_ms = ms_since(t);
    phase("generate_end");
    ps5lm_report_allocations();
    r.vk_peak = vk_peak();
    r.submissions = submissions();
    llama_perf_context_print(g_ctx);
    llama_batch_free(batch);
}

static void free_all()
{
    mtmd_input_chunks_free(g_chunks);
    mtmd_free(g_mctx);
    llama_free(g_ctx);
    llama_model_free(g_model);
    llama_backend_free();
    phase("freed");
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

/* The checkable answer: the word in the image, case-insensitive. */
static bool answer_ok(const std::string &text)
{
    std::string up;
    for (char c : text) up += char(c >= 'a' && c <= 'z' ? c - 32 : c);
    return up.find(PS5LM_VISION_ANSWER) != std::string::npos;
}

/* Re-encode with the trace on; the samples go to trace_items. */
static void trace_encode(const mtmd_input_chunk *image)
{
    trace_items.clear();
    trace_on = true;
    if (mtmd_encode_chunk(g_mctx, image) != 0) fatal("vision_trace_encode");
    trace_on = false;
}

static const mtmd_input_chunk *image_chunk()
{
    for (size_t i = 0; i < mtmd_input_chunks_size(g_chunks); ++i) {
        const mtmd_input_chunk *c = mtmd_input_chunks_get(g_chunks, i);
        if (mtmd_input_chunk_get_type(c) == MTMD_INPUT_CHUNK_TYPE_IMAGE) return c;
    }
    return nullptr;
}

static std::string ids_json(const std::vector<step> &steps)
{
    std::string ids;
    for (size_t i = 0; i < steps.size(); ++i) ids += (i ? "," : "") + std::to_string(steps[i].top1);
    return ids;
}

#if defined(PS5LM_HOST_REFERENCE)
static bool read_file(const char *path, std::vector<unsigned char> &out)
{
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    unsigned char buf[1 << 16];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return true;
}

static bool write_file(const std::string &path, const void *data, size_t bytes)
{
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data, 1, bytes, f) == bytes;
    return std::fclose(f) == 0 && ok;
}

/* usage: ref  MODEL MMPROJ PNG OUTDIR          CPU reference, writes OUTDIR/{embd.bin,trace.bin,
 *                                              model-reference.h,vision-reference.h,reference.json}
 *        gpu  MODEL MMPROJ PNG REFDIR OUTDIR   Metal stand-in, compares with REFDIR */
int main(int argc, char **argv)
{
    if (argc < 6 || (std::strcmp(argv[1], "ref") != 0 && std::strcmp(argv[1], "gpu") != 0) ||
        (std::strcmp(argv[1], "gpu") == 0 && argc != 7)) {
        std::fprintf(stderr, "usage: %s ref MODEL MMPROJ PNG OUTDIR | gpu MODEL MMPROJ PNG REFDIR OUTDIR\n", argv[0]);
        return 64;
    }
    host_gpu = std::strcmp(argv[1], "gpu") == 0;
    host_model_path = argv[2];
    host_mmproj_path = argv[3];
    if (!read_file(argv[4], host_png)) fatal("read_png");
    const std::string outdir = argv[host_gpu ? 6 : 5];
    run r;
    generate(r);
    const mtmd_input_chunk *image = image_chunk();
    trace_encode(image);   // CPU: the reference samples; Metal: compared below
    decode_and_generate(r);
    const unsigned n = unsigned(r.steps.size());
    if (!write_file(outdir + "/embd.bin", r.embd.data(), r.embd.size() * sizeof(float))) fatal("write_embd");
    std::vector<float> trace_flat;
    std::string trace_table;
    for (const trace_item &it : trace_items) {
        char row[160];
        std::snprintf(row, sizeof row, "    {\"%s\", %lld, %zu, %zu, %.9g},\n", it.name.c_str(),
                      (long long)it.elements, it.samples.size(), trace_flat.size(), it.sumsq);
        trace_table += row;
        trace_flat.insert(trace_flat.end(), it.samples.begin(), it.samples.end());
    }
    if (!write_file(outdir + "/trace.bin", trace_flat.data(), trace_flat.size() * sizeof(float))) fatal("write_trace");
    std::string summary;
    if (!host_gpu) {
        if (r.nonfinite || n == 0) fatal("reference_incomplete");
        FILE *h = std::fopen((outdir + "/model-reference.h").c_str(), "w");
        if (!h) fatal("reference_write");
        std::fprintf(h, "/* Generated by the host CPU reference (ggml-vision.cpp); do not edit.\n"
                        " * Text: %s */\n", json_text(r.text, 600).c_str());
        std::fprintf(h, "#define PS5LM_REFERENCE_PROMPT_TOKENS %d\n#define PS5LM_REFERENCE_GENERATED %u\n",
                     r.prompt_tokens, n);
        std::fprintf(h, "static const int ps5lm_reference_tokens[%u] = {", n);
        for (unsigned i = 0; i < n; ++i) std::fprintf(h, "%s%d", i ? "," : "", r.steps[i].top1);
        std::fprintf(h, "};\nstatic const float ps5lm_reference_gaps[%u] = {", n);
        for (unsigned i = 0; i < n; ++i)
            std::fprintf(h, "%s%.9gf", i ? "," : "", double(r.steps[i].logit1 - r.steps[i].logit2));
        std::fprintf(h, "};\n");
        std::fclose(h);
        h = std::fopen((outdir + "/vision-reference.h").c_str(), "w");
        if (!h) fatal("reference_write");
        std::fprintf(h, "/* Generated by the host CPU reference (ggml-vision.cpp); do not edit. */\n"
                        "#define PS5LM_REFERENCE_IMAGE_TOKENS %d\n#define PS5LM_REFERENCE_N_EMBD_INP %d\n"
                        "#define PS5LM_REFERENCE_TRACE_FLOATS %zu\n"
                        "struct ps5lm_trace_ref { const char *name; long long elements; unsigned long samples, "
                        "offset; double sumsq; };\nstatic const struct ps5lm_trace_ref ps5lm_trace_refs[] = {\n%s};\n",
                        r.image_tokens, r.n_embd_inp, trace_flat.size(), trace_table.c_str());
        std::fclose(h);
    } else {
        std::vector<unsigned char> ref_embd, ref_trace;
        if (!read_file((std::string(argv[5]) + "/embd.bin").c_str(), ref_embd) ||
            ref_embd.size() != r.embd.size() * sizeof(float))
            fatal("read_ref_embd");
        const embd_cmp c = compare_embd(r.embd.data(), reinterpret_cast<const float *>(ref_embd.data()),
                                        r.image_tokens, r.n_embd_inp, r.n_embd);
        summary = "PS5LM_VISION_EMBD_CHECK " + cmp_json(c) + (embd_within(c) ? " within\n" : " OUTSIDE\n");
        std::fputs(summary.c_str(), stdout);
    }
    char line[4096];
    std::snprintf(line, sizeof line,
        "PS5LM_VISION_REFERENCE {\"backend\":\"%s\",\"generated\":%u,\"longest_repeat\":%u,\"answer_ok\":%s,"
        "\"prompt_text_tokens\":%d,\"image_tokens\":%d,\"image_pos\":%d,\"n_embd_inp\":%d,\"traced\":%zu,"
        "\"load_ms\":%.1f,\"mmproj_ms\":%.1f,\"encode_ms\":%.1f,\"image_decode_ms\":%.1f,\"text_ms\":%.1f,"
        "\"generate_ms\":%.1f,\"offloaded\":%d,\"clip_unsupported_warnings\":%u,\"tokens\":[%s],\"text\":\"%s\"}\n",
        host_gpu ? "metal" : "cpu", n, longest_repeat(r.steps), answer_ok(r.text) ? "true" : "false",
        r.prompt_tokens, r.image_tokens, r.image_pos, r.n_embd_inp, trace_items.size(), r.load_ms, r.mmproj_ms,
        r.encode_ms, r.image_decode_ms, r.text_ms, r.generate_ms, placement.offloaded, placement.clip_unsupported,
        ids_json(r.steps).c_str(), json_text(r.text, 1500).c_str());
    std::fputs(line, stdout);
    std::string gaps = "PS5LM_VISION_GAPS [";
    for (unsigned i = 0; i < n; ++i) {
        char g[32];
        std::snprintf(g, sizeof g, "%s%.5f", i ? "," : "", double(r.steps[i].logit1 - r.steps[i].logit2));
        gaps += g;
    }
    std::fputs((gaps + "]\n").c_str(), stdout);
    free_all();
    return 0;
}
#else
static_assert(PS5LM_REFERENCE_GENERATED <= PS5LM_MODEL_PREDICT, "Reference length");

/* Strided samples of each traced tensor against the CPU's: relative L2 per tensor. */
static void trace_report()
{
    const float *ref = ps5lm_vision_trace_ref;
    const size_t ref_floats = size_t(ps5lm_vision_trace_ref_end - ps5lm_vision_trace_ref);
    std::string json = "PS5LM_VISION_TRACE {\"items\":[";
    int first_bad = -1;
    for (size_t k = 0; k < trace_items.size(); ++k) {
        const trace_item &it = trace_items[k];
        const ps5lm_trace_ref *r = nullptr;
        for (const auto &x : ps5lm_trace_refs)
            if (it.name == x.name) { r = &x; break; }
        double dd = 0, rr = 0, ma = 0;
        bool shape = r && r->elements == it.elements && r->samples == it.samples.size() &&
                     r->offset + r->samples <= ref_floats;
        if (shape)
            for (size_t i = 0; i < it.samples.size(); ++i) {
                const double d = double(it.samples[i]) - ref[r->offset + i];
                dd += d * d; rr += double(ref[r->offset + i]) * ref[r->offset + i];
                ma = std::max(ma, std::fabs(d));
            }
        const double rel = shape && rr > 0 ? std::sqrt(dd / rr) : -1;
        const double norm_ratio = shape && r->sumsq > 0 ? std::sqrt(it.sumsq / r->sumsq) : -1;
        if (first_bad < 0 && (rel < 0 || rel > PS5LM_VISION_TOL_REL_L2)) first_bad = int(k);
        char item[256];
        std::snprintf(item, sizeof item, "%s{\"name\":\"%s\",\"rel_l2\":%.6g,\"max_abs\":%.6g,\"norm_ratio\":%.6f}",
                      k ? "," : "", it.name.c_str(), rel, ma, norm_ratio);
        json += item;
    }
    char tail[160];
    std::snprintf(tail, sizeof tail, "],\"first_over_tolerance\":\"%s\"}\n",
                  first_bad >= 0 ? trace_items[size_t(first_bad)].name.c_str() : "");
    json += tail;
    std::fputs(json.c_str(), stdout);
    klog(tail);
}

extern "C" int ps5lm_run_ggml_ops(void)
{
    try {
        run r;
        generate(r);
        if (r.prompt_tokens != PS5LM_REFERENCE_PROMPT_TOKENS) fatal("model_prompt_mismatch");
        if (r.image_tokens != PS5LM_REFERENCE_IMAGE_TOKENS || r.n_embd_inp != PS5LM_REFERENCE_N_EMBD_INP ||
            size_t(ps5lm_vision_embd_ref_end - ps5lm_vision_embd_ref) != r.embd.size())
            fatal("vision_reference_shape");
        const embd_cmp c = compare_embd(r.embd.data(), ps5lm_vision_embd_ref, r.image_tokens, r.n_embd_inp, r.n_embd);
        const bool embd_ok = embd_within(c);
        const std::string cj = cmp_json(c);
        std::printf("PS5LM_VISION_EMBD_CHECK %s\n", cj.c_str());
        flush_output();
        if (!embd_ok) {
            /* Find the first encoder tensor that leaves the tolerance (one more encode). */
            phase("trace_begin");
            trace_encode(image_chunk());
            r.trace_pass = 1;
            phase("trace_end");
            trace_report();
            flush_output();
        }
        decode_and_generate(r);
        const unsigned n = unsigned(r.steps.size());
        const unsigned nref = PS5LM_REFERENCE_GENERATED;
        int first_diff = -1;
        for (unsigned i = 0; i < std::min(n, nref) && first_diff < 0; ++i)
            if (r.steps[i].top1 != ps5lm_reference_tokens[i]) first_diff = int(i);
        if (first_diff < 0 && n != nref) first_diff = int(std::min(n, nref));
        const unsigned repeat = longest_repeat(r.steps);
        const char *rule = "fail";
        if (first_diff < 0) rule = "identical";
        else if (unsigned(first_diff) >= PS5LM_MODEL_PREFIX) rule = "prefix";
        else if (unsigned(first_diff) < nref && ps5lm_reference_gaps[first_diff] <= PS5LM_MODEL_TIE_GAP) rule = "near_tie";
        const bool answer = answer_ok(r.text);
        const bool sane = !r.nonfinite && repeat < 8 && placement.errors == 0 && vk_errors() == 0;
        const bool pass = std::strcmp(rule, "fail") != 0 && sane && answer && embd_ok;
        unsigned matched = 0;
        while (matched < std::min(n, nref) && r.steps[matched].top1 == ps5lm_reference_tokens[matched]) ++matched;
        const double tg = r.generate_ms > 0 && n > 1 ? (n - 1) * 1000.0 / r.generate_ms : 0;
        const double image_tok_s = r.encode_ms + r.image_decode_ms > 0 ?
                                   r.image_tokens * 1000.0 / (r.encode_ms + r.image_decode_ms) : 0;
        char line[4096];
        std::snprintf(line, sizeof line,
            "PS5LM_MODEL_SUMMARY {\"pass\":%s,\"rule\":\"%s\",\"answer_ok\":%s,\"embd_ok\":%s,\"embd\":%s,"
            "\"trace_pass\":%d,\"generated\":%u,\"reference_generated\":%u,\"matched_prefix\":%u,\"first_diff\":%d,"
            "\"reference_gap_at_diff\":%.6f,\"nonfinite\":%u,\"longest_repeat\":%u,\"eog\":%s,\"offloaded\":%d,"
            "\"offload_total\":%d,\"splits_batch\":%d,\"splits_single\":%d,\"clip_splits\":%d,\"clip_nodes\":%d,"
            "\"clip_unsupported_warnings\":%u,\"host_model_buffer_mib\":%.2f,\"weights_moved_lines\":%u,"
            "\"log_warnings\":%u,\"log_errors\":%u,\"model_size_bytes\":%llu,\"weights_bytes\":%llu,"
            "\"mmproj_bytes\":%llu,\"context_bytes\":%llu,\"direct_peak_bytes\":%llu,\"vk_peak_bytes\":%llu,"
            "\"vk_live_after_free\":%llu,\"vk_errors\":%u,\"submissions\":%u,\"load_ms\":%.1f,\"mmproj_ms\":%.1f,"
            "\"context_ms\":%.1f,\"image_tokens\":%d,\"text_tokens\":%d,\"encode_ms\":%.1f,\"image_decode_ms\":%.1f,"
            "\"image_tok_s\":%.2f,\"text_ms\":%.1f,\"generate_ms\":%.1f,\"gen_tok_s\":%.2f,\"tokens\":[%s]}\n",
            pass ? "true" : "false", rule, answer ? "true" : "false", embd_ok ? "true" : "false", cj.c_str(),
            r.trace_pass, n, nref, matched, first_diff,
            first_diff >= 0 && unsigned(first_diff) < nref ? double(ps5lm_reference_gaps[first_diff]) : -1.0,
            r.nonfinite, repeat, r.eog ? "true" : "false", placement.offloaded, placement.offload_total,
            placement.splits_batch, placement.splits_single, placement.clip_splits, placement.clip_nodes,
            placement.clip_unsupported, placement.host_model_mib, placement.moved, placement.warnings,
            placement.errors, (unsigned long long)r.model_bytes,
            (unsigned long long)(r.live_model - r.live_start), (unsigned long long)(r.live_mmproj - r.live_model),
            (unsigned long long)(r.live_context - r.live_mmproj), (unsigned long long)direct_peak(), r.vk_peak,
            0ull, vk_errors(), r.submissions, r.load_ms, r.mmproj_ms, r.context_ms, r.image_tokens,
            r.prompt_tokens, r.encode_ms, r.image_decode_ms, image_tok_s, r.text_ms, r.generate_ms, tg,
            ids_json(r.steps).c_str());
        free_all();
        /* vk_live after the frees, patched into the line. */
        std::string summary = line;
        const std::string key = "\"vk_live_after_free\":0,";
        const size_t at = summary.find(key);
        if (at != std::string::npos)
            summary.replace(at, key.size(), "\"vk_live_after_free\":" + std::to_string(vk_live()) + ",");
        std::fputs(summary.c_str(), stdout);
        klog(summary.c_str());
        std::printf("PS5LM_MODEL_TEXT {\"text\":\"%s\"}\n", json_text(r.text, 1500).c_str());
        flush_output();
        phase("flushed");
        ps5lm_ops_checked(PS5LM_MODEL_PREDICT, pass ? 0 : 1);
        return pass ? 0 : 6;
    } catch (const std::exception &) {
        fatal("model_exception");
    } catch (...) {
        fatal("model_unknown_exception");
    }
}
#endif
