#include "over-server.h"
#include "over-bio.h"
// 保存线程描述符的数组
static pthread_t bio_threads[BIO_NUM_OPS];
// 保存互斥锁的数组
static pthread_mutex_t bio_mutex[BIO_NUM_OPS];
// 保存条件变量的两个数组
static pthread_cond_t bio_newjob_cond[BIO_NUM_OPS];
// 存放每种类型工作的队列
static list *bio_jobs[BIO_NUM_OPS];
// 记录每种类型 job 队列里有多少 job 等待执行
static unsigned long long bio_pending[BIO_NUM_OPS];

// 表示后台任务的数据结构
struct bio_job {
    int fd;                // 用于基于文件的后台job
    lazy_free_fn *free_fn; // 每种后台线程对应的处理函数
    void *free_args[];     // 任务的参数
};

void *bioProcessBackgroundJobs(void *arg);

// 子线程栈大小
#define REDIS_THREAD_STACK_SIZE (1024 * 1024 * 4)

// 初始化后台任务系统,生成线程
void bioInit(void) {
    pthread_t thread;
    int j;
    size_t stacksize = 0; // 堆栈大小变量
    pthread_attr_t attr;  // 线程属性结构体变量
    // 初始化互斥锁数组和条件变量数组
    for (j = 0; j < BIO_NUM_OPS; j++) {
        pthread_mutex_init(&bio_mutex[j], NULL);
        pthread_cond_init(&bio_newjob_cond[j], NULL); // 用来初始化条件变量的函数
        bio_jobs[j] = listCreate();                   // 给 bio_jobs 这个数组的每个元素创建一个链表,每个链表保存了每个后台线程要处理的任务列表
        bio_pending[j] = 0;                           // 每种任务中,处于等待状态的任务个数.
    }

    pthread_attr_init(&attr);                     // 线程设置属性,包括堆栈地址和大小、优先级等
    pthread_attr_getstacksize(&attr, &stacksize); // 获取当前的线程栈大小, 512k
    // 打印堆栈值
    serverLog(LL_NOTICE, "\n栈大小 = %zuB, %luk\n", stacksize, stacksize / 1024);
    if (!stacksize) {
        stacksize = 1; // 针对Solaris系统做处理
    }
    while (stacksize < REDIS_THREAD_STACK_SIZE) { // 4mb
        stacksize *= 2;
    }
    pthread_attr_setstacksize(&attr, stacksize); // 设置线程栈

    /* 准备派生我们的线程.我们使用线程函数接受的单个参数来传递线程负责的作业ID.*/
    //  * 创建线程
    for (j = 0; j < BIO_NUM_OPS; j++) {
        void *arg = (void *)(unsigned long)j;
        // 把创建的 proc 绑在编号为arg的逻辑核上

        // tidp,指向线程数据结构 pthread_t 的指针;
        // attr,指向线程属性结构 pthread_attr_t 的指针;
        // start_routine,线程所要运行的函数的起始地址,也是指向函数的指针;
        // arg,传给运行函数的参数.

        if (pthread_create(&thread, &attr, bioProcessBackgroundJobs, arg) != 0) {
            serverLog(LL_WARNING, "Fatal: 不能初始化后台线程");
            exit(1);
        }
        bio_threads[j] = thread;
    }
}

// 把任务挂到对应类型的「链表」下
void bioSubmitJob(int type, struct bio_job *job) {
    // 「多线程」读写链表数据,这个过程是需要「加锁」操作的.
    pthread_mutex_lock(&bio_mutex[type]); // 加锁
    listAddNodeTail(bio_jobs[type], job); // 添加到链表尾
    bio_pending[type]++;                  // +1
    // 发送一个信号给另外一个正在处于阻塞等待状态的线程,使其脱离阻塞状态,继续执行.如果没有线程处在阻塞等待状态,pthread_cond_signal也会成功返回
    // 唤醒任务线程
    pthread_cond_signal(&bio_newjob_cond[type]);
    pthread_mutex_unlock(&bio_mutex[type]); // 解锁
}

// 将 value 的释放添加到异步线程队列中去，后台处理, 任务类型为 异步释放内存
void bioCreateLazyFreeJob(lazy_free_fn free_fn, int arg_count, ...) {
    va_list valist;
    /* 为作业结构和所有必需的参数分配内存 */
    struct bio_job *job = zmalloc(sizeof(*job) + sizeof(void *) * (arg_count));
    job->free_fn = free_fn;

    va_start(valist, arg_count);
    for (int i = 0; i < arg_count; i++) {
        job->free_args[i] = va_arg(valist, void *);
    }
    va_end(valist);
    bioSubmitJob(BIO_LAZY_FREE, job);
}

void bioCreateCloseJob(int fd) {
    struct bio_job *job = zmalloc(sizeof(*job));
    job->fd = fd;

    bioSubmitJob(BIO_CLOSE_FILE, job);
}

void bioCreateFsyncJob(int fd) {
    struct bio_job *job = zmalloc(sizeof(*job));
    job->fd = fd;

    bioSubmitJob(BIO_AOF_FSYNC, job);
}

// 处理后台任务
void *bioProcessBackgroundJobs(void *arg) {
    struct bio_job *job;
    unsigned long type = (unsigned long)arg;
    sigset_t sigset;

    // 类型检查
    if (type >= BIO_NUM_OPS) {
        serverLog(LL_WARNING, "Warning: bio线程以错误类型启动了 %lu", type);
        return NULL;
    }

    switch (type) {
        case BIO_CLOSE_FILE:
            redis_set_thread_title("bio_close_file");
            break;
        case BIO_AOF_FSYNC:
            redis_set_thread_title("bio_aof_fsync");
            break;
        case BIO_LAZY_FREE:
            redis_set_thread_title("bio_lazy_free");
            break;
    }

    redisSetCpuAffinity(server.bio_cpulist);

    makeThreadKillable();
    // 异步删除， bio正跑着，unlink无法获得锁提交job
    pthread_mutex_lock(&bio_mutex[type]);
    /* 阻塞SIGALRM，以确保只有主线程将接收看门狗信号。 */
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGALRM);
    if (pthread_sigmask(SIG_BLOCK, &sigset, NULL))
        serverLog(LL_WARNING, "在bio.c线程中不能屏蔽SIGALRM: %s", strerror(errno));

    while (1) {
        listNode *ln;

        // 先获取锁
        if (listLength(bio_jobs[type]) == 0) {
            pthread_cond_wait(&bio_newjob_cond[type], &bio_mutex[type]);
            continue;
        }
        // 从任务队列中获取任务
        ln = listFirst(bio_jobs[type]);
        job = ln->value;
        /* 现在可以解锁后台系统，因为我们知道有一个独立的作业结构来处理。*/
        pthread_mutex_unlock(&bio_mutex[type]);

        // 执行任务,此时已经释放了锁,因此可以添加任务
        if (type == BIO_CLOSE_FILE) {
            close(job->fd);
        }
        else if (type == BIO_AOF_FSYNC) {
            // 如果是AOF同步写任务,那就调用redis_fsync函数
            if (redis_fsync(job->fd) == -1 && errno != EBADF && errno != EINVAL) {
                int last_status;
                atomicGet(server.aof_bio_fsync_status, last_status);
                atomicSet(server.aof_bio_fsync_status, C_ERR);
                atomicSet(server.aof_bio_fsync_errno, errno);
                if (last_status == C_OK) {
                    serverLog(LL_WARNING, "文件刷盘失败: %s", strerror(errno));
                }
            }
            else {
                atomicSet(server.aof_bio_fsync_status, C_OK);
            }
        }
        else if (type == BIO_LAZY_FREE) {
            // lazyfreeFreeObject
            // lazyfreeFreeDatabase
            // lazyFreeTrackingTable
            // lazyFreeLuaScripts
            // lazyFreeFunctionsCtx
            // lazyFreeReplicationBacklogRefMem
            // 如果是惰性删除任务,调用不同的惰性删除函数执行
            job->free_fn(job->free_args);
        }
        else {
            serverPanic("错误的job类型 in bioProcessBackgroundJobs().");
        }
        zfree(job);

        /* Lock again before reiterating the loop, if there are no longer jobs to process we'll block again in pthread_cond_wait(). */
        pthread_mutex_lock(&bio_mutex[type]);
        // 任务执行完成后,调用listDelNode在任务队列中删除该任务
        listDelNode(bio_jobs[type], ln);
        // 将对应的等待任务个数减一.
        bio_pending[type]--;
    }
}

// 返回等待中的 type 类型的工作的数量
unsigned long long bioPendingJobsOfType(int type) {
    unsigned long long val;
    pthread_mutex_lock(&bio_mutex[type]);
    val = bio_pending[type];
    pthread_mutex_unlock(&bio_mutex[type]);
    return val;
}

/* Kill the running bio threads in an unclean way. This function should be
 * used only when it's critical to stop the threads for some reason.
 * Currently Redis does this only on crash (for instance on SIGSEGV) in order
 * to perform a fast memory check without other threads messing with memory. */
// 不进行清理,直接杀死进程,只在出现严重错误时使用
void bioKillThreads(void) {
    int err, j;

    for (j = 0; j < BIO_NUM_OPS; j++) {
        if (bio_threads[j] == pthread_self())
            continue;
        if (bio_threads[j] && pthread_cancel(bio_threads[j]) == 0) {
            if ((err = pthread_join(bio_threads[j], NULL)) != 0) {
                serverLog(LL_WARNING, "Bio thread for job type #%d can not be joined: %s", j, strerror(err));
            }
            else {
                serverLog(LL_WARNING, "Bio thread for job type #%d terminated", j);
            }
        }
    }
}
