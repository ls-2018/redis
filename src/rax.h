/* Rax -- A radix tree implementation.
 *
 * Copyright (c) 2017-2018, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef RAX_H
#define RAX_H

#include <stdint.h>

/* Representation of a radix tree as implemented in this file, that contains
 * the strings "foo", "foobar" and "footer" after the insertion of each
 * word. When the node represents a key inside the radix tree, we write it
 * between [], otherwise it is written between ().
 *
 * This is the vanilla representation:
 *
 *              (f) ""
 *                \
 *                (o) "f"
 *                  \
 *                  (o) "fo"
 *                    \
 *                  [t   b] "foo"
 *                  /     \
 *         "foot" (e)     (a) "foob"
 *                /         \
 *      "foote" (r)         (r) "fooba"
 *              /             \
 *    "footer" []             [] "foobar"
 *
 * However, this implementation implements a very common optimization where
 * successive nodes having a single child are "compressed" into the node
 * itself as a string of characters, each representing a next-level child,
 * and only the link to the node representing the last character node is
 * provided inside the representation. So the above representation is turned
 * into:
 *
 *                  ["foo"] ""
 *                     |
 *                  [t   b] "foo"
 *                  /     \
 *        "foot" ("er")    ("ar") "foob"
 *                 /          \
 *       "footer" []          [] "foobar"
 *
 * However this optimization makes the implementation a bit more complex.
 * For instance if a key "first" is added in the above radix tree, a
 * "node splitting" operation is needed, since the "foo" prefix is no longer
 * composed of nodes having a single child one after the other. This is the
 * above tree and the resulting node splitting after this event happens:
 *
 *
 *                    (f) ""
 *                    /
 *                 (i o) "f"
 *                 /   \
 *    "firs"  ("rst")  (o) "fo"
 *              /        \
 *    "first" []       [t   b] "foo"
 *                     /     \
 *           "foot" ("er")    ("ar") "foob"
 *                    /          \
 *          "footer" []          [] "foobar"
 *
 * Similarly after deletion, if a new chain of nodes having a single child
 * is created (the chain must also not include nodes that represent keys),
 * it must be compressed back into a single node.
 *
 */

#define RAX_NODE_MAX_SIZE ((1 << 29) - 1)
typedef struct raxNode {
    // 表示从 Radix Tree 的根节点到当前节点路径上的字符组成的字符串,是否表示了一个完整的 key.
    // 如果是的话,那么 iskey 的值为 1.否则,iskey 的值为 0.
    // 不过,这里需要注意的是,当前节点所表示的 key,并不包含该节点自身的内容.
    uint32_t iskey : 1;

    // 表示当前节点是否为空节点.如果当前节点是空节点,那么该节点就不需要为指向 value 的指针分配内存空间了.
    uint32_t isnull : 1;  // 节点的值是否为NULL
    uint32_t iscompr : 1; // 节点是否是压缩节点

    // 表示当前节点的大小,具体值会根据节点是压缩节点还是非压缩节点而不同.
    // 如果当前节点是压缩节点,该值表示压缩数据的长度;如果是非压缩节点,该值表示该节点指向的子节点个数.
    uint32_t size : 29; // 节点大小

    // 对于非压缩节点来说,data 数组包括子节点对应的字符、指向子节点的指针,以及节点表示 key 时对应的 value 指针;
    // 对于压缩节点来说,data 数组包括子节点对应的合并字符串、指向子节点的指针,以及节点为 key 时的 value 指针.
    unsigned char data[];
} raxNode;

// rax 前缀压缩树
typedef struct rax {
    raxNode *head;     // Radix Tree的头指针
    uint64_t numele;   // Radix Tree中key的个数
    uint64_t numnodes; // Radix Tree中raxNode的个数
} rax;

// raxLowWalk()使用的堆栈数据结构,以便有选择地向调用者返回父节点列表.节点没有关于空间的“父”字段,因此我们在需要时使用辅助堆栈.
#define RAX_STACK_STATIC_ITEMS 32

typedef struct raxStack {
    void **stack;           /* Points to static_items or an heap allocated array. */
    size_t items, maxitems; /* 包含的项目数量和总空间.*/
    /* Up to RAXSTACK_STACK_ITEMS items we avoid to allocate on the heap
     * and use this static array of pointers instead. */
    void *static_items[RAX_STACK_STATIC_ITEMS];
    int oom; /* True if pushing into this stack failed for OOM at some point. */
} raxStack;

/* Optional callback used for iterators and be notified on each rax node,
 * including nodes not representing keys. If the callback returns true
 * the callback changed the node pointer in the iterator structure, and the
 * iterator implementation will have to replace the pointer in the radix tree
 * internals. This allows the callback to reallocate the node to perform
 * very special operations, normally not needed by normal applications.
 *
 * This callback is used to perform very low level analysis of the radix tree
 * structure, scanning each possible node (but the root node), or in order to
 * reallocate the nodes to reduce the allocation fragmentation (this is the
 * Redis application for this callback).
 *
 * This is currently only supported in forward iterations (raxNext) */
typedef int (*raxNodeCallback)(raxNode **noderef);

#define RAX_ITER_STATIC_LEN 128       // 基数树迭代器状态被封装到这个数据结构中.
#define RAX_ITER_JUST_SEEKED (1 << 0) // 只是seek迭代器.返回第一次迭代的当前元素并清除标记
#define RAX_ITER_EOF (1 << 1)         // 迭代结束.
#define RAX_ITER_SAFE (1 << 2)        // 安全的迭代器,允许在迭代时进行操作.但速度较慢.

typedef struct raxIterator {
    int flags;
    rax *rt;                                              // 我们正在迭代的基数树.
    unsigned char *key;                                   // 当前字符串
    void *data;                                           // 与此key关联的数据
    size_t key_len;                                       // 当前key的长度
    size_t key_max;                                       // 当前迭代过的 最大key长度
    unsigned char key_static_string[RAX_ITER_STATIC_LEN]; //
    raxNode *node;                                        // 当前节点.只适用于不安全的迭代.
    raxStack stack;                                       // 用于不安全迭代的堆栈.
    raxNodeCallback node_cb;                              // 可选  节点回调函数.通常设置为NULL.
} raxIterator;

/* A special pointer returned for not found items. */
extern void *raxNotFound;

/* Exported API. */
rax *raxNew(void);

int raxInsert(rax *rax, unsigned char *s, size_t len, void *data, void **old);

int raxTryInsert(rax *rax, unsigned char *s, size_t len, void *data, void **old);

int raxRemove(rax *rax, unsigned char *s, size_t len, void **old);

void *raxFind(rax *rax, unsigned char *s, size_t len);

void raxFree(rax *rax);

void raxFreeWithCallback(rax *rax, void (*free_callback)(void *));

void raxStart(raxIterator *it, rax *rt);

int raxSeek(raxIterator *it, const char *op, unsigned char *ele, size_t len);

int raxNext(raxIterator *it);

int raxPrev(raxIterator *it);

int raxRandomWalk(raxIterator *it, size_t steps);

int raxCompare(raxIterator *iter, const char *op, unsigned char *key, size_t key_len);

void raxStop(raxIterator *it);

int raxEOF(raxIterator *it);

void raxShow(rax *rax);

uint64_t raxSize(rax *rax);

unsigned long raxTouch(raxNode *n);

void raxSetDebugMsg(int onoff);

/* Internal API. May be used by the node callback in order to access rax nodes
 * in a low level way, so this function is exported as well. */
void raxSetData(raxNode *n, void *data);

#endif
