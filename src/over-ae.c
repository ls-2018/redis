// over
#include "over-ae.h"
#include "anet.h"
#include "redisassert.h"

#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "over-zmalloc.h"
#include "config.h"

// 包括本系统支持的最佳多路复用层。以下按性能顺序，降序排列。
#ifdef HAVE_EVPORT
#    include "over-ae_evport.c"
#else
#    ifdef HAVE_EPOLL
#        include "over-ae_epoll.c"
#    else
#        ifdef HAVE_KQUEUE
#            include "over-ae_kqueue.c"
#        else
#            include "over-ae_select.c"
#        endif
#    endif
#endif

// 初始化事件处理器状态
aeEventLoop *aeCreateEventLoop(int setsize) {
    aeEventLoop *eventLoop;
    int i;

    monotonicInit(); // 单调时钟初始化

    // 给eventLoop变量分配内存空间
    if ((eventLoop = zmalloc(sizeof(*eventLoop))) == NULL) {
        goto err;
    }
    // 给IO事件、已触发事件分配内存空间
    eventLoop->events = zmalloc(sizeof(aeFileEvent) * setsize); // 10128
    eventLoop->fired = zmalloc(sizeof(aeFiredEvent) * setsize);
    if (eventLoop->events == NULL || eventLoop->fired == NULL) {
        // 判断有没有初始好内存空间
        goto err;
    }
    eventLoop->setsize = setsize;    // 设置ae数组大小
    eventLoop->timeEventHead = NULL; // 设置时间事件的链表头为NULL 初始化
    eventLoop->timeEventNextId = 0;  // 初始化
    eventLoop->stop = 0;             // 初始化
    eventLoop->maxfd = -1;           // 初始化
    eventLoop->beforesleep = NULL;   // 初始化
    eventLoop->aftersleep = NULL;    // 初始化
    eventLoop->flags = 0;            // 初始化

    // 调用aeApiCreate函数,去实际调用操作系统提供的IO多路复用函数
    if (aeApiCreate(eventLoop) == -1) { // IO多路复用初始化
        goto err;
    }
    // 将所有网络IO事件对应文件描述符的掩码设置为AE_NONE
    for (i = 0; i < setsize; i++) {
        eventLoop->events[i].mask = AE_NONE;
    }
    // 返回事件循环
    return eventLoop;
    // 初始化失败后的处理逻辑,
err:
    if (eventLoop) { // 主动释放内存
        zfree(eventLoop->events);
        zfree(eventLoop->fired);
        zfree(eventLoop);
    }
    return NULL;
}

// 返回当前ae事件槽大小
int aeGetSetSize(aeEventLoop *eventLoop) {
    return eventLoop->setsize;
}

// 告诉事件处理的下一次迭代将超时设置为0。
// 没什么用，只调用了一次，就是去掉了这个flag  AE_DONT_WAIT
void aeSetDontWait(aeEventLoop *eventLoop, int noWait) {
    if (noWait)
        eventLoop->flags |= AE_DONT_WAIT;
    else
        eventLoop->flags &= ~AE_DONT_WAIT; // 去掉AE_DONT_WAIT
}

// 调整事件槽的大小
//
// 如果尝试调整大小为 setsize ,但是有 >= setsize 的文件描述符存在
// 那么返回 AE_ERR ,不进行任何动作.
//
// 否则,执行大小调整操作,并返回 AE_OK .
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize) {
    int i;

    if (setsize == eventLoop->setsize)
        return AE_OK;
    if (eventLoop->maxfd >= setsize)
        return AE_ERR;
    if (aeApiResize(eventLoop, setsize) == -1)
        return AE_ERR;

    eventLoop->events = zrealloc(eventLoop->events, sizeof(aeFileEvent) * setsize);
    eventLoop->fired = zrealloc(eventLoop->fired, sizeof(aeFiredEvent) * setsize);
    eventLoop->setsize = setsize;

    /* Make sure that if we created new slots, they are initialized with
     * an AE_NONE mask. */
    for (i = eventLoop->maxfd + 1; i < setsize; i++) eventLoop->events[i].mask = AE_NONE;
    return AE_OK;
}

// 删除事件处理器
void aeDeleteEventLoop(aeEventLoop *eventLoop) {
    aeApiFree(eventLoop);
    zfree(eventLoop->events);
    zfree(eventLoop->fired);

    // 释放时间事件列表
    aeTimeEvent *next_te, *te = eventLoop->timeEventHead;
    while (te) {
        next_te = te->next;
        zfree(te);
        te = next_te;
    }
    zfree(eventLoop);
}

// 停止事件处理器
void aeStop(aeEventLoop *eventLoop) {
    eventLoop->stop = 1;
}

// 实现事件和处理函数的注册
//  循环流程结构体 *eventLoop
//  IO 事件对应的文件描述符  fd
//  事件类型掩码 mask       AE_NONE|AE_READABLE|AE_WRITABLE|AE_BARRIER
//  事件处理回调函数*proc
//  以及事件私有数据*clientData.
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask, aeFileProc *proc, void *clientData) {
    // fd是文件描述符的编号,一般是从6开始
    // 运行之出,首先是ipv4 ,ipv6对6379端口的套接字
    if (fd >= eventLoop->setsize) { // 10128
        errno = ERANGE;
        return AE_ERR;
    }
    // 取出文件事件结构
    aeFileEvent *fe = &eventLoop->events[fd];
    // 通过调用 epoll_ctl,来注册希望监听的事件和相应的处理函数.
    if (aeApiAddEvent(eventLoop, fd, mask) == -1) {
        return AE_ERR;
    }
    // 设置文件事件类型,以及事件的处理器
    fe->mask |= mask;
    if (mask & AE_READABLE) {
        fe->rfileProc = proc;
    }
    if (mask & AE_WRITABLE) {
        fe->wfileProc = proc;
    }

    // 私有数据
    fe->clientData = clientData;
    // 如果有需要,更新事件处理器的最大 fd
    if (fd > eventLoop->maxfd) {
        //      文件描述符从3开始,从小到大编号,因为0、1、2被分配给了标准IO描述符
        eventLoop->maxfd = fd;
    }
    return AE_OK;
}

// 将 fd 从 mask 指定的监听队列中删除
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask) {
    if (fd >= eventLoop->setsize)
        return;
    // 取出文件事件结构
    aeFileEvent *fe = &eventLoop->events[fd];
    // 未设置监听的事件类型,直接返回
    if (fe->mask == AE_NONE)
        return;

    if (mask & AE_WRITABLE) { // 设置了写,就要设置翻转,即先写了在读
        mask |= AE_BARRIER;
    }
    // 取消对给定 fd 的给定事件的监视
    aeApiDelEvent(eventLoop, fd, mask);
    // 计算新掩码
    fe->mask = fe->mask & (~mask);
    if (fd == eventLoop->maxfd && fe->mask == AE_NONE) {
        int j;
        for (j = eventLoop->maxfd - 1; j >= 0; j--) {
            if (eventLoop->events[j].mask != AE_NONE)
                break;
        }
        eventLoop->maxfd = j;
    }
}

void *aeGetFileClientData(aeEventLoop *eventLoop, int fd) {
    if (fd >= eventLoop->setsize)
        return NULL;
    aeFileEvent *fe = &eventLoop->events[fd];
    if (fe->mask == AE_NONE)
        return NULL;

    return fe->clientData;
}
// 获取给定 fd 正在监听的事件类型
int aeGetFileEvents(aeEventLoop *eventLoop, int fd) {
    if (fd >= eventLoop->setsize)
        return 0;
    aeFileEvent *fe = &eventLoop->events[fd];

    return fe->mask;
}

// 将一个新的时间事件添加到服务器,这个时间事件将在当前时间的milliseconds毫秒后到达
// proc 所创建时间事件触发后的回调函数.
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds, aeTimeProc *proc, void *clientData, aeEventFinalizerProc *finalizerProc) {
    long long id = eventLoop->timeEventNextId++; // 更新时间计数器
    aeTimeEvent *te;                             // 创建时间事件结构

    te = zmalloc(sizeof(*te));
    if (te == NULL) {
        return AE_ERR;
    }
    te->id = id;                                       // 设置 ID
    te->when = getMonotonicUs() + milliseconds * 1000; // 设定处理事件触发的绝对时间  秒
    te->timeProc = proc;                               // 设置时间事件到达时应该触发的函数
    te->finalizerProc = finalizerProc;                 // 设置事件结束时应该触发的函数
    te->clientData = clientData;                       // 设置私有数据
    te->prev = NULL;
    te->next = eventLoop->timeEventHead; // 将新事件放入表头
    te->refcount = 0;
    if (te->next) {
        te->next->prev = te;
    }
    eventLoop->timeEventHead = te;
    return id;
}

// 从服务器中删除该ID所对应的时间事件                       事件ID
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id) {
    aeTimeEvent *te = eventLoop->timeEventHead; // 遍历链表
    while (te) {
        if (te->id == id) { // 发现目标事件,删除
            te->id = AE_DELETED_EVENT_ID;
            return AE_OK;
        }
        te = te->next;
    }
    return AE_ERR; /* NO event with the specified ID found */
}

// 获取 最早要发生的事件 与 当前时间 差值, 返回等待时间差值
static int64_t usUntilEarliestTimer(aeEventLoop *eventLoop) {
    // 程序运行之初,创建了一个timer 回调函数是serverCron
    aeTimeEvent *te = eventLoop->timeEventHead;
    if (te == NULL) {
        return -1;
    }

    aeTimeEvent *earliest = NULL;
    // 寻找链表里when最小的事件
    while (te) {
        if (!earliest || te->when < earliest->when) {
            earliest = te;
        }
        te = te->next;
    }

    monotime now = getMonotonicUs();
    if (now >= earliest->when) {
        return 0;
    }
    else {
        return earliest->when - now;
    }
}

// 处理所有已到达的时间事件
static int processTimeEvents(aeEventLoop *eventLoop) {
    int processed = 0;
    aeTimeEvent *te;
    long long maxId;

    te = eventLoop->timeEventHead;          // 从时间事件链表上逐一取出每一个事件
    maxId = eventLoop->timeEventNextId - 1; //
    monotime now = getMonotonicUs();        // 获取当前时间

    while (te) {
        long long id;

        // 事件需要被删除
        if (te->id == AE_DELETED_EVENT_ID) {
            aeTimeEvent *next = te->next;
            // 如果存在此计时器事件的引用,则不释放它.对于递归的timerProc调用,这个值目前是递增的
            if (te->refcount) {
                te = next;
                continue;
            }
            // 删除
            if (te->prev) { // 有前置节点
                te->prev->next = te->next;
            }
            else { // 无前置节点
                eventLoop->timeEventHead = te->next;
            }
            if (te->next) {
                te->next->prev = te->prev;
            }
            // 执行清理处理器
            if (te->finalizerProc) {
                te->finalizerProc(eventLoop, te->clientData);
            }
            zfree(te); // 归还空间,更新计数
            te = next;
            continue;
        }

        // 确保我们不会在此迭代中处理由时间事件创建的时间事件.
        // 注意,这个检查目前是无用的:我们总是在头部添加新的计时器,但是如果我们改变实现细节,这个检查可能会再次有用:我们把它保存在这里,以备将来的防御.
        // 跳过无效事件
        if (te->id > maxId) {
            te = te->next;
            continue;
        }
        // 已经到了定时的时间
        if (te->when <= now) {
            //          然后根据当前时间判断该事件的触发时间戳是否已满足
            int retval; // 执行结果

            id = te->id; // 事件ID
            te->refcount++;
            // retVal 为时间事件的执行间隔
            retval = te->timeProc(eventLoop, id, te->clientData); // 调用注册的回调函数处理     serverCron、evictionTimeProc、moduleTimerHandler   返回执行状态
            te->refcount--;
            processed++;
            now = getMonotonicUs();    // 获取当前时间
            if (retval == AE_NOMORE) { // 判断时否是驱逐状态码                evictionTimeProc、moduleTimerHandler
                // 如果超时了,那么标记为删除
                te->id = AE_DELETED_EVENT_ID;
            }
            else {
                te->when = now + retval * 1000; // 下一次要执行的时间         serverCron
            }
        }
        te = te->next; // 获取下一个时间事件
    }
    return processed;
}

// 事件的捕获【aeApiPoll】、分发、处理
int aeProcessEvents(aeEventLoop *eventLoop, int flags) {
    // over-server.c        flasgs  AE_ALL_EVENTS[AE_FILE_EVENTS | AE_TIME_EVENTS] | AE_CALL_BEFORE_SLEEP | AE_CALL_AFTER_SLEEP
    // networking.c    flasgs  AE_FILE_EVENTS | AE_DONT_WAIT | AE_CALL_BEFORE_SLEEP | AE_CALL_AFTER_SLEEP
    int processed = 0, numevents;

    // 既没有时间事件,也没有网络事件;
    if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS)) {
        return 0;
    }

    // 有 IO 事件或者有需要紧急处理的时间事件;,则开始处理
    // eventLoop->maxfd 应该是6  [0,1,2  3,4 5,6:6379端口描述符 ]
    if (eventLoop->maxfd != -1 || ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT))) {
        // aemain 函数进来的
        int j;
        struct timeval tv;
        struct timeval *tvp;
        int64_t usUntilTimer = -1; // epoll wait等待的时间
        // 那么根据最近可执行时间事件和现在时间的时间差来决定文件事件的阻塞时间
        if (flags & AE_TIME_EVENTS && !(flags & AE_DONT_WAIT)) {
            usUntilTimer = usUntilEarliestTimer(eventLoop); // 获取 最早要发生的事件 与 当前时间 差值, 返回等待时间差值   ;  也就是 epoll wait等待的时间
        }

        if (usUntilTimer >= 0) {
            tv.tv_sec = usUntilTimer / 1000000;  // 秒
            tv.tv_usec = usUntilTimer % 1000000; // 毫秒数
            tvp = &tv;
        }
        else {
            // 时间差小于 0 ,说明事件已经可以执行了,将秒和毫秒设为 0 （不阻塞）
            // 执行到这一步,说明没有时间事件
            // 那么根据 AE_DONT_WAIT 是否设置来决定是否阻塞,以及阻塞的时间长度

            /* 如果我们必须检查事件,但是因为AE_DONT_WAIT而需要尽快返回,我们需要将超时设置为零 */

            if (flags & AE_DONT_WAIT) {
                // 设置文件事件不阻塞
                tv.tv_sec = tv.tv_usec = 0;
                tvp = &tv;
            }
            else {
                tvp = NULL; // 永远等待
            }
        }
        // 事件循环如果不支持阻塞等待，就将tv都设置为0
        if (eventLoop->flags & AE_DONT_WAIT) {
            tv.tv_sec = tv.tv_usec = 0;
            tvp = &tv;
        }
        // 如果beforeSleep函数不为空,则调用beforeSleep函数
        if (eventLoop->beforesleep != NULL && flags & AE_CALL_BEFORE_SLEEP) {
            eventLoop->beforesleep(eventLoop);
        }

        // 调用aeApiPoll获取就绪的描述符,tvp是阻塞等待多长时间
        numevents = aeApiPoll(eventLoop, tvp); // int类型,会将对应的描述符放到 eventLoop->fired

        // epoll 等待之后的回调函数
        if (eventLoop->aftersleep != NULL && flags & AE_CALL_AFTER_SLEEP) {
            eventLoop->aftersleep(eventLoop);
        }

        for (j = 0; j < numevents; j++) {
            int fd = eventLoop->fired[j].fd;
            aeFileEvent *fe = &eventLoop->events[fd]; // 从已就绪数组中获取事件
            int mask = eventLoop->fired[j].mask;
            int fired = 0; // 当前事件处理的次数

            // 通常我们先执行可读事件,然后再执行可写事件.这很有用,因为有时我们可能能够在处理查询之后立即提供查询的回复.
            // 然而,如果AE_BARRIER是在掩码中设置的,我们的应用程序会要求我们做相反的事情
            // 例如,当我们想在回复客户端之前在 beforeSleep() 钩子中做一些事情时,这很有用,比如将文件fsyncing到磁盘.
            int invert = fe->mask & AE_BARRIER;

            // fe->mask 已就绪的fd,对应的之前存储的mask
            // mask     已就绪的fd,对应的mask
            //
            // 对于同一个 fe  我们先处理 read,在处理write
            // 如果翻转了  先处理write 在处理read
            // 如果触发的是可读事件,调用事件注册时设置的读事件回调处理函数
            if (!invert && fe->mask & mask & AE_READABLE) { // & & 检查事件是否仍然有效
                fe->rfileProc(eventLoop, fd, fe->clientData,
                              mask); // 可以读的,回调函数    【创建链接，读取数据】   client的可读回调是connSocketEventHandler
                fired++;
                // 根本原因是事件循环可能从事件回调本身，导致事件指针无效
                fe = &eventLoop->events[fd]; // 在调整大小的情况下刷新.    513931df
            }

            // 如果触发的是可写事件,调用事件注册时设置的写事件回调处理函数
            // 事件类型,ae触发的类型,指定的类型
            if (fe->mask & mask & AE_WRITABLE) {
                // 如果没有处理过可读事件 或 可读、可写的处理函数不一样
                if (!fired || fe->wfileProc != fe->rfileProc) {         // 同时处罚可读写,只要处理函数不一样,就处理
                    fe->wfileProc(eventLoop, fd, fe->clientData, mask); // 可以写的,回调函数
                    fired++;
                }
            }

            // 如果必须反转调用,则在可写事件之后触发可读事件.
            if (invert) {
                fe = &eventLoop->events[fd]; // 在调整大小的情况下刷新。
                if ((fe->mask & mask & AE_READABLE) && (!fired || fe->wfileProc != fe->rfileProc)) {
                    fe->rfileProc(eventLoop, fd, fe->clientData, mask);
                    fired++;
                }
            }

            processed++;
        }
    }
    // 检测时间事件是否触发
    if (flags & AE_TIME_EVENTS) {
        processed += processTimeEvents(eventLoop);
    }

    return processed; // 返回当前调用 已经处理的文件或时间事件个数
}

// 在给定的时间内阻塞并等待套接字的给定类型事件产生,当事件成功产生,或者等待超时后,函数返回
int aeWait(int fd, int mask, long long milliseconds) {
    struct pollfd pfd;
    // struct pollfd{
    //   int fd; /*文件描述符，如建立socket后获取的fd, 此处表示想查询的文件描述符*/
    //   short events;	/*等待的事件，就是要监测的感兴趣的事情*/
    //   short revents;	/*实际发生了的事情*/
    // };

    int retmask = 0;
    int retval = 0;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    // POLLIN/POLLRDNORM(可读)，POLLOUT/PILLWRNORM(可写)，POLLEER(出错)。
    if (mask & AE_READABLE)
        pfd.events |= POLLIN;
    if (mask & AE_WRITABLE)
        pfd.events |= POLLOUT;
    // poll()函数的作用是把当前的文件指针挂到等待队列中。
    //
    // pollfd *fds : 指向pollfd结构体数组，用于存放需要检测器状态的Socket 描述符或其它文件描述符。
    // unsigned int nfds: 指定pollfd 结构体数组的个数，即监控几个pollfd.
    // timeout:指poll() 函数调用阻塞的时间，单位是ms.如果timeout=0则不阻塞，如timeout=INFTIM 表 示一直阻塞直到感兴趣的事情发生。
    // 返回值：>0 表示数组fds 中准备好读，写或出错状态的那些socket描述符的总数量
    // ==0 表示数组fds 中都没有准备好读写或出错，当poll 阻塞超时timeout 就会返回。
    // -1 表示poll() 函数调用失败，同时回自动设置全局变量errno.
    if ((retval = poll(&pfd, 1, milliseconds)) == 1) {
        if (pfd.revents & POLLIN)
            retmask |= AE_READABLE;
        if (pfd.revents & POLLOUT)
            retmask |= AE_WRITABLE;
        if (pfd.revents & POLLERR)
            retmask |= AE_WRITABLE;
        if (pfd.revents & POLLHUP) // shutdown 或 close
            retmask |= AE_WRITABLE;
        return retmask;
    }
    else {
        return retval;
    }
}

// --事件处理器的主循环
void aeMain(aeEventLoop *eventLoop) {
    eventLoop->stop = 0;
    while (!eventLoop->stop) { // 没有停止,继续往下走
        // 根据事件类型进行相应的处理                           所有事件、事件循环之前、之后都需要执行
        aeProcessEvents(eventLoop, AE_ALL_EVENTS | AE_CALL_BEFORE_SLEEP | AE_CALL_AFTER_SLEEP);
    }
}

// 返回底层所使用的多路复用库的名称
char *aeGetApiName(void) {
    return aeApiName();
}

// 设置处理事件前需要被执行的函数
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep) {
    eventLoop->beforesleep = beforesleep;
}

// 设置处理事件后需要被执行的函数
void aeSetAfterSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *aftersleep) {
    eventLoop->aftersleep = aftersleep;
}
