#ifndef __ADLIST_H__
#define __ADLIST_H__

// 双端链表节点
typedef struct listNode {
    struct listNode *prev; // 前置节点
    struct listNode *next; // 后置节点
    void *value;           // 节点的值
} listNode;

// 迭代器
typedef struct listIter {
    listNode *next;
    int direction;
} listIter;

// 无环双向链表结构
typedef struct list {
    listNode *head;                     // 表头节点
    listNode *tail;                     // 表尾节点
    void *(*dup)(void *ptr);            // 节点值复制函数
    void (*free)(void *ptr);            // 节点值释放函数
    int (*match)(void *ptr, void *key); // 节点值对比函数
    unsigned long len;                  // 链表所包含的节点数量
} list;
// 缺点:
// 1、每个节点都不是连续的,意味着无法很好利用CPU缓存
// 2、每个节点都需要结构头,内存开销较大

#define listLength(l) ((l)->len)
#define listFirst(l) ((l)->head)
#define listLast(l) ((l)->tail) // 获取最末尾的元素指针
#define listPrevNode(n) ((n)->prev)
#define listNextNode(n) ((n)->next)
// 返回当前节点正在保存的值
#define listNodeValue(n) ((n)->value)
// 将给定的函数设置为链表的节点复制函数
#define listSetDupMethod(l, m) ((l)->dup = (m))
#define listSetFreeMethod(l, m) ((l)->free = (m))
#define listSetMatchMethod(l, m) ((l)->match = (m))

#define listGetDupMethod(l) ((l)->dup)
#define listGetFreeMethod(l) ((l)->free)
#define listGetMatchMethod(l) ((l)->match)

/* Prototypes */
// 创建一个新的列表
list *listCreate(void);

// 释放给定链表,以及包含的所有节点
void listRelease(list *list);

void listEmpty(list *list);

// 讲一个包含给定值的新节点添加到给定链表的表头
list *listAddNodeHead(list *list, void *value);

// 讲一个包含给定值的新节点添加到给定链表的表尾
list *listAddNodeTail(list *list, void *value);

// 将一个新节点插入到指定节点的前或后
list *listInsertNode(list *list, listNode *old_node, void *value, int after);

// 从链表中删除指定节点
void listDelNode(list *list, listNode *node);

// 从指定位置,生成迭代器
listIter *listGetIterator(list *list, int direction);

// 返回迭代器的下一个元素
listNode *listNext(listIter *iter);

void listReleaseIterator(listIter *iter);

list *listDup(list *orig);                      // 复制一个给定链表的副本
listNode *listSearchKey(list *list, void *key); // 从链表中查找并返回指定值的节点
listNode *listIndex(list *list, long index);    // 返回链表在给定索引上的节点
void listRewind(list *list, listIter *li);

void listRewindTail(list *list, listIter *li);

void listRotateTailToHead(list *list);

void listRotateHeadToTail(list *list);

void listJoin(list *l, list *o);

// 迭代器遍历的方向 ,正向、反向
#define AL_START_HEAD 0
#define AL_START_TAIL 1

#endif /* __ADLIST_H__ */
