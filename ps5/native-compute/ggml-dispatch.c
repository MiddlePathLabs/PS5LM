/* SPDX-License-Identifier: GPL-3.0-or-later
 * Only the bounded ggml-ops build uses this closed static RADV dispatch path.
 * Failure closes the title without C++ unwinding or GPU resource destruction.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
int sceKernelDebugOutText(int channel, const char *text);

_Noreturn void ps5lm_ops_fatal(const char *stage);
_Noreturn void __wrap_abort(void)
{
    ps5lm_ops_fatal("ggml_abort");
}
void ps5lm_ops_submission(bool accepted);
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance, const char *);
static VkInstance instance;
static atomic_uint submissions;

/* Performance counters (docs/PORTING.md "Performance"): where a token's time
 * goes on the CPU side. Relaxed atomics and one monotonic clock read on each
 * side of a submit or a wait; nothing is logged per event. */
static atomic_ullong perf_dispatches, perf_submit_ns, perf_fence_waits, perf_fence_ns;
static atomic_ullong perf_semaphore_waits, perf_semaphore_ns, perf_graphs, perf_graph_ns;
static uint64_t graph_started; /* ggml computes graphs on one thread at a time */
static uint64_t mono_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0;
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
#define PERF_ADD(counter, value) atomic_fetch_add_explicit(&(counter), (value), memory_order_relaxed)

/* Runtime tuning knobs, read by patches/ggml-vulkan/0007 at the start of each
 * graph (fusion, graph optimisation, submit cadence) or at each mat-vec
 * pipeline choice (workgroup size). -1 keeps what the closed environment set
 * at device creation. Set only through ps5lm_vk_set_knob (the server's
 * POST /ps5lm/knobs), each value range-checked. The build's defaults are the
 * PS5LM_VK_DEFAULT_* values below. */
#ifndef PS5LM_VK_DEFAULT_DISABLE_FUSION
#define PS5LM_VK_DEFAULT_DISABLE_FUSION -1
#endif
#ifndef PS5LM_VK_DEFAULT_DISABLE_GRAPH_OPTIMIZE
#define PS5LM_VK_DEFAULT_DISABLE_GRAPH_OPTIMIZE -1
#endif
#ifndef PS5LM_VK_DEFAULT_NODES_PER_SUBMIT
#define PS5LM_VK_DEFAULT_NODES_PER_SUBMIT -1
#endif
#ifndef PS5LM_VK_DEFAULT_SUBMIT_MFLOP
#define PS5LM_VK_DEFAULT_SUBMIT_MFLOP -1
#endif
#ifndef PS5LM_VK_DEFAULT_DMMV_WG
#define PS5LM_VK_DEFAULT_DMMV_WG -1
#endif
#ifndef PS5LM_VK_DEFAULT_DISABLE_MMVQ
#define PS5LM_VK_DEFAULT_DISABLE_MMVQ -1
#endif
static const struct { const char *name; long long min, max; } knob_info[] = {
    {"disable_fusion", 0, 1},
    {"disable_graph_optimize", 0, 1},
    {"nodes_per_submit", 1, 100000},
    {"submit_mflop", 0, 10000000},
    {"dmmv_wg", 0, 1},
    {"disable_mmvq", 0, 1},
};
#define KNOB_COUNT (sizeof(knob_info) / sizeof(knob_info[0]))
static atomic_llong knobs[KNOB_COUNT] = {
    PS5LM_VK_DEFAULT_DISABLE_FUSION, PS5LM_VK_DEFAULT_DISABLE_GRAPH_OPTIMIZE,
    PS5LM_VK_DEFAULT_NODES_PER_SUBMIT, PS5LM_VK_DEFAULT_SUBMIT_MFLOP, PS5LM_VK_DEFAULT_DMMV_WG,
    PS5LM_VK_DEFAULT_DISABLE_MMVQ,
};
long long ps5lm_vk_knob(int which)
{
    return which >= 0 && (unsigned)which < KNOB_COUNT ?
        atomic_load_explicit(&knobs[which], memory_order_relaxed) : -1;
}
/* 0 set, -1 unknown name, -2 out of range. -1 always restores the default. */
int ps5lm_vk_set_knob(const char *name, long long value)
{
    for (unsigned i = 0; name && i < KNOB_COUNT; ++i) {
        if (strcmp(name, knob_info[i].name) != 0) continue;
        if (value != -1 && (value < knob_info[i].min || value > knob_info[i].max)) return -2;
        atomic_store(&knobs[i], value);
        return 0;
    }
    return -1;
}
void ps5lm_vk_graph_begin(void)
{
    graph_started = mono_ns();
}
void ps5lm_vk_graph_end(void)
{
    const uint64_t end = mono_ns();
    PERF_ADD(perf_graphs, 1);
    if (graph_started && end >= graph_started) PERF_ADD(perf_graph_ns, end - graph_started);
    graph_started = 0;
}
/* One JSON object: the counters since launch and the knobs. */
int ps5lm_vk_perf_json(char *out, size_t size)
{
#define LOAD(c) (unsigned long long)atomic_load_explicit(&(c), memory_order_relaxed)
    return snprintf(out, size,
        "{\"submissions\":%u,\"submit_ns\":%llu,\"dispatches\":%llu,\"fence_waits\":%llu,"
        "\"fence_wait_ns\":%llu,\"semaphore_waits\":%llu,\"semaphore_wait_ns\":%llu,"
        "\"graphs\":%llu,\"graph_ns\":%llu,\"knobs\":{\"disable_fusion\":%lld,"
        "\"disable_graph_optimize\":%lld,\"nodes_per_submit\":%lld,\"submit_mflop\":%lld,"
        "\"dmmv_wg\":%lld,\"disable_mmvq\":%lld}}",
        atomic_load(&submissions), LOAD(perf_submit_ns), LOAD(perf_dispatches), LOAD(perf_fence_waits),
        LOAD(perf_fence_ns), LOAD(perf_semaphore_waits), LOAD(perf_semaphore_ns), LOAD(perf_graphs),
        LOAD(perf_graph_ns), ps5lm_vk_knob(0), ps5lm_vk_knob(1), ps5lm_vk_knob(2), ps5lm_vk_knob(3),
        ps5lm_vk_knob(4), ps5lm_vk_knob(5));
#undef LOAD
}
#define TIMEOUT 2000000000ull
#if defined(PS5LM_GGML_MODEL)
#include "ggml-model.h"
/* A resident model: bounded large allocations, many small submissions.
 * Fence/semaphore waits stay at two seconds; idle waits stay refused. */
#define LIMIT ((VkDeviceSize)PS5LM_MODEL_GPU_ALLOCATION_MIB << 20)
#define ALIGNMENT_LIMIT 1048576u
#if defined(PS5LM_GGML_SERVER)
#include "ggml-server.h"
#define SUBMIT_LIMIT PS5LM_SERVER_SUBMIT_LIMIT
#else
#define SUBMIT_LIMIT PS5LM_MODEL_SUBMIT_LIMIT /* 8192 for the first models */
#endif
#define SUBMIT_BATCH_LIMIT 16u
#define COMMAND_LIMIT 64u
#define GROUP_DIM_LIMIT 65535u
#define GROUP_TOTAL_LIMIT (1ull << 26)
#define GROUP_Z_LIMIT 65535u
#define DEVICE_MEMORY_LIMIT ((VkDeviceSize)(PS5LM_MODEL_WEIGHTS_MIB + PS5LM_MODEL_CONTEXT_MIB) << 20)
#else
#define LIMIT 1048576u
#define ALIGNMENT_LIMIT LIMIT
#define SUBMIT_LIMIT 32u
#define SUBMIT_BATCH_LIMIT 4u
#define COMMAND_LIMIT 8u
#define GROUP_DIM_LIMIT 64u
#define GROUP_TOTAL_LIMIT 64ull
#define GROUP_Z_LIMIT 1u
#define DEVICE_MEMORY_LIMIT ((VkDeviceSize)128 << 20)
#endif
/* Live Vulkan device memory, by handle, so frees are exact. */
#define MEMORY_SLOTS 4096u
static struct { VkDeviceMemory memory; VkDeviceSize size; } memory_slots[MEMORY_SLOTS];
static atomic_flag memory_lock = ATOMIC_FLAG_INIT;
static VkDeviceSize memory_live, memory_peak;
static unsigned memory_allocations, memory_errors;

static PFN_vkVoidFunction device_proc(VkDevice device, const char *name)
{
    PFN_vkGetDeviceProcAddr get = (PFN_vkGetDeviceProcAddr)
        vk_icdGetInstanceProcAddr(instance, "vkGetDeviceProcAddr");
    if (!get) ps5lm_ops_fatal("missing_device_proc_addr");
    PFN_vkVoidFunction proc = get(device, name);
    if (!proc) ps5lm_ops_fatal("missing_device_proc");
    return proc;
}
static void lock_memory(void)
{
    while (atomic_flag_test_and_set_explicit(&memory_lock, memory_order_acquire))
        __asm__ volatile("pause");
}
static void unlock_memory(void)
{
    atomic_flag_clear_explicit(&memory_lock, memory_order_release);
}
unsigned long long ps5lm_vk_memory_peak_bytes(void)
{
    lock_memory();
    const VkDeviceSize peak = memory_peak;
    unlock_memory();
    return peak;
}
unsigned long long ps5lm_vk_memory_live_bytes(void)
{
    lock_memory();
    const VkDeviceSize live = memory_live;
    unlock_memory();
    return live;
}
unsigned ps5lm_vk_memory_errors(void)
{
    lock_memory();
    const unsigned count = memory_errors;
    unlock_memory();
    return count;
}
unsigned ps5lm_vk_memory_allocations(void)
{
    lock_memory();
    const unsigned count = memory_allocations;
    unlock_memory();
    return count;
}
static VkResult allocate(VkDevice d, const VkMemoryAllocateInfo *i,
                         const VkAllocationCallbacks *a, VkDeviceMemory *m)
{
    if (!i || !m || i->allocationSize > LIMIT) ps5lm_ops_fatal("ggml_memory_limit");
    lock_memory();
    unsigned slot = 0;
    while (slot < MEMORY_SLOTS && memory_slots[slot].memory != VK_NULL_HANDLE)
        ++slot;
    if (slot == MEMORY_SLOTS || i->allocationSize > DEVICE_MEMORY_LIMIT - memory_live) {
        unlock_memory();
        ps5lm_ops_fatal("ggml_device_memory_budget");
    }
    unlock_memory();
    VkResult rc = ((PFN_vkAllocateMemory)device_proc(d, "vkAllocateMemory"))(d, i, a, m);
    if (rc != VK_SUCCESS) return rc;
    lock_memory();
    memory_slots[slot].memory = *m;
    memory_slots[slot].size = i->allocationSize;
    memory_live += i->allocationSize;
    if (memory_live > memory_peak) memory_peak = memory_live;
    ++memory_allocations;
    unlock_memory();
    return rc;
}
static void free_memory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a)
{
    if (m != VK_NULL_HANDLE) {
        lock_memory();
        unsigned slot = 0;
        while (slot < MEMORY_SLOTS && memory_slots[slot].memory != m)
            ++slot;
        /* An untracked free means the accounting is incomplete: count it and
         * fail the run's summary, but still free (this may be teardown). */
        if (slot == MEMORY_SLOTS) {
            ++memory_errors;
        } else {
            memory_live -= memory_slots[slot].size;
            memory_slots[slot].memory = VK_NULL_HANDLE;
            memory_slots[slot].size = 0;
        }
        unlock_memory();
    }
    ((PFN_vkFreeMemory)device_proc(d, "vkFreeMemory"))(d, m, a);
}
static VkResult buffer(VkDevice d, const VkBufferCreateInfo *i,
                       const VkAllocationCallbacks *a, VkBuffer *b)
{
    if (!i || i->size > LIMIT) ps5lm_ops_fatal("ggml_buffer_limit");
    return ((PFN_vkCreateBuffer)device_proc(d, "vkCreateBuffer"))(d, i, a, b);
}
static void requirements(VkDevice d, VkBuffer b, VkMemoryRequirements *r)
{
    ((PFN_vkGetBufferMemoryRequirements)device_proc(d, "vkGetBufferMemoryRequirements"))(d, b, r);
    if (r->size > LIMIT || r->alignment > ALIGNMENT_LIMIT) ps5lm_ops_fatal("ggml_alignment_limit");
}
static VkResult wait_fences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t t)
{
    const uint64_t started = mono_ns();
    VkResult rc = ((PFN_vkWaitForFences)device_proc(d, "vkWaitForFences"))
        (d, n, f, all, t < TIMEOUT ? t : TIMEOUT);
    if (rc != VK_SUCCESS) ps5lm_ops_fatal("ggml_fence_failed");
    PERF_ADD(perf_fence_waits, 1);
    PERF_ADD(perf_fence_ns, mono_ns() - started);
    return rc;
}
static VkResult wait_semaphores(VkDevice d, const VkSemaphoreWaitInfo *i, uint64_t t)
{
    const uint64_t started = mono_ns();
    VkResult rc = ((PFN_vkWaitSemaphores)device_proc(d, "vkWaitSemaphores"))
        (d, i, t < TIMEOUT ? t : TIMEOUT);
    if (rc != VK_SUCCESS) ps5lm_ops_fatal("ggml_semaphore_failed");
    PERF_ADD(perf_semaphore_waits, 1);
    PERF_ADD(perf_semaphore_ns, mono_ns() - started);
    return rc;
}
static _Noreturn VkResult device_idle(VkDevice d)
{
    (void)d;
    ps5lm_ops_fatal("ggml_device_idle_refused");
}
static _Noreturn VkResult queue_idle(VkQueue q)
{
    (void)q;
    ps5lm_ops_fatal("ggml_queue_idle_refused");
}
/* Queue/command handles have no device parameter. Bind these entry points once
 * at lookup, before ggml submits; this title creates exactly one Vulkan device. */
static PFN_vkQueueSubmit real_submit;
static PFN_vkCmdDispatch real_dispatch;
unsigned ps5lm_submission_count(void)
{
    return atomic_load(&submissions);
}
static VkResult submit(VkQueue q, uint32_t n, const VkSubmitInfo *i, VkFence f)
{
    if (!real_submit || n > SUBMIT_BATCH_LIMIT || atomic_fetch_add(&submissions, 1) >= SUBMIT_LIMIT)
        ps5lm_ops_fatal("ggml_submission_limit");
    for (uint32_t j = 0; j < n; ++j)
        if (i[j].commandBufferCount > COMMAND_LIMIT) ps5lm_ops_fatal("ggml_command_limit");
    ps5lm_ops_submission(false);
    const uint64_t started = mono_ns();
    VkResult rc = real_submit(q, n, i, f);
    if (rc != VK_SUCCESS) ps5lm_ops_fatal("ggml_submit_failed");
    PERF_ADD(perf_submit_ns, mono_ns() - started);
    ps5lm_ops_submission(true);
    return rc;
}
static void dispatch(VkCommandBuffer b, uint32_t x, uint32_t y, uint32_t z)
{
    if (!real_dispatch || !x || !y || !z || x > GROUP_DIM_LIMIT || y > GROUP_DIM_LIMIT ||
        z > GROUP_Z_LIMIT || (uint64_t)x * y * z > GROUP_TOTAL_LIMIT) {
        char line[96];
        snprintf(line, sizeof line, "PS5LM_DISPATCH_LIMIT {\"x\":%u,\"y\":%u,\"z\":%u}\n", x, y, z);
        sceKernelDebugOutText(0, line);
        ps5lm_ops_fatal("ggml_dispatch_limit");
    }
    PERF_ADD(perf_dispatches, 1);
    real_dispatch(b, x, y, z);
}
PFN_vkVoidFunction ps5lm_vkGetDeviceProcAddr(VkDevice, const char *);
static PFN_vkVoidFunction guarded(const char *name, PFN_vkVoidFunction real)
{
    if (!real) return NULL; /* Do not advertise unsupported extensions. */
#define ENTRY(n, f) if (strcmp(name, "vk" n) == 0) return (PFN_vkVoidFunction)f
    ENTRY("GetDeviceProcAddr", ps5lm_vkGetDeviceProcAddr);
    ENTRY("AllocateMemory", allocate);
    ENTRY("FreeMemory", free_memory);
    ENTRY("CreateBuffer", buffer);
    ENTRY("GetBufferMemoryRequirements", requirements);
    ENTRY("WaitForFences", wait_fences);
    ENTRY("WaitSemaphores", wait_semaphores);
    ENTRY("WaitSemaphoresKHR", wait_semaphores);
    ENTRY("DeviceWaitIdle", device_idle);
    ENTRY("QueueWaitIdle", queue_idle);
    if (strcmp(name, "vkQueueSubmit") == 0) {
        real_submit = (PFN_vkQueueSubmit)real;
        return (PFN_vkVoidFunction)submit;
    }
    if (strcmp(name, "vkCmdDispatch") == 0) {
        real_dispatch = (PFN_vkCmdDispatch)real;
        return (PFN_vkVoidFunction)dispatch;
    }
#undef ENTRY
    return real;
}
PFN_vkVoidFunction ps5lm_vkGetInstanceProcAddr(VkInstance i, const char *name)
{
    if (i) instance = i;
    if (strcmp(name, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)ps5lm_vkGetInstanceProcAddr;
    return guarded(name, vk_icdGetInstanceProcAddr(i, name));
}
PFN_vkVoidFunction ps5lm_vkGetDeviceProcAddr(VkDevice d, const char *name)
{
    PFN_vkGetDeviceProcAddr get = (PFN_vkGetDeviceProcAddr)
        vk_icdGetInstanceProcAddr(instance, "vkGetDeviceProcAddr");
    if (!get) ps5lm_ops_fatal("missing_device_proc_addr");
    /* Vulkan-Hpp probes unsupported extensions during dispatcher init. */
    return guarded(name, get(d, name));
}
