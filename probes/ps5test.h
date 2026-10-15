/* PS5LM test payload helpers, read by scripts/ps5-test.sh.
 *
 * Payload Manager starts a payload with stdout going nowhere, so
 * ps5test_init() points stdout and stderr at /data/PS5LM/tests/NAME.out,
 * which ps5-test.sh fetches over FTP once the process is gone. The exit code
 * comes from the kernel log ("calls exit() exit_value=N").
 *
 * A test says how it went in one final line:
 *
 *   PS5TEST_RESULT {"test":"hello","success":true,"duration_ms":12,"metrics":{...}}
 *
 * The JSON is the payload's own; ps5-test.sh copies it into result.json as
 * "reported".
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PS5LM_PS5TEST_H
#define PS5LM_PS5TEST_H

#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PS5TEST_DIR "/data/PS5LM/tests"

/* Call first in main. Line-buffered, so a crash loses at most a partial line. */
static inline void ps5test_init(const char *test) {
    char path[256];
    mkdir("/data/PS5LM", 0777);
    mkdir(PS5TEST_DIR, 0777);
    snprintf(path, sizeof(path), PS5TEST_DIR "/%s.out", test);
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        fflush(stdout);
        fflush(stderr);
        dup2(fd, 1);
        dup2(fd, 2);
        close(fd);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("PS5TEST_BEGIN {\"test\":\"%s\",\"pid\":%d}\n", test, (int)getpid());
}

static inline double ps5test_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec * 1e-6;
}

/* metrics_json is the body of the "metrics" object, e.g. "\"gbps\":41.2",
 * or "" for none. */
static inline void ps5test_result(const char *test, int success, double t0_ms,
                                  const char *metrics_json) {
    printf("PS5TEST_RESULT {\"test\":\"%s\",\"success\":%s,\"duration_ms\":%.0f,\"metrics\":{%s}}\n",
           test, success ? "true" : "false", ps5test_now_ms() - t0_ms,
           metrics_json ? metrics_json : "");
    fflush(stdout);
}

#endif
