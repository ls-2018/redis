

#include <stdlib.h>
#include "adlist.h"
#include "zmalloc.h"

// 创建一个新的列表.
// 创建的列表可以通过 listRelease() 释放,但是每个节点的私有值需要由用户在调用 listRelease() 之前释放,
// 或者通过使用 listSetFreeMethod 设置释放方法.如果出错,将返回NULL.否则将返回新列表的指针.
list *listCreate(void) {
    struct list *list;

    if ((list = zmalloc(sizeof(*list))) == NULL) {
        return NULL;
    }
    list->head = list->tail = NULL;
    list->len = 0;
    list->dup = NULL;
    list->free = NULL;
    list->match = NULL;
    return list;
}

// 从列表中删除所有元素,但不破坏列表本身.
void listEmpty(list *list) {
    unsigned long len;
    listNode     *current, *next;

    current = list->head;
    len = list->len;
    while (len--) {
        next = current->next;
        if (list->free) {
            list->free(current->value);
        }
        zfree(current);
        current = next;
    }
    list->head = list->tail = NULL;
    list->len = 0;
}

// 释放整个列表.这个函数不会失败.
void listRelease(list *list) {
    listEmpty(list);
    zfree(list);
}

/* Add a new node to the list, to head, containing the specified 'value'
 * pointer as value.
 *
 * On error, NULL is returned and no operation is performed (i.e. the
 * list remains unaltered).
 * On success the 'list' pointer you pass to the function is returned. */
list *listAddNodeHead(list *list, void *value) {
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
    node->value = value;
    if (list->len == 0) {
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    }
    else {
        node->prev = NULL;
        node->next = list->head;
        list->head->prev = node;
        list->head = node;
    }
    list->len++;
    return list;
}

/* Add a new node to the list, to tail, containing the specified 'value'
 * pointer as value.
 *
 * On error, NULL is returned and no operation is performed (i.e. the
 * list remains unaltered).
 * On success the 'list' pointer you pass to the function is returned. */
// 添加到链表尾
list *listAddNodeTail(list *list, void *value) {
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
    node->value = value;
    if (list->len == 0) {
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    }
    else {
        node->prev = list->tail;
        node->next = NULL;
        list->tail->next = node;
        list->tail = node;
    }
    list->len++;
    return list;
}

// 将一个新节点插入到指定节点的前或后
list *listInsertNode(list *list, listNode *old_node, void *value, int after) {
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
    node->value = value;
    if (after) {
        node->prev = old_node;
        node->next = old_node->next;
        if (list->tail == old_node) {
            list->tail = node;
        }
    }
    else {
        node->next = old_node;
        node->prev = old_node->prev;
        if (list->head == old_node) {
            list->head = node;
        }
    }
    if (node->prev != NULL) {
        node->prev->next = node;
    }
    if (node->next != NULL) {
        node->next->prev = node;
    }
    list->len++;
    return list;
}

// 从链表中删除指定节点
void listDelNode(list *list, listNode *node) {
    if (node->prev)
        node->prev->next = node->next;
    else
        list->head = node->next;
    if (node->next)
        node->next->prev = node->prev;
    else
        list->tail = node->prev;
    if (list->free)
        list->free(node->value);
    zfree(node);
    list->len--;
}

// 从指定位置,生成迭代器
listIter *listGetIterator(list *list, int direction) {
    listIter *iter;

    if ((iter = zmalloc(sizeof(*iter))) == NULL)
        return NULL;
    if (direction == AL_START_HEAD)
        iter->next = list->head;
    else
        iter->next = list->tail;
    iter->direction = direction;
    return iter;
}

/* Release the iterator memory */
void listReleaseIterator(listIter *iter) {
    zfree(iter);
}

// 在列表私有迭代器结构中创建一个迭代器
void listRewind(list *list, listIter *li) {
    li->next = list->head;
    li->direction = AL_START_HEAD;
}

void listRewindTail(list *list, listIter *li) {
    li->next = list->tail;
    li->direction = AL_START_TAIL;
}

// 返回迭代器的下一个元素
listNode *listNext(listIter *iter) {
    listNode *current = iter->next;

    if (current != NULL) {
        if (iter->direction == AL_START_HEAD)
            iter->next = current->next;
        else
            iter->next = current->prev;
    }
    return current;
}

// 复制一个给定链表的副本
list *listDup(list *orig) {
    list     *copy;
    listIter  iter;
    listNode *node;

    if ((copy = listCreate()) == NULL)
        return NULL;
    copy->dup = orig->dup;
    copy->free = orig->free;
    copy->match = orig->match;
    listRewind(orig, &iter);
    while ((node = listNext(&iter)) != NULL) {
        void *value;

        if (copy->dup) {
            value = copy->dup(node->value);
            if (value == NULL) {
                listRelease(copy);
                return NULL;
            }
        }
        else {
            value = node->value;
        }

        if (listAddNodeTail(copy, value) == NULL) {
            /* Free value if dup succeed but listAddNodeTail failed. */
            if (copy->free)
                copy->free(value);

            listRelease(copy);
            return NULL;
        }
    }
    return copy;
}

// 从链表中查找并返回指定值的节点
listNode *listSearchKey(list *list, void *key) {
    listIter  iter;
    listNode *node;

    listRewind(list, &iter);
    while ((node = listNext(&iter)) != NULL) {
        if (list->match) {
            if (list->match(node->value, key)) {
                return node;
            }
        }
        else {
            if (key == node->value) {
                return node;
            }
        }
    }
    return NULL;
}

// 返回链表在给定索引上的节点
listNode *listIndex(list *list, long index) {
    listNode *n;

    if (index < 0) {
        index = (-index) - 1;
        n = list->tail;
        while (index-- && n) n = n->prev;
    }
    else {
        n = list->head;
        while (index-- && n) n = n->next;
    }
    return n;
}

// 旋转列表,移除尾部节点并将其插入到头部.
void listRotateTailToHead(list *list) {
    if (listLength(list) <= 1)
        return;

    /* Detach current tail */
    listNode *tail = list->tail;
    list->tail = tail->prev;
    list->tail->next = NULL;
    /* Move it as head */
    list->head->prev = tail;
    tail->prev = NULL;
    tail->next = list->head;
    list->head = tail;
}

// 旋转列表,移除头部节点并将其插入到尾部.
void listRotateHeadToTail(list *list) {
    if (listLength(list) <= 1)
        return;

    listNode *head = list->head;
    /* Detach current head */
    list->head = head->next;
    list->head->prev = NULL;
    /* Move it as tail */
    list->tail->next = head;
    head->next = NULL;
    head->prev = list->tail;
    list->tail = head;
}

// 将o合并到l
void listJoin(list *l, list *o) {
    if (o->len == 0)
        return;

    o->head->prev = l->tail;

    if (l->tail)
        l->tail->next = o->head;
    else
        l->head = o->head;

    l->tail = o->tail;
    l->len += o->len;

    /* Setup other as an empty list. */
    o->head = o->tail = NULL;
    o->len = 0;
}
