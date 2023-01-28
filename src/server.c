// 1、initServerConfig               为各种参数设置默认值
// 2、main                           逐一解析命令行参数
// 3、loadServerConfig               进行第二、三赋值
// 4、loadServerConfigFromString     对配置项字符串中的每一个配置项进行匹配

#include "server.h"
#include "monotonic.h"
#include "cluster.h"
#include "slowlog.h"
#include "bio.h"
#include "latency.h"
#include "atomicvar.h"
#include "mt19937-64.h"
#include "functions.h"

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
#include <sys/mman.h>
#endif

#if defined(HAVE_SYSCTL_KIPC_SOMAXCONN) || defined(HAVE_SYSCTL_KERN_SOMAXCONN)
#include <sys/sysctl.h>

#endif

/* Our shared "common" objects */

struct sharedObjectsStruct shared;

/* Global vars that are actually used as constants. The following double
 * values are used for double on-disk serialization, and are initialized
 * at runtime to avoid strange compiler optimizations. */

double R_Zero, R_PosInf, R_NegInf, R_Nan;

/*================================= Globals ================================= */

/* Global vars */
struct redisServer server; /* Server global state */

/*============================ Internal prototypes ========================== */

static inline int isShutdownInitiated(void);

int isReadyToShutdown(void);

int finishShutdown(void);

const char *replstateToString(int replstate);

/*============================ Utility functions ============================ */

/* We use a private localtime implementation which is fork-safe. The logging
 * function of Redis may be called from other threads. */
void nolocks_localtime(struct tm *tmp, time_t t, time_t tz, int dst);

// 监听tcp端口
int mylistenToPort() {
    return anetTcpServer(server.neterr, 6479, "*", server.tcp_backlog);
}

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

/* Like serverLogRaw() but with printf-alike support. This is the function that
 * is used across the code. The raw version is only used in order to dump
 * the INFO output on crash. */
void _serverLog(int level, const char *fmt, ...) {
    va_list ap;
    char msg[LOG_MAX_LEN];

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    serverLogRaw(level, msg);
}

/* Log a fixed message without printf-alike capabilities, in a way that is
 * safe to call from a signal handler.
 *
 * We actually use this only for signals that are not fatal from the point
 * of view of Redis. Signals that are going to kill the server anyway and
 * where we need printf-alike features are served by serverLog(). */
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
mstime_t mstime(void) {
    return ustime() / 1000;
}

/* After an RDB dump or AOF rewrite we exit from children using _exit() instead of
 * exit(), because the latter may interact with the same file objects used by
 * the parent process. However if we are testing the coverage normal exit() is
 * used in order to obtain the right coverage information. */
void exitFromChild(int retcode) {
#ifdef COVERAGE_TEST
    exit(retcode);
#else
    _exit(retcode);
#endif
}

/*====================== Hash table type implementation  ==================== */

/* This is a hash table type that uses the SDS dynamic strings library as
 * keys and redis objects as values (objects can hold SDS strings,
 * lists, sets). */

void dictVanillaFree(dict *d, void *val) {
    UNUSED(d);
    zfree(val);
}

void dictListDestructor(dict *d, void *val) {
    UNUSED(d);
    listRelease((list *)val);
}

int dictSdsKeyCompare(dict *d, const void *key1, const void *key2) {
    int l1, l2;
    UNUSED(d);

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2)
        return 0;
    return memcmp(key1, key2, l1) == 0;
}

/* A case insensitive version used for the command lookup table and other
 * places where case insensitive non binary-safe comparison is needed. */
int dictSdsKeyCaseCompare(dict *d, const void *key1, const void *key2) {
    UNUSED(d);
    return strcasecmp(key1, key2) == 0;
}

void dictObjectDestructor(dict *d, void *val) {
    UNUSED(d);
    if (val == NULL)
        return; /* Lazy freeing will set value to NULL. */
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

int dictObjKeyCompare(dict *d, const void *key1, const void *key2) {
    const robj *o1 = key1, *o2 = key2;
    return dictSdsKeyCompare(d, o1->ptr, o2->ptr);
}

uint64_t dictObjHash(const void *key) {
    const robj *o = key;
    return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
}

uint64_t dictSdsHash(const void *key) {
    return dictGenHashFunction((unsigned char *)key, sdslen((char *)key));
}

uint64_t dictSdsCaseHash(const void *key) {
    return dictGenCaseHashFunction((unsigned char *)key, sdslen((char *)key));
}

/* Dict hash function for null terminated string */
uint64_t distCStrHash(const void *key) {
    return dictGenHashFunction((unsigned char *)key, strlen((char *)key));
}

/* Dict hash function for null terminated string */
uint64_t distCStrCaseHash(const void *key) {
    return dictGenCaseHashFunction((unsigned char *)key, strlen((char *)key));
}

/* Dict compare function for null terminated string */
int distCStrKeyCompare(dict *d, const void *key1, const void *key2) {
    int l1, l2;
    UNUSED(d);

    l1 = strlen((char *)key1);
    l2 = strlen((char *)key2);
    if (l1 != l2)
        return 0;
    return memcmp(key1, key2, l1) == 0;
}

/* Dict case insensitive compare function for null terminated string */
int distCStrKeyCaseCompare(dict *d, const void *key1, const void *key2) {
    UNUSED(d);
    return strcasecmp(key1, key2) == 0;
}

int dictEncObjKeyCompare(dict *d, const void *key1, const void *key2) {
    robj *o1 = (robj *)key1, *o2 = (robj *)key2;
    int cmp;

    if (o1->encoding == OBJ_ENCODING_INT && o2->encoding == OBJ_ENCODING_INT)
        return o1->ptr == o2->ptr;

    /* Due to OBJ_STATIC_REFCOUNT, we avoid calling getDecodedObject() without
     * good reasons, because it would incrRefCount() the object, which
     * is invalid. So we check to make sure dictFind() works with static
     * objects as well. */
    if (o1->refcount != OBJ_STATIC_REFCOUNT)
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
        serverPanic("Unknown string encoding");
    }
}

/* Return 1 if currently we allow dict to expand. Dict may allocate huge
 * memory to contain hash buckets when dict expands, that may lead redis
 * rejects user's requests or evicts some keys, we can stop dict to expand
 * provisionally if used memory will be over maxmemory after dict expands,
 * but to guarantee the performance of redis, we still allow dict to expand
 * if dict load factor exceeds HASHTABLE_MAX_LOAD_FACTOR. */
int dictExpandAllowed(size_t moreMem, double usedRatio) {
    if (usedRatio <= HASHTABLE_MAX_LOAD_FACTOR) {
        return !overMaxmemoryAfterAlloc(moreMem);
    }
    else {
        return 1; // 允许扩容
    }
}

/* Returns the size of the DB dict entry metadata in bytes. In cluster mode, the
 * metadata is used for constructing a doubly linked list of the dict entries
 * belonging to the same cluster slot. See the Slot to Key API in cluster.c. */
size_t dictEntryMetadataSize(dict *d) {
    UNUSED(d);
    /* NOTICE: this also affect overhead_ht_slot_to_keys in getMemoryOverheadData.
     * If we ever add non-cluster related data here, that code must be modified too. */
    return server.cluster_enabled ? sizeof(clusterDictEntryMetadata) : 0;
}

/* Generic hash table type where keys are Redis Objects, Values
 * dummy pointers. */
dictType objectKeyPointerValueDictType = {dictEncObjHash, NULL, NULL, dictEncObjKeyCompare, dictObjectDestructor, NULL, NULL};

/* Like objectKeyPointerValueDictType(), but values can be destroyed, if
 * not NULL, calling zfree(). */
dictType objectKeyHeapPointerValueDictType = {dictEncObjHash, NULL, NULL, dictEncObjKeyCompare, dictObjectDestructor, dictVanillaFree, NULL};

/* Set dictionary type. Keys are SDS strings, values are not used. */
dictType setDictType = {dictSdsHash, NULL, NULL, dictSdsKeyCompare, dictSdsDestructor, NULL};

/* Sorted sets hash (note: a skiplist is used in addition to the hash table) */
dictType zsetDictType = {dictSdsHash, NULL, NULL, dictSdsKeyCompare, NULL, NULL, NULL};

/* Db->dict, keys are sds strings, vals are Redis objects. */
dictType dbDictType = {
    dictSdsHash,
    NULL,
    NULL,
    dictSdsKeyCompare,
    dictSdsDestructor,    // key的释放函数
    dictObjectDestructor, // value的释放函数
    dictExpandAllowed,
    dictEntryMetadataSize};

// Db->expires 的dict结构
dictType dbExpiresDictType = {
    dictSdsHash,
    NULL,
    NULL,
    dictSdsKeyCompare, //
    NULL,
    NULL,
    dictExpandAllowed};

// sds->command 的dict结构
dictType commandTableDictType = {
    dictSdsCaseHash, //
    NULL,
    NULL,
    dictSdsKeyCaseCompare, //
    dictSdsDestructor,
    NULL,
    NULL};

/* Hash type hash table (note that small hashes are represented with listpacks) */
dictType hashDictType = {
    dictSdsHash, //
    NULL,
    NULL, //
    dictSdsKeyCompare,
    dictSdsDestructor,
    dictSdsDestructor,
    NULL};

/* Dict type without destructor */
dictType sdsReplyDictType = {
    dictSdsHash, //
    NULL,
    NULL, //
    dictSdsKeyCompare,
    NULL,
    NULL,
    NULL};

/* Keylist hash table type has unencoded redis objects as keys and
 * lists as values. It's used for blocking operations (BLPOP) and to
 * map swapped keys to a list of clients waiting for this keys to be loaded. */
dictType keylistDictType = {
    dictObjHash, //
    NULL,
    NULL, //
    dictObjKeyCompare,
    dictObjectDestructor,
    dictListDestructor,
    NULL};

/* Modules system dictionary type. Keys are module name,
 * values are pointer to RedisModule struct. */
dictType modulesDictType = {
    dictSdsCaseHash, //
    NULL,
    NULL,
    dictSdsKeyCaseCompare,
    dictSdsDestructor,
    NULL,
    NULL};

// 缓存迁移的dict类型.
dictType migrateCacheDictType = {dictSdsHash, NULL, NULL, dictSdsKeyCompare, dictSdsDestructor, NULL, NULL};

/* Dict for for case-insensitive search using null terminated C strings.
 * The keys stored in dict are sds though. */
dictType stringSetDictType = {distCStrCaseHash, NULL, NULL, distCStrKeyCaseCompare, dictSdsDestructor, NULL, NULL};

/* Dict for for case-insensitive search using null terminated C strings.
 * The key and value do not have a destructor. */
dictType externalStringType = {distCStrCaseHash, NULL, NULL, distCStrKeyCaseCompare, NULL, NULL, NULL};

/* Dict for case-insensitive search using sds objects with a zmalloc
 * allocated object as the value. */
dictType sdsHashDictType = {dictSdsCaseHash, NULL, NULL, dictSdsKeyCaseCompare, dictSdsDestructor, dictVanillaFree, NULL};

int htNeedsResize(dict *dict) {
    long long size, used;

    size = dictSlots(dict);
    used = dictSize(dict);
    return (size > DICT_HT_INITIAL_SIZE && (used * 100 / size < HASHTABLE_MIN_FILL));
}

/* If the percentage of used slots in the HT reaches HASHTABLE_MIN_FILL
 * we resize the hash table to save memory */
// 如果字典的使用率比 REDIS_HT_MINFILL 常量要低
// 那么通过缩小字典的体积来节约内存
void tryResizeHashTables(int dbid) {
    if (htNeedsResize(server.db[dbid].dict)) {
        dictResize(server.db[dbid].dict);
    }
    if (htNeedsResize(server.db[dbid].expires)) {
        dictResize(server.db[dbid].expires);
    }
}

// * 虽然服务器在对数据库执行读取/写入命令时会对数据库进行渐进式 rehash ,
// * 但如果服务器长期没有执行命令的话,数据库字典的 rehash 就可能一直没办法完成,
// * 为了防止出现这种情况,我们需要对数据库执行主动 rehash .
// * 函数在执行了主动 rehash 时返回 1 ,否则返回 0 .
int incrementallyRehash(int dbid) {
    /* Keys dictionary */
    if (dictIsRehashing(server.db[dbid].dict)) {
        dictRehashMilliseconds(server.db[dbid].dict, 1);
        return 1; /* already used our millisecond for this loop... */
    }
    /* Expires */
    if (dictIsRehashing(server.db[dbid].expires)) {
        dictRehashMilliseconds(server.db[dbid].expires, 1);
        return 1; /* already used our millisecond for this loop... */
    }
    return 0;
}

// 禁止在 AOF 重写期间进行 rehash 操作.
// 以便更好地进行写时复制(否则当发生大小调整时,会复制大量内存页).
void updateDictResizePolicy(void) {
    if (!hasActiveChildProcess()) { // 是否正在运行的AOF进程
        dictEnableResize();
    }
    else {
        dictDisableResize();
    }
}

const char *strChildType(int type) {
    switch (type) {
        case CHILD_TYPE_RDB:
            return "RDB";
        case CHILD_TYPE_AOF:
            return "AOF";
        case CHILD_TYPE_LDB:
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

void resetChildState() {
    server.child_type = CHILD_TYPE_NONE;
    server.child_pid = -1;
    server.stat_current_cow_peak = 0;
    server.stat_current_cow_bytes = 0;
    server.stat_current_cow_updated = 0;
    server.stat_current_save_keys_processed = 0;
    server.stat_module_progress = 0;
    server.stat_current_save_keys_total = 0;
    updateDictResizePolicy();
    closeChildInfoPipe();
    moduleFireServerEvent(REDISMODULE_EVENT_FORK_CHILD, REDISMODULE_SUBEVENT_FORK_CHILD_DIED, NULL);
}

/* Return if child type is mutual exclusive with other fork children */
int isMutuallyExclusiveChildType(int type) {
    return type == CHILD_TYPE_RDB || type == CHILD_TYPE_AOF || type == CHILD_TYPE_MODULE;
}

/* Return true if this instance has persistence completely turned off:
 * both RDB and AOF are disabled. */
int allPersistenceDisabled(void) {
    return server.saveparamslen == 0 && server.aof_state == AOF_OFF;
}

/* ======================= Cron: called every 100 ms ======================== */

/* Add a sample to the operations per second array of samples. */
void trackInstantaneousMetric(int metric, long long current_reading) {
    long long now = mstime();
    long long t = now - server.inst_metric[metric].last_sample_time;
    long long ops = current_reading - server.inst_metric[metric].last_sample_count;
    long long ops_sec;

    ops_sec = t > 0 ? (ops * 1000 / t) : 0;

    server.inst_metric[metric].samples[server.inst_metric[metric].idx] = ops_sec;
    server.inst_metric[metric].idx++;
    server.inst_metric[metric].idx %= STATS_METRIC_SAMPLES;
    server.inst_metric[metric].last_sample_time = now;
    server.inst_metric[metric].last_sample_count = current_reading;
}

/* Return the mean of all the samples. */
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
    time_t idletime = server.unixtime - c->lastinteraction;

    /* Only resize the query buffer if the buffer is actually wasting at least a
     * few kbytes */
    if (sdsavail(c->querybuf) > 1024 * 4) {
        // 符合以下两个条件的话,执行大小调整：
        if (idletime > 2) {
            // 空闲了一段时间
            c->querybuf = sdsRemoveFreeSpace(c->querybuf);
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
    /* We reset to either the current used, or currently processed bulk size,
     * which ever is bigger. */
    if (c->bulklen != -1 && (size_t)c->bulklen > c->querybuf_peak)
        c->querybuf_peak = c->bulklen;
    return 0;
}

/* The client output buffer can be adjusted to better fit the memory requirements.
 *
 * the logic is:
 * in case the last observed peak size of the buffer equals the buffer size - we double the size
 * in case the last observed peak size of the buffer is less than half the buffer size - we shrink by half.
 * The buffer peak will be reset back to the buffer position every server.reply_buffer_peak_reset_time milliseconds
 * The function always returns 0 as it never terminates the client. */
int clientsCronResizeOutputBuffer(client *c, mstime_t now_ms) {
    size_t new_buffer_size = 0;
    char *oldbuf = NULL;
    const size_t buffer_target_shrink_size = c->buf_usable_size / 2;
    const size_t buffer_target_expand_size = c->buf_usable_size * 2;

    /* in case the resizing is disabled return immediately */
    if (!server.reply_buffer_resizing_enabled)
        return 0;

    if (buffer_target_shrink_size >= PROTO_REPLY_MIN_BYTES && c->buf_peak < buffer_target_shrink_size) {
        new_buffer_size = max(PROTO_REPLY_MIN_BYTES, c->buf_peak + 1);
        server.stat_reply_buffer_shrinks++;
    }
    else if (buffer_target_expand_size < PROTO_REPLY_CHUNK_BYTES * 2 && c->buf_peak == c->buf_usable_size) {
        new_buffer_size = min(PROTO_REPLY_CHUNK_BYTES, buffer_target_expand_size);
        server.stat_reply_buffer_expands++;
    }

    /* reset the peak value each server.reply_buffer_peak_reset_time seconds. in case the client will be idle
     * it will start to shrink.
     */
    if (server.reply_buffer_peak_reset_time >= 0 && now_ms - c->buf_peak_last_reset_time >= server.reply_buffer_peak_reset_time) {
        c->buf_peak = c->bufpos;
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

/* This function is used in order to track clients using the biggest amount
 * of memory in the latest few seconds. This way we can provide such information
 * in the INFO output (clients section), without having to do an O(N) scan for
 * all the clients.
 *
 * This is how it works. We have an array of CLIENTS_PEAK_MEM_USAGE_SLOTS slots
 * where we track, for each, the biggest client output and input buffers we
 * saw in that slot. Every slot correspond to one of the latest seconds, since
 * the array is indexed by doing UNIXTIME % CLIENTS_PEAK_MEM_USAGE_SLOTS.
 *
 * When we want to know what was recently the peak memory usage, we just scan
 * such few slots searching for the maximum value. */
#define CLIENTS_PEAK_MEM_USAGE_SLOTS 8
size_t ClientsPeakMemInput[CLIENTS_PEAK_MEM_USAGE_SLOTS] = {0};
size_t ClientsPeakMemOutput[CLIENTS_PEAK_MEM_USAGE_SLOTS] = {0};

int clientsCronTrackExpansiveClients(client *c, int time_idx) {
    size_t in_usage = sdsZmallocSize(c->querybuf) + c->argv_len_sum + (c->argv ? zmalloc_size(c->argv) : 0);
    size_t out_usage = getClientOutputBufferMemoryUsage(c);

    /* Track the biggest values observed so far in this slot. */
    if (in_usage > ClientsPeakMemInput[time_idx])
        ClientsPeakMemInput[time_idx] = in_usage;
    if (out_usage > ClientsPeakMemOutput[time_idx])
        ClientsPeakMemOutput[time_idx] = out_usage;

    return 0; /* This function never terminates the client. */
}

/* All normal clients are placed in one of the "mem usage buckets" according
 * to how much memory they currently use. We use this function to find the
 * appropriate bucket based on a given memory usage value. The algorithm simply
 * does a log2(mem) to ge the bucket. This means, for examples, that if a
 * client's memory usage doubles it's moved up to the next bucket, if it's
 * halved we move it down a bucket.
 * For more details see CLIENT_MEM_USAGE_BUCKETS documentation in server.h. */
static inline clientMemUsageBucket *getMemUsageBucket(size_t mem) {
    int size_in_bits = 8 * (int)sizeof(mem);
    int clz = mem > 0 ? __builtin_clzl(mem) : size_in_bits;
    int bucket_idx = size_in_bits - clz;
    if (bucket_idx > CLIENT_MEM_USAGE_BUCKET_MAX_LOG)
        bucket_idx = CLIENT_MEM_USAGE_BUCKET_MAX_LOG;
    else if (bucket_idx < CLIENT_MEM_USAGE_BUCKET_MIN_LOG)
        bucket_idx = CLIENT_MEM_USAGE_BUCKET_MIN_LOG;
    bucket_idx -= CLIENT_MEM_USAGE_BUCKET_MIN_LOG;
    return &server.client_mem_usage_buckets[bucket_idx];
}

/* This is called both on explicit clients when something changed their buffers,
 * so we can track clients' memory and enforce clients' maxmemory in real time,
 * and also from the clientsCron. We call it from the cron so we have updated
 * stats for non CLIENT_TYPE_NORMAL/PUBSUB clients and in case a configuration
 * change requires us to evict a non-active client.
 *
 * This also adds the client to the correct memory usage bucket. Each bucket contains
 * all clients with roughly the same amount of memory. This way we group
 * together clients consuming about the same amount of memory and can quickly
 * free them in case we reach maxmemory-clients (client eviction).
 */
int updateClientMemUsage(client *c) {
    serverAssert(io_threads_op == IO_THREADS_OP_IDLE);
    size_t mem = getClientMemoryUsage(c, NULL);
    int type = getClientType(c);

    /* Remove the old value of the memory used by the client from the old
     * category, and add it back. */
    if (type != c->last_memory_type) {
        server.stat_clients_type_memory[c->last_memory_type] -= c->last_memory_usage;
        server.stat_clients_type_memory[type] += mem;
        c->last_memory_type = type;
    }
    else {
        server.stat_clients_type_memory[type] += mem - c->last_memory_usage;
    }

    int allow_eviction = (type == CLIENT_TYPE_NORMAL || type == CLIENT_TYPE_PUBSUB) && !(c->flags & CLIENT_NO_EVICT);

    /* Update the client in the mem usage buckets */
    if (c->mem_usage_bucket) {
        c->mem_usage_bucket->mem_usage_sum -= c->last_memory_usage;
        /* If this client can't be evicted then remove it from the mem usage
         * buckets */
        if (!allow_eviction) {
            listDelNode(c->mem_usage_bucket->clients, c->mem_usage_bucket_node);
            c->mem_usage_bucket = NULL;
            c->mem_usage_bucket_node = NULL;
        }
    }
    if (allow_eviction) {
        clientMemUsageBucket *bucket = getMemUsageBucket(mem);
        bucket->mem_usage_sum += mem;
        if (bucket != c->mem_usage_bucket) {
            if (c->mem_usage_bucket)
                listDelNode(c->mem_usage_bucket->clients, c->mem_usage_bucket_node);
            c->mem_usage_bucket = bucket;
            listAddNodeTail(bucket->clients, c);
            c->mem_usage_bucket_node = listLast(bucket->clients);
        }
    }

    /* Remember what we added, to remove it next time. */
    c->last_memory_usage = mem;

    return 0;
}

/* Return the max samples in the memory usage of clients tracked by
 * the function clientsCronTrackExpansiveClients(). */
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

/* This function is called by serverCron() and is used in order to perform
 * operations on clients that are important to perform constantly. For instance
 * we use this function in order to disconnect clients after a timeout, including
 * clients blocked in some blocking command with a non-zero timeout.
 *
 * The function makes some effort to process all the clients every second, even
 * if this cannot be strictly guaranteed, since serverCron() may be called with
 * an actual frequency lower than server.hz in case of latency events like slow
 * commands.
 *
 * It is very important for this function, and the functions it calls, to be
 * very fast: sometimes Redis has tens of hundreds of connected clients, and the
 * default server.hz value is 10, so sometimes here we need to process thousands
 * of clients per second, turning this function into a source of latency.
 */
#define CLIENTS_CRON_MIN_ITERATIONS 5

void clientsCron(void) {
    // 这个函数每次执行都会处理至少 numclients/server.hz 个客户端.
    // 因为通常情况下（如果没有大的延迟事件）,这个函数每秒被调用server.hz次,在平均情况下,我们在1秒内处理所有的客户端.
    int numclients = listLength(server.clients); // 客户端数量

    int iterations = numclients / server.hz; // 要处理的客户端数量

    mstime_t now = mstime();

    /* Process at least a few clients while we are at it, even if we need
     * to process less than CLIENTS_CRON_MIN_ITERATIONS to meet our contract
     * of processing each client once per second. */
    if (iterations < CLIENTS_CRON_MIN_ITERATIONS) // 至少要处理 5 个客户端
        iterations = (numclients < CLIENTS_CRON_MIN_ITERATIONS) ? numclients : CLIENTS_CRON_MIN_ITERATIONS;

    int curr_peak_mem_usage_slot = server.unixtime % CLIENTS_PEAK_MEM_USAGE_SLOTS;
    /* Always zero the next sample, so that when we switch to that second, we'll
     * only register samples that are greater in that second without considering
     * the history of such slot.
     *
     * Note: our index may jump to any random position if serverCron() is not
     * called for some reason with the normal frequency, for instance because
     * some slow command is called taking multiple seconds to execute. In that
     * case our array may end containing data which is potentially older
     * than CLIENTS_PEAK_MEM_USAGE_SLOTS seconds: however this is not a problem
     * since here we want just to track if "recently" there were very expansive
     * clients from the POV of memory usage. */
    int zeroidx = (curr_peak_mem_usage_slot + 1) % CLIENTS_PEAK_MEM_USAGE_SLOTS;
    ClientsPeakMemInput[zeroidx] = 0;
    ClientsPeakMemOutput[zeroidx] = 0;

    while (listLength(server.clients) && iterations--) {
        client *c;
        listNode *head;

        /* Rotate the list, take the current head, process.
         * This way if the client must be removed from the list it's the
         * first element and we don't incur into O(N) computation. */
        // 翻转列表,然后取出表头元素,这样一来上一个被处理的客户端会被放到表头
        // 另外,如果程序要删除当前客户端,那么只要删除表头元素就可以了
        listRotateTailToHead(server.clients);
        head = listFirst(server.clients);
        c = listNodeValue(head);
        /* The following functions do different service checks on the client.
         * The protocol is that they return non-zero if the client was
         * terminated. */
        // 检查客户端,并在客户端超时时关闭它
        if (clientsCronHandleTimeout(c, now))
            continue;
        // 根据情况,缩小客户端查询缓冲区的大小
        if (clientsCronResizeQueryBuffer(c))
            continue;
        // 根据情况,缩小客户端输出缓冲区的大小
        if (clientsCronResizeOutputBuffer(c, now))
            continue;

        if (clientsCronTrackExpansiveClients(c, curr_peak_mem_usage_slot))
            continue;

        /* Iterating all the clients in getMemoryOverheadData() is too slow and
         * in turn would make the INFO command too slow. So we perform this
         * computation incrementally and track the (not instantaneous but updated
         * to the second) total memory used by clients using clientsCron() in
         * a more incremental way (depending on server.hz). */
        if (updateClientMemUsage(c))
            continue;
        if (closeClientOnOutputBufferLimitReached(c, 0))
            continue;
    }
}

/* This function handles 'background' operations we are required to do
 * incrementally in Redis databases, such as active key expiring, resizing,
 * rehashing. */
// 对数据库执行删除过期键,调整大小,以及主动和渐进式 rehash
void databasesCron(void) {
    // 函数先从数据库中删除过期键,然后再对数据库的大小进行修改
    /* Expire keys by random sampling. Not required for slaves
     * as master will synthesize DELs for us. */
    // 如果服务器不是从服务器,那么执行主动过期键清除
    if (server.active_expire_enabled) {
        if (iAmMaster()) {
            // 清除模式为 CYCLE_SLOW ,这个模式会尽量多清除过期键
            activeExpireCycle(ACTIVE_EXPIRE_CYCLE_SLOW);
        }
        else {
            expireSlaveKeys();
        }
    }

    /* Defrag keys gradually. */
    activeDefragCycle();

    /* Perform hash tables rehashing if needed, but only if there are no
     * other processes saving the DB on disk. Otherwise rehashing is bad
     * as will cause a lot of copy-on-write of memory pages. */
    // 在没有 BGSAVE 或者 BGREWRITEAOF 执行时,对哈希表进行 rehash
    if (!hasActiveChildProcess()) {
        /* We use global counters so if we stop the computation at a given
         * DB we'll be able to start from the successive in the next
         * cron loop iteration. */
        static unsigned int resize_db = 0;
        static unsigned int rehash_db = 0;
        int dbs_per_call = CRON_DBS_PER_CALL;
        int j;

        /* Don't test more DBs than we have. */
        // 设定要测试的数据库数量

        if (dbs_per_call > server.dbnum)
            dbs_per_call = server.dbnum;

        /* Resize */
        // 调整字典的大小

        for (j = 0; j < dbs_per_call; j++) {
            tryResizeHashTables(resize_db % server.dbnum);
            resize_db++;
        }

        /* Rehash */
        // 对字典进行渐进式 rehash

        if (server.active_rehashing) {
            for (j = 0; j < dbs_per_call; j++) {
                int work_done = incrementallyRehash(rehash_db);
                if (work_done) {
                    /* If the function did some work, stop here, we'll do
                     * more at the next cron loop. */
                    break;
                }
                else {
                    /* If this db didn't need rehash, we'll try the next one. */
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
    atomicSet(server.unixtime, unix_time);

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

void checkChildrenDone(void) {
    int statloc = 0;
    pid_t pid;
    // 接收子进程发来的信号,非阻塞
    if ((pid = waitpid(-1, &statloc, WNOHANG)) != 0) {
        int exitcode = WIFEXITED(statloc) ? WEXITSTATUS(statloc) : -1;
        int bysignal = 0;

        if (WIFSIGNALED(statloc)) {
            bysignal = WTERMSIG(statloc);
        }

        /* sigKillChildHandler catches the signal and calls exit(), but we
         * must make sure not to flag lastbgsave_status, etc incorrectly.
         * We could directly terminate the child process via SIGUSR1
         * without handling it */
        if (exitcode == SERVER_CHILD_NOERROR_RETVAL) {
            bysignal = SIGUSR1;
            exitcode = 1;
        }

        if (pid == -1) {
            serverLog(LL_WARNING, "waitpid() returned an error: %s.  child_type: %s, child_pid = %d", strerror(errno), strChildType(server.child_type), (int)server.child_pid);
        }
        else if (pid == server.child_pid) {
            if (server.child_type == CHILD_TYPE_RDB) { // BGSAVE 执行完毕

                backgroundSaveDoneHandler(exitcode, bysignal);
            }
            else if (server.child_type == CHILD_TYPE_AOF) { // BGREWRITEAOF 执行完毕

                backgroundRewriteDoneHandler(exitcode, bysignal);
            }
            else if (server.child_type == CHILD_TYPE_MODULE) {
                ModuleForkDoneHandler(exitcode, bysignal);
            }
            else {
                serverPanic("Unknown child type %d for child pid %d", server.child_type, server.child_pid);
                exit(1);
            }
            if (!bysignal && exitcode == 0)
                receiveChildInfo();
            resetChildState();
        }
        else {
            if (!ldbRemoveChild(pid)) {
                serverLog(LL_WARNING, "Warning, detected child with unmatched pid: %ld", (long)pid);
            }
        }

        /* start any pending forks immediately. */
        replicationStartPendingFork();
    }
}

/* Called from serverCron and cronUpdateMemoryStats to update cached memory metrics. */
void cronUpdateMemoryStats() {
    /* Record the max memory used since the server was started. */
    if (zmalloc_used_memory() > server.stat_peak_memory)
        server.stat_peak_memory = zmalloc_used_memory();

    run_with_period(100) {
        /* Sample the RSS and other metrics here since this is a relatively slow call.
         * We must sample the zmalloc_used at the same time we take the rss, otherwise
         * the frag ratio calculate may be off (ratio of two samples at different times) */
        server.cron_malloc_stats.process_rss = zmalloc_get_rss();
        server.cron_malloc_stats.zmalloc_used = zmalloc_used_memory();
        /* Sampling the allocator info can be slow too.
         * The fragmentation ratio it'll show is potentially more accurate
         * it excludes other RSS pages such as: shared libraries, LUA and other non-zmalloc
         * allocations, and allocator reserved pages that can be pursed (all not actual frag) */
        zmalloc_get_allocator_info(&server.cron_malloc_stats.allocator_allocated, &server.cron_malloc_stats.allocator_active, &server.cron_malloc_stats.allocator_resident);
        /* in case the allocator isn't providing these stats, fake them so that
         * fragmentation info still shows some (inaccurate metrics) */
        if (!server.cron_malloc_stats.allocator_resident) {
            /* LUA memory isn't part of zmalloc_used, but it is part of the process RSS,
             * so we must deduct it in order to be able to calculate correct
             * "allocator fragmentation" ratio */
            size_t lua_memory = evalMemory();
            server.cron_malloc_stats.allocator_resident = server.cron_malloc_stats.process_rss - lua_memory;
        }
        if (!server.cron_malloc_stats.allocator_active)
            server.cron_malloc_stats.allocator_active = server.cron_malloc_stats.allocator_resident;
        if (!server.cron_malloc_stats.allocator_allocated)
            server.cron_malloc_stats.allocator_allocated = server.cron_malloc_stats.zmalloc_used;
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
    //    printf("serverCron ---> %lld\n", ustime());
    int j;
    UNUSED(eventLoop);
    UNUSED(id);
    UNUSED(clientData);

    /* Software watchdog: . */
    // 如果我们没有足够快地返回这里,交付将到达信号处理程序的SIGALRM
    if (server.watchdog_period) {
        watchdogScheduleSignal(server.watchdog_period);
    }

    // 更新server的时间
    updateCachedTime(1);

    server.hz = server.config_hz;
    /* Adapt the server.hz value to the number of configured clients. If we have
     * many clients, we want to call serverCron() with an higher frequency. */
    if (server.dynamic_hz) {
        while (listLength(server.clients) / server.hz > MAX_CLIENTS_PER_CLOCK_TICK) {
            server.hz *= 2;
            if (server.hz > CONFIG_MAX_HZ) {
                server.hz = CONFIG_MAX_HZ;
                break;
            }
        }
    }

    /* for debug purposes: skip actual cron work if pause_cron is on */
    if (server.pause_cron)
        return 1000 / server.hz;
    // 记录服务器执行命令的次数
    run_with_period(100) {
        long long stat_net_input_bytes, stat_net_output_bytes;
        atomicGet(server.stat_net_input_bytes, stat_net_input_bytes);
        atomicGet(server.stat_net_output_bytes, stat_net_output_bytes);

        trackInstantaneousMetric(STATS_METRIC_COMMAND, server.stat_numcommands);
        trackInstantaneousMetric(STATS_METRIC_NET_INPUT, stat_net_input_bytes);
        trackInstantaneousMetric(STATS_METRIC_NET_OUTPUT, stat_net_output_bytes);
    }
    unsigned int lruclock = getLRUClock();
    atomicSet(server.lru_clock, lruclock); // 初始化 LRU 时间

    cronUpdateMemoryStats();

    /* We received a SIGTERM or SIGINT, shutting down here in a safe way, as it is
     * not ok doing so inside the signal handler. */
    // 如果收到进程结束信号,则执行server关闭操作
    if (server.shutdown_asap && !isShutdownInitiated()) {
        int shutdownFlags = SHUTDOWN_NOFLAGS;
        if (server.last_sig_received == SIGINT && server.shutdown_on_sigint)
            shutdownFlags = server.shutdown_on_sigint;
        else if (server.last_sig_received == SIGTERM && server.shutdown_on_sigterm)
            shutdownFlags = server.shutdown_on_sigterm;

        if (prepareForShutdown(shutdownFlags) == C_OK)
            exit(0);
    }
    else if (isShutdownInitiated()) {
        if (server.mstime >= server.shutdown_mstime || isReadyToShutdown()) {
            if (finishShutdown() == C_OK)
                exit(0);
            /* Shutdown failed. Continue running. An error has been logged. */
        }
    }

    /* Show some info about non-empty databases */
    // 打印数据库的键值对信息
    if (server.verbosity <= LL_VERBOSE) {
        run_with_period(5000) {
            for (j = 0; j < server.dbnum; j++) {
                long long size, used, vkeys;
                size = dictSlots(server.db[j].dict);    // 可用键值对的数量
                used = dictSize(server.db[j].dict);     // 已用键值对的数量
                vkeys = dictSize(server.db[j].expires); // 带有过期时间的键值对数量

                // 用 LOG 打印数量
                if (used || vkeys) {
                    serverLog(LL_VERBOSE, "DB %d: %lld keys (%lld volatile) in %lld slots HT.", j, used, vkeys, size);
                }
            }
        }
    }

    /* Show information about connected clients */
    // 如果服务器没有运行在 SENTINEL 模式下,那么打印客户端的连接信息
    if (!server.sentinel_mode) {
        run_with_period(5000) {
            serverLog(LL_DEBUG, "%lu clients connected (%lu replicas), %zu bytes in use", listLength(server.clients) - listLength(server.slaves), listLength(server.slaves), zmalloc_used_memory());
        }
    }

    clientsCron();   // 执行客户端的异步操作   检查客户端,关闭超时客户端,并释放客户端多余的缓冲区
    databasesCron(); // 执行数据库的后台操作

    /* Start a scheduled AOF rewrite if this was requested by the user while
     * a BGSAVE was in progress. */

    // 如果 BGSAVE 和 BGREWRITEAOF 都没有在执行
    // 并且有一个 BGREWRITEAOF 在等待,那么执行 BGREWRITEAOF
    if (!hasActiveChildProcess() && server.aof_rewrite_scheduled && !aofRewriteLimited()) {
        rewriteAppendOnlyFileBackground();
    }

    /* Check if a background saving or AOF rewrite in progress terminated. */
    // 检查 BGSAVE 或者 BGREWRITEAOF 是否已经执行完毕
    if (hasActiveChildProcess() || ldbPendingChildren()) {
        run_with_period(1000) receiveChildInfo();
        checkChildrenDone();
    }
    else {
        /* If there is not a background saving/rewrite in progress check if
         * we have to save/rewrite now. */
        // 既然没有 BGSAVE 或者 BGREWRITEAOF 在执行,那么检查是否需要执行它们

        // 遍历所有保存条件,看是否需要执行 BGSAVE 命令
        for (j = 0; j < server.saveparamslen; j++) {
            struct saveparam *sp = server.saveparams + j;

            /* Save if we reached the given amount of changes,
             * the given amount of seconds, and if the latest bgsave was
             * successful or if, in case of an error, at least
             * CONFIG_BGSAVE_RETRY_DELAY seconds already elapsed. */
            // 检查是否有某个保存条件已经满足了
            if (server.dirty >= sp->changes && server.unixtime - server.lastsave > sp->seconds && (server.unixtime - server.lastbgsave_try > CONFIG_BGSAVE_RETRY_DELAY || server.lastbgsave_status == C_OK)) {
                serverLog(LL_NOTICE, "%d changes in %d seconds. Saving...", sp->changes, (int)sp->seconds);
                rdbSaveInfo rsi, *rsiptr;
                rsiptr = rdbPopulateSaveInfo(&rsi);
                // 执行 BGSAVE
                rdbSaveBackground(SLAVE_REQ_NONE, server.rdb_filename, rsiptr);
                break;
            }
        }

        // 如果AOF功能启用、没有RDB子进程和AOF重写子进程在执行、AOF文件大小比例设定了阈值,
        // 以及AOF文件大小绝对值超出了阈值,那么,进一步判断AOF文件大小比例是否超出阈值
        if (server.aof_state == AOF_ON && !hasActiveChildProcess() && server.aof_rewrite_perc &&
            // AOF 文件的当前大小大于执行 BGREWRITEAOF 所需的最小大小
            server.aof_current_size > server.aof_rewrite_min_size) {
            // 计算AOF文件当前大小超出基础大小的比例
            long long base = server.aof_rewrite_base_size ? server.aof_rewrite_base_size : 1;
            // AOF 文件当前的体积相对于 base 的体积的百分比

            long long growth = (server.aof_current_size * 100 / base) - 100;
            // 如果增长体积的百分比超过了 growth ,那么执行 BGREWRITEAOF
            // 如果AOF文件当前大小超出基础大小的比例已经超出预设阈值,那么执行AOF重写
            if (growth >= server.aof_rewrite_perc && !aofRewriteLimited()) {
                serverLog(LL_NOTICE, "Starting automatic rewriting of AOF on %lld%% growth", growth);
                // 执行 BGREWRITEAOF
                // AOF 功能已启用、AOF 文件大小比例超出阈值,以及 AOF 文件大小绝对值超出阈值
                rewriteAppendOnlyFileBackground();
            }
        }
    }
    /* Just for the sake of defensive programming, to avoid forgetting to
     * call this function when needed. */
    updateDictResizePolicy();

    // 根据 AOF 政策,
    // 考虑是否需要将 AOF 缓冲区中的内容写入到 AOF 文件中
    /* AOF postponed flush: Try at every cron cycle if the slow fsync
     * completed. */
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

    /* Clear the paused clients state if needed. */
    checkClientPauseTimeoutAndReturnIfPaused();

    /* Replication cron function -- used to reconnect to master,
     * detect transfer failures, start background RDB transfers and so forth.
     *
     * If Redis is trying to failover then run the replication cron faster so
     * progress on the handshake happens more quickly. */
    // 复制函数
    // 重连接主服务器、向主服务器发送 ACK 、判断数据发送失败情况、断开本服务器超时的从服务器,等等

    if (server.failover_state != NO_FAILOVER) {
        // 故障转移情况下,100ms执行一次
        run_with_period(100) replicationCron();
    }
    else {
        // 正常情况下,1s执行一次
        run_with_period(1000) replicationCron();
    }

    /* Run the Redis Cluster cron. */
    // 集群模式下 每执行 10 次（至少间隔一秒钟），就向一个随机节点发送 gossip 信息
    run_with_period(100) {
        if (server.cluster_enabled) {
            // 如果服务器运行在集群模式下,那么执行集群操作
            clusterCron();
        }
    }

    /* Run the Sentinel timer if we are in sentinel mode. */
    if (server.sentinel_mode) {
        // 如果服务器运行在 sentinel 模式下,那么执行 SENTINEL 的主函数
        sentinelTimer();
    }

    /* Cleanup expired MIGRATE cached sockets. */
    // 集群...TODO
    run_with_period(1000) {
        migrateCloseTimedoutSockets();
    }

    // 如果没有阻塞的工作,停止子线程
    stopThreadedIOIfNeeded();

    /* Resize tracking keys table if needed. This is also done at every
     * command execution, but we want to be sure that if the last command
     * executed changes the value via CONFIG SET, the server will perform
     * the operation even if completely idle. */
    if (server.tracking_clients)
        trackingLimitUsedSlots();

    /* Start a scheduled BGSAVE if the corresponding flag is set. This is
     * useful when we are forced to postpone a BGSAVE because an AOF
     * rewrite is in progress.
     *
     * Note: this code must be after the replicationCron() call above so
     * make sure when refactoring this file to keep this order. This is useful
     * because we want to give priority to RDB savings for replication. */
    if (!hasActiveChildProcess() && server.rdb_bgsave_scheduled && (server.unixtime - server.lastbgsave_try > CONFIG_BGSAVE_RETRY_DELAY || server.lastbgsave_status == C_OK)) {
        rdbSaveInfo rsi, *rsiptr;
        rsiptr = rdbPopulateSaveInfo(&rsi);
        if (rdbSaveBackground(SLAVE_REQ_NONE, server.rdb_filename, rsiptr) == C_OK)
            server.rdb_bgsave_scheduled = 0;
    }

    run_with_period(100) {
        if (moduleCount())
            modulesCron();
    }

    /* Fire the cron loop modules event. */
    RedisModuleCronLoopV1 ei = {REDISMODULE_CRON_LOOP_VERSION, server.hz};
    moduleFireServerEvent(REDISMODULE_EVENT_CRON_LOOP, 0, &ei);

    server.cronloops++; // 增加 loop 计数器

    return 1000 / server.hz;
}

void blockingOperationStarts() {
    if (!server.blocking_op_nesting++) {
        updateCachedTime(0);
        server.blocked_last_cron = server.mstime;
    }
}

void blockingOperationEnds() {
    if (!(--server.blocking_op_nesting)) {
        server.blocked_last_cron = 0;
    }
}

/* This function fills in the role of serverCron during RDB or AOF loading, and
 * also during blocked scripts.
 * It attempts to do its duties at a similar rate as the configured server.hz,
 * and updates cronloops variable so that similarly to serverCron, the
 * run_with_period can be used. */
void whileBlockedCron() {
    /* Here we may want to perform some cron jobs (normally done server.hz times
     * per second). */

    /* Since this function depends on a call to blockingOperationStarts, let's
     * make sure it was done. */
    serverAssert(server.blocked_last_cron);

    /* In case we where called too soon, leave right away. This way one time
     * jobs after the loop below don't need an if. and we don't bother to start
     * latency monitor if this function is called too often. */
    if (server.blocked_last_cron >= server.mstime)
        return;

    mstime_t latency;
    latencyStartMonitor(latency);

    /* In some cases we may be called with big intervals, so we may need to do
     * extra work here. This is because some of the functions in serverCron rely
     * on the fact that it is performed every 10 ms or so. For instance, if
     * activeDefragCycle needs to utilize 25% cpu, it will utilize 2.5ms, so we
     * need to call it multiple times. */
    long hz_ms = 1000 / server.hz;
    while (server.blocked_last_cron < server.mstime) {
        /* Defrag keys gradually. */
        activeDefragCycle();

        server.blocked_last_cron += hz_ms;

        /* Increment cronloop so that run_with_period works. */
        server.cronloops++;
    }

    /* Other cron jobs do not need to be done in a loop. No need to check
     * server.blocked_last_cron since we have an early exit at the top. */

    /* Update memory stats during loading (excluding blocked scripts) */
    if (server.loading)
        cronUpdateMemoryStats();

    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("while-blocked-cron", latency);

    /* We received a SIGTERM during loading, shutting down here in a safe way,
     * as it isn't ok doing so inside the signal handler. */
    // 服务器进程收到 SIGTERM 信号,关闭服务器
    if (server.shutdown_asap && server.loading) {
        // 尝试关闭服务器
        if (prepareForShutdown(SHUTDOWN_NOSAVE) == C_OK) {
            exit(0);
        }
        // 如果关闭失败,那么打印 LOG ,并移除关闭标识
        serverLog(LL_WARNING, "SIGTERM received but errors trying to shut down the server, check the logs for more information");
        server.shutdown_asap = 0;
        server.last_sig_received = 0;
    }
}

static void sendGetackToReplicas(void) {
    robj *argv[3];
    argv[0] = shared.replconf;
    argv[1] = shared.getack;
    argv[2] = shared.special_asterick; /* 未用到的参数 */
    replicationFeedSlaves(server.slaves, server.slaveseldb, argv, 3);
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

    //    逐步释放 环形缓冲复制队列 ,10倍的正常速度是为了尽可能地释放
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

/* This function is called immediately after the event loop multiplexing
 * API returned, and the control is going to soon return to Redis by invoking
 * the different events callbacks. */
void afterSleep(struct aeEventLoop *eventLoop) {
    UNUSED(eventLoop);

    /* Do NOT add anything above moduleAcquireGIL !!! */

    /* Acquire the modules GIL so that their threads won't touch anything. */
    if (!ProcessingEventsWhileBlocked) {
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
    shared.roslaveerr = createObject(OBJ_STRING, sdsnew("-READONLY You can't write against a read only replica.\r\n"));
    shared.noautherr = createObject(OBJ_STRING, sdsnew("-NOAUTH Authentication required.\r\n"));
    shared.oomerr = createObject(OBJ_STRING, sdsnew("-OOM command not allowed when used memory > 'maxmemory'.\r\n"));
    shared.execaborterr = createObject(OBJ_STRING, sdsnew("-EXECABORT Transaction discarded because of previous errors.\r\n"));
    shared.noreplicaserr = createObject(OBJ_STRING, sdsnew("-NOREPLICAS Not enough good replicas to write.\r\n"));
    shared.busykeyerr = createObject(OBJ_STRING, sdsnew("-BUSYKEY Target key name already exists.\r\n"));

    /* The shared NULL depends on the protocol version. */
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

    // 常用 SELECT 命令
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

    /* Shared command argument */
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
    /* The following two shared objects, minstring and maxstring, are not
     * actually used for their value but as a special object meaning
     * respectively the minimum possible string and the maximum possible
     * string in string comparisons for the ZRANGEBYLEX command. */
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
        //  "*", "-::*"
        server.bindaddr[j] = zstrdup(default_bindaddr[j]); // 字长与字节对齐 CPU一次性 能读取数据的二进制位数称为字长,也就是我们通常所说的32位系统(字长4个字节)、64位系统(字长8个字节)的由来
    }
    server.ipfd.count = 0;
    server.tlsfd.count = 0;
    server.sofd = -1;
    server.active_expire_enabled = 1;
    server.skip_checksum_validation = 0;
    server.loading = 0;
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
    server.commands = dictCreate(&commandTableDictType);
    server.orig_commands = dictCreate(&commandTableDictType);

    populateCommandTable(); // 开始填充Redis命令表.  redisCommandTable

    /* 调试 */
    server.watchdog_period = 0;
}

extern char **environ;

/* Restart the server, executing the same executable that started this
 * instance, with the same arguments and configuration file.
 *
 * The function is designed to directly call execve() so that the new
 * server instance will retain the PID of the previous one.
 *
 * The list of flags, that may be bitwise ORed together, alter the
 * behavior of this function:
 *
 * RESTART_SERVER_NONE              No flags.
 * RESTART_SERVER_GRACEFULLY        Do a proper shutdown before restarting.
 * RESTART_SERVER_CONFIG_REWRITE    Rewrite the config file before restarting.
 *
 * On success the function does not return, because the process turns into
 * a different process. On error C_ERR is returned. */
int restartServer(int flags, mstime_t delay) {
    int j;

    /* Check if we still have accesses to the executable that started this
     * server instance. */
    if (access(server.executable, X_OK) == -1) {
        serverLog(
            LL_WARNING,
            "Can't restart: this process has no "
            "permissions to execute %s",
            server.executable);
        return C_ERR;
    }

    /* Config rewriting. */
    if (flags & RESTART_SERVER_CONFIG_REWRITE && server.configfile && rewriteConfig(server.configfile, 0) == -1) {
        serverLog(
            LL_WARNING,
            "Can't restart: configuration rewrite process "
            "failed: %s",
            strerror(errno));
        return C_ERR;
    }

    /* Perform a proper shutdown. We don't wait for lagging replicas though. */
    if (flags & RESTART_SERVER_GRACEFULLY && prepareForShutdown(SHUTDOWN_NOW) != C_OK) {
        serverLog(LL_WARNING, "Can't restart: error preparing for shutdown");
        return C_ERR;
    }

    /* Close all file descriptors, with the exception of stdin, stdout, stderr
     * which are useful if we restart a Redis server which is not daemonized. */
    for (j = 3; j < (int)server.maxclients + 1024; j++) {
        /* Test the descriptor validity before closing it, otherwise
         * Valgrind issues a warning on close(). */
        if (fcntl(j, F_GETFD) != -1)
            close(j);
    }

    /* Execute the server with the original command line. */
    if (delay)
        usleep(delay * 1000);
    zfree(server.exec_argv[0]);
    server.exec_argv[0] = zstrdup(server.executable);
    execve(server.executable, server.exec_argv, environ);

    /* If an error occurred here, there is nothing we can do, but exit. */
    _exit(1);

    return C_ERR; /* Never reached. */
}

/*
这个函数将根据.配置当前进程的oom_score_adj到用户指定的配置.这是目前在Linux上实现的
process_class值为-1表示OOM_CONFIG_MASTER或OOM_CONFIG_REPLICA,取决于当前的角色.
 */
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

                /* We failed to set file limit to 'bestlimit'. Try with a
                 * smaller limit decrementing by a few FDs per iteration. */
                if (bestlimit < decr_step)
                    break;
                bestlimit -= decr_step;
            }

            /* Assume that the limit we get initially is still valid if
             * our last try was even lower. */
            if (bestlimit < oldlimit)
                bestlimit = oldlimit;

            if (bestlimit < maxfiles) {
                unsigned int old_maxclients = server.maxclients;
                server.maxclients = bestlimit - CONFIG_MIN_RESERVED_FDS;
                /* maxclients is unsigned so may overflow: in order
                 * to check if maxclients is now logically less than 1
                 * we test indirectly via bestlimit. */
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

// 检查tcp_backlog和系统的somaxconn参数值
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
            serverLog(LL_WARNING, "WARNING: The TCP backlog setting of %d cannot be enforced because kern.ipc.somaxconn is set to the lower value of %d.", server.tcp_backlog, somaxconn);
        }
    }
#elif defined(HAVE_SYSCTL_KERN_SOMAXCONN)
    int somaxconn, mib[2];
    size_t len = sizeof(int);

    mib[0] = CTL_KERN;
    mib[1] = KERN_SOMAXCONN;

    if (sysctl(mib, 2, &somaxconn, &len, NULL, 0) == 0) {
        if (somaxconn > 0 && somaxconn < server.tcp_backlog) {
            serverLog(LL_WARNING, "WARNING: The TCP backlog setting of %d cannot be enforced because kern.somaxconn is set to the lower value of %d.", server.tcp_backlog, somaxconn);
        }
    }
#elif defined(SOMAXCONN)
    if (SOMAXCONN < server.tcp_backlog) {
        serverLog(LL_WARNING, "WARNING: The TCP backlog setting of %d cannot be enforced because SOMAXCONN is set to the lower value of %d.", server.tcp_backlog, SOMAXCONN);
    }
#endif
}
// 关闭套接字
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

    //  如果我们没有绑定地址,我们就不在TCP套接字上监听.
    if (server.bindaddr_count == 0)
        return C_OK;
    // tcp6       0      0  *.6379                 *.*                    LISTEN
    // tcp4       0      0  *.6379                 *.*                    LISTEN
    for (j = 0; j < server.bindaddr_count; j++) { // 2
        char *addr = bindaddr[j];
        //  "*", "-::*"
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
        anetNonBlock(NULL, sfd->fd[sfd->count]);
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
    makeThreadKillable();  // ok

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
        server.db[j].slots_to_keys = NULL; /* Set by clusterInit later on if necessary. */
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
    server.lastsave = time(NULL); /* At startup we consider the DB saved. */
    server.lastbgsave_try = 0;    /* At startup we never tried to BGSAVE. */
    server.rdb_save_time_last = -1;
    server.rdb_save_time_start = -1;
    server.rdb_last_load_keys_expired = 0;
    server.rdb_last_load_keys_loaded = 0;
    server.dirty = 0;
    resetServerStats(); // 重置server运行状态信息
    /* A few stats we don't want to reset: server startup time, and peak mem. */
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

    // 为server后台任务创建定时事件,1秒一次,定时事件为 serverCron() 创建时间事件
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

/* The purpose of this function is to try to "glue" consecutive range
 * key specs in order to build the legacy (first,last,step) spec
 * used by the COMMAND command.
 * By far the most common case is just one range spec (e.g. SET)
 * but some commands' ranges were split into two or more ranges
 * in order to have different flags for different keys (e.g. SMOVE,
 * first key is "read write", second key is "write").
 *
 * This functions uses very basic heuristics and is "best effort":
 * 1. Only commands which have only "range" specs are considered.
 * 2. Only range specs with keystep of 1 are considered.
 * 3. The order of the range specs must be ascending (i.e.
 *    lastkey of spec[i] == firstkey-1 of spec[i+1]).
 *
 * This function will succeed on all native Redis commands and may
 * fail on module commands, even if it only has "range" specs that
 * could actually be "glued", in the following cases:
 * 1. The order of "range" specs is not ascending (e.g. the spec for
 *    the key at index 2 was added before the spec of the key at
 *    index 1).
 * 2. The "range" specs have keystep >1.
 *
 * If this functions fails it means that the legacy (first,last,step)
 * spec used by COMMAND will show 0,0,0. This is not a dire situation
 * because anyway the legacy (first,last,step) spec is to be deprecated
 * and one should use the new key specs scheme.
 */
void populateCommandLegacyRangeSpec(struct redisCommand *c) {
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

sds catSubCommandFullname(const char *parent_name, const char *sub_name) {
    return sdscatfmt(sdsempty(), "%s|%s", parent_name, sub_name);
}

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

/* Recursively populate the args structure (setting num_args to the number of
 * subargs) and return the number of args. */
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

// * 重置命令表中的统计信息
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
    oa->ops = NULL;
    oa->numops = 0;
    oa->capacity = 0;
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
    op = oa->ops + oa->numops;
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
        //        目前我们只支持一层子命令
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

/* Lookup the command in the current table, if not found also check in
 * the original table containing the original command names unaffected by
 * redis.conf rename-command statement.
 *
 * This is used by functions rewriting the argument vector such as
 * rewriteClientCommandVector() in order to set client->cmd pointer
 * correctly even if the command was renamed. */
// * 从当前命令表 server.commands 中查找给定名字,
// * 如果没找到的话,就尝试从 server.orig_commands 中查找未被改名的原始名字
// * 原始表中的命令名不受 redis.conf 中命令改名的影响
// * 这个函数可以在命令被更名之后,仍然在重写命令时得出正确的名字.

struct redisCommand *lookupCommandOrOriginal(robj **argv, int argc) {
    // 查找当前表

    struct redisCommand *cmd = lookupCommandLogic(server.commands, argv, argc, 0);
    // 如果有需要的话,查找原始表

    if (!cmd) {
        cmd = lookupCommandLogic(server.orig_commands, argv, argc, 0);
    }
    return cmd;
}

/* 来自master客户端或AOF客户端的命令永远不应该被拒绝. */
int mustObeyClient(client *c) {
    return c->id == CLIENT_ID_AOF || c->flags & CLIENT_MASTER;
}

static int shouldPropagate(int target) {
    if (!server.replication_allowed || target == PROPAGATE_NONE || server.loading)
        return 0;

    if (target & PROPAGATE_AOF) {
        if (server.aof_state != AOF_OFF)
            return 1;
    }
    if (target & PROPAGATE_REPL) {
        if (server.masterhost == NULL && (server.repl_backlog || listLength(server.slaves) != 0))
            return 1;
    }

    return 0;
}

/* Propagate the specified command (in the context of the specified database id)
 * to AOF and Slaves.
 *
 * 将指定命令（以及执行该命令的上下文,比如数据库 id 等信息）传播到 AOF 和 slave .
 *
 * flags are an xor between:
 * FLAG 可以是以下标识的 xor ：
 *
 * + REDIS_PROPAGATE_NONE (no propagation of command at all)
 *   不传播
 *
 * + REDIS_PROPAGATE_AOF (propagate into the AOF file if is enabled)
 *   传播到 AOF
 *
 * + REDIS_PROPAGATE_REPL (propagate into the replication link)
 *   传播到 slave
 */
static void propagateNow(int dbid, robj **argv, int argc, int target) {
    if (!shouldPropagate(target))
        return;

    /* This needs to be unreachable since the dataset should be fixed during
     * client pause, otherwise data may be lost during a failover. */
    serverAssert(!(areClientsPaused() && !server.client_pause_in_transaction));
    // 传播到 AOF

    if (server.aof_state != AOF_OFF && target & PROPAGATE_AOF)
        feedAppendOnlyFile(dbid, argv, argc);
    // 传播到 slave
    if (target & PROPAGATE_REPL)
        replicationFeedSlaves(server.slaves, dbid, argv, argc);
}

/* Used inside commands to schedule the propagation of additional commands
 * after the current command is propagated to AOF / Replication.
 *
 * dbid is the database ID the command should be propagated into.
 * Arguments of the command to propagate are passed as an array of redis
 * objects pointers of len 'argc', using the 'argv' vector.
 *
 * The function does not take a reference to the passed 'argv' vector,
 * so it is up to the caller to release the passed argv (but it is usually
 * stack allocated).  The function automatically increments ref count of
 * passed objects, so the caller does not need to. */
void alsoPropagate(int dbid, robj **argv, int argc, int target) {
    robj **argvcopy;
    int j;

    if (!shouldPropagate(target))
        return;

    argvcopy = zmalloc(sizeof(robj *) * argc);
    for (j = 0; j < argc; j++) {
        argvcopy[j] = argv[j];
        incrRefCount(argv[j]);
    }
    redisOpArrayAppend(&server.also_propagate, dbid, argvcopy, argc, target);
}

/* It is possible to call the function forceCommandPropagation() inside a
 * Redis command implementation in order to to force the propagation of a
 * specific command execution into AOF / Replication. */
void forceCommandPropagation(client *c, int flags) {
    serverAssert(c->cmd->flags & (CMD_WRITE | CMD_MAY_REPLICATE));
    if (flags & PROPAGATE_REPL)
        c->flags |= CLIENT_FORCE_REPL;
    if (flags & PROPAGATE_AOF)
        c->flags |= CLIENT_FORCE_AOF;
}

/* Avoid that the executed command is propagated at all. This way we
 * are free to just propagate what we want using the alsoPropagate()
 * API. */
void preventCommandPropagation(client *c) {
    c->flags |= CLIENT_PREVENT_PROP;
}

/* AOF specific version of preventCommandPropagation(). */
void preventCommandAOF(client *c) {
    c->flags |= CLIENT_PREVENT_AOF_PROP;
}

/* Replication specific version of preventCommandPropagation(). */
void preventCommandReplication(client *c) {
    c->flags |= CLIENT_PREVENT_REPL_PROP;
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

/* Handle the alsoPropagate() API to handle commands that want to propagate
 * multiple separated commands. Note that alsoPropagate() is not affected
 * by CLIENT_PREVENT_PROP flag. */
void propagatePendingCommands() {
    if (server.also_propagate.numops == 0)
        return;
    // 传播额外的命令
    int j;
    redisOp *rop;
    int multi_emitted = 0;

    /* Wrap the commands in server.also_propagate array,
     * but don't wrap it if we are already in MULTI context,
     * in case the nested MULTI/EXEC.
     *
     * And if the array contains only one command, no need to
     * wrap it, since the single command is atomic. */
    if (server.also_propagate.numops > 1 && !server.propagate_no_multi) {
        /* We use the first command-to-propagate to set the dbid for MULTI,
         * so that the SELECT will be propagated beforehand */
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
//      ERROR_COMMAND_REJECTED - 更新 rejected_calls
//      ERROR_COMMAND_FAILED - 更新 failed_calls
// 该函数还重置 prev_err_count,以确保我们不会计算相同的错误两次,它可能通过一个NULL cmd值,以表明错误在其他地方被计算.
// 如果统计信息更新了,函数返回true,如果没有更新则返回false.
int incrCommandStatsOnError(struct redisCommand *cmd, int flags) {
    // 保持上次执行命令时捕获的prev错误计数
    static long long prev_err_count = 0;
    int res = 0;
    if (cmd) {
        if ((server.stat_total_error_replies - prev_err_count) > 0) {
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
    // processCommand
    // RM_Call
    // execCommand
    // scriptCall
    long long dirty;
    uint64_t client_old_flags = c->flags; // 记录命令开始执行前的 FLAG
    struct redisCommand *real_cmd = c->realcmd;
    // 重置 AOF、REPL 传播标志; AOF、REPL 禁止传播标志
    c->flags &= ~(CLIENT_FORCE_AOF | CLIENT_FORCE_REPL | CLIENT_PREVENT_PROP);

    //    当call()的第一个入口点是processCommand()时,Redis的核心是负责传播.
    //    在没有processCommand作为入口点的情况下调用()的唯一其他选项是,如果模块在call()上下文之外触发RM_Call(例如,在计时器中).
    //    在这种情况下,模块负责传播.因为call()是可重入的,我们必须缓存和恢复server.core_propagates.
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
        updateCachedTimeWithUs(0, call_timer);
    }

    monotime monotonic_start = 0;
    if (monotonicGetType() == MONOTONIC_CLOCK_HW) {
        monotonic_start = getMonotonicUs();
    }

    server.in_nested_call++;
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

        /* Check if the command operated changes in the data set. If so
         * set for replication / AOF propagation. */
        // 如果数据库有被修改,那么启用 REPL 和 AOF 传播

        if (dirty) { // 有key被变更了
            propagate_flags |= (PROPAGATE_AOF | PROPAGATE_REPL);
        }

        /* If the client forced AOF / replication of the command, set
         * the flags regardless of the command effects on the data set. */
        // 强制 REPL 传播

        if (c->flags & CLIENT_FORCE_REPL) {
            propagate_flags |= PROPAGATE_REPL;
        }
        // 强制 AOF 传播

        if (c->flags & CLIENT_FORCE_AOF) {
            propagate_flags |= PROPAGATE_AOF;
        }

        /* However prevent AOF / replication propagation if the command
         * implementation called preventCommandPropagation() or similar,
         * or if we don't have the call() flags to do so. */
        if (c->flags & CLIENT_PREVENT_REPL_PROP || !(flags & CMD_CALL_PROPAGATE_REPL)) {
            propagate_flags &= ~PROPAGATE_REPL;
        }
        if (c->flags & CLIENT_PREVENT_AOF_PROP || !(flags & CMD_CALL_PROPAGATE_AOF)) {
            propagate_flags &= ~PROPAGATE_AOF;
        }

        /* Call alsoPropagate() only if at least one of AOF / replication
         * propagation is needed. */
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

/* This is called after a command in call, we can do some maintenance job in it. */
void afterCommand(client *c) {
    UNUSED(c);
    if (!server.in_nested_call) {
        /* If we are at the top-most call() we can propagate what we accumulated.
         * Should be done before trackingHandlePendingKeyInvalidations so that we
         * reply to client before invalidating cache (makes more sense) */
        if (server.core_propagates)
            propagatePendingCommands();
        /* Flush pending invalidation messages only when we are not in nested call.
         * So the messages are not interleaved with transaction response. */
        trackingHandlePendingKeyInvalidations();
    }
}

/* Returns 1 for commands that may have key names in their arguments, but the legacy range
 * spec doesn't cover all of them. */
void populateCommandMovableKeys(struct redisCommand *cmd) {
    int movablekeys = 0;
    if (cmd->getkeys_proc && !(cmd->flags & CMD_MODULE)) {
        /* Redis command with getkeys proc */
        movablekeys = 1;
    }
    else if (cmd->flags & CMD_MODULE_GETKEYS) {
        /* Module command with getkeys proc */
        movablekeys = 1;
    }
    else {
        /* Redis command without getkeys proc, but possibly has
         * movable keys because of a keys spec. */
        for (int i = 0; i < cmd->key_specs_num; i++) {
            if (cmd->key_specs[i].begin_search_type != KSPEC_BS_INDEX || cmd->key_specs[i].find_keys_type != KSPEC_FK_RANGE) {
                /* If we have a non-range spec it means we have movable keys */
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
        *err = sdscatprintf(*err, "unknown subcommand '%.128s'. Try %s HELP.", (char *)c->argv[1]->ptr, cmd);
        sdsfree(cmd);
    }
    else {
        sds args = sdsempty();
        int i;
        for (i = 1; i < c->argc && sdslen(args) < 128; i++) args = sdscatprintf(args, "'%.*s' ", 128 - (int)sdslen(args), (char *)c->argv[i]->ptr);
        *err = sdsnew(NULL);
        *err = sdscatprintf(*err, "unknown command '%.128s', with args beginning with: %s", (char *)c->argv[0]->ptr, args);
        sdsfree(args);
    }
    /* Make sure there are no newlines in the string, otherwise invalid protocol
     * is emitted (The args come from the user, they may contain any character). */
    sdsmapchars(*err, "\r\n", "  ", 2);
    return 0;
}

// 参数个数检查
// todo ,为什么参数个数要用负数存储呢？
int commandCheckArity(client *c, sds *err) {
    //  arity 命令执行需要的 参数个数 规定好的
    if ((c->cmd->arity > 0 && c->cmd->arity != c->argc) || (c->argc < -c->cmd->arity)) {
        if (err) {
            *err = sdsnew(NULL);
            *err = sdscatprintf(*err, "wrong number of arguments for '%s' command", c->cmd->fullname);
        }
        return 0;
    }

    return 1;
}

// * 这个函数执行时,我们已经读入了一个完整的命令到客户端,
// * 这个函数负责执行这个命令,
// * 或者服务器准备从客户端中进行一次读取.
// * 如果这个函数返回 1 ,那么表示客户端在执行命令之后仍然存在,
// * 调用者可以继续执行其他操作.
// * 否则,如果这个函数返回 0 ,那么表示客户端已经被销毁.
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
    printf("c->argv[0]->ptr->  %s\n", c->argv[0]->ptr); // SET
    // 处理可能的安全攻击
    if (!strcasecmp(c->argv[0]->ptr, "host:") || !strcasecmp(c->argv[0]->ptr, "post")) {
        securityWarningCommand(c);
        return C_ERR;
    }

    /* 如果我们在一个模块阻塞的上下文中,产生的结果是希望避免处理客户端,则延迟该命令*/
    //    if (server.busy_module_yield_flags & BUSY_MODULE_YIELD_EVENTS) { // 感觉这么写也可以

    if (server.busy_module_yield_flags != BUSY_MODULE_YIELD_NONE && !(server.busy_module_yield_flags & BUSY_MODULE_YIELD_CLIENTS)) {
        c->bpop.timeout = 0; // 设置阻塞超时为0,永不超时？
        blockClient(c, BLOCKED_POSTPONE);
        return C_OK;
    }

    // 查找命令,并进行命令合法性检查,以及命令参数个数检查
    c->cmd = c->lastcmd = c->realcmd = lookupCommand(c->argv, c->argc);
    sds err;
    if (!commandCheckExistence(c, &err)) { // 检查命令是否存在
        rejectCommandSds(c, err);
        return C_OK;
    }
    if (!commandCheckArity(c, &err)) { // 检查命令参数
        rejectCommandSds(c, err);
        return C_OK;
    }

    /* 检查该命令是否被标记为protected并且相关配置允许它*/
    if (c->cmd->flags & CMD_PROTECTED) {
        if ((c->cmd->proc == debugCommand && !allowProtectedAction(server.enable_debug_cmd, c)) || (c->cmd->proc == moduleCommand && !allowProtectedAction(server.enable_module_cmd, c))) {
            rejectCommandFormat(
                c,
                "%s command not allowed. If the %s option is set to \"local\", "
                "you can run it from a local connection, otherwise you need to set this option "
                "in the configuration file, and then restart the server.",
                c->cmd->proc == debugCommand ? "DEBUG" : "MODULE", c->cmd->proc == debugCommand ? "enable-debug-command" : "enable-module-command");
            return C_OK;
        }
    }
    // 是否是读命令
    int is_read_command = (c->cmd->flags & CMD_READONLY) || (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_READONLY));
    // 是否是写命令
    int is_write_command = (c->cmd->flags & CMD_WRITE) || (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_WRITE));
    // 是否会导致OOM
    int is_denyoom_command = (c->cmd->flags & CMD_DENYOOM) || (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_DENYOOM));
    // 是否允许在从节点带有过期数据时执行的命令
    int is_denystale_command = !(c->cmd->flags & CMD_STALE) || (c->cmd->proc == execCommand && (c->mstate.cmd_inv_flags & CMD_STALE));
    // 允许在载入数据库时使用的命令
    int is_denyloading_command = !(c->cmd->flags & CMD_LOADING) || (c->cmd->proc == execCommand && (c->mstate.cmd_inv_flags & CMD_LOADING));
    // 命令可能会产生复制流量、且允许写
    int is_may_replicate_command = (c->cmd->flags & (CMD_WRITE | CMD_MAY_REPLICATE)) || (c->cmd->proc == execCommand && (c->mstate.cmd_flags & (CMD_WRITE | CMD_MAY_REPLICATE)));
    // 在异步加载期间拒绝（当副本使用无盘同步swapdb时,允许访问旧数据集）.
    int is_deny_async_loading_command = (c->cmd->flags & CMD_NO_ASYNC_LOADING) || (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_NO_ASYNC_LOADING));
    // 是否必须服从的客户端
    int obey_client = mustObeyClient(c);

    // 检查认证信息
    if (authRequired(c)) {
        if (!(c->cmd->flags & CMD_NO_AUTH)) {
            rejectCommand(c, shared.noautherr);
            return C_OK;
        }
    }
    // 客户端处于 事务状态 ,并且其中的一条命令 不允许在事务中运行
    if (c->flags & CLIENT_MULTI && c->cmd->flags & CMD_NO_MULTI) {
        rejectCommandFormat(c, "命令不允许在事务中运行");
        return C_OK;
    }

    /* 检查用户是否可以根据当前的acl执行该命令.*/
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
        // 必须遵从的客户端 不能转向
        server.cluster_enabled && !mustObeyClient(c) &&
        (
            // keys 可以移动 ,
            (c->cmd->flags & CMD_MOVABLE_KEYS) || c->cmd->key_specs_num != 0 || c->cmd->proc == execCommand)) {
        int error_code;
        clusterNode *n = getNodeByQuery(c, c->cmd, c->argv, c->argc, &c->slot, &error_code);
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

    /* 如果客户端总内存过高,则断开一些客户端.我们在删除键之前、在执行最后一个命令并消耗了一些客户端输出缓冲区内存之后执行此操作.*/
    evictClients(); // 驱逐客户端
    if (server.current_client == NULL) {
        /* 如果我们驱逐自己,那么就中止处理命令 */
        return C_ERR;
    }

    // 处理maxmemory指令.
    // 注意,如果我们在这里重新进入事件循环,我们不希望回收内存,因为有一个繁忙的Lua脚本在超时条件下运行,以避免由于退出而混合了脚本的传播和del的传播.
    if (server.maxmemory && !scriptIsTimedout()) { // 设置了最大内存,且脚本没有超时
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

        int reject_cmd_on_oom = is_denyoom_command;
        /* If client is in MULTI/EXEC context, queuing may consume an unlimited
         * amount of memory, so we want to stop that.
         * However, we never want to reject DISCARD, or even EXEC (unless it
         * contains denied commands, in which case is_denyoom_command is already
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

    /* 确保为客户端缓存元数据使用合理的内存量. */
    if (server.tracking_clients) {
        trackingLimitUsedSlots();
    }

    /* 不要接受写命令,如果有问题持续在磁盘上,除非来自我们的主机,在这种情况下,检查副本忽略磁盘写错误配置,要么日志或崩溃.*/
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
                    serverLog(
                        LL_WARNING,
                        "Replica is applying a command even though "
                        "it is unable to write to disk.");
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
    if (is_write_command && !checkGoodReplicasStatus()) {
        //    if (!checkGoodReplicasStatus() && is_write_command) {
        rejectCommand(c, shared.noreplicaserr);
        return C_OK;
    }

    // 如果这个服务器是一个只读 slave 的话,那么拒绝执行写命令
    if (server.masterhost && server.repl_slave_ro && !obey_client && is_write_command) {
        rejectCommand(c, shared.roslaveerr);
        return C_OK;
    }

    // 订阅发布, 在RESP2中只允许一部分命令, RESP3没有限制
    if ((c->flags & CLIENT_PUBSUB && c->resp == 2) && c->cmd->proc != pingCommand && c->cmd->proc != subscribeCommand && c->cmd->proc != ssubscribeCommand && c->cmd->proc != unsubscribeCommand && c->cmd->proc != sunsubscribeCommand && c->cmd->proc != psubscribeCommand && c->cmd->proc != punsubscribeCommand && c->cmd->proc != quitCommand &&
        c->cmd->proc != resetCommand) {
        // pingCommand
        // subscribeCommand
        // ssubscribeCommand
        // unsubscribeCommand
        // sunsubscribeCommand
        // psubscribeCommand
        // punsubscribeCommand
        // quitCommand
        // resetCommand
        // 这些命令, 只能在RESP3协议中使用
        rejectCommandFormat(c, "不能执行'%s': 在当前上下文中只允许 (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / PING / QUIT / RESET 命令的执行", c->cmd->fullname);
        return C_OK;
    }

    //    当replica-serve-stale-data为no,并且我们是一个与master链接断开的副本时,只允许带有 CMD_STALE 标志的命令,如 INFO, REPLICAOF等.
    if (server.masterhost && server.repl_state != REPL_STATE_CONNECTED && server.repl_serve_stale_data == 0 && is_denystale_command) {
        rejectCommand(c, shared.masterdownerr);
        return C_OK;
    }

    // 如果服务器正在载入数据到数据库,那么只执行带有 REDIS_CMD_LOADING 标识的命令,否则将出错
    if (server.loading && !server.async_loading && is_denyloading_command) {
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
        call(c, CMD_CALL_FULL); // 调用call函数执行命令
        c->woff = server.master_repl_offset;

        if (listLength(server.ready_keys)) {
            // 处理那些解除了阻塞的键
            handleClientsBlockedOnKeys();
        }
    }

    return C_OK;
}

/* ====================== Error lookup and execution ===================== */

void incrementErrorCount(const char *fullerr, size_t namelen) {
    struct redisError *error = raxFind(server.errors, (unsigned char *)fullerr, namelen);
    if (error == raxNotFound) {
        error = zmalloc(sizeof(*error));
        error->count = 0;
        raxInsert(server.errors, (unsigned char *)fullerr, namelen, error, NULL);
    }
    error->count++;
}

/*================================== Shutdown =============================== */

/* Close listening sockets. Also unlink the unix domain socket if
 * unlink_unix_socket is non-zero. */
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
        serverLog(LL_NOTICE, "Removing the unix socket file.");
        if (unlink(server.unixsocket) != 0)
            serverLog(LL_WARNING, "Error removing the unix socket file: %s", strerror(errno));
    }
}

/* Prepare for shutting down the server. Flags:
 *
 * - SHUTDOWN_SAVE: Save a database dump even if the server is configured not to
 *   save any dump.
 *
 * - SHUTDOWN_NOSAVE: Don't save any database dump even if the server is
 *   configured to save one.
 *
 * - SHUTDOWN_NOW: Don't wait for replicas to catch up before shutting down.
 *
 * - SHUTDOWN_FORCE: Ignore errors writing AOF and RDB files on disk, which
 *   would normally prevent a shutdown.
 *
 * Unless SHUTDOWN_NOW is set and if any replicas are lagging behind, C_ERR is
 * returned and server.shutdown_mstime is set to a timestamp to allow a grace
 * period for the replicas to catch up. This is checked and handled by
 * serverCron() which completes the shutdown as soon as possible.
 *
 * If shutting down fails due to errors writing RDB or AOF files, C_ERR is
 * returned and an error is logged. If the flag SHUTDOWN_FORCE is set, these
 * errors are logged but ignored and C_OK is returned.
 *
 * On success, this function returns C_OK and then it's OK to call exit(0). */
int prepareForShutdown(int flags) {
    if (isShutdownInitiated())
        return C_ERR;

    /* When SHUTDOWN is called while the server is loading a dataset in
     * memory we need to make sure no attempt is performed to save
     * the dataset on shutdown (otherwise it could overwrite the current DB
     * with half-read data).
     *
     * Also when in Sentinel mode clear the SAVE flag and force NOSAVE. */
    if (server.loading || server.sentinel_mode)
        flags = (flags & ~SHUTDOWN_SAVE) | SHUTDOWN_NOSAVE;

    server.shutdown_flags = flags;

    serverLog(LL_WARNING, "User requested shutdown...");
    if (server.supervised_mode == SUPERVISED_SYSTEMD)
        redisCommunicateSystemd("STOPPING=1\n");

    /* If we have any replicas, let them catch up the replication offset before
     * we shut down, to avoid data loss. */
    if (!(flags & SHUTDOWN_NOW) && server.shutdown_timeout != 0 && !isReadyToShutdown()) {
        server.shutdown_mstime = server.mstime + server.shutdown_timeout * 1000;
        if (!areClientsPaused())
            sendGetackToReplicas();
        pauseClients(PAUSE_DURING_SHUTDOWN, LLONG_MAX, CLIENT_PAUSE_WRITE);
        serverLog(LL_NOTICE, "Waiting for replicas before shutting down.");
        return C_ERR;
    }

    return finishShutdown();
}

static inline int isShutdownInitiated(void) {
    return server.shutdown_mstime != 0;
}

/* Returns 0 if there are any replicas which are lagging in replication which we
 * need to wait for before shutting down. Returns 1 if we're ready to shut
 * down now. */
int isReadyToShutdown(void) {
    if (listLength(server.slaves) == 0)
        return 1; /* No replicas. */

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

static void cancelShutdown(void) {
    server.shutdown_asap = 0;
    server.shutdown_flags = 0;
    server.shutdown_mstime = 0;
    server.last_sig_received = 0;
    replyToClientsBlockedOnShutdown();
    unpauseClients(PAUSE_DURING_SHUTDOWN);
}

/* Returns C_OK if shutdown was aborted and C_ERR if shutdown wasn't ongoing. */
int abortShutdown(void) {
    if (isShutdownInitiated()) {
        cancelShutdown();
    }
    else if (server.shutdown_asap) {
        /* Signal handler has requested shutdown, but it hasn't been initiated
         * yet. Just clear the flag. */
        server.shutdown_asap = 0;
    }
    else {
        /* Shutdown neither initiated nor requested. */
        return C_ERR;
    }
    serverLog(LL_NOTICE, "Shutdown manually aborted.");
    return C_OK;
}

/* The final step of the shutdown sequence. Returns C_OK if the shutdown
 * sequence was successful and it's OK to call exit(). If C_ERR is returned,
 * it's not safe to call exit(). */
int finishShutdown(void) {
    int save = server.shutdown_flags & SHUTDOWN_SAVE;
    int nosave = server.shutdown_flags & SHUTDOWN_NOSAVE;
    int force = server.shutdown_flags & SHUTDOWN_FORCE;

    /* Log a warning for each replica that is lagging. */
    listIter replicas_iter;
    listNode *replicas_list_node;
    int num_replicas = 0, num_lagging_replicas = 0;
    listRewind(server.slaves, &replicas_iter);
    while ((replicas_list_node = listNext(&replicas_iter)) != NULL) {
        client *replica = listNodeValue(replicas_list_node);
        num_replicas++;
        if (replica->repl_ack_off != server.master_repl_offset) {
            num_lagging_replicas++;
            long lag = replica->replstate == SLAVE_STATE_ONLINE ? time(NULL) - replica->repl_ack_time : 0;
            serverLog(LL_WARNING, "Lagging replica %s reported offset %lld behind master, lag=%ld, state=%s.", replicationGetSlaveName(replica), server.master_repl_offset - replica->repl_ack_off, lag, replstateToString(replica->replstate));
        }
    }
    if (num_replicas > 0) {
        serverLog(LL_NOTICE, "%d of %d replicas are in sync when shutting down.", num_replicas - num_lagging_replicas, num_replicas);
    }

    /* Kill all the Lua debugger forked sessions. */
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
        ret = sdscatfmt(sdsempty(), "-MISCONF Errors writing to the AOF file: %s", strerror(server.aof_last_write_errno));
    }
    return ret;
}

/* The PING command. It works in a different way if the client is in
 * in Pub/Sub mode. */
void pingCommand(client *c) {
    /* The command takes zero or one arguments. */
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

    /* gettimeofday() can only fail if &tv is a bad address so we
     * don't check for errors. */
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

/* Must match redisCommandArgType */
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

/* Must match redisCommandRESP2Type */
const char *RESP2_TYPE_STR[] = {
    "simple-string", "error", "integer", "bulk-string", "null-bulk-string", "array", "null-array",
};

/* Must match redisCommandRESP3Type */
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

/* Reply with an array of sub-command using the provided reply callback. */
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

/* Helper for COMMAND GETKEYS and GETKEYSANDFLAGS */
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
            printf("------------\n");
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

/* Convert an amount of bytes into a human readable string in the form
 * of 100B, 2G, 100M, 4K, and so forth. */
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

/* Fill percentile distribution of latencies. */
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

/* Characters we sanitize on INFO output to maintain expected format. */
static char unsafe_info_chars[] = "#:\n\r";
static char unsafe_info_chars_substs[] = "____"; /* Must be same length as above */

/* Returns a sanitized version of s that contains no unsafe info string chars.
 * If no unsafe characters are found, simply returns s. Caller needs to
 * free tmp if it is non-null on return.
 */
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
                getSafeInfoString(c->fullname, sdslen(c->fullname), &tmpsafe),                                  // 具体命令
                c->calls,                                                                                       // 调用次数
                c->microseconds,                                                                                // 耗费CPU时间
                (c->calls == 0) ? 0 : ((float)c->microseconds / c->calls),                                      // 每个命令平均耗费的CPU(单位为微妙)
                c->rejected_calls,                                                                              //
                c->failed_calls                                                                                 //
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

/* Takes a null terminated sections list, and adds them to the dict. */
void addInfoSectionsToDict(dict *section_dict, char **sections) {
    while (*sections) {
        sds section = sdsnew(*sections);
        if (dictAdd(section_dict, section, NULL) == DICT_ERR)
            sdsfree(section);
        sections++;
    }
}

/* Cached copy of the default sections, as an optimization. */
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

/* Create the string returned by the INFO command. This is decoupled
 * by the INFO command itself as we need to report the same information
 * on memory corruption problems. */
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
            "monotonic_clock:%s\r\n"
            "multiplexing_api:%s\r\n"
            "atomicvar_api:%s\r\n"
            "gcc_version:%i.%i.%i\r\n"
            "process_id:%I\r\n"
            "process_supervised:%s\r\n"
            "run_id:%s\r\n"
            "tcp_port:%i\r\n"
            "server_time_usec:%I\r\n"
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
            (int64_t)getpid(), supervised, server.runid, server.port ? server.port : server.tls_port, (int64_t)server.ustime, (int64_t)uptime, (int64_t)(uptime / (3600 * 24)), server.hz, server.config_hz, lruclock, server.executable ? server.executable : "", server.configfile ? server.configfile : "", server.io_threads_active);

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
        info = sdscatprintf(info, "# 客户端连接数量\r\n");
        info = sdscatprintf(info, "connected_clients:%lu\r\n", listLength(server.clients) - listLength(server.slaves));
        info = sdscatprintf(info, "cluster_connections:%lu\r\n", getClusterConnectionsCount());
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
        info = sdscatprintf(info, "used_memory_peak:%zu\r\n", server.stat_peak_memory);
        info = sdscatprintf(info, "used_memory_peak_human:%s\r\n", peak_hmem);
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
        info = sdscatprintf(info, "total_connections_received:%lld\r\n", server.stat_numconnections);
        info = sdscatprintf(info, "total_commands_processed:%lld\r\n", server.stat_numcommands);
        info = sdscatprintf(info, "# 平均每秒处理请求数\r\n");
        info = sdscatprintf(info, "instantaneous_ops_per_sec:%lld\r\n", getInstantaneousMetric(STATS_METRIC_COMMAND));
        info = sdscatprintf(info, "total_net_input_bytes:%lld\r\n", stat_net_input_bytes);
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
        info = sdscatprintf(info, "expired_keys:%lld\r\n", server.stat_expiredkeys);
        info = sdscatprintf(info, "expired_stale_perc:%.2f\r\n", server.stat_expired_stale_perc * 100);
        info = sdscatprintf(info, "expired_time_cap_reached_count:%lld\r\n", server.stat_expired_time_cap_reached_count);
        info = sdscatprintf(info, "expire_cycle_cpu_milliseconds:%lld\r\n", server.stat_expire_cycle_time_used / 1000);
        info = sdscatprintf(info, "# 由于最大内存限制被移除的key的数量\r\n");
        info = sdscatprintf(info, "evicted_keys:%lld\r\n", server.stat_evictedkeys);
        info = sdscatprintf(info, "evicted_clients:%lld\r\n", server.stat_evictedclients);
        info = sdscatprintf(info, "total_eviction_exceeded_time:%lld\r\n", (server.stat_total_eviction_exceeded_time + current_eviction_exceeded_time) / 1000);
        info = sdscatprintf(info, "current_eviction_exceeded_time:%lld\r\n", current_eviction_exceeded_time / 1000);
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
        info = sdscatprintf(info, "total_reads_processed:%lld\r\n", stat_total_reads_processed);
        info = sdscatprintf(info, "total_writes_processed:%lld\r\n", stat_total_writes_processed);
        info = sdscatprintf(info, "io_threaded_reads_processed:%lld\r\n", server.stat_io_reads_processed);
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

        /* If min-slaves-to-write is active, write the number of slaves
         * currently considered 'good'. */
        if (server.repl_min_slaves_to_write && server.repl_min_slaves_max_lag) {
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

    /* Latency by percentile distribution per command */
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

    /* Get info from modules.
     * if user asked for "everything" or "modules", or a specific section
     * that's not found yet. */
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
        /**
         * A client that has CLIENT_DENY_BLOCKING flag on
         * expects a reply per command and so can't execute MONITOR. */
        addReplyError(c, "MONITOR isn't allowed for DENY BLOCKING client");
        return;
    }
    // 这个客户端是从服务器,或者已经是监视器
    /* ignore MONITOR if already slave or in monitor mode */
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

#ifdef __arm64__

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
#ifndef MADV_FREE
#define MADV_FREE 8
#endif
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
#endif /* __arm64__ */
#endif /* __linux__ */

void createPidFile(void) {
    /* If pid_file requested, but no pid_file defined, use
     * default pid_file path */
    if (!server.pid_file)
        server.pid_file = zstrdup(CONFIG_DEFAULT_PID_FILE);

    /* Try to write the pid file in a best-effort way. */
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
    fprintf(stderr, "Examples:\n");
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
#include "asciilogo.h"

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

    /* Bind to the new port */
    if ((server.port != 0 && listenToPort(server.port, &server.ipfd) != C_OK) || (server.tls_port != 0 && listenToPort(server.tls_port, &server.tlsfd) != C_OK)) {
        serverLog(LL_WARNING, "Failed to bind");

        closeSocketListeners(&server.ipfd);
        closeSocketListeners(&server.tlsfd);
        return C_ERR;
    }

    /* Create TCP and TLS event handlers */
    if (createSocketAcceptHandler(&server.ipfd, acceptTcpHandler) != C_OK) {
        serverPanic("Unrecoverable error creating TCP socket accept handler.");
    }
    if (createSocketAcceptHandler(&server.tlsfd, acceptTLSHandler) != C_OK) {
        serverPanic("Unrecoverable error creating TLS socket accept handler.");
    }

    if (server.set_proc_title)
        redisSetProcTitle(NULL);

    return C_OK;
}

int changeListenPort(int port, socketFds *sfd, aeFileProc *accept_handler) {
    socketFds new_sfd = {{0}};

    /* Close old servers */
    closeSocketListeners(sfd);

    /* Just close the server if port disabled */
    if (port == 0) {
        if (server.set_proc_title)
            redisSetProcTitle(NULL);
        return C_OK;
    }

    // 绑定新的端口
    if (listenToPort(port, &new_sfd) != C_OK) {
        return C_ERR;
    }

    /* Create event handlers */
    if (createSocketAcceptHandler(&new_sfd, accept_handler) != C_OK) {
        closeSocketListeners(&new_sfd);
        return C_ERR;
    }

    /* Copy new descriptors */
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
            msg = "Received SIGINT scheduling shutdown...";
            break;
        case SIGTERM:
            msg = "Received SIGTERM scheduling shutdown...";
            break;
        default:
            msg = "Received shutdown signal, scheduling shutdown...";
    };

    /* SIGINT is often delivered via Ctrl+C in an interactive session.
     * If we receive the signal the second time, we interpret this as
     * the user really wanting to quit ASAP without waiting to persist
     * on disk and without waiting for lagging replicas. */
    if (server.shutdown_asap && sig == SIGINT) {
        serverLogFromHandler(LL_WARNING, "You insist... exiting now.");
        rdbRemoveTempFile(getpid(), 1);
        exit(1); /* Exit with an error since this was not a clean shutdown. */
    }
    else if (server.loading) {
        msg = "Received shutdown signal during loading, scheduling shutdown.";
    }

    serverLogFromHandler(LL_WARNING, msg);
    server.shutdown_asap = 1; // 打开关闭标识
    server.last_sig_received = sig;
}

void setupSignalHandlers(void) {
    struct sigaction act;

    // 如果在sa_flags中设置了SA_SIGINFO标志,则使用sa_sigaction.否则,使用sa_handler.
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

/* This is the signal handler for children process. It is currently useful
 * in order to track the SIGUSR1, that we send to a child in order to terminate
 * it in a clean way, without the parent detecting an error and stop
 * accepting writes because of a write error condition. */
static void sigKillChildHandler(int sig) {
    UNUSED(sig);
    int level = server.in_fork_child == CHILD_TYPE_MODULE ? LL_VERBOSE : LL_WARNING;
    serverLogFromHandler(level, "Received SIGUSR1 in child, exiting now.");
    exitFromChild(SERVER_CHILD_NOERROR_RETVAL);
}

void setupChildSignalHandlers(void) {
    struct sigaction act;

    /* When the SA_SIGINFO flag is set in sa_flags then sa_sigaction is used.
     * Otherwise, sa_handler is used. */
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = sigKillChildHandler;
    sigaction(SIGUSR1, &act, NULL);
}

/* After fork, the child process will inherit the resources
 * of the parent process, e.g. fd(socket or flock) etc.
 * should close the resources not used by the child process, so that if the
 * parent restarts it can bind/lock despite the child possibly still running. */
void closeChildUnusedResourceAfterFork() {
    closeListeningSockets(0);
    if (server.cluster_enabled && server.cluster_config_file_lock_fd != -1)
        close(server.cluster_config_file_lock_fd); /* don't care if this fails */

    /* Clear server.pid_file, this is the parent pid_file which should not
     * be touched (or deleted) by the child (on exit / crash) */
    zfree(server.pid_file);
    server.pid_file = NULL;
}

/* purpose is one of CHILD_TYPE_ types */
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
         *
         * The order of setting things up follows some reasoning:
         * Setup signal handlers first because a signal could fire at any time.
         * Adjust OOM score before everything else to assist the OOM killer if
         * memory resources are low.
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

        /* The child_pid and child_type are only for mutual exclusive children.
         * other child types should handle and store their pid's in dedicated variables.
         *
         * Today, we allows CHILD_TYPE_LDB to run in parallel with the other fork types:
         * - it isn't used for production, so it will not make the server be less efficient
         * - used for debugging, and we don't want to block it from running while other
         *   forks are running (like RDB and AOF) */
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

void sendChildCowInfo(childInfoType info_type, char *pname) {
    sendChildInfoGeneric(info_type, 0, -1, pname);
}

void sendChildInfo(childInfoType info_type, size_t keys, char *pname) {
    sendChildInfoGeneric(info_type, keys, -1, pname);
}

/* Try to release pages back to the OS directly (bypassing the allocator),
 * in an effort to decrease CoW during fork. For small allocations, we can't
 * release any full page, so in an effort to avoid getting the size of the
 * allocation from the allocator (malloc_size) when we already know it's small,
 * we check the size_hint. If the size is not already known, passing a size_hint
 * of 0 will lead the checking the real size of the allocation.
 * Also please note that the size may be not accurate, so in order to make this
 * solution effective, the judgement for releasing memory pages should not be
 * too strict. */
void dismissMemory(void *ptr, size_t size_hint) {
    if (ptr == NULL)
        return;

    /* madvise(MADV_DONTNEED) can not release pages if the size of memory
     * is too small, we try to release only for the memory which the size
     * is more than half of page size. */
    if (size_hint && size_hint <= server.page_size / 2)
        return;

    zmadvise_dontneed(ptr);
}

/* Dismiss big chunks of memory inside a client structure, see dismissMemory() */
void dismissClientMemory(client *c) {
    /* Dismiss client query buffer and static reply buffer. */
    dismissMemory(c->buf, c->buf_usable_size);
    dismissSds(c->querybuf);
    /* Dismiss argv array only if we estimate it contains a big buffer. */
    if (c->argc && c->argv_len_sum / c->argc >= server.page_size) {
        for (int i = 0; i < c->argc; i++) {
            dismissObject(c->argv[i], 0);
        }
    }
    if (c->argc)
        dismissMemory(c->argv, c->argc * sizeof(robj *));

    /* Dismiss the reply array only if the average buffer size is bigger
     * than a page. */
    if (listLength(c->reply) && c->reply_bytes / listLength(c->reply) >= server.page_size) {
        listIter li;
        listNode *ln;
        listRewind(c->reply, &li);
        while ((ln = listNext(&li))) {
            clientReplyBlock *bulk = listNodeValue(ln);
            /* Default bulk size is 16k, actually it has extra data, maybe it
             * occupies 20k according to jemalloc bin size if using jemalloc. */
            if (bulk)
                dismissMemory(bulk, bulk->size);
        }
    }
}

/* In the child process, we don't need some buffers anymore, and these are
 * likely to change in the parent when there's heavy write traffic.
 * We dismis them right away, to avoid CoW.
 * see dismissMemeory(). */
void dismissMemoryInChild(void) {
    /* madvise(MADV_DONTNEED) may not work if Transparent Huge Pages is enabled. */
    if (server.thp_enabled)
        return;

        /* Currently we use zmadvise_dontneed only when we use jemalloc with Linux.
         * so we avoid these pointless loops when they're not going to do anything. */
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

// OK
void redisOutOfMemoryHandler(size_t allocation_size) {
    serverLog(LL_WARNING, "内存分配不足 %zu bytes!", allocation_size);
    serverPanic("由于内存不足,Redis正在中止.分配 %zu bytes!", allocation_size);
}

/* Callback for sdstemplate on proc-title-template. See redis.conf for
 * supported variables.
 */
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

/* Expand the specified proc-title-template string and return a newly
 * allocated sds, or NULL. */
static sds expandProcTitleTemplate(const char *template, const char *title) {
    sds res = sdstemplate(template, redisProcTitleGetVariable, (void *)title);
    if (!res)
        return NULL;
    return sdstrim(res, " ");
}

/* Validate the specified template, returns 1 if valid or 0 otherwise. */
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
// 沟通
int redisCommunicateSystemd(const char *sd_notify_msg) {
#ifdef HAVE_LIBSYSTEMD
    int ret = sd_notify(0, sd_notify_msg);

    if (ret == 0)
        serverLog(LL_WARNING, "systemd supervision error: NOTIFY_SOCKET not found!");
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

/* Attempt to set up systemd supervision. Returns 1 if successful. */
static int redisSupervisedSystemd(void) {
#ifndef HAVE_LIBSYSTEMD
    serverLog(LL_WARNING, "systemd supervision requested or auto-detected, but Redis is compiled without libsystemd support!");
    return 0;
#else
    if (redisCommunicateSystemd("STATUS=Redis is loading...\n") <= 0)
        return 0;
    serverLog(LL_NOTICE, "Supervised by systemd. Please make sure you set appropriate values for TimeoutStartSec and TimeoutStopSec in your service unit.");
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
    return ((!server.cluster_enabled && server.masterhost == NULL) || (server.cluster_enabled && nodeIsMaster(server.cluster->myself)));
}

#ifdef REDIS_TEST
#include "testhelp.h"

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
    //    阶段一：基本初始化
    // 我们需要初始化我们的库,以及服务器配置.
#ifdef INIT_SETPROCTITLE_REPLACEMENT
    spt_init(argc, argv);
#endif

    setlocale(LC_COLLATE, "");                                                // 函数既可以用来对当前程序进行地域设置（本地设置、区域设置）,也可以用来获取当前程序的地域设置信息.
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

    //    阶段二：检查哨兵模式,并检查是否要执行 RDB 检测或 AOF 检测
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
    //    阶段三：运行参数解析
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
    //    阶段四：初始化 server

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
#if defined(__arm64__)
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
#endif                                 /* __arm64__ */
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
        serverLog(LL_WARNING, "警报:您指定了小于1MB的maxmemory值(当前值是%llu字节).你确定这是你想要的吗?", server.maxmemory);
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

/* The End */
