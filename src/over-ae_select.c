#include "redisassert.h"
#include "over-ae.h"
#include "zmalloc.h"
#include "errno.h"
#include <sys/select.h>
#include <string.h>

typedef struct aeApiState {
    fd_set rfds; // 监听可读
    fd_set wfds; // 监听可写
    // 文件描述符的拷贝,我们需要有一个fd集的副本,因为在select()之后重复使用FD集是不安全的.
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
    // 只需确保fd_set类型中有足够的空间。
    if (setsize >= FD_SETSIZE) {
        return -1; // select 最多支持1024个fd
    }
    return 0;
}

static void aeApiFree(aeEventLoop *eventLoop) {
    zfree(eventLoop->apidata);
}

static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;

    if (mask & AE_READABLE) {
        FD_SET(fd, &state->rfds);
    }
    if (mask & AE_WRITABLE) {
        FD_SET(fd, &state->wfds);
    }
    return 0;
}

static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    if (mask & AE_READABLE) {
        FD_CLR(fd, &state->rfds); // 从读集合队列中移除该fd
    }
    if (mask & AE_WRITABLE) {
        FD_CLR(fd, &state->wfds); // 从写集合队列中移除该fd
    }
}
// 获取就绪的网络事件
static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int retval, j, numevents = 0;

    memcpy(&state->_rfds, &state->rfds, sizeof(fd_set));
    memcpy(&state->_wfds, &state->wfds, sizeof(fd_set));
    // select()用来等待文件描述符状态的改变。
    // 参数n代表最大的文件描述词加1
    // 参数readfds、writefds和exceptfds 称为描述词组，是用来回传该描述词的读，写或例外的状况。

    // tvp 阻塞时间
    retval = select(eventLoop->maxfd + 1, &state->_rfds, &state->_wfds, NULL, tvp);
    // 执行成功则返回文件描述词状态已改变的个数

    if (retval > 0) {
        for (j = 0; j <= eventLoop->maxfd; j++) {
            // 遍历现有所有的描述符,判断是否在 读、写响应集合中
            int mask = 0;
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
        // EBADF 文件描述词为无效的或该文件已关闭
        // EINTR 此调用被信号所中断
        // EINVAL 参数n为负值。
        // ENOMEM 核心内存不足
        panic("aeApiPoll: select, %s", strerror(errno));
    }

    return numevents;
}

static char *aeApiName(void) {
    return "select";
}
