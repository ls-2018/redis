#include <sys/epoll.h>
// * 事件状态
typedef struct aeApiState {
    int epfd;                   // epoll实例的描述符
    struct epoll_event *events; // epoll_event结构体数组,记录监听事件
} aeApiState;

// * 创建一个新的 epoll 实例,并将它赋值给 eventLoop
static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = zmalloc(sizeof(aeApiState));

    if (!state)
        return -1;
    // 将epoll_event数组保存在aeApiState结构体变量state中
    state->events = zmalloc(sizeof(struct epoll_event) * eventLoop->setsize);
    if (!state->events) {
        zfree(state);
        return -1;
    }
    // 将epoll实例描述符保存在aeApiState结构体变量state中
    state->epfd = epoll_create(1024); // 1024只是内核的一个提示
    if (state->epfd == -1) {
        zfree(state->events);
        zfree(state);
        return -1;
    }
    anetCloexec(state->epfd);
    // 这样一来,eventLoop 结构体中就有了 epoll 实例和 epoll_event 数组的信息,这样就可以用来基于 epoll 创建和处理事件了
    eventLoop->apidata = state;
    return 0;
}

// * 调整事件槽大小
static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;

    state->events = zrealloc(state->events, sizeof(struct epoll_event) * setsize);
    return 0;
}

// * 释放 epoll 实例和事件槽
static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    close(state->epfd);
    zfree(state->events);
    zfree(state);
}

// * 关联给定事件到 fd
static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ee = {0}; // 创建epoll_event类型变量

    // 如果文件描述符fd对应的IO事件已存在,则操作类型为修改,否则为添加
    int op = eventLoop->events[fd].mask == AE_NONE ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
    // 注册事件到 epoll
    ee.events = 0;
    mask |= eventLoop->events[fd].mask; /* Merge old events */
    // 将可读或可写IO事件类型转换为epoll监听的类型EPOLLIN或EPOLLOUT
    if (mask & AE_READABLE) {
        ee.events |= EPOLLIN;
    }
    if (mask & AE_WRITABLE) {
        ee.events |= EPOLLOUT;
    }
    ee.data.fd = fd; // 将要监听的文件描述符赋值给ee
                     // 调用epoll_ctl实际创建监听事件
    if (epoll_ctl(state->epfd, op, fd, &ee) == -1) {
        return -1;
    }
    return 0;
}

// * 从 fd 中删除给定事件
static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask) {
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ee = {0}; /* avoid valgrind warning */
    int mask = eventLoop->events[fd].mask & (~delmask);

    ee.events = 0;
    if (mask & AE_READABLE)
        ee.events |= EPOLLIN;
    if (mask & AE_WRITABLE)
        ee.events |= EPOLLOUT;
    ee.data.fd = fd;
    if (mask != AE_NONE) {
        epoll_ctl(state->epfd, EPOLL_CTL_MOD, fd, &ee);
    }
    else {
        /* Note, Kernel < 2.6.9 requires a non null event pointer even for
         * EPOLL_CTL_DEL. */
        epoll_ctl(state->epfd, EPOLL_CTL_DEL, fd, &ee);
    }
}

// * 获取可执行事件
static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int retval, numevents = 0;
    // 调用epoll_wait获取监听到的事件,检测并返回内核中发生的网络IO事件
    retval = epoll_wait(state->epfd, state->events, eventLoop->setsize, tvp ? (tvp->tv_sec * 1000 + (tvp->tv_usec + 999) / 1000) : -1);
    // 有至少一个事件就绪？
    if (retval > 0) {
        int j;
        // 获得监听到的事件数量
        numevents = retval;
        // 针对每一个事件,进行处理
        for (j = 0; j < numevents; j++) {
            int mask = 0;

            struct epoll_event *e = state->events + j;

            if (e->events & EPOLLIN)
                mask |= AE_READABLE;
            if (e->events & EPOLLOUT)
                mask |= AE_WRITABLE;
            if (e->events & EPOLLERR)
                mask |= AE_WRITABLE | AE_READABLE;
            if (e->events & EPOLLHUP)
                mask |= AE_WRITABLE | AE_READABLE;
            eventLoop->fired[j].fd = e->data.fd;
            eventLoop->fired[j].mask = mask;
        }
    }
    else if (retval == -1 && errno != EINTR) {
        panic("aeApiPoll: epoll_wait, %s", strerror(errno));
    }
    // 返回已就绪事件个数
    return numevents;
}
//* 返回当前正在使用的 poll 库的名字
static char *aeApiName(void) {
    return "epoll";
}
