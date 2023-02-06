/*
1、什么时候触发rehash
    _dictExpandIfNeeded     函数会根据 Hash 表的负载因子以及能否进行 rehash 的标识,判断是否进行 rehash
    updateDictResizePolicy  函数会根据 RDB 和 AOF 的执行情况,启用或禁用 rehash.

2、rehash扩容,扩多大
    _dictNextExp  直接扩到下一个2的xx次方
3、rehash何时进行




 */
#ifndef __DICT_H
#define __DICT_H

#include "mt19937-64.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#define DICT_OK 0  // 操作成功
#define DICT_ERR 1 // 操作失败（或出错）

// 哈希表
typedef struct dictEntry {
    void *key; // string 对象    redisObject 对象
    union
    {
        void *val;    // redisObject对象   值为整数或双精度浮点数时,本身就是64位,就可以不用指针指向了,而是可以直接存在键值对的结构体中,这样就避免了再用一个指针,从而节省了内存空间.
        uint64_t u64; // 无符号64位
        int64_t s64;
        double d;
    } v;
    struct dictEntry *next; // 指向下个哈希表节点,形成链表
    void *metadata[];       /* An arbitrary number of bytes (starting at a
                             * pointer-aligned address) of size as returned
                             * by dictType's dictEntryMetadataBytes(). */
} dictEntry;
typedef struct dict dict;

// 字典类型特定函数
typedef struct dictType {
    uint64_t (*hashFunction)(const void *key);                      // 计算哈希值的函数
    void *(*keyDup)(dict *d, const void *key);                      // 复制键的函数
    void *(*valDup)(dict *d, const void *obj);                      // 复制值的函数
    int (*keyCompare)(dict *d, const void *key1, const void *key2); // 对比键的函数
    void (*keyDestructor)(dict *d, void *key);                      // 销毁键的函数
    void (*valDestructor)(dict *d, void *obj);                      // 销毁值的函数
    int (*expandAllowed)(size_t moreMem, double usedRatio);         // 是否允许扩容,两个参数:扩容后的大小, 当前的负载因子
    size_t (*dictEntryMetadataBytes)(dict *d);                      // 允许一个dictEntry携带额外的调用者定义的元数据. 当dictEntry被分配时,额外的内存被初始化为0.
} dictType;

// 字典
struct dict {
    dictType *type;           // 类型特定函数
    dictEntry **ht_table[2];  // 哈希表;一般情况下,字典只使用ht[0]哈希表,ht[1]哈希表只会在对ht[0]哈希表进行rehash时使用
    unsigned long ht_used[2]; // 该哈希表已有节点的数量
    long rehashidx;           // rehash 索引; 当 rehash 不在进行时,值为 -1 , >=0正在rehash
    /* Keep small vars at end for optimal (minimal) struct padding */
    int16_t pauserehash;        // >0 表示rehash被暂停了
    signed char ht_size_exp[2]; // 记录了大小指数,实际大小是1<<n
};

#define DICTHT_SIZE(exp) ((exp) == -1 ? 0 : (unsigned long)1 << (exp))
#define DICTHT_SIZE_MASK(exp) ((exp) == -1 ? 0 : (DICTHT_SIZE(exp)) - 1)

// 字典迭代器
//
// 如果 safe 属性的值为 1 ,那么在迭代进行的过程中,
// 程序仍然可以执行 dictAdd 、 dictFind 和其他函数,对字典进行修改.
//
// 如果 safe 不为 1 ,那么程序只会调用 dictNext 对字典进行迭代,
// 而不对字典进行修改.
typedef struct dictIterator {
    dict *d;                        // 被迭代的字典
    long index;                     // 迭代器当前所指向的哈希表索引位置.
    int table;                      // 正在被迭代的哈希表号码,值可以是 0 或 1 .
    int safe;                       // 标识这个迭代器是否安全 0不安全 1安全
    dictEntry *entry;               // 当前迭代到的节点的指针
    dictEntry *nextEntry;           // 当前迭代节点的下一个节点,  因为在安全迭代器运作时, entry 所指向的节点可能会被修改,所以需要一个额外的指针来保存下一节点的位置,从而防止指针丢失
    unsigned long long fingerprint; // 不安全迭代时的ht[0] ht[1]状态
} dictIterator;

typedef void(dictScanFunction)(void *privdata, const dictEntry *de);

typedef void(dictScanBucketFunction)(dict *d, dictEntry **bucketref);

// hash表 初始时的 大小指数
#define DICT_HT_INITIAL_EXP 2
// 哈希表的初始大小 4
#define DICT_HT_INITIAL_SIZE (1 << (DICT_HT_INITIAL_EXP))

/* ------------------------------- Macros ------------------------------------*/
// 释放给定字典节点的值
#define dictFreeVal(d, entry)     \
    if ((d)->type->valDestructor) \
    (d)->type->valDestructor((d), (entry)->v.val)
// 设置给定字典节点的值
#define dictSetVal(d, entry, _val_)                         \
    do {                                                    \
        if ((d)->type->valDup)                              \
            (entry)->v.val = (d)->type->valDup((d), _val_); \
        else                                                \
            (entry)->v.val = (_val_);                       \
    } while (0)

#define dictSetSignedIntegerVal(entry, _val_) \
    do {                                      \
        (entry)->v.s64 = _val_;               \
    } while (0)

#define dictSetUnsignedIntegerVal(entry, _val_) \
    do {                                        \
        (entry)->v.u64 = _val_;                 \
    } while (0)

#define dictSetDoubleVal(entry, _val_) \
    do {                               \
        (entry)->v.d = _val_;          \
    } while (0)

#define dictFreeKey(d, entry)     \
    if ((d)->type->keyDestructor) \
    (d)->type->keyDestructor((d), (entry)->key)

#define dictSetKey(d, entry, _key_)                       \
    do {                                                  \
        if ((d)->type->keyDup)                            \
            (entry)->key = (d)->type->keyDup((d), _key_); \
        else                                              \
            (entry)->key = (_key_);                       \
    } while (0)

#define dictCompareKeys(d, key1, key2) (((d)->type->keyCompare) ? (d)->type->keyCompare((d), key1, key2) : (key1) == (key2))

#define dictMetadata(entry) (&(entry)->metadata)
#define dictMetadataSize(d) ((d)->type->dictEntryMetadataBytes ? (d)->type->dictEntryMetadataBytes(d) : 0)

#define dictHashKey(d, key) (d)->type->hashFunction(key)
#define dictGetKey(he) ((he)->key)
#define dictGetVal(he) ((he)->v.val)
#define dictGetSignedIntegerVal(he) ((he)->v.s64)
#define dictGetUnsignedIntegerVal(he) ((he)->v.u64)
#define dictGetDoubleVal(he) ((he)->v.d)
#define dictSlots(d) (DICTHT_SIZE((d)->ht_size_exp[0]) + DICTHT_SIZE((d)->ht_size_exp[1]))
#define dictSize(d) ((d)->ht_used[0] + (d)->ht_used[1])
#define dictIsRehashing(d) ((d)->rehashidx != -1)
#define dictPauseRehashing(d) (d)->pauserehash++
#define dictResumeRehashing(d) (d)->pauserehash--

/* If our unsigned long type can store a 64 bit number, use a 64 bit PRNG. */
#if ULONG_MAX >= 0xffffffffffffffff
#    define randomULong() ((unsigned long)genrand64_int64())
#else
#    define randomULong() random()
#endif

/* API */
dict *dictCreate(dictType *type);

int dictExpand(dict *d, unsigned long size);

int dictTryExpand(dict *d, unsigned long size);

int dictAdd(dict *d, void *key, void *val);

dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing);

dictEntry *dictAddOrFind(dict *d, void *key);

int dictReplace(dict *d, void *key, void *val);

int dictDelete(dict *d, const void *key);

dictEntry *dictUnlink(dict *d, const void *key);

void dictFreeUnlinkedEntry(dict *d, dictEntry *he);

void dictRelease(dict *d);

dictEntry *dictFind(dict *d, const void *key);

void *dictFetchValue(dict *d, const void *key);

int dictResize(dict *d);

dictIterator *dictGetIterator(dict *d);

dictIterator *dictGetSafeIterator(dict *d);

dictEntry *dictNext(dictIterator *iter);

void dictReleaseIterator(dictIterator *iter);

dictEntry *dictGetRandomKey(dict *d);

dictEntry *dictGetFairRandomKey(dict *d);

unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count);

void dictGetStats(char *buf, size_t bufsize, dict *d);

uint64_t dictGenHashFunction(const void *key, size_t len);

uint64_t dictGenCaseHashFunction(const unsigned char *buf, size_t len);

void dictEmpty(dict *d, void(callback)(dict *));

void dictEnableResize(void);

void dictDisableResize(void);

int dictRehash(dict *d, int n);

int dictRehashMilliseconds(dict *d, int ms);

void dictSetHashFunctionSeed(uint8_t *seed);

uint8_t *dictGetHashFunctionSeed(void);

unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn, dictScanBucketFunction *bucketfn, void *privdata);

uint64_t dictGetHash(dict *d, const void *key);

dictEntry **dictFindEntryRefByPtrAndHash(dict *d, const void *oldptr, uint64_t hash);

#ifdef REDIS_TEST
int dictTest(int argc, char *argv[], int flags);
#endif

#endif /* __DICT_H */
