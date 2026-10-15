/* PS5LM hello: the smallest payload, and the fixtures that test
 * scripts/ps5-test.sh itself. One source, four ELFs (see the Makefile):
 *
 *   hello.elf        prints a few lines and reports success
 *   hello-fail.elf   reports failure (the test ran, the check did not pass)
 *   hello-crash.elf  dies on a null write before reporting
 *   hello-hang.elf   never finishes; the wrapper's timeout has to kill it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../ps5test.h"

#ifndef HELLO_MODE
#define HELLO_MODE 0
#endif
#define MODE_OK    0
#define MODE_FAIL  1
#define MODE_CRASH 2
#define MODE_HANG  3

static const char *const names[] = {"hello", "hello-fail", "hello-crash", "hello-hang"};

int main(void) {
    const double t0 = ps5test_now_ms();
    const char *name = names[HELLO_MODE];

    ps5test_init(name);
    printf("PS5LM %s, pid %d\n", name, (int)getpid());

    if (HELLO_MODE == MODE_CRASH) {
        volatile int *p = NULL;
        *p = 1;
    }
    if (HELLO_MODE == MODE_HANG) {
        for (;;)
            sleep(1);
    }

    printf("%s: done\n", name);
    ps5test_result(name, HELLO_MODE == MODE_OK, t0, "\"answer\":42");
    return 0;
}
