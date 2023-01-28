// 如果内存满了,Redis 还会按照一定规则清除不需要的数据
#include "server.h"
#include "bio.h"
#include "atomicvar.h"
#include "script.h"
#include <math.h>

/* ----------------------------------------------------------------------------
 * Data structures
 * --------------------------------------------------------------------------*/

/* To improve the quality of the LRU approximation we take a set of keys
 * that are good candidate for eviction across performEvictions() calls.
 *
 * Entries inside the eviction pool are taken ordered by idle time, putting
 * greater idle times to the right (ascending order).
 *
 * When an LFU policy is used instead, a reverse frequency indication is used
 * instead of the idle time, so that we still evict by larger value (larger
 * inverse frequency means to evict keys with the least frequent accesses).
 *
 * Empty entries have the key pointer set to NULL. */
#define EVPOOL_SIZE 16
#define EVPOOL_CACHED_SDS_SIZE 255
struct evictionPoolEntry {
    unsigned long long idle; // 待淘汰的键值对的空闲时间 (inverse frequency for LFU) */
    sds key;                 // 待淘汰的键值对的key
    sds cached;              // 缓存的SDS对象
    int dbid;                // 待淘汰键值对的key所在的数据库ID
};

static struct evictionPoolEntry *EvictionPoolLRU;

/* ----------------------------------------------------------------------------
 * 驱逐老龄化 的LRU实现
 * --------------------------------------------------------------------------*/

// 获得以毫秒为单位计算的 UNIX 时间戳,底层
unsigned int getLRUClock(void) {
    return (mstime() / LRU_CLOCK_RESOLUTION) & LRU_CLOCK_MAX; // 11111111 11111111 11111111 11111111
    // 4字节 可以存储 138 年的秒 ,距1969年
}

// LRU时钟时间
unsigned int LRU_CLOCK(void) {
    // unsigned int 4个字节
    unsigned int lruclock = 0;
    if (1000 / server.hz <= LRU_CLOCK_RESOLUTION) { // <=1000
        atomicGet(server.lru_clock, lruclock);      // lruclock = server.lru_clock
    }
    else {
        lruclock = getLRUClock();
    }
    return lruclock;
}

/* Given an object returns the min number of milliseconds the object was never
 * requested, using an approximated LRU algorithm. */
unsigned long long estimateObjectIdleTime(robj *o) {
    unsigned long long lruclock = LRU_CLOCK();
    if (lruclock >= o->lru) {
        return (lruclock - o->lru) * LRU_CLOCK_RESOLUTION;
    }
    else {
        return (lruclock + (LRU_CLOCK_MAX - o->lru)) * LRU_CLOCK_RESOLUTION;
    }
}

/* LRU approximation algorithm
 *
 * Redis uses an approximation of the LRU algorithm that runs in constant
 * memory. Every time there is a key to expire, we sample N keys (with
 * N very small, usually in around 5) to populate a pool of best keys to
 * evict of M keys (the pool size is defined by EVPOOL_SIZE).
 *
 * The N keys sampled are added in the pool of good keys to expire (the one
 * with an old access time) if they are better than one of the current keys
 * in the pool.
 *
 * After the pool is populated, the best key we have in the pool is expired.
 * However note that we don't remove keys from the pool when they are deleted
 * so the pool may contain keys that no longer exist.
 *
 * When we try to evict a key, and all the entries in the pool don't exist
 * we populate it again. This time we'll be sure that the pool has at least
 * one key that can be evicted, if there is at least one key that can be
 * evicted in the whole database. */

// 创建key过期 驱逐池,来保存待淘汰候选键值对集合
void evictionPoolAlloc(void) {
    struct evictionPoolEntry *ep;
    int j;

    ep = zmalloc(sizeof(*ep) * EVPOOL_SIZE); // 16    为 EvictionPoolLRU 数组分配内存空间
    for (j = 0; j < EVPOOL_SIZE; j++) {
        ep[j].idle = 0;
        ep[j].key = NULL;
        ep[j].cached = sdsnewlen(NULL, EVPOOL_CACHED_SDS_SIZE); // 256
        ep[j].dbid = 0;
    }
    EvictionPoolLRU = ep;
}

/* This is a helper function for performEvictions(), it is used in order
 * to populate the evictionPool with a few entries every time we want to
 * expire a key. Keys with idle time bigger than one of the current
 * keys are added. Keys are always added if there are free entries.
 *
 * We insert keys on place in ascending order, so keys with the smaller
 * idle time are on the left, and keys with the higher idle time on the
 * right. */
// 填充驱逐池
void evictionPoolPopulate(int dbid, dict *sampledict, dict *keydict, struct evictionPoolEntry *pool) {
    // sampledict    如果 maxmemory_policy 配置的是 allkeys_lru,那么待采样哈希表就是 Redis server 的全局哈希表,也就是在所有键值对中进行采样;
    // sampledict    否则,待采样哈希表就是保存着设置了过期时间的 key 的哈希表
    int j, k;
    unsigned int count;
    // 采样后的集合,大小为maxmemory_samples
    dictEntry *samples[server.maxmemory_samples]; // 5
    // 将待采样的哈希表sampledict、采样后的集合samples、以及采样数量maxmemory_samples,作为参数传给dictGetSomeKeys
    count = dictGetSomeKeys(sampledict, samples, server.maxmemory_samples); // 从待采样的哈希表中随机获取一定数量的 key
    for (j = 0; j < count; j++) {
        unsigned long long idle;
        sds key;
        robj *o;
        dictEntry *de;

        de = samples[j];
        key = dictGetKey(de);

        /* If the dictionary we are sampling from is not the main
         * dictionary (but the expires one) we need to lookup the key
         * again in the key dictionary to obtain the value object. */
        if (server.maxmemory_policy != MAXMEMORY_VOLATILE_TTL) {
            if (sampledict != keydict) {
                de = dictFind(keydict, key);
            }
            o = dictGetVal(de);
        }

        /* Calculate the idle time according to the policy. This is called
         * idle just because the code initially handled LRU, but is in fact
         * just a score where an higher score means better candidate. */
        if (server.maxmemory_policy & MAXMEMORY_FLAG_LRU) {
            idle = estimateObjectIdleTime(o); // 计算在采样集合中的每一个键值对的空闲时间
        }
        else if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
            /* When we use an LRU policy, we sort the keys by idle time
             * so that we expire keys starting from greater idle time.
             * However when the policy is an LFU one, we have a frequency
             * estimation, and we want to evict keys with lower frequency
             * first. So inside the pool we put objects using the inverted
             * frequency subtracting the actual frequency to the maximum
             * frequency of 255. */
            idle = 255 - LFUDecrAndReturn(o);
        }
        else if (server.maxmemory_policy == MAXMEMORY_VOLATILE_TTL) {
            /* In this case the sooner the expire the better. */
            idle = ULLONG_MAX - (long)dictGetVal(de);
        }
        else {
            serverPanic("Unknown eviction policy in evictionPoolPopulate()");
        }

        /* Insert the element inside the pool.
         * First, find the first empty bucket or the first populated
         * bucket that has an idle time smaller than our idle time. */
        k = 0;
        while (k < EVPOOL_SIZE && pool[k].key && pool[k].idle < idle) k++;
        if (k == 0 && pool[EVPOOL_SIZE - 1].key != NULL) {
            /* Can't insert if the element is < the worst element we have
             * and there are no empty buckets. */
            continue;
        }
        else if (k < EVPOOL_SIZE && pool[k].key == NULL) { /* Inserting into empty position. No setup needed before insert. */
        }
        else {
            /* Inserting in the middle. Now k points to the first element
             * greater than the element to insert.  */
            if (pool[EVPOOL_SIZE - 1].key == NULL) {
                /* Free space on the right? Insert at k shifting
                 * all the elements from k to end to the right. */

                /* Save SDS before overwriting. */
                sds cached = pool[EVPOOL_SIZE - 1].cached;
                memmove(pool + k + 1, pool + k, sizeof(pool[0]) * (EVPOOL_SIZE - k - 1));
                pool[k].cached = cached;
            }
            else {
                /* No free space on right? Insert at k-1 */
                k--;
                /* Shift all elements on the left of k (included) to the
                 * left, so we discard the element with smaller idle time. */
                sds cached = pool[0].cached; /* Save SDS before overwriting. */
                if (pool[0].key != pool[0].cached)
                    sdsfree(pool[0].key);
                memmove(pool, pool + 1, sizeof(pool[0]) * k);
                pool[k].cached = cached;
            }
        }

        /* Try to reuse the cached SDS string allocated in the pool entry,
         * because allocating and deallocating this object is costly
         * (according to the profiler, not my fantasy. Remember:
         * premature optimization bla bla bla. */
        int klen = sdslen(key);
        if (klen > EVPOOL_CACHED_SDS_SIZE) {
            pool[k].key = sdsdup(key);
        }
        else {
            memcpy(pool[k].cached, key, klen + 1);
            sdssetlen(pool[k].cached, klen);
            pool[k].key = pool[k].cached;
        }
        pool[k].idle = idle;
        pool[k].dbid = dbid;
    }
}

/* ----------------------------------------------------------------------------
 * LFU (Least Frequently Used) 最不频繁使用算法
 *          16 bits      8 bits
 *     +----------------+--------+
 *     + Last decr time | LOG_C  |
 *     +----------------+--------+
 *     时间、频率
 */
unsigned long LFUGetTimeInMinutes(void) {
    // 返回当前时间,单位是分钟,取后16bit
    return (server.unixtime / 60) & 65535;
}

// 获取两次访问时间的差值
unsigned long LFUTimeElapsed(unsigned long ldt) {
    unsigned long now = LFUGetTimeInMinutes();
    if (now >= ldt) {
        return now - ldt; // 计算当前时间和数据最近一次访问时间的差值[分钟]
    }
    else {
        serverLog(LL_WARNING, "LFUTimeElapsed 获取过期时间 数据溢出 %lu", 65535 - ldt + now);
        return 65535 - ldt + now;
    }
}

// https://redis.com/wp-content/uploads/2019/04/Redis-Day-2019-Tel-Aviv-sripathi-krishnan-LFU-Cache-Optimization.pdf
// https://redis.io/topics/lru-cache
// 计算新的频率权重  0-255
uint8_t LFULogIncr(uint8_t counter) {
    // 如果频繁访问,counter不会减少, p会在一分钟内保持不变, 但随着访问次数的增加,r会一直重试,总会有<p的时候
    // 且随着counter的增加,这个概率也会更低

    // counter 当前访问次数
    //     非线性递增的计数器,可以有效地区分不同的访问次数
    if (counter == 255) {
        return 255;
    }
    double r = (double)rand() / RAND_MAX;
    double baseval = counter - LFU_INIT_VAL; // 5  不是 0,这样可以避免数据刚被写入缓存,就因为访问次数少而被立即淘汰.
    if (baseval < 0) {
        baseval = 0;
    }
    //    默认lfu_log_factor是10,如果一段时间内,访问一万次该key,计数将会在 50 左右. 基本上,如果是频繁访问的缓存,应该设置大一些,反之则相反.
    double p = 1.0 / (baseval * server.lfu_log_factor + 1);

    // 阈值 p 的大小就决定了访问次数增加的难度;越小、越难
    if (r < p) {
        counter++;
    }
    return counter;

    // baseval 的大小：这反映了当前访问次数的多少.比如,访问次数越多的键值对,它的访问次数再增加的难度就会越大;
    // lfu-log-factor 的大小：这是可以被设置的.也就是说,Redis 源码提供了让我们人为调节访问次数增加难度的方法.
}

// 获取键值对衰减之后的值
unsigned long LFUDecrAndReturn(robj *o) {
    //   LFU 删减因子
    unsigned long ldt = o->lru >> 8; // 获取当前键值对的上一次访问时间,取前16位的值

    unsigned long counter = o->lru & 255; // 获取当前的访问次数 , 取后8位的值
    // 计算衰减周期,   lfu_decay_time默认大小为1
    unsigned long num_periods = server.lfu_decay_time ? LFUTimeElapsed(ldt) / server.lfu_decay_time : 0;

    // 如果衰减大小不为0
    if (num_periods) {
        // 如果衰减大小小于当前访问次数,那么,衰减后的访问次数是当前访问次数减去衰减大小;否则,衰减后的访问次数等于0
        if (num_periods > counter) {
            counter = 0;
        }
        else {
            counter = counter - num_periods;
        }
    }
    return counter; // 如果衰减大小为0,则返回原来的访问次数
}

/* We don't want to count AOF buffers and slaves output buffers as
 * used memory: the eviction should use mostly data size, because
 * it can cause feedback-loop when we push DELs into them, putting
 * more and more DELs will make them bigger, if we count them, we
 * need to evict more keys, and then generate more DELs, maybe cause
 * massive eviction loop, even all keys are evicted.
 *
 * This function returns the sum of AOF and replication buffer. */
size_t freeMemoryGetNotCountedMemory(void) {
    size_t overhead = 0;

    /* Since all replicas and replication backlog share global replication
     * buffer, we think only the part of exceeding backlog size is the extra
     * separate consumption of replicas.
     *
     * Note that although the backlog is also initially incrementally grown
     * (pushing DELs consumes memory), it'll eventually stop growing and
     * remain constant in size, so even if its creation will cause some
     * eviction, it's capped, and also here to stay (no resonance effect)
     *
     * Note that, because we trim backlog incrementally in the background,
     * backlog size may exceeds our setting if slow replicas that reference
     * vast replication buffer blocks disconnect. To avoid massive eviction
     * loop, we don't count the delayed freed replication backlog into used
     * memory even if there are no replicas, i.e. we still regard this memory
     * as replicas'. */
    if ((long long)server.repl_buffer_mem > server.repl_backlog_size) {
        /* We use list structure to manage replication buffer blocks, so backlog
         * also occupies some extra memory, we can't know exact blocks numbers,
         * we only get approximate size according to per block size. */
        size_t extra_approx_size = (server.repl_backlog_size / PROTO_REPLY_CHUNK_BYTES + 1) * (sizeof(replBufBlock) + sizeof(listNode));
        size_t counted_mem = server.repl_backlog_size + extra_approx_size;
        if (server.repl_buffer_mem > counted_mem) {
            overhead += (server.repl_buffer_mem - counted_mem);
        }
    }

    if (server.aof_state != AOF_OFF) {
        overhead += sdsAllocSize(server.aof_buf);
    }
    return overhead;
}

/* Get the memory status from the point of view of the maxmemory directive:
 * if the memory used is under the maxmemory setting then C_OK is returned.
 * Otherwise, if we are over the memory limit, the function returns
 * C_ERR.
 *
 * The function may return additional info via reference, only if the
 * pointers to the respective arguments is not NULL. Certain fields are
 * populated only when C_ERR is returned:
 *
 *  'total'     total amount of bytes used.
 *              (Populated both for C_ERR and C_OK)
 *
 *  'logical'   the amount of memory used minus the slaves/AOF buffers.
 *              (Populated when C_ERR is returned)
 *
 *  'tofree'    the amount of memory that should be released
 *              in order to return back into the memory limits.
 *              (Populated when C_ERR is returned)
 *
 *  'level'     this usually ranges from 0 to 1, and reports the amount of
 *              memory currently used. May be > 1 if we are over the memory
 *              limit.
 *              (Populated both for C_ERR and C_OK)
 */
int getMaxmemoryState(size_t *total, size_t *logical, size_t *tofree, float *level) {
    size_t mem_reported, mem_used, mem_tofree;

    /* Check if we are over the memory usage limit. If we are not, no need
     * to subtract the slaves output buffers. We can just return ASAP. */
    mem_reported = zmalloc_used_memory(); // 计算已使用的内存量
    if (total)
        *total = mem_reported;

    /* We may return ASAP if there is no need to compute the level. */
    if (!server.maxmemory) {
        if (level)
            *level = 0;
        return C_OK;
    }
    if (mem_reported <= server.maxmemory && !level)
        return C_OK;

    /* Remove the size of slaves output buffers and AOF buffer from the
     * count of used memory. */
    // 计算出 Redis 目前占用的内存总数,但有两个方面的内存不会计算在内：
    // 1）从服务器的输出缓冲区的内存
    // 2）AOF 缓冲区的内存
    mem_used = mem_reported;
    size_t overhead = freeMemoryGetNotCountedMemory();
    mem_used = (mem_used > overhead) ? mem_used - overhead : 0;

    /* Compute the ratio of memory usage. */
    if (level) {
        *level = (float)mem_used / (float)server.maxmemory;
    }

    if (mem_reported <= server.maxmemory) {
        return C_OK;
    }

    // 如果目前使用的内存大小比设置的 maxmemory 要小,那么无须执行进一步操作

    if (mem_used <= server.maxmemory) {
        return C_OK;
    }

    // 计算需要释放多少字节的内存
    mem_tofree = mem_used - server.maxmemory;

    if (logical) {
        *logical = mem_used;
    }
    if (tofree) {
        *tofree = mem_tofree;
    }

    return C_ERR;
}

/* Return 1 if used memory is more than maxmemory after allocating more memory,
 * return 0 if not. Redis may reject user's requests or evict some keys if used
 * memory exceeds maxmemory, especially, when we allocate huge memory at once. */
int overMaxmemoryAfterAlloc(size_t moremem) {
    if (!server.maxmemory)
        return 0; /* No limit. */

    /* Check quickly. */
    size_t mem_used = zmalloc_used_memory();
    if (mem_used + moremem <= server.maxmemory)
        return 0;

    size_t overhead = freeMemoryGetNotCountedMemory();
    mem_used = (mem_used > overhead) ? mem_used - overhead : 0;
    return mem_used + moremem > server.maxmemory;
}

/* The evictionTimeProc is started when "maxmemory" has been breached and
 * could not immediately be resolved.  This will spin the event loop with short
 * eviction cycles until the "maxmemory" condition has resolved or there are no
 * more evictable items.  */
static int isEvictionProcRunning = 0;

// 事件驱逐 处理器
static int evictionTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    UNUSED(eventLoop);
    UNUSED(id);
    UNUSED(clientData);

    if (performEvictions() == EVICT_RUNNING) {
        // 保持驱逐
        return 0;
    }

    /* For EVICT_OK - things are good, no need to keep evicting.
     * For EVICT_FAIL - there is nothing left to evict.  */
    isEvictionProcRunning = 0;
    return AE_NOMORE;
}

void startEvictionTimeProc(void) {
    if (!isEvictionProcRunning) {
        isEvictionProcRunning = 1;
        aeCreateTimeEvent(server.el, 0, evictionTimeProc, NULL, NULL);
    }
}

/* Check if it's safe to perform evictions.
 *   Returns 1 if evictions can be performed
 *   Returns 0 if eviction processing should be skipped
 */
static int isSafeToPerformEvictions(void) {
    /* - There must be no script in timeout condition.
     * - Nor we are loading data right now.  */
    if (scriptIsTimedout() || server.loading)
        return 0;

    /* By default replicas should ignore maxmemory
     * and just be masters exact copies. */
    if (server.masterhost && server.repl_slave_ignore_maxmemory)
        return 0;

    /* When clients are paused the dataset should be static not just from the
     * POV of clients not being able to write, but also from the POV of
     * expires and evictions of keys not being performed. */
    if (checkClientPauseTimeoutAndReturnIfPaused())
        return 0;

    return 1;
}

/* Algorithm for converting tenacity (0-100) to a time limit.  */
static unsigned long evictionTimeLimitUs() {
    serverAssert(server.maxmemory_eviction_tenacity >= 0);
    serverAssert(server.maxmemory_eviction_tenacity <= 100);

    if (server.maxmemory_eviction_tenacity <= 10) {
        /* A linear progression from 0..500us */
        return 50uL * server.maxmemory_eviction_tenacity;
    }

    if (server.maxmemory_eviction_tenacity < 100) {
        /* A 15% geometric progression, resulting in a limit of ~2 min at tenacity==99  */
        return (unsigned long)(500.0 * pow(1.15, server.maxmemory_eviction_tenacity - 10.0));
    }

    return ULONG_MAX; /* No limit to eviction time */
}

/* Check that memory usage is within the current "maxmemory" limit.  If over
 * "maxmemory", attempt to free memory by evicting data (if it's safe to do so).
 *
 * It's possible for Redis to suddenly be significantly over the "maxmemory"
 * setting.  This can happen if there is a large allocation (like a hash table
 * resize) or even if the "maxmemory" setting is manually adjusted.  Because of
 * this, it's important to evict for a managed period of time - otherwise Redis
 * would become unresponsive while evicting.
 *
 * The goal of this function is to improve the memory situation - not to
 * immediately resolve it.  In the case that some items have been evicted but
 * the "maxmemory" limit has not been achieved, an aeTimeProc will be started
 * which will continue to evict items until memory limits are achieved or
 * nothing more is evictable.
 *
 * This should be called before execution of commands.  If EVICT_FAIL is
 * returned, commands which will result in increased memory usage should be
 * rejected.
 *
 * Returns:
 *   EVICT_OK       - memory is OK or it's not possible to perform evictions now
 *   EVICT_RUNNING  - memory is over the limit, but eviction is still processing
 *   EVICT_FAIL     - memory is over the limit, and there's nothing to evict
 * */
int performEvictions(void) {
    /* Note, we don't goto update_metrics here because this check skips eviction
     * as if it wasn't triggered. it's a fake EVICT_OK. */
    if (!isSafeToPerformEvictions())
        return EVICT_OK;

    int keys_freed = 0;
    size_t mem_reported, mem_tofree;
    long long mem_freed; /* May be negative */
    mstime_t latency, eviction_latency;
    long long delta;
    long long del_before;
    int slaves = listLength(server.slaves);
    int result = EVICT_FAIL;

    if (getMaxmemoryState(&mem_reported, NULL, &mem_tofree, NULL) == C_OK) {
        result = EVICT_OK;
        goto update_metrics;
    }
    // 如果占用内存比 maxmemory 要大,但是 maxmemory 策略为不淘汰,那么直接返回
    if (server.maxmemory_policy == MAXMEMORY_NO_EVICTION) {
        result = EVICT_FAIL; /* We need to free memory, but policy forbids. */
        goto update_metrics;
    }

    unsigned long eviction_time_limit_us = evictionTimeLimitUs();
    // 初始化已释放内存的字节数为 0

    mem_freed = 0;

    latencyStartMonitor(latency);

    monotime evictionTimer;
    elapsedStart(&evictionTimer);

    /* Unlike active-expire and blocked client, we can reach here from 'CONFIG SET maxmemory'
     * so we have to back-up and restore server.core_propagates. */
    int prev_core_propagates = server.core_propagates;
    serverAssert(server.also_propagate.numops == 0);
    server.core_propagates = 1;
    server.propagate_no_multi = 1;
    // 根据 maxmemory 策略,
    // 遍历字典,释放内存并记录被释放内存的字节数
    // 执行循环流程,删除淘汰数据
    while (mem_freed < (long long)mem_tofree) {
        int j, k, i;
        static unsigned int next_db = 0;
        sds bestkey = NULL;
        int bestdbid;
        redisDb *db;
        dict *dict;
        dictEntry *de;
        if (server.maxmemory_policy & (MAXMEMORY_FLAG_LRU | MAXMEMORY_FLAG_LFU) || server.maxmemory_policy == MAXMEMORY_VOLATILE_TTL) {
            struct evictionPoolEntry *pool = EvictionPoolLRU;

            while (bestkey == NULL) {
                unsigned long total_keys = 0, keys;

                /* We don't want to make local-db choices when expiring keys,
                 * so to start populate the eviction pool sampling keys from
                 * every DB. */
                for (i = 0; i < server.dbnum; i++) {
                    // 对Redis server上的每一个数据库都执行
                    db = server.db + i;
                    // 根据淘汰策略,决定使用全局哈希表还是设置了过期时间的key的哈希表
                    dict = (server.maxmemory_policy & MAXMEMORY_FLAG_ALLKEYS) ? db->dict : db->expires;
                    if ((keys = dictSize(dict)) != 0) {
                        // 将选择的哈希表dict传入evictionPoolPopulate函数,同时将全局哈希表也传给evictionPoolPopulate函数
                        evictionPoolPopulate(i, dict, db->dict, pool);
                        total_keys += keys;
                    }
                }
                if (!total_keys)
                    break; /* No keys to evict. */

                /* Go backward from best to worst element to evict. */
                for (k = EVPOOL_SIZE - 1; k >= 0; k--) { // 从数组最后一个key开始查找
                    if (pool[k].key == NULL)             // 当前key为空值,则查找下一个key
                        continue;
                    bestdbid = pool[k].dbid;
                    // 从全局哈希表或是expire哈希表中,获取当前key对应的键值对;并将当前key从EvictionPoolLRU数组删除

                    if (server.maxmemory_policy & MAXMEMORY_FLAG_ALLKEYS) {
                        // 那么淘汰的目标为所有数据库键
                        de = dictFind(server.db[bestdbid].dict, pool[k].key);
                    }
                    else {
                        // 那么淘汰的目标为带过期时间的数据库键
                        de = dictFind(server.db[bestdbid].expires, pool[k].key);
                    }

                    /* Remove the entry from the pool. */
                    if (pool[k].key != pool[k].cached)
                        sdsfree(pool[k].key);
                    pool[k].key = NULL;
                    pool[k].idle = 0;

                    // 如果当前key对应的键值对不为空,选择当前key为被淘汰的key
                    if (de) {
                        bestkey = dictGetKey(de);
                        break;
                    }
                    else { // 否则,继续查找下个key
                    }
                }
            }
        }
        // 如果使用的是volatile-random and allkeys-random  随机策略,那么从目标字典中随机选出键
        else if (server.maxmemory_policy == MAXMEMORY_ALLKEYS_RANDOM || server.maxmemory_policy == MAXMEMORY_VOLATILE_RANDOM) {
            /* When evicting a random key, we try to evict a key for
             * each DB, so we use the static 'next_db' variable to
             * incrementally visit all DBs. */
            for (i = 0; i < server.dbnum; i++) {
                j = (++next_db) % server.dbnum;
                db = server.db + j;
                dict = (server.maxmemory_policy == MAXMEMORY_ALLKEYS_RANDOM) ? db->dict : db->expires;
                if (dictSize(dict) != 0) {
                    de = dictGetRandomKey(dict);
                    bestkey = dictGetKey(de);
                    bestdbid = j;
                    break;
                }
            }
        }

        /* Finally remove the selected key. */
        // 删除被选中的键
        if (bestkey) {
            db = server.db + bestdbid;
            // 数组的第一个元素是删除操作对应的命令对象,而第二个元素是被删除的 key 对象.
            robj *keyobj = createStringObject(bestkey, sdslen(bestkey));
            /* We compute the amount of memory freed by db*Delete() alone.
             * It is possible that actually the memory needed to propagate
             * the DEL in AOF and replication link is greater than the one
             * we are freeing removing the key, but we can't account for
             * that otherwise we would never exit the loop.
             *
             * Same for CSC invalidation messages generated by signalModifiedKey.
             *
             * AOF and Output buffer memory will be freed eventually so
             * we only care about memory used by the key space. */
            // 计算删除键所释放的内存数量

            del_before = (long long)zmalloc_used_memory();
            latencyStartMonitor(eviction_latency);
            if (server.lazyfree_lazy_eviction) {
                dbAsyncDelete(db, keyobj); // 如果启用了惰性删除,则进行异步删除
            }
            else {
                dbSyncDelete(db, keyobj); // 否则,进行同步删除
            }
            latencyEndMonitor(eviction_latency);
            latencyAddSampleIfNeeded("eviction-del", eviction_latency);
            delta = del_before - (long long)zmalloc_used_memory(); // 根据当前内存使用量计算数据删除前后释放的内存量
            mem_freed += delta;                                    // 更新已释放的内存量
            server.stat_evictedkeys++;                             // 对淘汰键的计数器增一
            signalModifiedKey(NULL, db, keyobj);
            notifyKeyspaceEvent(NOTIFY_EVICTED, "evicted", keyobj, db->id);
            propagateDeletion(db, keyobj, server.lazyfree_lazy_eviction); // 删除对应的key
            decrRefCount(keyobj);
            keys_freed++;

            if (keys_freed % 16 == 0) {
                /* When the memory to free starts to be big enough, we may
                 * start spending so much time here that is impossible to
                 * deliver data to the replicas fast enough, so we force the
                 * transmission here inside the loop. */
                if (slaves) {
                    flushSlavesOutputBuffers();
                }

                /* Normally our stop condition is the ability to release
                 * a fixed, pre-computed amount of memory. However when we
                 * are deleting objects in another thread, it's better to
                 * check, from time to time, if we already reached our target
                 * memory, since the "mem_freed" amount is computed only
                 * across the dbAsyncDelete() call, while the thread can
                 * release the memory all the time. */
                // 如果使用了惰性删除,并且每删除key后,统计下当前内存使用量
                if (server.lazyfree_lazy_eviction) {
                    // 计算当前内存使用量是否不超过最大内存容量
                    if (getMaxmemoryState(NULL, NULL, NULL, NULL) == C_OK) {
                        break;
                    }
                }

                /* After some time, exit the loop early - even if memory limit
                 * hasn't been reached.  If we suddenly need to free a lot of
                 * memory, don't want to spend too much time here.  */
                if (elapsedUs(evictionTimer) > eviction_time_limit_us) {
                    // We still need to free memory - start eviction timer proc
                    startEvictionTimeProc();
                    break;
                }
            }
        }
        else {
            goto cant_free; /* nothing to free... */
        }
    }
    /* at this point, the memory is OK, or we have reached the time limit */
    result = (isEvictionProcRunning) ? EVICT_RUNNING : EVICT_OK;

cant_free:
    if (result == EVICT_FAIL) {
        /* At this point, we have run out of evictable items.  It's possible
         * that some items are being freed in the lazyfree thread.  Perform a
         * short wait here if such jobs exist, but don't wait long.  */
        if (bioPendingJobsOfType(BIO_LAZY_FREE)) {
            usleep(eviction_time_limit_us);
            if (getMaxmemoryState(NULL, NULL, NULL, NULL) == C_OK) {
                result = EVICT_OK;
            }
        }
    }

    serverAssert(server.core_propagates); /* This function should not be re-entrant */

    /* Propagate all DELs */
    propagatePendingCommands();

    server.core_propagates = prev_core_propagates;
    server.propagate_no_multi = 0;

    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("eviction-cycle", latency);

update_metrics:
    if (result == EVICT_RUNNING || result == EVICT_FAIL) {
        if (server.stat_last_eviction_exceeded_time == 0)
            elapsedStart(&server.stat_last_eviction_exceeded_time);
    }
    else if (result == EVICT_OK) {
        if (server.stat_last_eviction_exceeded_time != 0) {
            server.stat_total_eviction_exceeded_time += elapsedUs(server.stat_last_eviction_exceeded_time);
            server.stat_last_eviction_exceeded_time = 0;
        }
    }
    return result;
}
