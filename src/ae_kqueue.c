#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

typedef struct aeApiState {
    int kqfd;              // kqueue 句柄
    struct kevent *events; //
    char *eventsMask;      // 事件掩码用于合并读写事件.为了减少内存消耗,我们使用2位来存储一个事件的掩码,因此1字节将存储4个事件的掩码.
} aeApiState;

#define EVENT_MASK_MALLOC_SIZE(sz) (((sz) + 3) / 4)
#define EVENT_MASK_OFFSET(fd) ((fd) % 4 * 2)
#define EVENT_MASK_ENCODE(fd, mask) (((mask)&0x3) << EVENT_MASK_OFFSET(fd))

static inline int getEventMask(const char *eventsMask, int fd) {
    return (eventsMask[fd / 4] >> EVENT_MASK_OFFSET(fd)) & 0x3;
}

static inline void addEventMask(char *eventsMask, int fd, int mask) {
    eventsMask[fd / 4] |= EVENT_MASK_ENCODE(fd, mask);
}

static inline void resetEventMask(char *eventsMask, int fd) {
    eventsMask[fd / 4] &= ~EVENT_MASK_ENCODE(fd, 0x3);
}

static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = zmalloc(sizeof(aeApiState));

    if (!state)
        return -1;
    state->events = zmalloc(sizeof(struct kevent) * eventLoop->setsize);
    if (!state->events) {
        zfree(state);
        return -1;
    }
    state->kqfd = kqueue();
    if (state->kqfd == -1) {
        zfree(state->events);
        zfree(state);
        return -1;
    }
    anetCloexec(state->kqfd);
    state->eventsMask = zmalloc(EVENT_MASK_MALLOC_SIZE(eventLoop->setsize));
    memset(state->eventsMask, 0, EVENT_MASK_MALLOC_SIZE(eventLoop->setsize));
    eventLoop->apidata = state;
    return 0;
}

static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;

    state->events = zrealloc(state->events, sizeof(struct kevent) * setsize);
    state->eventsMask = zrealloc(state->eventsMask, EVENT_MASK_MALLOC_SIZE(setsize));
    memset(state->eventsMask, 0, EVENT_MASK_MALLOC_SIZE(setsize));
    return 0;
}

static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    close(state->kqfd);
    zfree(state->events);
    zfree(state->eventsMask);
    zfree(state);
}

/*

ident 是事件唯一的 key,在 socket 使用中,它是 socket 的 fd 句柄

filter 是事件的类型,有 15 种,其中几种常用的是
        EVFILT_READ   socket 可读事件
        EVFILT_WRITE  socket 可写事件
        EVFILT_SIGNAL  unix 系统的各种信号
        EVFILT_TIMER  定时器事件
        EVFILT_USER  用户自定义的事件

flags   操作方式
        EV_ADD 添加
        EV_DELETE 删除
        EV_ENABLE 激活
        EV_DISABLE 不激活

fflags 第二种操作方式,NOTE_TRIGGER 立即激活等等
data int 型的用户数据,socket 里面它是可读写的数据长度
udata 指针类型的数据,你可以携带任何想携带的附加数据.比如对象

 */

/*
kevent 函数
    kq 就是 kqueue 的句柄
    changelist 是 kevent 的数组,就是一次可以添加多个事件
    nchanges 是 changelist 数组长度
    eventlist 是待接收事件的数组,里面是空的,准备给 kqueue 放数据的
    nevents 是 eventlist 数组长度    传了 eventlist,kevent() 将会阻塞等待事件发生才返回,返回的全部事件在 eventlist 数组里面.
    timeout 是阻塞超时时间,超过这个时间就不阻塞了,直接返回
 */

// 对应描述符要监听对应的事件,
static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    // 从eventLoop结构体中获取aeApiState变量,里面保存了epoll实例
    aeApiState *state = eventLoop->apidata;
    struct kevent ke;

    // 在changes列表中注册标准输入流的读事件 以及 标准输出流的写事件
    // 最后一个参数可以是任意的附加数据（void * 类型）,在这里给事件附上了当前的文件描述符,后面会用到
    if (mask & AE_READABLE) {
        // EV_SET 是用于初始化kevent结构的便利宏,其签名为:
        EV_SET(&ke, fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
        // 已经就绪的文件描述符数量
        if (kevent(state->kqfd, &ke, 1, NULL, 0, NULL) == -1) {
            return -1;
        }
    }
    if (mask & AE_WRITABLE) {
        EV_SET(&ke, fd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
        if (kevent(state->kqfd, &ke, 1, NULL, 0, NULL) == -1) {
            return -1;
        }
    }
    return 0;
}

static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    struct kevent ke; // kqueue 封装的结构体

    if (mask & AE_READABLE) {
        // 在ke中注册标准输入流的读事件 以及 标准输出流的写事件
        // 最后一个参数可以是任意的附加数据（void * 类型）,在这里给事件附上了当前的文件描述符,后面会用到
        EV_SET(&ke, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        kevent(state->kqfd, &ke, 1, NULL, 0, NULL);
    }
    if (mask & AE_WRITABLE) {
        EV_SET(&ke, fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        kevent(state->kqfd, &ke, 1, NULL, 0, NULL);
    }
}

// 在指定的时间段内,阻塞并等待所有被aeCreateFileEvent函数设置为监听状态的套接字产生文件事件,当有至少一个事件产生,或者等待超时后,函数返回
static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int retval, numevents = 0;

    if (tvp != NULL) {
        struct timespec timeout;
        timeout.tv_sec = tvp->tv_sec;
        timeout.tv_nsec = tvp->tv_usec * 1000;
        retval = kevent(state->kqfd, NULL, 0, state->events, eventLoop->setsize, &timeout); // 阻塞事件
    }
    else {
        retval = kevent(state->kqfd, NULL, 0, state->events, eventLoop->setsize, NULL);
    }

    if (retval > 0) {
        int j;

        // 通常我们先执行读事件,然后再执行写事件.等屏障设置好了,我们就反过来做.
        // 然而,在kqueue下,读和写事件将是独立的事件,这将使它不可能控制读和写的顺序.因此,我们存储事件的掩码,并在以后合并相同的fd事件.
        for (j = 0; j < retval; j++) {
            struct kevent *e = state->events + j;
            int fd = e->ident;
            int mask = 0;

            if (e->filter == EVFILT_READ)
                mask = AE_READABLE;
            else if (e->filter == EVFILT_WRITE)
                mask = AE_WRITABLE;
            addEventMask(state->eventsMask, fd, mask); // 实现细节不用管, 存储事件类型
        }

        /* 重新遍历以合并读写事件,并将fd的掩码设置为0,以便在再次遇到fd时不再添加事件.*/
        numevents = 0;
        for (j = 0; j < retval; j++) {
            struct kevent *e = state->events + j;
            int fd = e->ident;
            int mask = getEventMask(state->eventsMask, fd);

            if (mask) {
                eventLoop->fired[numevents].fd = fd;
                eventLoop->fired[numevents].mask = mask;
                resetEventMask(state->eventsMask, fd);
                numevents++;
            }
        }
    }
    else if (retval == -1 && errno != EINTR) {
        panic("aeApiPoll: kevent, %s", strerror(errno));
    }

    return numevents;
}

static char *aeApiName(void) {
    return "kqueue";
}
