// 针对过期 key 可以使用不同删除策略

#include "over-server.h"

/*-----------------------------------------------------------------------------
 * Incremental collection of expired keys.
 *
 * When keys are accessed they are expired on-access. However we need a
 * mechanism in order to ensure keys are eventually removed when expired even
 * if no access is performed on them.
 *----------------------------------------------------------------------------*/

// 如果键已经过期,那么移除它,并返回 1 ,否则不做动作,并返回 0 .
// activeExpireCycle的helper函数               参数 now 是毫秒格式的当前时间
int activeExpireCycleTryExpire(redisDb *db, dictEntry *de, long long now) {
    long long t = dictGetSignedIntegerVal(de); // 获取键的过期时间

    if (now > t) {
        sds key = dictGetKey(de); // 键已过期

        robj *keyobj = createStringObject(key, sdslen(key));
        deleteExpiredKeyAndPropagate(db, keyobj);
        decrRefCount(keyobj); // 更新计数器
        return 1;
    }
    else {
        return 0; // 键未过期
    }
}

/*
 * 函数尝试删除数据库中已经过期的键.
 * 当带有过期时间的键比较少时,函数运行得比较保守,
 * 如果带有过期时间的键比较多,那么函数会以更积极的方式来删除过期键,
 * 从而可能地释放被过期键占用的内存.
 * 每次循环中被测试的数据库数目不会超过 REDIS_DBCRON_DBS_PER_CALL .
 *
 * 过期循环的类型：
    1、 * 如果循环的类型为 ACTIVE_EXPIRE_CYCLE_FAST ,
        * 那么函数会以“快速过期”模式执行,
        * 执行的时间不会长过 ACTIVE_EXPIRE_CYCLE_FAST_DURATION 毫秒,并且在相同的时间内不会再重复
        * 如果最近的慢速循环没有因为时间限制条件而终止,该循环也将完全拒绝运行.

    2、 按照server.hz频率进行工作
        然而,慢速循环会因超时而退出,因为它使用了太多的时间.出于这个原因,在beforeSleep()函数中,
        该函数也被调用,以便在每个事件循环周期执行一个快速循环.快速循环将尝试执行较少的工作,但会更频繁地进行.

        如果类型是ACTIVE_EXPIRE_CYCLE_SLOW,则执行该正常的过期周期,
        在快速周期中,一旦数据库中已经过期的键的数量估计低于一个给定的百分比,对每个数据库的检查就会中断,
        以避免做太多的工作而获得太多储存量太少.

 */

#define ACTIVE_EXPIRE_CYCLE_KEYS_PER_LOOP 20    // 默认每个数据库检查的键数量
#define ACTIVE_EXPIRE_CYCLE_FAST_DURATION 1000  // 1000微秒
#define ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC 25   // 使用CPU的最大百分比
#define ACTIVE_EXPIRE_CYCLE_ACCEPTABLE_STALE 10 // %的过期键,之后我们要做额外的努力.

// 过期键的定期删除
void activeExpireCycle(int type) {
    // 根据配置过期 调整运行参数.默认工作量是1,最大可配置工作量是10
    unsigned long effort = server.active_expire_effort - 1; // 索引调整

    // 每个循环影响的键数 20 + 20/4*1
    unsigned long config_keys_per_loop = ACTIVE_EXPIRE_CYCLE_KEYS_PER_LOOP + ACTIVE_EXPIRE_CYCLE_KEYS_PER_LOOP / 4 * effort;
    // 每个循环耗时 1000 + 1000/4*1
    unsigned long config_cycle_fast_duration = ACTIVE_EXPIRE_CYCLE_FAST_DURATION + ACTIVE_EXPIRE_CYCLE_FAST_DURATION / 4 * effort;
    // 每多一个effort,cpu上限多2%
    unsigned long config_cycle_slow_time_perc = ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC + 2 * effort;

    unsigned long config_cycle_acceptable_stale = ACTIVE_EXPIRE_CYCLE_ACCEPTABLE_STALE - effort;

    // static 变量,下次调用值不变
    static unsigned int current_db = 0; // 静态变量,用来累积函数连续执行时的数据
    static int timelimit_exit = 0;      // 上一次调用设置的 时间限制
                                        // 如果 timelimit_exit 为真,那么说明还有更多删除工作要做,
                                        // 那么在 beforeSleep() 函数调用时,程序会再次执行这个函数.

    static long long last_fast_cycle = 0; // 上一次循环的时间

    int j, iteration = 0;
    int dbs_per_call = CRON_DBS_PER_CALL; // 默认每次处理的数据库数量
    long long start = ustime();           // 函数开始的时间
    long long timelimit;
    long long elapsed;

    // 当客户端暂停时,数据集应该是静态的,不仅从客户端不能写入的角度来看,而且从键的过期和驱逐不被执行的角度来看.
    if (checkClientPauseTimeoutAndReturnIfPaused()) {
        return; // 暂停写、暂停所有
    }

    // 快速模式
    if (type == ACTIVE_EXPIRE_CYCLE_FAST) {
        /* Don't start a fast cycle if the previous cycle did not exit
         * for time limit, unless the percentage of estimated stale keys is
         * too high. Also never repeat a fast cycle for the same period
         * as the fast cycle total duration itself. */
        // 如果上次函数没有触发 timelimit_exit ,那么不执行处理
        if (!timelimit_exit && server.stat_expired_stale_perc < config_cycle_acceptable_stale)
            return;
        // 如果距离上次执行未够一定时间,那么不执行处理
        if (start < last_fast_cycle + (long long)config_cycle_fast_duration * 2)
            return;
        // 运行到这里,说明执行快速处理,记录当前时间
        last_fast_cycle = start;
    }

    /* 一般情况下,函数只处理 REDIS_DBCRON_DBS_PER_CALL 个数据库,,但有两个例外.
     * 1)  当前数据库的数量小于 REDIS_DBCRON_DBS_PER_CALL.
     * 2)  如果上次处理遇到了时间上限,那么这次需要对所有数据库进行扫描, 这可以避免过多的过期键占用空间
     * */
    if (dbs_per_call > server.dbnum || timelimit_exit) {
        dbs_per_call = server.dbnum;
    }

    /* We can use at max 'config_cycle_slow_time_perc' percentage of CPU
     * time per iteration. Since this function gets called with a frequency of
     * server.hz times per second, the following is the max amount of
     * microseconds we can spend in this function. */
    // 函数处理的微秒时间上限
    // ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC 默认为 25 ,也即是 25 % 的 CPU 时间
    timelimit = config_cycle_slow_time_perc * 1000000 / server.hz / 100;
    timelimit_exit = 0;
    if (timelimit <= 0)
        timelimit = 1;
    // 如果是运行在快速模式之下
    // 那么最多只能运行 FAST_DURATION 微秒
    // 默认值为 1000 （微秒）
    if (type == ACTIVE_EXPIRE_CYCLE_FAST)
        timelimit = config_cycle_fast_duration; /* in microseconds. */

    /* Accumulate some global stats as we expire keys, to have some idea
     * about the number of keys that are already logically expired, but still
     * existing inside the database. */
    long total_sampled = 0;
    long total_expired = 0;

    /* Sanity: There can't be any pending commands to propagate when
     * we're in cron */
    serverAssert(server.also_propagate.numops == 0);
    server.core_propagates = 1;
    server.propagate_no_multi = 1;
    // 遍历数据库
    for (j = 0; j < dbs_per_call && timelimit_exit == 0; j++) {
        /* Expired and checked in a single loop. */
        unsigned long expired, sampled;
        // 指向要处理的数据库
        redisDb *db = server.db + (current_db % server.dbnum);

        /* Increment the DB now so we are sure if we run out of time
         * in the current DB we'll restart from the next. This allows to
         * distribute the time evenly across DBs. */
        // 为 DB 计数器加一,如果进入 do 循环之后因为超时而跳出
        // 那么下次会直接从下个 DB 开始处理
        current_db++;

        /* Continue to expire if at the end of the cycle there are still
         * a big percentage of keys to expire, compared to the number of keys
         * we scanned. The percentage, stored in config_cycle_acceptable_stale
         * is not fixed, but depends on the Redis configured "expire effort". */
        do {
            unsigned long num, slots;
            long long now, ttl_sum;
            int ttl_samples;
            iteration++; // 更新遍历次数

            /* If there is nothing to expire try next DB ASAP. */
            // 获取数据库中带过期时间的键的数量
            // 如果该数量为 0 ,直接跳过这个数据库
            if ((num = dictSize(db->expires)) == 0) {
                db->avg_ttl = 0;
                break;
            }
            slots = dictSlots(db->expires); // 获取数据库中键值对的数量
            now = mstime();                 // 当前时间

            /* When there are less than 1% filled slots, sampling the key
             * space is expensive, so stop here waiting for better times...
             * The dictionary will be resized asap. */
            // 这个数据库的使用率低于 1% ,扫描起来太费力了（大部分都会 MISS）
            // 跳过,等待字典收缩程序运行
            if (slots > DICT_HT_INITIAL_SIZE && (num * 100 / slots < 1))
                break;

            /* The main collection cycle. Sample random keys among keys
             * with an expire set, checking for expired ones. */
            expired = 0;     // 已处理过期键计数器
            sampled = 0;     //
            ttl_sum = 0;     // 键的总 TTL 计数器
            ttl_samples = 0; // 总共处理的键计数器

            if (num > config_keys_per_loop) { //
                // 每次最多只能检查 LOOKUPS_PER_LOOP 个键
                num = config_keys_per_loop;
            }
            /* Here we access the low level representation of the hash table
             * for speed concerns: this makes this code coupled with dict.c,
             * but it hardly changed in ten years.
             *
             * Note that certain places of the hash table may be empty,
             * so we want also a stop condition about the number of
             * buckets that we scanned. However scanning for free buckets
             * is very fast: we are in the cache line scanning a sequential
             * array of NULL pointers, so we can scan a lot more buckets
             * than keys in the same time. */
            long max_buckets = num * 20;
            long checked_buckets = 0;
            // 开始遍历数据库
            while (sampled < num && checked_buckets < max_buckets) { //
                for (int table = 0; table < 2; table++) {
                    // 从 expires 中随机取出一个带过期时间的键
                    if (table == 1 && !dictIsRehashing(db->expires))
                        break;

                    unsigned long idx = db->expires_cursor;
                    idx &= DICTHT_SIZE_MASK(db->expires->ht_size_exp[table]);
                    dictEntry *de = db->expires->ht_table[table][idx];
                    long long ttl;

                    /* Scan the current bucket of the current table. */
                    checked_buckets++;
                    while (de) {
                        /* Get the next entry now since this entry may get
                         * deleted. */
                        dictEntry *e = de;
                        de = de->next;
                        // 计算 TTL
                        ttl = dictGetSignedIntegerVal(e) - now;
                        // 如果键已经过期,那么删除它,并将 expired 计数器增一
                        if (activeExpireCycleTryExpire(db, e, now))
                            expired++;
                        if (ttl > 0) {
                            // 累积键的 TTL
                            ttl_sum += ttl;
                            // 累积处理键的个数
                            ttl_samples++;
                        }
                        sampled++;
                    }
                }
                db->expires_cursor++;
            }
            total_expired += expired;
            total_sampled += sampled;

            /* Update the average TTL stats for this database. */
            // 为这个数据库更新平均 TTL 统计数据
            if (ttl_samples) {
                // 计算当前平均值
                long long avg_ttl = ttl_sum / ttl_samples;

                /* Do a simple running average with a few samples.
                 * We just use the current estimate with a weight of 2%
                 * and the previous estimate with a weight of 98%. */
                // 如果这是第一次设置数据库平均 TTL ,那么进行初始化
                if (db->avg_ttl == 0) {
                    db->avg_ttl = avg_ttl;
                }
                // 取数据库的上次平均 TTL 和今次平均 TTL 的平均值
                db->avg_ttl = (db->avg_ttl / 50) * 49 + (avg_ttl / 50);
            }

            /* We can't block forever here even if there are many keys to
             * expire. So after a given amount of milliseconds return to the
             * caller waiting for the other active expire cycle. */
            // 我们不能用太长时间处理过期键,
            // 所以这个函数执行一定时间之后就要返回
            if ((iteration & 0xf) == 0) { // 每遍历 16 次执行一次

                elapsed = ustime() - start;
                if (elapsed > timelimit) {
                    // 如果遍历次数正好是 16 的倍数
                    // 并且遍历的时间超过了 timelimit
                    // 那么断开 timelimit_exit
                    timelimit_exit = 1;
                    server.stat_expired_time_cap_reached_count++;
                    break;
                }
            }
            /* We don't repeat the cycle for the current database if there are
             * an acceptable amount of stale keys (logically expired but yet
             * not reclaimed). */
            // 如果已删除的过期键占当前总数据库带过期时间的键数量的 25 %
            // 那么不再遍历
        } while (sampled == 0 || (expired * 100 / sampled) > config_cycle_acceptable_stale);
    }

    serverAssert(server.core_propagates); /* This function should not be re-entrant */

    /* Propagate all DELs */
    propagatePendingCommands();

    server.core_propagates = 0;
    server.propagate_no_multi = 0;

    elapsed = ustime() - start;
    server.stat_expire_cycle_time_used += elapsed;
    latencyAddSampleIfNeeded("expire-cycle", elapsed / 1000);

    /* Update our estimate of keys existing but yet to be expired.
     * Running average with this sample accounting for 5%. */
    double current_perc;
    if (total_sampled) {
        current_perc = (double)total_expired / total_sampled;
    }
    else
        current_perc = 0;
    server.stat_expired_stale_perc = (current_perc * 0.05) + (server.stat_expired_stale_perc * 0.95);
}

/*-----------------------------------------------------------------------------
 * Expires of keys created in writable slaves
 *
 * Normally slaves do not process expires: they wait the masters to synthesize
 * DEL operations in order to retain consistency. However writable slaves are
 * an exception: if a key is created in the slave and an expire is assigned
 * to it, we need a way to expire such a key, since the master does not know
 * anything about such a key.
 *
 * In order to do so, we track keys created in the slave side with an expire
 * set, and call the expireSlaveKeys() function from time to time in order to
 * reclaim the keys if they already expired.
 *
 * Note that the use case we are trying to cover here, is a popular one where
 * slaves are put in writable mode in order to compute slow operations in
 * the slave side that are mostly useful to actually read data in a more
 * processed way. Think at sets intersections in a tmp key, with an expire so
 * that it is also used as a cache to avoid intersecting every time.
 *
 * This implementation is currently not perfect but a lot better than leaking
 * the keys as implemented in 3.2.
 *----------------------------------------------------------------------------*/

/* The dictionary where we remember key names and database ID of keys we may
 * want to expire from the slave. Since this function is not often used we
 * don't even care to initialize the database at startup. We'll do it once
 * the feature is used the first time, that is, when rememberSlaveKeyWithExpire()
 * is called.
 *
 * The dictionary has an SDS string representing the key as the hash table
 * key, while the value is a 64 bit unsigned integer with the bits corresponding
 * to the DB where the keys may exist set to 1. Currently the keys created
 * with a DB id > 63 are not expired, but a trivial fix is to set the bitmap
 * to the max 64 bit unsigned value when we know there is a key with a DB
 * ID greater than 63, and check all the configured DBs in such a case. */
dict *slaveKeysWithExpire = NULL;

// 检查带有过期设置的主服务器创建的keys  以检查它们是否应该被清除.
void expireSlaveKeys(void) {
    if (slaveKeysWithExpire == NULL || dictSize(slaveKeysWithExpire) == 0)
        return;

    int cycles = 0, noexpire = 0;
    mstime_t start = mstime();
    while (1) {
        dictEntry *de = dictGetRandomKey(slaveKeysWithExpire);
        sds keyname = dictGetKey(de);
        uint64_t dbids = dictGetUnsignedIntegerVal(de);
        uint64_t new_dbids = 0;

        /* Check the key against every database corresponding to the
         * bits set in the value bitmap. */
        int dbid = 0;
        while (dbids && dbid < server.dbnum) {
            if ((dbids & 1) != 0) {
                redisDb *db = server.db + dbid;
                dictEntry *expire = dictFind(db->expires, keyname);
                int expired = 0;

                if (expire && activeExpireCycleTryExpire(server.db + dbid, expire, start)) {
                    expired = 1;
                }

                /* If the key was not expired in this DB, we need to set the
                 * corresponding bit in the new bitmap we set as value.
                 * At the end of the loop if the bitmap is zero, it means we
                 * no longer need to keep track of this key. */
                if (expire && !expired) {
                    noexpire++;
                    new_dbids |= (uint64_t)1 << dbid;
                }
            }
            dbid++;
            dbids >>= 1;
        }

        /* Set the new bitmap as value of the key, in the dictionary
         * of keys with an expire set directly in the writable slave. Otherwise
         * if the bitmap is zero, we no longer need to keep track of it. */
        if (new_dbids)
            dictSetUnsignedIntegerVal(de, new_dbids);
        else
            dictDelete(slaveKeysWithExpire, keyname);

        /* Stop conditions: found 3 keys we can't expire in a row or
         * time limit was reached. */
        cycles++;
        if (noexpire > 3)
            break;
        if ((cycles % 64) == 0 && mstime() - start > 1)
            break;
        if (dictSize(slaveKeysWithExpire) == 0)
            break;
    }
}

/* Track keys that received an EXPIRE or similar command in the context
 * of a writable slave. */
void rememberSlaveKeyWithExpire(redisDb *db, robj *key) {
    if (slaveKeysWithExpire == NULL) {
        static dictType dt = {
            dictSdsHash,       /* hash function */
            NULL,              /* key dup */
            NULL,              /* val dup */
            dictSdsKeyCompare, /* key compare */
            dictSdsDestructor, /* key destructor */
            NULL,              /* val destructor */
            NULL               /* allow to expand */
        };
        slaveKeysWithExpire = dictCreate(&dt);
    }
    if (db->id > 63)
        return;

    dictEntry *de = dictAddOrFind(slaveKeysWithExpire, key->ptr);
    /* If the entry was just created, set it to a copy of the SDS string
     * representing the key: we don't want to need to take those keys
     * in sync with the main DB. The keys will be removed by expireSlaveKeys()
     * as it scans to find keys to remove. */
    if (de->key == key->ptr) {
        de->key = sdsdup(key->ptr);
        dictSetUnsignedIntegerVal(de, 0);
    }

    uint64_t dbids = dictGetUnsignedIntegerVal(de);
    dbids |= (uint64_t)1 << db->id;
    dictSetUnsignedIntegerVal(de, dbids);
}

/* Return the number of keys we are tracking. */
size_t getSlaveKeyWithExpireCount(void) {
    if (slaveKeysWithExpire == NULL)
        return 0;
    return dictSize(slaveKeysWithExpire);
}

/* Remove the keys in the hash table. We need to do that when data is
 * flushed from the server. We may receive new keys from the master with
 * the same name/db and it is no longer a good idea to expire them.
 *
 * Note: technically we should handle the case of a single DB being flushed
 * but it is not worth it since anyway race conditions using the same set
 * of key names in a writable slave and in its master will lead to
 * inconsistencies. This is just a best-effort thing we do. */
void flushSlaveKeysWithExpireList(void) {
    if (slaveKeysWithExpire) {
        dictRelease(slaveKeysWithExpire);
        slaveKeysWithExpire = NULL;
    }
}

int checkAlreadyExpired(long long when) {
    /* EXPIRE with negative TTL, or EXPIREAT with a timestamp into the past
     * should never be executed as a DEL when load the AOF or in the context
     * of a slave instance.
     *
     * Instead we add the already expired key to the database with expire time
     * (possibly in the past) and wait for an explicit DEL from the master. */
    return (when <= mstime() && !server.loading && !server.masterhost);
}

#define EXPIRE_NX (1 << 0)
#define EXPIRE_XX (1 << 1)
#define EXPIRE_GT (1 << 2)
#define EXPIRE_LT (1 << 3)

/* Parse additional flags of expire commands
 *
 * Supported flags:
 * - NX: set expiry only when the key has no expiry
 * - XX: set expiry only when the key has an existing expiry
 * - GT: set expiry only when the new expiry is greater than current one
 * - LT: set expiry only when the new expiry is less than current one */
int parseExtendedExpireArgumentsOrReply(client *c, int *flags) {
    int nx = 0, xx = 0, gt = 0, lt = 0;

    int j = 3;
    while (j < c->argc) {
        char *opt = c->argv[j]->ptr;
        if (!strcasecmp(opt, "nx")) {
            *flags |= EXPIRE_NX;
            nx = 1;
        }
        else if (!strcasecmp(opt, "xx")) {
            *flags |= EXPIRE_XX;
            xx = 1;
        }
        else if (!strcasecmp(opt, "gt")) {
            *flags |= EXPIRE_GT;
            gt = 1;
        }
        else if (!strcasecmp(opt, "lt")) {
            *flags |= EXPIRE_LT;
            lt = 1;
        }
        else {
            addReplyErrorFormat(c, "Unsupported option %s", opt);
            return C_ERR;
        }
        j++;
    }

    if ((nx && xx) || (nx && gt) || (nx && lt)) {
        addReplyError(c, "NX and XX, GT or LT options at the same time are not compatible");
        return C_ERR;
    }

    if (gt && lt) {
        addReplyError(c, "GT and LT options at the same time are not compatible");
        return C_ERR;
    }

    return C_OK;
}

/*-----------------------------------------------------------------------------
 * Expires Commands
 *----------------------------------------------------------------------------*/

/* This is the generic command implementation for EXPIRE, PEXPIRE, EXPIREAT
 * and PEXPIREAT. Because the command second argument may be relative or absolute
 * the "basetime" argument is used to signal what the base time is (either 0
 * for *AT variants of the command, or the current time for relative expires).
 *
 * unit is either UNIT_SECONDS or UNIT_MILLISECONDS, and is only used for
 * the argv[2] parameter. The basetime is always specified in milliseconds.
 *
 * Additional flags are supported and parsed via parseExtendedExpireArguments */
void expireGenericCommand(client *c, long long basetime, int unit) {
    robj *key = c->argv[1], *param = c->argv[2];
    long long when; /* unix time in milliseconds when the key will expire. */
    long long current_expire = -1;
    int flag = 0;

    /* checking optional flags */
    if (parseExtendedExpireArgumentsOrReply(c, &flag) != C_OK) {
        return;
    }

    if (getLongLongFromObjectOrReply(c, param, &when, NULL) != C_OK)
        return;

    /* EXPIRE allows negative numbers, but we can at least detect an
     * overflow by either unit conversion or basetime addition. */
    if (unit == UNIT_SECONDS) {
        if (when > LLONG_MAX / 1000 || when < LLONG_MIN / 1000) {
            addReplyErrorExpireTime(c);
            return;
        }
        when *= 1000;
    }

    if (when > LLONG_MAX - basetime) {
        addReplyErrorExpireTime(c);
        return;
    }
    when += basetime;

    /* No key, return zero. */
    if (lookupKeyWrite(c->db, key) == NULL) {
        addReply(c, shared.czero);
        return;
    }

    if (flag) {
        current_expire = getExpire(c->db, key);

        /* NX option is set, check current expiry */
        if (flag & EXPIRE_NX) {
            if (current_expire != -1) {
                addReply(c, shared.czero);
                return;
            }
        }

        /* XX option is set, check current expiry */
        if (flag & EXPIRE_XX) {
            if (current_expire == -1) {
                /* reply 0 when the key has no expiry */
                addReply(c, shared.czero);
                return;
            }
        }

        /* GT option is set, check current expiry */
        if (flag & EXPIRE_GT) {
            /* When current_expire is -1, we consider it as infinite TTL,
             * so expire command with gt always fail the GT. */
            if (when <= current_expire || current_expire == -1) {
                /* reply 0 when the new expiry is not greater than current */
                addReply(c, shared.czero);
                return;
            }
        }

        /* LT option is set, check current expiry */
        if (flag & EXPIRE_LT) {
            /* When current_expire -1, we consider it as infinite TTL,
             * but 'when' can still be negative at this point, so if there is
             * an expiry on the key and it's not less than current, we fail the LT. */
            if (current_expire != -1 && when >= current_expire) {
                /* reply 0 when the new expiry is not less than current */
                addReply(c, shared.czero);
                return;
            }
        }
    }

    if (checkAlreadyExpired(when)) {
        robj *aux;

        int deleted = server.lazyfree_lazy_expire ? dbAsyncDelete(c->db, key) : dbSyncDelete(c->db, key);
        serverAssertWithInfo(c, key, deleted);
        server.dirty++;

        /* Replicate/AOF this as an explicit DEL or UNLINK. */
        aux = server.lazyfree_lazy_expire ? shared.unlink : shared.del;
        rewriteClientCommandVector(c, 2, aux, key);
        signalModifiedKey(c, c->db, key);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", key, c->db->id);
        addReply(c, shared.cone);
        return;
    }
    else {
        setExpire(c, c->db, key, when);
        addReply(c, shared.cone);
        /* Propagate as PEXPIREAT millisecond-timestamp */
        robj *when_obj = createStringObjectFromLongLong(when);
        rewriteClientCommandVector(c, 3, shared.pexpireat, key, when_obj);
        decrRefCount(when_obj);
        signalModifiedKey(c, c->db, key);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "expire", key, c->db->id);
        server.dirty++;
        return;
    }
}

/* EXPIRE key seconds */
void expireCommand(client *c) {
    expireGenericCommand(c, mstime(), UNIT_SECONDS);
}

/* EXPIREAT key time */
void expireatCommand(client *c) {
    expireGenericCommand(c, 0, UNIT_SECONDS);
}

/* PEXPIRE key milliseconds */
void pexpireCommand(client *c) {
    expireGenericCommand(c, mstime(), UNIT_MILLISECONDS);
}

/* PEXPIREAT key ms_time */
void pexpireatCommand(client *c) {
    expireGenericCommand(c, 0, UNIT_MILLISECONDS);
}

/* Implements TTL, PTTL, EXPIRETIME and PEXPIRETIME */
void ttlGenericCommand(client *c, int output_ms, int output_abs) {
    long long expire, ttl = -1;

    /* If the key does not exist at all, return -2 */
    if (lookupKeyReadWithFlags(c->db, c->argv[1], LOOKUP_NOTOUCH) == NULL) {
        addReplyLongLong(c, -2);
        return;
    }

    /* The key exists. Return -1 if it has no expire, or the actual
     * TTL value otherwise. */
    expire = getExpire(c->db, c->argv[1]);
    if (expire != -1) {
        ttl = output_abs ? expire : expire - mstime();
        if (ttl < 0)
            ttl = 0;
    }
    if (ttl == -1) {
        addReplyLongLong(c, -1);
    }
    else {
        addReplyLongLong(c, output_ms ? ttl : ((ttl + 500) / 1000));
    }
}

/* TTL key */
void ttlCommand(client *c) {
    ttlGenericCommand(c, 0, 0);
}

/* PTTL key */
void pttlCommand(client *c) {
    ttlGenericCommand(c, 1, 0);
}

/* EXPIRETIME key */
void expiretimeCommand(client *c) {
    ttlGenericCommand(c, 0, 1);
}

/* PEXPIRETIME key */
void pexpiretimeCommand(client *c) {
    ttlGenericCommand(c, 1, 1);
}

/* PERSIST key */
void persistCommand(client *c) {
    if (lookupKeyWrite(c->db, c->argv[1])) {
        if (removeExpire(c->db, c->argv[1])) {
            signalModifiedKey(c, c->db, c->argv[1]);
            notifyKeyspaceEvent(NOTIFY_GENERIC, "persist", c->argv[1], c->db->id);
            addReply(c, shared.cone);
            server.dirty++;
        }
        else {
            addReply(c, shared.czero);
        }
    }
    else {
        addReply(c, shared.czero);
    }
}

/* TOUCH key1 [key2 key3 ... keyN] */
void touchCommand(client *c) {
    int touched = 0;
    for (int j = 1; j < c->argc; j++)
        if (lookupKeyRead(c->db, c->argv[j]) != NULL)
            touched++;
    addReplyLongLong(c, touched);
}
