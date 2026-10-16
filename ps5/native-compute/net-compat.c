/* SPDX-License-Identifier: GPL-3.0-or-later
 * Server mode only: libc and socket functions llama.cpp's server, its common
 * library and cpp-httplib reference that no module a title may import
 * provides (the link reported each one undefined). Adapted from cobanov/PS5LM
 * ps5/app/compat_app.c, keeping only what this link needs.
 *
 * Each is defined as ps5lm_NAME and bound with --defsym=NAME=ps5lm_NAME and a
 * local version-script entry (scripts/build-native-compute.sh), as mkstemp is:
 * a title that exported libc's own names would be refused by the converter.
 * None of them is on the inference path except the resolver, which cpp-httplib
 * uses even for the numeric address it binds to. localeconv is the exception
 * to "no module provides": libc's own reports a locale the JSON reader cannot
 * use, so it is replaced, not supplied.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pwd.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* The SDK's platform getaddrinfo refuses every lookup, and cpp-httplib binds
 * through getaddrinfo: numeric addresses and the passive wildcard only. */
static struct addrinfo *make_ai(int family, int socktype, int protocol, const void *addr, unsigned port)
{
    const size_t addrlen = family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    struct addrinfo *ai = calloc(1, sizeof(*ai) + addrlen);
    if (!ai)
        return NULL;
    ai->ai_family = family;
    ai->ai_socktype = socktype;
    ai->ai_protocol = protocol;
    ai->ai_addrlen = (socklen_t)addrlen;
    ai->ai_addr = (struct sockaddr *)(ai + 1);
    if (family == AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in *)ai->ai_addr;
        in->sin_len = sizeof(*in);
        in->sin_family = AF_INET;
        in->sin_port = htons((unsigned short)port);
        memcpy(&in->sin_addr, addr, sizeof(in->sin_addr));
    } else {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)ai->ai_addr;
        in6->sin6_len = sizeof(*in6);
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons((unsigned short)port);
        memcpy(&in6->sin6_addr, addr, sizeof(in6->sin6_addr));
    }
    return ai;
}

int ps5lm_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
    const int family = hints ? hints->ai_family : AF_UNSPEC;
    const int socktype = hints ? hints->ai_socktype : 0;
    const int protocol = hints ? hints->ai_protocol : 0;
    const int passive = hints && (hints->ai_flags & AI_PASSIVE);
    unsigned port = 0;
    if (!res)
        return EAI_FAIL;
    *res = NULL;
    if (service && *service) {
        char *end;
        const unsigned long v = strtoul(service, &end, 10);
        if (*end || v > 65535)
            return EAI_SERVICE;
        port = (unsigned)v;
    }
    /* No module exports in6addr_any or in6addr_loopback to a title. */
    static const struct in6_addr any6 = IN6ADDR_ANY_INIT, loopback6 = IN6ADDR_LOOPBACK_INIT;
    struct in_addr v4;
    struct in6_addr v6;
    if (!node || strcmp(node, "localhost") == 0) {
        if (family == AF_INET6) {
            v6 = !node && passive ? any6 : loopback6;
            *res = make_ai(AF_INET6, socktype, protocol, &v6, port);
        } else {
            v4.s_addr = htonl(!node && passive ? INADDR_ANY : INADDR_LOOPBACK);
            *res = make_ai(AF_INET, socktype, protocol, &v4, port);
        }
    } else if (family != AF_INET6 && inet_pton(AF_INET, node, &v4) == 1) {
        *res = make_ai(AF_INET, socktype, protocol, &v4, port);
    } else if (family != AF_INET && inet_pton(AF_INET6, node, &v6) == 1) {
        *res = make_ai(AF_INET6, socktype, protocol, &v6, port);
    } else {
        return EAI_NONAME; /* no names: the console has no resolver for a title */
    }
    return *res ? 0 : EAI_MEMORY;
}

void ps5lm_freeaddrinfo(struct addrinfo *ai)
{
    while (ai) {
        struct addrinfo *next = ai->ai_next;
        free(ai);
        ai = next;
    }
}

/* Numeric only: the HTTP server asks for the peer's numeric host and port. */
int ps5lm_getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host, size_t hostlen,
                      char *serv, size_t servlen, int flags)
{
    (void)salen;
    (void)flags;
    const void *addr;
    unsigned port;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        addr = &in->sin_addr;
        port = ntohs(in->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
        addr = &in6->sin6_addr;
        port = ntohs(in6->sin6_port);
    } else {
        return EAI_FAMILY;
    }
    if (host && hostlen && !inet_ntop(sa->sa_family, addr, host, (socklen_t)hostlen))
        return EAI_OVERFLOW;
    if (serv && servlen && snprintf(serv, servlen, "%u", port) >= (int)servlen)
        return EAI_OVERFLOW;
    return 0;
}

/* accept plus the flags; the reason goes to the log when accept fails, since
 * the listener stops on an error it does not know to be transient. */
int ps5lm_accept4(int s, struct sockaddr *addr, socklen_t *addrlen, int flags)
{
    const int fd = accept(s, addr, addrlen);
    if (fd < 0) {
        const int saved = errno;
        fprintf(stderr, "ps5lm-server: accept failed, errno %d\n", saved);
        errno = saved;
        return fd;
    }
    if (flags & SOCK_NONBLOCK) {
        const int fl = fcntl(fd, F_GETFL);
        if (fl != -1)
            (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
    if (flags & SOCK_CLOEXEC)
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

/* llama's log asks whether to use colours (it does not: --log-colors off). */
int ps5lm_isatty(int fd)
{
    (void)fd;
    errno = ENOTTY;
    return 0;
}

/* The home directory comes from HOME (the title's closed environment). */
struct passwd *ps5lm_getpwuid(uid_t uid)
{
    (void)uid;
    return NULL;
}

/* Days from 1970-01-01 to a civil date (Howard Hinnant's algorithm); for
 * cpp-httplib's HTTP date parsing. */
static long long days_from_civil(long long y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

time_t ps5lm_timegm(struct tm *tm)
{
    long long y = tm->tm_year + 1900LL;
    long long mon = tm->tm_mon;
    y += mon / 12;
    mon %= 12;
    if (mon < 0) {
        mon += 12;
        y--;
    }
    const long long days = days_from_civil(y, (unsigned)mon + 1, 1) + tm->tm_mday - 1;
    return (time_t)(days * 86400 + tm->tm_hour * 3600LL + tm->tm_min * 60LL + tm->tm_sec);
}

/* libc++'s filesystem and the router mode's /proc lookup; neither is used. */
ssize_t ps5lm_readlink(const char *path, char *buf, size_t size)
{
    (void)path;
    (void)buf;
    (void)size;
    errno = EINVAL; /* not a symbolic link */
    return -1;
}

int ps5lm_link(const char *from, const char *to)
{
    (void)from;
    (void)to;
    errno = ENOSYS;
    return -1;
}

int ps5lm_symlink(const char *from, const char *to)
{
    (void)from;
    (void)to;
    errno = ENOSYS;
    return -1;
}

/* libc++'s current_path sizes its buffer from _PC_PATH_MAX. */
long ps5lm_pathconf(const char *path, int name)
{
    (void)path;
    if (name == _PC_PATH_MAX)
        return PATH_MAX;
    errno = EINVAL;
    return -1;
}

/* Nothing the server does in this title deletes a file (no slot save path, no
 * downloads); refuse rather than let a request path remove one. */
int ps5lm_remove(const char *path)
{
    (void)path;
    errno = EPERM;
    return -1;
}

/* The console reports an empty decimal point while its strtod reads '.'.
 * nlohmann::json puts the reported point into a number before strtod, so a
 * float in any request ("temperature": 0.7) failed its assertion and took the
 * server down. This is the C locale's. From cobanov/PS5LM 4f57e0e. */
struct lconv *ps5lm_localeconv(void)
{
    static struct lconv c = {
        .decimal_point = ".", .thousands_sep = "", .grouping = "",
        .int_curr_symbol = "", .currency_symbol = "", .mon_decimal_point = "",
        .mon_thousands_sep = "", .mon_grouping = "", .positive_sign = "", .negative_sign = "",
        .int_frac_digits = CHAR_MAX, .frac_digits = CHAR_MAX, .p_cs_precedes = CHAR_MAX,
        .p_sep_by_space = CHAR_MAX, .n_cs_precedes = CHAR_MAX, .n_sep_by_space = CHAR_MAX,
        .p_sign_posn = CHAR_MAX, .n_sign_posn = CHAR_MAX, .int_p_cs_precedes = CHAR_MAX,
        .int_n_cs_precedes = CHAR_MAX, .int_p_sep_by_space = CHAR_MAX, .int_n_sep_by_space = CHAR_MAX,
        .int_p_sign_posn = CHAR_MAX, .int_n_sign_posn = CHAR_MAX,
    };
    return &c;
}
