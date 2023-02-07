// 后台线程

#ifndef __BIO_H
#define __BIO_H

typedef void lazy_free_fn(void *args[]);

// Redis Server 在启动时,会在 over-server.c 中调用 bioInit 函数,这个函数会创建4类后台任务
void bioInit(void);

unsigned long long bioPendingJobsOfType(int type);

unsigned long long bioWaitStepOfType(int type);

void bioKillThreads(void);

void bioCreateCloseJob(int fd);

void bioCreateFsyncJob(int fd);

void bioCreateLazyFreeJob(lazy_free_fn free_fn, int arg_count, ...);

#define BIO_CLOSE_FILE 0 // 后台线程关闭 fd  close(fd)
#define BIO_AOF_FSYNC 1  // AOF 配置为 everysec,后台线程刷盘 fsync(fd)
#define BIO_LAZY_FREE 2  // 后台线程释放 key 内存 freeObject/freeDatabase/freeSlowsMap
#define BIO_NUM_OPS 3    // 表示的是 Redis 后台任务的类型有三种

#endif
