/* PS5LM launch: start one registered native title, then leave.
 *
 * scripts/ps5-native-test.sh loads this through Payload Manager in place of a
 * hand pressing Play. It refuses when any application is already running (a
 * game left open, or the last run's title that never closed), starts the
 * title, waits until the console reports it running and exits. Watching and
 * closing the title are the script's and the title's job, not this one's.
 *
 * The title id is fixed at build time (launch-PPSA99041.elf, see the
 * Makefile), and only the titles PS5LM registers are accepted.
 *
 * The launch calls, the parameter layout and the order they are tried in come
 * from Mihawk-99's ps5vkctl (PS5_Vulkan, payload/ps5vkctl/main.c, GPL-3.0),
 * which took them from etaHEN, AirPSX and ps5-payload-manager.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include <string.h>

#include "../ps5test.h"

#ifndef TITLE_ID
#error "build with -DTITLE_ID=\"PPSA99041\" (make launch-PPSA99041.elf)"
#endif

#define NAME "launch-" TITLE_ID
#define NO_APP 0xffffffffu
#define ALREADY_RUNNING 0x8094000cu

/* The launch parameter etaHEN's launcher passes, field for field. */
typedef struct {
    uint32_t size;
    int user_id;
    uint32_t app_opt;
    uint64_t crash_report;
    uint64_t check_flag;
} lnc_app_param_t;

typedef struct {
    int user_id[4];
} login_user_id_list_t;

uint32_t sceLncUtilGetAppIdOfRunningBigApp(void);
int sceLncUtilGetAppTitleId(uint32_t app_id, char *title_id);
int sceLncUtilLaunchApp(const char *title_id, const char *argv[], lnc_app_param_t *param);
int sceSystemServiceLaunchApp(const char *title_id, const char *argv[], lnc_app_param_t *param);
int sceUserServiceInitialize(void *param);
int sceUserServiceGetForegroundUser(int *user_id);
int sceUserServiceGetLoginUserIdList(login_user_id_list_t *list);

static const char *const allowed[] = {"PPSA99040", "PPSA99041"};

/* The running application and its title id, NO_APP when there is none. */
static uint32_t running_app(char *title, size_t size) {
    title[0] = '\0';
    const uint32_t app_id = sceLncUtilGetAppIdOfRunningBigApp();
    if (app_id == NO_APP || app_id == 0)
        return NO_APP;
    char buffer[32] = {0};
    if (sceLncUtilGetAppTitleId(app_id, buffer) != 0)
        buffer[0] = '\0';
    snprintf(title, size, "%s", buffer);
    return app_id;
}

static int login_user(void) {
    login_user_id_list_t users;
    memset(&users, 0, sizeof(users));
    if (sceUserServiceGetLoginUserIdList(&users) != 0)
        return -1;
    for (int i = 0; i < 4; i++)
        if (users.user_id[i] >= 0)
            return users.user_id[i];
    return -1;
}

typedef struct {
    const char *name;
    int (*call)(const char *, const char *[], lnc_app_param_t *);
    int user_id;
    uint64_t check_flag;
} attempt_t;

static int fail(double t0, const char *stage, const char *detail) {
    printf("%s: %s: %s\n", NAME, stage, detail);
    char metrics[384];
    snprintf(metrics, sizeof(metrics), "\"title\":\"%s\",\"stage\":\"%s\",\"detail\":\"%s\"",
             TITLE_ID, stage, detail);
    ps5test_result(NAME, 0, t0, metrics);
    return 1;
}

int main(void) {
    const double t0 = ps5test_now_ms();
    ps5test_init(NAME);

    int ok = 0;
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
        ok |= strcmp(TITLE_ID, allowed[i]) == 0;
    if (!ok)
        return fail(t0, "refused", "not a PS5LM title");

    const int init = sceUserServiceInitialize(NULL);
    printf("%s: sceUserServiceInitialize -> 0x%08x\n", NAME, (unsigned)init);

    char running[32];
    const uint32_t busy = running_app(running, sizeof(running));
    if (busy != NO_APP) {
        char detail[96];
        snprintf(detail, sizeof(detail), "app %u (%s) is already running", busy,
                 running[0] ? running : "unknown title");
        return fail(t0, "busy", detail);
    }

    int foreground = -1;
    if (sceUserServiceGetForegroundUser(&foreground) != 0)
        foreground = -1;
    const int login = login_user();
    const attempt_t attempts[] = {
        {"lncUtilApp/foreground", sceLncUtilLaunchApp, foreground, 0},
        {"lncUtilApp/login", sceLncUtilLaunchApp, login, 0},
        {"systemServiceApp/login", sceSystemServiceLaunchApp, login, 0},
        {"lncUtilApp/login+skipCheck", sceLncUtilLaunchApp, login, 1},
    };

    for (size_t i = 0; i < sizeof(attempts) / sizeof(attempts[0]); i++) {
        const attempt_t *a = &attempts[i];
        if (a->user_id < 0)
            continue;
        lnc_app_param_t param;
        memset(&param, 0, sizeof(param));
        param.size = sizeof(param);
        param.user_id = a->user_id;
        param.check_flag = a->check_flag;
        const uint32_t rc = (uint32_t)a->call(TITLE_ID, NULL, &param);
        printf("%s: %s as user %d -> 0x%08x\n", NAME, a->name, a->user_id, rc);

        /* The console decides, not the return code: wait up to 10 s for the
         * title to be the running application before trying another call. */
        uint32_t app_id = NO_APP;
        for (int wait = 0; wait < 40; wait++) {
            app_id = running_app(running, sizeof(running));
            if (app_id != NO_APP && strcmp(running, TITLE_ID) == 0)
                break;
            app_id = NO_APP;
            usleep(250000);
        }
        if (app_id != NO_APP || rc == ALREADY_RUNNING) {
            char metrics[256];
            snprintf(metrics, sizeof(metrics),
                     "\"title\":\"%s\",\"via\":\"%s\",\"user_id\":%d,\"rc\":%u,\"app_id\":%u",
                     TITLE_ID, a->name, a->user_id, rc, app_id);
            printf("%s: %s is running (app %u)\n", NAME, TITLE_ID, app_id);
            ps5test_result(NAME, 1, t0, metrics);
            return 0;
        }
        /* Something other than our title came up: never try again over it. */
        if (running_app(running, sizeof(running)) != NO_APP)
            return fail(t0, "wrong_app", running);
    }

    char detail[96];
    snprintf(detail, sizeof(detail), "no call started it (foreground user %d, login user %d)",
             foreground, login);
    return fail(t0, "launch", detail);
}
