/* SPDX-License-Identifier: GPL-3.0-or-later
 * Local-build-only native RADV probe. Launch only AFTER PPSA99040 capabilities
 * and an independently observed clean exit. No loader, display or privilege API.
 *
 * Extra linker wraps, in addition to tools/radv-link.sh:
 *   sceKernelAllocateDirectMemory sceKernelReleaseDirectMemory
 *   ps5p_heap_count_fallback getenv secure_getenv
 *   fopen freopen open mkdir
 * Keep these definitions local in the title's symbol map.
 */
#if !defined(__PROSPERO__)
#error This probe requires the native PS5 SDK; there is no host-model fallback.
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <ps5platform/heap.h>
#include <ps5platform/kernel.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if !defined(PS5LM_GGML_OPS)
#include "compute_spv.h"
#endif

#if defined(PS5LM_GGML_SERVER)
/* The server mode is the model mode (same model, caps, ledger, guards and
 * close) running llama.cpp's server for a bounded window (ggml-server.cpp). */
#include "ggml-model.h"
#include "ggml-server.h"
#define WORD_COUNT 1u
#define OUTPUT_PATH "/data/PS5LM/tests/native-server.out"
#define CACHE_PATH "/data/PS5LM/cache/native-server"
#define WORKLOAD PS5LM_SERVER_WORKLOAD
#elif defined(PS5LM_GGML_MODEL)
/* The model mode shares the ggml-ops runtime; only its limits and files differ. */
#include "ggml-model.h"
#define WORD_COUNT PS5LM_MODEL_PREDICT
#define OUTPUT_PATH "/data/PS5LM/tests/native-model.out"
#define CACHE_PATH "/data/PS5LM/cache/native-model"
#define WORKLOAD PS5LM_GGML_WORKLOAD
#elif defined(PS5LM_GGML_OPS)
#include "ggml-ops.h"
#define WORD_COUNT PS5LM_GGML_VALUE_COUNT
#define OUTPUT_PATH "/data/PS5LM/tests/native-ggml-ops.out"
#define CACHE_PATH "/data/PS5LM/cache/native-ggml-ops"
#define WORKLOAD PS5LM_GGML_WORKLOAD
#else
#define WORD_COUNT 64u
#define OUTPUT_PATH "/data/PS5LM/tests/native-compute.out"
#define CACHE_PATH "/data/PS5LM/cache/native-compute"
#define WORKLOAD "integer-dispatch"
#endif
#define BUFFER_LIMIT UINT64_C(1048576)
#if defined(PS5LM_GGML_MODEL)
/* The model's explicit budget (docs/PORTING.md), not the queried 12 GiB. */
#define DIRECT_LIMIT ((uint64_t)PS5LM_MODEL_TOTAL_MIB * UINT64_C(1048576))
#if defined(PS5LM_GGML_SERVER)
/* Load + serving window + stop + margin (ggml-server.h), not unbounded. */
#define WATCHDOG_TIMEOUT_NS ((uint64_t)PS5LM_SERVER_WATCHDOG_S * UINT64_C(1000000000))
#else
/* 300 s for the first models; larger ones size it from the measured load rate. */
#define WATCHDOG_TIMEOUT_NS ((uint64_t)PS5LM_MODEL_WATCHDOG_S * UINT64_C(1000000000))
#endif
#else
#define DIRECT_LIMIT UINT64_C(134217728)
#define WATCHDOG_TIMEOUT_NS UINT64_C(30000000000)
#endif
#define DIRECT_UNIT UINT64_C(65536)
#define FENCE_TIMEOUT_NS UINT64_C(2000000000)
#define OUTPUT_BYTES (WORD_COUNT * sizeof(uint32_t))
#define SENTINEL UINT32_C(0xa5a5a5a5)
#define LEDGER_SLOTS (DIRECT_LIMIT / DIRECT_UNIT)
#define KERNEL_ENOMEM ((int32_t)UINT32_C(0x8002000c))
#define KERNEL_EINVAL ((int32_t)UINT32_C(0x80020016))

int sceKernelDebugOutText(int channel, const char *text);
int sceShellCoreUtilExitApp(void);
int sceKernelUsleep(unsigned int microseconds);
/* SDK src/dlmalloc/malloc.c, ONLY_MSPACES: M_GRANULARITY is -2. The SDK
 * defaults to 16 MiB PER arena (eight arenas), not just the output buffer. */
int mspace_mallopt(int parameter, int value);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

static atomic_uint_fast64_t start_ns, direct_live, direct_peak, direct_denials;
static atomic_uint direct_errors, phase, verified_words, mismatches;
static atomic_int first_mismatch = ATOMIC_VAR_INIT(-1);
static atomic_uint first_expected, first_actual, memory_type = ATOMIC_VAR_INIT(UINT32_MAX);
static atomic_uint_fast64_t buffer_bytes, init_ns, compile_ns, submit_ns, verify_ns, cleanup_ns;
static atomic_bool attempted, submitted, fence_complete, cleanup_complete, coherent;
static atomic_bool result_claimed;
static atomic_int terminal_rc;
static const char *const phases[] = {
    "startup", "runtime_setup", "instance", "device", "buffer", "pipeline",
    "record", "submit_wait", "readback", "cleanup", "complete"
};

enum { RC_SETUP = 2, RC_ENTRYPOINT = 3, RC_LIMIT = 4, RC_VULKAN = 5,
       RC_MISMATCH = 6, RC_BUDGET = 7, RC_CLOCK = 8, RC_PATH = 9, RC_CLOSE = 10,
       RC_WATCHDOG = 124 };

static uint64_t now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0 || t.tv_sec < 0)
        return 0;
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

static uint64_t elapsed_ns(uint64_t before)
{
    const uint64_t after = now_ns();
    return before && after >= before ? after - before : 0;
}

#if defined(PS5LM_GGML_SERVER)
/* Atomics only (ggml-server.cpp): the watchdog's result may call these too. */
unsigned ps5lm_server_responses(void);
unsigned ps5lm_server_errors(void);
unsigned long long ps5lm_server_served_ms(void);
const char *ps5lm_server_stop_reason(void);
#define SERVER_FORMAT "\"http_responses\":%u,\"http_errors\":%u,\"served_ms\":%llu,\"stop_reason\":\"%s\","
#define SERVER_VALUES , ps5lm_server_responses(), ps5lm_server_errors(), ps5lm_server_served_ms(), \
    ps5lm_server_stop_reason()
#else
#define SERVER_FORMAT
#define SERVER_VALUES
#endif

static void emit_result(int rc, VkResult vk_rc, const char *stage)
{
    char line[2048];
    const uint64_t start = atomic_load(&start_ns);
    const int n = snprintf(line, sizeof(line),
        "[ps5lm-native] PS5LM_COMPUTE_RESULT {\"workload\":\"" WORKLOAD "\",\"success\":%s,\"rc\":%d,"
        "\"vk_rc\":%d,\"stage\":\"%s\",\"submission_attempted\":%s,"
        "\"submitted\":%s,\"fence_complete\":%s,\"cleanup_complete\":%s,"
        "\"verified_words\":%u,\"mismatches\":%u,\"first_mismatch\":%d,"
        "\"expected\":%u,\"actual\":%u,\"direct_budget_bytes\":%llu,"
        "\"direct_peak_bytes\":%llu,\"direct_live_bytes\":%llu,"
        "\"direct_denials\":%llu,\"direct_errors\":%u,"
        "\"buffer_allocation_bytes\":%llu,\"buffer_limit_bytes\":%llu,"
        "\"memory_type\":%u,\"host_coherent\":%s,"
        "\"fence_timeout_ns\":%llu,\"watchdog_timeout_ns\":%llu,"
        "\"init_us\":%llu,\"compile_us\":%llu,\"submit_wait_us\":%llu,"
        "\"verify_us\":%llu,\"cleanup_us\":%llu,\"total_us\":%llu,"
        "\"timing_zero_may_be_unfinished\":true," SERVER_FORMAT
        "\"system_allocations_capped\":false,\"clean_exit_proven\":false}\n",
        rc == 0 ? "true" : "false", rc, (int)vk_rc, stage,
        atomic_load(&attempted) ? "true" : "false",
        atomic_load(&submitted) ? "true" : "false",
        atomic_load(&fence_complete) ? "true" : "false",
        atomic_load(&cleanup_complete) ? "true" : "false",
        atomic_load(&verified_words), atomic_load(&mismatches), atomic_load(&first_mismatch),
        atomic_load(&first_expected), atomic_load(&first_actual),
        (unsigned long long)DIRECT_LIMIT, (unsigned long long)atomic_load(&direct_peak),
        (unsigned long long)atomic_load(&direct_live),
        (unsigned long long)atomic_load(&direct_denials), atomic_load(&direct_errors),
        (unsigned long long)atomic_load(&buffer_bytes), (unsigned long long)BUFFER_LIMIT,
        atomic_load(&memory_type), atomic_load(&coherent) ? "true" : "false",
        (unsigned long long)FENCE_TIMEOUT_NS, (unsigned long long)WATCHDOG_TIMEOUT_NS,
        (unsigned long long)(atomic_load(&init_ns) / 1000),
        (unsigned long long)(atomic_load(&compile_ns) / 1000),
        (unsigned long long)(atomic_load(&submit_ns) / 1000),
        (unsigned long long)(atomic_load(&verify_ns) / 1000),
        (unsigned long long)(atomic_load(&cleanup_ns) / 1000),
        (unsigned long long)(elapsed_ns(start) / 1000) SERVER_VALUES);
    if (n > 0 && (size_t)n < sizeof(line))
        sceKernelDebugOutText(0, line);
    else
        sceKernelDebugOutText(0, "PS5LM_COMPUTE_RESULT {\"success\":false,\"rc\":2,\"stage\":\"result_format\"}\n");
}

/* No raw exit syscall or CRT teardown: the capabilities probe established the
 * native shell-close route. This never destroys GPU-owned resources. Both the
 * main thread and watchdog may request closure; neither disarms the watchdog.
 * SystemService IPC can block or fail, so this is not a recovery guarantee. */
static _Noreturn void close_probe(int rc)
{
    char line[256];
    snprintf(line, sizeof(line),
             "PS5LM_COMPUTE_EXIT_REQUEST {\"intended_status\":%d}\n", rc);
    sceKernelDebugOutText(0, line);
    const int close_rc = sceShellCoreUtilExitApp();
    if (close_rc != 0) {
        int expected = 0;
        atomic_compare_exchange_strong(&terminal_rc, &expected, RC_CLOSE);
    }
    snprintf(line, sizeof(line),
             "%s {\"rc\":%d,\"close_rc\":%d,\"stage\":\"shellcore_exit_app\",\"clean_exit_proven\":false}\n",
             close_rc == 0 ? "PS5LM_COMPUTE_EXIT_RETURN" : "PS5LM_COMPUTE_ABORT",
             close_rc == 0 ? rc : RC_CLOSE, close_rc);
    sceKernelDebugOutText(0, line);
    for (;;)
        sceKernelUsleep(1000000);
}

/* The first fatal path owns RESULT; a later watchdog emits ABORT, never a
 * competing success result. ABORT invalidates a preceding completion marker. */
static _Noreturn void terminate_probe(int rc, const char *stage)
{
    int expected = 0;
    atomic_compare_exchange_strong(&terminal_rc, &expected, rc);
    if (!atomic_exchange(&result_claimed, true)) {
        emit_result(rc, VK_SUCCESS, stage);
    } else {
        char line[192];
        snprintf(line, sizeof(line),
                 "PS5LM_COMPUTE_ABORT {\"success\":false,\"rc\":%d,\"stage\":\"%s\",\"clean_exit_proven\":false}\n",
                 rc, stage);
        sceKernelDebugOutText(0, line);
    }
    close_probe(rc);
}

/* Environment gates are not a filesystem sandbox. Guard the actual writable
 * open/mkdir calls made by the pinned static objects too: a cache, ACO's fixed
 * /tmp/fileXXXXXX CLRX disassembler, or a debug dump must fail BEFORE a write,
 * not silently redirect a diagnostic file into our successful test output.
 * Imported modules' internal I/O is outside a static linker's wrap coverage. */
static const char *const runtime_directories[] = {
    "/data/PS5LM", "/data/PS5LM/tests", "/data/PS5LM/cache",
    CACHE_PATH, "/data/PS5LM/tmp"
};

static void require_output_path(const char *path)
{
    if (!path || strcmp(path, OUTPUT_PATH) != 0)
        terminate_probe(RC_PATH, "write_path_refused");
}

FILE *__real_fopen(const char *, const char *);
FILE *__real_freopen(const char *, const char *, FILE *);
int __real_open(const char *, int, ...);
int __real_mkdir(const char *, mode_t);

__attribute__((visibility("hidden"))) FILE *__wrap_fopen(const char *path, const char *mode)
{
    if (!mode || strpbrk(mode, "wa+"))
        require_output_path(path);
    return __real_fopen(path, mode);
}

__attribute__((visibility("hidden"))) FILE *
__wrap_freopen(const char *path, const char *mode, FILE *stream)
{
    if (!mode || strpbrk(mode, "wa+"))
        require_output_path(path);
    return __real_freopen(path, mode, stream);
}

__attribute__((visibility("hidden"))) int __wrap_open(const char *path, int flags, ...)
{
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC | O_APPEND)))
        require_output_path(path);
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        const int mode = va_arg(args, int);
        va_end(args);
        return __real_open(path, flags, mode);
    }
    return __real_open(path, flags);
}

__attribute__((visibility("hidden"))) int __wrap_mkdir(const char *path, mode_t mode)
{
    if (path)
        for (size_t i = 0; i < sizeof(runtime_directories) / sizeof(runtime_directories[0]); ++i)
            if (strcmp(path, runtime_directories[i]) == 0)
                return __real_mkdir(path, mode);
    terminate_probe(RC_PATH, "mkdir_path_refused");
}

/* Link-time interception covers every direct allocation in the pinned static
 * RADV winsys and SDK platform heap, thread stacks, shm and exec helpers.
 * Round charges UP to 64 KiB even for RADV's 16 KiB requests. Pending kernel
 * allocations reserve their charge BEFORE entering the kernel. No heap use.
 * Imported system modules' own allocations cannot be intercepted this way. */
struct direct_record {
    int64_t physical;
    size_t bytes;
    uint64_t charge;
    unsigned state; /* 0 free, 1 pending, 2 live, 3 releasing */
};
static struct direct_record direct_records[LEDGER_SLOTS];
static atomic_flag ledger_lock = ATOMIC_FLAG_INIT;

static void lock_ledger(void)
{
    while (atomic_flag_test_and_set_explicit(&ledger_lock, memory_order_acquire))
        __asm__ volatile("pause");
}

static void unlock_ledger(void)
{
    atomic_flag_clear_explicit(&ledger_lock, memory_order_release);
}

int32_t __real_sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t *);

#if defined(PS5LM_GGML_MODEL)
/* Who takes large blocks: size, time and caller (offset from main, for
 * llvm-symbolizer on llvm-pie.elf). Fixed table, no heap use. */
#define BIG_ALLOCATION UINT64_C(4194304)
#define BIG_SLOTS 64u
int main(void);
static struct { uint64_t bytes, live, ms; int64_t caller; } big_records[BIG_SLOTS];
static atomic_uint big_count;
static void record_big(size_t bytes, uint64_t live, void *caller)
{
    const unsigned slot = atomic_fetch_add(&big_count, 1);
    if (slot < BIG_SLOTS) {
        big_records[slot].bytes = bytes;
        big_records[slot].live = live;
        big_records[slot].ms = elapsed_ns(atomic_load(&start_ns)) / 1000000;
        big_records[slot].caller = (int64_t)((intptr_t)caller - (intptr_t)&main);
    }
}
void ps5lm_report_allocations(void)
{
    static unsigned reported; /* main thread only: each call reports what is new */
    const unsigned count = atomic_load(&big_count);
    char line[256];
    for (unsigned i = reported; i < count && i < BIG_SLOTS; ++i) {
        snprintf(line, sizeof(line),
                 "PS5LM_MODEL_ALLOC {\"index\":%u,\"bytes\":%llu,\"live_after\":%llu,\"ms\":%llu,"
                 "\"caller_from_main\":%lld}\n", i, (unsigned long long)big_records[i].bytes,
                 (unsigned long long)big_records[i].live, (unsigned long long)big_records[i].ms,
                 (long long)big_records[i].caller);
        sceKernelDebugOutText(0, line);
    }
    reported = count;
    snprintf(line, sizeof(line), "PS5LM_MODEL_ALLOC_COUNT {\"recorded\":%u,\"limit\":%u}\n", count, BIG_SLOTS);
    sceKernelDebugOutText(0, line);
}
#endif
int32_t __real_sceKernelReleaseDirectMemory(int64_t, size_t);

__attribute__((visibility("hidden"))) int32_t
__wrap_sceKernelAllocateDirectMemory(int64_t lo, int64_t hi, size_t bytes,
                                   size_t alignment, int type, int64_t *physical)
{
    if (!physical || !bytes || bytes > DIRECT_LIMIT || alignment > DIRECT_LIMIT) {
        atomic_fetch_add(&direct_denials, 1);
        return KERNEL_ENOMEM;
    }
    const uint64_t charge = ((uint64_t)bytes + DIRECT_UNIT - 1) & ~(DIRECT_UNIT - 1);
    lock_ledger();
    unsigned slot = 0;
    while (slot < LEDGER_SLOTS && direct_records[slot].state != 0)
        ++slot;
    const uint64_t live = atomic_load(&direct_live);
    if (slot == LEDGER_SLOTS || charge > DIRECT_LIMIT - live) {
        atomic_fetch_add(&direct_denials, 1);
        unlock_ledger();
        return KERNEL_ENOMEM;
    }
    direct_records[slot] = (struct direct_record){.bytes = bytes, .charge = charge, .state = 1};
    atomic_store(&direct_live, live + charge);
    if (live + charge > atomic_load(&direct_peak))
        atomic_store(&direct_peak, live + charge);
    unlock_ledger();
    const int32_t rc = __real_sceKernelAllocateDirectMemory(lo, hi, bytes, alignment, type, physical);
    lock_ledger();
    if (rc == 0) {
        direct_records[slot].physical = *physical;
        direct_records[slot].state = 2;
#if defined(PS5LM_GGML_MODEL)
        if (bytes >= BIG_ALLOCATION)
            record_big(bytes, atomic_load(&direct_live), __builtin_return_address(0));
#endif
    } else {
        atomic_fetch_sub(&direct_live, charge);
        direct_records[slot].state = 0;
    }
    unlock_ledger();
    return rc;
}

__attribute__((visibility("hidden"))) int32_t
__wrap_sceKernelReleaseDirectMemory(int64_t physical, size_t bytes)
{
    lock_ledger();
    unsigned slot = 0;
    while (slot < LEDGER_SLOTS && (direct_records[slot].state != 2 ||
           direct_records[slot].physical != physical || direct_records[slot].bytes != bytes))
        ++slot;
    if (slot == LEDGER_SLOTS) {
        atomic_fetch_add(&direct_errors, 1);
        unlock_ledger();
        return KERNEL_EINVAL; /* Unknown/partial/double release: never undercount. */
    }
    direct_records[slot].state = 3;
    const uint64_t charge = direct_records[slot].charge;
    unlock_ledger();
    const int32_t rc = __real_sceKernelReleaseDirectMemory(physical, bytes);
    lock_ledger();
    if (rc == 0) {
        atomic_fetch_sub(&direct_live, charge);
        direct_records[slot].state = 0;
    } else {
        atomic_fetch_add(&direct_errors, 1);
        direct_records[slot].state = 2;
    }
    unlock_ledger();
    return rc;
}

/* SDK heap_wrap.c calls this separate-object function immediately BEFORE
 * falling back to libc's flexible heap. Do not turn a rejected direct-memory
 * allocation into an unbudgeted successful fallback. */
__attribute__((visibility("hidden"))) void __wrap_ps5p_heap_count_fallback(void)
{
    terminate_probe(RC_BUDGET, "heap_fallback_refused");
}

/* A closed, title-local environment for STATIC callers: no inherited tracing,
 * shader-dump, fake GPU family, compiler, or alternate-cache options. libc's
 * own internal environment is also set below before the first Mesa call. */
static const struct { const char *name; char *value; } probe_environment[] = {
    {"MESA_SHADER_CACHE_DISABLE", "true"},
    {"MESA_SHADER_CACHE_DIR", CACHE_PATH},
    {"MESA_DISK_CACHE_DATABASE", "1"},
    {"RADV_DEBUG", "nocache"},
    {"RADV_THREADED_RECORDING", "0"},
    {"HOME", "/data/PS5LM"},
    {"XDG_CACHE_HOME", "/data/PS5LM/cache"},
    {"TMPDIR", "/data/PS5LM/tmp"},
#if defined(PS5LM_GGML_OPS)
    {"GGML_VK_DISABLE_ASYNC", "1"},
    {"GGML_VK_DISABLE_COOPMAT", "1"},
    {"GGML_VK_DISABLE_COOPMAT2", "1"},
#if !defined(PS5LM_GGML_MODEL)
    /* The operator probes keep fusion and graph optimisation off. */
    {"GGML_VK_DISABLE_FUSION", "1"},
    {"GGML_VK_DISABLE_GRAPH_OPTIMIZE", "1"},
    {"GGML_VK_PS5_ATTN_DIAGNOSTICS", "1"}, /* per dispatch: too many lines for a model */
#else
    /* Models: fusion and graph optimisation on, a submission every 1000 nodes
     * (the flop-based split stays), measured on the console in one serving
     * window per model with tokens identical to the model mode's
     * (docs/PORTING.md "Performance"). Runtime knobs can still override. */
    {"GGML_VK_MAX_NODES_PER_SUBMIT", "1000"},
#endif
#endif
#if defined(PS5LM_GGML_MODEL)
    /* ggml splits model buffers below the dispatch guard's 256 MiB cap,
     * leaving room for its +32-byte host buffers and alignment. */
    {"GGML_VK_FORCE_MAX_ALLOCATION_SIZE", PS5LM_MODEL_VK_MAX_ALLOCATION}, /* "251658240" unless a model needs more */
    /* max_buffer_size is used only by supports_op's tensor-size check, never
     * for an allocation. llama's load-time probe for an output layer is a
     * MUL_MAT with 512 rows, so a 248320-token vocabulary has a 485 MiB probe
     * result; at 240 MiB it was rejected and the LM head went to Vulkan_Host.
     * 512 MiB accepts it; allocations stay capped by the line above. */
    {"GGML_VK_FORCE_MAX_BUFFER_SIZE", PS5LM_MODEL_VK_MAX_BUFFER}, /* "536870912" unless a model needs more */
#endif
};

__attribute__((visibility("hidden"))) char *__wrap_getenv(const char *name)
{
    if (name)
        for (size_t i = 0; i < sizeof(probe_environment) / sizeof(probe_environment[0]); ++i)
            if (strcmp(name, probe_environment[i].name) == 0)
                return probe_environment[i].value;
    return NULL;
}

__attribute__((visibility("hidden"))) char *__wrap_secure_getenv(const char *name)
{
    return __wrap_getenv(name);
}

static void *watchdog(void *unused)
{
    (void)unused;
    for (;;) {
        const uint64_t now = now_ns();
        const uint64_t start = atomic_load(&start_ns);
        if (!now || now < start)
            terminate_probe(RC_CLOCK, "watchdog_clock");
        if (now - start >= WATCHDOG_TIMEOUT_NS) {
            const unsigned at = atomic_load(&phase);
            terminate_probe(RC_WATCHDOG, phases[at < sizeof(phases) / sizeof(phases[0]) ? at : 0]);
        }
        const struct timespec tick = {.tv_sec = 0, .tv_nsec = 10000000};
        if (nanosleep(&tick, NULL) != 0 && errno != EINTR)
            terminate_probe(RC_CLOCK, "watchdog_sleep");
    }
}

#if !defined(PS5LM_GGML_OPS)
#define INSTANCE_FUNCTIONS(X) \
    X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
    X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceMemoryProperties) \
    X(CreateDevice) X(GetDeviceProcAddr)
#define DEVICE_FUNCTIONS(X) \
    X(DestroyDevice) X(GetDeviceQueue) X(CreateBuffer) X(DestroyBuffer) \
    X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) \
    X(MapMemory) X(UnmapMemory) X(FlushMappedMemoryRanges) X(InvalidateMappedMemoryRanges) \
    X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout) \
    X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
    X(CreateComputePipelines) X(DestroyPipeline) X(CreateDescriptorPool) \
    X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) \
    X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdDispatch) \
    X(CreateFence) X(DestroyFence) X(QueueSubmit) X(WaitForFences)
#define DECLARE(name) static PFN_vk##name vk##name;
INSTANCE_FUNCTIONS(DECLARE)
DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE

struct probe {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mapping;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout pipeline_layout;
    VkDescriptorPool descriptor_pool;
    VkShaderModule shader;
    VkPipeline pipeline;
    VkCommandPool command_pool;
    VkFence fence;
    bool unsafe_submission;
    VkResult vk_rc;
    const char *failure;
};

#define VK_CALL(label, expression) do { \
    p->failure = label; \
    p->vk_rc = (expression); \
    if (p->vk_rc != VK_SUCCESS) return RC_VULKAN; \
} while (0)
#define REQUIRE(condition, label) do { \
    if (!(condition)) { p->failure = label; return RC_LIMIT; } \
} while (0)

static int run_probe(struct probe *p)
{
    atomic_store(&phase, 2);
    const PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance)
        vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create_instance) { p->failure = "load_vkCreateInstance"; return RC_ENTRYPOINT; }
    const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "PS5LM Native Compute", .apiVersion = VK_API_VERSION_1_0};
    const VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app};
    VK_CALL("create_instance", create_instance(&instance_info, NULL, &p->instance));
#define LOAD_INSTANCE(name) do { \
    vk##name = (PFN_vk##name)vk_icdGetInstanceProcAddr(p->instance, "vk" #name); \
    if (!vk##name) { p->failure = "load_vk" #name; return RC_ENTRYPOINT; } \
} while (0);
    INSTANCE_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
    uint32_t count = 0;
    VK_CALL("enumerate_devices_count", vkEnumeratePhysicalDevices(p->instance, &count, NULL));
    REQUIRE(count == 1, "require_one_physical_device");
    VK_CALL("enumerate_devices", vkEnumeratePhysicalDevices(p->instance, &count, &p->physical));
    REQUIRE(count == 1, "physical_device_count_changed");
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(p->physical, &properties);
    REQUIRE(properties.vendorID == 0x1002 &&
        (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ||
         properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU), "require_amd_hardware_gpu");
    REQUIRE(properties.limits.maxComputeWorkGroupInvocations >= WORD_COUNT &&
        properties.limits.maxComputeWorkGroupSize[0] >= WORD_COUNT &&
        properties.limits.maxComputeWorkGroupCount[0] >= 1, "compute_limits");
    count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(p->physical, &count, NULL);
    REQUIRE(count && count <= 16, "queue_family_count");
    VkQueueFamilyProperties families[16];
    vkGetPhysicalDeviceQueueFamilyProperties(p->physical, &count, families);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < count; ++i)
        if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            family = i;
            break;
        }
    REQUIRE(family != UINT32_MAX, "compute_queue_family");
    atomic_store(&phase, 3);
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    const VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info};
    VK_CALL("create_device", vkCreateDevice(p->physical, &device_info, NULL, &p->device));
#define LOAD_DEVICE(name) do { \
    vk##name = (PFN_vk##name)vkGetDeviceProcAddr(p->device, "vk" #name); \
    if (!vk##name) { p->failure = "load_vk" #name; return RC_ENTRYPOINT; } \
} while (0);
    DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
    vkGetDeviceQueue(p->device, family, 0, &p->queue);
    REQUIRE(p->queue != VK_NULL_HANDLE, "get_device_queue");

    atomic_store(&phase, 4);
    const VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = OUTPUT_BYTES, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VK_CALL("create_buffer", vkCreateBuffer(p->device, &buffer_info, NULL, &p->buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(p->device, p->buffer, &requirements);
    REQUIRE(requirements.size >= OUTPUT_BYTES && requirements.size <= BUFFER_LIMIT &&
        requirements.alignment && requirements.alignment <= BUFFER_LIMIT &&
        !(requirements.alignment & (requirements.alignment - 1)), "buffer_memory_requirements");
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(p->physical, &memory_properties);
    uint32_t selected = UINT32_MAX;
    unsigned best_score = 0;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        const VkMemoryType type = memory_properties.memoryTypes[i];
        if (!(requirements.memoryTypeBits & (UINT32_C(1) << i)) ||
            !(type.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
            (type.propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) ||
            type.heapIndex >= memory_properties.memoryHeapCount ||
            memory_properties.memoryHeaps[type.heapIndex].size < requirements.size)
            continue;
        const unsigned score = 1 + 2 * !!(type.propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) +
                                   !!(type.propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (score > best_score) { best_score = score; selected = i; }
    }
    REQUIRE(selected != UINT32_MAX, "host_visible_memory_type");
    atomic_store(&memory_type, selected);
    atomic_store(&coherent, !!(memory_properties.memoryTypes[selected].propertyFlags &
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
    const VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = selected};
    VK_CALL("allocate_buffer_memory", vkAllocateMemory(p->device, &allocation, NULL, &p->memory));
    atomic_store(&buffer_bytes, requirements.size);
    VK_CALL("bind_buffer_memory", vkBindBufferMemory(p->device, p->buffer, p->memory, 0));
    VK_CALL("map_buffer_memory", vkMapMemory(p->device, p->memory, 0, VK_WHOLE_SIZE, 0, &p->mapping));
    REQUIRE(p->mapping != NULL, "mapped_pointer");
    volatile uint32_t *const words = p->mapping;
    for (uint32_t i = 0; i < WORD_COUNT; ++i)
        words[i] = SENTINEL;
    /* Whole mapped allocation: offset zero is atom-aligned and VK_WHOLE_SIZE
     * handles an allocation whose final atom is partial, without overflow. */
    const VkMappedMemoryRange mapped_range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = p->memory, .offset = 0, .size = VK_WHOLE_SIZE};
    if (!atomic_load(&coherent))
        VK_CALL("flush_sentinel", vkFlushMappedMemoryRanges(p->device, 1, &mapped_range));
    atomic_store(&init_ns, elapsed_ns(atomic_load(&start_ns)));

    atomic_store(&phase, 5);
    const uint64_t compile_start = now_ns();
    const VkDescriptorSetLayoutBinding binding = {.binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    const VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding};
    VK_CALL("create_set_layout", vkCreateDescriptorSetLayout(p->device, &layout_info, NULL, &p->set_layout));
    const VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &p->set_layout};
    VK_CALL("create_pipeline_layout", vkCreatePipelineLayout(p->device, &pipeline_layout_info, NULL,
                                                           &p->pipeline_layout));
    const VkShaderModuleCreateInfo shader_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(ps5lm_compute_spv), .pCode = ps5lm_compute_spv};
    VK_CALL("create_shader", vkCreateShaderModule(p->device, &shader_info, NULL, &p->shader));
    const VkComputePipelineCreateInfo pipeline_info = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = p->shader, .pName = "main"},
        .layout = p->pipeline_layout};
    VK_CALL("create_compute_pipeline", vkCreateComputePipelines(p->device, VK_NULL_HANDLE, 1,
                                                              &pipeline_info, NULL, &p->pipeline));
    atomic_store(&compile_ns, elapsed_ns(compile_start));
    const VkDescriptorPoolSize pool_size = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1};
    const VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    VK_CALL("create_descriptor_pool", vkCreateDescriptorPool(p->device, &pool_info, NULL, &p->descriptor_pool));
    const VkDescriptorSetAllocateInfo set_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = p->descriptor_pool, .descriptorSetCount = 1, .pSetLayouts = &p->set_layout};
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VK_CALL("allocate_descriptor_set", vkAllocateDescriptorSets(p->device, &set_info, &descriptor_set));
    const VkDescriptorBufferInfo descriptor_buffer = {.buffer = p->buffer, .offset = 0, .range = OUTPUT_BYTES};
    const VkWriteDescriptorSet descriptor_write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descriptor_set, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &descriptor_buffer};
    vkUpdateDescriptorSets(p->device, 1, &descriptor_write, 0, NULL);

    atomic_store(&phase, 6);
    const VkCommandPoolCreateInfo command_pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = family};
    VK_CALL("create_command_pool", vkCreateCommandPool(p->device, &command_pool_info, NULL, &p->command_pool));
    const VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = p->command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command = VK_NULL_HANDLE;
    VK_CALL("allocate_command_buffer", vkAllocateCommandBuffers(p->device, &command_info, &command));
    const VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CALL("begin_command_buffer", vkBeginCommandBuffer(command, &begin_info));
    VkBufferMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = p->buffer, .offset = 0, .size = OUTPUT_BYTES};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 1, &barrier, 0, NULL);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline_layout,
                            0, 1, &descriptor_set, 0, NULL);
    vkCmdDispatch(command, 1, 1, 1); /* The sole application GPU dispatch. */
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &barrier, 0, NULL);
    VK_CALL("end_command_buffer", vkEndCommandBuffer(command));
    const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CALL("create_fence", vkCreateFence(p->device, &fence_info, NULL, &p->fence));

    atomic_store(&phase, 7);
    const uint64_t submit_start = now_ns();
    const VkSubmitInfo submit_info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command};
    /* Even a failed submit may have partially reached the device. No Vulkan
     * destruction after ANY submit attempt without a successful fence wait. */
    p->unsafe_submission = true;
    atomic_store(&attempted, true);
    p->failure = "queue_submit";
    p->vk_rc = vkQueueSubmit(p->queue, 1, &submit_info, p->fence);
    atomic_store(&submit_ns, elapsed_ns(submit_start));
    if (p->vk_rc != VK_SUCCESS)
        return RC_VULKAN;
    atomic_store(&submitted, true);
    p->failure = "wait_fence";
    p->vk_rc = vkWaitForFences(p->device, 1, &p->fence, VK_TRUE, FENCE_TIMEOUT_NS);
    atomic_store(&submit_ns, elapsed_ns(submit_start));
    if (p->vk_rc != VK_SUCCESS)
        return RC_VULKAN;
    p->unsafe_submission = false;
    atomic_store(&fence_complete, true);
    atomic_store(&submit_ns, elapsed_ns(submit_start));

    atomic_store(&phase, 8);
    const uint64_t verify_start = now_ns();
    if (!atomic_load(&coherent))
        VK_CALL("invalidate_output", vkInvalidateMappedMemoryRanges(p->device, 1, &mapped_range));
    for (uint32_t i = 0; i < WORD_COUNT; ++i) {
        const uint32_t expected = i * UINT32_C(3) + UINT32_C(7);
        const uint32_t actual = words[i];
        if (actual != expected) {
            if (atomic_fetch_add(&mismatches, 1) == 0) {
                atomic_store(&first_mismatch, (int)i);
                atomic_store(&first_expected, expected);
                atomic_store(&first_actual, actual);
            }
        }
        atomic_fetch_add(&verified_words, 1);
    }
    atomic_store(&verify_ns, elapsed_ns(verify_start));
    if (atomic_load(&mismatches)) { p->failure = "readback_mismatch"; return RC_MISMATCH; }
    return 0;
}
#undef VK_CALL
#undef REQUIRE

static void cleanup_probe(struct probe *p)
{
    atomic_store(&phase, 9);
    const uint64_t before = now_ns();
    /* Called only before submission, or after fence success. No WaitIdle. */
    bool complete = true;
#define DESTROY(member, function) do { \
    if (p->member) { \
        if (vk##function) vk##function(p->device, p->member, NULL); \
        else complete = false; \
    } \
} while (0)
    if (p->device) {
        DESTROY(fence, DestroyFence);
        DESTROY(command_pool, DestroyCommandPool);
        DESTROY(pipeline, DestroyPipeline);
        DESTROY(shader, DestroyShaderModule);
        DESTROY(descriptor_pool, DestroyDescriptorPool);
        DESTROY(pipeline_layout, DestroyPipelineLayout);
        DESTROY(set_layout, DestroyDescriptorSetLayout);
        if (p->mapping) {
            if (vkUnmapMemory) vkUnmapMemory(p->device, p->memory);
            else complete = false;
        }
        DESTROY(buffer, DestroyBuffer);
        DESTROY(memory, FreeMemory);
        if (vkDestroyDevice) vkDestroyDevice(p->device, NULL);
        else complete = false;
    }
#undef DESTROY
    if (p->instance) {
        if (vkDestroyInstance) vkDestroyInstance(p->instance, NULL);
        else complete = false;
    }
    atomic_store(&cleanup_ns, elapsed_ns(before));
    atomic_store(&cleanup_complete, complete);
}
#else
/* Share the proven setup, ledger, watchdog and native close paths; only the
 * workload changes. Dispatch failures terminate immediately without unwinding. */
struct probe {
    VkResult vk_rc;
    const char *failure;
    bool unsafe_submission;
};
int ps5lm_run_ggml_ops(void);
#if defined(PS5LM_GGML_MODEL)
/* Writes to /data cost tens of milliseconds each from this title, so the model
 * mode buffers its output and flushes at phase boundaries. Only the main thread
 * flushes; the watchdog never touches stdio (it could hold the stream lock). */
static char stdout_buffer[1 << 20], stderr_buffer[1 << 16];
static pthread_t main_thread;
void ps5lm_flush_output(void)
{
    if (pthread_equal(pthread_self(), main_thread)) {
        fflush(stdout);
        fflush(stderr);
    }
}
#endif
_Noreturn void ps5lm_ops_fatal(const char *stage)
{
#if defined(PS5LM_GGML_MODEL)
    ps5lm_flush_output();
#endif
    terminate_probe(RC_VULKAN, stage);
}
void ps5lm_ops_checked(unsigned count, unsigned errors)
{
    atomic_fetch_add(&verified_words, count);
    atomic_fetch_add(&mismatches, errors);
}
uint64_t ps5lm_elapsed_ms(void)
{
    return elapsed_ns(atomic_load(&start_ns)) / 1000000;
}
uint64_t ps5lm_direct_live_bytes(void)
{
    return atomic_load(&direct_live);
}
uint64_t ps5lm_direct_peak_bytes(void)
{
    return atomic_load(&direct_peak);
}
void ps5lm_ops_submission(bool accepted)
{
    atomic_store(&attempted, true);
    if (accepted) atomic_store(&submitted, true);
}
static int run_probe(struct probe *p)
{
    atomic_store(&phase, 3);
    p->failure = "ggml_ops";
    const int rc = ps5lm_run_ggml_ops();
    atomic_store(&fence_complete, atomic_load(&submitted));
    return rc;
}
static void cleanup_probe(struct probe *p)
{
    (void)p;
    /* Runner released graph buffers, contexts and backends before returning.
     * Process-lifetime Vulkan registry/device objects remain until shell close. */
    atomic_store(&cleanup_complete, true);
}
#endif

/* Targets/stages are fixed literals or entries from the closed tables above.
 * Capture errno at the failed call, not after logging or Vulkan cleanup.
 * Type checks and setvbuf have no defined syscall errno to report. */
static int setup_failed(struct probe *p, const char *stage, const char *target,
                        int call_rc, int error, bool errno_valid, unsigned mode)
{
    p->failure = stage;
    char line[768];
    const int n = snprintf(line, sizeof(line),
        "PS5LM_COMPUTE_SETUP_ERROR {\"stage\":\"%s\",\"target\":\"%s\","
        "\"call_rc\":%d,\"errno\":%d,\"errno_valid\":%s,\"mode\":%u}\n",
        stage, target, call_rc, error, errno_valid ? "true" : "false", mode);
    if (n > 0 && (size_t)n < sizeof(line))
        sceKernelDebugOutText(0, line);
    else
        sceKernelDebugOutText(0, "PS5LM_COMPUTE_SETUP_ERROR {\"stage\":\"diagnostic_format\",\"rc\":2}\n");
    return RC_SETUP;
}

/* Native lstat is rejected on this title. Follow the pinned native helper's
 * no-follow descriptor pattern, scoped to setup rather than emulating lstat.
 * O_NONBLOCK avoids a FIFO hang; special output types are still rejected.
 * Like the previous lstat checks, O_NOFOLLOW protects the final component. */
static int check_setup_path(struct probe *p, const char *path, bool directory)
{
    const int flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC |
                      (directory ? O_DIRECTORY : 0);
    const int fd = open(path, flags);
    if (fd < 0) {
        const int error = errno;
        if (!directory && error == ENOENT)
            return 0; /* The output may not exist yet; never create it here. */
        return setup_failed(p, directory ? "setup_open_directory" : "setup_open_output",
                            path, fd, error, true, 0);
    }
    struct stat info;
    const int stat_rc = fstat(fd, &info);
    const int stat_errno = stat_rc != 0 ? errno : 0;
    const int close_rc = close(fd);
    const int close_errno = close_rc != 0 ? errno : 0;
    if (stat_rc != 0)
        return setup_failed(p, directory ? "setup_fstat_directory" : "setup_fstat_output",
                            path, stat_rc, stat_errno, true, 0);
    if (close_rc != 0)
        return setup_failed(p, directory ? "setup_close_directory" : "setup_close_output",
                            path, close_rc, close_errno, true, 0);
    if (directory ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode))
        return setup_failed(p, directory ? "setup_directory_type" : "setup_output_type",
                            path, stat_rc, 0, false, info.st_mode);
    return 0;
}

static int setup_runtime(struct probe *p)
{
    for (size_t i = 0; i < sizeof(runtime_directories) / sizeof(runtime_directories[0]); ++i) {
        int rc = mkdir(runtime_directories[i], 0755);
        if (rc != 0 && errno != EEXIST)
            return setup_failed(p, "setup_mkdir", runtime_directories[i], rc, errno, true, 0);
        rc = check_setup_path(p, runtime_directories[i], true);
        if (rc != 0)
            return rc;
    }
    const int output_rc = check_setup_path(p, OUTPUT_PATH, false);
    if (output_rc != 0)
        return output_rc;
#if defined(PS5LM_GGML_MODEL)
    /* The model must already be there, a regular file of the pinned size.
     * Read-only, no-follow; llama.cpp reopens it read-only for loading. */
    const int model_fd = open(PS5LM_MODEL_PATH, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (model_fd < 0)
        return setup_failed(p, "setup_open_model", PS5LM_MODEL_PATH, model_fd, errno, true, 0);
    struct stat model_info;
    const int model_stat = fstat(model_fd, &model_info);
    const int model_errno = model_stat != 0 ? errno : 0;
    const int model_close = close(model_fd);
    if (model_stat != 0)
        return setup_failed(p, "setup_fstat_model", PS5LM_MODEL_PATH, model_stat, model_errno, true, 0);
    if (model_close != 0)
        return setup_failed(p, "setup_close_model", PS5LM_MODEL_PATH, model_close, errno, true, 0);
    if (!S_ISREG(model_info.st_mode) || (uint64_t)model_info.st_size != PS5LM_MODEL_BYTES)
        return setup_failed(p, "setup_model_type_or_size", PS5LM_MODEL_PATH, model_stat, 0, false,
                            model_info.st_mode);
#if defined(PS5LM_MODEL_VISION)
    /* The mmproj too: present, a regular file of the pinned size, read-only. */
    const int mmproj_fd = open(PS5LM_MMPROJ_PATH, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (mmproj_fd < 0)
        return setup_failed(p, "setup_open_mmproj", PS5LM_MMPROJ_PATH, mmproj_fd, errno, true, 0);
    struct stat mmproj_info;
    const int mmproj_stat = fstat(mmproj_fd, &mmproj_info);
    const int mmproj_errno = mmproj_stat != 0 ? errno : 0;
    const int mmproj_close = close(mmproj_fd);
    if (mmproj_stat != 0)
        return setup_failed(p, "setup_fstat_mmproj", PS5LM_MMPROJ_PATH, mmproj_stat, mmproj_errno, true, 0);
    if (mmproj_close != 0)
        return setup_failed(p, "setup_close_mmproj", PS5LM_MMPROJ_PATH, mmproj_close, errno, true, 0);
    if (!S_ISREG(mmproj_info.st_mode) || (uint64_t)mmproj_info.st_size != PS5LM_MMPROJ_BYTES)
        return setup_failed(p, "setup_mmproj_type_or_size", PS5LM_MMPROJ_PATH, mmproj_stat, 0, false,
                            mmproj_info.st_mode);
#endif
#endif
    for (size_t i = 0; i < sizeof(probe_environment) / sizeof(probe_environment[0]); ++i) {
        const int rc = setenv(probe_environment[i].name, probe_environment[i].value, 1);
        if (rc != 0)
            return setup_failed(p, "setup_setenv", probe_environment[i].name, rc, errno, true, 0);
    }
    /* The PS5 port sets /app0/radv-shader-cache with overwrite=0; our explicit
     * environment wins. disk_cache_enabled() honors "true" before file setup.
     * Both streams append to the ONLY runtime output file. Results use klog.
     * No /dev/null, pipe logger, loader redirection, or /app0 writes. */
    if (!freopen(OUTPUT_PATH, "a", stdout))
        return setup_failed(p, "setup_freopen_stdout", OUTPUT_PATH, 0, errno, true, 0);
    if (!freopen(OUTPUT_PATH, "a", stderr))
        return setup_failed(p, "setup_freopen_stderr", OUTPUT_PATH, 0, errno, true, 0);
#if defined(PS5LM_GGML_MODEL)
    int rc = setvbuf(stdout, stdout_buffer, _IOFBF, sizeof(stdout_buffer));
    if (rc != 0)
        return setup_failed(p, "setup_setvbuf_stdout", OUTPUT_PATH, rc, 0, false, 0);
    rc = setvbuf(stderr, stderr_buffer, _IOFBF, sizeof(stderr_buffer));
#else
    int rc = setvbuf(stdout, NULL, _IONBF, 0);
    if (rc != 0)
        return setup_failed(p, "setup_setvbuf_stdout", OUTPUT_PATH, rc, 0, false, 0);
    rc = setvbuf(stderr, NULL, _IONBF, 0);
#endif
    if (rc != 0)
        return setup_failed(p, "setup_setvbuf_stderr", OUTPUT_PATH, rc, 0, false, 0);
    return 0;
}

int main(void)
{
    const uint64_t start = now_ns();
    atomic_store(&start_ns, start);
#if defined(PS5LM_GGML_MODEL)
    main_thread = pthread_self();
#endif
    if (!start)
        terminate_probe(RC_CLOCK, "start_clock");
    /* This is a tuning call on the SAME SDK allocator, not a replacement heap.
     * Its initialization makes no heap allocation. The physical cap is already
     * active for pre-main constructors; 16 GiB reserved VA is not committed RAM. */
    if (!mspace_mallopt(-2, (int)BUFFER_LIMIT))
        terminate_probe(RC_SETUP, "heap_granularity");
    pthread_t timer;
    const int thread_rc = pthread_create(&timer, NULL, watchdog, NULL);
    if (thread_rc != 0)
        terminate_probe(RC_SETUP, "watchdog_create"); /* No Mesa without watchdog. */
    /* Deliberately remain joinable: no reaper, cancellation, or early disarm.
     * Shell-managed process closure kills this thread; until then it covers
     * setup, Mesa init/compile/submit, teardown AND a stuck close request.
     * Scheduling, thread creation and close IPC remain hardware limits. */
    atomic_store(&phase, 1);
    struct probe p = {.vk_rc = VK_SUCCESS, .failure = "runtime_setup"};
    int rc = setup_runtime(&p);
    if (!rc)
        rc = run_probe(&p);
    if (p.unsafe_submission) {
        if (!atomic_exchange(&result_claimed, true))
            emit_result(rc ? rc : RC_VULKAN, p.vk_rc, p.failure);
        close_probe(rc ? rc : RC_VULKAN); /* GPU owns its buffers; DO NOT free. */
    }
    cleanup_probe(&p);
    if (!rc && (!atomic_load(&cleanup_complete) || atomic_load(&direct_denials) ||
                atomic_load(&direct_errors))) {
        rc = RC_BUDGET;
        p.failure = "cleanup_or_direct_budget";
    }
    if (!rc && (atomic_load(&verified_words) != WORD_COUNT || atomic_load(&mismatches) ||
                !atomic_load(&fence_complete))) {
        rc = RC_MISMATCH;
        p.failure = "incomplete_readback";
    }
    const uint64_t end = now_ns();
    if (!end || end < start)
        terminate_probe(RC_CLOCK, "final_clock");
    if (end - start >= WATCHDOG_TIMEOUT_NS)
        terminate_probe(RC_WATCHDOG, "final_deadline");
    if (atomic_load(&terminal_rc))
        close_probe(atomic_load(&terminal_rc));
    atomic_store(&phase, 10);
    if (atomic_exchange(&result_claimed, true))
        close_probe(RC_WATCHDOG);
    emit_result(rc, p.vk_rc, rc ? p.failure : "complete");
    /* Marker is not proof of clean exit. Keep the watchdog armed until the
     * shell removes this process; do not return through the native CRT. */
    close_probe(rc);
}
