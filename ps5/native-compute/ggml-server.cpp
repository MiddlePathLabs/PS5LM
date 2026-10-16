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
#include <pthread.h>
#include <atomic>
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
#define MODEL_PATH PS5LM_MODEL_PATH
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

extern "C" void ps5lm_server_ready(void)
{
    live_at_ready.store(ps5lm_direct_live_bytes());
    ready_ms.store(ps5lm_elapsed_ms());
    ready.store(true, std::memory_order_release);
    phase("ready");
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
        params.flash_attn_type != LLAMA_FLASH_ATTN_TYPE_ENABLED || params.type_k != GGML_TYPE_F16 ||
        params.type_v != GGML_TYPE_F16 || !params.offload_kqv || params.op_offload)
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
        offload_total.load() != llama_model_n_layer(model) + 1)
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
    "-fa", "on", "-ctk", "f16", "-ctv", "f16", "--no-op-offload", "--no-repack",
    "-lm", "none", "-lzm", "off", "-fit", "off",
    "--no-webui", "--cache-ram", "0", "--ctx-checkpoints", "0",
    "--threads-http", PS5LM_SERVER_HTTP_THREADS, "--log-colors", "off",
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
        const int rc = server_rc.load();
        const int reason = stop_reason.load();
        const unsigned long long vk_live = ps5lm_vk_memory_live_bytes();
        const bool pass = rc == 0 && (reason == 1 || reason == 2) && errors.load() == 0 &&
                          responses.load() > 0 && ps5lm_vk_memory_errors() == 0;
        char line[1024];
        std::snprintf(line, sizeof line,
            "PS5LM_SERVER_SUMMARY {\"pass\":%s,\"server_rc\":%d,\"stop_reason\":\"%s\",\"responses\":%u,"
            "\"errors\":%u,\"load_ms\":%llu,\"ready_ms\":%llu,\"served_ms\":%llu,\"stop_to_freed_ms\":%llu,"
            "\"weights_bytes\":%llu,\"context_bytes\":%llu,\"live_at_ready\":%llu,\"direct_peak_bytes\":%llu,"
            "\"direct_live_after_free\":%llu,\"vk_peak_bytes\":%llu,\"vk_live_after_free\":%llu,"
            "\"vk_errors\":%u,\"submissions\":%u,\"n_ctx\":%u,\"batch\":%u,\"offloaded\":%d,"
            "\"offload_total\":%d,\"splits_batch\":%d,\"splits_single\":%d}\n",
            pass ? "true" : "false", rc, reasons[reason >= 0 && reason < 4 ? reason : 0],
            responses.load(), errors.load(), (unsigned long long)load_ms.load(),
            (unsigned long long)ready_ms.load(), (unsigned long long)served_ms.load(),
            (unsigned long long)(stop_ms.load() ? ps5lm_elapsed_ms() - stop_ms.load() : 0),
            (unsigned long long)weights_bytes.load(), (unsigned long long)context_bytes.load(),
            (unsigned long long)live_at_ready.load(), (unsigned long long)ps5lm_direct_peak_bytes(),
            (unsigned long long)ps5lm_direct_live_bytes(), ps5lm_vk_memory_peak_bytes(), vk_live,
            ps5lm_vk_memory_errors(), ps5lm_submission_count(), PS5LM_SERVER_CTX, PS5LM_SERVER_BATCH,
            offloaded.load(), offload_total.load(), splits_batch.load(), splits_single.load());
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
