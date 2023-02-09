// 1、initServerConfig               为各种参数设置默认值
// 2、main                           逐一解析命令行参数
// 3、loadServerConfig               进行第二、三赋值
// 4、loadServerConfigFromString     对配置项字符串中的每一个配置项进行匹配

#include "over-server.h"
#include "over-monotonic.h"
#include "cluster.h"
#include "slowlog.h"
#include "over-bio.h"
#include "over-latency.h"
#include "over-atomicvar.h"
#include "over-mt19937-64.h"
#include "over-functions.h"

#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <locale.h>
#include <sys/socket.h>
#include <sys/resource.h>

#ifdef __linux__
#    include <sys/mman.h>
#endif

#if defined(HAVE_SYSCTL_KIPC_SOMAXCONN) || defined(HAVE_SYSCTL_KERN_SOMAXCONN)

#    include <sys/sysctl.h>

#endif

// 共享对象
struct sharedObjectsStruct shared;

/* Global vars that are actually used as constants. The following double
 * values are used for double on-disk serialization, and are initialized
 * at runtime to avoid strange compiler optimizations. */

double R_Zero, R_PosInf, R_NegInf, R_Nan;

/*================================= Globals ================================= */

struct redisServer server;

/*============================ 内部的原型 ========================== */

// 取消关闭服务器
static void cancelShutdown(void) {
    server.shutdown_asap = 0;          // 关闭服务器的标识
    server.shutdown_flags = 0;         // 传递给prepareForShutdown()的标志.
    server.shutdown_mstime = 0;        // 优雅关闭限制的时间
    server.last_sig_received = 0;      // 最近一次收到的信号
    replyToClientsBlockedOnShutdown(); // 如果一个或多个客户端在SHUTDOWN命令上被阻塞，该函数将向它们发送错误应答并解除阻塞。
    unpauseClients(PAUSE_DURING_SHUTDOWN);
}

static inline int isShutdownInitiated(void);

static inline int isShutdownInitiated(void) {
    return server.shutdown_mstime != 0;
}
int isReadyToShutdown(void);

// 如果有任何副本在复制中滞后，我们需要在关闭之前等待，则返回0。如果现在准备关闭，则返回1。
int isReadyToShutdown(void) {
    if (listLength(server.slaves) == 0)
        return 1;

    listIter li;
    listNode *ln;
    listRewind(server.slaves, &li);
    while ((ln = listNext(&li)) != NULL) {
        client *replica = listNodeValue(ln);
        if (replica->repl_ack_off != server.master_repl_offset)
            return 0;
    }
    return 1;
}
const char *replstateToString(int replstate);

const char *replstateToString(int replstate) {
    switch (replstate) {
        case SLAVE_STATE_WAIT_BGSAVE_START:
        case SLAVE_STATE_WAIT_BGSAVE_END:
            return "wait_bgsave";
        case SLAVE_STATE_SEND_BULK:
            return "send_bulk";
        case SLAVE_STATE_ONLINE:
            return "online";
        default:
            return "";
    }
}

// 关闭序列的最后一步。
// 如果关闭序列成功并且可以调用exit()，则返回C_OK。如果返回C_ERR，调用exit()是不安全的。
int finishShutdown(void);
int finishShutdown(void) {
    // todo
    int save = server.shutdown_flags & SHUTDOWN_SAVE;
    int nosave = server.shutdown_flags & SHUTDOWN_NOSAVE;
    int force = server.shutdown_flags & SHUTDOWN_FORCE;

    // 为每个滞后的副本记录一个警告。
    listIter replicas_iter;
    listNode *replicas_list_node;
    int num_replicas = 0;
    int num_lagging_replicas = 0; // 滞后的副本数
    listRewind(server.slaves, &replicas_iter);
    while ((replicas_list_node = listNext(&replicas_iter)) != NULL) {
        client *replica = listNodeValue(replicas_list_node);
        num_replicas++;
        if (replica->repl_ack_off != server.master_repl_offset) {
            num_lagging_replicas++;
            long lag = replica->replstate == SLAVE_STATE_ONLINE ? time(NULL) - replica->repl_ack_time : 0;
            serverLog(
                LL_WARNING, "滞后副本:%s,报告偏移量%lld落后于主副本，滞后=%ld，状态=%s。", //
                replicationGetSlaveName(replica),                                          //
                server.master_repl_offset - replica->repl_ack_off,                         //
                lag,                                                                       //
                replstateToString(replica->replstate)                                      //
            );
        }
    }
    if (num_replicas > 0) {
        serverLog(LL_NOTICE, "%d of %d 副本正在同步 在shutdown时", num_replicas - num_lagging_replicas, num_replicas);
    }
    // 杀死所有Lua调试器会话
    ldbKillForkedSessions();

    /* Kill the saving child if there is a background saving in progress.
       We want to avoid race conditions, for instance our saving child may
       overwrite the synchronous saving did by SHUTDOWN. */
    // 如果有 BGSAVE 正在执行,那么杀死子进程,避免竞争条件
    if (server.child_type == CHILD_TYPE_RDB) {
        serverLog(LL_WARNING, "There is a child saving an .rdb. Killing it!");
        killRDBChild();
        /* Note that, in killRDBChild normally has backgroundSaveDoneHandler
         * doing it's cleanup, but in this case this code will not be reached,
         * so we need to call rdbRemoveTempFile which will close fd(in order
         * to unlink file actually) in background thread.
         * The temp rdb file fd may won't be closed when redis exits quickly,
         * but OS will close this fd when process exits. */
        rdbRemoveTempFile(server.child_pid, 0);
    }

    /* Kill module child if there is one. */
    if (server.child_type == CHILD_TYPE_MODULE) {
        serverLog(LL_WARNING, "There is a module fork child. Killing it!");
        TerminateModuleForkChild(server.child_pid, 0);
    }

    /* Kill the AOF saving child as the AOF we already have may be longer
     * but contains the full dataset anyway. */
    if (server.child_type == CHILD_TYPE_AOF) {
        /* If we have AOF enabled but haven't written the AOF yet, don't
         * shutdown or else the dataset will be lost. */
        if (server.aof_state == AOF_WAIT_REWRITE) {
            if (force) {
                serverLog(LL_WARNING, "Writing initial AOF. Exit anyway.");
            }
            else {
                serverLog(LL_WARNING, "Writing initial AOF, can't exit.");
                goto error;
            }
        }
        serverLog(LL_WARNING, "There is a child rewriting the AOF. Killing it!");
        killAppendOnlyChild();
    }
    // 同理,杀死正在执行 BGREWRITEAOF 的子进程

    if (server.aof_state != AOF_OFF) {
        /* Append only file: flush buffers and fsync() the AOF at exit */
        serverLog(LL_NOTICE, "Calling fsync() on the AOF file.");
        flushAppendOnlyFile(1); // 将缓冲区的内容写入到硬盘里面

        if (redis_fsync(server.aof_fd) == -1) {
            serverLog(LL_WARNING, "Fail to fsync the AOF file: %s.", strerror(errno));
        }
    }

    /* Create a new RDB file before exiting. */
    // 如果客户端执行的是 SHUTDOWN save ,或者 SAVE 功能被打开
    // 那么执行 SAVE 操作
    if ((server.saveparamslen > 0 && !nosave) || save) {
        serverLog(LL_NOTICE, "Saving the final RDB snapshot before exiting.");
        if (server.supervised_mode == SUPERVISED_SYSTEMD)
            redisCommunicateSystemd("STATUS=Saving the final RDB snapshot\n");
        /* Snapshotting. Perform a SYNC SAVE and exit */
        rdbSaveInfo rsi, *rsiptr;
        rsiptr = rdbPopulateSaveInfo(&rsi);
        if (rdbSave(SLAVE_REQ_NONE, server.rdb_filename, rsiptr) != C_OK) {
            /* Ooops.. error saving! The best we can do is to continue
             * operating. Note that if there was a background saving process,
             * in the next cron() Redis will be notified that the background
             * saving aborted, handling special stuff like slaves pending for
             * synchronization... */
            if (force) {
                serverLog(LL_WARNING, "Error trying to save the DB. Exit anyway.");
            }
            else {
                serverLog(LL_WARNING, "Error trying to save the DB, can't exit.");
                if (server.supervised_mode == SUPERVISED_SYSTEMD)
                    redisCommunicateSystemd("STATUS=Error trying to save the DB, can't exit.\n");
                goto error;
            }
        }
    }

    /* Free the AOF manifest. */
    if (server.aof_manifest)
        aofManifestFree(server.aof_manifest);

    /* Fire the shutdown modules event. */
    moduleFireServerEvent(REDISMODULE_EVENT_SHUTDOWN, 0, NULL);

    /* Remove the pid file if possible and needed. */
    // 移除 pidfile 文件

    if (server.daemonize || server.pid_file) {
        serverLog(LL_NOTICE, "Removing the pid file.");
        unlink(server.pid_file);
    }

    /* Best effort flush of slave output buffers, so that we hopefully
     * send them pending writes. */
    flushSlavesOutputBuffers();

    /* Close the listening sockets. Apparently this allows faster restarts. */
    // 关闭监听套接字,这样在重启的时候会快一点

    closeListeningSockets(1);
    serverLog(LL_WARNING, "%s is now ready to exit, bye bye...", server.sentinel_mode ? "Sentinel" : "Redis");
    return C_OK;

error:
    serverLog(LL_WARNING, "Errors trying to shut down the server. Check the logs for more information.");
    cancelShutdown();
    return C_ERR;
}

/*============================ 效用函数 ============================ */

// 我们使用一个私有的本地时间实现，它是fork-safe的。Redis的日志功能可以从其他线程调用。
void nolocks_localtime(struct tm *tmp, time_t t, time_t tz, int dst);

// 低水平日志记录.只用于非常大的消息,否则最好使用serverLog().
void serverLogRaw(int level, const char *msg) {
    const int syslogLevelMap[] = {LOG_DEBUG, LOG_INFO, LOG_NOTICE, LOG_WARNING};
    const char *c = ".-*#";
    FILE *fp;
    char buf[64];
    int rawmode = (level & LL_RAW);
    int log_to_stdout = server.logfile[0] == '\0';

    level &= 0xff; /* clear flags */
    if (level < server.verbosity)
        return;

    fp = log_to_stdout ? stdout : fopen(server.logfile, "a");
    if (!fp)
        return;

    if (rawmode) {
        fprintf(fp, "%s", msg);
    }
    else {
        int off;
        struct timeval tv;
        int role_char;
        pid_t pid = getpid();

        gettimeofday(&tv, NULL);
        struct tm tm;
        // 自定义的localtime函数,标准的 localtime 在多线程下可能出现死锁的情况
        nolocks_localtime(&tm, tv.tv_sec, server.timezone, server.daylight_active);
        off = strftime(buf, sizeof(buf), "%d %b %Y %H:%M:%S.", &tm);
        snprintf(buf + off, sizeof(buf) - off, "%03d", (int)tv.tv_usec / 1000);
        if (server.sentinel_mode) {
            role_char = 'X'; // 哨兵
        }
        else if (pid != server.pid) {
            role_char = 'C'; // TODO 目前看来,启动之初都会走这
        }
        else {
            role_char = (server.masterhost ? 'S' : 'M'); // master 或 slave
        }
        fprintf(fp, "%d:%c %s %c %s\n", (int)getpid(), role_char, buf, c[level], msg);
    }
    fflush(fp);

    if (!log_to_stdout)
        fclose(fp);
    if (server.syslog_enabled)
        syslog(syslogLevelMap[level], "%s", msg);
}

void _serverLog(int level, const char *fmt, ...) {
    va_list ap;
    char msg[LOG_MAX_LEN];

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    serverLogRaw(level, msg);
}

void serverLogFromHandler(int level, const char *msg) {
    int fd;
    int log_to_stdout = server.logfile[0] == '\0';
    char buf[64];

    if ((level & 0xff) < server.verbosity || (log_to_stdout && server.daemonize))
        return;
    fd = log_to_stdout ? STDOUT_FILENO : open(server.logfile, O_APPEND | O_CREAT | O_WRONLY, 0644);
    if (fd == -1)
        return;
    ll2string(buf, sizeof(buf), getpid());
    if (write(fd, buf, strlen(buf)) == -1)
        goto err;
    if (write(fd, ":signal-handler (", 17) == -1)
        goto err;
    ll2string(buf, sizeof(buf), time(NULL));
    if (write(fd, buf, strlen(buf)) == -1)
        goto err;
    if (write(fd, ") ", 2) == -1)
        goto err;
    if (write(fd, msg, strlen(msg)) == -1)
        goto err;
    if (write(fd, "\n", 1) == -1)
        goto err;
err:
    if (!log_to_stdout)
        close(fd);
}

// 返回UNIX时间,单位为微秒  // 1 秒 = 1 000 000 微秒
long long ustime(void) {
    struct timeval tv;
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec) * 1000000;
    ust += tv.tv_usec;
    return ust;
}

// 返回毫秒格式的 UNIX 时间
// 1 秒 = 1 000 毫秒
mstime_t mstime(void) { // 返回毫秒
    return ustime() / 1000;
}

// 在RDB转储或AOF重写之后，我们使用_exit()而不是exit()退出子进程，因为后者可能与父进程使用的相同文件对象交互。但是，如果我们正在测试覆盖率，则使用正常的exit()来获得正确的覆盖率信息。
void exitFromChild(int retcode) {
#ifdef COVERAGE_TEST
    exit(retcode);
#else
    _exit(retcode);
#endif
}

/*====================== 哈希表类型实现 ==================== */
// 这是一个哈希表类型，使用SDS动态字符串库作为键，redis对象作为值(对象可以保存SDS字符串，列表，集)。

void dictVanillaFree(dict *d, void *val) {
    UNUSED(d);
    zfree(val);
}

void dictListDestructor(dict *d, void *val) {
    UNUSED(d);
    listRelease((list *)val);
}

void dictObjectDestructor(dict *d, void *val) {
    UNUSED(d);
    if (val == NULL)
        return; // 惰性释放会将值设置为NULL。
    decrRefCount(val);
}

void dictSdsDestructor(dict *d, void *val) {
    UNUSED(d);
    sdsfree(val);
}

void *dictSdsDup(dict *d, const void *key) {
    UNUSED(d);
    return sdsdup((const sds)key);
}
/*====================== hash函数 ==================== */
// dictSdsHash、dictSdsCaseHash、dictObjHash、dictEncObjHash
// 大小写敏感,key是char*
uint64_t dictSdsHash(const void *key) {
    return dictGenHashFunction((unsigned char *)key, sdslen((char *)key));
}
// 大小写不敏感
uint64_t dictSdsCaseHash(const void *key) {
    return dictGenCaseHashFunction((unsigned char *)key, sdslen((char *)key));
}
// 大小写敏感,key是robj*
uint64_t dictObjHash(const void *key) {
    const robj *o = key;
    return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
}
// dictObjHash、dictSdsHash 的集合体,会判断类型
uint64_t dictEncObjHash(const void *key) {
    robj *o = (robj *)key;

    if (sdsEncodedObject(o)) {
        return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
    }
    else if (o->encoding == OBJ_ENCODING_INT) {
        char buf[32];
        int len;

        len = ll2string(buf, 32, (long)o->ptr);
        return dictGenHashFunction((unsigned char *)buf, len);
    }
    else {
        serverPanic("未知字符串编码");
    }
}
// 空终止字符串的字典哈希函数
uint64_t distCStrCaseHash(const void *key) {
    return dictGenCaseHashFunction((unsigned char *)key, strlen((char *)key));
}
/*====================== compare函数 ==================== */
// dictEncObjKeyCompare、dictSdsKeyCompare、dictSdsKeyCaseCompare、dictObjKeyCompare、distCStrKeyCaseCompare
// key是robj*
int dictEncObjKeyCompare(dict *d, const void *key1, const void *key2) {
    robj *o1 = (robj *)key1, *o2 = (robj *)key2; // 类型转换一下
    int cmp;
    if (o1->encoding == OBJ_ENCODING_INT && o2->encoding == OBJ_ENCODING_INT)
        return o1->ptr == o2->ptr;

    // 由于OBJ_STATIC_REFCOUNT，我们避免在没有充分理由的情况下调用getDecodedObject()，因为它会incrRefCount()对象，这是无效的。
    // 因此，我们检查dictFind()是否也适用于静态对象。
    if (o1->refcount != OBJ_STATIC_REFCOUNT) // 在堆栈中分配的对象
        o1 = getDecodedObject(o1);
    if (o2->refcount != OBJ_STATIC_REFCOUNT)
        o2 = getDecodedObject(o2);
    cmp = dictSdsKeyCompare(d, o1->ptr, o2->ptr);
    if (o1->refcount != OBJ_STATIC_REFCOUNT)
        decrRefCount(o1);
    if (o2->refcount != OBJ_STATIC_REFCOUNT)
        decrRefCount(o2);
    return cmp;
}
// key是sds*
int dictSdsKeyCompare(dict *d, const void *key1, const void *key2) {
    int l1, l2;
    UNUSED(d);

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2)
        return 0;
    return memcmp(key1, key2, l1) == 0;
}
// 不区分大小写的版本，用于命令查找表和其他需要不区分大小写的非二进制安全比较的地方。,keys是char*
int dictSdsKeyCaseCompare(dict *d, const void *key1, const void *key2) {
    UNUSED(d);
    return strcasecmp(key1, key2) == 0;
}
// key是robj*
int dictObjKeyCompare(dict *d, const void *key1, const void *key2) {
    const robj *o1 = key1, *o2 = key2;
    return dictSdsKeyCompare(d, o1->ptr, o2->ptr);
}
// 字典不区分大小写的比较函数，用于空终止的字符串 keys是char*
int distCStrKeyCaseCompare(dict *d, const void *key1, const void *key2) {
    UNUSED(d);
    return strcasecmp(key1, key2) == 0;
}
/*====================== 扩容 函数 ==================== */
// 是否允许扩容
int dictExpandAllowed(size_t moreMem, double usedRatio) {
    if (usedRatio <= HASHTABLE_MAX_LOAD_FACTOR) {
        return !overMaxmemoryAfterAlloc(moreMem); // 扩容后是否超出最大内存
    }
    else {
        return 1; // 允许扩容
    }
}

// 以字节为单位返回DB dict条目元数据的大小。在集群模式下，元数据用于构造属于同一集群插槽的dict条目的双链表。请参见集群中的槽位到密钥API。
size_t dictEntryMetadataSize(dict *d) {
    UNUSED(d);
    // NOTICE: 这也会影响getMemoryOverheadData中的overhead_ht_slot_to_keys。
    // 如果我们在这里添加非集群相关的数据，代码也必须修改。
    return server.cluster_enabled ? sizeof(clusterDictEntryMetadata) : 0;
}

// 通用哈希表类型，其中键是Redis对象，值是虚拟指针。
dictType objectKeyPointerValueDictType = {dictEncObjHash, NULL, NULL, dictEncObjKeyCompare, dictObjectDestructor, NULL, NULL};
// 通用哈希表类型，其中键是Redis对象，值是虚拟指针, 并且值可以被销毁。
dictType objectKeyHeapPointerValueDictType = {dictEncObjHash, NULL, NULL, dictEncObjKeyCompare, dictObjectDestructor, dictVanillaFree, NULL};
// set字典类型。键是SDS字符串，不使用值。
dictType setDictType = {dictSdsHash, NULL, NULL, dictSdsKeyCompare, dictSdsDestructor, NULL};
// 有序集合字典类型
dictType zsetDictType = {dictSdsHash, NULL, NULL, dictSdsKeyCompare, NULL, NULL, NULL};
// Db->dict, key是sds字符串，val是Redis对象。
dictType dbDictType = {dictSdsHash, NULL, NULL, dictSdsKeyCompare, dictSdsDestructor, dictObjectDestructor, dictExpandAllowed, dictEntryMetadataSize};
// Db->expires 的dict结构
dictType dbExpiresDictType = {dictSdsHash, NULL, NULL, dictSdsKeyCompare, NULL, NULL, dictExpandAllowed};
// sds->command 的dict结构
dictType commandTableDictType = {dictSdsCaseHash, NULL, NULL, dictSdsKeyCaseCompare, dictSdsDestructor, NULL, NULL};
// 哈希类型哈希表(注意，小哈希表是用列表包表示的)
dictType hashDictType = {dictSdsHash, NULL, NULL, dictSdsKeyCompare, dictSdsDestructor, dictSdsDestructor, NULL};
// 字典类型没有析构函数
dictType sdsReplyDictType = {dictSdsHash, NULL, NULL, dictSdsKeyCompare, NULL, NULL, NULL};
// Keylist哈希表类型有未编码的redis对象作为键和列表作为值。它用于阻塞操作(BLPOP)，并将交换的键映射到等待加载此键的客户端列表。
dictType keylistDictType = {dictObjHash, NULL, NULL, dictObjKeyCompare, dictObjectDestructor, dictListDestructor, NULL};
// 模块系统字典类型。键是模块名，值是RedisModule结构的指针。
dictType modulesDictType = {dictSdsCaseHash, NULL, NULL, dictSdsKeyCaseCompare, dictSdsDestructor, NULL, NULL};
// 缓存迁移的dict类型.
dictType migrateCacheDictType = {dictSdsHash, NULL, NULL, dictSdsKeyCompare, dictSdsDestructor, NULL, NULL};
// 字典用于使用空终止的C字符串进行不区分大小写的搜索。不过，dict中存储的密钥是sds。
dictType stringSetDictType = {distCStrCaseHash, NULL, NULL, distCStrKeyCaseCompare, dictSdsDestructor, NULL, NULL};
// 字典用于使用空终止的C字符串进行不区分大小写的搜索。键和值没有析构函数。
dictType externalStringType = {distCStrCaseHash, NULL, NULL, distCStrKeyCaseCompare, NULL, NULL, NULL};
// 使用zmalloc分配对象作为值的sds对象进行不区分大小写的搜索。
dictType sdsHashDictType = {dictSdsCaseHash, NULL, NULL, dictSdsKeyCaseCompare, dictSdsDestructor, dictVanillaFree, NULL};

// 检查字典的使用率是否低于系统允许的最小比率
int htNeedsResize(dict *dict) {
    long long size, used;
    size = dictSlots(dict); // 容量指数
    used = dictSize(dict);  // 已存储的key数量
    return (size > DICT_HT_INITIAL_SIZE && (used * 100 / size < HASHTABLE_MIN_FILL));
}

// 如果HT中已使用插槽的百分比达到HASHTABLE_MIN_FILL，否则调整哈希表的大小以节省内存
void tryResizeHashTables(int dbid) {
    // 16个数据库是否应该调整大小
    if (htNeedsResize(server.db[dbid].dict)) {
        dictResize(server.db[dbid].dict);
    }
    if (htNeedsResize(server.db[dbid].expires)) {
        dictResize(server.db[dbid].expires);
    }
}

// 虽然服务器在对数据库执行读取/写入命令时会对数据库进行渐进式 rehash ,
// 但如果服务器长期没有执行命令的话,数据库字典的 rehash 就可能一直没办法完成,
// 为了防止出现这种情况,我们需要对数据库执行主动 rehash .
// 函数在执行了主动 rehash 时返回 1 ,否则返回 0 .
int incrementallyRehash(int dbid) {
    // 键的字典
    if (dictIsRehashing(server.db[dbid].dict)) {
        dictRehashMilliseconds(server.db[dbid].dict, 1); // 调整1ms
        return 1;                                        // 已经为这个循环使用了我们的毫秒…
    }
    // 过期字典
    if (dictIsRehashing(server.db[dbid].expires)) {
        dictRehashMilliseconds(server.db[dbid].expires, 1);
        return 1;
    }
    return 0;
}

// 禁止在 AOF 重写期间进行 rehash 操作.
// 以便更好地进行写时复制(否则当发生大小调整时,会复制大量内存页).
void updateDictResizePolicy(void) {
    if (!hasActiveChildProcess()) { // 是否正在运行的AOF、RDB 进程
        dictEnableResize();
    }
    else {
        dictDisableResize(); // 有AOF、RDB进程会阻止扩容
    }
}

const char *strChildType(int type) {
    switch (type) {
        case CHILD_TYPE_RDB:
            return "RDB";
        case CHILD_TYPE_AOF:
            return "AOF";
        case CHILD_TYPE_LDB: // LUA进程
            return "LDB";
        case CHILD_TYPE_MODULE:
            return "MODULE";
        default:
            return "Unknown";
    }
}

// 是否正在运行的AOF\RDB进程
int hasActiveChildProcess() {
    return server.child_pid != -1;
}

// 重置子进程相关的状态
void resetChildState() {
    server.child_type = CHILD_TYPE_NONE;
    server.child_pid = -1;
    server.stat_current_cow_peak = 0;                                                                // 写入字节时拷贝的峰值大小。
    server.stat_current_cow_bytes = 0;                                                               // 当子节点处于活动状态时，在写字节上复制。
    server.stat_current_cow_updated = 0;                                                             // stat_current_cow_bytes最近一次更新时间
    server.stat_current_save_keys_processed = 0;                                                     // 当子节点处于活动状态时，已处理键。
    server.stat_module_progress = 0;                                                                 // 模块保存进度
    server.stat_current_save_keys_total = 0;                                                         // 子程序开始时的键数。
    updateDictResizePolicy();                                                                        // 更新是否允许dict rehash状态
    closeChildInfoPipe();                                                                            // 关闭子进程管道
    moduleFireServerEvent(REDISMODULE_EVENT_FORK_CHILD, REDISMODULE_SUBEVENT_FORK_CHILD_DIED, NULL); // 触发一些事件,由模块捕获,然后执行相应的逻辑
}

// 是不是互斥的ChildType
int isMutuallyExclusiveChildType(int type) {
    return type == CHILD_TYPE_RDB || type == CHILD_TYPE_AOF || type == CHILD_TYPE_MODULE;
}

// 如果此实例的持久性完全关闭，则返回true: RDB和AOF都被禁用。
int allPersistenceDisabled(void) {
    return server.saveparamslen == 0 && server.aof_state == AOF_OFF;
}

/* ======================= 定时任务: 每100ms调用一次 ======================== */

// 每秒操作样本数组中添加一个指标样本。
void trackInstantaneousMetric(int metric, long long current_reading) {
    // 指标
    long long now = mstime();                                                       // 毫秒
    long long t = now - server.inst_metric[metric].last_sample_time;                // 最后一次进行抽样的时间
    long long ops = current_reading - server.inst_metric[metric].last_sample_count; // 最后一次抽样时,服务器已执行命令的数量
    long long ops_sec;

    ops_sec = t > 0 ? (ops * 1000 / t) : 0;

    server.inst_metric[metric].samples[server.inst_metric[metric].idx] = ops_sec;
    server.inst_metric[metric].idx++;                       // 往后移一个
    server.inst_metric[metric].idx %= STATS_METRIC_SAMPLES; // 到达末尾，回到开始
    server.inst_metric[metric].last_sample_time = now;
    server.inst_metric[metric].last_sample_count = current_reading;
}

// 返回所有样本的均值。
long long getInstantaneousMetric(int metric) {
    int j;
    long long sum = 0;

    for (j = 0; j < STATS_METRIC_SAMPLES; j++) sum += server.inst_metric[metric].samples[j];
    return sum / STATS_METRIC_SAMPLES;
}

// 根据情况,缩小查询缓冲区的大小.
// 函数总是返回 0 ,因为它不会中止客户端.
int clientsCronResizeQueryBuffer(client *c) {
    size_t querybuf_size = sdsalloc(c->querybuf);
    time_t idletime = server.unixtime - c->lastinteraction; // 空闲时间，秒

    // 只有在缓冲区实际上至少浪费了几千bytes时才调整查询缓冲区的大小
    if (sdsavail(c->querybuf) > 1024 * 4) {
        // 符合以下两个条件的话,执行大小调整：
        if (idletime > 2) {
            // 空闲了一段时间
            c->querybuf = sdsRemoveFreeSpace(c->querybuf); // ✅
        }
        else if (querybuf_size > PROTO_RESIZE_THRESHOLD && querybuf_size / 2 > c->querybuf_peak) {
            // 客户端不活跃,并且缓冲区大于水位线.
            size_t resize = sdslen(c->querybuf);
            if (resize < c->querybuf_peak)
                resize = c->querybuf_peak;
            if (c->bulklen != -1 && resize < (size_t)c->bulklen)
                resize = c->bulklen;
            c->querybuf = sdsResize(c->querybuf, resize);
        }
    }

    // 重置峰值
    c->querybuf_peak = sdslen(c->querybuf);
    // todo 我们重置为当前使用的或当前处理的批量大小，后者会更大。
    if (c->bulklen != -1 && (size_t)c->bulklen > c->querybuf_peak)
        c->querybuf_peak = c->bulklen;
    return 0;
}

// 调整客户端输出缓冲区
int clientsCronResizeOutputBuffer(client *c, mstime_t now_ms) {
    // now_ms 毫秒
    size_t new_buffer_size = 0;
    char *oldbuf = NULL;
    const size_t buffer_target_shrink_size = c->buf_usable_size / 2;
    const size_t buffer_target_expand_size = c->buf_usable_size * 2;

    // 如果调整大小被禁用，立即返回
    if (!server.reply_buffer_resizing_enabled)
        return 0;
    if (buffer_target_shrink_size >= PROTO_REPLY_MIN_BYTES && c->buf_peak < buffer_target_shrink_size) {
        //  如果最后观察到的缓冲区峰值大小小于缓冲区大小的一半-我们缩小一半。
        new_buffer_size = max(PROTO_REPLY_MIN_BYTES, c->buf_peak + 1);
        server.stat_reply_buffer_shrinks++;
    }
    else if (buffer_target_expand_size < PROTO_REPLY_CHUNK_BYTES * 2 && c->buf_peak == c->buf_usable_size) {
        //  *如果最后观察到的缓冲区峰值大小等于缓冲区大小-我们将大小增加一倍
        new_buffer_size = min(PROTO_REPLY_CHUNK_BYTES, buffer_target_expand_size);
        server.stat_reply_buffer_expands++;
    }

    // 每台服务器重新设置峰值。reply_buffer_peak_reset_time 秒。在客户端空闲的情况下，它将开始收缩。
    if (server.reply_buffer_peak_reset_time >= 0 && now_ms - c->buf_peak_last_reset_time >= server.reply_buffer_peak_reset_time) {
        c->buf_peak = c->bufpos; // 记录了buf数组目前已使用的字节数量
        c->buf_peak_last_reset_time = now_ms;
    }

    if (new_buffer_size) {
        oldbuf = c->buf;
        c->buf = zmalloc_usable(new_buffer_size, &c->buf_usable_size);
        memcpy(c->buf, oldbuf, c->bufpos);
        zfree(oldbuf);
    }
    return 0;
}

// 定时追踪 使用大内存的 clients
// 在INFO输出(客户端部分)，而不必做O(N)扫所有clients。
// 这就是它的工作原理。我们有一个CLIENTS_PEAK_MEM_USAGE_SLOTS槽数组
// 我们跟踪每个插槽中最大的客户端输出和输入缓冲区。每个槽都对应最近的一秒，因为数组是通过执行 UNIXTIME % CLIENTS_PEAK_MEM_USAGE_SLOTS 进行索引的。
// 当我们想要知道最近的内存使用峰值是多少时，我们只需扫描这几个插槽来搜索最大值。
#define CLIENTS_PEAK_MEM_USAGE_SLOTS 8 // 8个槽位，对应最近8秒钟
size_t ClientsPeakMemInput[CLIENTS_PEAK_MEM_USAGE_SLOTS] = {0};
size_t ClientsPeakMemOutput[CLIENTS_PEAK_MEM_USAGE_SLOTS] = {0};

int clientsCronTrackExpansiveClients(client *c, int time_idx) {
    size_t in_usage = sdsZmallocSize(c->querybuf) + c->argv_len_sum + (c->argv ? zmalloc_size(c->argv) : 0);
    size_t out_usage = getClientOutputBufferMemoryUsage(c); // client仍然需要的大小
    // 跟踪到目前为止在这个槽中观察到的最大值。
    if (in_usage > ClientsPeakMemInput[time_idx])
        ClientsPeakMemInput[time_idx] = in_usage;
    if (out_usage > ClientsPeakMemOutput[time_idx])
        ClientsPeakMemOutput[time_idx] = out_usage;
    return 0;
}

// 所有正常的客户端根据它们当前使用的内存大小被放置在一个“内存使用桶”中。
// 我们使用这个函数根据给定的内存使用值找到合适的存储桶。
// 该算法简单地执行log2(mem)来获取桶。这意味着，例如，如果一个客户端的内存使用翻倍，
// 它就会移动到下一个存储桶，如果它减半，我们就会移动到下一个存储桶。
// 有关详细信息，请参阅server.h中的CLIENT_MEM_USAGE_BUCKETS文档。
static inline clientMemUsageBucket *getMemUsageBucket(size_t mem) {
    int size_in_bits = 8 * (int)sizeof(mem); // 是用多少个字节
    // __builtin_clzl   返回\(x\) 二进制下前导 \(0\) 的个数
    int clz = mem > 0 ? __builtin_clzl(mem) : size_in_bits;
    int bucket_idx = size_in_bits - clz;
    if (bucket_idx > CLIENT_MEM_USAGE_BUCKET_MAX_LOG)
        bucket_idx = CLIENT_MEM_USAGE_BUCKET_MAX_LOG;
    else if (bucket_idx < CLIENT_MEM_USAGE_BUCKET_MIN_LOG)
        bucket_idx = CLIENT_MEM_USAGE_BUCKET_MIN_LOG;
    bucket_idx -= CLIENT_MEM_USAGE_BUCKET_MIN_LOG;
    return &server.client_mem_usage_buckets[bucket_idx]; // 内存使用使用大致相同的一个桶集合
}

// 将客户端添加到内存使用桶中。每个桶包含所有具有大致相同内存的客户端。
// 通过这种方式，我们将消耗相同数量内存的客户端分组在一起，
// 并且可以在达到maxmemory-clients(驱逐客户端)时快速释放它们。
int updateClientMemUsage(client *c) {
    // https://www.cnblogs.com/luoming1224/articles/16838470.html
    serverAssert(io_threads_op == IO_THREADS_OP_IDLE);
    size_t mem = getClientMemoryUsage(c, NULL); // 获取客户端连接使用的内存
    int type = getClientType(c);

    // 从旧类别中删除客户端使用的内存的旧值，并将其添加回来。
    if (type != c->last_memory_type) {
        // 之前得记录过
        // 之前类型的 内存使用情况 减少 当前client的内存
        server.stat_clients_type_memory[c->last_memory_type] -= c->last_memory_usage;
        // 更新当前类型的内存和
        server.stat_clients_type_memory[type] += mem;
        c->last_memory_type = type;
    }
    else {
        // 之前不能记录过
        server.stat_clients_type_memory[type] += mem - c->last_memory_usage;
    }
    // 是否允许驱逐
    int allow_eviction = (                                                                //
                             type == CLIENT_TYPE_NORMAL || type == CLIENT_TYPE_PUBSUB) && //
                         !(c->flags & CLIENT_NO_EVICT);

    // 连接的类型可能发生变化，所以
    if (c->mem_usage_bucket) {
        // 先在该连接原来所属bucket中减去其之前使用的内存
        c->mem_usage_bucket->mem_usage_sum -= c->last_memory_usage;
        // 如果不再属于可以被驱逐的类型，则从bucket中删掉
        if (!allow_eviction) {
            listDelNode(c->mem_usage_bucket->clients, c->mem_usage_bucket_node);
            c->mem_usage_bucket = NULL;
            c->mem_usage_bucket_node = NULL;
        }
    }
    if (allow_eviction) {
        // 计算当前使用的内存在连接驱逐池中所属bucket
        clientMemUsageBucket *bucket = getMemUsageBucket(mem);
        bucket->mem_usage_sum += mem; // 在所属bucket中加上该连接所使用的内存
        // 如果该连接所属bucket发生变化，则先从原来的bucket中删掉，再加入新的bucket
        if (bucket != c->mem_usage_bucket) {
            if (c->mem_usage_bucket)
                listDelNode(c->mem_usage_bucket->clients, c->mem_usage_bucket_node);
            c->mem_usage_bucket = bucket;
            listAddNodeTail(bucket->clients, c);
            c->mem_usage_bucket_node = listLast(bucket->clients);
        }
    }
    // 记住我们添加了什么，下次再删除它。
    c->last_memory_usage = mem;
    return 0;
}

// 从serverCron和cronUpdateMemoryStats调用来更新缓存的内存指标。
void cronUpdateMemoryStats() {
    // 记录自服务器启动以来所使用的最大内存。
    if (zmalloc_used_memory() > server.stat_peak_memory)
        server.stat_peak_memory = zmalloc_used_memory();

    run_with_period(100) { // 100毫秒执行一次
                           // 这里对RSS和其他指标进行抽样，因为这是一个相对较慢的调用。
                           //*我们必须采样zmalloc_used的同时，我们采取rss，否则碎片比率计算可能会关闭(两个样本在不同时间的比率)
        server.cron_malloc_stats.process_rss = zmalloc_get_rss();
        server.cron_malloc_stats.zmalloc_used = zmalloc_used_memory();
        // 对分配器信息进行采样可能很慢。
        //*它将显示的碎片率可能更准确，它排除了其他RSS页面，如:共享库，LUA和其他非zmalloc分配，和分配器保留页面，可以被占用(所有不是实际的碎片)
        zmalloc_get_allocator_info(&server.cron_malloc_stats.allocator_allocated, &server.cron_malloc_stats.allocator_active, &server.cron_malloc_stats.allocator_resident);
        // 如果分配器没有提供这些统计数据，伪造它们，这样碎片信息仍然显示一些(不准确的指标)
        if (!server.cron_malloc_stats.allocator_resident) {
            // LUA内存不是zmalloc_used的一部分，但它是进程RSS的一部分，所以我们必须扣除它，以便能够计算正确的“分配器碎片”比率
            size_t lua_memory = evalMemory();
            server.cron_malloc_stats.allocator_resident = server.cron_malloc_stats.process_rss - lua_memory;
        }
        if (!server.cron_malloc_stats.allocator_active)
            server.cron_malloc_stats.allocator_active = server.cron_malloc_stats.allocator_resident;
        if (!server.cron_malloc_stats.allocator_allocated)
            server.cron_malloc_stats.allocator_allocated = server.cron_malloc_stats.zmalloc_used;
    }
}

// 返回 跟踪中的客户端内存使用的最大样本。[输入、输出缓冲区]
void getExpansiveClientsInfo(size_t *in_usage, size_t *out_usage) {
    size_t i = 0, o = 0;
    for (int j = 0; j < CLIENTS_PEAK_MEM_USAGE_SLOTS; j++) {
        if (ClientsPeakMemInput[j] > i)
            i = ClientsPeakMemInput[j];
        if (ClientsPeakMemOutput[j] > o)
            o = ClientsPeakMemOutput[j];
    }
    *in_usage = i;
    *out_usage = o;
}

// 该函数由serverCron()调用，用于在客户端上执行需要经常执行的重要操作。
// 例如，我们使用这个函数来在超时后断开客户端，包括在某些阻塞命令中阻塞的客户端，超时时间为非零。
// 该函数努力每秒处理所有客户端，即使这不能严格保证，因为调用serverCron()的实际频率可能低于server。Hz的情况下延迟事件，如缓慢的命令。
// 对于这个函数和它调用的函数来说，非常重要的是要非常快:有时Redis有几十个连接的客户端，以及默认的服务器。Hz值为10，因此有时我们需要每秒处理数千个客户端，将此函数转换为延迟的来源。
#define CLIENTS_CRON_MIN_ITERATIONS 5

void clientsCron(void) {
    // 这个函数每次执行都会处理至少 numclients/server.hz 个客户端.
    // 因为通常情况下（如果没有大的延迟事件）,这个函数每秒被调用server.hz次,在平均情况下,我们在1秒内处理所有的客户端.
    int numclients = listLength(server.clients); // 客户端数量

    int iterations = numclients / server.hz; // 要处理的客户端数量

    mstime_t now = mstime();

    // 处理至少几个客户端，即使我们需要处理少于CLIENTS_CRON_MIN_ITERATIONS来满足每秒处理每个客户端一次的约定。
    if (iterations < CLIENTS_CRON_MIN_ITERATIONS) // 至少要处理 5 个客户端
        iterations = (numclients < CLIENTS_CRON_MIN_ITERATIONS) ? numclients : CLIENTS_CRON_MIN_ITERATIONS;

    int curr_peak_mem_usage_slot = server.unixtime % CLIENTS_PEAK_MEM_USAGE_SLOTS;
    int zeroidx = (curr_peak_mem_usage_slot + 1) % CLIENTS_PEAK_MEM_USAGE_SLOTS;
    ClientsPeakMemInput[zeroidx] = 0;
    ClientsPeakMemOutput[zeroidx] = 0; //

    while (listLength(server.clients) && iterations--) {
        client *c;
        listNode *head;
        listRotateTailToHead(server.clients); // 移除尾部节点并将其插入到头部
        head = listFirst(server.clients);
        c = listNodeValue(head);
        if (clientsCronHandleTimeout(c, now)) // 检查客户端,并在客户端超时时关闭它
            continue;
        if (clientsCronResizeQueryBuffer(c)) // 根据情况,缩小客户端查询缓冲区的大小
            continue;
        if (clientsCronResizeOutputBuffer(c, now)) // 根据情况,缩小客户端输出缓冲区的大小
            continue;
        if (clientsCronTrackExpansiveClients(c, curr_peak_mem_usage_slot)) // 定时追踪 使用大内存的 clients   ,更新 最大的内存 使用量
            continue;
        if (updateClientMemUsage(c)) // 针对client   更新每种类型client 所有内存的使用情况
            continue;
        if (closeClientOnOutputBufferLimitReached(c, 0)) // 如果达到输出缓冲区大小的软限制或硬限制，异步关闭客户端。
            continue;
    }
}

// 对数据库执行删除过期键,调整大小,以及主动和渐进式 rehash
void databasesCron(void) {
    if (server.active_expire_enabled) { // 是否启用自动过期
        if (iAmMaster()) {              // 如果服务器不是从服务器,那么执行主动过期键清除
            // 清除模式为 CYCLE_SLOW ,这个模式会尽量多清除过期键
            activeExpireCycle(ACTIVE_EXPIRE_CYCLE_SLOW); // 清理主实例的过期键。
        }
        else {
            expireSlaveKeys(); // 清理从实例上的过期键。
        }
    }

    // 逐步清理keys
    activeDefragCycle();

    // 如果需要，执行哈希表重哈希，但仅当没有其他进程将DB保存在磁盘上时。
    // 否则重哈希是不好的，因为会导致大量内存页的写时复制。

    // 在没有 BGSAVE 或者 BGREWRITEAOF 执行时,对哈希表进行 rehash
    if (!hasActiveChildProcess()) {
        // 我们使用全局计数器，因此如果我们在给定的DB处停止计算，我们将能够在下一个cron循环迭代中从连续的开始。
        static unsigned int resize_db = 0;
        static unsigned int rehash_db = 0;
        int dbs_per_call = CRON_DBS_PER_CALL; // 每次调用，处理的数据量
        int j;

        if (dbs_per_call > server.dbnum)
            dbs_per_call = server.dbnum;

        // 调整字典的大小
        for (j = 0; j < dbs_per_call; j++) {
            // 如果HT中已使用插槽的百分比达到HASHTABLE_MIN_FILL，否则调整哈希表的大小以节省内存
            tryResizeHashTables(resize_db % server.dbnum);
            resize_db++;
        }

        // 对字典进行渐进式 rehash
        if (server.active_rehashing) { // 在执行 serverCron() 时进行渐进式 rehash
            for (j = 0; j < dbs_per_call; j++) {
                int work_done = incrementallyRehash(rehash_db); // 主动触发rehash
                if (work_done) {
                    // 如果函数做了一些工作，那么就到此为止，我们将在下一个cron循环中做更多工作。
                    break;
                }
                else {
                    rehash_db++;
                    rehash_db %= server.dbnum;
                }
            }
        }
    }
}

// OK
static inline void updateCachedTimeWithUs(int update_daylight_info, const long long ustime) {
    server.ustime = ustime;
    server.mstime = server.ustime / 1000;
    time_t unix_time = server.mstime / 1000;
    atomicSet(server.unixtime, unix_time); // 👌✅

    // 为了获得daylight的信息
    if (update_daylight_info) {
        struct tm tm;
        time_t ut = server.unixtime;
        // 用来将参数tm所指的tm结构数据转换成从公元1970年1月1日0时0分0 秒算起至今的UTC时间所经过的秒数.并存储到ut
        localtime_r(&ut, &tm);
        server.daylight_active = tm.tm_isdst;
    }
}

// 我们在全局状态下取一个unix时间的缓存值,因为有了虚拟内存和老化,需要在对象中存储当前时间.
// 在每次访问对象时都要将当前时间存储在对象中.
// 每一个对象的访问都要存储当前的时间,而准确性是不需要的.访问一个全局变量是比调用time(NULL)快很多.
//
// 只有在从serverCron()调用这个函数时才会更新daylight
void updateCachedTime(int update_daylight_info) {
    const long long us = ustime();
    updateCachedTimeWithUs(update_daylight_info, us);
}

// 检查子进程是否结束
void checkChildrenDone(void) {
    int statloc = 0;
    pid_t pid;
    // waitpid主要用于根据进程ID号等待指定的子进程
    if ((pid = waitpid(-1, &statloc, WNOHANG)) != 0) {
        // WIFEXITED(status) 若此值为非0 表明进程正常结束。
        // WIFSIGNALED(status)为非0 表明进程异常终止。
        // WEXITSTATUS(status)获取进程退出状态(exit时参数)
        int exitcode = WIFEXITED(statloc) ? WEXITSTATUS(statloc) : -1;
        int bysignal = 0;

        if (WIFSIGNALED(statloc)) {
            bysignal = WTERMSIG(statloc);
        }

        // sigKillChildHandler捕获信号并调用exit()，但我们必须确保不要错误地标记lastbgsave_status等。我们可以直接通过SIGUSR1终止子进程，而不处理它
        if (exitcode == SERVER_CHILD_NOERROR_RETVAL) {
            bysignal = SIGUSR1;
            exitcode = 1;
        }

        if (pid == -1) { // 没有子进程
            serverLog(LL_WARNING, "waitpid() 返回了一个错误: %s.  child_type: %s, child_pid = %d", strerror(errno), strChildType(server.child_type), (int)server.child_pid);
        }
        else if (pid == server.child_pid) {
            if (server.child_type == CHILD_TYPE_RDB) { // BGSAVE 执行完毕
                backgroundSaveDoneHandler(exitcode, bysignal);
            }
            else if (server.child_type == CHILD_TYPE_AOF) { // BGREWRITEAOF 执行完毕
                backgroundRewriteDoneHandler(exitcode, bysignal);
            }
            else if (server.child_type == CHILD_TYPE_MODULE) { // module 子进程
                ModuleForkDoneHandler(exitcode, bysignal);
            }
            else {
                serverPanic("未知的子进程类型 %d :子进程ID %d", server.child_type, server.child_pid);
                exit(1);
            }
            if (!bysignal && exitcode == 0)
                receiveChildInfo(); // 接收子进程数据
            resetChildState();
        }
        else {
            if (!ldbRemoveChild(pid)) {
                serverLog(LL_WARNING, "警告，检测到子pid不匹配: %ld", (long)pid);
            }
        }

        // 立即启动任何挂起的fork。
        replicationStartPendingFork();
    }
}

// 这是 Redis 的时间中断器,每秒调用 server.hz 次.
// 定期对自身的资源和状态检查和调整,从而确保服务器可以长期、稳定运行
// 1、更新服务器各类统计信息、比如时间、内存占用、数据库占用情况
// 2、清理数据库中的过期键值对  对数据库进行渐增式 Rehash
// 3、关闭、清理连接失效的客户端
// 4、尝试进行AOF、RDB持久化操作
// 5、如果是主服务器,那么对从服务器进行定期同步
// 6、如果处于集群模式,对集群进行定期同步和连接测试
int serverCron(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    //  serverLog(LL_DEBUG, "serverCron ---> %lld\n", ustime());
    int j;
    UNUSED(eventLoop);
    UNUSED(id);
    UNUSED(clientData);

    // 如果我们没有足够快地返回这里,交付将到达信号处理程序的SIGALRM
    if (server.watchdog_period) {
        watchdogScheduleSignal(server.watchdog_period);
    }

    // 更新server的时间
    updateCachedTime(1);

    server.hz = server.config_hz;
    // 调整服务器。Hz值为已配置的客户端数量。如果我们有很多客户端，我们希望以更高的频率调用serverCron()。
    if (server.dynamic_hz) {
        while (listLength(server.clients) / server.hz > MAX_CLIENTS_PER_CLOCK_TICK) {
            server.hz *= 2;
            if (server.hz > CONFIG_MAX_HZ) {
                server.hz = CONFIG_MAX_HZ;
                break;
            }
        }
    }

    // 出于调试目的:如果pause_cron打开，则跳过实际的cron工作
    if (server.pause_cron) {
        return 1000 / server.hz;
    }
    // 记录服务器执行命令的次数
    run_with_period(100) { // 100ms执行一次
        long long stat_net_input_bytes, stat_net_output_bytes;
        atomicGet(server.stat_net_input_bytes, stat_net_input_bytes);
        atomicGet(server.stat_net_output_bytes, stat_net_output_bytes);

        trackInstantaneousMetric(STATS_METRIC_COMMAND, server.stat_numcommands);
        trackInstantaneousMetric(STATS_METRIC_NET_INPUT, stat_net_input_bytes);
        trackInstantaneousMetric(STATS_METRIC_NET_OUTPUT, stat_net_output_bytes);
    }
    unsigned int lruclock = getLRUClock(); // 计算全局LRU时钟值
    atomicSet(server.lru_clock, lruclock); // 初始化 LRU 时间

    cronUpdateMemoryStats();

    // 如果收到进程结束信号,则执行server关闭操作
    if (server.shutdown_asap && !isShutdownInitiated()) { // 有shutdown的标志，但还没shutdown
        int shutdownFlags = SHUTDOWN_NOFLAGS;
        if (server.last_sig_received == SIGINT && server.shutdown_on_sigint)
            shutdownFlags = server.shutdown_on_sigint;
        else if (server.last_sig_received == SIGTERM && server.shutdown_on_sigterm)
            shutdownFlags = server.shutdown_on_sigterm;

        if (prepareForShutdown(shutdownFlags) == C_OK) // 暂停前的准备
            exit(0);
    }
    else if (isShutdownInitiated()) { // shutdown 超时时间
        if (server.mstime >= server.shutdown_mstime || isReadyToShutdown()) {
            if (finishShutdown() == C_OK)
                exit(0);
        }
    }

    // 打印数据库的键值对信息
    if (server.verbosity <= LL_VERBOSE) {
        run_with_period(5000) { // 5000毫秒 执行一次
            for (j = 0; j < server.dbnum; j++) {
                long long size, used, vkeys;
                size = dictSlots(server.db[j].dict);    // 可用键值对的数量
                used = dictSize(server.db[j].dict);     // 已用键值对的数量
                vkeys = dictSize(server.db[j].expires); // 带有过期时间的键值对数量

                // 用 LOG 打印数量
                if (used || vkeys) {
                    serverLog(LL_VERBOSE, "数据库 %d: %lld keys (%lld volatile) in %lld slots 哈希表.", j, used, vkeys, size);
                }
            }
        }
    }

    // 如果服务器没有运行在 SENTINEL 模式下,那么打印客户端的连接信息
    if (!server.sentinel_mode) {
        run_with_period(5000) {
            serverLog(LL_DEBUG, "serverCron: %lu 个客户端已连接 (%lu 从链接), %zu 字节在使用", listLength(server.clients) - listLength(server.slaves), listLength(server.slaves), zmalloc_used_memory());
        }
    }

    clientsCron();   // 执行客户端的异步操作   检查客户端,关闭超时客户端,并释放客户端多余的缓冲区
    databasesCron(); // 执行数据库的后台操作

    // 如果 BGSAVE 和 BGREWRITEAOF 都没有在执行
    // 并且有一个 BGREWRITEAOF 在等待,那么执行 BGREWRITEAOF
    if (!hasActiveChildProcess() && server.aof_rewrite_scheduled && !aofRewriteLimited()) {
        rewriteAppendOnlyFileBackground(); // serverCron
    }

    // 检查 BGSAVE 或者 BGREWRITEAOF 是否已经执行完毕
    if (hasActiveChildProcess() || ldbPendingChildren()) {
        run_with_period(1000) { // 1000ms执行一次
            receiveChildInfo(); // 接收子进程数据
        }
        checkChildrenDone(); // 检查子进程是否结束
    }
    else {
        // 既然没有 BGSAVE 或者 BGREWRITEAOF 在执行,那么检查是否需要执行它们
        // 遍历所有保存条件,看是否需要执行 BGSAVE 命令
        for (j = 0; j < server.saveparamslen; j++) {
            struct saveparam *sp = server.saveparams + j;

            // 检查是否有某个保存条件已经满足了
            if (                                                                                                          //
                server.dirty >= sp->changes &&                                                                            // server.dirty 上次 rdb 结束到本次rdb 开始时键值对改变的个数
                server.unixtime - server.lastsave > sp->seconds &&                                                        // 秒数
                (server.unixtime - server.lastbgsave_try > CONFIG_BGSAVE_RETRY_DELAY || server.lastbgsave_status == C_OK) // 稍等片刻再试一次、状态OK
            ) {
                serverLog(LL_NOTICE, "%d 改变了在 %d 秒中. 保存中...", sp->changes, (int)sp->seconds);
                rdbSaveInfo rsi, *rsiptr;
                rsiptr = rdbPopulateSaveInfo(&rsi); // 填充rdi信息
                // 执行 BGSAVE
                rdbSaveBackground(SLAVE_REQ_NONE, server.rdb_filename, rsiptr); // ✅
                break;
            }
        }
        // 如果AOF功能启用、没有RDB子进程和AOF重写子进程在执行、AOF文件大小比例设定了阈值,
        // 以及AOF文件大小绝对值超出了阈值,那么,进一步判断AOF文件大小比例是否超出阈值
        if (server.aof_state == AOF_ON &&                         //
            !hasActiveChildProcess() &&                           //
            server.aof_rewrite_perc &&                            // 增长率
            server.aof_current_size > server.aof_rewrite_min_size // AOF 文件的当前大小大于执行 BGREWRITEAOF 所需的最小大小
        ) {
            // 最后一次执行 BGREWRITEAOF 时,AOF 文件的大小
            long long base = server.aof_rewrite_base_size ? server.aof_rewrite_base_size : 1;
            // AOF 文件当前的体积相对于 base 的体积的百分比

            long long growth = (server.aof_current_size * 100 / base) - 100;

            if (growth >= server.aof_rewrite_perc && // 如果增长体积的百分比超过了 growth ,那么执行 BGREWRITEAOF
                !aofRewriteLimited()                 // AOF 文件大小绝对值没有超出阈值
            ) {
                serverLog(LL_NOTICE, "开始自动重写aof %lld%% growth", growth);
                rewriteAppendOnlyFileBackground(); // serverCron
            }
        }
    }
    // 只是为了进行防御性编程，避免在需要时忘记调用此函数。

    updateDictResizePolicy();

    // 根据 AOF 政策, 考虑是否需要将 AOF 缓冲区中的内容写入到 AOF 文件中
    if ((server.aof_state == AOF_ON || server.aof_state == AOF_WAIT_REWRITE) && server.aof_flush_postponed_start) {
        flushAppendOnlyFile(0);
    }

    /* AOF write errors: in this case we have a buffer to flush as well and
     * clear the AOF error in case of success to make the DB writable again,
     * however to try every second is enough in case of 'hz' is set to
     * a higher frequency. */
    // 每1秒执行1次,检查AOF是否有写错误
    run_with_period(1000) {
        if ((server.aof_state == AOF_ON || server.aof_state == AOF_WAIT_REWRITE) && server.aof_last_write_status == C_ERR) {
            flushAppendOnlyFile(0);
        }
    }

    // 如果需要，清除暂停的客户端状态。
    checkClientPauseTimeoutAndReturnIfPaused();

    // 重新连接主服务器、向主服务器发送 ACK 、判断数据发送失败情况、断开本服务器超时的从服务器,等等
    if (server.failover_state != NO_FAILOVER) {
        // 故障转移情况下,100ms执行一次
        run_with_period(100) replicationCron();
    }
    else {
        // 正常情况下,1s执行一次
        run_with_period(1000) replicationCron();
    }

    // 每100ms执行一次，集群模式下  就向一个随机节点发送 gossip 信息
    run_with_period(100) {
        if (server.cluster_enabled) {
            // 如果服务器运行在集群模式下,那么执行集群操作
            clusterCron();
        }
    }

    if (server.sentinel_mode) {
        // 如果服务器运行在 sentinel 模式下,那么执行 SENTINEL 的主函数
        sentinelTimer();
    }

    run_with_period(1000) {
        migrateCloseTimedoutSockets(); // 集群: 清除过期的MIGRATE cached socket。
    }

    // 如果没有阻塞的工作,停止子线程
    stopThreadedIOIfNeeded();

    // 如果需要，调整跟踪键表的大小。
    // 这也是在每个命令执行时执行的，但是我们希望确保如果最后执行的命令通过CONFIG SET改变了值，即使完全空闲，服务器也会执行该操作。
    if (server.tracking_clients) {
        trackingLimitUsedSlots();
    }

    // 如果设置了相应标志位，则启动定时BGSAVE。
    // 当我们因为AOF重写正在进行而被迫推迟BGSAVE时，这很有用。
    // 注意:这段代码必须在上面的replicationCron()调用之后，所以在重构这个文件时确保保持这个顺序。
    // 这很有用，因为我们希望优先考虑用于复制的RDB。
    if (!hasActiveChildProcess() &&                                                                               //
        server.rdb_bgsave_scheduled &&                                                                            //
        (server.unixtime - server.lastbgsave_try > CONFIG_BGSAVE_RETRY_DELAY || server.lastbgsave_status == C_OK) //
    ) {
        rdbSaveInfo rsi, *rsiptr;
        rsiptr = rdbPopulateSaveInfo(&rsi);
        if (rdbSaveBackground(SLAVE_REQ_NONE, server.rdb_filename, rsiptr) == C_OK)
            server.rdb_bgsave_scheduled = 0;
    }

    run_with_period(100) {
        if (moduleCount()) { // 返回已注册模块的数量
            modulesCron();
        }
    }

    // 触发cron循环模块事件。
    RedisModuleCronLoopV1 ei = {REDISMODULE_CRON_LOOP_VERSION, server.hz};
    moduleFireServerEvent(REDISMODULE_EVENT_CRON_LOOP, 0, &ei);

    server.cronloops++; // 增加 loop 计数器

    return 1000 / server.hz;
}

// 开始阻塞
void blockingOperationStarts() {
    if (!server.blocking_op_nesting++) {
        updateCachedTime(0);
        server.blocked_last_cron = server.mstime;
    }
}
// 解除阻塞
void blockingOperationEnds() {
    if (!(--server.blocking_op_nesting)) {
        server.blocked_last_cron = 0;
    }
}

// 这个函数在RDB或AOF运行过程中，以及在阻塞的脚本过程中 填补了serverCron的作用。
// 它试图以与配置的服务器相似的速度完成其职责，并更新 cronloops 变量，以便与serverCron相似，可以使用run_with_period。
void whileBlockedCron() { //
    // 在这里，我们可能想执行一些cron作业（通常是每秒做server.hz次）。
    // 由于这个函数依赖于对blockingOperationStarts的调用，让我们确保它被完成。
    serverAssert(server.blocked_last_cron);

    if (server.blocked_last_cron >= server.mstime)
        return;

    mstime_t latency;
    latencyStartMonitor(latency);

    // 在某些情况下，我们可能会被调用大的时间间隔，所以我们可能需要在这里做额外的工作。
    // 这是因为serverCron中的一些函数依赖于每10毫秒左右执行一次的事实。
    // 例如，如果activeDefragCycle需要利用25%的cpu，它将利用2.5ms，所以我们 需要多次调用它。
    long hz_ms = 1000 / server.hz;
    while (server.blocked_last_cron < server.mstime) {
        // 逐步整理keys,没有逻辑
        activeDefragCycle();
        server.blocked_last_cron += hz_ms;
        server.cronloops++;
    }

    // 在加载期间更新内存统计数据(不包括阻塞脚本)
    if (server.loading) {
        cronUpdateMemoryStats();
    }

    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("while-blocked-cron", latency);

    // 我们在加载过程中收到了一个SIGTERM，在这里以安全的方式关闭，因为在信号处理程序中这样做是不正确的。
    if (server.shutdown_asap && server.loading) {
        // 尝试关闭服务器
        if (prepareForShutdown(SHUTDOWN_NOSAVE) == C_OK) {
            exit(0);
        }
        // 如果关闭失败,那么打印 LOG ,并移除关闭标识
        serverLog(LL_WARNING, "已收到SIGTERM，但试图关闭服务器时出错，请检查日志以获取更多信息");
        server.shutdown_asap = 0;
        server.last_sig_received = 0;
    }
}

// 发送 恢复命令 到slave
static void sendGetackToReplicas(void) {
    robj *argv[3];
    argv[0] = shared.replconf;
    argv[1] = shared.getack;
    argv[2] = shared.special_asterick;
    replicationFeedSlaves(server.slaves, server.slaveseldb, argv, 3); // 👌🏻✅
}

extern int ProcessingEventsWhileBlocked; // 阻塞后处理事件

// 每次处理事件之前执行
void beforeSleep(struct aeEventLoop *eventLoop) {
    UNUSED(eventLoop);

    size_t zmalloc_used = zmalloc_used_memory();  // 获取使用的内存
    if (zmalloc_used > server.stat_peak_memory) { // 初始时是0
        // 已使用内存峰值
        server.stat_peak_memory = zmalloc_used;
    }

    /*调用关键函数的子集,以防我们从 processevenswhileblocked() 重新进入事件循环.
     * 注意,在本例中,我们跟踪正在处理的事件数量,因为processevenswhileblocked()希望在不再有事件需要处理时尽快停止.*/

    if (ProcessingEventsWhileBlocked) {
        uint64_t processed = 0;
        processed += handleClientsWithPendingReadsUsingThreads();
        processed += tlsProcessPendingData();
        if (server.aof_state == AOF_ON || server.aof_state == AOF_WAIT_REWRITE) {
            flushAppendOnlyFile(0);
        }
        processed += handleClientsWithPendingWrites();
        processed += freeClientsInAsyncFreeQueue();
        server.events_processed_while_blocked += processed;
        return;
    }

    /* 处理阻塞客户端的超时.*/
    handleBlockedClientsTimeout();

    /* 我们应该在事件循环后尽快处理挂起的读取.*/
    handleClientsWithPendingReadsUsingThreads();

    /* 处理TLS待处理数据.(必须在flushAppendOnlyFile之前完成) */
    tlsProcessPendingData();

    /* 如果tls仍然有挂起的未读数据,则不要休眠. */
    aeSetDontWait(server.el, tlsHasPendingData());

    /* 在休眠前调用Redis集群功能.注意这个函数可能会改变Redis Cluster的状态(从ok到fail,或者反过来),所以在这个函数中服务未阻塞的客户端之前调用它是一个好主意.*/
    if (server.cluster_enabled) {
        // 在进入下个事件循环前,执行一些集群收尾工作
        clusterBeforeSleep();
    }

    // 执行一次快速的主动过期检查
    if (server.active_expire_enabled && server.masterhost == NULL) {
        activeExpireCycle(ACTIVE_EXPIRE_CYCLE_FAST);
    }
    /* 解除WAIT中为同步复制而阻塞的所有客户端. */
    if (listLength(server.clients_waiting_acks)) {
        processClientsWaitingReplicas();
    }

    /* 检查是否有客户端被实现阻塞命令的模块解除阻塞. */
    if (moduleCount()) {
        moduleFireServerEvent(REDISMODULE_EVENT_EVENTLOOP, REDISMODULE_SUBEVENT_EVENTLOOP_BEFORE_SLEEP, NULL);
        moduleHandleBlockedClients();
    }

    /* 尝试为刚刚解除阻塞的客户端处理挂起的命令. */
    if (listLength(server.unblocked_clients)) {
        processUnblockedClients();
    }

    // 如果在之前的事件循环迭代中至少有一个客户端被阻塞,则向所有从服务器发送一个ACK请求.
    // 注意,我们在processUnblockedClients()之后执行此操作,
    // 因此,如果有多个流水线的WAIT,并且刚刚解除阻塞的WAIT再次被阻塞,那么在没有其他事件循环事件的情况下,我们不必等待服务器cron周期.看到# 6623.
    // 我们也不会在客户端暂停时发送ack,因为它会增加复制积压,如果我们仍然是主服务器,它们会在暂停后发送.
    if (server.get_ack_from_slaves && !checkClientPauseTimeoutAndReturnIfPaused()) {
        sendGetackToReplicas();
        server.get_ack_from_slaves = 0;
    }

    /*我们可能已经收到客户端关于当前偏移量的更新.注意:在接收到ACK的地方不能这样做,因为故障转移将断开我们的客户端.*/
    updateFailoverStatus();

    /*因为我们依赖于current_client发送预定的无效消息,我们必须在每个命令之后刷新它们,所以当我们到达这里时,列表必须为空.*/
    serverAssert(listLength(server.tracking_pending_keys) == 0);

    /*发送无效消息给参与广播(BCAST)模式的客户端.*/
    trackingBroadcastInvalidationMessages();

    // 尝试每隔一段时间处理阻塞的客户端.
    //
    // 示例:模块从计时器回调中调用RM_SignalKeyAsReady(所以我们根本不访问processCommand()).
    // 必须在flushAppendOnlyFile之前执行,以防appendfsync=always,因为未阻塞的客户端可能会写入数据.
    handleClientsBlockedOnKeys();

    // 将 AOF 缓冲区的内容写入到 AOF 文件
    if (server.aof_state == AOF_ON || server.aof_state == AOF_WAIT_REWRITE) {
        flushAppendOnlyFile(0);
    }

    // 使用挂起的输出缓冲区处理写操作.
    handleClientsWithPendingWritesUsingThreads();

    // 关闭那些需要异步关闭的客户端
    freeClientsInAsyncFreeQueue();

    //  逐步释放 环形缓冲复制队列 ,10倍的正常速度是为了尽可能地释放
    if (server.repl_backlog) {
        incrementalTrimReplicationBacklog(10 * REPL_BACKLOG_TRIM_BLOCKS_PER_CALL);
    }

    /* 如果某些客户端占用太多内存,则断开它们的连接. */
    evictClients();

    // 在我们sleep 进入网络等待【epoll wait 】 之前,通过释放GIL让线程访问数据集.Redis主线程现在不会触及任何东西.
    if (moduleCount()) {
        moduleReleaseGIL();
    }
    // 不要在moduleReleaseGIL下面添加任何东西
}

void afterSleep(struct aeEventLoop *eventLoop) {
    UNUSED(eventLoop);

    // 获取模块GIL，这样它们的线程就不会触及任何东西。
    if (!ProcessingEventsWhileBlocked) { //
        if (moduleCount()) {
            mstime_t latency;
            latencyStartMonitor(latency);
            moduleAcquireGIL();
            moduleFireServerEvent(REDISMODULE_EVENT_EVENTLOOP, REDISMODULE_SUBEVENT_EVENTLOOP_AFTER_SLEEP, NULL);
            latencyEndMonitor(latency);
            latencyAddSampleIfNeeded("module-acquire-GIL", latency);
        }
    }
}

/* =========================== 服务器初始化 ======================== */
// 服务器初始化
void createSharedObjects(void) {
    int j;
    // 常用回复
    shared.crlf = createObject(OBJ_STRING, sdsnew("\r\n"));
    shared.ok = createObject(OBJ_STRING, sdsnew("+OK\r\n"));
    shared.emptybulk = createObject(OBJ_STRING, sdsnew("$0\r\n\r\n"));
    shared.czero = createObject(OBJ_STRING, sdsnew(":0\r\n"));
    shared.cone = createObject(OBJ_STRING, sdsnew(":1\r\n"));
    shared.emptyarray = createObject(OBJ_STRING, sdsnew("*0\r\n"));
    shared.pong = createObject(OBJ_STRING, sdsnew("+PONG\r\n"));
    shared.queued = createObject(OBJ_STRING, sdsnew("+QUEUED\r\n"));
    shared.emptyscan = createObject(OBJ_STRING, sdsnew("*2\r\n$1\r\n0\r\n*0\r\n"));
    // 常用字符
    shared.space = createObject(OBJ_STRING, sdsnew(" "));
    shared.plus = createObject(OBJ_STRING, sdsnew("+"));

    // 常用错误回复
    shared.wrongtypeerr = createObject(OBJ_STRING, sdsnew("-WRONGTYPE 对持有错误类型值的键进行操作\r\n"));
    shared.err = createObject(OBJ_STRING, sdsnew("-ERR\r\n"));
    shared.nokeyerr = createObject(OBJ_STRING, sdsnew("-ERR 没有key\r\n"));
    shared.syntaxerr = createObject(OBJ_STRING, sdsnew("-ERR 语法错误\r\n"));
    shared.sameobjecterr = createObject(OBJ_STRING, sdsnew("-ERR 源和目标对象相同\r\n"));
    shared.outofrangeerr = createObject(OBJ_STRING, sdsnew("-ERR 索引越界\r\n"));
    shared.noscripterr = createObject(OBJ_STRING, sdsnew("-NOSCRIPT 没有匹配到脚本,请使用EVAL.\r\n"));
    shared.loadingerr = createObject(OBJ_STRING, sdsnew("-LOADING 正在加载数据集到内存中\r\n"));
    shared.slowevalerr = createObject(OBJ_STRING, sdsnew("-BUSY Redis正忙着运行一个脚本.你只能调用脚本KILL或SHUTDOWN NOSAVE.\r\n"));
    shared.slowscripterr = createObject(OBJ_STRING, sdsnew("-BUSY Redis正忙着运行一个脚本.你只能调用函数KILL或SHUTDOWN NOSAVE.\r\n"));
    shared.slowmoduleerr = createObject(OBJ_STRING, sdsnew("-BUSY Redis正在忙着运行一个模块命令.\r\n"));
    shared.masterdownerr = createObject(OBJ_STRING, sdsnew("-MASTERDOWN MASTER链路断开,replica-serve-stale-data 设置为 'no'.\r\n"));
    shared.bgsaveerr = createObject(OBJ_STRING, sdsnew("-MISCONF 配置了保存RDB快照,但目前无法持久化到磁盘.禁用 可能修改数据集的命令,因为该实例配置为在RDB快照失败时报告错误(stop-write-on-bgsave-error选项).\r\n"));
    shared.roslaveerr = createObject(OBJ_STRING, sdsnew("-READONLY 不能对只读副本进行写操作.\r\n"));
    shared.noautherr = createObject(OBJ_STRING, sdsnew("-NOAUTH 需要认证.\r\n"));
    shared.oomerr = createObject(OBJ_STRING, sdsnew("-OOM 命令不允许使用 memory > 'maxmemory'.\r\n"));
    shared.execaborterr = createObject(OBJ_STRING, sdsnew("-EXECABORT 由于先前的错误而丢弃事务.\r\n"));
    shared.noreplicaserr = createObject(OBJ_STRING, sdsnew("-NOREPLICAS 没有足够好的副本可以写.\r\n"));
    shared.busykeyerr = createObject(OBJ_STRING, sdsnew("-BUSYKEY 目标key已经存在.\r\n"));

    // 共享NULL取决于协议版本。
    shared.null[0] = NULL;
    shared.null[1] = NULL;
    shared.null[2] = createObject(OBJ_STRING, sdsnew("$-1\r\n"));
    shared.null[3] = createObject(OBJ_STRING, sdsnew("_\r\n"));

    shared.nullarray[0] = NULL;
    shared.nullarray[1] = NULL;
    shared.nullarray[2] = createObject(OBJ_STRING, sdsnew("*-1\r\n"));
    shared.nullarray[3] = createObject(OBJ_STRING, sdsnew("_\r\n"));

    shared.emptymap[0] = NULL;
    shared.emptymap[1] = NULL;
    shared.emptymap[2] = createObject(OBJ_STRING, sdsnew("*0\r\n"));
    shared.emptymap[3] = createObject(OBJ_STRING, sdsnew("%0\r\n"));

    shared.emptyset[0] = NULL;
    shared.emptyset[1] = NULL;
    shared.emptyset[2] = createObject(OBJ_STRING, sdsnew("*0\r\n"));
    shared.emptyset[3] = createObject(OBJ_STRING, sdsnew("~0\r\n"));

    // 常用 SELECT 命令 , 初始化了10个 select ?  sds
    for (j = 0; j < PROTO_SHARED_SELECT_CMDS; j++) {
        char dictid_str[64];
        int dictid_len;

        dictid_len = ll2string(dictid_str, sizeof(dictid_str), j);
        shared.select[j] = createObject(OBJ_STRING, sdscatprintf(sdsempty(), "*2\r\n$6\r\nSELECT\r\n$%d\r\n%s\r\n", dictid_len, dictid_str));
    }

    // 发布与订阅的有关回复
    shared.messagebulk = createStringObject("$7\r\nmessage\r\n", 13);
    shared.pmessagebulk = createStringObject("$8\r\npmessage\r\n", 14);
    shared.subscribebulk = createStringObject("$9\r\nsubscribe\r\n", 15);
    shared.unsubscribebulk = createStringObject("$11\r\nunsubscribe\r\n", 18);
    shared.ssubscribebulk = createStringObject("$10\r\nssubscribe\r\n", 17);
    shared.sunsubscribebulk = createStringObject("$12\r\nsunsubscribe\r\n", 19);
    shared.psubscribebulk = createStringObject("$10\r\npsubscribe\r\n", 17);
    shared.punsubscribebulk = createStringObject("$12\r\npunsubscribe\r\n", 19);

    // 常用命令
    shared.del = createStringObject("DEL", 3);
    shared.unlink = createStringObject("UNLINK", 6);
    shared.rpop = createStringObject("RPOP", 4);
    shared.lpop = createStringObject("LPOP", 4);
    shared.lpush = createStringObject("LPUSH", 5);
    shared.rpoplpush = createStringObject("RPOPLPUSH", 9);
    shared.lmove = createStringObject("LMOVE", 5);
    shared.blmove = createStringObject("BLMOVE", 6);
    shared.zpopmin = createStringObject("ZPOPMIN", 7);
    shared.zpopmax = createStringObject("ZPOPMAX", 7);
    shared.multi = createStringObject("MULTI", 5);
    shared.exec = createStringObject("EXEC", 4);
    shared.hset = createStringObject("HSET", 4);
    shared.srem = createStringObject("SREM", 4);
    shared.xgroup = createStringObject("XGROUP", 6);
    shared.xclaim = createStringObject("XCLAIM", 6);
    shared.script = createStringObject("SCRIPT", 6);
    shared.replconf = createStringObject("REPLCONF", 8);
    shared.pexpireat = createStringObject("PEXPIREAT", 9);
    shared.pexpire = createStringObject("PEXPIRE", 7);
    shared.persist = createStringObject("PERSIST", 7);
    shared.set = createStringObject("SET", 3);
    shared.eval = createStringObject("EVAL", 4);

    /* 共享命令参数 */
    shared.left = createStringObject("left", 4);
    shared.right = createStringObject("right", 5);
    shared.pxat = createStringObject("PXAT", 4);
    shared.time = createStringObject("TIME", 4);
    shared.retrycount = createStringObject("RETRYCOUNT", 10);
    shared.force = createStringObject("FORCE", 5);
    shared.justid = createStringObject("JUSTID", 6);
    shared.entriesread = createStringObject("ENTRIESREAD", 11);
    shared.lastid = createStringObject("LASTID", 6);
    shared.default_username = createStringObject("default", 7);
    shared.ping = createStringObject("ping", 4);
    shared.setid = createStringObject("SETID", 5);
    shared.keepttl = createStringObject("KEEPTTL", 7);
    shared.absttl = createStringObject("ABSTTL", 6);
    shared.load = createStringObject("LOAD", 4);
    shared.createconsumer = createStringObject("CREATECONSUMER", 14);
    shared.getack = createStringObject("GETACK", 6);
    shared.special_asterick = createStringObject("*", 1);
    shared.special_equals = createStringObject("=", 1);
    shared.redacted = makeObjectShared(createStringObject("(redacted)", 10));

    // 常用整数 0~9999
    for (j = 0; j < OBJ_SHARED_INTEGERS; j++) {
        shared.integers[j] = makeObjectShared(createObject(OBJ_STRING, (void *)(long)j));
        shared.integers[j]->encoding = OBJ_ENCODING_INT;
    }
    // 常用长度 bulk 或者 multi bulk 回复
    for (j = 0; j < OBJ_SHARED_BULKHDR_LEN; j++) {
        shared.mbulkhdr[j] = createObject(OBJ_STRING, sdscatprintf(sdsempty(), "*%d\r\n", j));
        shared.bulkhdr[j] = createObject(OBJ_STRING, sdscatprintf(sdsempty(), "$%d\r\n", j));
        shared.maphdr[j] = createObject(OBJ_STRING, sdscatprintf(sdsempty(), "%%%d\r\n", j));
        shared.sethdr[j] = createObject(OBJ_STRING, sdscatprintf(sdsempty(), "~%d\r\n", j));
    }
    // 下面两个共享对象，minstring和maxstring，实际上不是用于它们的值，而是作为一个特殊对象，分别表示
    // 最小的字符串和最大的字符串
    // ZRANGEBYLEX命令的字符串比较中的字符串。
    shared.minstring = sdsnew("minstring");
    shared.maxstring = sdsnew("maxstring");
}

// 主要在初始化全局的server对象中的各个数据成员,设置默认值
void initServerConfig(void) {
    int j;
    char *default_bindaddr[CONFIG_DEFAULT_BINDADDR_COUNT] = CONFIG_DEFAULT_BINDADDR; // 2 = "*", "-::*"

    initConfigValues();                                  // 初始化配置结构体,并将其注册到configs
    updateCachedTime(1);                                 // 更新缓存的时间
    getRandomHexChars(server.runid, CONFIG_RUN_ID_SIZE); // 生成Redis的 "Run ID"  40+1位
    server.runid[CONFIG_RUN_ID_SIZE] = '\0';             // 为运行 ID 加上结尾字符 \0
    changeReplicationId();                               // 改变 复制ID
    clearReplicationId2();                               // 清空 复制ID
    server.hz = CONFIG_DEFAULT_HZ;                       // 设置默认服务器频率
    server.timezone = getTimeZone();                     // 初始时区
    server.configfile = NULL;                            // 设置默认配置文件路径
    server.executable = NULL;                            //
    server.arch_bits = (sizeof(long) == 8) ? 64 : 32;    // 设置服务器的运行架构

    server.bindaddr_count = CONFIG_DEFAULT_BINDADDR_COUNT; // 2
    for (j = 0; j < CONFIG_DEFAULT_BINDADDR_COUNT; j++) {
        // "*", "-::*"
        server.bindaddr[j] = zstrdup(default_bindaddr[j]); // 字长与字节对齐 CPU一次性 能读取数据的二进制位数称为字长,也就是我们通常所说的32位系统(字长4个字节)、64位系统(字长8个字节)的由来
    }
    server.ipfd.count = 0;               // tcp 套接字数组
    server.tlsfd.count = 0;              // tls 套接字数组
    server.sofd = -1;                    // unix套接字  文件描述符, 因为只会有一个unix.socket 这里就直接放了一个编号
    server.active_expire_enabled = 1;    // 是否启用自动过期,默认开启
    server.skip_checksum_validation = 0; // 禁用RDB和RESTORE负载的校验和验证功能.默认关闭
    server.loading = 0;                  // 正在加载AOF、RDB
    server.async_loading = 0;
    server.loading_rdb_used_mem = 0;
    server.aof_state = AOF_OFF;
    server.aof_rewrite_base_size = 0;
    server.aof_rewrite_scheduled = 0;
    server.aof_flush_sleep = 0;
    server.aof_last_fsync = time(NULL);
    server.aof_cur_timestamp = 0;
    atomicSet(server.aof_bio_fsync_status, C_OK);
    server.aof_rewrite_time_last = -1;
    server.aof_rewrite_time_start = -1;
    server.aof_lastbgrewrite_status = C_OK;
    server.aof_delayed_fsync = 0;
    server.aof_fd = -1;
    server.aof_selected_db = -1; // 确保第一次将不匹配
    server.aof_flush_postponed_start = 0;
    server.aof_last_incr_size = 0;
    server.active_defrag_running = 0;
    server.notify_keyspace_events = 0;
    server.blocked_clients = 0;
    memset(server.blocked_clients_by_type, 0, sizeof(server.blocked_clients_by_type)); // 初始化
    server.shutdown_asap = 0;
    server.shutdown_flags = 0;
    server.shutdown_mstime = 0;
    server.cluster_module_flags = CLUSTER_MODULE_FLAG_NONE;
    server.migrate_cached_sockets = dictCreate(&migrateCacheDictType); // 缓存迁移
    server.next_client_id = 1;                                         // 客户端ID,从1开始
    server.page_size = sysconf(_SC_PAGESIZE);                          // 获取操作系统页大小
    server.pause_cron = 0;

    server.latency_tracking_info_percentiles_len = 3; // 延迟追踪
    server.latency_tracking_info_percentiles = zmalloc(sizeof(double) * (server.latency_tracking_info_percentiles_len));
    server.latency_tracking_info_percentiles[0] = 50.0; /* p50 */
    server.latency_tracking_info_percentiles[1] = 99.0; /* p99 */
    server.latency_tracking_info_percentiles[2] = 99.9; /* p999 */

    unsigned int lru_clock = getLRUClock(); // 获取当前系统的毫秒数 该计数器总共占用24位  //调用getLRUClock函数计算全局LRU时钟值
    atomicSet(server.lru_clock, lru_clock); // 设置lruclock为刚计算的LRU时钟值
    resetServerSaveParams();                // 初始化并设置保存条件

    appendServerSaveParams(60 * 60, 1); // 一个小时内有1条数据
    appendServerSaveParams(300, 100);   // 5分钟内有100条数据
    appendServerSaveParams(60, 10000);  // 1分钟内有10000条数据

    // 初始化和复制相关的状态
    server.masterhost = NULL;
    server.masterport = 6379;
    server.master = NULL;
    server.cached_master = NULL;
    server.master_initial_offset = -1;
    server.repl_state = REPL_STATE_NONE;
    server.repl_transfer_tmpfile = NULL;
    server.repl_transfer_fd = -1;
    server.repl_transfer_s = NULL;
    server.repl_syncio_timeout = CONFIG_REPL_SYNCIO_TIMEOUT;
    server.repl_down_since = 0; // 从来没有连接,repl是down since EVER.
    server.master_repl_offset = 0;

    // 初始化 PSYNC 命令所使用的 backlog
    server.repl_backlog = NULL;
    server.repl_no_slaves_since = time(NULL);

    // 客户端输出缓冲区限制
    server.failover_end_time = 0;
    server.force_failover = 0;
    server.target_replica_host = NULL;
    server.target_replica_port = 0;
    server.failover_state = NO_FAILOVER;
    // Linux OOM Score配置
    // 设置客户端的输出缓冲区限制
    for (j = 0; j < CLIENT_TYPE_OBUF_COUNT; j++) {
        server.client_obuf_limits[j] = clientBufferLimitsDefaults[j];
    }

    // 初始化浮点常量
    R_Zero = 0.0;
    R_PosInf = 1.0 / R_Zero;
    R_NegInf = -1.0 / R_Zero;
    R_Nan = R_Zero / R_Zero;

    // 命令表——我们在这里初始化它,因为它是初始配置的一部分,因为命令名可以通过redis.conf使用rename-command指令更改.
    // 初始化命令表
    // 在这里初始化是因为接下来读取 .conf 文件时可能会用到这些命令
    server.commands = dictCreate(&commandTableDictType);      // 大小写不敏感哈希   创建、析构   ,这个数据可以被配置文件中修改
    server.orig_commands = dictCreate(&commandTableDictType); // 支持的命令，原生的，不支持修改

    populateCommandTable(); // 开始填充Redis命令表.  redisCommandTable

    /* 调试 */
    server.watchdog_period = 0;
}

extern char **environ;

// 重新启动服务器，使用相同的参数和配置文件执行启动此实例的相同可执行文件。
// 该函数被设计为直接调用execve()，以便新的服务器实例将保留前一个实例的PID。
// 标记列表，可以按位or排列在一起，改变这个函数的行为:
// RESTART_SERVER_NONE无标志。
// 在重新启动之前适当地关闭。
// RESTART_SERVER_CONFIG_REWRITE重启前重新编写配置文件。
// 如果成功，该函数将不返回，因为该进程将变成另一个进程。错误时返回C_ERR。
int restartServer(int flags, mstime_t delay) {
    int j;

    // 检查我们是否仍然可以访问启动这个服务器实例的可执行文件。
    if (access(server.executable, X_OK) == -1) {
        serverLog(LL_WARNING, "无法重新启动:此进程没有执行权限%s", server.executable);
        return C_ERR;
    }

    // 配置重写
    if (flags & RESTART_SERVER_CONFIG_REWRITE && server.configfile && rewriteConfig(server.configfile, 0) == -1) {
        serverLog(LL_WARNING, "不能重新启动:配置重写过程失败: %s", strerror(errno));
        return C_ERR;
    }
    // 执行正确的关机操作。但我们不会等待滞后的副本。
    if (flags & RESTART_SERVER_GRACEFULLY && prepareForShutdown(SHUTDOWN_NOW) != C_OK) {
        serverLog(LL_WARNING, "无法重新启动:准备关机时出错");
        return C_ERR;
    }

    // 关闭所有的文件描述符，除了stdin, stdout, stderr，如果我们重新启动一个没有守护的Redis服务器是有用的。
    for (j = 3; j < (int)server.maxclients + 1024; j++) {
        // 在关闭描述符之前测试它的有效性，否则Valgrind会对close()发出警告。
        // 取得close-on-exec flag。若此 flag 的FD_CLOEXEC位为0，代表在调用exec()相关函数时文件将不会关闭。
        if (fcntl(j, F_GETFD) != -1) {
            close(j);
        }
    }

    // 使用原始命令行执行服务器。
    if (delay) {
        usleep(delay * 1000);
    }

    zfree(server.exec_argv[0]);
    server.exec_argv[0] = zstrdup(server.executable);
    execve(server.executable, server.exec_argv, environ);

    // 如果这里发生错误，我们什么也做不了，只能退出。
    _exit(1);

    return C_ERR;
}

// 这个函数将根据process_class 配置当前进程的oom_score_adj到用户指定的配置.这是目前在Linux上实现的
// process_class值为-1表示OOM_CONFIG_MASTER或OOM_CONFIG_REPLICA,取决于当前的角色.
int setOOMScoreAdj(int process_class) {
    if (process_class == -1) {
        if (server.masterhost) {
            process_class = CONFIG_OOM_REPLICA;
        }
        else {
            process_class = CONFIG_OOM_MASTER;
        }
    }

    serverAssert(process_class >= 0 && process_class < CONFIG_OOM_COUNT);

#ifdef HAVE_PROC_OOM_SCORE_ADJ
    /* The following statics are used to indicate Redis has changed the process's oom score.
     * And to save the original score so we can restore it later if needed.
     * We need this so when we disabled oom-score-adj (also during configuration rollback
     * when another configuration parameter was invalid and causes a rollback after
     * applying a new oom-score) we can return to the oom-score value from before our
     * adjustments. */
    static int oom_score_adjusted_by_redis = 0;
    static int oom_score_adj_base = 0;

    int fd;
    int val;
    char buf[64];

    if (server.oom_score_adj != OOM_SCORE_ADJ_NO) {
        if (!oom_score_adjusted_by_redis) {
            oom_score_adjusted_by_redis = 1;
            /* Backup base value before enabling Redis control over oom score */
            fd = open("/proc/self/oom_score_adj", O_RDONLY);
            if (fd < 0 || read(fd, buf, sizeof(buf)) < 0) {
                serverLog(LL_WARNING, "Unable to read oom_score_adj: %s", strerror(errno));
                if (fd != -1)
                    close(fd);
                return C_ERR;
            }
            oom_score_adj_base = atoi(buf);
            close(fd);
        }

        val = server.oom_score_adj_values[process_class];
        if (server.oom_score_adj == OOM_SCORE_RELATIVE)
            val += oom_score_adj_base;
        if (val > 1000)
            val = 1000;
        if (val < -1000)
            val = -1000;
    }
    else if (oom_score_adjusted_by_redis) {
        oom_score_adjusted_by_redis = 0;
        val = oom_score_adj_base;
    }
    else {
        return C_OK;
    }

    snprintf(buf, sizeof(buf) - 1, "%d\n", val);

    fd = open("/proc/self/oom_score_adj", O_WRONLY);
    if (fd < 0 || write(fd, buf, strlen(buf)) < 0) {
        serverLog(LL_WARNING, "Unable to write oom_score_adj: %s", strerror(errno));
        if (fd != -1)
            close(fd);
        return C_ERR;
    }

    close(fd);
    return C_OK;
#else
    /* 不支持 */
    return C_ERR;
#endif
}

// 此函数将尝试根据配置的最大客户端数来提高打开文件的最大数量.它还保留了一些文件描述符(CONFIG_MIN_RESERVED_FDS),用于持久性、侦听套接字、日志文件等额外的操作.
// 如果不能将限制相应地设置为所配置的最大客户端数,该函数将执行反向设置服务器.Maxclients
// 到我们实际可以处理的值.
void adjustOpenFilesLimit(void) {
    rlim_t maxfiles = server.maxclients + CONFIG_MIN_RESERVED_FDS; // ?+32
    struct rlimit limit;
    // 用于检索和设定系统资源
    if (getrlimit(RLIMIT_NOFILE, &limit) == -1) {
        serverLog(LL_WARNING, "无法获取当前的NOFILE限制(%s),假设为1024并相应地设置最大客户端配置.", strerror(errno));
        server.maxclients = 1024 - CONFIG_MIN_RESERVED_FDS; // 1024 - 32
    }
    else {
        rlim_t oldlimit = limit.rlim_cur;

        // 如果当前限制不足以满足我们的需要,则设置最大文件数.
        if (oldlimit < maxfiles) {
            rlim_t bestlimit;
            int setrlimit_error = 0;

            // 尝试将文件限制设置为与'maxfiles'匹配,或至少与支持的小于maxfiles的更高值匹配.
            bestlimit = maxfiles;
            while (bestlimit > oldlimit) {
                rlim_t decr_step = 16;

                limit.rlim_cur = bestlimit;
                limit.rlim_max = bestlimit;
                if (setrlimit(RLIMIT_NOFILE, &limit) != -1)
                    break;
                setrlimit_error = errno;

                // 我们未能将文件限制设置为“bestlimit”。尝试使用较小的限制，每次迭代减少几个fd。
                if (bestlimit < decr_step)
                    break;
                bestlimit -= decr_step;
            }

            // 假设我们最初得到的极限仍然有效如果我们最后一次尝试更低。
            if (bestlimit < oldlimit)
                bestlimit = oldlimit;

            if (bestlimit < maxfiles) {
                unsigned int old_maxclients = server.maxclients;
                server.maxclients = bestlimit - CONFIG_MIN_RESERVED_FDS;
                // Maxclients是无符号的，所以可能溢出:为了检查Maxclients现在逻辑上是否小于1，我们通过bestlimit间接测试。
                if (bestlimit <= CONFIG_MIN_RESERVED_FDS) {
                    serverLog(LL_WARNING, "你目前的'ulimit -n'为%llu,不足以让服务器启动.请将你的打开文件限制增加到至少%llu.退出.", (unsigned long long)oldlimit, (unsigned long long)maxfiles);
                    exit(1);
                }
                serverLog(LL_WARNING, "你要求的maxclients为%d,要求至少有%llu的最大文件描述符.", old_maxclients, (unsigned long long)maxfiles);
                serverLog(LL_WARNING, "服务器不能将最大打开文件设置为%llu,因为操作系统错误:%s.", (unsigned long long)maxfiles, strerror(setrlimit_error));
                serverLog(LL_WARNING, "当前最大打开的文件是%llu.maxclients已经减少到%d以补偿低ulimit.如果你需要更高的最大客户数,请增加'ulimit -n'.", (unsigned long long)bestlimit, server.maxclients);
            }
            else {
                serverLog(LL_NOTICE, "最大打开文件数增加到%llu(最初设置为%llu).", (unsigned long long)maxfiles, (unsigned long long)oldlimit);
            }
        }
    }
}

// 检查 tcp_backlog 和系统的somaxconn参数值
void checkTcpBacklogSettings(void) {
#if defined(HAVE_PROC_SOMAXCONN)
    FILE *fp = fopen("/proc/sys/net/core/somaxconn", "r");
    char buf[1024];
    if (!fp)
        return;
    if (fgets(buf, sizeof(buf), fp) != NULL) {
        int somaxconn = atoi(buf);
        if (somaxconn > 0 && somaxconn < server.tcp_backlog) {
            serverLog(LL_WARNING, "WARNING: The TCP backlog setting of %d cannot be enforced because /proc/sys/net/core/somaxconn is set to the lower value of %d.", server.tcp_backlog, somaxconn);
        }
    }
    fclose(fp);
#elif defined(HAVE_SYSCTL_KIPC_SOMAXCONN)
    int somaxconn, mib[3];
    size_t len = sizeof(int);

    mib[0] = CTL_KERN;
    mib[1] = KERN_IPC;
    mib[2] = KIPC_SOMAXCONN;

    if (sysctl(mib, 3, &somaxconn, &len, NULL, 0) == 0) {
        if (somaxconn > 0 && somaxconn < server.tcp_backlog) {
            serverLog(LL_WARNING, "警告：由于 kern.ipc.somaxconn 被设置为较低的值%d，所以无法执行设置TCP backlog: %d [全连接队列数量=min(backlog,somaxconn)]。", server.tcp_backlog, somaxconn);
        }
    }
#elif defined(HAVE_SYSCTL_KERN_SOMAXCONN)
    int somaxconn, mib[2];
    size_t len = sizeof(int);

    mib[0] = CTL_KERN;
    mib[1] = KERN_SOMAXCONN;

    if (sysctl(mib, 2, &somaxconn, &len, NULL, 0) == 0) {
        if (somaxconn > 0 && somaxconn < server.tcp_backlog) {
            serverLog(LL_WARNING, "警告：由于 kern.ipc.somaxconn 被设置为较低的值%d，所以无法执行设置TCP backlog: %d [全连接队列数量=min(backlog,somaxconn)]。", server.tcp_backlog, somaxconn);
        }
    }
#elif defined(SOMAXCONN)
    if (SOMAXCONN < server.tcp_backlog) {
        serverLog(LL_WARNING, "WARNING: The TCP backlog setting of %d cannot be enforced because SOMAXCONN is set to the lower value of %d.", server.tcp_backlog, SOMAXCONN);
    }
#endif
}

// 关闭监听套接字
void closeSocketListeners(socketFds *sfd) {
    int j;

    for (j = 0; j < sfd->count; j++) {
        if (sfd->fd[j] == -1)
            continue;

        aeDeleteFileEvent(server.el, sfd->fd[j], AE_READABLE);
        close(sfd->fd[j]);
    }

    sfd->count = 0;
}

// 为每一个监听的IP设置连接事件的处理函数acceptTcpHandler
int createSocketAcceptHandler(socketFds *sfd, aeFileProc *accept_handler) {
    int j;
    for (j = 0; j < sfd->count; j++) {
        // 为每个IP端口的网络事件,调用aeCreateFileEvent
        // AE_READABLE 事件就是客户端的网络连接事件,而对应的处理函数就是接收 TCP 连接请求
        if (aeCreateFileEvent(server.el, sfd->fd[j], AE_READABLE, accept_handler, NULL) == AE_ERR) {
            // 如果第j个套接字监听事件 创建失败了,就删除前j个套接字的监听事件
            for (j = j - 1; j >= 0; j--) {
                aeDeleteFileEvent(server.el, sfd->fd[j], AE_READABLE);
            }
            return C_ERR;
        }
    }
    return C_OK;
}

// 监听tcp端口
int listenToPort(int port, socketFds *sfd) {
    int j;
    char **bindaddr = server.bindaddr;

    // 如果我们没有绑定地址,我们就不在TCP套接字上监听.
    if (server.bindaddr_count == 0)
        return C_OK;
    // tcp6       0      0  *.6379                 *.*                    LISTEN
    // tcp4       0      0  *.6379                 *.*                    LISTEN
    for (j = 0; j < server.bindaddr_count; j++) { // 2
        char *addr = bindaddr[j];
        // "*", "-::*"
        int optional = *addr == '-';
        if (optional)
            addr++;
        // 一个串中查找给定字符的第一个匹配之处
        if (strchr(addr, ':')) {
            // IPV6地址
            sfd->fd[sfd->count] = anetTcp6Server(server.neterr, port, addr, server.tcp_backlog);
        }
        else {
            // IPV4地址
            sfd->fd[sfd->count] = anetTcpServer(server.neterr, port, addr, server.tcp_backlog);
        }
        // 套接字创建失败
        if (sfd->fd[sfd->count] == ANET_ERR) {
            int net_errno = errno;
            serverLog(LL_WARNING, "警告:无法创建服务器TCP侦听套接字 %s:%d: %s", addr, port, server.neterr);
            // EADDRNOTAVAIL https://blog.csdn.net/xiaogazhang/article/details/46813815
            // ENOPROTOOPT：指定的协议层不能识别选项
            // ENOTSOCK：sock描述的不是套接字
            // EPROTONOSUPPORT：所选协议不支持套接字类型
            // ESOCKTNOSUPPORT：所选协议不支持套接字类型
            if (net_errno == EADDRNOTAVAIL && optional)
                continue;
            if (net_errno == ENOPROTOOPT || net_errno == EPROTONOSUPPORT || net_errno == ESOCKTNOSUPPORT || net_errno == EPFNOSUPPORT || net_errno == EAFNOSUPPORT)
                continue;

            // 退出前 关闭监听
            closeSocketListeners(sfd);
            return C_ERR;
        }
        if (server.socket_mark_id > 0) {
            anetSetSockMarkId(NULL, sfd->fd[sfd->count], server.socket_mark_id);
        }
        anetNonBlock(NULL, sfd->fd[sfd->count]); // 将 fd 设置为非阻塞模式（O_NONBLOCK）
        anetCloexec(sfd->fd[sfd->count]);
        sfd->count++;
    }
    return C_OK;
}

// 重置我们通过INFO或其他方式公开的统计信息,我们希望通过CONFIG RESETSTAT重置.该函数还用于在服务器启动时初始化initServer()中的这些字段.
void resetServerStats(void) {
    int j;

    server.stat_numcommands = 0;
    server.stat_numconnections = 0;
    server.stat_expiredkeys = 0;
    server.stat_expired_stale_perc = 0;
    server.stat_expired_time_cap_reached_count = 0;
    server.stat_expire_cycle_time_used = 0;
    server.stat_evictedkeys = 0;
    server.stat_evictedclients = 0;
    server.stat_total_eviction_exceeded_time = 0;
    server.stat_last_eviction_exceeded_time = 0;
    server.stat_keyspace_misses = 0;
    server.stat_keyspace_hits = 0;
    server.stat_active_defrag_hits = 0;
    server.stat_active_defrag_misses = 0;
    server.stat_active_defrag_key_hits = 0;
    server.stat_active_defrag_key_misses = 0;
    server.stat_active_defrag_scanned = 0;
    server.stat_total_active_defrag_time = 0;
    server.stat_last_active_defrag_time = 0;
    server.stat_fork_time = 0;
    server.stat_fork_rate = 0;
    server.stat_total_forks = 0;
    server.stat_rejected_conn = 0;
    server.stat_sync_full = 0;
    server.stat_sync_partial_ok = 0;
    server.stat_sync_partial_err = 0;
    server.stat_io_reads_processed = 0;
    atomicSet(server.stat_total_reads_processed, 0);
    server.stat_io_writes_processed = 0;
    atomicSet(server.stat_total_writes_processed, 0);
    for (j = 0; j < STATS_METRIC_COUNT; j++) {
        server.inst_metric[j].idx = 0;
        server.inst_metric[j].last_sample_time = mstime();
        server.inst_metric[j].last_sample_count = 0;
        memset(server.inst_metric[j].samples, 0, sizeof(server.inst_metric[j].samples));
    }
    server.stat_aof_rewrites = 0;
    server.stat_rdb_saves = 0;
    server.stat_aofrw_consecutive_failures = 0;
    atomicSet(server.stat_net_input_bytes, 0);
    atomicSet(server.stat_net_output_bytes, 0);
    server.stat_unexpected_error_replies = 0;
    server.stat_total_error_replies = 0;
    server.stat_dump_payload_sanitizations = 0;
    server.aof_delayed_fsync = 0;
    server.stat_reply_buffer_shrinks = 0;
    server.stat_reply_buffer_expands = 0;
    lazyfreeResetStats();
}

// 让线程在任何时候都可以被杀死,这样杀死线程的函数才能可靠地工作（默认的可取消类型是 PTHREAD_CANCEL_DEFERRED ）.
// 崩溃报告所使用的快速内存测试所需要的pthread_cancel.
void makeThreadKillable(void) {
    // myself    pthread_setcancelstate(PTHREAD_CANCEL_DEFERRED,NULL); // 默认值
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
}

// --继续初始化全局的server对象,并在里面初始化了全局的shared对象
// --监听端口和uinx socket文件
// --启动bio线程
// --lua环境
// 和 server 连接的客户端、从库
// Redis 用作缓存时的替换候选集
// server 运行时的状态信息
// 包括了 server 资源管理所需的数据结构初始化
// 键值对数据库初始化
// server 网络框架初始化
void initServer(void) {
    int j;
    // 第一个参数表示需要处理的信号值,第二个参数为处理函数或者是一个标识,这里,SIG_IGN表示忽略SIGHUP那个注册的信号
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    setupSignalHandlers(); // 设置信号处理函数
    makeThreadKillable();  // 让线程在任何时候都可以被杀死

    // 设置 syslog
    if (server.syslog_enabled) {
        openlog(server.syslog_ident, LOG_PID | LOG_NDELAY | LOG_NOWAIT, server.syslog_facility);
    }

    // 从配置系统设置默认值后的初始化.
    server.aof_state = server.aof_enabled ? AOF_ON : AOF_OFF; // 设置是否启用AOF
    server.hz = server.config_hz;
    server.pid = getpid();
    server.in_fork_child = CHILD_TYPE_NONE;
    server.main_thread_id = pthread_self();
    server.current_client = NULL;
    server.errors = raxNew();
    server.fixed_time_expire = 0;
    server.in_nested_call = 0;
    server.clients = listCreate();
    server.clients_index = raxNew();
    server.clients_to_close = listCreate();
    server.slaves = listCreate();
    server.monitors = listCreate();
    server.clients_pending_write = listCreate();
    server.clients_pending_read = listCreate();
    server.clients_timeout_table = raxNew();
    server.replication_allowed = 1;
    server.slaveseldb = -1; // 强制发出第一个SELECT命令.
    server.unblocked_clients = listCreate();
    server.ready_keys = listCreate();
    server.tracking_pending_keys = listCreate();
    server.clients_waiting_acks = listCreate();
    server.get_ack_from_slaves = 0;
    server.client_pause_type = CLIENT_PAUSE_OFF;
    server.client_pause_end_time = 0;
    memset(server.client_pause_per_purpose, 0, sizeof(server.client_pause_per_purpose));
    server.postponed_clients = listCreate();
    server.events_processed_while_blocked = 0;
    server.system_memory_size = zmalloc_get_memory_size();
    server.blocked_last_cron = 0;
    server.blocking_op_nesting = 0;
    server.thp_enabled = 0;
    server.cluster_drop_packet_filter = -1;
    server.reply_buffer_peak_reset_time = REPLY_BUFFER_DEFAULT_PEAK_RESET_TIME;
    server.reply_buffer_resizing_enabled = 1;
    resetReplicationBuffer();

    if ((server.tls_port || server.tls_replication || server.tls_cluster) && tlsConfigure(&server.tls_ctx_config) == C_ERR) {
        serverLog(LL_WARNING, "配置TLS失败.查看日志了解更多信息.");
        exit(1);
    }

    for (j = 0; j < CLIENT_MEM_USAGE_BUCKETS; j++) {
        server.client_mem_usage_buckets[j].mem_usage_sum = 0;
        server.client_mem_usage_buckets[j].clients = listCreate();
    }

    // 创建共享对象
    createSharedObjects();
    adjustOpenFilesLimit();
    const char *clk_msg = monotonicInit();
    serverLog(LL_NOTICE, "单调时钟: %s", clk_msg);
    // 初始化事件触发结构
    server.el = aeCreateEventLoop(server.maxclients + CONFIG_FDSET_INCR); // 10000 + 128
    if (server.el == NULL) {
        serverLog(LL_WARNING, "创建事件循环失败. 错误信息: '%s'", strerror(errno));
        exit(1);
    }
    server.db = zmalloc(sizeof(redisDb) * server.dbnum); // 创建16个数据库

    // 开始监听设置的TCP网络端口,用于等待客户端的命令请求
    if (server.port != 0 && listenToPort(server.port, &server.ipfd) == C_ERR) {
        serverLog(LL_WARNING, "端口监听失败 %u (TCP).", server.port);
        exit(1);
    }
    if (server.tls_port != 0 && listenToPort(server.tls_port, &server.tlsfd) == C_ERR) {
        serverLog(LL_WARNING, "端口监听失败 %u (TLS).", server.tls_port);
        exit(1);
    }

    // 打开 UNIX 本地端口
    if (server.unixsocket != NULL) {
        unlink(server.unixsocket); /* don't care if this fails */
        server.sofd = anetUnixServer(server.neterr, server.unixsocket, (mode_t)server.unixsocketperm, server.tcp_backlog);
        if (server.sofd == ANET_ERR) {
            serverLog(LL_WARNING, "Failed opening Unix socket: %s", server.neterr);
            exit(1);
        }
        anetNonBlock(NULL, server.sofd);
        anetCloexec(server.sofd);
    }

    // 如果根本没有侦听套接字,则终止.
    if (server.ipfd.count == 0 && server.tlsfd.count == 0 && server.sofd < 0) {
        serverLog(LL_WARNING, "配置为不监听任何地方,退出.");
        exit(1);
    }

    // 创建并初始化数据库结构
    for (j = 0; j < server.dbnum; j++) {
        server.db[j].dict = dictCreate(&dbDictType);                          // 创建全局哈希表
        server.db[j].expires = dictCreate(&dbExpiresDictType);                // 创建过期key的信息表
        server.db[j].expires_cursor = 0;                                      //
        server.db[j].blocking_keys = dictCreate(&keylistDictType);            // 为被BLPOP阻塞的key创建信息表
        server.db[j].ready_keys = dictCreate(&objectKeyPointerValueDictType); // 为将执行PUSH的阻塞key创建信息表
        server.db[j].watched_keys = dictCreate(&keylistDictType);             // 为被MULTI/WATCH操作监听的key创建信息表
        server.db[j].id = j;
        server.db[j].avg_ttl = 0;
        server.db[j].defrag_later = listCreate();
        server.db[j].slots_to_keys = NULL;
        listSetFreeMethod(server.db[j].defrag_later, (void (*)(void *))sdsfree);
    }
    evictionPoolAlloc(); // 采样生成用于淘汰的候选 key 集合

    // 创建 PUBSUB 相关结构
    server.pubsub_channels = dictCreate(&keylistDictType);
    server.pubsub_patterns = dictCreate(&keylistDictType);
    server.pubsubshard_channels = dictCreate(&keylistDictType);
    server.cronloops = 0;
    server.in_script = 0;
    server.in_exec = 0;
    server.busy_module_yield_flags = BUSY_MODULE_YIELD_NONE;
    server.busy_module_yield_reply = NULL;
    server.core_propagates = 0;
    server.propagate_no_multi = 0;
    server.module_ctx_nesting = 0;
    server.client_pause_in_transaction = 0;
    server.child_pid = -1;
    server.child_type = CHILD_TYPE_NONE;
    server.rdb_child_type = RDB_CHILD_TYPE_NONE;
    server.rdb_pipe_conns = NULL;
    server.rdb_pipe_numconns = 0;
    server.rdb_pipe_numconns_writing = 0;
    server.rdb_pipe_buff = NULL;
    server.rdb_pipe_bufflen = 0;
    server.rdb_bgsave_scheduled = 0;
    server.child_info_pipe[0] = -1;
    server.child_info_pipe[1] = -1;
    server.child_info_nread = 0;
    server.aof_buf = sdsempty();
    server.lastsave = time(NULL);
    server.lastbgsave_try = 0;
    server.rdb_save_time_last = -1;
    server.rdb_save_time_start = -1;
    server.rdb_last_load_keys_expired = 0;
    server.rdb_last_load_keys_loaded = 0;
    server.dirty = 0;
    resetServerStats(); // 重置server运行状态信息

    server.stat_starttime = time(NULL);
    server.stat_peak_memory = 0;
    server.stat_current_cow_peak = 0;
    server.stat_current_cow_bytes = 0;
    server.stat_current_cow_updated = 0;
    server.stat_current_save_keys_processed = 0;
    server.stat_current_save_keys_total = 0;
    server.stat_rdb_cow_bytes = 0;
    server.stat_aof_cow_bytes = 0;
    server.stat_module_cow_bytes = 0;
    server.stat_module_progress = 0;
    for (int j = 0; j < CLIENT_TYPE_COUNT; j++) {
        server.stat_clients_type_memory[j] = 0;
    }
    server.stat_cluster_links_memory = 0;
    server.cron_malloc_stats.zmalloc_used = 0;
    server.cron_malloc_stats.process_rss = 0;
    server.cron_malloc_stats.allocator_allocated = 0;
    server.cron_malloc_stats.allocator_active = 0;
    server.cron_malloc_stats.allocator_resident = 0;
    server.lastbgsave_status = C_OK;
    server.aof_last_write_status = C_OK;
    server.aof_last_write_errno = 0;
    server.repl_good_slaves_count = 0;
    server.last_sig_received = 0;

    // 为server后台任务创建定时事件,1毫秒*1000一次,定时事件为 serverCron() 创建时间事件
    if (aeCreateTimeEvent(server.el, 1, serverCron, NULL, NULL) == AE_ERR) {
        serverPanic("创建事件循环定时器失败.");
        exit(1);
    }

    // 2355行 listenToPort  代码监听的套接字,并将其保存在了server.ipfd中;    tcp套接字事件    accept、read、write、close
    if (createSocketAcceptHandler(&server.ipfd, acceptTcpHandler) != C_OK) {
        serverPanic("创建TCP套接字接受处理程序时出现错误.");
    }
    // tls套接字事件
    if (createSocketAcceptHandler(&server.tlsfd, acceptTLSHandler) != C_OK) {
        serverPanic("创建Tls套接字接受处理程序时出现错误");
    }
    // 为本地unix套接字注册事件处理器
    if (server.sofd > 0 && aeCreateFileEvent(server.el, server.sofd, AE_READABLE, acceptUnixHandler, NULL) == AE_ERR) {
        serverPanic("创建server.sofd文件事件发生了不可恢复的错误.");
    }
    // 为管道注册一个可读事件,用于从模块线程中唤醒事件循环.
    if (aeCreateFileEvent(server.el, server.module_pipe[0], AE_READABLE, modulePipeReadable, NULL) == AE_ERR) {
        serverPanic("为模块管道注册可读事件时出错.");
    }

    aeSetBeforeSleepProc(server.el, beforeSleep);
    aeSetAfterSleepProc(server.el, afterSleep);

    /* 32位实例的地址空间被限制在4GB,所以如果用户提供的配置中没有显式的限制,我们使用'noeviction'策略的maxmemory将地址空间限制在3gb.
     * 这避免了Redis实例因为内存不足而导致无用的崩溃.*/
    if (server.arch_bits == 32 && server.maxmemory == 0) {
        serverLog(LL_WARNING, "警告: 检测到32位实例,但没有设置内存限制.现在使用'noeviction'策略设置3 GB的最大内存限制.");
        server.maxmemory = 3072LL * (1024 * 1024); // 3GB    字节
        server.maxmemory_policy = MAXMEMORY_NO_EVICTION;
    }
    // 如果服务器以 cluster 模式打开,那么初始化 cluster
    if (server.cluster_enabled) {
        clusterInit();
    }

    // 与lua有关的
    scriptingInit(1); // 初始化脚本系统 ,todo
    functionsInit();

    slowlogInit();        // 初始化慢查询功能
    latencyMonitorInit(); // 监控延迟初始化

    /* 如果ACL缺省密码存在,初始化ACL缺省密码 */
    ACLUpdateDefaultUserPassword(server.requirepass);

    applyWatchdogPeriod(); // 设置SIGALRM信号的处理程序
}

// 初始化网络 IO 相关的线程资源
void InitServerLast() {
    // 启动 3 类后台线程,协助主线程工作（异步释放 fd、AOF 每秒刷盘、lazyfree）
    bioInit();        // 创建后台线程
    initThreadedIO(); // 初始化IO线程,加速数据读取、命令解析以及数据写回的速度
    set_jemalloc_bg_thread(server.jemalloc_bg_thread);
    server.initial_memory_usage = zmalloc_used_memory();
}

void populateCommandLegacyRangeSpec(struct redisCommand *c) {
    // 填充命令遗留范围规格
    memset(&c->legacy_range_key_spec, 0, sizeof(c->legacy_range_key_spec));

    if (c->key_specs_num == 0)
        return;

    if (c->key_specs_num == 1 && c->key_specs[0].begin_search_type == KSPEC_BS_INDEX && c->key_specs[0].find_keys_type == KSPEC_FK_RANGE) {
        /* Quick win */
        c->legacy_range_key_spec = c->key_specs[0];
        return;
    }

    int firstkey = INT_MAX, lastkey = 0;
    int prev_lastkey = 0;
    for (int i = 0; i < c->key_specs_num; i++) {
        if (c->key_specs[i].begin_search_type != KSPEC_BS_INDEX || c->key_specs[i].find_keys_type != KSPEC_FK_RANGE)
            continue;
        if (c->key_specs[i].fk.range.keystep != 1)
            return;
        if (prev_lastkey && prev_lastkey != c->key_specs[i].bs.index.pos - 1)
            return;
        firstkey = min(firstkey, c->key_specs[i].bs.index.pos);
        /* Get the absolute index for lastkey (in the "range" spec, lastkey is relative to firstkey) */
        int lastkey_abs_index = c->key_specs[i].fk.range.lastkey;
        if (lastkey_abs_index >= 0)
            lastkey_abs_index += c->key_specs[i].bs.index.pos;
        /* For lastkey we use unsigned comparison to handle negative values correctly */
        lastkey = max((unsigned)lastkey, (unsigned)lastkey_abs_index);
    }

    if (firstkey == INT_MAX)
        return;

    serverAssert(firstkey != 0);
    serverAssert(lastkey != 0);

    c->legacy_range_key_spec.begin_search_type = KSPEC_BS_INDEX;
    c->legacy_range_key_spec.bs.index.pos = firstkey;
    c->legacy_range_key_spec.find_keys_type = KSPEC_FK_RANGE;
    c->legacy_range_key_spec.fk.range.lastkey = lastkey < 0 ? lastkey : (lastkey - firstkey); /* in the "range" spec, lastkey is relative to firstkey */
    c->legacy_range_key_spec.fk.range.keystep = 1;
    c->legacy_range_key_spec.fk.range.limit = 0;
}

// 获取子命令的全称
sds catSubCommandFullname(const char *parent_name, const char *sub_name) {
    return sdscatfmt(sdsempty(), "%s|%s", parent_name, sub_name);
}
// 添加子命令
void commandAddSubcommand(struct redisCommand *parent, struct redisCommand *subcommand, const char *declared_name) {
    if (!parent->subcommands_dict)
        parent->subcommands_dict = dictCreate(&commandTableDictType);

    subcommand->parent = parent;                            /* Assign the parent command */
    subcommand->id = ACLGetCommandID(subcommand->fullname); /* Assign the ID used for ACL. */

    serverAssert(dictAdd(parent->subcommands_dict, sdsnew(declared_name), subcommand) == DICT_OK);
}

// 设置隐含的ACl类别
void setImplicitACLCategories(struct redisCommand *c) {
    if (c->flags & CMD_WRITE) // 写入命令,可能会修改 key space
        c->acl_categories |= ACL_CATEGORY_WRITE;
    if (c->flags & CMD_READONLY) // 读命令,不修改 key space
        c->acl_categories |= ACL_CATEGORY_READ;
    if (c->flags & CMD_ADMIN) // 管理命令   SAVE BGSAVE  SHUTDOWN
        c->acl_categories |= ACL_CATEGORY_ADMIN | ACL_CATEGORY_DANGEROUS;
    if (c->flags & CMD_PUBSUB) // 发布于订阅功能方面的命令
        c->acl_categories |= ACL_CATEGORY_PUBSUB;
    if (c->flags & CMD_FAST) // 快速命令.
        c->acl_categories |= ACL_CATEGORY_FAST;
    if (c->flags & CMD_BLOCKING) // 该命令有可能阻塞客户端.
        c->acl_categories |= ACL_CATEGORY_BLOCKING;

    // 如果没有设置fast标志,那么他就是slow
    if (!(c->acl_categories & ACL_CATEGORY_FAST))
        c->acl_categories |= ACL_CATEGORY_SLOW;
}

int populateArgsStructure(struct redisCommandArg *args) {
    if (!args)
        return 0;
    int count = 0;
    while (args->name) {
        serverAssert(count < INT_MAX);
        args->num_args = populateArgsStructure(args->subargs);
        count++;
        args++;
    }
    return count;
}

// 递归地填充命令结构.
void populateCommandStructure(struct redisCommand *c) {
    // Redis命令不需要比STATIC_KEY_SPECS_NUM更多的args（只有在模块命令中,键的规格数可以大于STATIC_KEY_SPECS_NUM）.
    c->key_specs = c->key_specs_static;      // 默认所有命令都没有设置key_specs_static
    c->key_specs_max = STATIC_KEY_SPECS_NUM; // 4

    // 我们从一个未分配的直方图开始,并且只在第一次发出命令时才分配内存
    c->latency_histogram = NULL;

    for (int i = 0; i < STATIC_KEY_SPECS_NUM; i++) {
        if (c->key_specs[i].begin_search_type == KSPEC_BS_INVALID) {
            break;
        }
        c->key_specs_num++;
    }

    /* Count things so we don't have to use deferred reply in COMMAND reply. */
    while (c->history && c->history[c->num_history].since) c->num_history++;
    while (c->tips && c->tips[c->num_tips]) c->num_tips++;
    c->num_args = populateArgsStructure(c->args);

    populateCommandLegacyRangeSpec(c);

    /* Handle the "movablekeys" flag (must be done after populating all key specs). */
    populateCommandMovableKeys(c);

    /* Assign the ID used for ACL. */
    c->id = ACLGetCommandID(c->fullname);

    /* Handle subcommands */
    if (c->subcommands) {
        for (int j = 0; c->subcommands[j].declared_name; j++) {
            struct redisCommand *sub = c->subcommands + j;

            /* Translate the command string flags description into an actual
             * set of flags. */
            setImplicitACLCategories(sub);
            sub->fullname = catSubCommandFullname(c->declared_name, sub->declared_name);
            populateCommandStructure(sub);
            commandAddSubcommand(c, sub, sub->declared_name);
        }
    }
}

extern struct redisCommand redisCommandTable[];

// 填充Redis命令表.
void populateCommandTable(void) {
    int j;
    struct redisCommand *c;

    for (j = 0;; j++) {
        c = redisCommandTable + j; // 指定命令

        if (c->declared_name == NULL) { // 类型
            break;
        }

        int retval1, retval2;

        setImplicitACLCategories(c); // 设置隐含的ACl类别  acl_categories字段
        // 取出字符串 FLAG
        if (!(c->flags & CMD_SENTINEL) && server.sentinel_mode) {
            // 当前是哨兵模式,但命令不支持哨兵模式
            continue;
        }

        if (c->flags & CMD_ONLY_SENTINEL && !server.sentinel_mode) {
            // 当前是不是哨兵模式,但命令仅仅支持哨兵模式
            continue;
        }
        // 哨兵模式、且命令支持哨兵模式
        c->fullname = sdsnew(c->declared_name); // 指令名称
        populateCommandStructure(c);            // 递归地填充命令结构.
        // 将命令关联到命令表
        retval1 = dictAdd(server.commands, sdsdup(c->fullname), c);

        // 将命令也关联到原始命令表, 原始命令表不会受 redis.conf 中命令改名的影响
        retval2 = dictAdd(server.orig_commands, sdsdup(c->fullname), c);

        serverAssert(retval1 == DICT_OK && retval2 == DICT_OK);
    }
}

// 重置命令表中的统计信息
void resetCommandTableStats(dict *commands) {
    struct redisCommand *c;
    dictEntry *de;
    dictIterator *di;

    di = dictGetSafeIterator(commands);
    while ((de = dictNext(di)) != NULL) {
        c = (struct redisCommand *)dictGetVal(de);
        c->microseconds = 0;   // 清零时间
        c->calls = 0;          // 清零调用次数
        c->rejected_calls = 0; // 清零次数
        c->failed_calls = 0;   // 清零次数
        if (c->latency_histogram) {
            hdr_close(c->latency_histogram);
            c->latency_histogram = NULL;
        }
        if (c->subcommands_dict)
            resetCommandTableStats(c->subcommands_dict);
    }
    dictReleaseIterator(di);
}

void resetErrorTableStats(void) {
    raxFreeWithCallback(server.errors, zfree);
    server.errors = raxNew();
}

/* ========================== Redis OP Array API ============================ */

void redisOpArrayInit(redisOpArray *oa) {
    oa->ops = NULL;   //
    oa->numops = 0;   // 操作数
    oa->capacity = 0; // 容量
}

int redisOpArrayAppend(redisOpArray *oa, int dbid, robj **argv, int argc, int target) {
    redisOp *op;
    int prev_capacity = oa->capacity;

    if (oa->numops == 0) {
        oa->capacity = 16;
    }
    else if (oa->numops >= oa->capacity) {
        oa->capacity *= 2;
    }

    if (prev_capacity != oa->capacity)
        oa->ops = zrealloc(oa->ops, sizeof(redisOp) * oa->capacity);
    op = oa->ops + oa->numops; // 指针移动到最新的位置
    op->dbid = dbid;
    op->argv = argv;
    op->argc = argc;
    op->target = target;
    oa->numops++;
    return oa->numops;
}

void redisOpArrayFree(redisOpArray *oa) {
    while (oa->numops) {
        int j;
        redisOp *op;

        oa->numops--;
        op = oa->ops + oa->numops;
        for (j = 0; j < op->argc; j++) decrRefCount(op->argv[j]);
        zfree(op->argv);
    }
    zfree(oa->ops);
    redisOpArrayInit(oa);
}

/* ====================== 查找命令并执行 ===================== */

int isContainerCommandBySds(sds s) {
    struct redisCommand *base_cmd = dictFetchValue(server.commands, s);
    int has_subcommands = base_cmd && base_cmd->subcommands_dict;
    return has_subcommands;
}

// OK
struct redisCommand *lookupSubcommand(struct redisCommand *container, sds sub_name) {
    return dictFetchValue(container->subcommands_dict, sub_name);
}

/* 通过 argc、argv 查找对应的命令
如果' strict '是1,我们希望argc是精确的(例如,对于子命令argc==2,对于顶级命令argc==1)
strict '应该在每次我们想要查找命令名时(例如在command INFO中)使用,而不是在processCommand中查找用户请求执行的命令.
 */
struct redisCommand *lookupCommandLogic(dict *commands, robj **argv, int argc, int strict) {
    struct redisCommand *base_cmd = dictFetchValue(commands, argv[0]->ptr);
    int has_subcommands = base_cmd && base_cmd->subcommands_dict;
    // 严格模式下,参数命令只能是1、2
    if (argc == 1 || !has_subcommands) {
        if (strict && argc != 1)
            return NULL;
        // 只有一个参数,且没有子命令,例如         CONFIG
        return base_cmd;
    }
    else { /* argc > 1 && has_subcommands */
        if (strict && argc != 2)
            return NULL;
        //      目前我们只支持一层子命令
        return lookupSubcommand(base_cmd, argv[1]->ptr);
    }
}

// 查找命令
struct redisCommand *lookupCommand(robj **argv, int argc) {
    return lookupCommandLogic(server.commands, argv, argc, 0);
}

struct redisCommand *lookupCommandBySdsLogic(dict *commands, sds s) {
    int argc, j;
    sds *strings = sdssplitlen(s, sdslen(s), "|", 1, &argc);
    if (strings == NULL)
        return NULL;
    if (argc > 2) {
        /* Currently we support just one level of subcommands */
        sdsfreesplitres(strings, argc);
        return NULL;
    }

    robj objects[argc];
    robj *argv[argc];
    for (j = 0; j < argc; j++) {
        initStaticStringObject(objects[j], strings[j]);
        argv[j] = &objects[j];
    }

    struct redisCommand *cmd = lookupCommandLogic(commands, argv, argc, 1);
    sdsfreesplitres(strings, argc);
    return cmd;
}

struct redisCommand *lookupCommandBySds(sds s) {
    return lookupCommandBySdsLogic(server.commands, s);
}

struct redisCommand *lookupCommandByCStringLogic(dict *commands, const char *s) {
    struct redisCommand *cmd;
    sds name = sdsnew(s);

    cmd = lookupCommandBySdsLogic(commands, name);
    sdsfree(name);
    return cmd;
}

// 根据给定命令名字（C 字符串）,查找命令
struct redisCommand *lookupCommandByCString(const char *s) {
    return lookupCommandByCStringLogic(server.commands, s);
}

// 从当前命令表 server.commands 中查找给定名字,
// 如果没找到的话,就尝试从 server.orig_commands 中查找未被改名的原始名字
// 原始表中的命令名不受 redis.conf 中命令改名的影响
// 这个函数可以在命令被更名之后,仍然在重写命令时得出正确的名字.

struct redisCommand *lookupCommandOrOriginal(robj **argv, int argc) {
    // 查找当前表

    struct redisCommand *cmd = lookupCommandLogic(server.commands, argv, argc, 0);
    // 如果有需要的话,查找原始表

    if (!cmd) {
        cmd = lookupCommandLogic(server.orig_commands, argv, argc, 0);
    }
    return cmd;
}

// 来自master客户端或AOF客户端的命令永远不应该被拒绝.
int mustObeyClient(client *c) {
    return c->id == CLIENT_ID_AOF || c->flags & CLIENT_MASTER;
}

// 是否应该传播
static int shouldPropagate(int target) {
    if (!server.replication_allowed || target == PROPAGATE_NONE || server.loading)
        return 0;

    if (target & PROPAGATE_AOF) {
        if (server.aof_state != AOF_OFF) {
            return 1;
        }
    }
    if (target & PROPAGATE_REPL) {
        if (server.masterhost == NULL &&                               // 没有配置主节点
            (server.repl_backlog || listLength(server.slaves) != 0)) { // 自己是主节点
            return 1;
        }
    }

    return 0;
}

// 将指定命令（以及执行该命令的上下文,比如数据库 id 等信息）传播到 AOF 和 slave .
// FLAG 可以是以下标识的 xor ：
// + REDIS_PROPAGATE_NONE (no propagation of command at all) 不传播
// + REDIS_PROPAGATE_AOF (propagate into the AOF file if is enabled)传播到 AOF
// + REDIS_PROPAGATE_REPL (propagate into the replication link)传播到 slave
static void propagateNow(int dbid, robj **argv, int argc, int target) {
    if (!shouldPropagate(target)) // 是否应该传播
        return;

    serverAssert(!(areClientsPaused() && !server.client_pause_in_transaction));
    // 传播到 AOF

    if (server.aof_state != AOF_OFF && target & PROPAGATE_AOF)
        feedAppendOnlyFile(dbid, argv, argc); // 记录AOF日志
    // 传播到 slave
    if (target & PROPAGATE_REPL)
        replicationFeedSlaves(server.slaves, dbid, argv, argc);
}

// 在命令内部使用，用于在当前命令传播到AOF / Replication之后安排其他命令的传播。
// dbid是命令应该传播到的数据库ID。要传播的命令的参数作为len 'argc'的redis对象指针数组传递，使用'argv'向量。
// 该函数不接受传递的'argv'向量的引用，因此由调用者释放传递的argv(但它通常是堆栈分配的)。该函数自动增加传递对象的ref计数，因此调用者不需要这样做。
void alsoPropagate(int dbid, robj **argv, int argc, int target) {
    robj **argvcopy;
    int j;

    if (!shouldPropagate(target)) // 是否应该传播
        return;

    argvcopy = zmalloc(sizeof(robj *) * argc);
    for (j = 0; j < argc; j++) {
        argvcopy[j] = argv[j];
        incrRefCount(argv[j]);
    }
    redisOpArrayAppend(&server.also_propagate, dbid, argvcopy, argc, target);
}

// 可以在Redis命令实现中调用函数 forceCommandPropagation()，以强制将特定命令的执行传播到AOF/Replication中。
void forceCommandPropagation(client *c, int flags) {
    serverAssert(c->cmd->flags & (CMD_WRITE | CMD_MAY_REPLICATE));
    if (flags & PROPAGATE_REPL)
        c->flags |= CLIENT_FORCE_REPL; // 表示强制服务器将当前执行的命令复制给所有从服务器
    if (flags & PROPAGATE_AOF)
        c->flags |= CLIENT_FORCE_AOF;
}

// 完全避免传播所执行的命令。这样我们就可以使用alsoPropagate() API自由地传播我们想要的东西。
void preventCommandPropagation(client *c) {
    c->flags |= CLIENT_PREVENT_PROP; // 阻止命令传播到aof、repl
}

// 记录命令到 slowlog
void slowlogPushCurrentCommand(client *c, struct redisCommand *cmd, ustime_t duration) {
    // 一些命令可能包含敏感数据, 因此不能存储在slowlog
    if (cmd->flags & CMD_SKIP_SLOWLOG) {
        return;
    }
    // 如果命令参数向量被重写,使用原始的参数
    robj **argv = c->original_argv ? c->original_argv : c->argv;
    int argc = c->original_argv ? c->original_argc : c->argc;
    slowlogPushEntryIfNeeded(c, argv, argc, duration);
}

// 调用这个函数是为了更新命令直方图的总持续时间.延迟时间单位为纳秒.如果需要,它将分配直方图内存并将持续时间修剪到跟踪上限/下限
// 更新延迟直方图
void updateCommandLatencyHistogram(struct hdr_histogram **latency_histogram, int64_t duration_hist) {
    if (duration_hist < LATENCY_HISTOGRAM_MIN_VALUE)
        duration_hist = LATENCY_HISTOGRAM_MIN_VALUE;
    if (duration_hist > LATENCY_HISTOGRAM_MAX_VALUE)
        duration_hist = LATENCY_HISTOGRAM_MAX_VALUE;
    if (*latency_histogram == NULL) {
        hdr_init(LATENCY_HISTOGRAM_MIN_VALUE, LATENCY_HISTOGRAM_MAX_VALUE, LATENCY_HISTOGRAM_PRECISION, latency_histogram);
    }
    hdr_record_value(*latency_histogram, duration_hist);
}

// 处理alsoPropagate() API来处理想要传播多个分离命令的命令。注意，alsoPropagate()不受CLIENT_PREVENT_PROP标志的影响。
void propagatePendingCommands() {
    if (server.also_propagate.numops == 0)
        return;
    // 传播额外的命令
    int j;
    redisOp *rop;
    int multi_emitted = 0;

    // 在服务器中包装命令。但如果我们已经在MULTI上下文中，就不要包装它，以防嵌套的MULTI/EXEC。
    // 如果数组只包含一个命令，则不需要对其进行换行，因为单个命令是原子的。
    if (server.also_propagate.numops > 1 && !server.propagate_no_multi) { // 允许传播事务

        // 我们使用第一个传播命令来为MULTI设置dbid，这样SELECT将提前传播
        int multi_dbid = server.also_propagate.ops[0].dbid;
        propagateNow(multi_dbid, &shared.multi, 1, PROPAGATE_AOF | PROPAGATE_REPL);
        multi_emitted = 1;
    }

    for (j = 0; j < server.also_propagate.numops; j++) {
        rop = &server.also_propagate.ops[j];
        serverAssert(rop->target);
        propagateNow(rop->dbid, rop->argv, rop->argc, rop->target);
    }

    if (multi_emitted) {
        /* We take the dbid from last command so that propagateNow() won't inject another SELECT */
        int exec_dbid = server.also_propagate.ops[server.also_propagate.numops - 1].dbid;
        propagateNow(exec_dbid, &shared.exec, 1, PROPAGATE_AOF | PROPAGATE_REPL);
    }

    redisOpArrayFree(&server.also_propagate);
}

// 增加命令失败计数器(rejected_calls或failed_calls).
// 使用flags参数决定递增哪个计数器,选项如下:
//    ERROR_COMMAND_REJECTED - 更新 rejected_calls
//    ERROR_COMMAND_FAILED - 更新 failed_calls
// 该函数还重置 prev_err_count,以确保我们不会计算相同的错误两次,它可能通过一个NULL cmd值,以表明错误在其他地方被计算.
// 如果统计信息更新了,函数返回true,如果没有更新则返回false.
int incrCommandStatsOnError(struct redisCommand *cmd, int flags) {
    // 保持上次执行命令时捕获的prev错误计数
    static long long prev_err_count = 0;
    int res = 0;
    if (cmd) {
        if (server.stat_total_error_replies > prev_err_count) {
            if (flags & ERROR_COMMAND_REJECTED) {
                cmd->rejected_calls++;
                res = 1;
            }
            else if (flags & ERROR_COMMAND_FAILED) {
                cmd->failed_calls++;
                res = 1;
            }
        }
    }
    prev_err_count = server.stat_total_error_replies;
    return res;
}

/* Call() 是Redis执行命令的核心.
 *
 * The following flags can be passed:
 * CMD_CALL_NONE        没有标志
 * CMD_CALL_SLOWLOG     检查命令执行事件,如果需要,记录慢日志
 * CMD_CALL_STATS       填充命令统计数据.
 * CMD_CALL_PROPAGATE_AOF   如果AOF修改了数据集,或者客户端标志强制传播,则向AOF追加命令.
 * CMD_CALL_PROPAGATE_REPL  发送命令给slave,如果它修改了数据集或如果客户端标志强制传播.
 * CMD_CALL_PROPAGATE   PROPAGATE_AOF|PROPAGATE_REPL.
 * CMD_CALL_FULL        SLOWLOG|STATS|PROPAGATE.
 *
 * 确切的传播行为取决于客户端标志.具体地说:
1.  如果设置了客户端标志CLIENT_FORCE_AOF或CLIENT_FORCE_REPL,并假定在调用标志中设置了相应的CMD_CALL_PROPAGATE_AOF/REPL,那么即使数据集不受该命令的影响,该命令也会被传播.
2.  如果客户端设置了CLIENT_PREVENT_REPL_PROP或CLIENT_PREVENT_AOF_PROP标志,那么即使命令修改了数据集,也不会执行到AOF或slave的传播.

注意,不管客户端标记是什么,如果CMD_CALL_PROPAGATE_AOF或CMD_CALL_PROPAGATE_REPL没有设置,那么分别不会发生AOF或 repl 传播.
客户端标志通过使用以下API实现给定的命令来修改:
 *
 * forceCommandPropagation(client *c, int flags);
 * preventCommandPropagation(client *c);
 * preventCommandAOF(client *c);
 * preventCommandReplication(client *c);
 */
// 调用命令的实现函数,执行命令
void call(client *c, int flags) {
    // 调用函数
    long long dirty;
    uint64_t client_old_flags = c->flags; // 记录命令开始执行前的 FLAG
    struct redisCommand *real_cmd = c->realcmd;
    // 重置 AOF、REPL 传播标志; AOF、REPL 禁止传播标志
    c->flags &= ~(CLIENT_FORCE_AOF | CLIENT_FORCE_REPL | CLIENT_PREVENT_PROP);

    int prev_core_propagates = server.core_propagates;
    if (!server.core_propagates && !(flags & CMD_CALL_FROM_MODULE)) {
        // core_propagates 为0,且不是从module调用的
        server.core_propagates = 1; // 可以理解为是不是从 processCommand 调用的
    }

    // 保留旧 dirty 参数个数值
    dirty = server.dirty;
    incrCommandStatsOnError(NULL, 0); // 增加命令错误计数

    const long long call_timer = ustime(); // 计算命令开始执行的时间

    // 更新缓存时间,如果我们有嵌套调用,我们希望只在第一次调用时更新
    if (server.fixed_time_expire == 0) {
        server.fixed_time_expire++;
        updateCachedTimeWithUs(0, call_timer); // 更新server缓存的时间
    }

    monotime monotonic_start = 0;
    if (monotonicGetType() == MONOTONIC_CLOCK_HW) { // 单调时钟类型
        monotonic_start = getMonotonicUs();
    }

    server.in_nested_call++; // 表示嵌套层级
    // 执行实现函数
    c->cmd->proc(c);
    server.in_nested_call--;

    // 为了避免由于使用一个系统调用查询时钟3次而造成的性能影响,我们使用单调时钟,当我们确信它的成本很低时,否则就退回到非单调调用.
    ustime_t duration;
    // 计算命令执行耗费的时间
    if (monotonicGetType() == MONOTONIC_CLOCK_HW) {
        duration = getMonotonicUs() - monotonic_start;
    }
    else {
        duration = ustime() - call_timer;
    }

    c->duration = duration;       // 命令执行时间
    dirty = server.dirty - dirty; // 计算命令执行之后的 dirty 值
    if (dirty < 0) {
        dirty = 0;
    }

    // 更新失败计数
    if (!incrCommandStatsOnError(real_cmd, ERROR_COMMAND_FAILED) && c->deferred_reply_errors) {
        real_cmd->failed_calls++;
    }

    // 在执行命令后,如果设置了'CLIENT_CLOSE_AFTER_COMMAND'标志,我们将在写完整个回复后关闭客户端.
    if (c->flags & CLIENT_CLOSE_AFTER_COMMAND) {
        c->flags &= ~CLIENT_CLOSE_AFTER_COMMAND; // 只置空 CLIENT_CLOSE_AFTER_COMMAND
        c->flags |= CLIENT_CLOSE_AFTER_REPLY;    // 设置 CLIENT_CLOSE_AFTER_REPLY
    }

    // 服务器正在进行载入 且 这是lua使用的非连接 客户端  ; 不将从 Lua 中发出的命令放入 SLOWLOG ,也不进行统计
    if (server.loading && c->flags & CLIENT_SCRIPT) {
        flags &= ~(CMD_CALL_SLOWLOG | CMD_CALL_STATS); //
    }

    // 如果调用者是 Lua ,那么根据命令 FLAG 和客户端 FLAG 打开传播（propagate)标志
    if (c->flags & CLIENT_SCRIPT && server.script_caller) {
        if (c->flags & CLIENT_FORCE_REPL) {
            server.script_caller->flags |= CLIENT_FORCE_REPL;
        }
        if (c->flags & CLIENT_FORCE_AOF) {
            server.script_caller->flags |= CLIENT_FORCE_AOF;
        }
    }

    // 注意:下面的代码使用的是实际执行的命令c->cmd和c->lastcmd可能不同,如果是MULTI-EXEC或重写的命令,如EXPIRE, GEOADD等.
    // 记录此命令在主线程上引起的延迟.除非caller不要log.(发生在从AOF内部处理MULTI-EXEC时).
    if (flags & CMD_CALL_SLOWLOG) {
        char *latency_event = (real_cmd->flags & CMD_FAST) ? "fast-command" : "command";
        latencyAddSampleIfNeeded(latency_event, duration / 1000); // 添加延迟样本数据
        UNUSED(server.latency_monitor_threshold);
    }

    // 如果有需要,将命令放到 SLOWLOG 里面
    if ((flags & CMD_CALL_SLOWLOG) && !(c->flags & CLIENT_BLOCKED)) {
        slowlogPushCurrentCommand(c, real_cmd, duration); // 记录慢日志
    }

    // 如果适用,将命令发送到MONITOR模式的客户端.管理命令被认为太危险而不能显示.
    if (!(c->cmd->flags & (CMD_SKIP_MONITOR | CMD_ADMIN))) {
        // 有 CMD_SKIP_MONITOR、CMD_ADMIN 任一标志,就不发送了
        robj **argv = c->original_argv ? c->original_argv : c->argv;
        int argc = c->original_argv ? c->original_argc : c->argc;
        // 如果可以的话,将命令发送到 MONITOR
        // 把请求命令复制到monitors链表的每个元素的缓冲区上
        replicationFeedMonitors(c, server.monitors, c->db->id, argv, argc);
    }

    if (!(c->flags & CLIENT_BLOCKED)) {
        // 没有阻塞
        freeClientOriginalArgv(c); // 释放original_argv变量占用的内存
    }

    // 填充我们在INFO commandstats中显示的每个命令的统计信息.
    if (flags & CMD_CALL_STATS) {
        real_cmd->microseconds += duration; // 微妙
        real_cmd->calls++;
        // 如果客户端被阻塞,当它被解除阻塞时,我们将处理延迟统计.
        if (server.latency_tracking_enabled && !(c->flags & CLIENT_BLOCKED)) {
            // 允许追踪  且 客户端没有阻塞
            updateCommandLatencyHistogram(&(real_cmd->latency_histogram), duration * 1000);
        }
    }

    // 将命令复制到 AOF 和 slave 节点
    if (flags & CMD_CALL_PROPAGATE && (c->flags & CLIENT_PREVENT_PROP) != CLIENT_PREVENT_PROP && c->cmd->proc != execCommand && !(c->cmd->flags & CMD_MODULE)) {
        int propagate_flags = PROPAGATE_NONE;

        // 如果数据库有被修改,那么启用 REPL 和 AOF 传播

        if (dirty) { // 有key被变更了
            propagate_flags |= (PROPAGATE_AOF | PROPAGATE_REPL);
        }
        // 强制 REPL 传播
        if (c->flags & CLIENT_FORCE_REPL) {
            propagate_flags |= PROPAGATE_REPL;
        }
        // 强制 AOF 传播
        if (c->flags & CLIENT_FORCE_AOF) {
            propagate_flags |= PROPAGATE_AOF;
        }

        /* 但是，如果命令实现调用preventCommandPropagation()或类似的方法，或者如果我们没有call()标志来这样做，则防止AOF / replication传播。 */
        if (c->flags & CLIENT_PREVENT_REPL_PROP || !(flags & CMD_CALL_PROPAGATE_REPL)) {
            propagate_flags &= ~PROPAGATE_REPL;
        }
        if (c->flags & CLIENT_PREVENT_AOF_PROP || !(flags & CMD_CALL_PROPAGATE_AOF)) {
            propagate_flags &= ~PROPAGATE_AOF;
        }

        if (propagate_flags != PROPAGATE_NONE) {
            alsoPropagate(c->db->id, c->argv, c->argc, propagate_flags);
        }
    }

    // 恢复旧的复制标志,因为call()可以递归执行.
    {
        c->flags &= ~(CLIENT_FORCE_AOF | CLIENT_FORCE_REPL | CLIENT_PREVENT_PROP);
        c->flags |= client_old_flags & (CLIENT_FORCE_AOF | CLIENT_FORCE_REPL | CLIENT_PREVENT_PROP);
    }

    // 如果客户端为客户端缓存启用了键跟踪,请确保记住它通过这个命令获取的键.
    if (c->cmd->flags & CMD_READONLY) {
        // 当前call 函数的调用方
        client *caller = (c->flags & CLIENT_SCRIPT && server.script_caller) ? server.script_caller : c;
        if (caller->flags & CLIENT_TRACKING && !(caller->flags & CLIENT_TRACKING_BCAST)) {
            // 客户端启用了key跟踪,以便执行客户端缓存.
            trackingRememberKeys(caller);
        }
    }

    server.fixed_time_expire--; // 如果>0,则根据server.mstime对密钥进行过期.
    server.stat_numcommands++;  // 已处理命令的数量

    // 记录服务器的内存峰值
    size_t zmalloc_used = zmalloc_used_memory();
    if (zmalloc_used > server.stat_peak_memory) {
        server.stat_peak_memory = zmalloc_used;
    }

    // 做一些维护工作和清洁工作
    afterCommand(c);

    // 客户端暂停在事务完成后生效.这需要在传播所有内容之后进行定位.
    if (!server.in_exec && server.client_pause_in_transaction) {
        // 执行完,在暂停中
        server.client_pause_in_transaction = 0;
    }
    server.core_propagates = prev_core_propagates;
}

/* Used when a command that is ready for execution needs to be rejected, due to
 * various pre-execution checks. it returns the appropriate error to the client.
 * If there's a transaction is flags it as dirty, and if the command is EXEC,
 * it aborts the transaction.
 * Note: 'reply' is expected to end with \r\n */
void rejectCommand(client *c, robj *reply) {
    flagTransaction(c); // 标记事务
    if (c->cmd)
        c->cmd->rejected_calls++;
    if (c->cmd && c->cmd->proc == execCommand) {
        execCommandAbort(c, reply->ptr);
    }
    else {
        /* using addReplyError* rather than addReply so that the error can be logged. */
        addReplyErrorObject(c, reply);
    }
}

void rejectCommandSds(client *c, sds s) {
    flagTransaction(c); // 标记事务
    if (c->cmd) {
        c->cmd->rejected_calls++;
    }
    if (c->cmd && c->cmd->proc == execCommand) {
        execCommandAbort(c, s);
        sdsfree(s);
    }
    else {
        /* The following frees 's'. */
        addReplyErrorSds(c, s);
    }
}

void rejectCommandFormat(client *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sds s = sdscatvprintf(sdsempty(), fmt, ap);
    va_end(ap);
    /* Make sure there are no newlines in the string, otherwise invalid protocol
     * is emitted (The args come from the user, they may contain any character). */
    sdsmapchars(s, "\r\n", "  ", 2);
    rejectCommandSds(c, s);
}

// 这是在call命令后调用的，我们可以在其中做一些维护工作。
void afterCommand(client *c) {
    UNUSED(c);
    if (!server.in_nested_call) {
        // 如果我们在最顶端的call()，我们可以传播我们积累的东西。应该在trackingHandlePendingKeyInvalidations之前完成，以便我们在无效缓存之前回复客户端(更有意义)
        if (server.core_propagates)
            propagatePendingCommands();
        /* 仅当不在嵌套调用时刷新挂起的无效消息。因此消息不会与事务响应交织。*/
        trackingHandlePendingKeyInvalidations();
    }
}

// 对于参数中可能包含键名的命令，返回1，但传统范围规范没有涵盖所有这些命令。
void populateCommandMovableKeys(struct redisCommand *cmd) {
    int movablekeys = 0;
    if (cmd->getkeys_proc && !(cmd->flags & CMD_MODULE)) {
        movablekeys = 1;
    }
    else if (cmd->flags & CMD_MODULE_GETKEYS) {
        movablekeys = 1;
    }
    else {
        /* Redis命令没有getkeys过程，但可能有可移动的键，因为键规范。 */
        for (int i = 0; i < cmd->key_specs_num; i++) {
            if (cmd->key_specs[i].begin_search_type != KSPEC_BS_INDEX || cmd->key_specs[i].find_keys_type != KSPEC_FK_RANGE) {
                /* 如果我们有一个非范围规格，这意味着我们有可移动的键*/
                movablekeys = 1;
                break;
            }
        }
    }

    if (movablekeys)
        cmd->flags |= CMD_MOVABLE_KEYS;
}

// 检查c-> cmd 是否存在
int commandCheckExistence(client *c, sds *err) {
    if (c->cmd) {
        return 1;
    }
    if (!err) {
        return 0;
    }
    // 没有找到对应的命令
    if (isContainerCommandBySds(c->argv[0]->ptr)) {
        /* 如果我们找不到命令,但argv[0]本身是一个命令,这意味着我们正在处理一个无效的子命令.打印帮助.*/
        sds cmd = sdsnew((char *)c->argv[0]->ptr);
        sdstoupper(cmd);
        *err = sdsnew(NULL);
        *err = sdscatprintf(*err, "未知的子命令 '%.128s'. Try %s HELP.", (char *)c->argv[1]->ptr, cmd);
        sdsfree(cmd);
    }
    else {
        sds args = sdsempty();
        int i;
        for (i = 1; i < c->argc && sdslen(args) < 128; i++) args = sdscatprintf(args, "'%.*s' ", 128 - (int)sdslen(args), (char *)c->argv[i]->ptr);
        *err = sdsnew(NULL);
        *err = sdscatprintf(*err, "未知的命令 '%.128s', with args beginning with: %s", (char *)c->argv[0]->ptr, args);
        sdsfree(args);
    }
    // 确保字符串中没有换行符，否则将发出无效协议(args来自用户，它们可能包含任何字符)。
    sdsmapchars(*err, "\r\n", "  ", 2);
    return 0;
}

// 参数个数检查
int commandCheckArity(client *c, sds *err) {
    // arity 命令执行需要的 参数个数 规定好的  ， 可以用 -N 表示 >= N
    if ((c->cmd->arity > 0 && c->cmd->arity != c->argc) || (c->argc < -c->cmd->arity)) {
        if (err) {
            *err = sdsnew(NULL);
            *err = sdscatprintf(*err, "错误的参数个数，命令： '%s'", c->cmd->fullname);
        }
        return 0;
    }

    return 1;
}

// 这个函数执行时,我们已经读入了一个完整的命令到客户端,
// 这个函数负责执行这个命令,或者服务器准备从客户端中进行一次读取.
// 如果这个函数返回 1 ,那么表示客户端在执行命令之后仍然存在,调用者可以继续执行其他操作.
// 否则,如果这个函数返回 0 ,那么表示客户端已经被销毁.
int processCommand(client *c) {
    if (!scriptIsTimedout()) { // 脚本已经超时
        /*
        EXEC和EVAL都直接调用call(),所以不应该有办法in_exec或in_eval是1.
        除非lua_timeout,在这种情况下客户端可能会运行一些命令.
        */
        serverAssert(!server.in_exec);
        serverAssert(!server.in_script);
    }

    moduleCallCommandFilters(c);
    serverLog(LL_DEBUG, "c->argv[0]->ptr->  %s\n", (char *)c->argv[0]->ptr); // SET
    // 处理可能的安全攻击
    if (!strcasecmp(c->argv[0]->ptr, "host:") || !strcasecmp(c->argv[0]->ptr, "post")) {
        securityWarningCommand(c);
        return C_ERR;
    }

    if (server.busy_module_yield_flags != BUSY_MODULE_YIELD_NONE &&   // 我们是不是在一个繁忙的模块中
        !(server.busy_module_yield_flags & BUSY_MODULE_YIELD_CLIENTS) // 如果希望避免处理客户端,则延迟该命令
    ) {
        c->bpop.timeout = 0; // 设置阻塞超时为0,永不超时？
        blockClient(c, BLOCKED_POSTPONE);
        return C_OK;
    }

    // 查找命令,并进行命令合法性检查,以及命令参数个数检查
    c->cmd = c->lastcmd = c->realcmd = lookupCommand(c->argv, c->argc); // 命令查找入口 🚪
    sds err;
    if (!commandCheckExistence(c, &err)) { // 检查命令是否存在
        rejectCommandSds(c, err);
        return C_OK;
    }
    if (!commandCheckArity(c, &err)) { // 检查命令参数
        rejectCommandSds(c, err);
        return C_OK;
    }

    if (c->cmd->flags & CMD_PROTECTED) {                                                             // 命令被标记为受保护状态，只允许在本地连接
        if ((c->cmd->proc == debugCommand && !allowProtectedAction(server.enable_debug_cmd, c))      // debug命令集  且 server不允许启用Protected命令
            || (c->cmd->proc == moduleCommand && !allowProtectedAction(server.enable_module_cmd, c)) // module命令集 且 server不允许启用Protected命令
        ) {
            rejectCommandFormat(
                c,                                                                                                                           //
                "不允许%s命令。如果%s选项被设置为\"local\"，您可以从本地连接运行它，否则您需要在配置文件中设置此选项，然后重新启动服务器。", //
                c->cmd->proc == debugCommand ? "DEBUG" : "MODULE",                                                                           //
                c->cmd->proc == debugCommand ? "enable-debug-command" : "enable-module-command"                                              //
            );
            return C_OK;
        }
    }
    // 读命令,不修改 key space       (当前命令,以及事务里边的命令  这两者均要判断)
    int is_read_command = (c->cmd->flags & CMD_READONLY) || (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_READONLY));
    // 写入命令,可能会修改 key space
    int is_write_command = (c->cmd->flags & CMD_WRITE) || (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_WRITE));
    // 可能会占用大量内存,执行前需要先检查服务器的内存使用情况,如果内存紧缺的话就禁止执行这个命令
    int is_deny_oom_command = (c->cmd->flags & CMD_DENYOOM) || (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_DENYOOM));
    // 允许在从节点带有过期数据时执行的命令. 这类命令很少有,只有几个.
    int is_deny_stale_command = !(c->cmd->flags & CMD_STALE) || (c->cmd->proc == execCommand && (c->mstate.cmd_inv_flags & CMD_STALE));
    // 允许在载入数据库时使用的命令
    int is_deny_loading_command = !(c->cmd->flags & CMD_LOADING) || (c->cmd->proc == execCommand && (c->mstate.cmd_inv_flags & CMD_LOADING));
    // 命令可能会产生复制流量、且允许写,例如pubsub
    int is_may_replicate_command = (c->cmd->flags & (CMD_WRITE | CMD_MAY_REPLICATE)) || (c->cmd->proc == execCommand && (c->mstate.cmd_flags & (CMD_WRITE | CMD_MAY_REPLICATE)));
    // 在异步加载期间拒绝（当副本使用无盘同步swapdb时,允许访问旧数据集）.
    int is_deny_async_loading_command = (c->cmd->flags & CMD_NO_ASYNC_LOADING) || (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_NO_ASYNC_LOADING));
    // 来自master客户端或AOF客户端的命令永远不应该被拒绝
    int obey_client = mustObeyClient(c);

    // 检查认证信息
    if (authRequired(c)) {
        if (!(c->cmd->flags & CMD_NO_AUTH)) { // 命令需要认证
            rejectCommand(c, shared.noautherr);
            return C_OK;
        }
    }
    // 客户端处于 事务状态 ,并且其中的一条命令 不允许在事务中运行
    if (c->flags & CLIENT_MULTI && c->cmd->flags & CMD_NO_MULTI) {
        rejectCommandFormat(c, "命令不允许在事务中运行");
        return C_OK;
    }

    // 检查用户是否可以根据当前的acl执行该命令
    int acl_errpos;
    int acl_retval = ACLCheckAllPerm(c, &acl_errpos);
    if (acl_retval != ACL_OK) {
        addACLLogEntry(c, acl_retval, (c->flags & CLIENT_MULTI) ? ACL_LOG_CTX_MULTI : ACL_LOG_CTX_TOPLEVEL, acl_errpos, NULL, NULL);
        switch (acl_retval) {
            case ACL_DENIED_CMD: {
                rejectCommandFormat(c, "-NOPERM 这个用户没有权限运行 '%s' 命令", c->cmd->fullname);
                break;
            }
            case ACL_DENIED_KEY:
                rejectCommandFormat(c, "-NOPERM 这个用户没有权限访问输入的参数");
                break;
            case ACL_DENIED_CHANNEL:
                rejectCommandFormat(c, "-NOPERM 这个用户没有权限访问用作参数的通道之一");
                break;
            default:
                rejectCommandFormat(c, "没有权限");
                break;
        }
        return C_OK;
    }

    // 如果开启了集群模式,那么在这里进行转向操作.
    // 不过,如果有以下情况出现,那么节点不进行转向：
    // 1) 命令的发送者是本节点的主节点
    // 2) 命令没有 key 参数
    if (
        // 必须遵从的客户端 能转向
        server.cluster_enabled && !mustObeyClient(c) &&
        (
            // keys 可以移动 ,
            (c->cmd->flags & CMD_MOVABLE_KEYS) || c->cmd->key_specs_num != 0 || c->cmd->proc == execCommand)) {
        int error_code;
        clusterNode *n = getNodeByQuery(c, c->cmd, c->argv, c->argc, &c->slot, &error_code); // 返回指向能够执行该命令的集群节点的指针
        if (n == NULL || n != server.cluster->myself) {
            // 不能执行多键处理命令
            // 命令针对的槽和键不是本节点处理的,进行转向

            if (c->cmd->proc == execCommand) {
                discardTransaction(c);
            }
            else {
                flagTransaction(c); // 标记事务
            }
            clusterRedirectClient(c, n, c->slot, error_code);
            c->cmd->rejected_calls++;
            return C_OK;
        }
        // 如果执行到这里,说明键 key 所在的槽由本节点处理
        // 或者客户端执行的是无参数命令
    }

    // 如果客户端总内存过高,则断开一些客户端.我们在删除键之前、在执行最后一个命令并消耗了一些客户端输出缓冲区内存之后执行此操作.
    evictClients(); // 驱逐客户端
    if (server.current_client == NULL) {
        // 如果我们驱逐自己,那么就中止处理命令
        return C_ERR;
    }

    // 处理maxmemory指令.
    // 注意,如果我们在这里重新进入事件循环,我们不希望回收内存,因为有一个繁忙的Lua脚本在超时条件下运行,以避免由于退出而混合了脚本的传播和del的传播.
    if (server.maxmemory && !scriptIsTimedout()) { // 设置了最大内存,且脚本没有超时
        // TODO
        int out_of_memory = (performEvictions() == EVICT_FAIL);

        /* performEvictions may evict keys, so we need flush pending tracking
         * invalidation keys. If we don't do this, we may get an invalidation
         * message after we perform operation on the key, where in fact this
         * message belongs to the old value of the key before it gets evicted.*/
        trackingHandlePendingKeyInvalidations();

        /* performEvictions may flush slave output buffers. This may result
         * in a slave, that may be the active client, to be freed. */
        if (server.current_client == NULL)
            return C_ERR;

        int reject_cmd_on_oom = is_deny_oom_command;
        /* If client is in MULTI/EXEC context, queuing may consume an unlimited
         * amount of memory, so we want to stop that.
         * However, we never want to reject DISCARD, or even EXEC (unless it
         * contains denied commands, in which case is_deny_oom_command is already
         * set. */
        if (c->flags & CLIENT_MULTI && c->cmd->proc != execCommand && c->cmd->proc != discardCommand && c->cmd->proc != quitCommand && c->cmd->proc != resetCommand) {
            reject_cmd_on_oom = 1;
        }

        if (out_of_memory && reject_cmd_on_oom) {
            rejectCommand(c, shared.oomerr);
            return C_OK;
        }

        /* Save out_of_memory result at script start, otherwise if we check OOM
         * until first write within script, memory used by lua stack and
         * arguments might interfere. */
        if (c->cmd->proc == evalCommand || c->cmd->proc == evalShaCommand || c->cmd->proc == fcallCommand || c->cmd->proc == fcallroCommand) {
            server.script_oom = out_of_memory;
        }
    }

    // 确保为客户端缓存元数据使用合理的内存量
    if (server.tracking_clients) {
        trackingLimitUsedSlots();
    }

    // 检查磁盘是否有问题
    int deny_write_type = writeCommandsDeniedByDiskError();
    if (deny_write_type != DISK_ERROR_TYPE_NONE && (is_write_command || c->cmd->proc == pingCommand)) {
        // 磁盘有问题, 写命令|| ping命令
        if (obey_client) { // 该客户端的命令必须服从
            if (!server.repl_ignore_disk_write_error && c->cmd->proc != pingCommand) {
                serverPanic("副本无法将命令写入磁盘.");
            }
            else {
                static mstime_t last_log_time_ms = 0;
                const mstime_t log_interval_ms = 10000;
                if (server.mstime > last_log_time_ms + log_interval_ms) {
                    last_log_time_ms = server.mstime;
                    serverLog(LL_WARNING, "副本正在应用命令，即使它不能写入磁盘。");
                }
            }
        }
        else {
            sds err = writeCommandsGetDiskErrorMessage(deny_write_type);
            rejectCommandSds(c, err);
            return C_OK;
        }
    }

    // 如果一定数量的好的slave, 不接受写命令
    // 通过min-slaves-to-write配置
    if (is_write_command && !checkGoodReplicasStatus()) { // 没有 有效的副本
        rejectCommand(c, shared.noreplicaserr);
        return C_OK;
    }

    // 如果这个服务器是一个只读 slave 的话,那么拒绝执行写命令
    if (server.masterhost && server.repl_slave_ro && !obey_client && is_write_command) {
        rejectCommand(c, shared.roslaveerr);
        return C_OK;
    }

    // 订阅发布, 在RESP2中只允许一部分命令, RESP3没有限制
    if (                                              //
        (c->flags & CLIENT_PUBSUB && c->resp == 2) && //
        c->cmd->proc != pingCommand &&                //
        c->cmd->proc != subscribeCommand &&           //
        c->cmd->proc != ssubscribeCommand &&          //
        c->cmd->proc != unsubscribeCommand &&         //
        c->cmd->proc != sunsubscribeCommand &&        //
        c->cmd->proc != psubscribeCommand &&          //
        c->cmd->proc != punsubscribeCommand &&        //
        c->cmd->proc != quitCommand &&                //
        c->cmd->proc != resetCommand                  //
    ) {
        // 这些命令, 只能在RESP3协议中使用
        rejectCommandFormat(c, "不能执行'%s': 在当前上下文中只允许 (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / PING / QUIT / RESET 命令的执行", c->cmd->fullname);
        return C_OK;
    }

    //  当replica-serve-stale-data为no,并且我们是一个与master链接断开的副本时,只允许带有 CMD_STALE 标志的命令,如 INFO, REPLICAOF等.
    if (server.masterhost && server.repl_state != REPL_STATE_CONNECTED && server.repl_serve_stale_data == 0 && is_deny_stale_command) {
        rejectCommand(c, shared.masterdownerr);
        return C_OK;
    }

    // 如果服务器正在载入数据到数据库,那么只执行带有 REDIS_CMD_LOADING 标识的命令,否则将出错
    if (server.loading && !server.async_loading && is_deny_loading_command) {
        rejectCommand(c, shared.loadingerr);
        return C_OK;
    }

    // 异步加载中,是否阻塞当前命令
    if (server.async_loading && is_deny_async_loading_command) {
        rejectCommand(c, shared.loadingerr);
        return C_OK;
    }

    // Lua 脚本超时、模块繁忙,只允许执行限定的操作,比如 SHUTDOWN 和 SCRIPT KILL
    if ((scriptIsTimedout() || server.busy_module_yield_flags) && !(c->cmd->flags & CMD_ALLOW_BUSY)) {
        UNUSED(BUSY_MODULE_YIELD_NONE);
        UNUSED(BUSY_MODULE_YIELD_EVENTS);
        UNUSED(BUSY_MODULE_YIELD_CLIENTS);
        // 命令没有设置 CMD_ALLOW_BUSY 标志
        if (server.busy_module_yield_flags && server.busy_module_yield_reply) { // yield标志,以及有数据
            rejectCommandFormat(c, "-BUSY %s", server.busy_module_yield_reply);
        }
        else if (server.busy_module_yield_flags) { // 只有yield标志
            rejectCommand(c, shared.slowmoduleerr);
        }
        else if (scriptIsEval()) {
            rejectCommand(c, shared.slowevalerr);
        }
        else {
            rejectCommand(c, shared.slowscripterr);
        }
        return C_OK;
    }
    // 阻止slave 发送访问keyspace的命令.
    if ((c->flags & CLIENT_SLAVE) && (is_may_replicate_command || is_write_command || is_read_command)) {
        rejectCommandFormat(c, "slave 不能与 keyspace进行交互");
        return C_OK;
    }
    // 如果服务器被暂停,则阻止客户端,直到暂停结束.执行中的复制 永远不会被暂停.
    if (!(c->flags & CLIENT_SLAVE) && ((server.client_pause_type == CLIENT_PAUSE_ALL) || (server.client_pause_type == CLIENT_PAUSE_WRITE && is_may_replicate_command))) {
        c->bpop.timeout = 0;
        blockClient(c, BLOCKED_POSTPONE);
        return C_OK;
    }

    // 如果客户端有CLIENT_MULTI标记,并且当前不是exec、discard、multi、watch、quit、reset命令
    if (c->flags & CLIENT_MULTI && c->cmd->proc != execCommand && c->cmd->proc != discardCommand && c->cmd->proc != multiCommand && c->cmd->proc != watchCommand && c->cmd->proc != quitCommand && c->cmd->proc != resetCommand) {
        // 在事务上下文中
        // 除 EXEC 、 DISCARD 、 MULTI 和 WATCH 命令之外
        // 其他所有命令都会被入队到事务队列中
        queueMultiCommand(c); // 将命令入队保存,等待后续一起处理
        addReply(c, shared.queued);
    }
    else {
        // 执行命令
        call(c, CMD_CALL_FULL); // 调用call函数执行命令  processCommand
        c->woff = server.master_repl_offset;
        if (listLength(server.ready_keys)) {
            // 处理那些解除了阻塞的键
            handleClientsBlockedOnKeys();
        }
    }

    return C_OK;
}

/* ====================== 错误查找和执行 ===================== */
// 错误计数
void incrementErrorCount(const char *fullerr, size_t namelen) {
    struct redisError *error = raxFind(server.errors, (unsigned char *)fullerr, namelen);
    if (error == raxNotFound) {
        error = zmalloc(sizeof(*error));
        error->count = 0;
        raxInsert(server.errors, (unsigned char *)fullerr, namelen, error, NULL);
    }
    error->count++;
}

/*================================== 关闭 =============================== */

// 关闭监听套接字
void closeListeningSockets(int unlink_unix_socket) {
    int j;

    for (j = 0; j < server.ipfd.count; j++) close(server.ipfd.fd[j]);
    for (j = 0; j < server.tlsfd.count; j++) close(server.tlsfd.fd[j]);
    if (server.sofd != -1)
        close(server.sofd);
    if (server.cluster_enabled)
        for (j = 0; j < server.cfd.count; j++) close(server.cfd.fd[j]);
    if (unlink_unix_socket && server.unixsocket) {
        serverLog(LL_NOTICE, "删除unix socket文件。 ");
        if (unlink(server.unixsocket) != 0)
            serverLog(LL_WARNING, "删除unix套接字文件时出错: %s", strerror(errno));
    }
}

// 准备关闭服务器。flag:
// —SHUTDOWN_SAVE:保存数据库，即使服务器配置为不保存任何 dump。
// -SHUTDOWN_NOSAVE:不保存任何数据库 dump，即使服务器配置为保存。
// -SHUTDOWN_NOW:  关闭之前 不要等待副本赶上
// —SHUTDOWN_FORCE:忽略在磁盘上写入AOF和RDB文件的错误，这通常会阻止关机。
// 除非设置了SHUTDOWN_NOW，并且如果有任何副本滞后，则返回C_ERR。Shutdown_mstime被设置为一个时间戳，以允许副本有一个宽限期。这由serverCron()检查和处理，它会尽快完成关闭。
// 如果由于RDB或AOF文件写入错误导致关机失败，则返回C_ERR并记录错误。如果设置了标志SHUTDOWN_FORCE，则记录这些错误，但忽略这些错误并返回C_OK。
// 如果成功，这个函数返回C_OK，然后调用exit(0)。
int prepareForShutdown(int flags) {
    if (isShutdownInitiated()) // 优雅关闭限制的时间
        return C_ERR;

    // 当服务器在内存中加载数据集时调用SHUTDOWN，我们需要确保在关机时没有尝试保存数据集(否则它可能会用半读数据覆盖当前DB)。
    // 另外，在哨兵模式下，清除SAVE标志并强制NOSAVE。
    if (server.loading || server.sentinel_mode)
        flags = (flags & ~SHUTDOWN_SAVE) | SHUTDOWN_NOSAVE;

    server.shutdown_flags = flags;

    serverLog(LL_WARNING, "用户请求关闭...");
    if (server.supervised_mode == SUPERVISED_SYSTEMD)
        redisCommunicateSystemd("STOPPING=1\n");

    // 如果我们有任何副本，在关闭之前让它们赶上复制偏移量，以避免数据丢失。
    if (!(flags & SHUTDOWN_NOW) && server.shutdown_timeout != 0 && !isReadyToShutdown()) {
        server.shutdown_mstime = server.mstime + server.shutdown_timeout * 1000;
        if (!areClientsPaused()) {
            // 有暂停命令
            sendGetackToReplicas(); // 恢复slave状态
        }
        pauseClients(PAUSE_DURING_SHUTDOWN, LLONG_MAX, CLIENT_PAUSE_WRITE); // 暂停所有clients
        serverLog(LL_NOTICE, "在关闭之前等待副本同步。");
        return C_ERR;
    }

    return finishShutdown();
}

// 如果关闭被中止，返回C_OK，如果关闭没有进行，返回C_ERR。
int abortShutdown(void) {
    if (isShutdownInitiated()) {
        cancelShutdown();
    }
    else if (server.shutdown_asap) {
        /* 信号处理程序已经请求关闭，但还没有启动。只需清除该标志。*/
        server.shutdown_asap = 0;
    }
    else {
        return C_ERR;
    }
    serverLog(LL_NOTICE, "手动中止关闭。");
    return C_OK;
}

/*================================== Commands =============================== */

/*
有时候Redis不能接受写命令是因为RDB或AOF文件的持久性错误,Redis被配置为在这种情况下停止接受写.这个函数返回条件是否激活,以及条件的类型.
 * DISK_ERROR_TYPE_NONE:    没有问题,可以正常写.
 * DISK_ERROR_TYPE_AOF:     AOF错误.
 * DISK_ERROR_TYPE_RDB:     RDB错误.
 */
int writeCommandsDeniedByDiskError(void) {
    if (server.stop_writes_on_bgsave_err && server.saveparamslen > 0 && server.lastbgsave_status == C_ERR) {
        return DISK_ERROR_TYPE_RDB;
    }
    else if (server.aof_state != AOF_OFF) { // 没有关闭
        if (server.aof_last_write_status == C_ERR) {
            return DISK_ERROR_TYPE_AOF;
        }
        /* AOF fsync错误 */
        int aof_bio_fsync_status;
        atomicGet(server.aof_bio_fsync_status, aof_bio_fsync_status); // bio任务中的AOF同步状态
        if (aof_bio_fsync_status == C_ERR) {
            atomicGet(server.aof_bio_fsync_errno, server.aof_last_write_errno);
            return DISK_ERROR_TYPE_AOF;
        }
    }

    return DISK_ERROR_TYPE_NONE;
}

sds writeCommandsGetDiskErrorMessage(int error_code) {
    sds ret = NULL;
    if (error_code == DISK_ERROR_TYPE_RDB) {
        ret = sdsdup(shared.bgsaveerr->ptr);
    }
    else {
        ret = sdscatfmt(sdsempty(), "-MISCONF 写AOF文件出现错误: %s", strerror(server.aof_last_write_errno));
    }
    return ret;
}

/* PING命令。如果客户端处于Pub/Sub模式，它的工作方式就不同。*/
void pingCommand(client *c) {
    /* 该命令需要0个或1个参数。 */
    if (c->argc > 2) {
        addReplyErrorArity(c);
        return;
    }

    if (c->flags & CLIENT_PUBSUB && c->resp == 2) {
        addReply(c, shared.mbulkhdr[2]);
        addReplyBulkCBuffer(c, "pong", 4);
        if (c->argc == 1)
            addReplyBulkCBuffer(c, "", 0);
        else
            addReplyBulk(c, c->argv[1]);
    }
    else {
        if (c->argc == 1)
            addReply(c, shared.pong);
        else
            addReplyBulk(c, c->argv[1]);
    }
}

void echoCommand(client *c) {
    addReplyBulk(c, c->argv[1]);
}

void timeCommand(client *c) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    addReplyArrayLen(c, 2);
    addReplyBulkLongLong(c, tv.tv_sec);
    addReplyBulkLongLong(c, tv.tv_usec);
}

typedef struct replyFlagNames {
    uint64_t flag;
    const char *name;
} replyFlagNames;

// 输出flag的helper 函数
void addReplyCommandFlags(client *c, uint64_t flags, replyFlagNames *replyFlags) {
    int count = 0, j = 0;
    while (replyFlags[j].name) {
        if (flags & replyFlags[j].flag)
            count++;
        j++;
    }

    addReplySetLen(c, count);
    j = 0;
    while (replyFlags[j].name) {
        if (flags & replyFlags[j].flag) {
            addReplyStatus(c, replyFlags[j].name);
        }
        j++;
    }
}

void addReplyFlagsForCommand(client *c, struct redisCommand *cmd) {
    replyFlagNames flagNames[] = {
        {CMD_WRITE, "write"},
        {CMD_READONLY, "readonly"},
        {CMD_DENYOOM, "denyoom"},
        {CMD_MODULE, "module"},
        {CMD_ADMIN, "admin"},
        {CMD_PUBSUB, "pubsub"},
        {CMD_NOSCRIPT, "noscript"},
        {CMD_BLOCKING, "blocking"},
        {CMD_LOADING, "loading"},
        {CMD_STALE, "stale"},
        {CMD_SKIP_MONITOR, "skip_monitor"},
        {CMD_SKIP_SLOWLOG, "skip_slowlog"},
        {CMD_ASKING, "asking"},
        {CMD_FAST, "fast"},
        {CMD_NO_AUTH, "no_auth"},
        {CMD_MAY_REPLICATE, "may_replicate"},
        /* {CMD_SENTINEL,          "sentinel"}, Hidden on purpose */
        /* {CMD_ONLY_SENTINEL,     "only_sentinel"}, Hidden on purpose */
        {CMD_NO_MANDATORY_KEYS, "no_mandatory_keys"},
        /* {CMD_PROTECTED,         "protected"}, Hidden on purpose */
        {CMD_NO_ASYNC_LOADING, "no_async_loading"},
        {CMD_NO_MULTI, "no_multi"},
        {CMD_MOVABLE_KEYS, "movablekeys"},
        {CMD_ALLOW_BUSY, "allow_busy"},
        {0, NULL}};
    addReplyCommandFlags(c, cmd->flags, flagNames);
}

// 文档标志
void addReplyDocFlagsForCommand(client *c, struct redisCommand *cmd) {
    replyFlagNames docFlagNames[] = {
        {.flag = CMD_DOC_DEPRECATED, .name = "deprecated"}, // 弃用
        {.flag = CMD_DOC_SYSCMD, .name = "syscmd"},         // 系统命令
        {.flag = 0, .name = NULL}                           //
    };
    addReplyCommandFlags(c, cmd->doc_flags, docFlagNames);
}

void addReplyFlagsForKeyArgs(client *c, uint64_t flags) {
    replyFlagNames docFlagNames[] = {
        {.flag = CMD_KEY_RO, .name = "RO"},
        {.flag = CMD_KEY_RW, .name = "RW"},
        {.flag = CMD_KEY_OW, .name = "OW"},
        {.flag = CMD_KEY_RM, .name = "RM"},
        {.flag = CMD_KEY_ACCESS, .name = "access"},
        {.flag = CMD_KEY_UPDATE, .name = "update"},
        {.flag = CMD_KEY_INSERT, .name = "insert"},
        {.flag = CMD_KEY_DELETE, .name = "delete"},
        {.flag = CMD_KEY_NOT_KEY, .name = "not_key"},
        {.flag = CMD_KEY_INCOMPLETE, .name = "incomplete"},
        {.flag = CMD_KEY_VARIABLE_FLAGS, .name = "variable_flags"},
        {.flag = 0, NULL}};
    addReplyCommandFlags(c, flags, docFlagNames);
}

const char *ARG_TYPE_STR[] = {
    "string", "integer", "double", "key", "pattern", "unix-time", "pure-token", "oneof", "block",
};

void addReplyFlagsForArg(client *c, uint64_t flags) {
    replyFlagNames argFlagNames[] = {
        {.flag = CMD_ARG_OPTIONAL, .name = "optional"},
        {.flag = CMD_ARG_MULTIPLE, .name = "multiple"},
        {.flag = CMD_ARG_MULTIPLE_TOKEN, .name = "multiple_token"},
        {.flag = 0, .name = NULL},
    };
    addReplyCommandFlags(c, flags, argFlagNames);
}

// 返回命令参数列表
void addReplyCommandArgList(client *c, struct redisCommandArg *args, int num_args) {
    addReplyArrayLen(c, num_args);
    for (int j = 0; j < num_args; j++) {
        // 计算我们的回复len,这样我们就不用使用延迟回复了
        long maplen = 2;
        if (args[j].key_spec_index != -1)
            maplen++;
        if (args[j].token)
            maplen++;
        if (args[j].summary)
            maplen++;
        if (args[j].since)
            maplen++;
        if (args[j].deprecated_since)
            maplen++;
        if (args[j].flags)
            maplen++;
        if (args[j].type == ARG_TYPE_ONEOF || args[j].type == ARG_TYPE_BLOCK)
            maplen++;
        addReplyMapLen(c, maplen);

        addReplyBulkCString(c, "name");
        addReplyBulkCString(c, args[j].name);

        addReplyBulkCString(c, "type");
        addReplyBulkCString(c, ARG_TYPE_STR[args[j].type]);

        if (args[j].key_spec_index != -1) {
            addReplyBulkCString(c, "key_spec_index");
            addReplyLongLong(c, args[j].key_spec_index);
        }
        if (args[j].token) {
            addReplyBulkCString(c, "token");
            addReplyBulkCString(c, args[j].token);
        }
        if (args[j].summary) {
            addReplyBulkCString(c, "summary");
            addReplyBulkCString(c, args[j].summary);
        }
        if (args[j].since) {
            addReplyBulkCString(c, "since");
            addReplyBulkCString(c, args[j].since);
        }
        if (args[j].deprecated_since) {
            addReplyBulkCString(c, "deprecated_since");
            addReplyBulkCString(c, args[j].deprecated_since);
        }
        if (args[j].flags) {
            addReplyBulkCString(c, "flags");
            addReplyFlagsForArg(c, args[j].flags);
        }
        if (args[j].type == ARG_TYPE_ONEOF || args[j].type == ARG_TYPE_BLOCK) {
            addReplyBulkCString(c, "arguments");
            addReplyCommandArgList(c, args[j].subargs, args[j].num_args);
        }
    }
}

const char *RESP2_TYPE_STR[] = {
    "simple-string", "error", "integer", "bulk-string", "null-bulk-string", "array", "null-array",
};

const char *RESP3_TYPE_STR[] = {
    "simple-string", "error", "integer", "double", "bulk-string", "array", "map", "set", "bool", "null",
};

void addReplyCommandHistory(client *c, struct redisCommand *cmd) {
    addReplySetLen(c, cmd->num_history);
    for (int j = 0; j < cmd->num_history; j++) {
        addReplyArrayLen(c, 2);
        addReplyBulkCString(c, cmd->history[j].since);
        addReplyBulkCString(c, cmd->history[j].changes);
    }
}

void addReplyCommandTips(client *c, struct redisCommand *cmd) {
    addReplySetLen(c, cmd->num_tips);
    for (int j = 0; j < cmd->num_tips; j++) {
        addReplyBulkCString(c, cmd->tips[j]);
    }
}

void addReplyCommandKeySpecs(client *c, struct redisCommand *cmd) {
    addReplySetLen(c, cmd->key_specs_num);
    for (int i = 0; i < cmd->key_specs_num; i++) {
        int maplen = 3;
        if (cmd->key_specs[i].notes)
            maplen++;

        addReplyMapLen(c, maplen);

        if (cmd->key_specs[i].notes) {
            addReplyBulkCString(c, "notes");
            addReplyBulkCString(c, cmd->key_specs[i].notes);
        }

        addReplyBulkCString(c, "flags");
        addReplyFlagsForKeyArgs(c, cmd->key_specs[i].flags);

        addReplyBulkCString(c, "begin_search");
        switch (cmd->key_specs[i].begin_search_type) {
            case KSPEC_BS_UNKNOWN:
                addReplyMapLen(c, 2);
                addReplyBulkCString(c, "type");
                addReplyBulkCString(c, "unknown");

                addReplyBulkCString(c, "spec");
                addReplyMapLen(c, 0);
                break;
            case KSPEC_BS_INDEX:
                addReplyMapLen(c, 2);
                addReplyBulkCString(c, "type");
                addReplyBulkCString(c, "index");

                addReplyBulkCString(c, "spec");
                addReplyMapLen(c, 1);
                addReplyBulkCString(c, "index");
                addReplyLongLong(c, cmd->key_specs[i].bs.index.pos);
                break;
            case KSPEC_BS_KEYWORD:
                addReplyMapLen(c, 2);
                addReplyBulkCString(c, "type");
                addReplyBulkCString(c, "keyword");

                addReplyBulkCString(c, "spec");
                addReplyMapLen(c, 2);
                addReplyBulkCString(c, "keyword");
                addReplyBulkCString(c, cmd->key_specs[i].bs.keyword.keyword);
                addReplyBulkCString(c, "startfrom");
                addReplyLongLong(c, cmd->key_specs[i].bs.keyword.startfrom);
                break;
            default:
                serverPanic("Invalid begin_search key spec type %d", cmd->key_specs[i].begin_search_type);
        }

        addReplyBulkCString(c, "find_keys");
        switch (cmd->key_specs[i].find_keys_type) {
            case KSPEC_FK_UNKNOWN:
                addReplyMapLen(c, 2);
                addReplyBulkCString(c, "type");
                addReplyBulkCString(c, "unknown");

                addReplyBulkCString(c, "spec");
                addReplyMapLen(c, 0);
                break;
            case KSPEC_FK_RANGE:
                addReplyMapLen(c, 2);
                addReplyBulkCString(c, "type");
                addReplyBulkCString(c, "range");

                addReplyBulkCString(c, "spec");
                addReplyMapLen(c, 3);
                addReplyBulkCString(c, "lastkey");
                addReplyLongLong(c, cmd->key_specs[i].fk.range.lastkey);
                addReplyBulkCString(c, "keystep");
                addReplyLongLong(c, cmd->key_specs[i].fk.range.keystep);
                addReplyBulkCString(c, "limit");
                addReplyLongLong(c, cmd->key_specs[i].fk.range.limit);
                break;
            case KSPEC_FK_KEYNUM:
                addReplyMapLen(c, 2);
                addReplyBulkCString(c, "type");
                addReplyBulkCString(c, "keynum");

                addReplyBulkCString(c, "spec");
                addReplyMapLen(c, 3);
                addReplyBulkCString(c, "keynumidx");
                addReplyLongLong(c, cmd->key_specs[i].fk.keynum.keynumidx);
                addReplyBulkCString(c, "firstkey");
                addReplyLongLong(c, cmd->key_specs[i].fk.keynum.firstkey);
                addReplyBulkCString(c, "keystep");
                addReplyLongLong(c, cmd->key_specs[i].fk.keynum.keystep);
                break;
            default:
                serverPanic("Invalid find_keys key spec type %d", cmd->key_specs[i].begin_search_type);
        }
    }
}

void addReplyCommandSubCommands(client *c, struct redisCommand *cmd, void (*reply_function)(client *, struct redisCommand *), int use_map) {
    if (!cmd->subcommands_dict) {
        addReplySetLen(c, 0);
        return;
    }

    if (use_map)
        addReplyMapLen(c, dictSize(cmd->subcommands_dict));
    else
        addReplyArrayLen(c, dictSize(cmd->subcommands_dict));
    dictEntry *de;
    dictIterator *di = dictGetSafeIterator(cmd->subcommands_dict);
    while ((de = dictNext(di)) != NULL) {
        struct redisCommand *sub = (struct redisCommand *)dictGetVal(de);
        if (use_map)
            addReplyBulkCBuffer(c, sub->fullname, sdslen(sub->fullname));
        reply_function(c, sub);
    }
    dictReleaseIterator(di);
}

// 命令属于哪个组
const char *COMMAND_GROUP_STR[] = {"generic", "string", "list", "set", "sorted-set", "hash", "pubsub", "transactions", "connection", "server", "scripting", "hyperloglog", "cluster", "sentinel", "geo", "stream", "bitmap", "module"};

// Redis命令的输出.用于COMMAND命令和命令信息.
void addReplyCommandInfo(client *c, struct redisCommand *cmd) {
    if (!cmd) {
        addReplyNull(c);
    }
    else {
        int firstkey = 0, lastkey = 0, keystep = 0;
        if (cmd->legacy_range_key_spec.begin_search_type != KSPEC_BS_INVALID) {
            firstkey = cmd->legacy_range_key_spec.bs.index.pos;
            lastkey = cmd->legacy_range_key_spec.fk.range.lastkey;
            if (lastkey >= 0)
                lastkey += firstkey;
            keystep = cmd->legacy_range_key_spec.fk.range.keystep;
        }

        addReplyArrayLen(c, 10); // 下边是10个
        addReplyBulkCBuffer(c, cmd->fullname, sdslen(cmd->fullname));
        addReplyLongLong(c, cmd->arity);
        addReplyFlagsForCommand(c, cmd);
        addReplyLongLong(c, firstkey);
        addReplyLongLong(c, lastkey);
        addReplyLongLong(c, keystep);
        addReplyCommandCategories(c, cmd);
        addReplyCommandTips(c, cmd);
        addReplyCommandKeySpecs(c, cmd);
        addReplyCommandSubCommands(c, cmd, addReplyCommandInfo, 0);
    }
}

// Redis命令的输出.由命令DOCS使用.
void addReplyCommandDocs(client *c, struct redisCommand *cmd) {
    // 计算我们的回复len,这样我们就不用使用延迟回复了.
    long maplen = 1;
    if (cmd->summary) // 简介
        maplen++;
    if (cmd->since) // 起始版本
        maplen++;
    if (cmd->flags & CMD_MODULE) // 模块命令
        maplen++;
    if (cmd->complexity) // 复杂描述
        maplen++;
    if (cmd->doc_flags) // 文档标志
        maplen++;
    if (cmd->deprecated_since) // 在哪个版本弃用
        maplen++;
    if (cmd->replaced_by) // 替代的命令
        maplen++;
    if (cmd->history) // 命令历史
        maplen++;
    if (cmd->args) // 参数数组
        maplen++;
    if (cmd->subcommands_dict)
        maplen++;
    addReplyMapLen(c, maplen); // 返回数组长度

    if (cmd->summary) {
        addReplyBulkCString(c, "summary");
        addReplyBulkCString(c, cmd->summary);
    }
    if (cmd->since) {
        addReplyBulkCString(c, "since");
        addReplyBulkCString(c, cmd->since);
    }

    // 总是有group,对于模块命令,组总是"module".
    addReplyBulkCString(c, "group");
    addReplyBulkCString(c, COMMAND_GROUP_STR[cmd->group]);

    if (cmd->complexity) {
        addReplyBulkCString(c, "complexity");
        addReplyBulkCString(c, cmd->complexity);
    }
    if (cmd->flags & CMD_MODULE) {
        addReplyBulkCString(c, "module");
        addReplyBulkCString(c, moduleNameFromCommand(cmd));
    }
    if (cmd->doc_flags) {
        addReplyBulkCString(c, "doc_flags");
        addReplyDocFlagsForCommand(c, cmd);
    }
    if (cmd->deprecated_since) {
        addReplyBulkCString(c, "deprecated_since");
        addReplyBulkCString(c, cmd->deprecated_since);
    }
    if (cmd->replaced_by) {
        addReplyBulkCString(c, "replaced_by");
        addReplyBulkCString(c, cmd->replaced_by);
    }
    if (cmd->history) {
        addReplyBulkCString(c, "history");
        addReplyCommandHistory(c, cmd);
    }
    if (cmd->args) {
        addReplyBulkCString(c, "arguments");
        addReplyCommandArgList(c, cmd->args, cmd->num_args);
    }
    if (cmd->subcommands_dict) {
        addReplyBulkCString(c, "subcommands");
        addReplyCommandSubCommands(c, cmd, addReplyCommandDocs, 1);
    }
}

void getKeysSubcommandImpl(client *c, int with_flags) {
    struct redisCommand *cmd = lookupCommand(c->argv + 2, c->argc - 2);
    getKeysResult result = GETKEYS_RESULT_INIT;
    int j;

    if (!cmd) {
        addReplyError(c, "Invalid command specified");
        return;
    }
    else if (cmd->getkeys_proc == NULL && cmd->key_specs_num == 0) {
        addReplyError(c, "The command has no key arguments");
        return;
    }
    else if ((cmd->arity > 0 && cmd->arity != c->argc - 2) || ((c->argc - 2) < -cmd->arity)) {
        addReplyError(c, "Invalid number of arguments specified for command");
        return;
    }

    if (!getKeysFromCommandWithSpecs(cmd, c->argv + 2, c->argc - 2, GET_KEYSPEC_DEFAULT, &result)) {
        if (cmd->flags & CMD_NO_MANDATORY_KEYS) {
            addReplyArrayLen(c, 0);
        }
        else {
            addReplyError(c, "Invalid arguments specified for command");
        }
    }
    else {
        addReplyArrayLen(c, result.numkeys);
        for (j = 0; j < result.numkeys; j++) {
            if (!with_flags) {
                addReplyBulk(c, c->argv[result.keys[j].pos + 2]);
            }
            else {
                addReplyArrayLen(c, 2);
                addReplyBulk(c, c->argv[result.keys[j].pos + 2]);
                addReplyFlagsForKeyArgs(c, result.keys[j].flags);
            }
        }
    }
    getKeysFreeResult(&result);
}

/* COMMAND GETKEYSANDFLAGS cmd arg1 arg2 ... */
void commandGetKeysAndFlagsCommand(client *c) {
    getKeysSubcommandImpl(c, 1);
}

/* COMMAND GETKEYS cmd arg1 arg2 ... */
void getKeysSubcommand(client *c) {
    getKeysSubcommandImpl(c, 0);
}

/* COMMAND (no args) */
void commandCommand(client *c) {
    dictIterator *di;
    dictEntry *de;

    addReplyArrayLen(c, dictSize(server.commands));
    di = dictGetIterator(server.commands);
    while ((de = dictNext(di)) != NULL) {
        addReplyCommandInfo(c, dictGetVal(de)); // COMMAND (no args)
    }
    dictReleaseIterator(di);
}

/* COMMAND COUNT */
void commandCountCommand(client *c) {
    addReplyLongLong(c, dictSize(server.commands));
}

typedef enum {
    COMMAND_LIST_FILTER_MODULE,
    COMMAND_LIST_FILTER_ACLCAT,
    COMMAND_LIST_FILTER_PATTERN,
} commandListFilterType;

typedef struct {
    commandListFilterType type;
    sds arg;
    struct {
        int valid;
        union
        {
            uint64_t aclcat;
            void *module_handle;
        } u;
    } cache;
} commandListFilter;

int shouldFilterFromCommandList(struct redisCommand *cmd, commandListFilter *filter) {
    switch (filter->type) {
        case (COMMAND_LIST_FILTER_MODULE):
            if (!filter->cache.valid) {
                filter->cache.u.module_handle = moduleGetHandleByName(filter->arg);
                filter->cache.valid = 1;
            }
            return !moduleIsModuleCommand(filter->cache.u.module_handle, cmd);
        case (COMMAND_LIST_FILTER_ACLCAT): {
            if (!filter->cache.valid) {
                filter->cache.u.aclcat = ACLGetCommandCategoryFlagByName(filter->arg);
                filter->cache.valid = 1;
            }
            uint64_t cat = filter->cache.u.aclcat;
            if (cat == 0)
                return 1; /* Invalid ACL category */
            return (!(cmd->acl_categories & cat));
            break;
        }
        case (COMMAND_LIST_FILTER_PATTERN):
            return !stringmatchlen(filter->arg, sdslen(filter->arg), cmd->fullname, sdslen(cmd->fullname), 1);
        default:
            serverPanic("Invalid filter type %d", filter->type);
    }
}

/* COMMAND LIST FILTERBY (MODULE <module-name>|ACLCAT <cat>|PATTERN <pattern>) */
void commandListWithFilter(client *c, dict *commands, commandListFilter filter, int *numcmds) {
    dictEntry *de;
    dictIterator *di = dictGetIterator(commands);

    while ((de = dictNext(di)) != NULL) {
        struct redisCommand *cmd = dictGetVal(de);
        if (!shouldFilterFromCommandList(cmd, &filter)) {
            addReplyBulkCBuffer(c, cmd->fullname, sdslen(cmd->fullname));
            (*numcmds)++;
        }

        if (cmd->subcommands_dict) {
            commandListWithFilter(c, cmd->subcommands_dict, filter, numcmds);
        }
    }
    dictReleaseIterator(di);
}

/* COMMAND LIST */
void commandListWithoutFilter(client *c, dict *commands, int *numcmds) {
    dictEntry *de;
    dictIterator *di = dictGetIterator(commands);

    while ((de = dictNext(di)) != NULL) {
        struct redisCommand *cmd = dictGetVal(de);
        addReplyBulkCBuffer(c, cmd->fullname, sdslen(cmd->fullname));
        (*numcmds)++;

        if (cmd->subcommands_dict) {
            commandListWithoutFilter(c, cmd->subcommands_dict, numcmds);
        }
    }
    dictReleaseIterator(di);
}

/* COMMAND LIST [FILTERBY (MODULE <module-name>|ACLCAT <cat>|PATTERN <pattern>)] */
void commandListCommand(client *c) {
    /* Parse options. */
    int i = 2, got_filter = 0;
    commandListFilter filter = {0};
    for (; i < c->argc; i++) {
        int moreargs = (c->argc - 1) - i; /* Number of additional arguments. */
        char *opt = c->argv[i]->ptr;
        if (!strcasecmp(opt, "filterby") && moreargs == 2) {
            char *filtertype = c->argv[i + 1]->ptr;
            if (!strcasecmp(filtertype, "module")) {
                filter.type = COMMAND_LIST_FILTER_MODULE;
            }
            else if (!strcasecmp(filtertype, "aclcat")) {
                filter.type = COMMAND_LIST_FILTER_ACLCAT;
            }
            else if (!strcasecmp(filtertype, "pattern")) {
                filter.type = COMMAND_LIST_FILTER_PATTERN;
            }
            else {
                addReplyErrorObject(c, shared.syntaxerr);
                return;
            }
            got_filter = 1;
            filter.arg = c->argv[i + 2]->ptr;
            i += 2;
        }
        else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
    }

    int numcmds = 0;
    void *replylen = addReplyDeferredLen(c);

    if (got_filter) {
        commandListWithFilter(c, server.commands, filter, &numcmds);
    }
    else {
        commandListWithoutFilter(c, server.commands, &numcmds);
    }

    setDeferredArrayLen(c, replylen, numcmds);
}

/* COMMAND INFO [<command-name> ...] */
void commandInfoCommand(client *c) {
    int i;

    if (c->argc == 2) {
        dictIterator *di;
        dictEntry *de;
        addReplyArrayLen(c, dictSize(server.commands));
        di = dictGetIterator(server.commands);
        while ((de = dictNext(di)) != NULL) {
            addReplyCommandInfo(c, dictGetVal(de)); // COMMAND INFO [<command-name> ...]
        }
        dictReleaseIterator(di);
    }
    else {
        addReplyArrayLen(c, c->argc - 2);
        for (i = 2; i < c->argc; i++) {
            addReplyCommandInfo(c, lookupCommandBySds(c->argv[i]->ptr)); // COMMAND INFO [<command-name> ...]
        }
    }
}

/* COMMAND DOCS [<command-name> ...] */
void commandDocsCommand(client *c) {
    int i;
    if (c->argc == 2) {
        // 以数组的形式返回所有命令
        dictIterator *di;
        dictEntry *de;
        addReplyMapLen(c, dictSize(server.commands)); // 先返回 *480\r\n
        di = dictGetIterator(server.commands);
        while ((de = dictNext(di)) != NULL) {
            struct redisCommand *cmd = dictGetVal(de);
            addReplyBulkCBuffer(c, cmd->fullname, sdslen(cmd->fullname));
            addReplyCommandDocs(c, cmd);
        }
        dictReleaseIterator(di);
    }
    else {
        // 使用请求命令的数组进行回复(如果我们找到它们)
        int numcmds = 0;
        void *replylen = addReplyDeferredLen(c); // 使用空节点 占位  server.reply
        for (i = 2; i < c->argc; i++) {
            struct redisCommand *cmd = lookupCommandBySds(c->argv[i]->ptr);
            if (!cmd) {
                continue;
            }
            addReplyBulkCBuffer(c, cmd->fullname, sdslen(cmd->fullname));
            addReplyCommandDocs(c, cmd);
            numcmds++;
        }
        setDeferredMapLen(c, replylen, numcmds);
    }
}

/* COMMAND GETKEYS arg0 arg1 arg2 ... */
void commandGetKeysCommand(client *c) {
    getKeysSubcommand(c);
}

// command help
void commandHelpCommand(client *c) {
    const char *help[] = {
        "(no subcommand)",
        "    返回所有Redis命令的详细信息.",
        "COUNT",
        "    返回Redis服务器中命令的总数.",
        "LIST",
        "    返回Redis服务器中所有命令的列表.",
        "INFO [<command-name> ...]",
        "    返回多个Redis命令详情.如果没有给出命令名称,则返回所有命令的文档详细信息.",
        "DOCS [<command-name> ...]",
        "    返回多个Redis命令的详细文档[更加详细].如果没有给出命令名称,则返回所有命令的文档详细信息.",
        "GETKEYS <full-command>",
        "    从一个完整的Redis命令中返回对应的key.",
        "GETKEYSANDFLAGS <full-command>",
        "    从一个完整的Redis命令中返回key和访问标志.",
        NULL};

    addReplyHelp(c, help);
}

void bytesToHuman(char *s, unsigned long long n) {
    double d;

    if (n < 1024) {
        /* Bytes */
        sprintf(s, "%lluB", n);
    }
    else if (n < (1024 * 1024)) {
        d = (double)n / (1024);
        sprintf(s, "%.2fK", d);
    }
    else if (n < (1024LL * 1024 * 1024)) {
        d = (double)n / (1024 * 1024);
        sprintf(s, "%.2fM", d);
    }
    else if (n < (1024LL * 1024 * 1024 * 1024)) {
        d = (double)n / (1024LL * 1024 * 1024);
        sprintf(s, "%.2fG", d);
    }
    else if (n < (1024LL * 1024 * 1024 * 1024 * 1024)) {
        d = (double)n / (1024LL * 1024 * 1024 * 1024);
        sprintf(s, "%.2fT", d);
    }
    else if (n < (1024LL * 1024 * 1024 * 1024 * 1024 * 1024)) {
        d = (double)n / (1024LL * 1024 * 1024 * 1024 * 1024);
        sprintf(s, "%.2fP", d);
    }
    else {
        /* Let's hope we never need this */
        sprintf(s, "%lluB", n);
    }
}

// 填充百分比分布延迟
sds fillPercentileDistributionLatencies(sds info, const char *histogram_name, struct hdr_histogram *histogram) {
    info = sdscatfmt(info, "latency_percentiles_usec_%s:", histogram_name);
    for (int j = 0; j < server.latency_tracking_info_percentiles_len; j++) {
        char fbuf[128];
        size_t len = sprintf(fbuf, "%f", server.latency_tracking_info_percentiles[j]);
        len = trimDoubleString(fbuf, len);
        info = sdscatprintf(info, "p%s=%.3f", fbuf, ((double)hdr_value_at_percentile(histogram, server.latency_tracking_info_percentiles[j])) / 1000.0f);
        if (j != server.latency_tracking_info_percentiles_len - 1)
            info = sdscatlen(info, ",", 1);
    }
    info = sdscatprintf(info, "\r\n");
    return info;
}

/* 我们对INFO输出进行净化以保持预期格式的字符。*/
static char unsafe_info_chars[] = "#:\n\r";
static char unsafe_info_chars_substs[] = "____"; /* Must be same length as above */

const char *getSafeInfoString(const char *s, size_t len, char **tmp) {
    *tmp = NULL;
    if (mempbrk(s, len, unsafe_info_chars, sizeof(unsafe_info_chars) - 1) == NULL)
        return s;
    char *new = *tmp = zmalloc(len + 1);
    memcpy(new, s, len);
    new[len] = '\0';
    return memmapchars(new, len, unsafe_info_chars, unsafe_info_chars_substs, sizeof(unsafe_info_chars) - 1);
}

sds genRedisInfoStringCommandStats(sds info, dict *commands) {
    struct redisCommand *c;
    dictEntry *de;
    dictIterator *di;
    di = dictGetSafeIterator(commands);
    while ((de = dictNext(di)) != NULL) {
        char *tmpsafe;
        c = (struct redisCommand *)dictGetVal(de);
        if (c->calls || c->failed_calls || c->rejected_calls) {
            info = sdscatprintf(
                info,                                                                                           //
                "cmdstat_%s:calls=%lld,usec=%lld,usec_per_call=%.2f,rejected_calls=%lld,failed_calls=%lld\r\n", //
                getSafeInfoString(
                    c->fullname, sdslen(c->fullname),
                    &tmpsafe),                                             // 具体命令
                c->calls,                                                  // 调用次数
                c->microseconds,                                           // 耗费CPU时间
                (c->calls == 0) ? 0 : ((float)c->microseconds / c->calls), // 每个命令平均耗费的CPU(单位为微妙)
                c->rejected_calls,                                         //
                c->failed_calls                                            //
            );
            if (tmpsafe != NULL) {
                zfree(tmpsafe);
            }
        }
        if (c->subcommands_dict) {
            info = genRedisInfoStringCommandStats(info, c->subcommands_dict);
        }
    }
    dictReleaseIterator(di);

    return info;
}

sds genRedisInfoStringLatencyStats(sds info, dict *commands) {
    struct redisCommand *c;
    dictEntry *de;
    dictIterator *di;
    di = dictGetSafeIterator(commands);
    while ((de = dictNext(di)) != NULL) {
        char *tmpsafe;
        c = (struct redisCommand *)dictGetVal(de);
        if (c->latency_histogram) {
            info = fillPercentileDistributionLatencies(info, getSafeInfoString(c->fullname, sdslen(c->fullname), &tmpsafe), c->latency_histogram);
            if (tmpsafe != NULL)
                zfree(tmpsafe);
        }
        if (c->subcommands_dict) {
            info = genRedisInfoStringLatencyStats(info, c->subcommands_dict);
        }
    }
    dictReleaseIterator(di);

    return info;
}

void addInfoSectionsToDict(dict *section_dict, char **sections) {
    while (*sections) {
        sds section = sdsnew(*sections);
        if (dictAdd(section_dict, section, NULL) == DICT_ERR)
            sdsfree(section);
        sections++;
    }
}

static dict *cached_default_info_sections = NULL;

void releaseInfoSectionDict(dict *sec) {
    if (sec != cached_default_info_sections)
        dictRelease(sec);
}

/* Create a dictionary with unique section names to be used by genRedisInfoString.
 * 'argv' and 'argc' are list of arguments for INFO.
 * 'defaults' is an optional null terminated list of default sections.
 * 'out_all' and 'out_everything' are optional.
 * The resulting dictionary should be released with releaseInfoSectionDict. */
dict *genInfoSectionDict(robj **argv, int argc, char **defaults, int *out_all, int *out_everything) {
    char *default_sections[] = {"server", "clients", "memory", "persistence", "stats", "replication", "cpu", "module_list", "errorstats", "cluster", "keyspace", NULL};
    if (!defaults)
        defaults = default_sections;

    if (argc == 0) {
        /* In this case we know the dict is not gonna be modified, so we cache
         * it as an optimization for a common case. */
        if (cached_default_info_sections)
            return cached_default_info_sections;
        cached_default_info_sections = dictCreate(&stringSetDictType);
        dictExpand(cached_default_info_sections, 16);
        addInfoSectionsToDict(cached_default_info_sections, defaults);
        return cached_default_info_sections;
    }

    dict *section_dict = dictCreate(&stringSetDictType);
    dictExpand(section_dict, min(argc, 16));
    for (int i = 0; i < argc; i++) {
        if (!strcasecmp(argv[i]->ptr, "default")) {
            addInfoSectionsToDict(section_dict, defaults);
        }
        else if (!strcasecmp(argv[i]->ptr, "all")) {
            if (out_all)
                *out_all = 1;
        }
        else if (!strcasecmp(argv[i]->ptr, "everything")) {
            if (out_everything)
                *out_everything = 1;
            if (out_all)
                *out_all = 1;
        }
        else {
            sds section = sdsnew(argv[i]->ptr);
            if (dictAdd(section_dict, section, NULL) != DICT_OK)
                sdsfree(section);
        }
    }
    return section_dict;
}

sds genRedisInfoString(dict *section_dict, int all_sections, int everything) {
    sds info = sdsempty();
    time_t uptime = server.unixtime - server.stat_starttime;
    int j;
    int sections = 0;
    if (everything)
        all_sections = 1;

    /* Server */
    if (all_sections || (dictFind(section_dict, "server") != NULL)) {
        static int call_uname = 1;
        static struct utsname name;
        char *mode;
        char *supervised;

        if (server.cluster_enabled)
            mode = "cluster";
        else if (server.sentinel_mode)
            mode = "sentinel";
        else
            mode = "standalone";

        if (server.supervised) {
            if (server.supervised_mode == SUPERVISED_UPSTART)
                supervised = "upstart";
            else if (server.supervised_mode == SUPERVISED_SYSTEMD)
                supervised = "systemd";
            else
                supervised = "unknown";
        }
        else {
            supervised = "no";
        }

        if (sections++)
            info = sdscat(info, "\r\n");

        if (call_uname) {
            /* Uname can be slow and is always the same output. Cache it. */
            uname(&name);
            call_uname = 0;
        }

        unsigned int lruclock;
        atomicGet(server.lru_clock, lruclock); // 初始化 LRU 时间
        info = sdscatfmt(
            info,
            "# Server\r\n"
            "redis_version:%s\r\n"
            "redis_git_sha1:%s\r\n"
            "redis_git_dirty:%i\r\n"
            "redis_build_id:%s\r\n"
            "redis_mode:%s\r\n"
            "os:%s %s %s\r\n"
            "arch_bits:%i\r\n"
            "#单调时钟\r\n"
            "monotonic_clock:%s\r\n"
            "#多路复用\r\n"
            "multiplexing_api:%s\r\n"
            "atomicvar_api:%s\r\n"
            "gcc_version:%i.%i.%i\r\n"
            "process_id:%I\r\n"
            "process_supervised:%s\r\n"
            "run_id:%s\r\n"
            "tcp_port:%i\r\n"
            "server_time_usec:%I\r\n"
            "#自Redis服务器启动以来的秒数\r\n"
            "uptime_in_seconds:%I\r\n"
            "uptime_in_days:%I\r\n"
            "hz:%i\r\n"
            "configured_hz:%i\r\n"
            "lru_clock:%u\r\n"
            "executable:%s\r\n"
            "config_file:%s\r\n"
            "io_threads_active:%i\r\n",
            REDIS_VERSION, redisGitSHA1(), strtol(redisGitDirty(), NULL, 10) > 0, redisBuildIdString(), mode, name.sysname, name.release, name.machine, server.arch_bits, monotonicInfoString(), aeGetApiName(), REDIS_ATOMIC_API,
#ifdef __GNUC__
            __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__,
#else
            0, 0, 0,
#endif
            (int64_t)getpid(), supervised,               //
            server.runid,                                //
            server.port ? server.port : server.tls_port, //
            (int64_t)server.ustime,                      //
            (int64_t)uptime,                             //
            (int64_t)(uptime / (3600 * 24)),             //
            server.hz,                                   //
            server.config_hz,                            //
            lruclock,                                    //
            server.executable ? server.executable : "",  //
            server.configfile ? server.configfile : "",  //
            server.io_threads_active                     //
        );                                               //

        /* Conditional properties */
        if (isShutdownInitiated()) {
            info = sdscatfmt(info, "shutdown_in_milliseconds:%I\r\n", (int64_t)(server.shutdown_mstime - server.mstime));
        }
    }

    /* Clients */
    if (all_sections || (dictFind(section_dict, "clients") != NULL)) {
        size_t maxin, maxout;
        getExpansiveClientsInfo(&maxin, &maxout);
        if (sections++)
            info = sdscat(info, "\r\n");

        info = sdscatprintf(info, "# Clients\r\n");
        info = sdscatprintf(info, "# 客户端连接数（不包括来自副本的连接）\r\n");
        info = sdscatprintf(info, "connected_clients:%lu\r\n", listLength(server.clients) - listLength(server.slaves));
        info = sdscatprintf(info, "# 集群总线使用的套接字数量的近似值\r\n");
        info = sdscatprintf(info, "cluster_connections:%lu\r\n", getClusterConnectionsCount());
        info = sdscatprintf(info, "# connected_clients，connected_slaves和 cluster_connections的总和\r\n");
        info = sdscatprintf(info, "maxclients:%u\r\n", server.maxclients);
        info = sdscatprintf(info, "client_recent_max_input_buffer:%zu\r\n", maxin);
        info = sdscatprintf(info, "client_recent_max_output_buffer:%zu\r\n", maxout);
        info = sdscatprintf(info, "# 由于BLPOP,BRPOP,BRPOPLPUSH而被阻塞的客户端\r\n");
        info = sdscatprintf(info, "blocked_clients:%d\r\n", server.blocked_clients);
        info = sdscatprintf(info, "tracking_clients:%d\r\n", server.tracking_clients);
        info = sdscatprintf(info, "clients_in_timeout_table:%llu\r\n", (unsigned long long)raxSize(server.clients_timeout_table));
    }

    /* Memory */
    if (all_sections || (dictFind(section_dict, "memory") != NULL)) {
        char hmem[64];
        char peak_hmem[64];
        char total_system_hmem[64];
        char used_memory_lua_hmem[64];
        char used_memory_vm_total_hmem[64];
        char used_memory_scripts_hmem[64];
        char used_memory_rss_hmem[64];
        char maxmemory_hmem[64];
        size_t zmalloc_used = zmalloc_used_memory();
        size_t total_system_mem = server.system_memory_size;
        const char *evict_policy = evictPolicyToString();
        long long memory_lua = evalMemory();
        long long memory_functions = functionsMemory();
        struct redisMemOverhead *mh = getMemoryOverheadData();

        /* Peak memory is updated from time to time by serverCron() so it
         * may happen that the instantaneous value is slightly bigger than
         * the peak value. This may confuse users, so we update the peak
         * if found smaller than the current memory usage. */
        if (zmalloc_used > server.stat_peak_memory)
            server.stat_peak_memory = zmalloc_used;

        bytesToHuman(hmem, zmalloc_used);
        bytesToHuman(peak_hmem, server.stat_peak_memory);
        bytesToHuman(total_system_hmem, total_system_mem);
        bytesToHuman(used_memory_lua_hmem, memory_lua);
        bytesToHuman(used_memory_vm_total_hmem, memory_functions + memory_lua);
        bytesToHuman(used_memory_scripts_hmem, mh->lua_caches + mh->functions_caches);
        bytesToHuman(used_memory_rss_hmem, server.cron_malloc_stats.process_rss);
        bytesToHuman(maxmemory_hmem, server.maxmemory);

        if (sections++) {
            info = sdscat(info, "\r\n");
        }

        info = sdscatprintf(info, "# Memory\r\n");

        info = sdscatprintf(info, "# 向操作系统申请的内存,字节\r\n");
        info = sdscatprintf(info, "used_memory:%zu\r\n", zmalloc_used);
        info = sdscatprintf(info, "# 向操作系统申请的内存,M\r\n");
        info = sdscatprintf(info, "used_memory_human:%s\r\n", hmem);
        info = sdscatprintf(info, "# 操作系统实际分配的内存,:字节\r\n");
        info = sdscatprintf(info, "used_memory_rss:%zu\r\n", server.cron_malloc_stats.process_rss);
        info = sdscatprintf(info, "# 操作系统实际分配的内存,:M\r\n");
        info = sdscatprintf(info, "used_memory_rss_human:%s\r\n", used_memory_rss_hmem);
        info = sdscatprintf(info, "# Redis消耗的峰值内存,:字节\r\n");
        info = sdscatprintf(info, "used_memory_peak:%zu\r\n", server.stat_peak_memory);
        info = sdscatprintf(info, "used_memory_peak_human:%s\r\n", peak_hmem);
        info = sdscatprintf(info, "# used_memory_peak/used_memory\r\n");
        info = sdscatprintf(info, "used_memory_peak_perc:%.2f%%\r\n", mh->peak_perc);
        info = sdscatprintf(info, "used_memory_overhead:%zu\r\n", mh->overhead_total);
        info = sdscatprintf(info, "used_memory_startup:%zu\r\n", mh->startup_allocated);
        info = sdscatprintf(info, "used_memory_dataset:%zu\r\n", mh->dataset);
        info = sdscatprintf(info, "used_memory_dataset_perc:%.2f%%\r\n", mh->dataset_perc);
        info = sdscatprintf(info, "allocator_allocated:%zu\r\n", server.cron_malloc_stats.allocator_allocated);
        info = sdscatprintf(info, "allocator_active:%zu\r\n", server.cron_malloc_stats.allocator_active);
        info = sdscatprintf(info, "allocator_resident:%zu\r\n", server.cron_malloc_stats.allocator_resident);
        info = sdscatprintf(info, "total_system_memory:%lu\r\n", (unsigned long)total_system_mem);
        info = sdscatprintf(info, "total_system_memory_human:%s\r\n", total_system_hmem);
        info = sdscatprintf(info, "used_memory_lua:%lld\r\n", memory_lua);
        info = sdscatprintf(info, "used_memory_vm_eval:%lld\r\n", memory_lua);
        info = sdscatprintf(info, "used_memory_lua_human:%s\r\n", used_memory_lua_hmem);
        info = sdscatprintf(info, "used_memory_scripts_eval:%lld\r\n", (long long)mh->lua_caches);
        info = sdscatprintf(info, "number_of_cached_scripts:%lu\r\n", dictSize(evalScriptsDict()));
        info = sdscatprintf(info, "number_of_functions:%lu\r\n", functionsNum());
        info = sdscatprintf(info, "number_of_libraries:%lu\r\n", functionsLibNum());
        info = sdscatprintf(info, "used_memory_vm_functions:%lld\r\n", memory_functions);
        info = sdscatprintf(info, "used_memory_vm_total:%lld\r\n", memory_functions + memory_lua);
        info = sdscatprintf(info, "used_memory_vm_total_human:%s\r\n", used_memory_vm_total_hmem);
        info = sdscatprintf(info, "used_memory_functions:%lld\r\n", (long long)mh->functions_caches);
        info = sdscatprintf(info, "used_memory_scripts:%lld\r\n", (long long)mh->lua_caches + (long long)mh->functions_caches);
        info = sdscatprintf(info, "used_memory_scripts_human:%s\r\n", used_memory_scripts_hmem);
        info = sdscatprintf(info, "maxmemory:%lld\r\n", server.maxmemory);
        info = sdscatprintf(info, "maxmemory_human:%s\r\n", maxmemory_hmem);
        info = sdscatprintf(info, "maxmemory_policy:%s\r\n", evict_policy);
        info = sdscatprintf(info, "allocator_frag_ratio:%.2f\r\n", mh->allocator_frag); // 当前的内存碎片率 ~1[swap] 1~1.5  1.5~
        info = sdscatprintf(info, "allocator_frag_bytes:%zu\r\n", mh->allocator_frag_bytes);
        info = sdscatprintf(info, "allocator_rss_ratio:%.2f\r\n", mh->allocator_rss);
        info = sdscatprintf(info, "allocator_rss_bytes:%zd\r\n", mh->allocator_rss_bytes);
        info = sdscatprintf(info, "rss_overhead_ratio:%.2f\r\n", mh->rss_extra);
        info = sdscatprintf(info, "rss_overhead_bytes:%zd\r\n", mh->rss_extra_bytes);
        info = sdscatprintf(info, "# 内存碎片率\r\n");
        info = sdscatprintf(info, "mem_fragmentation_ratio:%.2f\r\n", mh->total_frag);
        info = sdscatprintf(info, "mem_fragmentation_bytes:%zd\r\n", mh->total_frag_bytes);
        info = sdscatprintf(info, "mem_not_counted_for_evict:%zu\r\n", freeMemoryGetNotCountedMemory());
        info = sdscatprintf(info, "mem_replication_backlog:%zu\r\n", mh->repl_backlog);
        info = sdscatprintf(info, "mem_total_replication_buffers:%zu\r\n", server.repl_buffer_mem);
        info = sdscatprintf(info, "mem_clients_slaves:%zu\r\n", mh->clients_slaves);
        info = sdscatprintf(info, "mem_clients_normal:%zu\r\n", mh->clients_normal);
        info = sdscatprintf(info, "mem_cluster_links:%zu\r\n", mh->cluster_links);
        info = sdscatprintf(info, "mem_aof_buffer:%zu\r\n", mh->aof_buffer);
        info = sdscatprintf(info, "mem_allocator:%s\r\n", ZMALLOC_LIB);
        info = sdscatprintf(info, "active_defrag_running:%d\r\n", server.active_defrag_running);
        info = sdscatprintf(info, "lazyfree_pending_objects:%zu\r\n", lazyfreeGetPendingObjectsCount());
        info = sdscatprintf(info, "lazyfreed_objects:%zu\r\n", lazyfreeGetFreedObjectsCount());

        freeMemoryOverheadData(mh);
    }

    /* Persistence */
    if (all_sections || (dictFind(section_dict, "persistence") != NULL)) {
        if (sections++)
            info = sdscat(info, "\r\n");
        double fork_perc = 0;
        if (server.stat_module_progress) {
            fork_perc = server.stat_module_progress * 100;
        }
        else if (server.stat_current_save_keys_total) {
            fork_perc = ((double)server.stat_current_save_keys_processed / server.stat_current_save_keys_total) * 100;
        }
        int aof_bio_fsync_status;
        atomicGet(server.aof_bio_fsync_status, aof_bio_fsync_status);

        info = sdscatprintf(info, "# Persistence\r\n");
        info = sdscatprintf(info, "loading:%d\r\n", (int)(server.loading && !server.async_loading));
        info = sdscatprintf(info, "async_loading:%d\r\n", (int)server.async_loading);
        info = sdscatprintf(info, "current_cow_peak:%zu\r\n", server.stat_current_cow_peak);
        info = sdscatprintf(info, "current_cow_size:%zu\r\n", server.stat_current_cow_bytes);
        info = sdscatprintf(info, "current_cow_size_age:%lu\r\n", server.stat_current_cow_updated ? (unsigned long)elapsedMs(server.stat_current_cow_updated) / 1000 : 0);
        info = sdscatprintf(info, "current_fork_perc:%.2f\r\n", fork_perc);
        info = sdscatprintf(info, "current_save_keys_processed:%zu\r\n", server.stat_current_save_keys_processed);
        info = sdscatprintf(info, "current_save_keys_total:%zu\r\n", server.stat_current_save_keys_total);
        info = sdscatprintf(info, "# 自最后一次持久化以来数据库的更改数\r\n");
        info = sdscatprintf(info, "rdb_changes_since_last_save:%lld\r\n", server.dirty);
        info = sdscatprintf(info, "rdb_bgsave_in_progress:%d\r\n", server.child_type == CHILD_TYPE_RDB);
        info = sdscatprintf(info, "# 最后一次持久化保存磁盘的时间戳\r\n");
        info = sdscatprintf(info, "rdb_last_save_time:%jd\r\n", (intmax_t)server.lastsave);
        info = sdscatprintf(info, "rdb_last_bgsave_status:%s\r\n", (server.lastbgsave_status == C_OK) ? "ok" : "err");
        info = sdscatprintf(info, "rdb_last_bgsave_time_sec:%jd\r\n", (intmax_t)server.rdb_save_time_last);
        info = sdscatprintf(info, "rdb_current_bgsave_time_sec:%jd\r\n", (intmax_t)((server.child_type != CHILD_TYPE_RDB) ? -1 : time(NULL) - server.rdb_save_time_start));
        info = sdscatprintf(info, "rdb_saves:%lld\r\n", server.stat_rdb_saves);
        info = sdscatprintf(info, "rdb_last_cow_size:%zu\r\n", server.stat_rdb_cow_bytes);
        info = sdscatprintf(info, "rdb_last_load_keys_expired:%lld\r\n", server.rdb_last_load_keys_expired);
        info = sdscatprintf(info, "rdb_last_load_keys_loaded:%lld\r\n", server.rdb_last_load_keys_loaded);
        info = sdscatprintf(info, "aof_enabled:%d\r\n", server.aof_state != AOF_OFF);
        info = sdscatprintf(info, "aof_rewrite_in_progress:%d\r\n", server.child_type == CHILD_TYPE_AOF);
        info = sdscatprintf(info, "aof_rewrite_scheduled:%d\r\n", server.aof_rewrite_scheduled);
        info = sdscatprintf(info, "aof_last_rewrite_time_sec:%jd\r\n", (intmax_t)server.aof_rewrite_time_last);
        info = sdscatprintf(info, "aof_current_rewrite_time_sec:%jd\r\n", (intmax_t)((server.child_type != CHILD_TYPE_AOF) ? -1 : time(NULL) - server.aof_rewrite_time_start));
        info = sdscatprintf(info, "aof_last_bgrewrite_status:%s\r\n", (server.aof_lastbgrewrite_status == C_OK) ? "ok" : "err");
        info = sdscatprintf(info, "aof_rewrites:%lld\r\n", server.stat_aof_rewrites);
        info = sdscatprintf(info, "aof_rewrites_consecutive_failures:%lld\r\n", server.stat_aofrw_consecutive_failures);
        info = sdscatprintf(info, "aof_last_write_status:%s\r\n", (server.aof_last_write_status == C_OK && aof_bio_fsync_status == C_OK) ? "ok" : "err");
        info = sdscatprintf(info, "aof_last_cow_size:%zu\r\n", server.stat_aof_cow_bytes);
        info = sdscatprintf(info, "module_fork_in_progress:%d\r\n", server.child_type == CHILD_TYPE_MODULE);
        info = sdscatprintf(info, "module_fork_last_cow_size:%zu\r\n", server.stat_module_cow_bytes);

        if (server.aof_enabled) {
            info = sdscatprintf(info, "aof_current_size:%lld\r\n", (long long)server.aof_current_size);
            info = sdscatprintf(info, "aof_base_size:%lld\r\n", (long long)server.aof_rewrite_base_size);
            info = sdscatprintf(info, "aof_pending_rewrite:%d\r\n", server.aof_rewrite_scheduled);
            info = sdscatprintf(info, "aof_buffer_length:%zu\r\n", sdslen(server.aof_buf));
            info = sdscatprintf(info, "aof_pending_bio_fsync:%llu\r\n", bioPendingJobsOfType(BIO_AOF_FSYNC));
            info = sdscatprintf(info, "aof_delayed_fsync:%lu\r\n", server.aof_delayed_fsync);
        }

        if (server.loading) {
            double perc = 0;
            time_t eta, elapsed;
            off_t remaining_bytes = 1;

            if (server.loading_total_bytes) {
                perc = ((double)server.loading_loaded_bytes / server.loading_total_bytes) * 100;
                remaining_bytes = server.loading_total_bytes - server.loading_loaded_bytes;
            }
            else if (server.loading_rdb_used_mem) {
                perc = ((double)server.loading_loaded_bytes / server.loading_rdb_used_mem) * 100;
                remaining_bytes = server.loading_rdb_used_mem - server.loading_loaded_bytes;
                /* used mem is only a (bad) estimation of the rdb file size, avoid going over 100% */
                if (perc > 99.99)
                    perc = 99.99;
                if (remaining_bytes < 1)
                    remaining_bytes = 1;
            }

            elapsed = time(NULL) - server.loading_start_time;
            if (elapsed == 0) {
                eta = 1; /* A fake 1 second figure if we don't have
                            enough info */
            }
            else {
                eta = (elapsed * remaining_bytes) / (server.loading_loaded_bytes + 1);
            }

            info = sdscatprintf(info, "loading_start_time:%jd\r\n", (intmax_t)server.loading_start_time);
            info = sdscatprintf(info, "loading_total_bytes:%llu\r\n", (unsigned long long)server.loading_total_bytes);
            info = sdscatprintf(info, "loading_rdb_used_mem:%llu\r\n", (unsigned long long)server.loading_rdb_used_mem);
            info = sdscatprintf(info, "loading_loaded_bytes:%llu\r\n", (unsigned long long)server.loading_loaded_bytes);
            info = sdscatprintf(info, "loading_loaded_perc:%.2f\r\n", perc);
            info = sdscatprintf(info, "loading_eta_seconds:%jd\r\n", (intmax_t)eta);
        }
    }

    /* Stats */
    if (all_sections || (dictFind(section_dict, "stats") != NULL)) {
        long long stat_total_reads_processed, stat_total_writes_processed;
        long long stat_net_input_bytes, stat_net_output_bytes;
        long long current_eviction_exceeded_time = server.stat_last_eviction_exceeded_time ? (long long)elapsedUs(server.stat_last_eviction_exceeded_time) : 0;
        long long current_active_defrag_time = server.stat_last_active_defrag_time ? (long long)elapsedUs(server.stat_last_active_defrag_time) : 0;
        atomicGet(server.stat_total_reads_processed, stat_total_reads_processed);
        atomicGet(server.stat_total_writes_processed, stat_total_writes_processed);
        atomicGet(server.stat_net_input_bytes, stat_net_input_bytes);
        atomicGet(server.stat_net_output_bytes, stat_net_output_bytes);

        if (sections++) {
            info = sdscat(info, "\r\n");
        }

        info = sdscatprintf(info, "# Stats\r\n");
        info = sdscatprintf(info, "# 服务器接受的连接总数\r\n");
        info = sdscatprintf(info, "total_connections_received:%lld\r\n", server.stat_numconnections);
        info = sdscatprintf(info, "# 服务器处理的命令总数\r\n");
        info = sdscatprintf(info, "total_commands_processed:%lld\r\n", server.stat_numcommands);
        info = sdscatprintf(info, "# 平均每秒处理命令数\r\n");
        info = sdscatprintf(info, "instantaneous_ops_per_sec:%lld\r\n", getInstantaneousMetric(STATS_METRIC_COMMAND));
        info = sdscatprintf(info, "# 每秒网络的读取速率，以KB /秒为单位\r\n");
        info = sdscatprintf(info, "total_net_input_bytes:%lld\r\n", stat_net_input_bytes);
        info = sdscatprintf(info, "# 每秒的网络写入速率，以KB /秒为单位\r\n");
        info = sdscatprintf(info, "total_net_output_bytes:%lld\r\n", stat_net_output_bytes);
        info = sdscatprintf(info, "instantaneous_input_kbps:%.2f\r\n", (float)getInstantaneousMetric(STATS_METRIC_NET_INPUT) / 1024);
        info = sdscatprintf(info, "instantaneous_output_kbps:%.2f\r\n", (float)getInstantaneousMetric(STATS_METRIC_NET_OUTPUT) / 1024);
        info = sdscatprintf(info, "# 由于达到max client限制而被拒绝的连接数\r\n");
        info = sdscatprintf(info, "rejected_connections:%lld\r\n", server.stat_rejected_conn);
        info = sdscatprintf(info, "sync_full:%lld\r\n", server.stat_sync_full);
        info = sdscatprintf(info, "sync_partial_ok:%lld\r\n", server.stat_sync_partial_ok);
        // 通过查看sync_partial_err变量的次数来决定是否需要扩大积压缓冲区，它表示主从半同步复制失败的次数
        info = sdscatprintf(info, "# 主从半同步复制失败的次数\r\n");
        info = sdscatprintf(info, "sync_partial_err:%lld\r\n", server.stat_sync_partial_err);
        info = sdscatprintf(info, "# key过期事件总数\r\n");
        info = sdscatprintf(info, "expired_keys:%lld\r\n", server.stat_expiredkeys);
        info = sdscatprintf(info, "expired_stale_perc:%.2f\r\n", server.stat_expired_stale_perc * 100);
        info = sdscatprintf(info, "expired_time_cap_reached_count:%lld\r\n", server.stat_expired_time_cap_reached_count);
        info = sdscatprintf(info, "expire_cycle_cpu_milliseconds:%lld\r\n", server.stat_expire_cycle_time_used / 1000);
        info = sdscatprintf(info, "# 由于最大内存限制被移除的key的数量\r\n");
        info = sdscatprintf(info, "evicted_keys:%lld\r\n", server.stat_evictedkeys);
        info = sdscatprintf(info, "evicted_clients:%lld\r\n", server.stat_evictedclients);
        info = sdscatprintf(info, "total_eviction_exceeded_time:%lld\r\n", (server.stat_total_eviction_exceeded_time + current_eviction_exceeded_time) / 1000);
        info = sdscatprintf(info, "current_eviction_exceeded_time:%lld\r\n", current_eviction_exceeded_time / 1000);
        info = sdscatprintf(info, "# key值查找成功(命中)次数\r\n");
        info = sdscatprintf(info, "keyspace_hits:%lld\r\n", server.stat_keyspace_hits);
        info = sdscatprintf(info, "# key值查找失败(没有命中)次数\r\n");
        info = sdscatprintf(info, "keyspace_misses:%lld\r\n", server.stat_keyspace_misses);
        info = sdscatprintf(info, "pubsub_channels:%ld\r\n", dictSize(server.pubsub_channels));
        info = sdscatprintf(info, "pubsub_patterns:%lu\r\n", dictSize(server.pubsub_patterns));
        info = sdscatprintf(info, "latest_fork_usec:%lld\r\n", server.stat_fork_time);
        info = sdscatprintf(info, "total_forks:%lld\r\n", server.stat_total_forks);
        info = sdscatprintf(info, "migrate_cached_sockets:%ld\r\n", dictSize(server.migrate_cached_sockets));
        info = sdscatprintf(info, "slave_expires_tracked_keys:%zu\r\n", getSlaveKeyWithExpireCount());
        info = sdscatprintf(info, "active_defrag_hits:%lld\r\n", server.stat_active_defrag_hits);
        info = sdscatprintf(info, "active_defrag_misses:%lld\r\n", server.stat_active_defrag_misses);
        info = sdscatprintf(info, "active_defrag_key_hits:%lld\r\n", server.stat_active_defrag_key_hits);
        info = sdscatprintf(info, "active_defrag_key_misses:%lld\r\n", server.stat_active_defrag_key_misses);
        info = sdscatprintf(info, "total_active_defrag_time:%lld\r\n", (server.stat_total_active_defrag_time + current_active_defrag_time) / 1000);
        info = sdscatprintf(info, "current_active_defrag_time:%lld\r\n", current_active_defrag_time / 1000);
        info = sdscatprintf(info, "tracking_total_keys:%lld\r\n", (unsigned long long)trackingGetTotalKeys());
        info = sdscatprintf(info, "tracking_total_items:%lld\r\n", (unsigned long long)trackingGetTotalItems());
        info = sdscatprintf(info, "tracking_total_prefixes:%lld\r\n", (unsigned long long)trackingGetTotalPrefixes());
        info = sdscatprintf(info, "unexpected_error_replies:%lld\r\n", server.stat_unexpected_error_replies);
        info = sdscatprintf(info, "total_error_replies:%lld\r\n", server.stat_total_error_replies);
        info = sdscatprintf(info, "dump_payload_sanitizations:%lld\r\n", server.stat_dump_payload_sanitizations);
        info = sdscatprintf(info, "# 处理的读取事件总数\r\n");
        info = sdscatprintf(info, "total_reads_processed:%lld\r\n", stat_total_reads_processed);
        info = sdscatprintf(info, "# 处理的写入事件总数\r\n");
        info = sdscatprintf(info, "total_writes_processed:%lld\r\n", stat_total_writes_processed);
        info = sdscatprintf(info, "# 主线程和I / O线程处理的读取事件数\r\n");
        info = sdscatprintf(info, "io_threaded_reads_processed:%lld\r\n", server.stat_io_reads_processed);
        info = sdscatprintf(info, "# 主线程和I / O线程处理的写事件数\r\n");
        info = sdscatprintf(info, "io_threaded_writes_processed:%lld\r\n", server.stat_io_writes_processed);
        info = sdscatprintf(info, "reply_buffer_shrinks:%lld\r\n", server.stat_reply_buffer_shrinks);
        info = sdscatprintf(info, "reply_buffer_expands:%lld\r\n", server.stat_reply_buffer_expands);
    }

    /* Replication */
    if (all_sections || (dictFind(section_dict, "replication") != NULL)) {
        if (sections++)
            info = sdscat(info, "\r\n");
        info = sdscatprintf(
            info,
            "# Replication\r\n"
            "role:%s\r\n",
            server.masterhost == NULL ? "master" : "slave");
        if (server.masterhost) {
            long long slave_repl_offset = 1;
            long long slave_read_repl_offset = 1;

            if (server.master) {
                slave_repl_offset = server.master->reploff;
                slave_read_repl_offset = server.master->read_reploff;
            }
            else if (server.cached_master) {
                slave_repl_offset = server.cached_master->reploff;
                slave_read_repl_offset = server.cached_master->read_reploff;
            }

            info = sdscatprintf(info, "master_host:%s\r\n", server.masterhost);
            info = sdscatprintf(info, "master_port:%d\r\n", server.masterport);
            info = sdscatprintf(info, "master_link_status:%s\r\n", (server.repl_state == REPL_STATE_CONNECTED) ? "up" : "down");
            info = sdscatprintf(info, "# 最近一次主从交互之后的秒数\r\n");
            info = sdscatprintf(info, "master_last_io_seconds_ago:%d\r\n", server.master ? ((int)(server.unixtime - server.master->lastinteraction)) : -1);
            info = sdscatprintf(info, "master_sync_in_progress:%d\r\n", server.repl_state == REPL_STATE_TRANSFER);
            info = sdscatprintf(info, "slave_read_repl_offset:%lld\r\n", slave_read_repl_offset);
            info = sdscatprintf(info, "slave_repl_offset:%lld\r\n", slave_repl_offset);

            if (server.repl_state == REPL_STATE_TRANSFER) {
                double perc = 0;
                if (server.repl_transfer_size) {
                    perc = ((double)server.repl_transfer_read / server.repl_transfer_size) * 100;
                }
                info = sdscatprintf(info, "master_sync_total_bytes:%lld\r\n", (long long)server.repl_transfer_size);
                info = sdscatprintf(info, "master_sync_read_bytes:%lld\r\n", (long long)server.repl_transfer_read);
                info = sdscatprintf(info, "master_sync_left_bytes:%lld\r\n", (long long)(server.repl_transfer_size - server.repl_transfer_read));
                info = sdscatprintf(info, "master_sync_perc:%.2f\r\n", perc);
                info = sdscatprintf(info, "master_sync_last_io_seconds_ago:%d\r\n", (int)(server.unixtime - server.repl_transfer_lastio));
            }

            if (server.repl_state != REPL_STATE_CONNECTED) {
                info = sdscatprintf(info, "# 主从断开的持续时间（以秒为单位)\r\n");
                info = sdscatprintf(info, "master_link_down_since_seconds:%jd\r\n", server.repl_down_since ? (intmax_t)(server.unixtime - server.repl_down_since) : -1);
            }
            info = sdscatprintf(info, "slave_priority:%d\r\n", server.slave_priority);
            info = sdscatprintf(info, "slave_read_only:%d\r\n", server.repl_slave_ro);
            info = sdscatprintf(info, "replica_announced:%d\r\n", server.replica_announced);
        }
        info = sdscatprintf(info, "# slave连接数量\r\n");
        info = sdscatprintf(info, "connected_slaves:%lu\r\n", listLength(server.slaves));

        if (server.repl_min_slaves_to_write && server.repl_min_slaves_max_lag) {
            info = sdscatprintf(info, "# 状态良好的从节点数量\r\n");
            info = sdscatprintf(info, "min_slaves_good_slaves:%d\r\n", server.repl_good_slaves_count);
        }

        if (listLength(server.slaves)) {
            int slaveid = 0;
            listNode *ln;
            listIter li;

            listRewind(server.slaves, &li);
            while ((ln = listNext(&li))) {
                client *slave = listNodeValue(ln);
                char ip[NET_IP_STR_LEN], *slaveip = slave->slave_addr;
                int port;
                long lag = 0;

                if (!slaveip) {
                    if (connPeerToString(slave->conn, ip, sizeof(ip), &port) == -1)
                        continue;
                    slaveip = ip;
                }
                const char *state = replstateToString(slave->replstate);
                if (state[0] == '\0')
                    continue;
                if (slave->replstate == SLAVE_STATE_ONLINE)
                    lag = time(NULL) - slave->repl_ack_time;

                info = sdscatprintf(
                    info,
                    "slave%d:ip=%s,port=%d,state=%s,"
                    "offset=%lld,lag=%ld\r\n",
                    slaveid, slaveip, slave->slave_listening_port, state, slave->repl_ack_off, lag);
                slaveid++;
            }
        }
        info = sdscatprintf(info, "master_failover_state:%s\r\n", getFailoverStateString());
        info = sdscatprintf(info, "master_replid:%s\r\n", server.replid);
        info = sdscatprintf(info, "master_replid2:%s\r\n", server.replid2);
        info = sdscatprintf(info, "master_repl_offset:%lld\r\n", server.master_repl_offset);
        info = sdscatprintf(info, "second_repl_offset:%lld\r\n", server.second_replid_offset);
        info = sdscatprintf(info, "repl_backlog_active:%d\r\n", server.repl_backlog != NULL);
        // 复制积压缓冲区如果设置得太小，会导致里面的指令被覆盖掉找不到偏移量，从而触发全量同步
        info = sdscatprintf(info, "复制积压缓冲区\r\n");
        info = sdscatprintf(info, "repl_backlog_size:%lld\r\n", server.repl_backlog_size);
        info = sdscatprintf(info, "repl_backlog_first_byte_offset:%lld\r\n", server.repl_backlog ? server.repl_backlog->offset : 0);
        info = sdscatprintf(info, "repl_backlog_histlen:%lld\r\n", server.repl_backlog ? server.repl_backlog->histlen : 0);
    }

    /* CPU */
    if (all_sections || (dictFind(
                             section_dict, //
                             "cpu") != NULL)) {
        if (sections++)
            info = sdscat(
                info, //
                "\r\n");

        struct rusage self_ru, c_ru;
        getrusage(RUSAGE_SELF, &self_ru);
        getrusage(RUSAGE_CHILDREN, &c_ru);
        info = sdscatprintf(
            info,
            "# CPU\r\n"
            "used_cpu_sys:%ld.%06ld\r\n"
            "used_cpu_user:%ld.%06ld\r\n"
            "used_cpu_sys_children:%ld.%06ld\r\n"
            "used_cpu_user_children:%ld.%06ld\r\n",
            (long)self_ru.ru_stime.tv_sec, (long)self_ru.ru_stime.tv_usec, //
            (long)self_ru.ru_utime.tv_sec, (long)self_ru.ru_utime.tv_usec, //
            (long)c_ru.ru_stime.tv_sec, (long)c_ru.ru_stime.tv_usec,       //
            (long)c_ru.ru_utime.tv_sec, (long)c_ru.ru_utime.tv_usec        //
        );
#ifdef RUSAGE_THREAD
        struct rusage m_ru;
        getrusage(RUSAGE_THREAD, &m_ru);
        info = sdscatprintf(
            info,
            "used_cpu_sys_main_thread:%ld.%06ld\r\n"
            "used_cpu_user_main_thread:%ld.%06ld\r\n",
            (long)m_ru.ru_stime.tv_sec,  //
            (long)m_ru.ru_stime.tv_usec, //
            (long)m_ru.ru_utime.tv_sec,  //
            (long)m_ru.ru_utime.tv_usec);
#endif /* RUSAGE_THREAD */
    }

    /* Modules */
    if (all_sections || (dictFind(section_dict, "module_list") != NULL) || (dictFind(section_dict, "modules") != NULL)) {
        if (sections++)
            info = sdscat(info, "\r\n");
        info = sdscatprintf(info, "# Modules\r\n");
        info = genModulesInfoString(info);
    }

    /* Command statistics */
    if (all_sections || (dictFind(section_dict, "commandstats") != NULL)) {
        if (sections++)
            info = sdscat(info, "\r\n");
        info = sdscatprintf(info, "# Commandstats\r\n");
        info = genRedisInfoStringCommandStats(info, server.commands);
    }

    /* Error statistics */
    if (all_sections || (dictFind(section_dict, "errorstats") != NULL)) {
        if (sections++)
            info = sdscat(info, "\r\n");
        info = sdscat(info, "# Errorstats\r\n");
        raxIterator ri;
        raxStart(&ri, server.errors);
        raxSeek(&ri, "^", NULL, 0);
        struct redisError *e;
        while (raxNext(&ri)) {
            char *tmpsafe;
            e = (struct redisError *)ri.data;
            info = sdscatprintf(info, "errorstat_%.*s:count=%lld\r\n", (int)ri.key_len, getSafeInfoString((char *)ri.key, ri.key_len, &tmpsafe), e->count);
            if (tmpsafe != NULL)
                zfree(tmpsafe);
        }
        raxStop(&ri);
    }

    /* 按每个命令的百分比分布的延迟 */
    if (all_sections || (dictFind(section_dict, "latencystats") != NULL)) {
        if (sections++)
            info = sdscat(info, "\r\n");
        info = sdscatprintf(info, "# Latencystats\r\n");
        if (server.latency_tracking_enabled) {
            info = genRedisInfoStringLatencyStats(info, server.commands);
        }
    }

    /* Cluster */
    if (all_sections || (dictFind(section_dict, "cluster") != NULL)) {
        if (sections++)
            info = sdscat(info, "\r\n");
        info = sdscatprintf(
            info,
            "# Cluster\r\n"
            "cluster_enabled:%d\r\n",
            server.cluster_enabled);
    }

    /* Key space */
    if (all_sections || (dictFind(section_dict, "keyspace") != NULL)) {
        if (sections++) {
            info = sdscat(info, "\r\n");
        }
        info = sdscatprintf(info, "# 数据库中的key值总数\r\n");
        info = sdscatprintf(info, "# Keyspace\r\n");
        for (j = 0; j < server.dbnum; j++) {
            long long keys, vkeys;

            keys = dictSize(server.db[j].dict);
            vkeys = dictSize(server.db[j].expires);
            if (keys || vkeys) {
                info = sdscatprintf(info, "db%d:keys=%lld,expires=%lld,avg_ttl=%lld\r\n", j, keys, vkeys, server.db[j].avg_ttl);
            }
        }
    }

    /*从模块中获取信息。
如果用户要求“一切”或“模块”，或尚未找到的特定部分。
  */
    if (everything || dictFind(section_dict, "modules") != NULL || sections < (int)dictSize(section_dict)) {
        info = modulesCollectInfo(
            info, everything || dictFind(section_dict, "modules") != NULL ? NULL : section_dict, 0, /* not a crash report */
            sections);
    }
    return info;
}

void infoCommand(client *c) {
    if (server.sentinel_mode) {
        sentinelInfoCommand(c);
        return;
    }
    int all_sections = 0;
    int everything = 0;
    dict *sections_dict = genInfoSectionDict(c->argv + 1, c->argc - 1, NULL, &all_sections, &everything);
    sds info = genRedisInfoString(sections_dict, all_sections, everything);
    addReplyVerbatim(c, info, sdslen(info), "txt");
    sdsfree(info);
    releaseInfoSectionDict(sections_dict);
    return;
}

void monitorCommand(client *c) {
    if (c->flags & CLIENT_DENY_BLOCKING) {
        /** 具有CLIENT_DENY_BLOCKING标志的客户端期望每个命令都有应答，因此不能执行MONITOR。 * */
        addReplyError(c, "MONITOR不允许DENY BLOCKING客户端");
        return;
    }
    // 这个客户端是从服务器,或者已经是监视器
    if (c->flags & CLIENT_SLAVE)
        return;
    // 打开 SLAVE 标志和 MONITOR 标志
    c->flags |= (CLIENT_SLAVE | CLIENT_MONITOR);
    // 添加客户端到 monitors 链表
    listAddNodeTail(server.monitors, c);
    // 返回 OK
    addReply(c, shared.ok);
}

/* =================================== Main! ================================ */

int checkIgnoreWarning(const char *warning) {
    int argc, j;
    sds *argv = sdssplitargs(server.ignore_warnings, &argc);
    if (argv == NULL)
        return 0;

    for (j = 0; j < argc; j++) {
        char *flag = argv[j];
        if (!strcasecmp(flag, warning))
            break;
    }
    sdsfreesplitres(argv, argc);
    return j < argc;
}

#ifdef __linux__
int linuxOvercommitMemoryValue(void) {
    FILE *fp = fopen("/proc/sys/vm/overcommit_memory", "r");
    char buf[64];

    if (!fp)
        return -1;
    if (fgets(buf, 64, fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    return atoi(buf);
}
// 检查系统的THP和overcommit_memory
void linuxMemoryWarnings(void) {
    if (linuxOvercommitMemoryValue() == 0) {
        serverLog(LL_WARNING, "WARNING overcommit_memory is set to 0! Background save may fail under low memory condition. To fix this issue add 'vm.overcommit_memory = 1' to /etc/sysctl.conf and then reboot or run the command 'sysctl vm.overcommit_memory=1' for this to take effect.");
    }
    if (THPIsEnabled()) {
        server.thp_enabled = 1;
        if (THPDisable() == 0) {
            server.thp_enabled = 0;
            return;
        }
        serverLog(
            LL_WARNING,
            "WARNING you have Transparent Huge Pages (THP) support enabled in your kernel. This will create latency and memory usage issues with Redis. To fix this issue run the command 'echo madvise > /sys/kernel/mm/transparent_hugepage/enabled' as root, and add it to your /etc/rc.local in order to retain the setting after a reboot. Redis must be "
            "restarted after THP is disabled (set to 'madvise' or 'never').");
    }
}

#    ifdef __arm64__

/* Get size in kilobytes of the Shared_Dirty pages of the calling process for the
 * memory map corresponding to the provided address, or -1 on error. */
static int smapsGetSharedDirty(unsigned long addr) {
    int ret, in_mapping = 0, val = -1;
    unsigned long from, to;
    char buf[64];
    FILE *f;

    f = fopen("/proc/self/smaps", "r");
    if (!f)
        return -1;

    while (1) {
        if (!fgets(buf, sizeof(buf), f))
            break;

        ret = sscanf(buf, "%lx-%lx", &from, &to);
        if (ret == 2)
            in_mapping = from <= addr && addr < to;

        if (in_mapping && !memcmp(buf, "Shared_Dirty:", 13)) {
            sscanf(buf, "%*s %d", &val);
            /* If parsing fails, we remain with val == -1 */
            break;
        }
    }

    fclose(f);
    return val;
}

/* Older arm64 Linux kernels have a bug that could lead to data corruption
 * during background save in certain scenarios. This function checks if the
 * kernel is affected.
 * The bug was fixed in commit ff1712f953e27f0b0718762ec17d0adb15c9fd0b
 * titled: "arm64: pgtable: Ensure dirty bit is preserved across pte_wrprotect()"
 * Return -1 on unexpected test failure, 1 if the kernel seems to be affected,
 * and 0 otherwise. */
int linuxMadvFreeForkBugCheck(void) {
    int ret, pipefd[2] = {-1, -1};
    pid_t pid;
    char *p = NULL, *q;
    int bug_found = 0;
    long page_size = sysconf(_SC_PAGESIZE);
    long map_size = 3 * page_size;

    /* Create a memory map that's in our full control (not one used by the allocator). */
    p = mmap(NULL, map_size, PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) {
        serverLog(LL_WARNING, "Failed to mmap(): %s", strerror(errno));
        return -1;
    }

    q = p + page_size;

    /* Split the memory map in 3 pages by setting their protection as RO|RW|RO to prevent
     * Linux from merging this memory map with adjacent VMAs. */
    ret = mprotect(q, page_size, PROT_READ | PROT_WRITE);
    if (ret < 0) {
        serverLog(LL_WARNING, "Failed to mprotect(): %s", strerror(errno));
        bug_found = -1;
        goto exit;
    }

    /* Write to the page once to make it resident */
    *(volatile char *)q = 0;

    /* Tell the kernel that this page is free to be reclaimed. */
#        ifndef MADV_FREE
#            define MADV_FREE 8
#        endif
    ret = madvise(q, page_size, MADV_FREE);
    if (ret < 0) {
        /* MADV_FREE is not available on older kernels that are presumably
         * not affected. */
        if (errno == EINVAL)
            goto exit;

        serverLog(LL_WARNING, "Failed to madvise(): %s", strerror(errno));
        bug_found = -1;
        goto exit;
    }

    /* Write to the page after being marked for freeing, this is supposed to take
     * ownership of that page again. */
    *(volatile char *)q = 0;

    /* Create a pipe for the child to return the info to the parent. */
    ret = anetPipe(pipefd, 0, 0);
    if (ret < 0) {
        serverLog(LL_WARNING, "Failed to create pipe: %s", strerror(errno));
        bug_found = -1;
        goto exit;
    }

    /* Fork the process. */
    pid = fork();
    if (pid < 0) {
        serverLog(LL_WARNING, "Failed to fork: %s", strerror(errno));
        bug_found = -1;
        goto exit;
    }
    else if (!pid) {
        /* Child: check if the page is marked as dirty, page_size in kb.
         * A value of 0 means the kernel is affected by the bug. */
        ret = smapsGetSharedDirty((unsigned long)q);
        if (!ret)
            bug_found = 1;
        else if (ret == -1) /* Failed to read */
            bug_found = -1;

        if (write(pipefd[1], &bug_found, sizeof(bug_found)) < 0)
            serverLog(LL_WARNING, "Failed to write to parent: %s", strerror(errno));
        exitFromChild(0);
    }
    else {
        /* Read the result from the child. */
        ret = read(pipefd[0], &bug_found, sizeof(bug_found));
        if (ret < 0) {
            serverLog(LL_WARNING, "Failed to read from child: %s", strerror(errno));
            bug_found = -1;
        }

        /* Reap the child pid. */
        waitpid(pid, NULL, 0);
    }

exit:
    /* Cleanup */
    if (pipefd[0] != -1)
        close(pipefd[0]);
    if (pipefd[1] != -1)
        close(pipefd[1]);
    if (p != NULL)
        munmap(p, map_size);

    return bug_found;
}
#    endif /* __arm64__ */
#endif     /* __linux__ */

void createPidFile(void) {
    if (!server.pid_file)
        server.pid_file = zstrdup(CONFIG_DEFAULT_PID_FILE);

    FILE *fp = fopen(server.pid_file, "w");
    if (fp) {
        fprintf(fp, "%d\n", (int)getpid());
        fclose(fp);
    }
}

// 后台运行
void daemonize(void) {
    int fd;
    // 当返回值小于 0 时,此时表明 fork 函数执行有误;
    // 当返回值等于 0 时,此时,返回值对应的代码分支就会在子进程中运行;
    // 当返回值大于 0 时,此时,返回值对应的代码分支仍然会在父进程中运行.
    if (fork() != 0) {
        // fork失败
        // 成功后的父进程分支退出
        exit(0);
    }
    // 子进程开始运行
    // 创建成功
    setsid(); // 创建新的会话
    // 将子进程的标准输入、标准输出、标准错误输出重定向到/dev/null中
    fd = open("/dev/null", O_RDWR, 0);
    if (fd != -1) {
        dup2(fd, STDIN_FILENO); // 复制存在的文件描述符
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) {
            close(fd);
        }
    }
}

void version(void) {
    printf("Redis server v=%s sha=%s:%d malloc=%s bits=%d build=%llx\n", REDIS_VERSION, redisGitSHA1(), atoi(redisGitDirty()) > 0, ZMALLOC_LIB, sizeof(long) == 4 ? 32 : 64, (unsigned long long)redisBuildId());
    exit(0);
}

void usage(void) {
    fprintf(stderr, "Usage: ./redis-server [/path/to/redis.conf] [options] [-]\n");
    fprintf(stderr, "       ./redis-server - (read config from stdin)\n");
    fprintf(stderr, "       ./redis-server -v or --version\n");
    fprintf(stderr, "       ./redis-server -h or --help\n");
    fprintf(stderr, "       ./redis-server --test-memory <megabytes>\n\n");
    fprintf(stderr, "示例 :\n");
    fprintf(stderr, "       ./redis-server (run the server with default conf)\n");
    fprintf(stderr, "       ./redis-server /etc/redis/6379.conf\n");
    fprintf(stderr, "       ./redis-server --port 7777\n");
    fprintf(stderr, "       ./redis-server --port 7777 --replicaof 127.0.0.1 8888\n");
    fprintf(stderr, "       ./redis-server /etc/myredis.conf --loglevel verbose -\n");
    fprintf(stderr, "       ./redis-server /etc/myredis.conf --loglevel verbose\n\n");
    fprintf(stderr, "Sentinel mode:\n");
    fprintf(stderr, "       ./redis-server /etc/sentinel.conf --sentinel\n");
    exit(1);
}

// ok 打印redis logo
void redisAsciiArt(void) {
#include "over-asciilogo.h"

    char *buf = zmalloc(1024 * 16);
    char *mode;

    if (server.cluster_enabled)
        mode = "cluster";
    else if (server.sentinel_mode)
        mode = "sentinel";
    else
        mode = "standalone";

    /* Show the ASCII logo if: log file is stdout AND stdout is a
     * tty AND syslog logging is disabled. Also show logo if the user
     * forced us to do so via redis.conf. */
    // 如果:log文件是标准输出且标准输出是tty且syslog日志被禁用,则显示ASCII标志.如果用户强迫我们通过redis.conf这样做,也显示logo.

    int show_logo = ((!server.syslog_enabled && server.logfile[0] == '\0' && isatty(fileno(stdout))) || server.always_show_logo);

    if (!show_logo) {
        serverLog(LL_NOTICE, "Running mode=%s, port=%d.", mode, server.port ? server.port : server.tls_port);
    }
    else {
        snprintf(buf, 1024 * 16, ascii_logo, REDIS_VERSION, redisGitSHA1(), strtol(redisGitDirty(), NULL, 10) > 0, (sizeof(long) == 8) ? "64" : "32", mode, server.port ? server.port : server.tls_port, (long)getpid());
        serverLogRaw(LL_NOTICE | LL_RAW, buf);
    }
    zfree(buf);
}

int changeBindAddr(void) {
    /* Close old TCP and TLS servers */
    closeSocketListeners(&server.ipfd);
    closeSocketListeners(&server.tlsfd);

    // bind 新端口
    if ((server.port != 0 && listenToPort(server.port, &server.ipfd) != C_OK) || (server.tls_port != 0 && listenToPort(server.tls_port, &server.tlsfd) != C_OK)) {
        serverLog(LL_WARNING, "bind 失败");

        closeSocketListeners(&server.ipfd);
        closeSocketListeners(&server.tlsfd);
        return C_ERR;
    }

    /* Create TCP and TLS event handlers */
    if (createSocketAcceptHandler(&server.ipfd, acceptTcpHandler) != C_OK) {
        serverPanic("创建TCP套接字接受处理程序时出现不可恢复的错误。");
    }
    if (createSocketAcceptHandler(&server.tlsfd, acceptTLSHandler) != C_OK) {
        serverPanic("创建TLS套接字接受处理程序时出现不可恢复的错误。");
    }

    if (server.set_proc_title)
        redisSetProcTitle(NULL);

    return C_OK;
}

int changeListenPort(int port, socketFds *sfd, aeFileProc *accept_handler) {
    socketFds new_sfd = {{0}};

    closeSocketListeners(sfd);

    if (port == 0) {
        if (server.set_proc_title)
            redisSetProcTitle(NULL);
        return C_OK;
    }

    // 绑定新的端口
    if (listenToPort(port, &new_sfd) != C_OK) {
        return C_ERR;
    }

    if (createSocketAcceptHandler(&new_sfd, accept_handler) != C_OK) {
        closeSocketListeners(&new_sfd);
        return C_ERR;
    }

    sfd->count = new_sfd.count;
    memcpy(sfd->fd, new_sfd.fd, sizeof(new_sfd.fd));

    if (server.set_proc_title)
        redisSetProcTitle(NULL);

    return C_OK;
}

// 信号的处理器
static void sigShutdownHandler(int sig) {
    char *msg;

    switch (sig) {
        case SIGINT:
            msg = "收到了 SIGINT scheduling shutdown...";
            break;
        case SIGTERM:
            msg = "收到了 SIGTERM scheduling shutdown...";
            break;
        default:
            msg = "收到了 shutdown signal, scheduling shutdown...";
    };

    /* SIGINT通常在交互会话中通过Ctrl+C传递。
    如果我们第二次接收到信号，我们将其解释为用户真的想要尽快退出，而不需要等待磁盘上的持久化，也不需要等待滞后的副本。
  */
    if (server.shutdown_asap && sig == SIGINT) {
        serverLogFromHandler(LL_WARNING, "You insist... exiting now.");
        rdbRemoveTempFile(getpid(), 1);
        exit(1); /* Exit with an error since this was not a clean shutdown. */
    }
    else if (server.loading) {
        msg = "收到了 shutdown signal during loading, scheduling shutdown.";
    }

    serverLogFromHandler(LL_WARNING, msg);
    server.shutdown_asap = 1; // 打开关闭标识
    server.last_sig_received = sig;
}

// 信号处理函数
void setupSignalHandlers(void) {
    struct sigaction act;
    // 如果在sa_flags中设置了 SA_SIGINFO 标志,则使用sa_sigaction.否则,使用sa_handler.
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = sigShutdownHandler;
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGINT, &act, NULL);

    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_NODEFER | SA_RESETHAND | SA_SIGINFO;
    act.sa_sigaction = sigsegvHandler;
    if (server.crashlog_enabled) {
        sigaction(SIGSEGV, &act, NULL);
        sigaction(SIGBUS, &act, NULL);
        sigaction(SIGFPE, &act, NULL);
        sigaction(SIGILL, &act, NULL);
        sigaction(SIGABRT, &act, NULL);
    }
    return;
}

void removeSignalHandlers(void) {
    struct sigaction act;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_NODEFER | SA_RESETHAND;
    act.sa_handler = SIG_DFL;
    sigaction(SIGSEGV, &act, NULL);
    sigaction(SIGBUS, &act, NULL);
    sigaction(SIGFPE, &act, NULL);
    sigaction(SIGILL, &act, NULL);
    sigaction(SIGABRT, &act, NULL);
}

// 这是子进程的信号处理程序。它目前用于跟踪SIGUSR1，我们将SIGUSR1发送给子进程，
// 以便以一种干净的方式终止它，而父进程不会检测到错误并因为写入错误条件而停止接受写入。
static void sigKillChildHandler(int sig) {
    UNUSED(sig);
    int level = server.in_fork_child == CHILD_TYPE_MODULE ? LL_VERBOSE : LL_WARNING;
    serverLogFromHandler(level, "收到了 SIGUSR1 in child, exiting now.");
    exitFromChild(SERVER_CHILD_NOERROR_RETVAL);
}

void setupChildSignalHandlers(void) {
    struct sigaction act;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = sigKillChildHandler;
    sigaction(SIGUSR1, &act, NULL);
}

/* fork之后，子进程将继承父进程的资源，例如fd(socket或flock)等。
应该关闭子进程未使用的资源，以便在父进程重新启动时可以绑定/锁定子进程，尽管子进程可能仍在运行。 */
void closeChildUnusedResourceAfterFork() {
    closeListeningSockets(0);
    if (server.cluster_enabled && server.cluster_config_file_lock_fd != -1)
        close(server.cluster_config_file_lock_fd);

    /* 清除server.pid_file，这是父级pid_file，子级不应该触及（或删除）（退出/崩溃时）。*/
    zfree(server.pid_file);
    server.pid_file = NULL;
}

// 子进程类型
int redisFork(int purpose) {
    if (isMutuallyExclusiveChildType(purpose)) {
        if (hasActiveChildProcess()) {
            errno = EEXIST;
            return -1;
        }
        openChildInfoPipe(); // 打开当前进程和子进程之间的进程通信
    }

    int childpid;
    long long start = ustime(); // 记录fork 开始时间
    childpid = fork();
    // if 条件表达式中通过fork 开启 子进程,此时 当前进程和子进程开启各自不同的工作内容,子进程开启rdb,而当前进程则完成部分统计信息之后正常对外提供服务
    // 非阻塞指的是 rdb 过程不阻塞主进程,但是 从开始fork到fork完成这段时间仍然是阻塞的
    if (childpid == 0) { // 子进程
        /* Child.
         *设置的顺序遵循一些推理:首先设置信号处理程序，因为信号可能随时触发。如果内存资源不足，首先调整OOM分数以帮助OOM杀手。
         */
        server.in_fork_child = purpose;
        setupChildSignalHandlers();
        setOOMScoreAdj(CONFIG_OOM_BGCHILD);
        dismissMemoryInChild();
        closeChildUnusedResourceAfterFork(); // 关闭子进程从父进程中继承过来不需要使用的资源
    }
    else {
        /* Parent */
        if (childpid == -1) {
            int fork_errno = errno;
            if (isMutuallyExclusiveChildType(purpose))
                closeChildInfoPipe();
            errno = fork_errno;
            return -1;
        }

        server.stat_total_forks++;
        server.stat_fork_time = ustime() - start;
        server.stat_fork_rate = (double)zmalloc_used_memory() * 1000000 / server.stat_fork_time / (1024 * 1024 * 1024); /* GB per second. */
        latencyAddSampleIfNeeded("fork", server.stat_fork_time / 1000);

        /*
        child_pid和child_type仅用于互斥子节点。其他子类型应该在专用变量中处理和存储它们的pid。
        今天，我们允许CHILD_TYPE_LDB与其他fork类型并行运行:
        - 没有用于生产，所以不会降低服务器的效率
        - 用于调试，我们不想在其他fork正在运行时阻止它运行(比如RDB和AOF)
         */
        if (isMutuallyExclusiveChildType(purpose)) {
            server.child_pid = childpid;
            server.child_type = purpose;
            server.stat_current_cow_peak = 0;
            server.stat_current_cow_bytes = 0;
            server.stat_current_cow_updated = 0;
            server.stat_current_save_keys_processed = 0;
            server.stat_module_progress = 0;
            server.stat_current_save_keys_total = dbTotalServerKeyCount();
        }

        updateDictResizePolicy(); // 关闭rehash功能
        moduleFireServerEvent(REDISMODULE_EVENT_FORK_CHILD, REDISMODULE_SUBEVENT_FORK_CHILD_BORN, NULL);
    }
    return childpid;
}
// 这里主要发送的信息是说
// 子进程自己单独享用的内存空间是多少
// 这个信息能在命令info里面看到
void sendChildCowInfo(childInfoType info_type, char *pname) {
    sendChildInfoGeneric(info_type, 0, -1, pname);
}

void sendChildInfo(childInfoType info_type, size_t keys, char *pname) {
    sendChildInfoGeneric(info_type, keys, -1, pname);
}

// 尝试直接将页面释放回操作系统(绕过分配器)，以减少fork期间的CoW【写时拷贝】。
// 对于小的分配，我们不能释放任何完整的页面，因此为了避免从分配器(malloc_size)获取分配的大小，
// 当我们已经知道它很小时，我们检查size_hint。如果size还不知道，则传递size_hint为0将导致检查分配的实际大小。
// 另外请注意，大小可能不准确，所以为了使这个解决方案有效，对于释放内存页的判断不要太严格。
void dismissMemory(void *ptr, size_t size_hint) {
    if (ptr == NULL)
        return;

    /*如果内存太小，madvise(MADV_DONTNEED)不能释放页面，我们尝试只释放超过页面大小一半的内存。*/
    if (size_hint && size_hint <= server.page_size / 2)
        return;

    zmadvise_dontneed(ptr);
}

/* 解散客户端结构中的大块内存，参见dismismemory () */
void dismissClientMemory(client *c) {
    dismissMemory(c->buf, c->buf_usable_size);
    dismissSds(c->querybuf);
    if (c->argc && c->argv_len_sum / c->argc >= server.page_size) {
        for (int i = 0; i < c->argc; i++) {
            dismissObject(c->argv[i], 0);
        }
    }
    if (c->argc)
        dismissMemory(c->argv, c->argc * sizeof(robj *));

    if (listLength(c->reply) && c->reply_bytes / listLength(c->reply) >= server.page_size) {
        listIter li;
        listNode *ln;
        listRewind(c->reply, &li);
        while ((ln = listNext(&li))) {
            clientReplyBlock *bulk = listNodeValue(ln);
            if (bulk)
                dismissMemory(bulk, bulk->size);
        }
    }
}

/* 立即释放一些资源 */
void dismissMemoryInChild(void) {
    /* madvise(MADV_DONTNEED)可能不工作，如果透明大页启用。 */
    if (server.thp_enabled)
        return;

        /* 目前，我们只在Linux中使用jemalloc时使用zmadvise_dontneed。所以我们避免了这些无意义的循环当它们什么都不会做的时候。 */
#if defined(USE_JEMALLOC) && defined(__linux__)
    listIter li;
    listNode *ln;

    /* Dismiss replication buffer. We don't need to separately dismiss replication
     * backlog and replica' output buffer, because they just reference the global
     * replication buffer but don't cost real memory. */
    listRewind(server.repl_buffer_blocks, &li);
    while ((ln = listNext(&li))) {
        replBufBlock *o = listNodeValue(ln);
        dismissMemory(o, o->size);
    }

    /* Dismiss all clients memory. */
    listRewind(server.clients, &li);
    while ((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        dismissClientMemory(c);
    }
#endif
}

void memtest(size_t megabytes, int passes);

// 会根据你的命令以及参数,检查判断是否是以 sentinel 模式启动,如果是则返回 1
// 反之.如果是以 sentinel 启动,则会进行一个 sentinel 的初始化操作.
int checkForSentinelMode(int argc, char **argv, char *exec_name) {
    // strstr(str1,str2) 函数用于判断字符串str2是否是str1的子串
    // 程序名是redis-sentinel
    if (strstr(exec_name, "redis-sentinel") != NULL) {
        return 1;
    }
    // ./redis-server /etc/sentinel.conf --sentinel
    for (int j = 1; j < argc; j++)
        if (!strcmp(argv[j], "--sentinel"))
            return 1;
    return 0;
}

// OK
void redisOutOfMemoryHandler(size_t allocation_size) {
    serverLog(LL_WARNING, "内存分配不足 %zu bytes!", allocation_size);
    serverPanic("由于内存不足,Redis正在中止.分配 %zu bytes!", allocation_size);
}

// 根据RDB或者AOF文件加载旧数据,优先AOF文件
void loadDataFromDisk(void) {
    // 记录开始时间
    long long start = ustime();
    // 有限打开aof日志,因为它更新的快
    if (server.aof_state == AOF_ON) {
        // 尝试载入 AOF 文件
        int ret = loadAppendOnlyFiles(server.aof_manifest);
        if (ret == AOF_FAILED || ret == AOF_OPEN_ERR) {
            exit(1);
        }
    }
    else { // AOF 持久化未打开

        rdbSaveInfo rsi = RDB_SAVE_INFO_INIT;
        errno = 0; /* Prevent a stale value from affecting error checking */
        int rdb_flags = RDBFLAGS_NONE;
        if (iAmMaster()) {
            /* Master may delete expired keys when loading, we should
             * propagate expire to replication backlog. */
            createReplicationBacklog();
            rdb_flags |= RDBFLAGS_FEED_REPL;
        }
        // 尝试载入 RDB 文件
        if (rdbLoad(server.rdb_filename, &rsi, rdb_flags) == C_OK) {
            // 打印载入信息,并计算载入耗时长度
            serverLog(LL_NOTICE, "DB loaded from disk: %.3f seconds", (float)(ustime() - start) / 1000000);

            /* Restore the replication ID / offset from the RDB file. */
            if (rsi.repl_id_is_set && rsi.repl_offset != -1 &&
                /* Note that older implementations may save a repl_stream_db
                 * of -1 inside the RDB file in a wrong way, see more
                 * information in function rdbPopulateSaveInfo. */
                rsi.repl_stream_db != -1) {
                if (!iAmMaster()) {
                    memcpy(server.replid, rsi.repl_id, sizeof(server.replid));
                    server.master_repl_offset = rsi.repl_offset;
                    /* If this is a replica, create a cached master from this
                     * information, in order to allow partial resynchronizations
                     * with masters. */
                    replicationCacheMasterUsingMyself();
                    selectDb(server.cached_master, rsi.repl_stream_db);
                }
                else {
                    /* If this is a master, we can save the replication info
                     * as secondary ID and offset, in order to allow replicas
                     * to partial resynchronizations with masters. */
                    memcpy(server.replid2, rsi.repl_id, sizeof(server.replid));
                    server.second_replid_offset = rsi.repl_offset + 1;
                    /* Rebase master_repl_offset from rsi.repl_offset. */
                    server.master_repl_offset += rsi.repl_offset;
                    serverAssert(server.repl_backlog);
                    server.repl_backlog->offset = server.master_repl_offset - server.repl_backlog->histlen + 1;
                    rebaseReplicationBuffer(rsi.repl_offset);
                    server.repl_no_slaves_since = time(NULL);
                }
            }
        }
        else if (errno != ENOENT) {
            serverLog(LL_WARNING, "Fatal error loading the DB: %s. Exiting.", strerror(errno));
            exit(1);
        }

        /* We always create replication backlog if server is a master, we need
         * it because we put DELs in it when loading expired keys in RDB, but
         * if RDB doesn't have replication info or there is no rdb, it is not
         * possible to support partial resynchronization, to avoid extra memory
         * of replication backlog, we drop it. */
        if (server.master_repl_offset == 0 && server.repl_backlog) {
            freeReplicationBacklog();
        }
    }
}

static sds redisProcTitleGetVariable(const sds varname, void *arg) {
    if (!strcmp(varname, "title")) {
        return sdsnew(arg);
    }
    else if (!strcmp(varname, "listen-addr")) {
        if (server.port || server.tls_port)
            return sdscatprintf(sdsempty(), "%s:%u", server.bindaddr_count ? server.bindaddr[0] : "*", server.port ? server.port : server.tls_port);
        else
            return sdscatprintf(sdsempty(), "unixsocket:%s", server.unixsocket);
    }
    else if (!strcmp(varname, "server-mode")) {
        // 启动集群？
        if (server.cluster_enabled)
            return sdsnew("[cluster]");
        else if (server.sentinel_mode)
            return sdsnew("[sentinel]");
        else
            return sdsempty();
    }
    else if (!strcmp(varname, "config-file")) {
        return sdsnew(server.configfile ? server.configfile : "-");
    }
    else if (!strcmp(varname, "port")) {
        return sdscatprintf(sdsempty(), "%u", server.port);
    }
    else if (!strcmp(varname, "tls-port")) {
        return sdscatprintf(sdsempty(), "%u", server.tls_port);
    }
    else if (!strcmp(varname, "unixsocket")) {
        return sdsnew(server.unixsocket);
    }
    else
        return NULL; /* Unknown variable name */
}

// 展开指定的proc-title-template字符串并返回一个新分配的sds或NULL。
static sds expandProcTitleTemplate(const char *template, const char *title) {
    sds res = sdstemplate(template, redisProcTitleGetVariable, (void *)title);
    if (!res)
        return NULL;
    return sdstrim(res, " ");
}

int validateProcTitleTemplate(const char *template) {
    int ok = 1;
    sds res = expandProcTitleTemplate(template, "");
    if (!res)
        return 0;
    if (sdslen(res) == 0)
        ok = 0;
    sdsfree(res);
    return ok;
}

// 设置进程名称
int redisSetProcTitle(char *title) {
#ifdef USE_SETPROCTITLE
    if (!title)
        title = server.exec_argv[0];
    sds proc_title = expandProcTitleTemplate(server.proc_title_template, title);
    if (!proc_title)
        return C_ERR; /* Not likely, proc_title_template is validated */

    setproctitle("%s", proc_title);
    sdsfree(proc_title);
#else
    UNUSED(title);
#endif

    return C_OK;
}

void redisSetCpuAffinity(const char *cpulist) {
#ifdef USE_SETCPUAFFINITY
    setcpuaffinity(cpulist);
#else
    UNUSED(cpulist);
#endif
}

// 向systemd发送一条通知信息.成功时返回sd_notify返回代码,为正数.
int redisCommunicateSystemd(const char *sd_notify_msg) {
#ifdef HAVE_LIBSYSTEMD
    int ret = sd_notify(0, sd_notify_msg);

    if (ret == 0)
        serverLog(LL_WARNING, "systemd supervision error: 没有发现NOTIFY_SOCKET!");
    else if (ret < 0)
        serverLog(LL_WARNING, "systemd supervision error: sd_notify: %d", ret);
    return ret;
#else
    UNUSED(sd_notify_msg);
    return 0;
#endif
}

// 尝试设置 upstart 管理守护进程, 如果成功返回1
static int redisSupervisedUpstart(void) {
    const char *upstart_job = getenv("UPSTART_JOB");

    if (!upstart_job) {
        serverLog(LL_WARNING, "upstart 管理守护进程已请求,但没有发现环境变量 UPSTART_JOB!");
        return 0;
    }

    serverLog(LL_NOTICE, "通过 upstart 管理守护进程ing...");
    raise(SIGSTOP); // 给当前线程发送一个信号
    unsetenv("UPSTART_JOB");
    return 1;
}

// 尝试设置systemd 管理，成功返回1
static int redisSupervisedSystemd(void) {
#ifndef HAVE_LIBSYSTEMD
    serverLog(LL_WARNING, "主动或自动使用systemd管理 失败, redis没有与libsystemd一起编译");
    return 0;
#else
    if (redisCommunicateSystemd("STATUS=Redis is loading...\n") <= 0)
        return 0;
    serverLog(LL_NOTICE, "由systemd监督。请确保在serivce.unit中为TimeoutStartSec和TimeoutStopSec设置了适当的值。");
    return 1;
#endif
}

// 判断当redis进程是否运行中
int redisIsSupervised(int mode) {
    int ret = 0;
    // supervised no - 没有监督互动
    // supervised upstart - 通过将Redis置于SIGSTOP模式来启动
    // supervised systemd - signal systemd将READY = 1写入 $NOTIFY_SOCKET
    // supervised auto - 检测upstart或systemd方法基于 UPSTART_JOB 或 NOTIFY_SOCKET

    // 环境变量
    // #define SUPERVISED_NONE 0
    // #define SUPERVISED_AUTODETECT 1
    // #define SUPERVISED_SYSTEMD 2
    // #define SUPERVISED_UPSTART 3
    if (mode == SUPERVISED_AUTODETECT) {
        if (getenv("UPSTART_JOB")) {
            serverLog(LL_VERBOSE, "Upstart管理守护进程");
            mode = SUPERVISED_UPSTART;
        }
        else if (getenv("NOTIFY_SOCKET")) {
            serverLog(LL_VERBOSE, "Systemd 管理守护进程");
            mode = SUPERVISED_SYSTEMD;
        }
    }

    switch (mode) {
        case SUPERVISED_UPSTART:
            ret = redisSupervisedUpstart();
            break;
        case SUPERVISED_SYSTEMD:
            ret = redisSupervisedSystemd();
            break;
        default:
            break;
    }

    if (ret) {
        server.supervised_mode = mode;
    }

    return ret;
}

int iAmMaster(void) {
    return (
        (!server.cluster_enabled && server.masterhost == NULL) ||        //
        (server.cluster_enabled && nodeIsMaster(server.cluster->myself)) //
    );
}

#ifdef REDIS_TEST
#    include "testhelp.h"

int __failed_tests = 0;
int __test_num = 0;

/* The flags are the following:
 * --accurate:     Runs tests with more iterations.
 * --large-memory: Enables tests that consume more than 100mb. */
typedef int redisTestProc(int argc, char **argv, int flags);
struct redisTest {
    char *name;
    redisTestProc *proc;
    int failed;
} redisTests[] = {{"ziplist", ziplistTest}, // 如果启动参数有test和ziplist,那么就调用ziplistTest函数进行ziplist的测试
                  {"quicklist", quicklistTest}, {"intset", intsetTest}, {"zipmap", zipmapTest}, {"sha1test", sha1Test}, {"util", utilTest}, {"endianconv", endianconvTest}, {"crc64", crc64Test}, {"zmalloc", zmalloc_test}, {"sds", sdsTest}, {"dict", dictTest}, {"listpack", listpackTest}};
redisTestProc *getTestProcByName(const char *name) {
    int numtests = sizeof(redisTests) / sizeof(struct redisTest);
    for (int j = 0; j < numtests; j++) {
        if (!strcasecmp(name, redisTests[j].name)) {
            return redisTests[j].proc;
        }
    }
    return NULL;
}
#endif

int main(int argc, char **argv) {
    system("python3 ../utils/kill-redis.py");
    struct timeval tv; // 程序启动时的时间
    int j;
    char config_from_stdin = 0;

#ifdef REDIS_TEST

    if (argc >= 3 && !strcasecmp(argv[1], "test")) {
        int flags = 0;
        for (j = 3; j < argc; j++) {
            char *arg = argv[j];
            if (!strcasecmp(arg, "--accurate"))
                flags |= REDIS_TEST_ACCURATE;
            else if (!strcasecmp(arg, "--large-memory"))
                flags |= REDIS_TEST_LARGE_MEMORY;
        }

        if (!strcasecmp(argv[2], "all")) {
            int numtests = sizeof(redisTests) / sizeof(struct redisTest);
            for (j = 0; j < numtests; j++) {
                redisTests[j].failed = (redisTests[j].proc(argc, argv, flags) != 0);
            }

            /* Report tests result */
            int failed_num = 0;
            for (j = 0; j < numtests; j++) {
                if (redisTests[j].failed) {
                    failed_num++;
                    printf("[failed] Test - %s\n", redisTests[j].name);
                }
                else {
                    printf("[ok] Test - %s\n", redisTests[j].name);
                }
            }

            printf("%d tests, %d passed, %d failed\n", numtests, numtests - failed_num, failed_num);

            return failed_num == 0 ? 0 : 1;
        }
        else {
            redisTestProc *proc = getTestProcByName(argv[2]);
            if (!proc)
                return -1; /* test not found */
            return proc(argc, argv, flags);
        }

        return 0;
    }
#endif
    //  阶段一：基本初始化
    // 我们需要初始化我们的库,以及服务器配置.
#ifdef INIT_SETPROCTITLE_REPLACEMENT
    spt_init(argc, argv);
#endif

    setlocale(LC_COLLATE,
              "");                                                            // 函数既可以用来对当前程序进行地域设置（本地设置、区域设置）,也可以用来获取当前程序的地域设置信息.
    tzset();                                                                  // 设置时间环境变量【
    zmalloc_set_oom_handler(redisOutOfMemoryHandler);                         // 内存溢出的注册回调,redisOutOfMemoryHandler只用来打日志
    srand(time(NULL) ^ getpid());                                             // 随机种子
    gettimeofday(&tv, NULL);                                                  // 获取系统时间
    init_genrand64(((long long)tv.tv_sec * 1000000 + tv.tv_usec) ^ getpid()); // 初始化 状态向量数组       秒+100000微秒
    crc64_init();                                                             // 根据本地时间生成一个随机数种子,后面要介绍的dict结构体内会有用到
    umask(server.umask = umask(0777));                                        // 设置即将创建的文件权限掩码 如果设置了0777  ,那么创建的文件权限就是  777-777=000  没有任何权限

    // 设置随机种子
    uint8_t hashseed[16];
    getRandomBytes(hashseed, sizeof(hashseed));
    dictSetHashFunctionSeed(hashseed); // 初始化全局变量 dict.c dict_hash_function_seed

    char *exec_name = strrchr(argv[0], '/'); // 最后一个出现/的位置指针        exec_name="/reids-server"
    if (exec_name == NULL) {
        exec_name = argv[0];
    }
    server.sentinel_mode = checkForSentinelMode(argc, argv, exec_name); // 检查服务器是否以 Sentinel 模式启动
    initServerConfig();                                                 // 初始化服务器配置,默认值
    ACLInit();                                                          // 初始化用户访问、权限相关的

    // 占用了 3,4号 文件描述符
    moduleInitModulesSystem(); // 初始化所有的依赖模块

    tlsInit(); // 和ssl相关的初始化
    // 将可执行路径和参数存储在安全的地方,以便稍后能够重新启动服务器.
    sds s = (sds)getAbsolutePath(argv[0]);
    printf("sds ---->: %s\n", s);
    server.executable = getAbsolutePath(argv[0]);
    server.exec_argv = zmalloc(sizeof(char *) * (argc + 1));
    server.exec_argv[argc] = NULL;
    // 保存命令行参数
    for (j = 0; j < argc; j++) {
        server.exec_argv[j] = zstrdup(argv[j]); // 字符串复制
    }

    //  阶段二：检查哨兵模式,并检查是否要执行 RDB 检测或 AOF 检测
    // 判断server是否设置为哨兵模式
    if (server.sentinel_mode) {
        initSentinelConfig(); // 初始化哨兵的配置
        initSentinel();       // 初始化哨兵模式
    }

    // 如果运行的是redis-check-rdb程序,调用redis_check_rdb_main函数检测RDB文件
    if (strstr(exec_name, "redis-check-rdb") != NULL) {
        // strstr(str1,str2) 函数用于判断字符串str2是否是str1的子串
        redis_check_rdb_main(argc, argv, NULL);
    }
    // 如果运行的是redis-check-aof程序,调用redis_check_aof_main函数检测AOF文件
    else if (strstr(exec_name, "redis-check-aof") != NULL) {
        redis_check_aof_main(argc, argv);
    }
    //  阶段三：运行参数解析
    if (argc >= 2) {
        j = 1; // 在argv[]中解析的第一个选项
        sds options = sdsempty();

        /* Handle special options --help and --version */
        // 处理特殊选项 -h 、-v 和 --test-memory
        if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0) {
            version();
        }
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            usage();
        }
        if (strcmp(argv[1], "--test-memory") == 0) {
            if (argc == 3) {
                memtest(atoi(argv[2]), 50);
                exit(0);
            }
            else {
                fprintf(stderr, "请指定要测试的内存量(以兆为单位).\n");
                fprintf(stderr, "Example: ./redis-server --test-memory 4096\n\n");
                exit(1);
            }
        }

        /* First argument is the config file name? */
        // 如果第一个参数（argv[1]）不是以 "--" 开头
        // 那么它应该是一个配置文件
        if (argv[1][0] != '-') {
            /* Replace the config file in server.exec_argv with its absolute path. */
            // 获取配置文件的绝对路径
            server.configfile = getAbsolutePath(argv[1]);
            zfree(server.exec_argv[1]);
            server.exec_argv[1] = zstrdup(server.configfile);
            j = 2; // Skip this arg when parsing options
        }
        // 对每个运行时参数进行解析
        while (j < argc) {
            /* Either first or last argument - Should we read config from stdin? */
            if (argv[j][0] == '-' && argv[j][1] == '\0' && (j == 1 || j == argc - 1)) {
                config_from_stdin = 1;
            }
            // 对用户给定的其余选项进行分析,并将分析所得的字符串追加稍后载入的配置文件的内容之后
            // 比如 --port 6380 会被分析为 "port 6380\n"
            /* All the other options are parsed and conceptually appended to the
             * configuration file. For instance --port 6380 will generate the
             * string "port 6380\n" to be parsed after the actual config file
             * and stdin input are parsed (if they exist). */
            else if (argv[j][0] == '-' && argv[j][1] == '-') {
                /* Option name */
                if (sdslen(options))
                    options = sdscat(options, "\n");
                options = sdscat(options, argv[j] + 2);
                options = sdscat(options, " ");
            }
            else {
                /* Option argument */
                options = sdscatrepr(options, argv[j], strlen(argv[j]));
                options = sdscat(options, " ");
            }
            j++;
        }
        // 载入配置文件,options 是前面分析出的给定选项
        loadServerConfig(server.configfile, config_from_stdin, options);
        serverLog(LL_WARNING, "指定配置文件 %s", server.configfile);

        if (server.sentinel_mode) {
            loadSentinelConfigFromQueue();
        }
        sdsfree(options);
    }
    if (server.sentinel_mode) {
        // 哨兵模式
        sentinelCheckConfigFile();
    }
    // 也可以把 Redis 托管给 upstart 或 systemd 来启动/停止（supervised = no|upstart|systemd|auto）.
    server.supervised = redisIsSupervised(server.supervised_mode);
    int background = server.daemonize && !server.supervised;
    if (background) {
        // 将服务器设置为守护进程
        daemonize();
    }

    serverLog(LL_WARNING, "-------> Redis is starting <-------");
    serverLog(LL_WARNING, "Redis version=%s,bits=%d,commit=%s,modified=%d,pid=%d,just started", REDIS_VERSION, (sizeof(long) == 8) ? 64 : 32, redisGitSHA1(), strtol(redisGitDirty(), NULL, 10) > 0, (int)getpid());
    serverLog(LL_WARNING, "配置已加载");
    //  阶段四：初始化 server

    initServer(); // 创建并初始化服务器数据结构
    if (background || server.pid_file) {
        // 如果服务器是守护进程,那么创建 PID 文件
        createPidFile();
    }
    if (server.set_proc_title) {
        redisSetProcTitle(NULL); // 为服务器进程设置名字
    }
    redisAsciiArt();           // 打印redis logo
    checkTcpBacklogSettings(); // 检查tcp_backlog和系统的somaxconn参数值

    // 如果服务器不是运行在 SENTINEL 模式,那么执行以下代码
    if (!server.sentinel_mode) {
        serverLog(LL_WARNING, "Server 初始化完成");
#ifdef __linux__
        // 打印内存警告
        linuxMemoryWarnings();
#    if defined(__arm64__)
        int ret;
        if ((ret = linuxMadvFreeForkBugCheck())) {
            if (ret == 1)
                serverLog(
                    LL_WARNING,
                    "WARNING Your kernel has a bug that could lead to data corruption during background save. "
                    "Please upgrade to the latest stable kernel.");
            else
                serverLog(
                    LL_WARNING,
                    "Failed to test the kernel for a bug that could lead to data corruption during background save. "
                    "Your system could be affected, please report this error.");
            if (!checkIgnoreWarning("ARM64-COW-BUG")) {
                serverLog(
                    LL_WARNING,
                    "Redis will now exit to prevent data corruption. "
                    "Note that it is possible to suppress this warning by setting the following config: ignore-warnings ARM64-COW-BUG");
                exit(1);
            }
        }
#    endif                             /* __arm64__ */
#endif                                 /* __linux__ */
        moduleInitModulesSystemLast(); // 暂无逻辑
        moduleLoadFromQueue();         // 加载模块
        ACLLoadUsersAtStartup();       // 用户访问控制、权限 加载
        InitServerLast();              // 初始化网络 IO 相关的线程资源
        aofLoadManifestFromDisk();     // 从磁盘中加载aof日志
        loadDataFromDisk();            // 从 AOF 文件或者 RDB 文件中载入数据
        aofOpenIfNeededOnServerStart();
        aofDelHistoryFiles();
        if (server.cluster_enabled) {                     // 启动集群？
            if (verifyClusterConfigWithData() == C_ERR) { // 检查当前节点的节点配置是否正确,包含的数据是否正确
                serverLog(LL_WARNING, "在集群模式下,你不能在DB中有不同于DB 0的键.退出.");
                exit(1);
            }
        }
        // 打印 TCP 端口
        if (server.ipfd.count > 0 || server.tlsfd.count > 0) {
            serverLog(LL_NOTICE, "准备接收链接");
        }
        // 打印本地套接字端口
        if (server.sofd > 0) {
            serverLog(LL_NOTICE, "服务器现在准备接受连接%s", server.unixsocket);
        }
        if (server.ipfd.count > 0 || server.tlsfd.count > 0)
            serverLog(LL_NOTICE, "准备接受连接\n");
        if (server.sofd > 0) // 打印 TCP 端口
            serverLog(LL_NOTICE, "The server is now ready to accept connections at %s", server.unixsocket);
        if (server.supervised_mode == SUPERVISED_SYSTEMD) { // systemd模式
            if (!server.masterhost) {
                redisCommunicateSystemd("STATUS=准备接收连接\n");
            }
            else {
                redisCommunicateSystemd("STATUS=准备以只读模式接受连接.等待 MASTER <-> REPLICA 同步\n");
            }
            redisCommunicateSystemd("READY=1\n");
        }
    }
    else {
        ACLLoadUsersAtStartup();
        InitServerLast();    // 初始化网络 IO 相关的线程资源
        sentinelIsRunning(); // 设置启动哨兵模式
        if (server.supervised_mode == SUPERVISED_SYSTEMD) {
            redisCommunicateSystemd("STATUS=准备好接收链接\n");
            redisCommunicateSystemd("READY=1\n");
        }
    }

    // 检查不正常的 maxmemory 配置
    if (server.maxmemory > 0 && server.maxmemory < 1024 * 1024) {
        serverLog(LL_WARNING, "警报:您指定了小于1MB的 maxmemory 值(当前值是%llu字节).你确定这是你想要的吗?", server.maxmemory);
    }
    // 设置CPU亲和
    redisSetCpuAffinity(server.server_cpulist);
    setOOMScoreAdj(-1);
    // 执行事件捕获、分发和处理循环
    aeMain(server.el);

    // 服务器关闭,停止事件循环
    aeDeleteEventLoop(server.el);
    return 0;
}
