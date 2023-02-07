#include "over-server.h"
#include "over-bio.h"
#include "atomicvar.h"
#include "functions.h"

static redisAtomic size_t lazyfree_objects = 0;
static redisAtomic size_t lazyfreed_objects = 0;

/* Release objects from the lazyfree thread. It's just decrRefCount()
 * updating the count of objects to release. */
void lazyfreeFreeObject(void *args[]) {
    robj *o = (robj *)args[0];
    decrRefCount(o);
    atomicDecr(lazyfree_objects, 1);
    atomicIncr(lazyfreed_objects, 1);
}

/* Release a database from the lazyfree thread. The 'db' pointer is the
 * database which was substituted with a fresh one in the main thread
 * when the database was logically deleted. */
void lazyfreeFreeDatabase(void *args[]) {
    dict *ht1 = (dict *)args[0];
    dict *ht2 = (dict *)args[1];

    size_t numkeys = dictSize(ht1);
    dictRelease(ht1);
    dictRelease(ht2);
    atomicDecr(lazyfree_objects, numkeys);
    atomicIncr(lazyfreed_objects, numkeys);
}

/* Release the key tracking table. */
void lazyFreeTrackingTable(void *args[]) {
    rax *rt = args[0];
    size_t len = rt->numele;
    freeTrackingRadixTree(rt);
    atomicDecr(lazyfree_objects, len);
    atomicIncr(lazyfreed_objects, len);
}

/* Release the lua_scripts dict. */
void lazyFreeLuaScripts(void *args[]) {
    dict *lua_scripts = args[0];
    long long len = dictSize(lua_scripts);
    dictRelease(lua_scripts);
    atomicDecr(lazyfree_objects, len);
    atomicIncr(lazyfreed_objects, len);
}

/* Release the functions ctx. */
void lazyFreeFunctionsCtx(void *args[]) {
    functionsLibCtx *functions_lib_ctx = args[0];
    size_t len = functionsLibCtxfunctionsLen(functions_lib_ctx);
    functionsLibCtxFree(functions_lib_ctx);
    atomicDecr(lazyfree_objects, len);
    atomicIncr(lazyfreed_objects, len);
}

/* Release replication backlog referencing memory. */
void lazyFreeReplicationBacklogRefMem(void *args[]) {
    list *blocks = args[0];
    rax *index = args[1];
    long long len = listLength(blocks);
    len += raxSize(index);
    listRelease(blocks);
    raxFree(index);
    atomicDecr(lazyfree_objects, len);
    atomicIncr(lazyfreed_objects, len);
}

/* Return the number of currently pending objects to free. */
size_t lazyfreeGetPendingObjectsCount(void) {
    size_t aux;
    atomicGet(lazyfree_objects, aux);
    return aux;
}

/* Return the number of objects that have been freed. */
size_t lazyfreeGetFreedObjectsCount(void) {
    size_t aux;
    atomicGet(lazyfreed_objects, aux);
    return aux;
}

void lazyfreeResetStats() {
    atomicSet(lazyfreed_objects, 0);
}

/* Return the amount of work needed in order to free an object.
 * The return value is not always the actual number of allocations the
 * object is composed of, but a number proportional to it.
 *
 * For strings the function always returns 1.
 *
 * For aggregated objects represented by hash tables or other data structures
 * the function just returns the number of elements the object is composed of.
 *
 * Objects composed of single allocations are always reported as having a
 * single item even if they are actually logical composed of multiple
 * elements.
 *
 * For lists the function returns the number of elements in the quicklist
 * representing the list. */
// 什么情况才会真正异步释放内存？这和key的类型、编码方式、元素数量都有关系
// 开启 lazy-free 后,Redis 在释放一个 key 内存时,首先会评估「代价」,如果代价很小,那么就直接在「主线程」操作了,「没必要」放到后台线程中执行（不同线程传递数据也会有性能消耗）
// 7、什么情况才会真正异步释放内存？这和 key 的类型、编码方式、元素数量都有关系（详见 lazyfreeGetFreeEffort 函数）：
//
// a) 当 Hash/Set 底层采用哈希表存储（非 ziplist/int 编码存储）时,并且元素数量超过 64 个
// b) 当 ZSet 底层采用跳表存储（非 ziplist 编码存储）时,并且元素数量超过 64 个
// c) 当 List 链表节点数量超过 64 个（注意,不是元素数量,而是链表节点的数量,List 底层实现是一个链表,链表每个节点是一个 ziplist,一个 ziplist 可能有多个元素数据）
//
// 只有满足以上条件,在释放 key 内存时,才会真正放到「后台线程」中执行,其它情况一律还是在主线程操作.
//
// 也就是说 String（不管内存占用多大）、List（少量元素）、Set（int 编码存储）、Hash/ZSet（ziplist 编码存储）这些情况下的 key,在释放内存时,依旧在「主线程」中操作.
//
// 8、可见,即使打开了 lazy-free,String 类型的 bigkey,在删除时依旧有「阻塞」主线程的风险.所以,即便 Redis 提供了 lazy-free,还是不建议在 Redis 存储 bigkey
//
// 9、Redis 在释放内存「评估」代价时,不是看 key 的内存大小,而是关注释放内存时的「工作量」有多大.从上面分析可以看出,如果 key 内存是连续的,释放内存的代价就比较低,则依旧放在「主线程」处理.如果 key 内存不连续（包含大量指针）,这个代价就比较高,这才会放在「后台线程」中执行
//
// 计算 List 和 Set 类型键值对的删除开销
size_t lazyfreeGetFreeEffort(robj *key, robj *obj, int dbid) {
    if (obj->type == OBJ_LIST) { // 如果是List类型键值对,就返回List的长度,也就其中元素个数
        quicklist *ql = obj->ptr;
        return ql->len;
    }
    else if (obj->type == OBJ_SET && obj->encoding == OBJ_ENCODING_HT) {
        dict *ht = obj->ptr;
        return dictSize(ht); // 如果是Set类型键值对,就返回Set中的元素个数
    }
    else if (obj->type == OBJ_ZSET && obj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = obj->ptr;
        return zs->zsl->length;
    }
    else if (obj->type == OBJ_HASH && obj->encoding == OBJ_ENCODING_HT) {
        dict *ht = obj->ptr;
        return dictSize(ht);
    }
    else if (obj->type == OBJ_STREAM) {
        size_t effort = 0;
        stream *s = obj->ptr;

        /* Make a best effort estimate to maintain constant runtime. Every macro
         * node in the Stream is one allocation. */
        effort += s->rax->numnodes;

        /* Every consumer group is an allocation and so are the entries in its
         * PEL. We use size of the first group's PEL as an estimate for all
         * others. */
        if (s->cgroups && raxSize(s->cgroups)) {
            raxIterator ri;
            streamCG *cg;
            raxStart(&ri, s->cgroups);
            raxSeek(&ri, "^", NULL, 0);
            /* There must be at least one group so the following should always
             * work. */
            serverAssert(raxNext(&ri));
            cg = ri.data;
            effort += raxSize(s->cgroups) * (1 + raxSize(cg->pel));
            raxStop(&ri);
        }
        return effort;
    }
    else if (obj->type == OBJ_MODULE) {
        size_t effort = moduleGetFreeEffort(key, obj, dbid);
        /* If the module's free_effort returns 0, we will use asynchronous free
         * memory by default. */
        return effort == 0 ? ULONG_MAX : effort;
    }
    else {
        return 1; /* Everything else is a single allocation. */
    }
}

/* If there are enough allocations to free the value object asynchronously, it
 * may be put into a lazy free list instead of being freed synchronously. The
 * lazy free list will be reclaimed in a different bio.c thread. If the value is
 * composed of a few allocations, to free in a lazy way is actually just
 * slower... So under a certain limit we just free the object synchronously. */
#define LAZYFREE_THRESHOLD 64

/* 释放一个对象，如果对象足够大，以异步方式释放它。 */
void freeObjAsync(robj *key, robj *obj, int dbid) {
    size_t free_effort = lazyfreeGetFreeEffort(key, obj, dbid);// 判断释放的开销
    /* Note that if the object is shared, to reclaim it now it is not
     * possible. This rarely happens, however sometimes the implementation
     * of parts of the Redis core may call incrRefCount() to protect
     * objects, and then call dbDelete(). */
    // 如果要淘汰的键值对包含超过64个元素

    // 其实异步方法与同步方法的差别在这，即要求 删除的元素影响须大于某阀值(64)
    // 否则按照同步方式直接删除，因为那样代价更小

    if (free_effort > LAZYFREE_THRESHOLD && obj->refcount == 1) {
        atomicIncr(lazyfree_objects, 1);// 异步释放+1，原子操作
        // 创建惰性删除的后台任务,交给后台线程执行
        // 将 value 的释放添加到异步线程队列中去，后台处理, 任务类型为 异步释放内存
        bioCreateLazyFreeJob(lazyfreeFreeObject, 1, obj);
    }
    else {
        decrRefCount(obj);
    }
}

/* Empty a Redis DB asynchronously. What the function does actually is to
 * create a new empty set of hash tables and scheduling the old ones for
 * lazy freeing. */
void emptyDbAsync(redisDb *db) {
    dict *oldht1 = db->dict, *oldht2 = db->expires;
    db->dict = dictCreate(&dbDictType);
    db->expires = dictCreate(&dbExpiresDictType);
    atomicIncr(lazyfree_objects, dictSize(oldht1));
    bioCreateLazyFreeJob(lazyfreeFreeDatabase, 2, oldht1, oldht2);
}

/* Free the key tracking table.
 * If the table is huge enough, free it in async way. */
void freeTrackingRadixTreeAsync(rax *tracking) {
    /* Because this rax has only keys and no values so we use numnodes. */
    if (tracking->numnodes > LAZYFREE_THRESHOLD) {
        atomicIncr(lazyfree_objects, tracking->numele);
        bioCreateLazyFreeJob(lazyFreeTrackingTable, 1, tracking);
    }
    else {
        freeTrackingRadixTree(tracking);
    }
}

/* Free lua_scripts dict, if the dict is huge enough, free it in async way. */
void freeLuaScriptsAsync(dict *lua_scripts) {
    if (dictSize(lua_scripts) > LAZYFREE_THRESHOLD) {
        atomicIncr(lazyfree_objects, dictSize(lua_scripts));
        bioCreateLazyFreeJob(lazyFreeLuaScripts, 1, lua_scripts);
    }
    else {
        dictRelease(lua_scripts);
    }
}

/* Free functions ctx, if the functions ctx contains enough functions, free it in async way. */
void freeFunctionsAsync(functionsLibCtx *functions_lib_ctx) {
    if (functionsLibCtxfunctionsLen(functions_lib_ctx) > LAZYFREE_THRESHOLD) {
        atomicIncr(lazyfree_objects, functionsLibCtxfunctionsLen(functions_lib_ctx));
        bioCreateLazyFreeJob(lazyFreeFunctionsCtx, 1, functions_lib_ctx);
    }
    else {
        functionsLibCtxFree(functions_lib_ctx);
    }
}

/* Free replication backlog referencing buffer blocks and rax index. */
void freeReplicationBacklogRefMemAsync(list *blocks, rax *index) {
    if (listLength(blocks) > LAZYFREE_THRESHOLD || raxSize(index) > LAZYFREE_THRESHOLD) {
        atomicIncr(lazyfree_objects, listLength(blocks) + raxSize(index));
        bioCreateLazyFreeJob(lazyFreeReplicationBacklogRefMem, 2, blocks, index);
    }
    else {
        listRelease(blocks);
        raxFree(index);
    }
}
