// 链接创建、读、写 事件分别由不同函数处理;
// 事件驱动框架
// ------------------------------------
// 事件的数据结构、框架主循环函数、事件捕获分发函数、
// 事件和handler注册函数
// ------------------------------------
// ae_epoll  linux
// ae_evport  Solaris
// ae_kqueue  macos、FreeBSD
// ae_select  linux、windows

#ifndef __AE_H__
#define __AE_H__

#include "over-monotonic.h"

// * 事件执行状态
#define AE_OK 0     // 成功
#define AE_ERR (-1) // 出错

// * 文件事件状态
#define AE_NONE 0     // 000未设置                                                      00000
#define AE_READABLE 1 // 001描述符可读信号                                                00001
#define AE_WRITABLE 2 // 010描述符可写信号                                                00010
#define AE_BARRIER 4  // 100屏障事件的主要作用是用来反转事件的处理顺序.                        00100
// 比如在默认情况下,Redis 会先给客户端返回结果,但是如果面临需要把数据尽快写入磁盘的情况,
// Redis 就会用到屏障事件,把写数据和回复客户端的顺序做下调整,先把数据落盘,再给客户端回复.

// 时间处理器的执行 flags
#define AE_FILE_EVENTS (1 << 0)                         // 文件事件                                         00001
#define AE_TIME_EVENTS (1 << 1)                         // 时间事件                                         00010
#define AE_ALL_EVENTS (AE_FILE_EVENTS | AE_TIME_EVENTS) // 所有事件                                         00011
#define AE_DONT_WAIT (1 << 2)                           // 不阻塞,也不进行等待                                00100
#define AE_CALL_BEFORE_SLEEP (1 << 3)                   //                                                 01000
#define AE_CALL_AFTER_SLEEP (1 << 4)                    //                                                 10000
#define AE_NOMORE (-1)                                  // 决定时间事件是否要持续执行的 flag
#define AE_DELETED_EVENT_ID (-1)                        // 要删除的事件   ID 被置为-1

/* Macros */
#define AE_NOTUSED(V) ((void)(V))

//* 事件处理器状态
struct aeEventLoop;

// * 事件接口
typedef void aeFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
// 返回值,决定是定时事件、还是周期性事件
typedef int aeTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData);

typedef void aeEventFinalizerProc(struct aeEventLoop *eventLoop, void *clientData);

typedef void aeBeforeSleepProc(struct aeEventLoop *eventLoop);

// 文件事件结构,
// 定义了IO事件      对应了客户端发送的网络请求
// 定义了时间事件     Redis自身的周期性操作
typedef struct aeFileEvent {
    //    用来表示事件类型的掩码.对于网络通信的事件来说,主要有 AE_READABLE、AE_WRITABLE 和 AE_BARRIER 三种类型事件.
    //    框架在分发事件时,依赖的就是结构体中的事件类型;
    int mask;
    aeFileProc *rfileProc; // AE_READABLE事件处理器
    aeFileProc *wfileProc; // AE_WRITABLE事件处理器
    void *clientData;      // 用来指向客户端私有数据的指针
} aeFileEvent;

// 时间事件结构
typedef struct aeTimeEvent {
    long long id;  // 事件全局唯一ID,也可以是某几个特殊值
    monotime when; // 毫秒精度的UNIX时间戳,记录了时间事件的到达时间  微秒

    // evictionTimeProc
    // moduleTimerHandler
    // showThroughput
    // serverCron
    // 返回 AE_NOMORE 是定时事件,该事件在到达一次之后就会被删除,之后不再到达
    // 返回不是AE_NOMORE 是周期性事件,当一个事件到达后,服务器会根据事件处理器返回的值,对when属性进行更新,让这个事件在一段时间后再次到达.
    aeTimeProc *timeProc;                // 时间事件触发后的处理函数
    aeEventFinalizerProc *finalizerProc; // 事件结束后的处理函数
    void *clientData;                    // 多路复用库的私有数据
    struct aeTimeEvent *prev;            // 指向上个时间事件结构,形成链表
    struct aeTimeEvent *next;            // 指向下个时间事件结构,形成链表
    int refcount;                        // refcount,以防止定时器事件在递归的时间事件调用中被释放.
} aeTimeEvent;

//  * 已就绪事件
typedef struct aeFiredEvent {
    int fd;   // 已就绪文件描述符
    int mask; // 事件类型掩码, 值可以是 AE_READABLE 1 或 AE_WRITABLE 2 或者是两者的或
} aeFiredEvent;

//* 事件处理器 封装了select、queue、epoll
typedef struct aeEventLoop {
    int maxfd;                      // 目前已注册的最大描述符
    int setsize;                    // 目前已追踪的最大描述符
    long long timeEventNextId;      // 用于生成时间事件 id ,自增
    aeFileEvent *events;            // IO事件数组,每个文件描述组都按照对应索引存储在里边
    aeFiredEvent *fired;            // 已触发事件数组 , 存储了每次 epoll wait 返回的事件      比上边数据量小
    aeTimeEvent *timeEventHead;     // 记录时间事件的链表头
    int stop;                       // 事件处理器的开关 0 没有停止
    void *apidata;                  // 底层监听实现的抽象 [epoll、select、]
    aeBeforeSleepProc *beforesleep; // 在调用事件循环 获取事件 前要执行的函数
    aeBeforeSleepProc *aftersleep;  // 在调用事件循环 获取事件 后要执行的函数
    int flags;
} aeEventLoop;

// 原型
aeEventLoop *aeCreateEventLoop(int setsize);

void aeDeleteEventLoop(aeEventLoop *eventLoop);

void aeStop(aeEventLoop *eventLoop);

int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask, aeFileProc *proc, void *clientData);

void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask);

int aeGetFileEvents(aeEventLoop *eventLoop, int fd);

void *aeGetFileClientData(aeEventLoop *eventLoop, int fd);

long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds, aeTimeProc *proc, void *clientData, aeEventFinalizerProc *finalizerProc);

int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id);

int aeProcessEvents(aeEventLoop *eventLoop, int flags);

int aeWait(int fd, int mask, long long milliseconds);

void aeMain(aeEventLoop *eventLoop);

char *aeGetApiName(void);

void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep);

void aeSetAfterSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *aftersleep);

int aeGetSetSize(aeEventLoop *eventLoop);

int aeResizeSetSize(aeEventLoop *eventLoop, int setsize);

void aeSetDontWait(aeEventLoop *eventLoop, int noWait);

#endif
