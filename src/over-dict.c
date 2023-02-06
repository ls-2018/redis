// dictAdd、dictRelace、dictAddorFind -> dictAddRaw -> _dictKeyIndex -> _dictExpandIfNeeded
// dictAddRaw、dictGenericDelete、dictFind、dictGetRandomKey、dictGetSomeKeys -> _dictRehashStep -> dictRehash

#include "fmacros.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/time.h>

#include "over-dict.h"
#include "over-zmalloc.h"
#include "redisassert.h"

// 通过 dictEnableResize() 和 dictDisableResize() 两个函数,
// 程序可以手动地允许或阻止哈希表进行 rehash ,
// 这在 Redis 使用子进程进行保存操作时,可以有效地利用 copy-on-write 机制.
//
// 需要注意的是,并非所有 rehash 都会被 dictDisableResize 阻止:
// 如果已使用节点的数量和字典大小之间的比率,
// 大于字典强制 rehash 比率 dict_force_resize_ratio ,
// 那么 rehash 仍然会（强制）进行.

// 指示字典是否可以 rehash 的标识
static int dict_can_resize = 1;
// 强制 rehash 的比率
static unsigned int dict_force_resize_ratio = 5;

/* -------------------------- 私有属性 ---------------------------- */

static int _dictExpandIfNeeded(dict *d);

static signed char _dictNextExp(unsigned long size);

static long _dictKeyIndex(dict *d, const void *key, uint64_t hash, dictEntry **existing);

static int _dictInit(dict *d, dictType *type);

/* -------------------------- hash 函数 -------------------------------- */

static uint8_t dict_hash_function_seed[16];

void dictSetHashFunctionSeed(uint8_t *seed) {
    // 设置dict 哈希函数
    memcpy(dict_hash_function_seed, seed, sizeof(dict_hash_function_seed));
}

uint8_t *dictGetHashFunctionSeed(void) {
    return dict_hash_function_seed;
}

// 默认使用siphash.c提供的哈希函数
uint64_t siphash(const uint8_t *in, const size_t inlen, const uint8_t *k);

// ok
uint64_t siphash_nocase(const uint8_t *in, const size_t inlen, const uint8_t *k);

uint64_t dictGenHashFunction(const void *key, size_t len) {
    return siphash(key, len, dict_hash_function_seed);
}

uint64_t dictGenCaseHashFunction(const unsigned char *buf, size_t len) {
    return siphash_nocase(buf, len, dict_hash_function_seed);
}

/* ----------------------------- API 实现 ------------------------- */

// 重置（或初始化）给定哈希表的各项属性值
static void _dictReset(dict *d, int htidx) {
    d->ht_table[htidx] = NULL;  // 初始化dictEntry指针
    d->ht_size_exp[htidx] = -1; // 大小指数,实际大小是1<<n
    d->ht_used[htidx] = 0;      // 该哈希表已有的键数
}

// 创建一个新的字典
dict *dictCreate(dictType *type) {
    dict *d = zmalloc(sizeof(*d));
    _dictInit(d, type);
    return d;
}

// 初始化哈希表
int _dictInit(dict *d, dictType *type) {
    _dictReset(d, 0);  // 重置（或初始化）给定哈希表的各项属性值
    _dictReset(d, 1);  // 重置（或初始化）给定哈希表的各项属性值
    d->type = type;    // 设置类型特定函数
    d->rehashidx = -1; // 设置哈希表 rehash 状态
    d->pauserehash = 0;
    return DICT_OK;
}

// 缩小给定字典
// 让它的已用节点数和字典大小之间的比率接近 1:1
int dictResize(dict *d) {
    unsigned long minimal;
    // 不能rehash 或 正在rehash
    if (!dict_can_resize || dictIsRehashing(d)) {
        return DICT_ERR;
    }
    // 计算让比率接近 1:1 所需要的最少节点数量
    minimal = d->ht_used[0];
    if (minimal < DICT_HT_INITIAL_SIZE) { // 4
        minimal = DICT_HT_INITIAL_SIZE;
    }
    return dictExpand(d, minimal);
}

// ########### 实际扩容函数 ####################

// 扩容、创建哈希表 ,size是新的大小  ,创建了一个新的dict
int _dictExpand(dict *d, unsigned long size, int *malloc_failed) {
    if (malloc_failed) {
        *malloc_failed = 0; // 初始默认值
    }

    if (dictIsRehashing(d) || d->ht_used[0] > size) { // 已使用的大小> 新的大小
        return DICT_ERR;
    }

    // 新的hash表
    dictEntry **new_ht_table;
    unsigned long new_ht_used;
    // 计算新的哈希表指数
    signed char new_ht_size_exp = _dictNextExp(size);

    // 发现溢出
    size_t newsize = 1ul << new_ht_size_exp;
    if (newsize < size || newsize * sizeof(dictEntry *) < newsize) {
        return DICT_ERR;
    }
    // rehash 前后大小一致
    if (new_ht_size_exp == d->ht_size_exp[0]) {
        return DICT_ERR;
    }

    // 分配新的hash表,并初始化所有指针为null
    if (malloc_failed) {
        new_ht_table = ztrycalloc(newsize * sizeof(dictEntry *));
        *malloc_failed = new_ht_table == NULL;
        if (*malloc_failed) {
            return DICT_ERR;
        }
    }
    else {
        new_ht_table = zcalloc(newsize * sizeof(dictEntry *));
    }

    new_ht_used = 0;

    /*这是第一次初始化吗?如果是这样，它就不是真正的重哈希我们只是设置了第一个哈希表以便它可以接受键。*/
    if (d->ht_table[0] == NULL) {
        d->ht_size_exp[0] = new_ht_size_exp;
        d->ht_used[0] = new_ht_used;
        d->ht_table[0] = new_ht_table;
        return DICT_OK;
    }

    /* 准备第二个哈希表进行增量重哈希 */
    d->ht_size_exp[1] = new_ht_size_exp;
    d->ht_used[1] = new_ht_used;
    d->ht_table[1] = new_ht_table;
    d->rehashidx = 0;
    return DICT_OK;
}

// 如果内存分配失败,返回DICT_ERR
int dictExpand(dict *d, unsigned long size) {
    return _dictExpand(d, size, NULL);
}

// 如果内存分配失败,返回DICT_ERR
int dictTryExpand(dict *d, unsigned long size) {
    int malloc_failed;
    _dictExpand(d, size, &malloc_failed);
    return malloc_failed ? DICT_ERR : DICT_OK;
}

// 全局哈希表,包含ht[0] ht[1] ; 需要进行键拷贝的桶数量
int dictRehash(dict *d, int n) {
    // 当负载因子大于等于 1 ,并且 Redis 没有在执行 bgsave 命令或者 bgrewiteaof 命令,也就是没有执行 RDB 快照或没有进行 AOF 重写的时候,就会进行 rehash 操作
    // 当负载因子大于等于 5 时,此时说明哈希冲突非常严重了,不管有没有有在执行 RDB 快照或 AOF 重写,都会强制进行 rehash 操作

    // rehash是会阻塞主线程的
    int empty_visits = n * 10; // 访问的最大 空 桶数
    if (!dictIsRehashing(d)) {
        return 0;
    }
    // 主循环,根据要拷贝的bucket数量n,循环n次后停止或ht[0]中的数据迁移完停止
    while (n-- && d->ht_used[0] != 0) {
        dictEntry *de, *nextde;

        /* Note that rehashidx can't overflow as we are sure there are more
         * elements because ht[0].used != 0 */
        assert(DICTHT_SIZE(d->ht_size_exp[0]) > (unsigned long)d->rehashidx);
        while (d->ht_table[0][d->rehashidx] == NULL) { // 跳到下一个有数据的位置
            d->rehashidx++;
            --empty_visits;
            if (empty_visits == 0) {
                return 1;
            }
        }
        // bucket
        de = d->ht_table[0][d->rehashidx];
        // 移动bucket中每一个key 到 新的bucket中
        while (de) {
            nextde = de->next;
            // 获取key在新dict中的索引
            uint64_t h = dictHashKey(d, de->key) & DICTHT_SIZE_MASK(d->ht_size_exp[1]);
            de->next = d->ht_table[1][h];
            d->ht_table[1][h] = de;
            d->ht_used[0]--;
            d->ht_used[1]++;
            de = nextde;
        }
        d->ht_table[0][d->rehashidx] = NULL; // 将旧的dict置空了
        d->rehashidx++;
    }

    // 判断ht[0]的数据是否迁移完成
    if (d->ht_used[0] == 0) {
        zfree(d->ht_table[0]);                 // 释放旧的dict
        d->ht_table[0] = d->ht_table[1];       // 从1移动到0
        d->ht_used[0] = d->ht_used[1];         //
        d->ht_size_exp[0] = d->ht_size_exp[1]; //
        _dictReset(d, 1);                      // 重置ht[1]的大小为0
        d->rehashidx = -1;                     // 设置全局哈希表的rehashidx标识为-1,表示rehash结束
        return 0;                              // 返回0,表示ht[0]中所有元素都迁移完
    }
    return 1; // 返回1,表示ht[0]中仍然有元素没有迁移完
}

// 返回以毫秒为单位的 UNIX 时间戳
long long timeInMilliseconds(void) {
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (((long long)tv.tv_sec) * 1000) + (tv.tv_usec / 1000);
}

// 在给定毫秒数内,以 100步 为单位,对字典进行 rehash .
// 定时 rehash 只会迁移全局哈希表中的数据,不会定时迁移 Hash/Set/Sorted Set 下的哈希表的数据,这些哈希表只会在操作数据时做实时的渐进式 rehash
int dictRehashMilliseconds(dict *d, int ms) {
    // 记录开始时间
    long long start = timeInMilliseconds();
    int rehashes = 0;

    while (dictRehash(d, 100)) {
        rehashes += 100;
        // 如果时间已过,跳出
        if (timeInMilliseconds() - start > ms) {
            break;
        }
    }
    return rehashes;
}

// 对字典一步一步的rehash
// 在字典不存在安全迭代器的情况下,对字典进行单步 rehash .字典有安全迭代器的情况下不能进行 rehash
static void _dictRehashStep(dict *d) {
    // 给dictRehash传入的循环次数参数为1,表明每迁移完一个bucket ,就执行正常操作
    if (d->pauserehash == 0) { // >0 表示rehash被暂停了, 0 表示正常情况下
        dictRehash(d, 1);
    }
}

// 尝试将给定键值对添加到字典中;只有给定键 key 不存在于字典时,添加操作才会成功
int dictAdd(dict *d, void *key, void *val) {
    // 尝试添加键到字典,并返回包含了这个键的新哈希节点
    dictEntry *entry = dictAddRaw(d, key, NULL);
    // 键已存在,添加失败
    if (!entry) {
        return DICT_ERR;
    }
    // 键不存在,设置节点的值
    dictSetVal(d, entry, val);
    // 添加成功
    return DICT_OK;
}

// 尝试将键插入到字典中,如果键已经在字典存在,那么返回 NULL,
// 如果键不存在,那么程序创建新的哈希节点,将节点和键关联,并插入到字典,然后返回节点本身.
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing) {
    long index;
    dictEntry *entry;
    int htidx;

    if (dictIsRehashing(d)) {
        _dictRehashStep(d);
    }

    /* Get the index of the new element, or -1 if
     * the element already exists. */
    if ((index = _dictKeyIndex(d, key, dictHashKey(d, key), existing)) == -1)
        return NULL;

    /* Allocate the memory and store the new entry.
     * Insert the element in top, with the assumption that in a database
     * system it is more likely that recently added entries are accessed
     * more frequently. */
    htidx = dictIsRehashing(d) ? 1 : 0;
    size_t metasize = dictMetadataSize(d);
    entry = zmalloc(sizeof(*entry) + metasize);
    if (metasize > 0) {
        memset(dictMetadata(entry), 0, metasize);
    }
    entry->next = d->ht_table[htidx][index];
    d->ht_table[htidx][index] = entry;
    d->ht_used[htidx]++;

    // 设置新节点的键
    dictSetKey(d, entry, key);
    return entry;
}

// 将给定的键值对添加到字典中,如果键已经存在,那么删除旧有的键值对.
// 新添加,那么返回 1 .更新得来的,那么返回 0 .
int dictReplace(dict *d, void *key, void *val) {
    dictEntry *entry, *existing, auxentry;
    // existing 找到的对应的节点
    // 尝试直接将键值对添加到字典 如果键 key 不存在的话,添加会成功
    entry = dictAddRaw(d, key, &existing);
    if (entry) {
        dictSetVal(d, entry, val);
        return 1;
    }
    // 先保存原有的值的指针
    auxentry = *existing;
    dictSetVal(d, existing, val);
    // 然后释放旧值
    dictFreeVal(d, &auxentry);
    return 0;
}

/* Add or Find:
 * dictAddOrFind() is simply a version of dictAddRaw() that always
 * returns the hash entry of the specified key, even if the key already
* exists and can't be added (in that case the entry of the already * existing key is returned.)

 *
 * See dictAddRaw() for more information. */
dictEntry *dictAddOrFind(dict *d, void *key) {
    dictEntry *entry, *existing;
    entry = dictAddRaw(d, key, &existing);
    return entry ? entry : existing;
}

// 根据传入参数 nofree 的值,决定是否实际释放 key 和 value 的内存空间
// 参数 nofree 决定是否调用键和值的释放函数 0 表示调用,1 表示不调用
// 找到并成功删除返回 DICT_OK ,没找到则返回 DICT_ERR
static dictEntry *dictGenericDelete(dict *d, const void *key, int nofree) {
    uint64_t h, idx;
    dictEntry *he, *prevHe;
    int table;

    if (dictSize(d) == 0) {
        return NULL;
    }
    // 进行单步 rehash ,T = O(1)
    if (dictIsRehashing(d)) {
        _dictRehashStep(d);
    }
    // 计算哈希值
    h = dictHashKey(d, key);

    // 遍历哈希表
    for (table = 0; table <= 1; table++) {
        // 余数
        idx = h & DICTHT_SIZE_MASK(d->ht_size_exp[table]);
        // 拿到对应的链表
        he = d->ht_table[table][idx];
        prevHe = NULL;
        while (he) {
            if (key == he->key || dictCompareKeys(d, key, he->key)) {
                // 从列表中解除元素的链接
                if (prevHe)
                    prevHe->next = he->next;
                else
                    d->ht_table[table][idx] = he->next; // 链表首元素后移
                if (!nofree) {                          // 调用释放函数
                    dictFreeUnlinkedEntry(d, he);
                }
                d->ht_used[table]--;
                return he;
            }
            else {
                // 在key不匹配的情况下,后移
                prevHe = he;
                he = he->next;
            }
        }
        if (!dictIsRehashing(d)) {
            // 不在rehash状态之下
            break;
        }
    }
    // 没找到
    return NULL; /* not found */
}

// 从字典中删除包含给定键的节点,并且调用键值的释放函数来删除键值, 找到并成功删除返回 DICT_OK ,没找到则返回 DICT_ERR
int dictDelete(dict *ht, const void *key) {
    // 0:要释放相应的val内存,
    // 1:不释放相应val内存只删除key
    return dictGenericDelete(ht, key, 0) ? DICT_OK : DICT_ERR;
}

dictEntry *dictUnlink(dict *ht, const void *key) {
    return dictGenericDelete(ht, key, 1);
}

/* 在调用dictUnlink()之后，需要调用这个函数来真正释放条目。用'he' = NULL来调用这个函数是安全的。 */
void dictFreeUnlinkedEntry(dict *d, dictEntry *he) {
    if (he == NULL)
        return;
    dictFreeKey(d, he);
    dictFreeVal(d, he);
    zfree(he);
}

// 删除哈希表上的所有节点,并重置哈希表的各项属性
int _dictClear(dict *d, int htidx, void(callback)(dict *)) {
    unsigned long i;

    // 遍历整个哈希表
    for (i = 0; i < DICTHT_SIZE(d->ht_size_exp[htidx]) && d->ht_used[htidx] > 0; i++) {
        dictEntry *he, *nextHe;

        if (callback && (i & 65535) == 0)
            callback(d);
        // 跳过空索引
        if ((he = d->ht_table[htidx][i]) == NULL)
            continue;
        // 遍历整个链表
        while (he) {
            nextHe = he->next;
            dictFreeKey(d, he);
            dictFreeVal(d, he);
            zfree(he);
            d->ht_used[htidx]--;
            he = nextHe;
        }
    }
    // 释放哈希表结构
    zfree(d->ht_table[htidx]);
    _dictReset(d, htidx); // 重置哈希表属性

    return DICT_OK;
}

// 删除并释放整个字典
void dictRelease(dict *d) {
    // 删除并清空两个哈希表
    _dictClear(d, 0, NULL); // dictRelease
    _dictClear(d, 1, NULL); // dictRelease
    zfree(d);               // 释放节点结构
}

// 返回字典中包含键 key 的节点 ,找到返回节点,找不到返回 NULL
dictEntry *dictFind(dict *d, const void *key) {
    dictEntry *he;
    uint64_t h, idx, table;
    // 字典（的哈希表）为空
    if (dictSize(d) == 0) {
        return NULL;
    }
    // 如果在rehash过程中
    if (dictIsRehashing(d)) {
        _dictRehashStep(d);
    }
    // 计算key的哈希值,调用dict 对应的hash函数
    h = dictHashKey(d, key); // dictSdsCaseHash

    // old 长度为 8
    // new 长度为 16
    // key 哈希值是1

    for (table = 0; table <= 1; table++) {
        // 1 & 2^3 -1  =   0001 & 0111 = 0001
        // 1 & 2^4 -1  =   00001 & 01111 = 00001

        idx = h & DICTHT_SIZE_MASK(d->ht_size_exp[table]);

        // idx 是对应哈希值在每个数组里对应的索引
        he = d->ht_table[table][idx];

        // 从链表中依次便利、查找key一样的值
        while (he) {
            if (key == he->key || dictCompareKeys(d, key, he->key)) {
                return he;
            }
            he = he->next;
        }
        // 如果程序遍历完 0 号哈希表,仍然没找到指定的键的节点
        // 那么程序会检查字典是否在进行 rehash ,
        // 然后才决定是直接返回 NULL ,还是继续查找 1 号哈希表
        if (!dictIsRehashing(d)) {
            return NULL;
        }
    }
    return NULL;
}

// 获取包含给定键的节点的值; 如果节点不为空,返回节点的值,否则返回 NULL
void *dictFetchValue(dict *d, const void *key) {
    dictEntry *he;
    // server.commands=dictCreate(&commandTableDictType);   dictSdsCaseHash
    he = dictFind(d, key);
    return he ? dictGetVal(he) : NULL;
}

// 返回一个64bit的值,代表了当前ht[0] ht[1]的状态
unsigned long long dictFingerprint(dict *d) {
    unsigned long long integers[6], hash = 0;
    int j;

    integers[0] = (long)d->ht_table[0];
    integers[1] = d->ht_size_exp[0];
    integers[2] = d->ht_used[0];
    integers[3] = (long)d->ht_table[1];
    integers[4] = d->ht_size_exp[1];
    integers[5] = d->ht_used[1];

    /* We hash N integers by summing every successive integer with the integer
     * hashing of the previous sum. Basically:
     *
     * Result = hash(hash(hash(int1)+int2)+int3) ...
     *
     * This way the same set of integers in a different order will (likely) hash
     * to a different number. */
    for (j = 0; j < 6; j++) {
        hash += integers[j];
        /* For the hashing step we use Tomas Wang's 64 bit integer hash. */
        hash = (~hash) + (hash << 21); // hash = (hash << 21) - hash - 1;
        hash = hash ^ (hash >> 24);
        hash = (hash + (hash << 3)) + (hash << 8); // hash * 265
        hash = hash ^ (hash >> 14);
        hash = (hash + (hash << 2)) + (hash << 4); // hash * 21
        hash = hash ^ (hash >> 28);
        hash = hash + (hash << 31);
    }
    return hash;
}

// 创建并返回给定字典的不安全迭代器
dictIterator *dictGetIterator(dict *d) {
    dictIterator *iter = zmalloc(sizeof(*iter));

    iter->d = d;
    iter->table = 0;
    iter->index = -1;
    iter->safe = 0;
    iter->entry = NULL;
    iter->nextEntry = NULL;
    return iter;
}

// 创建并返回给定节点的安全迭代器
dictIterator *dictGetSafeIterator(dict *d) {
    dictIterator *i = dictGetIterator(d);

    i->safe = 1;
    return i;
}

// 返回迭代器指向的当前节点 字典迭代完毕时,返回 NULL
dictEntry *dictNext(dictIterator *iter) {
    while (1) {
        // 进入这个循环有两种可能:
        // 1) 这是迭代器第一次运行
        // 2) 当前索引链表中的节点已经迭代完（NULL 为链表的表尾）
        if (iter->entry == NULL) {
            // 初次迭代时执行
            if (iter->index == -1 && iter->table == 0) {
                // 如果是安全迭代器,那么更新安全迭代器计数器
                if (iter->safe) {
                    dictPauseRehashing(iter->d);
                }
                else {
                    // 如果是不安全迭代器,那么计算ht[0] ht[1]状态
                    iter->fingerprint = dictFingerprint(iter->d);
                }
            }
            // 更新索引
            iter->index++;

            // 如果迭代器的当前索引大于当前被迭代的哈希表的大小
            // 那么说明这个哈希表已经迭代完毕
            if (iter->index >= (long)DICTHT_SIZE(iter->d->ht_size_exp[iter->table])) {
                // 如果正在 rehash 的话,那么说明 1 号哈希表也正在使用中
                // 那么继续对 1 号哈希表进行迭代
                if (dictIsRehashing(iter->d) && iter->table == 0) {
                    iter->table++;
                    iter->index = 0;
                    // 如果没有 rehash ,那么说明迭代已经完成
                }
                else {
                    break;
                }
            }

            // 如果进行到这里,说明这个哈希表并未迭代完
            // 更新节点指针,指向下个索引链表的表头节点
            iter->entry = iter->d->ht_table[iter->table][iter->index];
        }
        else {
            // 执行到这里,说明程序正在迭代某个链表
            // 将节点指针指向链表的下个节点
            iter->entry = iter->nextEntry;
        }

        // 如果当前节点不为空,那么也记录下该节点的下个节点
        // 因为安全迭代器有可能会将迭代器返回的当前节点删除
        if (iter->entry) {
            // 我们需要在这里保存'下一个',迭代器用户可能会删除我们返回的条目.
            iter->nextEntry = iter->entry->next;
            return iter->entry;
        }
    }
    // 迭代完毕
    return NULL;
}

// 释放给定字典迭代器
void dictReleaseIterator(dictIterator *iter) {
    if (!(iter->index == -1 && iter->table == 0)) {
        // 释放安全迭代器时,安全迭代器计数器减一
        if (iter->safe)
            dictResumeRehashing(iter->d);
        else
            // 释放不安全迭代器时,验证指纹是否有变化
            assert(iter->fingerprint == dictFingerprint(iter->d));
    }
    zfree(iter);
}

// 随机返回字典中任意一个节点. 可用于实现随机化算法. 如果字典为空,返回 NULL .
dictEntry *dictGetRandomKey(dict *d) {
    dictEntry *he, *orighe;
    unsigned long h;
    int listlen, listele;

    if (dictSize(d) == 0)
        return NULL;
    if (dictIsRehashing(d))
        _dictRehashStep(d);
    // 不用具体抠这个随机的细节
    if (dictIsRehashing(d)) {
        // 如果正在 rehash ,那么将 1 号哈希表也作为随机查找的目标
        unsigned long s0 = DICTHT_SIZE(d->ht_size_exp[0]);
        do {
            /* We are sure there are no elements in indexes from 0
             * to rehashidx-1 */
            h = d->rehashidx + (randomULong() % (dictSlots(d) - d->rehashidx));
            he = (h >= s0) ? d->ht_table[1][h - s0] : d->ht_table[0][h];
        } while (he == NULL);
    }
    else {
        // 否则,只从 0 号哈希表中查找节点
        unsigned long m = DICTHT_SIZE_MASK(d->ht_size_exp[0]);
        do {
            h = randomULong() & m;
            he = d->ht_table[0][h];
        } while (he == NULL);
    }

    // 目前 he 已经指向一个非空的节点链表
    // 程序将从这个链表随机返回一个节点
    listlen = 0;
    orighe = he;
    // 计算节点数量,
    while (he) {
        he = he->next;
        listlen++;
    }
    // 取模,得出随机节点的索引
    listele = random() % listlen;
    // 按索引查找节点
    he = orighe;
    while (listele--) {
        he = he->next;
    }
    // 返回随机节点

    return he;
}

/* This function samples the dictionary to return a few keys from random
 * locations.
 *
 * It does not guarantee to return all the keys specified in 'count', nor
 * it does guarantee to return non-duplicated elements, however it will make
 * some effort to do both things.
 *
 * Returned pointers to hash table entries are stored into 'des' that
 * points to an array of dictEntry pointers. The array must have room for
 * at least 'count' elements, that is the argument we pass to the function
 * to tell how many random elements we need.
 *
 * The function returns the number of items stored into 'des', that may
 * be less than 'count' if the hash table has less than 'count' elements
 * inside, or if not enough elements were found in a reasonable amount of
 * steps.
 *
 * Note that this function is not suitable when you need a good distribution
 * of the returned items, but only when you need to "sample" a given number
 * of continuous elements to run some kind of algorithm or to produce
 * statistics. However the function is much faster than dictGetRandomKey()
 * at producing N elements. */
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count) {
    unsigned long j;      /* internal hash table id, 0 or 1. */
    unsigned long tables; /* 1 or 2 tables? */
    unsigned long stored = 0, maxsizemask;
    unsigned long maxsteps;

    if (dictSize(d) < count)
        count = dictSize(d);
    maxsteps = count * 10;

    /* Try to do a rehashing work proportional to 'count'. */
    for (j = 0; j < count; j++) {
        if (dictIsRehashing(d))
            _dictRehashStep(d);
        else
            break;
    }

    tables = dictIsRehashing(d) ? 2 : 1;
    maxsizemask = DICTHT_SIZE_MASK(d->ht_size_exp[0]);
    // 使用哈希表的sizemask属性和哈希值,计算出索引值
    if (tables > 1 && maxsizemask < DICTHT_SIZE_MASK(d->ht_size_exp[1]))
        maxsizemask = DICTHT_SIZE_MASK(d->ht_size_exp[1]);

    /* Pick a random point inside the larger table. */
    unsigned long i = randomULong() & maxsizemask;
    unsigned long emptylen = 0; /* Continuous empty entries so far. */
    while (stored < count && maxsteps--) {
        for (j = 0; j < tables; j++) {
            /* Invariant of the dict.c rehashing: up to the indexes already
             * visited in ht[0] during the rehashing, there are no populated
             * buckets, so we can skip ht[0] for indexes between 0 and idx-1. */
            if (tables == 2 && j == 0 && i < (unsigned long)d->rehashidx) {
                /* Moreover, if we are currently out of range in the second
                 * table, there will be no elements in both tables up to
                 * the current rehashing index, so we jump if possible.
                 * (this happens when going from big to small table). */
                if (i >= DICTHT_SIZE(d->ht_size_exp[1]))
                    i = d->rehashidx;
                else
                    continue;
            }
            if (i >= DICTHT_SIZE(d->ht_size_exp[j]))
                continue; /* Out of range for this table. */
            dictEntry *he = d->ht_table[j][i];

            /* Count contiguous empty buckets, and jump to other
             * locations if they reach 'count' (with a minimum of 5). */
            if (he == NULL) {
                emptylen++;
                if (emptylen >= 5 && emptylen > count) {
                    i = randomULong() & maxsizemask;
                    emptylen = 0;
                }
            }
            else {
                emptylen = 0;
                while (he) {
                    /* Collect all the elements of the buckets found non
                     * empty while iterating. */
                    *des = he;
                    des++;
                    he = he->next;
                    stored++;
                    if (stored == count)
                        return stored;
                }
            }
        }
        i = (i + 1) & maxsizemask;
    }
    return stored;
}

/* This is like dictGetRandomKey() from the POV of the API, but will do more
 * work to ensure a better distribution of the returned element.
 *
 * This function improves the distribution because the dictGetRandomKey()
 * problem is that it selects a random bucket, then it selects a random
 * element from the chain in the bucket. However elements being in different
 * chain lengths will have different probabilities of being reported. With
 * this function instead what we do is to consider a "linear" range of the table
 * that may be constituted of N buckets with chains of different lengths
 * appearing one after the other. Then we report a random element in the range.
 * In this way we smooth away the problem of different chain lengths. */
#define GETFAIR_NUM_ENTRIES 15

dictEntry *dictGetFairRandomKey(dict *d) {
    dictEntry *entries[GETFAIR_NUM_ENTRIES];
    unsigned int count = dictGetSomeKeys(d, entries, GETFAIR_NUM_ENTRIES);
    /* Note that dictGetSomeKeys() may return zero elements in an unlucky
     * run() even if there are actually elements inside the hash table. So
     * when we get zero, we call the true dictGetRandomKey() that will always
     * yield the element if the hash table has at least one. */
    if (count == 0)
        return dictGetRandomKey(d);
    unsigned int idx = rand() % count;
    return entries[idx];
}

/* Function to reverse bits. Algorithm from:
 * http://graphics.stanford.edu/~seander/bithacks.html#ReverseParallel */
static unsigned long rev(unsigned long v) {
    unsigned long s = CHAR_BIT * sizeof(v); // bit size; must be power of 2
    unsigned long mask = ~0UL;
    while ((s >>= 1) > 0) {
        mask ^= (mask << s);
        v = ((v >> s) & mask) | ((v << s) & ~mask);
    }
    return v;
}

/* dictScan() 函数用于迭代给定字典中的元素.
 *
 * 迭代按以下方式执行:
 *
 * 1)  一开始,你使用 0 作为游标来调用函数.
 * 2)  函数执行一步迭代操作,并返回一个下次迭代时使用的新游标.
 * 3)  当函数返回的游标为 0 时,迭代完成.
 *
 * 函数保证,在迭代从开始到结束期间,一直存在于字典的元素肯定会被迭代到,
 * 但一个元素可能会被返回多次.

 * 每当一个元素被返回时,回调函数 fn 就会被执行, * fn 函数的第一个参数是 privdata ,而第二个参数则是字典节点 de .
 *
 * 工作原理
 *
 * 迭代所使用的算法是由 Pieter Noordhuis 设计的,算法的主要思路是在二进制高位上对游标进行加法计算
 * 也即是说,不是按正常的办法来对游标进行加法计算,而是首先将游标的二进制位翻转（reverse）过来,然后对翻转后的值进行加法计算,
 * 最后再次对加法计算之后的结果进行翻转.
 *
 * 这一策略是必要的,因为在一次完整的迭代过程中,哈希表的大小有可能在两次迭代之间发生改变.
 *
 * 哈希表的大小总是 2 的某个次方,并且哈希表使用链表来解决冲突,
 * 因此一个给定元素在一个给定表的位置总可以通过 Hash(key) & SIZE-1
 * 公式来计算得出,其中 SIZE-1 是哈希表的最大索引值,
 * 这个最大索引值就是哈希表的 mask （掩码）.
 *
 * 举个例子,如果当前哈希表的大小为 16 ,
 * 那么它的掩码就是二进制值 1111 ,
 * 这个哈希表的所有位置都可以使用哈希值的最后四个二进制位来记录.
 *
 * WHAT HAPPENS IF THE TABLE CHANGES IN SIZE?
 * 如果哈希表的大小改变了怎么办？
 *
 * 当对哈希表进行扩展时,元素可能会从一个槽移动到另一个槽,
 * 举个例子,假设我们刚好迭代至 4 位游标 1100 ,
 * 而哈希表的 mask 为 1111 （哈希表的大小为 16 ）.
 *
 * 如果这时哈希表将大小改为 64 ,那么哈希表的 mask 将变为 111111 ,
 * 等等...在 rehash 的时候可是会出现两个哈希表的阿！

 * 限制
 *
 * 这个迭代器是完全无状态的,这是一个巨大的优势,
 * 因为迭代可以在不使用任何额外内存的情况下进行.
 *
 * 这个设计的缺陷在于:
 *
 * 1) 函数可能会返回重复的元素,不过这个问题可以很容易在应用层解决.
 * 2) 为了不错过任何元素,迭代器需要返回给定桶上的所有键,以及因为扩展哈希表而产生出来的新表,所以迭代器必须在一次迭代中返回多个元素.
 * 3) 对游标进行翻转（reverse）的原因初看上去比较难以理解,不过阅读这份注释应该会有所帮助.
 */
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn, dictScanBucketFunction *bucketfn, void *privdata) {
    int htidx0, htidx1;
    const dictEntry *de, *next;
    unsigned long m0, m1;
    // 跳过空字典
    if (dictSize(d) == 0)
        return 0;

    /* This is needed in case the scan callback tries to do dictFind or alike. */
    dictPauseRehashing(d);

    if (!dictIsRehashing(d)) {
        htidx0 = 0;
        m0 = DICTHT_SIZE_MASK(d->ht_size_exp[htidx0]);

        /* Emit entries at cursor */
        if (bucketfn)
            bucketfn(d, &d->ht_table[htidx0][v & m0]);
        de = d->ht_table[htidx0][v & m0];
        while (de) {
            next = de->next;
            fn(privdata, de);
            de = next;
        }

        /* Set unmasked bits so incrementing the reversed cursor
         * operates on the masked bits */
        v |= ~m0;

        /* Increment the reverse cursor */
        v = rev(v);
        v++;
        v = rev(v);
    }
    else {
        // 指向两个哈希表
        htidx0 = 0;
        htidx1 = 1;
        // 确保 t0 比 t1 要小

        /* Make sure t0 is the smaller and t1 is the bigger table */
        if (DICTHT_SIZE(d->ht_size_exp[htidx0]) > DICTHT_SIZE(d->ht_size_exp[htidx1])) {
            htidx0 = 1;
            htidx1 = 0;
        }
        // 记录掩码

        m0 = DICTHT_SIZE_MASK(d->ht_size_exp[htidx0]);
        m1 = DICTHT_SIZE_MASK(d->ht_size_exp[htidx1]);

        /* Emit entries at cursor */
        if (bucketfn)
            bucketfn(d, &d->ht_table[htidx0][v & m0]);
        // 指向桶,并迭代桶中的所有节点
        de = d->ht_table[htidx0][v & m0];
        while (de) {
            next = de->next;
            fn(privdata, de);
            de = next;
        }

        /* Iterate over indices in larger table that are the expansion
         * of the index pointed to by the cursor in the smaller table */
        do {
            /* Emit entries at cursor */
            if (bucketfn)
                bucketfn(d, &d->ht_table[htidx1][v & m1]);
            // 指向桶,并迭代桶中的所有节点
            de = d->ht_table[htidx1][v & m1];
            while (de) {
                next = de->next;
                fn(privdata, de);
                de = next;
            }

            /* Increment the reverse cursor not covered by the smaller mask.*/
            v |= ~m1;
            v = rev(v);
            v++;
            v = rev(v);

            /* Continue while bits covered by mask difference is non-zero */
        } while (v & (m0 ^ m1));
    }

    dictResumeRehashing(d);

    return v;
}

/* ------------------------- 私有函数 ------------------------------ */

// 因为当dict 扩展时,我们可能需要立即分配巨大的内存块,我们将检查dict类型是否有expandAllowed成员函数.
static int dictTypeExpandAllowed(dict *d) {
    if (d->type->expandAllowed == NULL) {
        return 1;
    }
    // 扩容后的大小
    size_t a = DICTHT_SIZE(_dictNextExp(d->ht_used[0] + 1)) * sizeof(dictEntry *);
    // 当前的负载因子
    double b = (double)d->ht_used[0] / DICTHT_SIZE(d->ht_size_exp[0]);
    // 扩容后的大小, 当前的负载因子
    return d->type->expandAllowed(a, b); // dictExpandAllowed
}

// 根据需要,初始化字典（的哈希表）,或者对字典（的现有哈希表）进行扩展
static int _dictExpandIfNeeded(dict *d) {
    // 渐进式 rehash 已经在进行了,直接返回
    if (dictIsRehashing(d)) {
        return DICT_OK;
    }

    // 扩容判断时,只需要判断第一个元素

    // 如果哈希表为空,将其扩展为初始大小.
    if (DICTHT_SIZE(d->ht_size_exp[0]) == 0) {
        // 根据当前大小指数,获取数组大小,判断是不是0
        return dictExpand(d, DICT_HT_INITIAL_SIZE);
    }
    // 负载因子>=1
    size_t a = d->ht_used[0] / DICTHT_SIZE(d->ht_size_exp[0]);
    if (a >= 1) { // Hash表承载的元素个数超过其当前大小
        // 可以进行扩容 或  Hash表承载的元素个数已是当前大小的5倍
        // dict_can_resize 有子进程时, 禁止扩容
        if (dict_can_resize || d->ht_used[0] / DICTHT_SIZE(d->ht_size_exp[0]) > dict_force_resize_ratio) {
            if (dictTypeExpandAllowed(d)) {
                return dictExpand(d, d->ht_used[0] + 1);
            }
        }
    }
    return DICT_OK;
}

// 下一阶段扩容的指数
static signed char _dictNextExp(unsigned long size) {
    unsigned char e = DICT_HT_INITIAL_EXP; // 2
    if (size >= LONG_MAX) {
        return (8 * sizeof(long) - 1); // TODO ? 啥意思
    }
    while (1) {
        if (((unsigned long)1 << e) >= size) {
            return e;
        }
        e++;
    }
}

// 返回可以将 key 插入到哈希表的索引位置
// 如果 key 已经存在于哈希表,那么返回 -1
//
// 注意,如果字典正在进行 rehash ,那么总是返回 1 号哈希表的索引.
// 因为在字典进行 rehash 时,新节点总是插入到 1 号哈希表.
//
// T = O(N)

static long _dictKeyIndex(dict *d, const void *key, uint64_t hash, dictEntry **existing) {
    unsigned long idx, table;
    dictEntry *he;
    if (existing) {
        *existing = NULL;
    }

    // 单步 rehash
    if (_dictExpandIfNeeded(d) == DICT_ERR) { // 判断需不需要扩容,以及在扩容的过程中,有没有错误
        return -1;
    }
    for (table = 0; table <= 1; table++) {
        // 计算索引值
        idx = hash & DICTHT_SIZE_MASK(d->ht_size_exp[table]);
        // 查找 key 是否存在
        he = d->ht_table[table][idx]; // 链表
        while (he) {
            if (key == he->key || dictCompareKeys(d, key, he->key)) {
                if (existing) {
                    *existing = he;
                }
                return -1;
            }
            he = he->next;
        }
        // 如果运行到这里时,说明 0 号哈希表中所有节点都不包含 key
        // 如果这时 rehahs 正在进行,那么继续对 1 号哈希表进行 rehash
        if (!dictIsRehashing(d)) {
            break;
        }
    }
    return idx;
}

// 清空字典上的所有哈希表节点,并重置字典属性
void dictEmpty(dict *d, void(callback)(dict *)) {
    // 删除两个哈希表上的所有节点
    // T = O(N)
    _dictClear(d, 0, callback); // dictEmpty
    _dictClear(d, 1, callback); // dictEmpty
    // 重置属性
    d->rehashidx = -1;
    d->pauserehash = 0;
}

// 开启自动 rehash
void dictEnableResize(void) {
    dict_can_resize = 1;
}

// 关闭自动 rehash
void dictDisableResize(void) {
    dict_can_resize = 0;
}

uint64_t dictGetHash(dict *d, const void *key) {
    return dictHashKey(d, key);
}

/* Finds the dictEntry reference by using pointer and pre-calculated hash.
 * oldkey is a dead pointer and should not be accessed.
 * the hash value should be provided using dictGetHash.
 * no string / key comparison is performed.
 * return value is the reference to the dictEntry if found, or NULL if not found. */
dictEntry **dictFindEntryRefByPtrAndHash(dict *d, const void *oldptr, uint64_t hash) {
    dictEntry *he, **heref;
    unsigned long idx, table;

    if (dictSize(d) == 0)
        return NULL; /* dict is empty */
    for (table = 0; table <= 1; table++) {
        idx = hash & DICTHT_SIZE_MASK(d->ht_size_exp[table]);
        heref = &d->ht_table[table][idx];
        he = *heref;
        while (he) {
            if (oldptr == he->key)
                return heref;
            heref = &he->next;
            he = *heref;
        }
        if (!dictIsRehashing(d))
            return NULL;
    }
    return NULL;
}

/* ------------------------------- Debugging ---------------------------------*/

#define DICT_STATS_VECTLEN 50

size_t _dictGetStatsHt(char *buf, size_t bufsize, dict *d, int htidx) {
    unsigned long i, slots = 0, chainlen, maxchainlen = 0;
    unsigned long totchainlen = 0;
    unsigned long clvector[DICT_STATS_VECTLEN];
    size_t l = 0;

    if (d->ht_used[htidx] == 0) {
        return snprintf(buf, bufsize, "No stats available for empty dictionaries\n");
    }

    /* Compute stats. */
    for (i = 0; i < DICT_STATS_VECTLEN; i++) clvector[i] = 0;
    for (i = 0; i < DICTHT_SIZE(d->ht_size_exp[htidx]); i++) {
        dictEntry *he;

        if (d->ht_table[htidx][i] == NULL) {
            clvector[0]++;
            continue;
        }
        slots++;
        /* For each hash entry on this slot... */
        chainlen = 0;
        he = d->ht_table[htidx][i];
        while (he) {
            chainlen++;
            he = he->next;
        }
        clvector[(chainlen < DICT_STATS_VECTLEN) ? chainlen : (DICT_STATS_VECTLEN - 1)]++;
        if (chainlen > maxchainlen)
            maxchainlen = chainlen;
        totchainlen += chainlen;
    }

    /* Generate human readable stats. */
    l += snprintf(
        buf + l, bufsize - l,
        "Hash table %d stats (%s):\n"
        " table size: %lu\n"
        " number of elements: %lu\n"
        " different slots: %lu\n"
        " max chain length: %lu\n"
        " avg chain length (counted): %.02f\n"
        " avg chain length (computed): %.02f\n"
        " Chain length distribution:\n",
        htidx, (htidx == 0) ? "main hash table" : "rehashing target", DICTHT_SIZE(d->ht_size_exp[htidx]), d->ht_used[htidx], slots, maxchainlen, (float)totchainlen / slots, (float)d->ht_used[htidx] / slots);

    for (i = 0; i < DICT_STATS_VECTLEN - 1; i++) {
        if (clvector[i] == 0)
            continue;
        if (l >= bufsize)
            break;
        l += snprintf(buf + l, bufsize - l, "   %ld: %ld (%.02f%%)\n", i, clvector[i], ((float)clvector[i] / DICTHT_SIZE(d->ht_size_exp[htidx])) * 100);
    }

    /* Unlike snprintf(), return the number of characters actually written. */
    if (bufsize)
        buf[bufsize - 1] = '\0';
    return strlen(buf);
}

void dictGetStats(char *buf, size_t bufsize, dict *d) {
    size_t l;
    char *orig_buf = buf;
    size_t orig_bufsize = bufsize;

    l = _dictGetStatsHt(buf, bufsize, d, 0);
    buf += l;
    bufsize -= l;
    if (dictIsRehashing(d) && bufsize > 0) {
        _dictGetStatsHt(buf, bufsize, d, 1);
    }
    /* Make sure there is a NULL term at the end. */
    if (orig_bufsize)
        orig_buf[orig_bufsize - 1] = '\0';
}

/* ------------------------------- Benchmark ---------------------------------*/

#ifdef REDIS_TEST
#    include "testhelp.h"

#    define UNUSED(V) ((void)V)

uint64_t hashCallback(const void *key) {
    return dictGenHashFunction((unsigned char *)key, strlen((char *)key));
}

int compareCallback(dict *d, const void *key1, const void *key2) {
    int l1, l2;
    UNUSED(d);

    l1 = strlen((char *)key1);
    l2 = strlen((char *)key2);
    if (l1 != l2)
        return 0;
    return memcmp(key1, key2, l1) == 0;
}

void freeCallback(dict *d, void *val) {
    UNUSED(d);

    zfree(val);
}

char *stringFromLongLong(long long value) {
    char buf[32];
    int len;
    char *s;

    len = sprintf(buf, "%lld", value);
    s = zmalloc(len + 1);
    memcpy(s, buf, len);
    s[len] = '\0';
    return s;
}

dictType BenchmarkDictType = {hashCallback, NULL, NULL, compareCallback, freeCallback, NULL, NULL};

#    define start_benchmark() start = timeInMilliseconds()
#    define end_benchmark(msg)                                      \
        do {                                                        \
            elapsed = timeInMilliseconds() - start;                 \
            printf(msg ": %ld items in %lld ms\n", count, elapsed); \
        } while (0)

/* ./redis-server test dict [<count> | --accurate] */
int dictTest(int argc, char **argv, int flags) {
    long j;
    long long start, elapsed;
    dict *dict = dictCreate(&BenchmarkDictType);
    long count = 0;
    int accurate = (flags & REDIS_TEST_ACCURATE);

    if (argc == 4) {
        if (accurate) {
            count = 5000000;
        }
        else {
            count = strtol(argv[3], NULL, 10);
        }
    }
    else {
        count = 5000;
    }

    start_benchmark();
    for (j = 0; j < count; j++) {
        int retval = dictAdd(dict, stringFromLongLong(j), (void *)j);
        assert(retval == DICT_OK);
    }
    end_benchmark("Inserting");
    assert((long)dictSize(dict) == count);

    /* Wait for rehashing. */
    while (dictIsRehashing(dict)) {
        dictRehashMilliseconds(dict, 100);
    }

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(j);
        dictEntry *de = dictFind(dict, key);
        assert(de != NULL);
        zfree(key);
    }
    end_benchmark("Linear access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(j);
        dictEntry *de = dictFind(dict, key);
        assert(de != NULL);
        zfree(key);
    }
    end_benchmark("Linear access of existing elements (2nd round)");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(rand() % count);
        dictEntry *de = dictFind(dict, key);
        assert(de != NULL);
        zfree(key);
    }
    end_benchmark("Random access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++) {
        dictEntry *de = dictGetRandomKey(dict);
        assert(de != NULL);
    }
    end_benchmark("Accessing random keys");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(rand() % count);
        key[0] = 'X';
        dictEntry *de = dictFind(dict, key);
        assert(de == NULL);
        zfree(key);
    }
    end_benchmark("Accessing missing");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(j);
        int retval = dictDelete(dict, key);
        assert(retval == DICT_OK);
        key[0] += 17; /* Change first number to letter. */
        retval = dictAdd(dict, key, (void *)j);
        assert(retval == DICT_OK);
    }
    end_benchmark("Removing and adding");
    dictRelease(dict);
    return 0;
}
#endif
