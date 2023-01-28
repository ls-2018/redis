/*
 * Redis 的后台 I/O 服务
 * bio 实现了将工作放在后台执行的功能.
 *
 * 目前在后台执行的只有 close(2) 操作：
 * 因为当服务器是某个文件的最后一个拥有者时,
 * 关闭一个文件代表 unlinking 它,
 * 并且删除文件非常慢,会阻塞系统,
 * 所以我们将 close(2) 放到后台进行.
 *
 * (译注：现在不止 close(2) ,连 AOF 文件的 fsync 也是放到后台执行的）
 *
 * 这个后台服务将来可能会增加更多功能,或者切换到 libeio 上面去.
 * 不过我们可能会长期使用这个文件,以便支持一些 Redis 所特有的后台操作.
 * 比如说,将来我们可能需要一个非阻塞的 FLUSHDB 或者 FLUSHALL 也说不定.
 *
 * 设计很简单：
 * 用一个结构表示要执行的工作,而每个类型的工作有一个队列和线程,
 * 每个线程都顺序地执行队列中的工作.
 *
 * 同一类型的工作按 FIFO 的顺序执行.
 *
 * 目前还没有办法在任务完成时通知执行者,在有需要的时候,会实现这个功能.
 */

#include "server.h"
#include "bio.h"
//保存线程描述符的数组
static pthread_t bio_threads[BIO_NUM_OPS];
//保存互斥锁的数组
static pthread_mutex_t bio_mutex[BIO_NUM_OPS];
//保存条件变量的两个数组
static pthread_cond_t bio_newjob_cond[BIO_NUM_OPS];
static pthread_cond_t bio_step_cond[BIO_NUM_OPS];
// 存放每种类型工作的队列
static list *bio_jobs[BIO_NUM_OPS];
// 记录每种类型 job 队列里有多少 job 等待执行
static unsigned long long bio_pending[BIO_NUM_OPS];

// 表示后台任务的数据结构
struct bio_job {
    int           fd;          /* Fd for file based background jobs */
    lazy_free_fn *free_fn;     // 每种后台线程对应的处理函数
    void         *free_args[]; // 任务的参数
};

void *bioProcessBackgroundJobs(void *arg);

/* Make sure we have enough stack to perform all the things we do in the
 * main thread. */
// * 子线程栈大小
#define REDIS_THREAD_STACK_SIZE (1024 * 1024 * 4)

// 初始化后台任务系统,生成线程
void bioInit(void) {
    pthread_t      thread;
    int            j;
    size_t         stacksize = 0; //堆栈大小变量
    pthread_attr_t attr;          //线程属性结构体变量
    // 初始化互斥锁数组和条件变量数组
    for (j = 0; j < BIO_NUM_OPS; j++) {
        pthread_mutex_init(&bio_mutex[j], NULL);
        pthread_cond_init(&bio_newjob_cond[j], NULL);
        pthread_cond_init(&bio_step_cond[j], NULL);
        bio_jobs[j] = listCreate(); // 给 bio_jobs 这个数组的每个元素创建一个链表,每个链表保存了每个后台线程要处理的任务列表
        bio_pending[j] = 0;         // 每种任务中,处于等待状态的任务个数.
    }

    pthread_attr_init(&attr);                     // 线程设置属性,包括堆栈地址和大小、优先级等
    pthread_attr_getstacksize(&attr, &stacksize); // 获取当前的线程栈大小, 512k
    //打印堆栈值
    serverLog(LL_NOTICE, "\nstack_size = %zuB, %luk\n", stacksize, stacksize / 1024);
    if (!stacksize) {
        stacksize = 1; // 针对Solaris系统做处理
    }
    while (stacksize < REDIS_THREAD_STACK_SIZE) { // 4mb
        stacksize *= 2;
    }
    pthread_attr_setstacksize(&attr, stacksize); // 设置线程栈

    /* 准备派生我们的线程.我们使用线程函数接受的单个参数来传递线程负责的作业ID.*/
    //      * 创建线程
    for (j = 0; j < BIO_NUM_OPS; j++) {
        void *arg = (void *)(unsigned long)j;
        // 把创建的 proc 绑在编号为arg的逻辑核上

        // *tidp,指向线程数据结构 pthread_t 的指针;
        // *attr,指向线程属性结构 pthread_attr_t 的指针;
        // *start_routine,线程所要运行的函数的起始地址,也是指向函数的指针;
        // *arg,传给运行函数的参数.

        if (pthread_create(&thread, &attr, bioProcessBackgroundJobs, arg) != 0) {
            serverLog(LL_WARNING, "Fatal: 不能初始化后台线程");
            exit(1);
        }
        bio_threads[j] = thread;
    }
}

// 把任务挂到对应类型的「链表」下
void bioSubmitJob(int type, struct bio_job *job) {
    //    「多线程」读写链表数据,这个过程是需要「加锁」操作的.
    pthread_mutex_lock(&bio_mutex[type]);        // 加锁
    listAddNodeTail(bio_jobs[type], job);        // 添加到链表尾
    bio_pending[type]++;                         // +1
    pthread_cond_signal(&bio_newjob_cond[type]); //
    pthread_mutex_unlock(&bio_mutex[type]);      // 解锁
}

// * 创建后台任务
void bioCreateLazyFreeJob(lazy_free_fn free_fn, int arg_count, ...) {
    va_list valist;
    /* Allocate memory for the job structure and all required
     * arguments */
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
    unsigned long   type = (unsigned long)arg;
    sigset_t        sigset;

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

    pthread_mutex_lock(&bio_mutex[type]);
    /* Block SIGALRM so we are sure that only the main thread will
     * receive the watchdog signal. */
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGALRM);
    if (pthread_sigmask(SIG_BLOCK, &sigset, NULL))
        serverLog(LL_WARNING, "Warning: can't mask SIGALRM in bio.c thread: %s", strerror(errno));

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
        /* It is now possible to unlock the background system as we know have
         * a stand alone job structure to process.*/
        pthread_mutex_unlock(&bio_mutex[type]);

        /* Process the job accordingly to its type. */
        // 执行任务
        if (type == BIO_CLOSE_FILE) {
            close(job->fd);
        }
        else if (type == BIO_AOF_FSYNC) {
            /* The fd may be closed by main thread and reused for another
             * socket, pipe, or file. We just ignore these errno because
             * aof fsync did not really fail. */
            //如果是AOF同步写任务,那就调用redis_fsync函数
            if (redis_fsync(job->fd) == -1 && errno != EBADF && errno != EINVAL) {
                int last_status;
                atomicGet(server.aof_bio_fsync_status, last_status);
                atomicSet(server.aof_bio_fsync_status, C_ERR);
                atomicSet(server.aof_bio_fsync_errno, errno);
                if (last_status == C_OK) {
                    serverLog(LL_WARNING, "Fail to fsync the AOF file: %s", strerror(errno));
                }
            }
            else {
                atomicSet(server.aof_bio_fsync_status, C_OK);
            }
        }
        else if (type == BIO_LAZY_FREE) {
            // lazyfreeFreeObjectFromBioThread
            // lazyfreeFreeDatabaseFromBioThread
            // lazyfreeFreeSlotsMapFromBioThread
            //如果是惰性删除任务,调用不同的惰性删除函数执行
            job->free_fn(job->free_args);
        }
        else {
            serverPanic("Wrong job type in bioProcessBackgroundJobs().");
        }
        zfree(job);

        /* Lock again before reiterating the loop, if there are no longer
         * jobs to process we'll block again in pthread_cond_wait(). */
        pthread_mutex_lock(&bio_mutex[type]);
        //任务执行完成后,调用listDelNode在任务队列中删除该任务
        listDelNode(bio_jobs[type], ln);
        //将对应的等待任务个数减一.
        bio_pending[type]--;

        /* Unblock threads blocked on bioWaitStepOfType() if any. */
        pthread_cond_broadcast(&bio_step_cond[type]);
    }
}

/* Return the number of pending jobs of the specified type. */
//* 返回等待中的 type 类型的工作的数量
unsigned long long bioPendingJobsOfType(int type) {
    unsigned long long val;
    pthread_mutex_lock(&bio_mutex[type]);
    val = bio_pending[type];
    pthread_mutex_unlock(&bio_mutex[type]);
    return val;
}

/* If there are pending jobs for the specified type, the function blocks
 * and waits that the next job was processed. Otherwise the function
 * does not block and returns ASAP.
 *
 * The function returns the number of jobs still to process of the
 * requested type.
 *
 * This function is useful when from another thread, we want to wait
 * a bio.c thread to do more work in a blocking way.
 */
unsigned long long bioWaitStepOfType(int type) {
    unsigned long long val;
    pthread_mutex_lock(&bio_mutex[type]);
    val = bio_pending[type];
    if (val != 0) {
        pthread_cond_wait(&bio_step_cond[type], &bio_mutex[type]);
        val = bio_pending[type];
    }
    pthread_mutex_unlock(&bio_mutex[type]);
    return val;
}

/* Kill the running bio threads in an unclean way. This function should be
 * used only when it's critical to stop the threads for some reason.
 * Currently Redis does this only on crash (for instance on SIGSEGV) in order
 * to perform a fast memory check without other threads messing with memory. */
//* 不进行清理,直接杀死进程,只在出现严重错误时使用
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
