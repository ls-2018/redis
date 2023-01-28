#include "redisassert.h"
#include "ae.h"
#include "zmalloc.h"
#include "errno.h"
#include <sys/select.h>
#include <string.h>

typedef struct aeApiState {
    fd_set rfds, wfds;
    // 文件描述符的拷贝
    // 我们需要有一个fd集的副本,因为在select()之后重复使用FD集是不安全的.
    fd_set _rfds, _wfds;
} aeApiState;

static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = zmalloc(sizeof(aeApiState));

    if (!state) {
        return -1;
    }
    // 清空fd_set集合,即让fd_set集合不再包含任何文件句柄
    FD_ZERO(&state->rfds);
    FD_ZERO(&state->wfds);
    eventLoop->apidata = state;
    return 0;
}

static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    AE_NOTUSED(eventLoop);
    /* Just ensure we have enough room in the fd_set type. */
    if (setsize >= FD_SETSIZE)
        return -1;
    return 0;
}

static void aeApiFree(aeEventLoop *eventLoop) {
    zfree(eventLoop->apidata);
}

static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;

    if (mask & AE_READABLE)
        FD_SET(fd, &state->rfds);
    if (mask & AE_WRITABLE)
        FD_SET(fd, &state->wfds);
    return 0;
}

static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;

    if (mask & AE_READABLE)
        FD_CLR(fd, &state->rfds);
    if (mask & AE_WRITABLE)
        FD_CLR(fd, &state->wfds);
}

static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int         retval, j, numevents = 0;

    memcpy(&state->_rfds, &state->rfds, sizeof(fd_set));
    memcpy(&state->_wfds, &state->wfds, sizeof(fd_set));

    retval = select(eventLoop->maxfd + 1, &state->_rfds, &state->_wfds, NULL, tvp);
    if (retval > 0) {
        for (j = 0; j <= eventLoop->maxfd; j++) {
            int          mask = 0;
            aeFileEvent *fe = &eventLoop->events[j];

            if (fe->mask == AE_NONE)
                continue;
            if (fe->mask & AE_READABLE && FD_ISSET(j, &state->_rfds))
                mask |= AE_READABLE;
            if (fe->mask & AE_WRITABLE && FD_ISSET(j, &state->_wfds))
                mask |= AE_WRITABLE;
            eventLoop->fired[numevents].fd = j;
            eventLoop->fired[numevents].mask = mask;
            numevents++;
        }
    }
    else if (retval == -1 && errno != EINTR) {
        panic("aeApiPoll: select, %s", strerror(errno));
    }

    return numevents;
}

static char *aeApiName(void) {
    return "select";
}
