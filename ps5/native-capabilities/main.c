/* Native-title preflight. Queries only: no GPU, allocation, elevation or file I/O.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/sysctl.h>

int sceKernelDebugOutText(int channel, const char *text);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelConfiguredFlexibleMemorySize(size_t *size);
int sceKernelAvailableFlexibleMemorySize(size_t *size);
int scePthreadGetaffinity(pthread_t thread, uint64_t *mask);
int sceShellCoreUtilExitApp(void);
int sceKernelUsleep(unsigned int microseconds);

static _Noreturn void close_title(int status) {
    char line[256];
    snprintf(line, sizeof(line),
             "[ps5lm-native] PS5LM_NATIVE_EXIT_REQUEST {\"intended_status\":%d}\n", status);
    sceKernelDebugOutText(0, line);
    int rc = sceShellCoreUtilExitApp();
    snprintf(line, sizeof(line),
             "[ps5lm-native] %s {\"stage\":\"shellcore_exit_app\",\"rc\":%d}\n",
             rc == 0 ? "PS5LM_NATIVE_EXIT_RETURN" : "PS5LM_NATIVE_ABORT", rc);
    sceKernelDebugOutText(0, line);
    /* Shell-mediated closure is asynchronous. Never fall through to libc exit;
     * on rejection, leave the failure visible and wait for manual Close Game. */
    for (;;)
        sceKernelUsleep(1000000);
}

int main(void) {
    sceKernelDebugOutText(0, "[ps5lm-native] begin capabilities; queries only\n");
    char model[256] = {0};
    size_t configured = 0, available = 0;
    uint64_t mask = 0;
    /* hw.model describes the CPU, not the console's CFI chassis identifier. */
    size_t model_size = sizeof(model);
    int model_rc = sysctlbyname("hw.model", model, &model_size, NULL, 0);
    model[sizeof(model) - 1] = '\0';
    int configured_rc = sceKernelConfiguredFlexibleMemorySize(&configured);
    int available_rc = sceKernelAvailableFlexibleMemorySize(&available);
    int affinity_rc = scePthreadGetaffinity(pthread_self(), &mask);
    size_t direct = sceKernelGetDirectMemorySize();
    char line[768];
    snprintf(line, sizeof(line), "[ps5lm-native] cpu_model=%s model_query=hw.model model_rc=%d online_cpus=%ld\n",
             model, model_rc, sysconf(_SC_NPROCESSORS_ONLN));
    sceKernelDebugOutText(0, line);
    int ok = model_rc == 0 && configured_rc == 0 && available_rc == 0 && affinity_rc == 0;
    snprintf(line, sizeof(line),
             "[ps5lm-native] PS5LM_NATIVE_RESULT {\"success\":%s,"
             "\"direct_bytes\":%zu,\"flexible_configured_bytes\":%zu,"
             "\"flexible_available_bytes\":%zu,\"configured_rc\":%d,"
             "\"available_rc\":%d,\"affinity_rc\":%d,\"affinity_mask\":%llu,"
             "\"gpu_tested\":false,\"allocation_bytes\":0}\n",
             ok ? "true" : "false", direct, configured, available,
             configured_rc, available_rc, affinity_rc, (unsigned long long)mask);
    sceKernelDebugOutText(0, line);
    /* No owned test resources remain. Let the shell perform native teardown;
     * neither the query result nor exit request proves a clean process exit. */
    close_title(ok ? 0 : 1);
}
