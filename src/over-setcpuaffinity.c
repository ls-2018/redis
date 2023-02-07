#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef __linux__
#include <sched.h>
#endif
#ifdef __FreeBSD__
#include <sys/param.h>
#include <sys/cpuset.h>
#endif
#ifdef __DragonFly__
#include <pthread.h>
#include <pthread_np.h>
#endif
#ifdef __NetBSD__
#include <pthread.h>
#include <sched.h>
#endif

#include "over-config.h"

#ifdef USE_SETCPUAFFINITY
static const char *next_token(const char *q, int sep) {
    if (q)
        q = strchr(q, sep);
    if (q)
        q++;

    return q;
}

static int next_num(const char *str, char **end, int *result) {
    if (!str || *str == '\0' || !isdigit(*str))
        return -1;

    *result = strtoul(str, end, 10);
    if (str == *end)
        return -1;

    return 0;
}

/* set current thread cpu affinity to cpu list, this function works like
 * taskset command (actually cpulist parsing logic reference to util-linux).
 * example of this function: "0,2,3", "0,2-3", "0-20:2". */
void setcpuaffinity(const char *cpulist) {
    const char *p, *q;
    char *end = NULL;
#ifdef __linux__
    cpu_set_t cpuset;
#endif
#if defined(__FreeBSD__) || defined(__DragonFly__)
    cpuset_t cpuset;
#endif
#ifdef __NetBSD__
    cpuset_t *cpuset;
#endif

    if (!cpulist)
        return;

#ifndef __NetBSD__
    CPU_ZERO(&cpuset);
#else
    cpuset = cpuset_create();
#endif

    q = cpulist;
    while (p = q, q = next_token(q, ','), p) {
        int a, b, s;
        const char *c1, *c2;

        if (next_num(p, &end, &a) != 0)
            return;

        b = a;
        s = 1;
        p = end;

        c1 = next_token(p, '-');
        c2 = next_token(p, ',');

        if (c1 != NULL && (c2 == NULL || c1 < c2)) {
            if (next_num(c1, &end, &b) != 0)
                return;

            c1 = end && *end ? next_token(end, ':') : NULL;
            if (c1 != NULL && (c2 == NULL || c1 < c2)) {
                if (next_num(c1, &end, &s) != 0)
                    return;

                if (s == 0)
                    return;
            }
        }

        if ((a > b))
            return;

        while (a <= b) {
#ifndef __NetBSD__
            CPU_SET(a, &cpuset);
#else
            cpuset_set(a, cpuset);
#endif
            a += s;
        }
    }

    if (end && *end)
        return;
// 将当前线程绑定至特定CPU
#ifdef __linux__
    sched_setaffinity(0, sizeof(cpuset), &cpuset);
#endif
#ifdef __FreeBSD__
    cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, sizeof(cpuset), &cpuset);
#endif
#ifdef __DragonFly__
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
#endif
#ifdef __NetBSD__
    pthread_setaffinity_np(pthread_self(), cpuset_size(cpuset), cpuset);
    cpuset_destroy(cpuset);
#endif
}

#endif /* USE_SETCPUAFFINITY */
