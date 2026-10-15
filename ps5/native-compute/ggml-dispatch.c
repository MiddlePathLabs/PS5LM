/* SPDX-License-Identifier: GPL-3.0-or-later
 * Only the bounded ggml-ops build uses this closed static RADV dispatch path.
 * Failure closes the title without C++ unwinding or GPU resource destruction.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>

_Noreturn void ps5lm_ops_fatal(const char *stage);
_Noreturn void __wrap_abort(void)
{
    ps5lm_ops_fatal("ggml_abort");
}
void ps5lm_ops_submission(bool accepted);
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance, const char *);
static VkInstance instance;
static atomic_uint submissions;
#define TIMEOUT 2000000000ull
#if defined(PS5LM_GGML_MODEL)
#include "ggml-model.h"
/* A resident model: bounded large allocations, many small submissions.
 * Fence/semaphore waits stay at two seconds; idle waits stay refused. */
#define LIMIT ((VkDeviceSize)PS5LM_MODEL_GPU_ALLOCATION_MIB << 20)
#define ALIGNMENT_LIMIT 1048576u
#define SUBMIT_LIMIT 8192u
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
    VkResult rc = ((PFN_vkWaitForFences)device_proc(d, "vkWaitForFences"))
        (d, n, f, all, t < TIMEOUT ? t : TIMEOUT);
    if (rc != VK_SUCCESS) ps5lm_ops_fatal("ggml_fence_failed");
    return rc;
}
static VkResult wait_semaphores(VkDevice d, const VkSemaphoreWaitInfo *i, uint64_t t)
{
    VkResult rc = ((PFN_vkWaitSemaphores)device_proc(d, "vkWaitSemaphores"))
        (d, i, t < TIMEOUT ? t : TIMEOUT);
    if (rc != VK_SUCCESS) ps5lm_ops_fatal("ggml_semaphore_failed");
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
    VkResult rc = real_submit(q, n, i, f);
    if (rc != VK_SUCCESS) ps5lm_ops_fatal("ggml_submit_failed");
    ps5lm_ops_submission(true);
    return rc;
}
static void dispatch(VkCommandBuffer b, uint32_t x, uint32_t y, uint32_t z)
{
    if (!real_dispatch || !x || !y || !z || x > GROUP_DIM_LIMIT || y > GROUP_DIM_LIMIT ||
        z > GROUP_Z_LIMIT || (uint64_t)x * y * z > GROUP_TOTAL_LIMIT)
        ps5lm_ops_fatal("ggml_dispatch_limit");
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
