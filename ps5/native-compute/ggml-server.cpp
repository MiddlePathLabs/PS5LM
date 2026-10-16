/* SPDX-License-Identifier: GPL-3.0-or-later
 * Server mode: llama.cpp's server (OpenAI-compatible API on port 8081) inside
 * PPSA99041 for one bounded serving window, then a clean stop, one result
 * line and the title's usual close.
 *
 * The server runs llama_server() (tools/server/server.cpp, same pin) on its own
 * thread. This file keeps the title's controls around it:
 *  - one model, the pinned file, loaded and given a context exactly once, with
 *    the model mode's settings, through link-time wraps of
 *    llama_model_load_from_file and llama_init_from_model that also enforce
 *    the weights and context caps (ggml-model.h) as the model mode does;
 *  - the main thread owns the lifetime: it stops the server when the window
 *    ends or POST /ps5lm/shutdown arrives (patches/server/0001), and closes
 *    the title through main.c if loading or stopping takes too long;
 *  - every API response is counted (status >= 400 is an error).
 *
 * With PS5LM_HOST_REHEARSAL it builds for the Mac (scripts/host-server-rehearsal.sh):
 * the title's functions become simple stand-ins, and since the Mac linker has
 * no --wrap, the load and context settings are checked on the parsed arguments.
 */
#include "ggml.h"
#include "llama.h"
#include "ggml-model.h"
#include "ggml-server.h"
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <string>
#include <vector>

#if defined(PS5LM_HOST_REHEARSAL)
#include "arg.h"
#include "common.h"
#include <chrono>
#include <cstdlib>
static const auto host_start = std::chrono::steady_clock::now();
[[noreturn]] static void ps5lm_ops_fatal(const char *stage)
{
    std::fprintf(stderr, "PS5LM_HOST_FATAL %s\n", stage);
    std::fflush(nullptr);
    std::_Exit(3);
}
static void ps5lm_ops_checked(unsigned, unsigned) {}
static uint64_t ps5lm_direct_live_bytes(void) { return 0; }
static uint64_t ps5lm_direct_peak_bytes(void) { return 0; }
static int sceKernelDebugOutText(int, const char *text) { return std::fputs(text, stderr); }
static unsigned long long ps5lm_vk_memory_peak_bytes(void) { return 0; }
static unsigned long long ps5lm_vk_memory_live_bytes(void) { return 0; }
static unsigned ps5lm_vk_memory_errors(void) { return 0; }
static unsigned ps5lm_submission_count(void) { return 0; }
static void ps5lm_flush_output(void) { std::fflush(stdout); }
[[maybe_unused]] static void ps5lm_report_allocations(void) {}
static uint64_t ps5lm_elapsed_ms(void)
{
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - host_start).count());
}
#define MODEL_PATH PS5LM_HOST_MODEL_PATH
#if defined(PS5LM_MODEL_VISION)
#define MMPROJ_PATH PS5LM_HOST_MMPROJ_PATH
#endif
/* No Vulkan on the Mac: the knobs are kept and reported, nothing reads them. */
static std::atomic<long long> host_knobs[6] = {-1, -1, -1, -1, -1, -1};
static const char *const host_knob_names[6] = {"disable_fusion", "disable_graph_optimize", "nodes_per_submit",
                                               "submit_mflop", "dmmv_wg", "disable_mmvq"};
static int ps5lm_vk_set_knob(const char *name, long long value)
{
    for (int i = 0; i < 6; ++i)
        if (std::strcmp(name, host_knob_names[i]) == 0) { host_knobs[i].store(value); return 0; }
    return -1;
}
static void ps5lm_vk_perf_reset_max(void) {}
static int ps5lm_vk_perf_json(char *out, size_t size)
{
    return std::snprintf(out, size, "{\"host_rehearsal\":true,\"knobs\":{\"disable_fusion\":%lld,"
                         "\"disable_graph_optimize\":%lld,\"nodes_per_submit\":%lld,\"submit_mflop\":%lld,"
                         "\"dmmv_wg\":%lld,\"disable_mmvq\":%lld}}", host_knobs[0].load(), host_knobs[1].load(),
                         host_knobs[2].load(), host_knobs[3].load(), host_knobs[4].load(), host_knobs[5].load());
}
#else
extern "C" [[noreturn]] void ps5lm_ops_fatal(const char *stage);
extern "C" void ps5lm_ops_checked(unsigned count, unsigned errors);
extern "C" uint64_t ps5lm_direct_live_bytes(void);
extern "C" uint64_t ps5lm_direct_peak_bytes(void);
extern "C" int sceKernelDebugOutText(int channel, const char *text);
extern "C" unsigned long long ps5lm_vk_memory_peak_bytes(void);
extern "C" unsigned long long ps5lm_vk_memory_live_bytes(void);
extern "C" unsigned ps5lm_vk_memory_errors(void);
extern "C" unsigned ps5lm_submission_count(void);
extern "C" void ps5lm_flush_output(void);
extern "C" void ps5lm_report_allocations(void);
extern "C" uint64_t ps5lm_elapsed_ms(void);
extern "C" int ps5lm_vk_set_knob(const char *name, long long value);
extern "C" int ps5lm_vk_perf_json(char *out, size_t size);
extern "C" void ps5lm_vk_perf_reset_max(void);
#define MODEL_PATH PS5LM_MODEL_PATH
#if defined(PS5LM_MODEL_VISION)
#define MMPROJ_PATH PS5LM_MMPROJ_PATH
#endif
#endif
#if defined(PS5LM_MODEL_VISION)
#include "mtmd.h"
#endif

int llama_server(int argc, char **argv);
void llama_server_terminate();

[[maybe_unused]] static constexpr uint64_t MiB = 1048576;

static std::atomic<unsigned> responses{0}, errors{0};
[[maybe_unused]] static std::atomic<unsigned> model_loads{0}, contexts{0};
static std::atomic<bool> shutdown_requested{false}, ready{false}, server_done{false};
static std::atomic<int> server_rc{-1};
static std::atomic<uint64_t> ready_ms{0}, served_ms{0}, stop_ms{0}, live_at_ready{0};
static std::atomic<uint64_t> weights_bytes{0}, context_bytes{0}, load_ms{0};
static std::atomic<uint64_t> mmproj_bytes{0}, mmproj_ms{0};
[[maybe_unused]] static std::atomic<unsigned> mmproj_loads{0};
/* 0 serving, 1 window ended, 2 shutdown request, 3 server returned by itself */
static std::atomic<int> stop_reason{0};
static const char *const reasons[] = {"serving", "window", "shutdown_request", "server_returned"};

static void klog(const char *line) { sceKernelDebugOutText(0, line); }

static void phase(const char *name)
{
    char line[192];
    std::snprintf(line, sizeof line, "PS5LM_SERVER_PHASE {\"phase\":\"%s\",\"ms\":%llu,\"direct_live\":%llu}\n",
                  name, (unsigned long long)ps5lm_elapsed_ms(), (unsigned long long)ps5lm_direct_live_bytes());
    klog(line);
}

/* For main.c's PS5LM_COMPUTE_RESULT line; atomics only, so the watchdog may call these. */
extern "C" unsigned ps5lm_server_responses(void) { return responses.load(); }
extern "C" unsigned ps5lm_server_errors(void) { return errors.load(); }
extern "C" unsigned long long ps5lm_server_served_ms(void) { return served_ms.load(); }
extern "C" const char *ps5lm_server_stop_reason(void)
{
    const int r = stop_reason.load();
    return reasons[r >= 0 && r < 4 ? r : 0];
}

/* Hooks called by the patched server (patches/server/0001-ps5lm-server-hooks.patch). */
extern "C" void ps5lm_server_response(int status)
{
    responses.fetch_add(1);
    if (status >= 400) errors.fetch_add(1);
}

extern "C" void ps5lm_server_shutdown_request(void)
{
    if (!shutdown_requested.exchange(true)) phase("shutdown_requested");
}

/* The prompt chunk a step of the server decodes (patches/server/0003), at most
 * the context's n_batch; knobs and counters for GET/POST /ps5lm/... */
static std::atomic<int> prompt_batch{PS5LM_SERVER_PROMPT_BATCH};
static std::atomic<unsigned> knob_sets{0};

extern "C" int ps5lm_server_prompt_batch(int n_batch)
{
    const int chunk = prompt_batch.load();
    return chunk > 0 && chunk < n_batch ? chunk : n_batch;
}

extern "C" int ps5lm_server_set_knob(const char *name, long long value)
{
    if (!name) return -1;
    if (std::strcmp(name, "reset_max") == 0) {
        /* The longest submit/wait/graph counters start again (long-context runs). */
        if (value != 1) return -2;
        ps5lm_vk_perf_reset_max();
    } else if (std::strcmp(name, "prompt_batch") == 0) {
        if (value == -1) value = PS5LM_SERVER_PROMPT_BATCH;
        if (value < 1 || value > PS5LM_SERVER_BATCH) return -2;
        prompt_batch.store(int(value));
    } else {
        const int rc = ps5lm_vk_set_knob(name, value);
        if (rc != 0) return rc;
    }
    knob_sets.fetch_add(1);
    char line[160];
    std::snprintf(line, sizeof line, "PS5LM_SERVER_KNOB {\"name\":\"%.48s\",\"value\":%lld,\"ms\":%llu}\n", name,
                  value, (unsigned long long)ps5lm_elapsed_ms());
    sceKernelDebugOutText(0, line);
    return 0;
}

extern "C" int ps5lm_server_stats(char *out, size_t size)
{
    char vk[1536];
    const int n = ps5lm_vk_perf_json(vk, sizeof vk);
    if (n <= 0 || size_t(n) >= sizeof vk) return -1;
    return std::snprintf(out, size, "{\"vk\":%s,\"prompt_batch\":%d,\"context_batch\":%d,\"knob_sets\":%u,"
                         "\"responses\":%u,\"errors\":%u,\"ms\":%llu,\"direct_live\":%llu,\"direct_peak\":%llu,"
                         "\"vk_live\":%llu,\"vk_peak\":%llu}",
                         vk, prompt_batch.load(), PS5LM_SERVER_BATCH, knob_sets.load(), responses.load(),
                         errors.load(), (unsigned long long)ps5lm_elapsed_ms(),
                         (unsigned long long)ps5lm_direct_live_bytes(), (unsigned long long)ps5lm_direct_peak_bytes(),
                         ps5lm_vk_memory_live_bytes(), ps5lm_vk_memory_peak_bytes());
}

extern "C" void ps5lm_server_ready(void)
{
    live_at_ready.store(ps5lm_direct_live_bytes());
    ready_ms.store(ps5lm_elapsed_ms());
    ready.store(true, std::memory_order_release);
    phase("ready");
}

/* Layers llama.cpp loads beyond n_layer: a model's MTP ("nextn") layers, which the pin
 * offloads and counts in "offloaded N/M" but does not run (Qwen3.8 27B UD-Q2_K_XL has one). */
[[maybe_unused]] static int ps5lm_nextn_layers(const llama_model *model)
{
    char arch[64] = {0}, key[96], value[32] = {0};
    if (llama_model_meta_val_str(model, "general.architecture", arch, sizeof(arch)) <= 0) return 0;
    std::snprintf(key, sizeof(key), "%s.nextn_predict_layers", arch);
    if (llama_model_meta_val_str(model, key, value, sizeof(value)) <= 0) return 0;
    return std::atoi(value);
}

/* Placement, read from llama's own log lines on their way to the server's log,
 * as the model mode reads them: all layers offloaded, and the graph splits. */
static ggml_log_callback next_log;
static void *next_log_data;
static std::atomic<int> offloaded{-1}, offload_total{-1}, splits_batch{-1}, splits_single{-1};

[[maybe_unused]] static void log_tap(ggml_log_level level, const char *text, void *data)
{
    (void)data;
    if (level != GGML_LOG_LEVEL_CONT && text) {
        // The title's own device lines (limits, mat-vec settings) reach the output file
        // whatever the server's log verbosity, as in the model mode.
        if (std::strncmp(text, "PS5LM_VK_", 9) == 0) { std::fputs(text, stdout); std::fflush(stdout); }
        int a = -1, b = -1;
        if (const char *at = std::strstr(text, "offloaded "))
            if (std::sscanf(at, "offloaded %d/%d", &a, &b) == 2) { offloaded.store(a); offload_total.store(b); }
        if (const char *at = std::strstr(text, ", splits = ")) {
            const int n = std::sscanf(at, ", splits = %d / %d", &a, &b);
            if (n >= 1) { splits_batch.store(a); splits_single.store(n == 2 ? b : a); }
        }
    }
    if (next_log) next_log(level, text, next_log_data);
}

/* The model mode's load settings and, apart from the context size, its context settings. */
static void check_model_params(const char *path, const llama_model_params &params)
{
    if (!path || std::strcmp(path, MODEL_PATH) != 0) ps5lm_ops_fatal("server_model_path");
    if (params.load_mode != LLAMA_LOAD_MODE_NONE || params.lazy_mode != LLAMA_LAZY_MODE_OFF ||
        params.use_extra_bufts || params.check_tensors || params.load_mtp || params.n_gpu_layers < 999)
        ps5lm_ops_fatal("server_model_params");
}

static void check_context_params(const llama_context_params &params)
{
    if (params.n_ctx != PS5LM_SERVER_CTX || params.n_batch != PS5LM_SERVER_BATCH ||
        params.n_ubatch != PS5LM_SERVER_BATCH || params.n_seq_max != 1 ||
        params.n_threads != PS5LM_MODEL_THREADS || params.n_threads_batch != PS5LM_MODEL_THREADS ||
        params.flash_attn_type != LLAMA_FLASH_ATTN_TYPE_ENABLED || params.type_k != PS5LM_SERVER_KV_GGML ||
        params.type_v != PS5LM_SERVER_KV_GGML || !params.offload_kqv || params.op_offload)
        ps5lm_ops_fatal("server_context_params");
}

#if !defined(PS5LM_HOST_REHEARSAL)
/* One model, the pinned file, loaded once. */
extern "C" llama_model *__real_llama_model_load_from_file(const char *path, llama_model_params params);
extern "C" llama_model *__wrap_llama_model_load_from_file(const char *path, llama_model_params params)
{
    if (model_loads.fetch_add(1) != 0) ps5lm_ops_fatal("server_second_model_load");
    check_model_params(path, params);
    llama_log_get(&next_log, &next_log_data); // the server's (common) logger stays the sink
    llama_log_set(log_tap, nullptr);
    const uint64_t before = ps5lm_direct_live_bytes();
    const uint64_t t = ps5lm_elapsed_ms();
    phase("load_begin");
    ps5lm_flush_output();
    llama_model *model = __real_llama_model_load_from_file(path, params);
    load_ms.store(ps5lm_elapsed_ms() - t);
    phase("load_end");
    ps5lm_report_allocations();
    if (!model) ps5lm_ops_fatal("model_load");
    const uint64_t added = ps5lm_direct_live_bytes() - before;
    weights_bytes.store(added);
    if (added > PS5LM_MODEL_WEIGHTS_MIB * MiB) ps5lm_ops_fatal("model_weights_budget");
    if (offloaded.load() < 0 || offloaded.load() != offload_total.load() ||
        offload_total.load() != llama_model_n_layer(model) + ps5lm_nextn_layers(model) + 1)
        ps5lm_ops_fatal("model_not_fully_offloaded");
    return model;
}

/* One context, with the model mode's settings except the server's context size. */
extern "C" llama_context *__real_llama_init_from_model(llama_model *model, llama_context_params params);
extern "C" llama_context *__wrap_llama_init_from_model(llama_model *model, llama_context_params params)
{
    if (contexts.fetch_add(1) != 0) ps5lm_ops_fatal("server_second_context");
    check_context_params(params);
    const uint64_t before = ps5lm_direct_live_bytes();
    llama_context *ctx = __real_llama_init_from_model(model, params);
    phase("context_end");
    ps5lm_report_allocations();
    if (!ctx) ps5lm_ops_fatal("model_context");
    const uint64_t added = ps5lm_direct_live_bytes() - before;
    context_bytes.store(added);
    if (added > PS5LM_MODEL_CONTEXT_MIB * MiB) ps5lm_ops_fatal("model_context_budget");
    return ctx;
}
#endif

#if defined(PS5LM_MODEL_VISION)
/* The one mmproj load (vision builds): the pinned file, the model mode's
 * encoder settings (GPU, flash attention, warm-up, image tokens capped), and
 * its cap, measured as the model mode measures it. */
static void check_mmproj_params(const char *path, const mtmd_context_params &p)
{
    if (!path || std::strcmp(path, MMPROJ_PATH) != 0) ps5lm_ops_fatal("server_mmproj_path");
    if (!p.use_gpu || p.flash_attn_type != LLAMA_FLASH_ATTN_TYPE_ENABLED || !p.warmup ||
        p.image_min_tokens != PS5LM_VISION_MIN_TOKENS || p.image_max_tokens != PS5LM_VISION_MAX_TOKENS)
        ps5lm_ops_fatal("server_mmproj_params");
}
#if !defined(PS5LM_HOST_REHEARSAL)
extern "C" mtmd_context *__real_mtmd_init_from_file(const char *path, const llama_model *model,
                                                    mtmd_context_params params);
extern "C" mtmd_context *__wrap_mtmd_init_from_file(const char *path, const llama_model *model,
                                                    mtmd_context_params params)
{
    if (mmproj_loads.fetch_add(1) != 0) ps5lm_ops_fatal("server_second_mmproj_load");
    check_mmproj_params(path, params);
    const uint64_t before = ps5lm_direct_live_bytes();
    const uint64_t t = ps5lm_elapsed_ms();
    phase("mmproj_begin");
    ps5lm_flush_output();
    mtmd_context *ctx = __real_mtmd_init_from_file(path, model, params);
    mmproj_ms.store(ps5lm_elapsed_ms() - t);
    phase("mmproj_end");
    ps5lm_report_allocations();
    if (!ctx) ps5lm_ops_fatal("mmproj_load");
    const uint64_t added = ps5lm_direct_live_bytes() - before;
    mmproj_bytes.store(added);
    if (added > PS5LM_VISION_MMPROJ_MIB * MiB) ps5lm_ops_fatal("mmproj_budget");
    return ctx;
}
#endif
#endif

#define STR2(x) #x
#define STR(x) STR2(x)

// Every setting that matters is explicit; the wraps above check that the
// server passed them through.
static const char *const args[] = {
    "llama-server", "-m", MODEL_PATH,
    "--host", PS5LM_SERVER_HOST, "--port", PS5LM_SERVER_PORT,
    "-ngl", "999", "-c", STR(PS5LM_SERVER_CTX), "-b", STR(PS5LM_SERVER_BATCH),
    "-ub", STR(PS5LM_SERVER_BATCH), "-np", "1",
    "-t", STR(PS5LM_MODEL_THREADS), "-tb", STR(PS5LM_MODEL_THREADS),
    "-fa", "on", "-ctk", PS5LM_SERVER_KV, "-ctv", PS5LM_SERVER_KV, "--no-op-offload", "--no-repack",
    "-lm", "none", "-lzm", "off", "-fit", "off",
    "--no-webui", "--cache-ram", "0", "--ctx-checkpoints", "0",
    "--threads-http", PS5LM_SERVER_HTTP_THREADS, "--log-colors", "off",
#if defined(PS5LM_MODEL_VISION)
    /* The encoder on the GPU, image tokens capped as in the model mode. */
    "--mmproj", MMPROJ_PATH, "--image-min-tokens", STR(PS5LM_VISION_MIN_TOKENS),
    "--image-max-tokens", STR(PS5LM_VISION_MAX_TOKENS),
#endif
};

static std::vector<char *> server_argv()
{
    std::vector<char *> argv;
    for (const char *a : args) argv.push_back(const_cast<char *>(a));
    argv.push_back(nullptr);
    return argv;
}

#if defined(PS5LM_HOST_REHEARSAL)
/* No --wrap on the Mac: check what the same arguments turn into. */
static void rehearse_params()
{
    common_params params;
    std::vector<char *> argv = server_argv();
    if (!common_params_parse(int(argv.size() - 1), argv.data(), params, LLAMA_EXAMPLE_SERVER))
        ps5lm_ops_fatal("server_args");
    llama_model_params mp = common_model_params_to_llama(params);
    check_model_params(params.model.path.c_str(), mp);
    check_context_params(common_context_params_to_llama(params));
#if defined(PS5LM_MODEL_VISION)
    if (params.mmproj.path != MMPROJ_PATH || !params.mmproj_use_gpu || params.image_min_tokens != PS5LM_VISION_MIN_TOKENS ||
        params.image_max_tokens != PS5LM_VISION_MAX_TOKENS || params.flash_attn_type != LLAMA_FLASH_ATTN_TYPE_ENABLED ||
        !params.warmup)
        ps5lm_ops_fatal("server_mmproj_params");
#endif
    std::fprintf(stderr, "PS5LM_HOST_PARAMS ok\n");
}
#endif

static void *server_main(void *)
{
    std::vector<char *> argv = server_argv();
    int rc = -2;
    try {
        rc = llama_server(int(argv.size() - 1), argv.data());
    } catch (...) {
        rc = -3;
    }
    server_rc.store(rc);
    server_done.store(true, std::memory_order_release);
    return nullptr;
}

/* Storage read speed for the next phase (SSD weight streaming), measured after
 * the server has stopped and freed: read-only, the pinned model file, fixed
 * regions from its start (read first during the load, so the least likely to
 * still be cached), one 64 MiB heap buffer. Sequential reads of 1, 4, 16 and
 * 64 MiB, then four threads with their own descriptors, then the first region
 * again (a cached read, for comparison). Bounded: a test stops at 10 s and the
 * probe at 30 s. One PS5LM_STORAGE_PROBE line. */
/* Seven regions: four sequential, one parallel, one fread; 96 MiB, or less for a small file. */
static constexpr size_t PROBE_REGION = std::min<uint64_t>(uint64_t(96) << 20, (PS5LM_MODEL_BYTES / 7) & ~((uint64_t(1) << 20) - 1));
static constexpr unsigned PROBE_THREADS = 4;

static double probe_now_s()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/* Reads [offset, offset + bytes) in chunk-sized reads; the bytes read, or -1. */
static long long probe_read(int fd, char *buffer, size_t chunk, uint64_t offset, size_t bytes, double deadline)
{
    if (lseek(fd, off_t(offset), SEEK_SET) != off_t(offset)) return -1;
    size_t done = 0;
    while (done < bytes) {
        const size_t want = std::min(chunk, bytes - done);
        size_t got = 0;
        while (got < want) {
            const ssize_t n = read(fd, buffer + got, want - got);
            if (n <= 0) return -1;
            got += size_t(n);
        }
        done += got;
        if (probe_now_s() > deadline) break;
    }
    return (long long)done;
}

struct probe_thread {
    char *buffer;
    uint64_t offset;
    size_t bytes;
    double deadline;
    long long done;
};

static void *probe_worker(void *arg)
{
    probe_thread *t = static_cast<probe_thread *>(arg);
    const int fd = open(MODEL_PATH, O_RDONLY | O_CLOEXEC);
    t->done = fd < 0 ? -1 : probe_read(fd, t->buffer, size_t(4) << 20, t->offset, t->bytes, t->deadline);
    if (fd >= 0) close(fd);
    return nullptr;
}

static void storage_probe()
{
    static const unsigned chunks_mib[] = {1, 4, 16, 64};
    std::string json = "{\"file_bytes\":" + std::to_string(PS5LM_MODEL_BYTES) + ",\"region_mib\":" +
                       std::to_string(PROBE_REGION >> 20) + ",\"tests\":[";
    char *buffer = PROBE_REGION >= (size_t(16) << 20) ? static_cast<char *>(std::malloc(size_t(64) << 20)) : nullptr;
    const int fd = buffer ? open(MODEL_PATH, O_RDONLY | O_CLOEXEC) : -1;
    const double probe_deadline = probe_now_s() + 30.0;
    bool first = true;
    auto add = [&](const char *kind, unsigned chunk, unsigned threads, uint64_t offset, long long bytes, double s) {
        char item[256];
        std::snprintf(item, sizeof item, "%s{\"kind\":\"%s\",\"chunk_mib\":%u,\"threads\":%u,\"offset_mib\":%llu,"
                      "\"bytes\":%lld,\"s\":%.3f,\"mb_s\":%.1f}", first ? "" : ",", kind, chunk, threads,
                      (unsigned long long)(offset >> 20), bytes, s, bytes > 0 && s > 0 ? bytes / s / 1e6 : 0.0);
        json += item;
        first = false;
    };
    if (fd >= 0) {
        uint64_t offset = 0;
        for (unsigned c : chunks_mib) {
            if (probe_now_s() > probe_deadline) break;
            const double t0 = probe_now_s();
            const long long n = probe_read(fd, buffer, size_t(c) << 20, offset, PROBE_REGION,
                                           std::min(t0 + 10.0, probe_deadline));
            add("sequential", c, 1, offset, n, probe_now_s() - t0);
            offset += PROBE_REGION;
        }
        if (probe_now_s() < probe_deadline) {
            probe_thread threads[PROBE_THREADS];
            pthread_t ids[PROBE_THREADS];
            unsigned started = 0;
            const double t0 = probe_now_s();
            for (unsigned i = 0; i < PROBE_THREADS; ++i) {
                threads[i] = {buffer + (size_t(i) << 23), offset + i * (PROBE_REGION / PROBE_THREADS),
                              PROBE_REGION / PROBE_THREADS, std::min(t0 + 10.0, probe_deadline), -1};
                if (pthread_create(&ids[i], nullptr, probe_worker, &threads[i]) == 0) ++started;
                else break;
            }
            long long total = 0;
            for (unsigned i = 0; i < started; ++i) {
                pthread_join(ids[i], nullptr);
                total += threads[i].done > 0 ? threads[i].done : 0;
            }
            add("parallel", 4, started, offset, total, probe_now_s() - t0);
            offset += PROBE_REGION;
        }
        if (probe_now_s() < probe_deadline) {
            /* What llama.cpp's loader does without the read patch: 1 MiB freads
             * through a default-buffered FILE. */
            FILE *f = std::fopen(MODEL_PATH, "rb");
            long long n = -1;
            const double t0 = probe_now_s();
            if (f && std::fseek(f, long(offset), SEEK_SET) == 0) {
                n = 0;
                const double deadline = std::min(t0 + 10.0, probe_deadline);
                while (size_t(n) < PROBE_REGION && probe_now_s() < deadline) {
                    if (std::fread(buffer, size_t(1) << 20, 1, f) != 1) { n = -1; break; }
                    n += 1 << 20;
                }
            }
            add("fread_stdio", 1, 1, offset, n, probe_now_s() - t0);
            if (f) std::fclose(f);
            offset += PROBE_REGION;
        }
        if (probe_now_s() < probe_deadline) {
            const double t0 = probe_now_s();
            const long long n = probe_read(fd, buffer, size_t(16) << 20, 0, PROBE_REGION,
                                           std::min(t0 + 10.0, probe_deadline));
            add("reread_first_region", 16, 1, 0, n, probe_now_s() - t0);
        }
        close(fd);
    }
    std::free(buffer);
    json += "],\"ran\":";
    json += fd >= 0 ? "true}" : "false}";
    const std::string line = "PS5LM_STORAGE_PROBE " + json + "\n";
    std::fputs(line.c_str(), stdout);
    klog(line.c_str());
}

static void sleep_ms(unsigned ms)
{
    timespec t{time_t(ms / 1000), long(ms % 1000) * 1000000L};
    nanosleep(&t, nullptr);
}

extern "C" int ps5lm_run_ggml_ops(void)
{
    try {
#if defined(PS5LM_HOST_REHEARSAL)
        rehearse_params();
#endif
        phase("server_start");
        pthread_attr_t attr;
        if (pthread_attr_init(&attr) != 0 || pthread_attr_setstacksize(&attr, PS5LM_SERVER_STACK_BYTES) != 0)
            ps5lm_ops_fatal("server_thread_attr");
        pthread_t server;
        if (pthread_create(&server, &attr, server_main, nullptr) != 0) ps5lm_ops_fatal("server_thread_create");
        pthread_attr_destroy(&attr);
        bool stopping = false;
        uint64_t last_flush = ps5lm_elapsed_ms();
        for (;;) {
            sleep_ms(100);
            const uint64_t now = ps5lm_elapsed_ms();
            if (server_done.load(std::memory_order_acquire)) {
                if (!stopping) stop_reason.store(3);
                break;
            }
            const bool is_ready = ready.load(std::memory_order_acquire);
            if (!is_ready && now > PS5LM_SERVER_LOAD_S * 1000ull) {
                ps5lm_flush_output();
                ps5lm_ops_fatal("server_load_timeout");
            }
            if (is_ready && !stopping) {
                const int reason = shutdown_requested.load() ? 2 :
                                   now - ready_ms.load() >= PS5LM_SERVER_WINDOW_S * 1000ull ? 1 : 0;
                if (reason) {
                    stop_reason.store(reason);
                    served_ms.store(now - ready_ms.load());
                    stop_ms.store(now);
                    phase(reason == 1 ? "stop_window" : "stop_request");
                    llama_server_terminate();
                    stopping = true;
                }
            }
            if (stopping && now - stop_ms.load() > PS5LM_SERVER_STOP_S * 1000ull) {
                ps5lm_flush_output();
                ps5lm_ops_fatal("server_stop_timeout");
            }
            if (now - last_flush >= 2000) {
                ps5lm_flush_output();
                last_flush = now;
            }
        }
        pthread_join(server, nullptr);
        phase("freed");
        storage_probe();
        phase("storage_probe_end");
        const int rc = server_rc.load();
        const int reason = stop_reason.load();
        const unsigned long long vk_live = ps5lm_vk_memory_live_bytes();
        const bool pass = rc == 0 && (reason == 1 || reason == 2) && errors.load() == 0 &&
                          responses.load() > 0 && ps5lm_vk_memory_errors() == 0;
        char line[1536];
        std::snprintf(line, sizeof line,
            "PS5LM_SERVER_SUMMARY {\"pass\":%s,\"server_rc\":%d,\"stop_reason\":\"%s\",\"responses\":%u,"
            "\"errors\":%u,\"load_ms\":%llu,\"ready_ms\":%llu,\"served_ms\":%llu,\"stop_to_freed_ms\":%llu,"
            "\"weights_bytes\":%llu,\"context_bytes\":%llu,\"live_at_ready\":%llu,\"direct_peak_bytes\":%llu,"
            "\"direct_live_after_free\":%llu,\"vk_peak_bytes\":%llu,\"vk_live_after_free\":%llu,"
            "\"vk_errors\":%u,\"submissions\":%u,\"n_ctx\":%u,\"batch\":%u,\"offloaded\":%d,"
            "\"offload_total\":%d,\"splits_batch\":%d,\"splits_single\":%d,\"mmproj_bytes\":%llu,"
            "\"mmproj_ms\":%llu}\n",
            pass ? "true" : "false", rc, reasons[reason >= 0 && reason < 4 ? reason : 0],
            responses.load(), errors.load(), (unsigned long long)load_ms.load(),
            (unsigned long long)ready_ms.load(), (unsigned long long)served_ms.load(),
            (unsigned long long)(stop_ms.load() ? ps5lm_elapsed_ms() - stop_ms.load() : 0),
            (unsigned long long)weights_bytes.load(), (unsigned long long)context_bytes.load(),
            (unsigned long long)live_at_ready.load(), (unsigned long long)ps5lm_direct_peak_bytes(),
            (unsigned long long)ps5lm_direct_live_bytes(), ps5lm_vk_memory_peak_bytes(), vk_live,
            ps5lm_vk_memory_errors(), ps5lm_submission_count(), PS5LM_SERVER_CTX, PS5LM_SERVER_BATCH,
            offloaded.load(), offload_total.load(), splits_batch.load(), splits_single.load(),
            (unsigned long long)mmproj_bytes.load(), (unsigned long long)mmproj_ms.load());
        std::fputs(line, stdout);
        klog(line);
        ps5lm_flush_output();
        ps5lm_ops_checked(1, pass ? 0 : 1);
        return pass ? 0 : 6;
    } catch (const std::exception &) {
        ps5lm_ops_fatal("server_exception");
    } catch (...) {
        ps5lm_ops_fatal("server_unknown_exception");
    }
}

#if defined(PS5LM_HOST_REHEARSAL)
int main()
{
    const int rc = ps5lm_run_ggml_ops();
    std::fprintf(stderr, "PS5LM_HOST_RC %d\n", rc);
    return rc;
}
#endif
