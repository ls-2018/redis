// listpack 也叫紧凑列表,它的特点就是用一块连续的内存空间来紧凑地保存数据,同时为了节省内存空间,
// listpack 列表项使用了多种编码方式,来表示不同长度的数据,这些数据包括整数和字符串.

#ifndef __LISTPACK_H
#define __LISTPACK_H

#include <stdlib.h>
#include <stdint.h>

#define LP_INTBUF_SIZE 21 /* 20 digits of -2^63 + 1 null term = 21. */

/* lpInsert() where argument possible values: */
#define LP_BEFORE 0
#define LP_AFTER 1
#define LP_REPLACE 2

/* Each entry in the listpack is either a string or an integer. */
typedef struct {
    /* When string is used, it is provided with the length (slen). */
    unsigned char *sval; // 实际存放的数据
    uint32_t       slen; // 定义该元素的编码类型，会对不同长度的整数和字符串进行编码；
    /* When integer is used, 'sval' is NULL, and lval holds the value. */
    long long lval; // encoding+data的总长度
} listpackEntry;

unsigned char *lpNew(size_t capacity);

void lpFree(unsigned char *lp);

unsigned char *lpShrinkToFit(unsigned char *lp);

unsigned char *lpInsertString(unsigned char *lp, unsigned char *s, uint32_t slen, unsigned char *p, int where, unsigned char **newp);

unsigned char *lpInsertInteger(unsigned char *lp, long long lval, unsigned char *p, int where, unsigned char **newp);

unsigned char *lpPrepend(unsigned char *lp, unsigned char *s, uint32_t slen);

unsigned char *lpPrependInteger(unsigned char *lp, long long lval);

unsigned char *lpAppend(unsigned char *lp, unsigned char *s, uint32_t slen);

unsigned char *lpAppendInteger(unsigned char *lp, long long lval);

unsigned char *lpReplace(unsigned char *lp, unsigned char **p, unsigned char *s, uint32_t slen);

unsigned char *lpReplaceInteger(unsigned char *lp, unsigned char **p, long long lval);

unsigned char *lpDelete(unsigned char *lp, unsigned char *p, unsigned char **newp);

unsigned char *lpDeleteRangeWithEntry(unsigned char *lp, unsigned char **p, unsigned long num);

unsigned char *lpDeleteRange(unsigned char *lp, long index, unsigned long num);

unsigned char *lpMerge(unsigned char **first, unsigned char **second);

unsigned long lpLength(unsigned char *lp);

unsigned char *lpGet(unsigned char *p, int64_t *count, unsigned char *intbuf);

unsigned char *lpGetValue(unsigned char *p, unsigned int *slen, long long *lval);

unsigned char *lpFind(unsigned char *lp, unsigned char *p, unsigned char *s, uint32_t slen, unsigned int skip);

unsigned char *lpFirst(unsigned char *lp);

unsigned char *lpLast(unsigned char *lp);

unsigned char *lpNext(unsigned char *lp, unsigned char *p);

unsigned char *lpPrev(unsigned char *lp, unsigned char *p);

size_t lpBytes(unsigned char *lp);

unsigned char *lpSeek(unsigned char *lp, long index);

typedef int (*listpackValidateEntryCB)(unsigned char *p, unsigned int head_count, void *userdata);

int lpValidateIntegrity(unsigned char *lp, size_t size, int deep, listpackValidateEntryCB entry_cb, void *cb_userdata);

unsigned char *lpValidateFirst(unsigned char *lp);

int lpValidateNext(unsigned char *lp, unsigned char **pp, size_t lpbytes);

unsigned int lpCompare(unsigned char *p, unsigned char *s, uint32_t slen);

void lpRandomPair(unsigned char *lp, unsigned long total_count, listpackEntry *key, listpackEntry *val);

void lpRandomPairs(unsigned char *lp, unsigned int count, listpackEntry *keys, listpackEntry *vals);

unsigned int lpRandomPairsUnique(unsigned char *lp, unsigned int count, listpackEntry *keys, listpackEntry *vals);

int lpSafeToAdd(unsigned char *lp, size_t add);

void lpRepr(unsigned char *lp);

#ifdef REDIS_TEST
int listpackTest(int argc, char *argv[], int flags);
#endif

#endif
