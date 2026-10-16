/* SPDX-License-Identifier: GPL-3.0-or-later
 * SSD streaming probe (build mode "stream-probe", docs/PORTING.md "SSD streaming").
 * Measures, inside the native title and without loading a model:
 *   1. pread() of expert-sized slices (OLMoE 1.18 MB, Qwen3 0.45 MB), one and
 *      several threads (what a read()-based expert cache would pay per miss);
 *   2. the CPU's MUL_MAT_ID rate on real expert tensors (Q4_K, Q6_K, IQ2_XS,
 *      IQ3_XXS) at 1-8 threads with a persistent threadpool;
 *   3. one tiny GPU graph, so the title's usual completion checks hold.
 * Bounded: one 120 MiB heap buffer, every test has a time limit, files are
 * read with pread() only (never mapped: see section 1 below). One
 * PS5LM_STREAM_PROBE line per section. Not yet run on the console in this form.
 */
#include "ggml.h"
#include "ggml-backend.h"
#include "ggml-cpu.h"
#include "ggml-model.h"
#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <sys/resource.h>
extern "C" int sysctlbyname(const char *, void *, size_t *, const void *, size_t);
#include <unistd.h>
#include <vector>

extern "C" [[noreturn]] void ps5lm_ops_fatal(const char *stage);
extern "C" void ps5lm_ops_checked(unsigned count, unsigned errors);
extern "C" uint64_t ps5lm_direct_live_bytes(void);
extern "C" uint64_t ps5lm_direct_peak_bytes(void);
extern "C" int sceKernelDebugOutText(int channel, const char *text);
extern "C" void ps5lm_flush_output(void);
extern "C" int sceKernelAvailableFlexibleMemorySize(size_t *size);
extern "C" int sceKernelAvailableDirectMemorySize(off_t start, off_t end, size_t align, off_t *phys, size_t *size);

static constexpr uint64_t MiB = 1048576;
#ifndef PS5LM_PROBE_MODELS /* a host check of the probe's logic points this at models/ */
#define PS5LM_PROBE_MODELS "/data/PS5LM/models/"
#endif
static const char *const OLMOE = PS5LM_PROBE_MODELS "OLMoE-1B-7B-0125-Instruct-Q4_K_M.gguf";
static const char *const QWEN = PS5LM_PROBE_MODELS "Qwen_Qwen3-30B-A3B-Instruct-2507-IQ2_M.gguf";

static double now_s()
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return double(t.tv_sec) + double(t.tv_nsec) * 1e-9;
}

static void emit(const std::string &section, const std::string &body)
{
    std::string line = "PS5LM_STREAM_PROBE {\"section\":\"" + section + "\"," + body + "}\n";
    std::fputs(line.c_str(), stdout);
    // The kernel log line is capped; the output file has the full line.
    if (line.size() < 1900) sceKernelDebugOutText(0, line.c_str());
    else sceKernelDebugOutText(0, (line.substr(0, 1880) + "...}\n").c_str());
    ps5lm_flush_output();
}

static std::string fmt(const char *f, ...) __attribute__((format(printf, 1, 2)));
static std::string fmt(const char *f, ...)
{
    char b[1024];
    va_list a;
    va_start(a, f);
    std::vsnprintf(b, sizeof b, f, a);
    va_end(a);
    return b;
}

/* ---- memory state ---- */
struct mem_state {
    long long flex = -1, direct = -1;            // bytes available to the title
    long long v_free = -1, v_inactive = -1, v_active = -1, v_wire = -1, v_cache = -1, v_page = -1;
    long long majflt = -1, minflt = -1, inblock = -1;
};
static long long sysctl_u(const char *name)
{
    unsigned value = 0;
    size_t len = sizeof value;
    unsigned long long wide = 0;
    if (sysctlbyname(name, &value, &len, nullptr, 0) == 0) {
        if (len == sizeof value) return value;
    }
    len = sizeof wide;
    if (sysctlbyname(name, &wide, &len, nullptr, 0) == 0 && len == sizeof wide) return (long long)wide;
    return -1;
}
static long long page_bytes = 0;
static mem_state snapshot()
{
    mem_state s;
    size_t flex = 0;
    if (sceKernelAvailableFlexibleMemorySize(&flex) == 0) s.flex = (long long)flex;
    off_t phys = 0;
    size_t size = 0;
    if (sceKernelAvailableDirectMemorySize(0, (off_t)UINT64_C(0x400000000), 0, &phys, &size) == 0) s.direct = (long long)size;
    s.v_free = sysctl_u("vm.stats.vm.v_free_count");
    s.v_inactive = sysctl_u("vm.stats.vm.v_inactive_count");
    s.v_active = sysctl_u("vm.stats.vm.v_active_count");
    s.v_wire = sysctl_u("vm.stats.vm.v_wire_count");
    s.v_cache = sysctl_u("vm.stats.vm.v_cache_count");
    s.v_page = sysctl_u("vm.stats.vm.v_page_count");
    rusage u;
    if (getrusage(RUSAGE_SELF, &u) == 0) { s.majflt = u.ru_majflt; s.minflt = u.ru_minflt; s.inblock = u.ru_inblock; }
    return s;
}
static std::string json(const mem_state &s)
{
    return fmt("{\"flex\":%lld,\"direct_avail\":%lld,\"direct_live\":%llu,\"v_free\":%lld,\"v_inactive\":%lld,"
               "\"v_active\":%lld,\"v_wire\":%lld,\"v_cache\":%lld,\"v_page\":%lld,\"majflt\":%lld,\"minflt\":%lld,"
               "\"inblock\":%lld}", s.flex, s.direct, (unsigned long long)ps5lm_direct_live_bytes(), s.v_free,
               s.v_inactive, s.v_active, s.v_wire, s.v_cache, s.v_page, s.majflt, s.minflt, s.inblock);
}
/* pread sets, disjoint from each other:
 * OLMoE Q4_K tensors (64 slices of 1179648 bytes), Qwen3 IQ2_XS (128 of 454656). */
static const uint64_t olmoe_pread_1t[] = {2933487616ull, 3008985088ull, 3085015040ull, 3169974272ull,
                                          3245471744ull, 3321501696ull};  // blk.11-12
static const uint64_t olmoe_pread_4t[] = {2772498432ull, 2848528384ull, 3517642752ull, 3593672704ull}; // blk.10,13 gate/up
static const uint64_t qwen_pread_1t[] = {4779318272ull, 4837514240ull, 4896766976ull, 4964605952ull,
                                         5022801920ull, 5082054656ull};    // blk.22-23
static const uint64_t qwen_pread_4t[] = {5149893632ull, 5208089600ull, 5267342336ull, 5335181312ull,
                                         5393377280ull, 5452630016ull};    // blk.24-25
static const uint64_t qwen_pread_8t[] = {5520468992ull, 5578664960ull, 5637917696ull, 5705756672ull,
                                         5763952640ull, 5823205376ull};    // blk.26-27

static uint32_t rng = 0x9e3779b9u;
static uint32_t next_rand() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* ---- 1. mmap: REMOVED ----
 * Console run 1 of the SSD streaming work (test-results/ps5/PPSA99041/20261016-003554/)
 * mapped the 3.9 GiB OLMoE file here (PROT_READ, MAP_SHARED) and never returned
 * from mmap()/mincore(): the kernel's wired pages grew by ~20 MB/s from 451 MiB to
 * the whole ~1.2 GiB system pool (78,400 of 79,598 16 KiB pages), system processes
 * were swapped out, "FMEM allocation timeout" repeated, and the title did not close
 * after its watchdog's exit request. Never mmap a model file in a title
 * (docs/PORTING.md "SSD streaming"). */

/* ---- 2. pread of expert slices ---- */
struct pread_job {
    const char *path;
    const uint64_t *tensors;
    unsigned n_tensors, slices_per_tensor;
    uint64_t slice;
    unsigned count;
    char *buffer;
    double deadline;
    uint64_t bytes;
    unsigned done;
    uint32_t seed;
    std::vector<double> ms;
};
static void *pread_worker(void *arg)
{
    pread_job *j = static_cast<pread_job *>(arg);
    const int fd = open(j->path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return nullptr;
    uint32_t r = j->seed;
    for (unsigned i = 0; i < j->count && now_s() < j->deadline; ++i) {
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        const uint64_t off = j->tensors[r % j->n_tensors] + uint64_t((r >> 8) % j->slices_per_tensor) * j->slice;
        const double t0 = now_s();
        const ssize_t n = pread(fd, j->buffer, j->slice, (off_t)off);
        j->ms.push_back((now_s() - t0) * 1e3);
        if (n != (ssize_t)j->slice) break;
        j->bytes += j->slice;
        ++j->done;
    }
    close(fd);
    return nullptr;
}
static void pread_test(const char *label, const char *path, const uint64_t *tensors, unsigned n_tensors, unsigned spt, uint64_t slice,
                       unsigned threads, unsigned per_thread, char *buffer)
{
    std::vector<pread_job> jobs(threads);
    std::vector<pthread_t> ids(threads);
    const double t0 = now_s();
    unsigned started = 0;
    for (unsigned i = 0; i < threads; ++i) {
        jobs[i] = {path, tensors, n_tensors, spt, slice, per_thread, buffer + size_t(i) * (2 * MiB), t0 + 8.0, 0, 0,
                   0x1234567u * (i + 1) ^ next_rand(), {}};
        jobs[i].ms.reserve(per_thread);
        if (threads == 1) { pread_worker(&jobs[0]); started = 1; break; }
        if (pthread_create(&ids[i], nullptr, pread_worker, &jobs[i]) == 0) ++started;
    }
    if (threads > 1)
        for (unsigned i = 0; i < started; ++i) pthread_join(ids[i], nullptr);
    const double dt = now_s() - t0;
    uint64_t bytes = 0;
    unsigned done = 0;
    std::vector<double> all;
    for (auto &j : jobs) { bytes += j.bytes; done += j.done; all.insert(all.end(), j.ms.begin(), j.ms.end()); }
    std::sort(all.begin(), all.end());
    emit("pread", fmt("\"label\":\"%s\",\"slice_bytes\":%llu,\"threads\":%u,\"started\":%u,\"reads\":%u,\"ms\":%.1f,"
                      "\"mb_s\":%.1f,\"read_ms_p50\":%.3f,\"read_ms_p90\":%.3f",
                      label, (unsigned long long)slice, threads, started, done, dt * 1e3, double(bytes) / 1e6 / dt,
                      all.empty() ? 0.0 : all[all.size() / 2], all.empty() ? 0.0 : all[all.size() * 9 / 10]));
}

/* ---- 3. CPU MUL_MAT_ID ---- */
struct cpu_case {
    const char *label, *path;
    uint64_t offset;
    ggml_type type;
    int64_t k, m, n_expert;   // as = [k, m, n_expert]
    bool down;                // b is [k, 8] (one row per selected expert) instead of [k, 1]
};
static void cpu_section(char *heap)
{
    const cpu_case cases[] = {
        {"olmoe_gate_q4_k", OLMOE, 2263840768ull, GGML_TYPE_Q4_K, 2048, 1024, 64, false},
        {"olmoe_down_q6_k", OLMOE, 3951884288ull, GGML_TYPE_Q6_K, 1024, 2048, 64, true},
        {"qwen_gate_iq2_xs", QWEN, 4466938880ull, GGML_TYPE_IQ2_XS, 2048, 768, 128, false},
        {"qwen_down_iq3_xxs", QWEN, 363250688ull, GGML_TYPE_IQ3_XXS, 768, 2048, 128, true},
    };
    const int thread_counts[] = {1, 2, 4, 6, 8};
    for (const cpu_case &c : cases) {
        const size_t wbytes = ggml_row_size(c.type, c.k) * size_t(c.m) * size_t(c.n_expert);
        if (wbytes > 112 * MiB) { emit("cpu", fmt("\"label\":\"%s\",\"error\":\"too_large\"", c.label)); continue; }
        const int fd = open(c.path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) { emit("cpu", fmt("\"label\":\"%s\",\"error\":\"open\",\"errno\":%d", c.label, errno)); continue; }
        size_t got = 0;
        const double r0 = now_s();
        while (got < wbytes) {
            const ssize_t n = pread(fd, heap + got, std::min<size_t>(16 * MiB, wbytes - got), (off_t)(c.offset + got));
            if (n <= 0) break;
            got += size_t(n);
        }
        const double read_s = now_s() - r0;
        close(fd);
        if (got != wbytes) { emit("cpu", fmt("\"label\":\"%s\",\"error\":\"read\"", c.label)); continue; }
        ggml_init_params ip = {16 * MiB, nullptr, true};
        ggml_context *ctx = ggml_init(ip);
        ggml_tensor *as = ggml_new_tensor_3d(ctx, c.type, c.k, c.m, c.n_expert);
        ggml_tensor *b = ggml_new_tensor_3d(ctx, GGML_TYPE_F32, c.k, c.down ? 8 : 1, 1);
        ggml_tensor *ids = ggml_new_tensor_2d(ctx, GGML_TYPE_I32, 8, 1);
        ggml_tensor *out = ggml_mul_mat_id(ctx, as, b, ids);
        ggml_cgraph *gf = ggml_new_graph(ctx);
        ggml_build_forward_expand(gf, out);
        /* Small tensors in heap memory after the weights. */
        char *small = heap + ((wbytes + 4095) & ~size_t(4095));
        as->data = heap;
        b->data = small;
        ids->data = small + 64 * 1024;
        out->data = small + 128 * 1024;
        float *bf = static_cast<float *>(b->data);
        for (int64_t i = 0; i < c.k * (c.down ? 8 : 1); ++i) bf[i] = float((i * 7919) % 2003) / 2003.0f - 0.5f;
        std::string rows;
        for (int threads : thread_counts) {
            ggml_threadpool_params tpp = ggml_threadpool_params_default(threads);
            tpp.poll = 0;
            ggml_threadpool *tp = ggml_threadpool_new(&tpp);
            if (!tp) { rows += fmt("%s{\"threads\":%d,\"error\":\"threadpool\"}", rows.empty() ? "" : ",", threads); continue; }
            ggml_cplan plan = ggml_graph_plan(gf, threads, tp);
            std::vector<uint8_t> work(plan.work_size + 64);
            plan.work_data = work.data();
            unsigned iters = 0;
            const double t0 = now_s();
            while (now_s() - t0 < 0.4 && iters < 4000) {
                int32_t *id = static_cast<int32_t *>(ids->data);
                for (int e = 0; e < 8; ++e) {
                    int32_t v;
                    bool dup;
                    do { v = int32_t(next_rand() % uint32_t(c.n_expert)); dup = false; for (int q = 0; q < e; ++q) dup |= id[q] == v; } while (dup);
                    id[e] = v;
                }
                if (ggml_graph_compute(gf, &plan) != GGML_STATUS_SUCCESS) break;
                ++iters;
            }
            const double dt = now_s() - t0;
            ggml_threadpool_free(tp);
            const double bytes = double(wbytes) / double(c.n_expert) * 8.0 * iters;
            rows += fmt("%s{\"threads\":%d,\"iters\":%u,\"us_per_op\":%.1f,\"weight_gb_s\":%.2f}", rows.empty() ? "" : ",",
                        threads, iters, iters ? dt * 1e6 / iters : 0.0, bytes / 1e9 / dt);
        }
        const float *o = static_cast<const float *>(out->data);
        unsigned bad = 0;
        for (int64_t i = 0; i < c.m * 8; ++i) bad += !(o[i] == o[i]);
        emit("cpu", fmt("\"label\":\"%s\",\"weight_bytes\":%zu,\"slice_bytes\":%zu,\"read_ms\":%.1f,\"nan_outputs\":%u,\"rates\":[%s]",
                        c.label, wbytes, wbytes / size_t(c.n_expert), read_s * 1e3, bad, rows.c_str()));
        ggml_free(ctx);
    }
}

/* ---- 4. one tiny GPU graph ---- */
static unsigned gpu_check()
{
    ggml_backend_dev_t dev = ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_GPU);
    if (!dev) return 1;
    ggml_backend_t gpu = ggml_backend_dev_init(dev, nullptr);
    if (!gpu) return 1;
    ggml_init_params ip = {4 * MiB, nullptr, true};
    ggml_context *ctx = ggml_init(ip);
    ggml_tensor *a = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, 32);
    ggml_tensor *b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, 32);
    ggml_tensor *c = ggml_add(ctx, a, b);
    ggml_backend_buffer_t buf = ggml_backend_alloc_ctx_tensors(ctx, gpu);
    float x[32], y[32], z[32];
    for (int i = 0; i < 32; ++i) { x[i] = float(i); y[i] = float(2 * i + 1); }
    ggml_backend_tensor_set(a, x, 0, sizeof x);
    ggml_backend_tensor_set(b, y, 0, sizeof y);
    ggml_cgraph *gf = ggml_new_graph(ctx);
    ggml_build_forward_expand(gf, c);
    unsigned errors = ggml_backend_graph_compute(gpu, gf) == GGML_STATUS_SUCCESS ? 0 : 32;
    ggml_backend_tensor_get(c, z, 0, sizeof z);
    for (int i = 0; i < 32; ++i) errors += z[i] != float(3 * i + 1);
    ggml_backend_buffer_free(buf);
    ggml_free(ctx);
    ggml_backend_free(gpu);
    return errors;
}

extern "C" int ps5lm_run_ggml_ops(void)
{
    page_bytes = sysconf(_SC_PAGESIZE);
    if (page_bytes <= 0 || (page_bytes & (page_bytes - 1))) page_bytes = 16384;
    const double t0 = now_s();
    emit("start", fmt("\"page_bytes\":%lld,\"hw_ncpu\":%lld,\"mem\":%s", page_bytes, sysctl_u("hw.ncpu"),
                      json(snapshot()).c_str()));
    char *heap = static_cast<char *>(std::malloc(120 * MiB));
    if (!heap) ps5lm_ops_fatal("stream_probe_heap");
    pread_test("qwen_iq2_xs_1t", QWEN, qwen_pread_1t, 6, 128, 454656, 1, 96, heap);
    pread_test("qwen_iq2_xs_4t", QWEN, qwen_pread_4t, 6, 128, 454656, 4, 48, heap);
    pread_test("qwen_iq2_xs_8t", QWEN, qwen_pread_8t, 6, 128, 454656, 8, 24, heap);
    pread_test("olmoe_q4_k_1t", OLMOE, olmoe_pread_1t, 6, 64, 1179648, 1, 64, heap);
    pread_test("olmoe_q4_k_4t", OLMOE, olmoe_pread_4t, 4, 64, 1179648, 4, 24, heap);
    emit("after_pread", fmt("\"mem\":%s", json(snapshot()).c_str()));
    cpu_section(heap);
    std::free(heap);
    const unsigned gpu_errors = gpu_check();
    emit("end", fmt("\"seconds\":%.1f,\"gpu_errors\":%u,\"direct_peak\":%llu,\"mem\":%s", now_s() - t0, gpu_errors,
                    (unsigned long long)ps5lm_direct_peak_bytes(), json(snapshot()).c_str()));
    ps5lm_ops_checked(PS5LM_MODEL_PREDICT, gpu_errors ? 1 : 0);
    return gpu_errors ? 6 : 0;
}
