/* Bounded discovery: query budgets, touch 8 MiB, list project models.
 * No locking, direct-memory allocations, worker threads or ceiling tests.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../ps5test.h"
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ps5/kernel.h>

int sceKernelGetHwModelName(char *name);
long sceKernelGetCpuFrequency(void);
int sceKernelGetCpuTemperature(int *celsius);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelConfiguredFlexibleMemorySize(size_t *size);
int sceKernelAvailableFlexibleMemorySize(size_t *size);
int scePthreadGetaffinity(pthread_t thread, uint64_t *mask);

int main(void) {
    const double start = ps5test_now_ms();
    ps5test_init("discovery");
    char model[256] = {0};
    int temp = 0;
    uint64_t mask = 0;
    size_t configured = 0, available = 0;
    int model_rc = sceKernelGetHwModelName(model);
    printf("model rc=%d name=%s\n", model_rc, model);
    unsigned fw = kernel_get_fw_version();
    printf("firmware=0x%08x cpu_mhz=%ld\n", fw,
           sceKernelGetCpuFrequency() / 1000000);
    int temp_rc = sceKernelGetCpuTemperature(&temp);
    printf("temperature rc=%d celsius=%d\n", temp_rc, temp);
    int affinity_rc = scePthreadGetaffinity(pthread_self(), &mask);
    printf("affinity rc=%d mask=0x%llx online_cpus=%ld\n",
           affinity_rc, (unsigned long long)mask, sysconf(_SC_NPROCESSORS_ONLN));
    size_t direct = sceKernelGetDirectMemorySize();
    int configured_rc = sceKernelConfiguredFlexibleMemorySize(&configured);
    int available_rc = sceKernelAvailableFlexibleMemorySize(&available);
    printf("direct_bytes=%zu flexible_configured_bytes=%zu rc=%d "
           "flexible_available_bytes=%zu rc=%d\n", direct, configured,
           configured_rc, available, available_rc);

    const size_t bytes = 8 * 1024 * 1024;
    volatile unsigned char *p = malloc(bytes);
    int ok = p != NULL;
    if (p) {
        for (size_t i = 0; i < bytes; i += 4096) p[i] = 0x5a;
        for (size_t i = 0; i < bytes; i += 4096)
            if (p[i] != 0x5a) ok = 0;
        free((void *)p);
    }
    printf("8MiB allocation/touch/verify/free=%s\n", ok ? "pass" : "fail");

    DIR *dir = opendir("/data/PS5LM/models");
    if (!dir) printf("models directory unavailable errno=%d\n", errno);
    else {
        struct dirent *entry;
        unsigned count = 0;
        while (count < 100 && (entry = readdir(dir))) {
            if (entry->d_name[0] == '.') continue;
            char path[1024];
            struct stat st;
            snprintf(path, sizeof(path), "/data/PS5LM/models/%s", entry->d_name);
            if (stat(path, &st) == 0)
                printf("model_entry=%s bytes=%lld\n", entry->d_name, (long long)st.st_size);
            count++;
        }
        closedir(dir);
        printf("model_entries=%u (listing capped at 100)\n", count);
    }
    char metrics[512];
    snprintf(metrics, sizeof(metrics),
             "\"firmware\":%u,\"direct_bytes\":%zu,\"flexible_configured_bytes\":%zu,"
             "\"flexible_available_bytes\":%zu,\"affinity_mask\":%llu,"
             "\"configured_rc\":%d,\"available_rc\":%d,\"affinity_rc\":%d,"
             "\"allocation_bytes\":%zu", fw, direct, configured, available,
             (unsigned long long)mask, configured_rc, available_rc, affinity_rc, bytes);
    ps5test_result("discovery", ok, start, metrics);
    return ok ? 0 : 1;
}
