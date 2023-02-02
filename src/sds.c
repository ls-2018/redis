
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <limits.h>
#include "sds.h"
#include "sdsalloc.h"

const char *SDS_NOINIT = "SDS_NOINIT";

// 返回sds结构体长度,不算数据
int sdsHdrSize(char type) {
    switch (type & SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            return sizeof(struct sdshdr5);
        case SDS_TYPE_8:
            return sizeof(struct sdshdr8);
        case SDS_TYPE_16:
            return sizeof(struct sdshdr16);
        case SDS_TYPE_32:
            return sizeof(struct sdshdr32);
        case SDS_TYPE_64:
            return sizeof(struct sdshdr64);
    }
    return 0;
}

// 根据需要的长度,返回可以满足的最小的sds
static inline char sdsReqType(size_t string_size) {
    if (string_size < 1 << 5)
        return SDS_TYPE_5;
    if (string_size < 1 << 8)
        return SDS_TYPE_8;
    if (string_size < 1 << 16)
        return SDS_TYPE_16;
#if (LONG_MAX == LLONG_MAX)
    if (string_size < 1ll << 32)
        return SDS_TYPE_32;
    return SDS_TYPE_64;
#else
    return SDS_TYPE_32;
#endif
}

static inline size_t sdsTypeMaxSize(char type) {
    if (type == SDS_TYPE_5)
        return (1 << 5) - 1;
    if (type == SDS_TYPE_8)
        return (1 << 8) - 1;
    if (type == SDS_TYPE_16)
        return (1 << 16) - 1;
#if (LONG_MAX == LLONG_MAX)
    if (type == SDS_TYPE_32)
        return (1ll << 32) - 1;
#endif
    return -1; /* this is equivalent to the max SDS_TYPE_64 or SDS_TYPE_32 */
}

// init 内容指针, initlen初始长度
sds _sdsnewlen(const void *init, size_t initlen, int trymalloc) {
    void *sh;                        // 指向SDS结构体的指针
    sds s;                           // sds类型变量,即char*字符数组
    char type = sdsReqType(initlen); // 根据字符串长度选择不同类型
    if (type == SDS_TYPE_5 && initlen == 0) {
        // SDS_TYPE_5 强制转换为SDS_TYPE_8
        type = SDS_TYPE_8;
    }
    int hdrlen = sdsHdrSize(type); // 计算不同类型所需要的头部长度
    unsigned char *fp;             // 指向flags 指针
    size_t usable;
    // +1 为了保存结束符"\0"
    assert(initlen + hdrlen + 1 > initlen); // 数据溢出

    // 新建SDS结构,并分配内存空间
    if (trymalloc) {
        sh = s_trymalloc_usable(hdrlen + initlen + 1, &usable);
    }
    else {
        sh = s_malloc_usable(hdrlen + initlen + 1, &usable);
    }
    if (sh == NULL) {
        return NULL;
    }
    if (init == SDS_NOINIT) {
        init = NULL;
    }
    else if (!init) {
        memset(sh, 0, hdrlen + initlen + 1); // 将分配的内存全置为0,防止有残留数据
    }

    s = (char *)sh + hdrlen;       // s指向buf指针
    fp = ((unsigned char *)s) - 1; // s是动态数组buf的指针,-1即指向flags
    usable = usable - hdrlen - 1;
    if (usable > sdsTypeMaxSize(type)) {
        usable = sdsTypeMaxSize(type);
    }
    // 根据不同类型初始化不同结构
    switch (type) {
        case SDS_TYPE_5: {
            *fp = type | (initlen << SDS_TYPE_BITS);
            break;
        }
        case SDS_TYPE_8: {
            SDS_HDR_VAR(8, s);
            sh->len = initlen;
            sh->alloc = usable;
            *fp = type;
            break;
        }
        case SDS_TYPE_16: {
            SDS_HDR_VAR(16, s);
            sh->len = initlen;
            sh->alloc = usable;
            *fp = type;
            break;
        }
        case SDS_TYPE_32: {
            SDS_HDR_VAR(32, s);
            sh->len = initlen;
            sh->alloc = usable;
            *fp = type;
            break;
        }
        case SDS_TYPE_64: {
            SDS_HDR_VAR(64, s);
            sh->len = initlen;
            sh->alloc = usable;
            *fp = type;
            break;
        }
    }
    if (initlen && init) {
        memcpy(s, init, initlen);
    }
    s[initlen] = '\0'; // 变量s末尾增加\0,表示字符串结束

    // 返回的s就是一个字符数组的指针,即结构中的buf.这样设计的好处在于直接对上层提供了字符串内容指针,兼容了部分C函数,且通过偏移能迅速定位到SDS结构体的各处成员变量.
    // 释放字符串,通过对s的偏移,可定位到SDS结构体的首部,然后调用s_free释放内存;
    return s;
}

/*
 * 根据给定的初始化字符串 init 和字符串长度 initlen 创建一个新的 sds
 * 参数
 *  init ：初始化字符串指针
 *  initlen ：初始化字符串的长度
 *
 * 返回值
 *  sds ：创建成功返回 sdshdr 相对应的 sds
 *        创建失败返回 NULL
   O(N)
 */
sds sdsnewlen(const void *init, size_t initlen) {
    return _sdsnewlen(init, initlen, 0);
}

// OK
sds sdstrynewlen(const void *init, size_t initlen) {
    return _sdsnewlen(init, initlen, 1);
}

/* Create an empty (zero length) sds string. Even in this case the string
 * always has an implicit null term. */
sds sdsempty(void) {
    return sdsnewlen("", 0);
}

// 从一个以空结束的C字符串 、创建一个新的sds字符串.
sds sdsnew(const char *init) {
    size_t initlen = (init == NULL) ? 0 : strlen(init);
    return sdsnewlen(init, initlen);
}

// sds拷贝
sds sdsdup(const sds s) {
    return sdsnewlen(s, sdslen(s));
}

// sds是一个字符数组,  但是要释放是sdshdr? 因此要计算 header偏移
void sdsfree(sds s) {
    if (s == NULL)
        return;
    s_free((char *)s - sdsHdrSize(s[-1]));
}

/* Set the sds string length to the length as obtained with strlen(), so
 * considering as content only up to the first null term character.
 *
 * This function is useful when the sds string is hacked manually in some
 * way, like in the following example:
 *
 * s = sdsnew("foobar");
 * s[2] = '\0';
 * sdsupdatelen(s);
 * printf("%d\n", sdslen(s));
 *
 * The output will be "2", but if we comment out the call to sdsupdatelen()
 * the output will be "6" as the string was modified but the logical length
 * remains 6 bytes. */
void sdsupdatelen(sds s) {
    size_t reallen = strlen(s);
    sdssetlen(s, reallen);
}

/* Modify an sds string in-place to make it empty (zero length).
 * However all the existing buffer is not discarded but set as free space
 * so that next append operations will not require allocations up to the
 * number of bytes previously available. */
void sdsclear(sds s) {
    sdssetlen(s, 0);
    s[0] = '\0';
}

// 扩大sds字符串末尾的空闲空间，以便调用者确保在调用此函数后可以覆盖字符串末尾后最多addlen字节，再加上一个字节用于nul项。
// 如果已经有足够的空闲空间，这个函数不做任何操作就返回，如果没有足够的空闲空间，它会分配缺少的部分，甚至更多:
// 当greedy为1时，放大超过所需，以避免未来对增量增长的重新分配。
// 当greedy为0时，放大到足够大，以便为'addlen'留出自由空间。
// 注意:这不会改变sdslen()返回的sds字符串的长度，而只改变我们拥有的空闲缓冲区空间
sds _sdsMakeRoomFor(sds s, size_t addlen, int greedy) {
    void *sh, *newsh;
    size_t avail = sdsavail(s);
    size_t len, newlen, reqlen;
    char type, oldtype = s[-1] & SDS_TYPE_MASK;
    int hdrlen;
    size_t usable;

    /* Return ASAP if there is enough space left. */
    if (avail >= addlen)
        return s;
    //    printf("分配空间,%zu\n",addlen);
    len = sdslen(s);
    sh = (char *)s - sdsHdrSize(oldtype);
    reqlen = newlen = (len + addlen);
    assert(newlen > len); /* Catch size_t overflow */
    if (greedy == 1) {
        if (newlen < SDS_MAX_PREALLOC)
            newlen *= 2;
        else
            newlen += SDS_MAX_PREALLOC;
    }

    type = sdsReqType(newlen);

    /* Don't use type 5: the user is appending to the string and type 5 is
     * not able to remember empty space, so sdsMakeRoomFor() must be called
     * at every appending operation. */
    if (type == SDS_TYPE_5)
        type = SDS_TYPE_8;

    hdrlen = sdsHdrSize(type);
    assert(hdrlen + newlen + 1 > reqlen); /* Catch size_t overflow */
    if (oldtype == type) {
        newsh = s_realloc_usable(sh, hdrlen + newlen + 1, &usable);
        if (newsh == NULL)
            return NULL;
        s = (char *)newsh + hdrlen;
    }
    else {
        /* Since the header size changes, need to move the string forward,
         * and can't use realloc */
        newsh = s_malloc_usable(hdrlen + newlen + 1, &usable);
        if (newsh == NULL)
            return NULL;
        memcpy((char *)newsh + hdrlen, s, len + 1);
        s_free(sh);
        s = (char *)newsh + hdrlen;
        s[-1] = type;
        sdssetlen(s, len);
    }
    usable = usable - hdrlen - 1;
    if (usable > sdsTypeMaxSize(type))
        usable = sdsTypeMaxSize(type);
    sdssetalloc(s, usable);
    return s;
}

/* 将sds字符串末尾的空闲空间扩大到超出需要的范围,这有助于避免在重复追加sds时重复重新分配.*/
sds sdsMakeRoomFor(sds s, size_t addlen) {
    return _sdsMakeRoomFor(s, addlen, 1);
}

/* Unlike sdsMakeRoomFor(), 这个只会增长到所需的大小. */
sds sdsMakeRoomForNonGreedy(sds s, size_t addlen) {
    return _sdsMakeRoomFor(s, addlen, 0);
}

// 重新分配sds字符串,使其在末尾没有空闲空间.所包含的字符串没有改变,但是下一个连接操作将需要重新分配.
// 调用之后,传递的sds字符串不再有效,所有引用都必须用调用返回的新指针替换.
sds sdsRemoveFreeSpace(sds s) {
    void *sh; // sds 结构体的首地址
    void *newsh;
    char type, oldtype = s[-1] & SDS_TYPE_MASK;  // 获取sds的类型
    int hdrlen, oldhdrlen = sdsHdrSize(oldtype); // 返回sds结构体长度,不算数据
    size_t len = sdslen(s);
    size_t avail = sdsavail(s);
    sh = (char *)s - oldhdrlen;

    if (avail == 0) {
        return s;
    }

    // 检查刚刚好适合这个字符串的最小SDS头.
    type = sdsReqType(len);
    hdrlen = sdsHdrSize(type);

    // 如果类型相同,或者至少需要足够大的类型,则只需realloc(),让分配器只在真正需要时进行复制.否则,如果变化很大,我们手动重新分配字符串,以使用不同的头类型.
    if (oldtype == type || type > SDS_TYPE_8) {
        //        ptr -- 指针指向一个要重新分配内存的内存块,该内存块之前是通过调用 malloc、calloc 或 realloc 进行分配内存的.
        //        如果为空指针,则会分配一个新的内存块,且函数返回一个指向它的指针.
        //        size -- 内存块的新的大小,以字节为单位.如果大小为 0,且 ptr 指向一个已存在的内存块,则 ptr 所指向的内存块会被释放,并返回一个空指针.
        // 新分配的内存块里的数据不会变
        newsh = s_realloc(sh, oldhdrlen + len + 1);
        if (newsh == NULL) {
            return NULL;
        }
        s = (char *)newsh + oldhdrlen; // 问题来了, ptr指向的内存块没有被释放么？
    }
    else {
        newsh = s_malloc(hdrlen + len + 1);
        if (newsh == NULL)
            return NULL;
        memcpy((char *)newsh + hdrlen, s, len + 1);
        s_free(sh);
        s = (char *)newsh + hdrlen;
        s[-1] = type;
        sdssetlen(s, len);
    }
    sdssetalloc(s, len);
    return s;
}

/* Resize the allocation, this can make the allocation bigger or smaller,
 * if the size is smaller than currently used len, the data will be truncated */
sds sdsResize(sds s, size_t size) {
    void *sh, *newsh;
    char type, oldtype = s[-1] & SDS_TYPE_MASK;
    int hdrlen, oldhdrlen = sdsHdrSize(oldtype);
    size_t len = sdslen(s);
    sh = (char *)s - oldhdrlen;

    /* Return ASAP if the size is already good. */
    if (sdsalloc(s) == size)
        return s;

    /* Truncate len if needed. */
    if (size < len)
        len = size;

    /* Check what would be the minimum SDS header that is just good enough to
     * fit this string. */
    type = sdsReqType(size);
    /* Don't use type 5, it is not good for strings that are resized. */
    if (type == SDS_TYPE_5)
        type = SDS_TYPE_8;
    hdrlen = sdsHdrSize(type);

    /* If the type is the same, or can hold the size in it with low overhead
     * (larger than SDS_TYPE_8), we just realloc(), letting the allocator
     * to do the copy only if really needed. Otherwise if the change is
     * huge, we manually reallocate the string to use the different header
     * type. */
    if (oldtype == type || (type < oldtype && type > SDS_TYPE_8)) {
        newsh = s_realloc(sh, oldhdrlen + size + 1);
        if (newsh == NULL)
            return NULL;
        s = (char *)newsh + oldhdrlen;
    }
    else {
        newsh = s_malloc(hdrlen + size + 1);
        if (newsh == NULL)
            return NULL;
        memcpy((char *)newsh + hdrlen, s, len);
        s_free(sh);
        s = (char *)newsh + hdrlen;
        s[-1] = type;
    }
    s[len] = 0;
    sdssetlen(s, len);
    sdssetalloc(s, size);
    return s;
}

/* Return the total size of the allocation of the specified sds string,
 * including:
 * 1) The sds header before the pointer.
 * 2) The string.
 * 3) The free buffer at the end if any.
 * 4) The implicit null term.
 */
size_t sdsAllocSize(sds s) {
    size_t alloc = sdsalloc(s);
    return sdsHdrSize(s[-1]) + alloc + 1;
}

// 返回实际SDS分配的指针(通常SDS字符串由字符串缓冲区的开始引用)。
void *sdsAllocPtr(sds s) {
    return (void *)(s - sdsHdrSize(s[-1]));
}

// 对s增加incr空间的长度,只是将\0后移incr个长度
void sdsIncrLen(sds s, ssize_t incr) {
    unsigned char flags = s[-1];
    size_t len;
    switch (flags & SDS_TYPE_MASK) {
        case SDS_TYPE_5: {
            unsigned char *fp = ((unsigned char *)s) - 1;
            unsigned char oldlen = SDS_TYPE_5_LEN(flags);
            assert((incr > 0 && oldlen + incr < 32) || (incr < 0 && oldlen >= (unsigned int)(-incr)));
            *fp = SDS_TYPE_5 | ((oldlen + incr) << SDS_TYPE_BITS);
            len = oldlen + incr;
            break;
        }
        case SDS_TYPE_8: {
            SDS_HDR_VAR(8, s);
            assert((incr >= 0 && sh->alloc - sh->len >= incr) || (incr < 0 && sh->len >= (unsigned int)(-incr)));
            len = (sh->len += incr);
            break;
        }
        case SDS_TYPE_16: {
            SDS_HDR_VAR(16, s);
            assert((incr >= 0 && sh->alloc - sh->len >= incr) || (incr < 0 && sh->len >= (unsigned int)(-incr)));
            len = (sh->len += incr);
            break;
        }
        case SDS_TYPE_32: {
            SDS_HDR_VAR(32, s);
            assert((incr >= 0 && sh->alloc - sh->len >= (unsigned int)incr) || (incr < 0 && sh->len >= (unsigned int)(-incr)));
            len = (sh->len += incr);
            break;
        }
        case SDS_TYPE_64: {
            SDS_HDR_VAR(64, s);
            assert((incr >= 0 && sh->alloc - sh->len >= (uint64_t)incr) || (incr < 0 && sh->len >= (uint64_t)(-incr)));
            len = (sh->len += incr);
            break;
        }
        default:
            len = 0; /* Just to avoid compilation warnings. */
    }
    s[len] = '\0';
}

/* Grow the sds to have the specified length. Bytes that were not part of
 * the original length of the sds will be set to zero.
 *
 * if the specified length is smaller than the current length, no operation
 * is performed. */
sds sdsgrowzero(sds s, size_t len) {
    size_t curlen = sdslen(s);

    if (len <= curlen)
        return s;
    s = sdsMakeRoomFor(s, len - curlen);
    if (s == NULL)
        return NULL;

    /* Make sure added region doesn't contain garbage */
    // 将分配的内存全置为0,防止有残留数据
    memset(s + curlen, 0, (len - curlen + 1)); /* also set trailing \0 byte */
    sdssetlen(s, len);
    return s;
}

// 字符串追加操作
// 目标字符串 s、源字符串 t 和要追加的长度 len
//  s+t
sds sdscatlen(sds s, const void *t, size_t len) {
    size_t curlen = sdslen(s);  // 获取目标字符串s的当前长度
    s = sdsMakeRoomFor(s, len); // 根据要追加的长度len和目标字符串s的现有长度,判断是否要增加新的空间  , 空间检查和扩容
    if (s == NULL) {
        return NULL;
    }
    memcpy(s + curlen, t, len); // 将源字符串t中len长度的数据拷贝到目标字符串结尾
    sdssetlen(s, curlen + len); // 设置目标字符串的最新长度：拷贝前长度curlen加上拷贝长度
    s[curlen + len] = '\0';     // 拷贝后,在目标字符串结尾加上\0
    return s;
}

/* Append the specified null terminated C string to the sds string 's'.
 *
 * After the call, the passed sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
sds sdscat(sds s, const char *t) {
    return sdscatlen(s, t, strlen(t));
}

/* Append the specified sds 't' to the existing sds 's'.
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
sds sdscatsds(sds s, const sds t) {
    return sdscatlen(s, t, sdslen(t));
}

/* Destructively modify the sds string 's' to hold the specified binary
 * safe string pointed by 't' of length 'len' bytes. */
sds sdscpylen(sds s, const char *t, size_t len) {
    if (sdsalloc(s) < len) {
        s = sdsMakeRoomFor(s, len - sdslen(s));
        if (s == NULL)
            return NULL;
    }
    memcpy(s, t, len);
    s[len] = '\0';
    sdssetlen(s, len);
    return s;
}

/* Like sdscpylen() but 't' must be a null-terminated string so that the length
 * of the string is obtained with strlen(). */
sds sdscpy(sds s, const char *t) {
    return sdscpylen(s, t, strlen(t));
}

/* Helper for sdscatlonglong() doing the actual number -> string
 * conversion. 's' must point to a string with room for at least
 * SDS_LLSTR_SIZE bytes.
 *
 * The function returns the length of the null-terminated string
 * representation stored at 's'. */
#define SDS_LLSTR_SIZE 21

int sdsll2str(char *s, long long value) {
    char *p, aux;
    unsigned long long v;
    size_t l;

    /* Generate the string representation, this method produces
     * a reversed string. */
    if (value < 0) {
        /* Since v is unsigned, if value==LLONG_MIN, -LLONG_MIN will overflow. */
        if (value != LLONG_MIN) {
            v = -value;
        }
        else {
            v = ((unsigned long long)LLONG_MAX) + 1;
        }
    }
    else {
        v = value;
    }

    p = s;
    do {
        *p++ = '0' + (v % 10);
        v /= 10;
    } while (v);
    if (value < 0)
        *p++ = '-';

    /* Compute length and add null term. */
    l = p - s;
    *p = '\0';

    /* Reverse the string. */
    p--;
    while (s < p) {
        aux = *s;
        *s = *p;
        *p = aux;
        s++;
        p--;
    }
    return l;
}

/* Identical sdsll2str(), but for unsigned long long type. */
int sdsull2str(char *s, unsigned long long v) {
    char *p, aux;
    size_t l;

    /* Generate the string representation, this method produces
     * a reversed string. */
    p = s;
    do {
        *p++ = '0' + (v % 10);
        v /= 10;
    } while (v);

    /* Compute length and add null term. */
    l = p - s;
    *p = '\0';

    /* Reverse the string. */
    p--;
    while (s < p) {
        aux = *s;
        *s = *p;
        *p = aux;
        s++;
        p--;
    }
    return l;
}

/* Create an sds string from a long long value. It is much faster than:
 *
 * sdscatprintf(sdsempty(),"%lld\n", value);
 */
sds sdsfromlonglong(long long value) {
    char buf[SDS_LLSTR_SIZE];
    int len = sdsll2str(buf, value);

    return sdsnewlen(buf, len);
}

/* Like sdscatprintf() but gets va_list instead of being variadic. */
sds sdscatvprintf(sds s, const char *fmt, va_list ap) {
    va_list cpy;
    char staticbuf[1024], *buf = staticbuf, *t;
    size_t buflen = strlen(fmt) * 2;
    int bufstrlen;

    /* We try to start using a static buffer for speed.
     * If not possible we revert to heap allocation. */
    if (buflen > sizeof(staticbuf)) {
        buf = s_malloc(buflen);
        if (buf == NULL)
            return NULL;
    }
    else {
        buflen = sizeof(staticbuf);
    }

    /* Alloc enough space for buffer and \0 after failing to
     * fit the string in the current buffer size. */
    while (1) {
        va_copy(cpy, ap);
        bufstrlen = vsnprintf(buf, buflen, fmt, cpy);
        va_end(cpy);
        if (bufstrlen < 0) {
            if (buf != staticbuf)
                s_free(buf);
            return NULL;
        }
        if (((size_t)bufstrlen) >= buflen) {
            if (buf != staticbuf)
                s_free(buf);
            buflen = ((size_t)bufstrlen) + 1;
            buf = s_malloc(buflen);
            if (buf == NULL)
                return NULL;
            continue;
        }
        break;
    }

    /* Finally concat the obtained string to the SDS string and return it. */
    t = sdscatlen(s, buf, bufstrlen);
    if (buf != staticbuf)
        s_free(buf);
    return t;
}

/* Append to the sds string 's' a string obtained using printf-alike format
 * specifier.
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call.
 *
 * Example:
 *
 * s = sdsnew("Sum is: ");
 * s = sdscatprintf(s,"%d+%d = %d",a,b,a+b).
 *
 * Often you need to create a string from scratch with the printf-alike
 * format. When this is the need, just use sdsempty() as the target string:
 *
 * s = sdscatprintf(sdsempty(), "... your format ...", args);
 */
sds sdscatprintf(sds s, const char *fmt, ...) {
    va_list ap;
    char *t;
    va_start(ap, fmt);
    t = sdscatvprintf(s, fmt, ap);
    va_end(ap);
    return t;
}

/* This function is similar to sdscatprintf, but much faster as it does
 * not rely on sprintf() family functions implemented by the libc that
 * are often very slow. Moreover directly handling the sds string as
 * new data is concatenated provides a performance improvement.
 *
 * However this function only handles an incompatible subset of printf-alike
 * format specifiers:
 *
 * %s - C String
 * %S - SDS string
 * %i - signed int
 * %I - 64 bit signed integer (long long, int64_t)
 * %u - unsigned int
 * %U - 64 bit unsigned integer (unsigned long long, uint64_t)
 * %% - Verbatim "%" character.
 */
sds sdscatfmt(sds s, char const *fmt, ...) {
    size_t initlen = sdslen(s);
    const char *f = fmt;
    long i;
    va_list ap;

    /* To avoid continuous reallocations, let's start with a buffer that
     * can hold at least two times the format string itself. It's not the
     * best heuristic but seems to work in practice. */
    s = sdsMakeRoomFor(s, strlen(fmt) * 2);
    va_start(ap, fmt);
    f = fmt;     /* Next format specifier byte to process. */
    i = initlen; /* Position of the next byte to write to dest str. */
    while (*f) {
        char next, *str;
        size_t l;
        long long num;
        unsigned long long unum;

        /* Make sure there is always space for at least 1 char. */
        if (sdsavail(s) == 0) {
            s = sdsMakeRoomFor(s, 1);
        }

        switch (*f) {
            case '%':
                next = *(f + 1);
                if (next == '\0')
                    break;
                f++;
                switch (next) {
                    case 's':
                    case 'S':
                        str = va_arg(ap, char *);
                        l = (next == 's') ? strlen(str) : sdslen(str);
                        if (sdsavail(s) < l) {
                            s = sdsMakeRoomFor(s, l);
                        }
                        memcpy(s + i, str, l);
                        sdsinclen(s, l);
                        i += l;
                        break;
                    case 'i':
                    case 'I':
                        if (next == 'i')
                            num = va_arg(ap, int);
                        else
                            num = va_arg(ap, long long);
                        {
                            char buf[SDS_LLSTR_SIZE];
                            l = sdsll2str(buf, num);
                            if (sdsavail(s) < l) {
                                s = sdsMakeRoomFor(s, l);
                            }
                            memcpy(s + i, buf, l);
                            sdsinclen(s, l);
                            i += l;
                        }
                        break;
                    case 'u':
                    case 'U':
                        if (next == 'u')
                            unum = va_arg(ap, unsigned int);
                        else
                            unum = va_arg(ap, unsigned long long);
                        {
                            char buf[SDS_LLSTR_SIZE];
                            l = sdsull2str(buf, unum);
                            if (sdsavail(s) < l) {
                                s = sdsMakeRoomFor(s, l);
                            }
                            memcpy(s + i, buf, l);
                            sdsinclen(s, l);
                            i += l;
                        }
                        break;
                    default: /* Handle %% and generally %<unknown>. */
                        s[i++] = next;
                        sdsinclen(s, 1);
                        break;
                }
                break;
            default:
                s[i++] = *f;
                sdsinclen(s, 1);
                break;
        }
        f++;
    }
    va_end(ap);

    /* Add null-term */
    s[i] = '\0';
    return s;
}

/* Remove the part of the string from left and from right composed just of
 * contiguous characters found in 'cset', that is a null terminated C string.
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call.
 *
 * Example:
 *
 * s = sdsnew("AA...AA.a.aa.aHelloWorld     :::");
 * s = sdstrim(s,"Aa. :");
 * printf("%s\n", s);
 *
 * Output will be just "HelloWorld".
 */
sds sdstrim(sds s, const char *cset) {
    char *end, *sp, *ep;
    size_t len;

    sp = s;
    ep = end = s + sdslen(s) - 1;
    while (sp <= end && strchr(cset, *sp)) sp++;
    while (ep > sp && strchr(cset, *ep)) ep--;
    len = (ep - sp) + 1;
    if (s != sp)
        memmove(s, sp, len);
    s[len] = '\0';
    sdssetlen(s, len);
    return s;
}

/* Changes the input string to be a subset of the original.
 * It does not release the free space in the string, so a call to
 * sdsRemoveFreeSpace may be wise after. */
void sdssubstr(sds s, size_t start, size_t len) {
    /* Clamp out of range input */
    size_t oldlen = sdslen(s);
    if (start >= oldlen)
        start = len = 0;
    if (len > oldlen - start)
        len = oldlen - start;

    /* Move the data */
    if (len)
        memmove(s, s + start, len);
    s[len] = 0;
    sdssetlen(s, len);
}

// 返回sds的一部分
void sdsrange(sds s, ssize_t start, ssize_t end) {
    size_t newlen, len = sdslen(s);
    if (len == 0)
        return;
    if (start < 0)
        start = len + start;
    if (end < 0)
        end = len + end;
    newlen = (start > end) ? 0 : (end - start) + 1;
    sdssubstr(s, start, newlen);
}

/* Apply tolower() to every character of the sds string 's'. */
void sdstolower(sds s) {
    size_t len = sdslen(s), j;

    for (j = 0; j < len; j++) s[j] = tolower(s[j]);
}

/* Apply toupper() to every character of the sds string 's'. */
void sdstoupper(sds s) {
    size_t len = sdslen(s), j;

    for (j = 0; j < len; j++) s[j] = toupper(s[j]);
}

/* Compare two sds strings s1 and s2 with memcmp().
 *
 * Return value:
 *
 *     positive if s1 > s2.
 *     negative if s1 < s2.
 *     0 if s1 and s2 are exactly the same binary string.
 *
 * If two strings share exactly the same prefix, but one of the two has
 * additional characters, the longer string is considered to be greater than
 * the smaller one. */
int sdscmp(const sds s1, const sds s2) {
    size_t l1, l2, minlen;
    int cmp;

    l1 = sdslen(s1);
    l2 = sdslen(s2);
    minlen = (l1 < l2) ? l1 : l2;
    cmp = memcmp(s1, s2, minlen);
    if (cmp == 0)
        return l1 > l2 ? 1 : (l1 < l2 ? -1 : 0);
    return cmp;
}

/* Split 's' with separator in 'sep'. An array
 * of sds strings is returned. *count will be set
 * by reference to the number of tokens returned.
 *
 * On out of memory, zero length string, zero length
 * separator, NULL is returned.
 *
 * Note that 'sep' is able to split a string using
 * a multi-character separator. For example
 * sdssplit("foo_-_bar","_-_"); will return two
 * elements "foo" and "bar".
 *
 * This version of the function is binary-safe but
 * requires length arguments. sdssplit() is just the
 * same function but for zero-terminated strings.
 */
sds *sdssplitlen(const char *s, ssize_t len, const char *sep, int seplen, int *count) {
    int elements = 0, slots = 5;
    long start = 0, j;
    sds *tokens;

    if (seplen < 1 || len <= 0) {
        *count = 0;
        return NULL;
    }
    tokens = s_malloc(sizeof(sds) * slots);
    if (tokens == NULL)
        return NULL;

    for (j = 0; j < (len - (seplen - 1)); j++) {
        /* make sure there is room for the next element and the final one */
        if (slots < elements + 2) {
            sds *newtokens;

            slots *= 2;
            newtokens = s_realloc(tokens, sizeof(sds) * slots);
            if (newtokens == NULL)
                goto cleanup;
            tokens = newtokens;
        }
        /* search the separator */
        if ((seplen == 1 && *(s + j) == sep[0]) || (memcmp(s + j, sep, seplen) == 0)) {
            tokens[elements] = sdsnewlen(s + start, j - start);
            if (tokens[elements] == NULL)
                goto cleanup;
            elements++;
            start = j + seplen;
            j = j + seplen - 1; /* skip the separator */
        }
    }
    /* Add the final element. We are sure there is room in the tokens array. */
    tokens[elements] = sdsnewlen(s + start, len - start);
    if (tokens[elements] == NULL)
        goto cleanup;
    elements++;
    *count = elements;
    return tokens;

cleanup : {
    int i;
    for (i = 0; i < elements; i++) sdsfree(tokens[i]);
    s_free(tokens);
    *count = 0;
    return NULL;
}
}

/* Free the result returned by sdssplitlen(), or do nothing if 'tokens' is NULL. */
void sdsfreesplitres(sds *tokens, int count) {
    if (!tokens)
        return;
    while (count--) sdsfree(tokens[count]);
    s_free(tokens);
}

// 在sds字符串"s"中添加一个转义字符串表示，其中所有不可打印的字符(用isprint()测试)都转换为转义，形式为"\n\r\a...."或"\x&lt;hex-number&gt;"
// 调用后，修改后的sds字符串不再有效，所有引用必须用调用返回的新指针代替。
sds sdscatrepr(sds s, const char *p, size_t len) {
    s = sdscatlen(s, "\"", 1);
    while (len--) {
        switch (*p) {
            case '\\':
            case '"':
                s = sdscatprintf(s, "\\%c", *p);
                break;
            case '\n':
                s = sdscatlen(s, "\\n", 2);
                break;
            case '\r':
                s = sdscatlen(s, "\\r", 2);
                break;
            case '\t':
                s = sdscatlen(s, "\\t", 2);
                break;
            case '\a':
                s = sdscatlen(s, "\\a", 2);
                break;
            case '\b':
                s = sdscatlen(s, "\\b", 2);
                break;
            default:
                if (isprint(*p))
                    s = sdscatprintf(s, "%c", *p);
                else
                    s = sdscatprintf(s, "\\x%02x", (unsigned char)*p);
                break;
        }
        p++;
    }
    return sdscatlen(s, "\"", 1);
}

/* Returns one if the string contains characters to be escaped
 * by sdscatrepr(), zero otherwise.
 *
 * Typically, this should be used to help protect aggregated strings in a way
 * that is compatible with sdssplitargs(). For this reason, also spaces will be
 * treated as needing an escape.
 */
int sdsneedsrepr(const sds s) {
    size_t len = sdslen(s);
    const char *p = s;

    while (len--) {
        if (*p == '\\' || *p == '"' || *p == '\n' || *p == '\r' || *p == '\t' || *p == '\a' || *p == '\b' || !isprint(*p) || isspace(*p))
            return 1;
        p++;
    }

    return 0;
}

/* Helper function for sdssplitargs() that returns non zero if 'c'
 * is a valid hex digit. */
int is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* Helper function for sdssplitargs() that converts a hex digit into an
 * integer from 0 to 15 */
int hex_digit_to_int(char c) {
    switch (c) {
        case '0':
            return 0;
        case '1':
            return 1;
        case '2':
            return 2;
        case '3':
            return 3;
        case '4':
            return 4;
        case '5':
            return 5;
        case '6':
            return 6;
        case '7':
            return 7;
        case '8':
            return 8;
        case '9':
            return 9;
        case 'a':
        case 'A':
            return 10;
        case 'b':
        case 'B':
            return 11;
        case 'c':
        case 'C':
            return 12;
        case 'd':
        case 'D':
            return 13;
        case 'e':
        case 'E':
            return 14;
        case 'f':
        case 'F':
            return 15;
        default:
            return 0;
    }
}

/* Split a line into arguments, where every argument can be in the
 * following programming-language REPL-alike form:
 *
 * foo bar "newline are supported\n" and "\xff\x00otherstuff"
 *
 * The number of arguments is stored into *argc, and an array
 * of sds is returned.
 *
 * The caller should free the resulting array of sds strings with
 * sdsfreesplitres().
 *
 * Note that sdscatrepr() is able to convert back a string into
 * a quoted string in the same format sdssplitargs() is able to parse.
 *
 * The function returns the allocated tokens on success, even when the
 * input string is empty, or NULL if the input contains unbalanced
 * quotes or closed quotes followed by non space characters
 * as in: "foo"bar or "foo'
 */
sds *sdssplitargs(const char *line, int *argc) {
    const char *p = line;
    char *current = NULL;
    char **vector = NULL;

    *argc = 0;
    while (1) {
        /* skip blanks */
        while (*p && isspace(*p)) p++;
        if (*p) {
            /* get a token */
            int inq = 0;  /* set to 1 if we are in "quotes" */
            int insq = 0; /* set to 1 if we are in 'single quotes' */
            int done = 0;

            if (current == NULL)
                current = sdsempty();
            while (!done) {
                if (inq) {
                    if (*p == '\\' && *(p + 1) == 'x' && is_hex_digit(*(p + 2)) && is_hex_digit(*(p + 3))) {
                        unsigned char byte;

                        byte = (hex_digit_to_int(*(p + 2)) * 16) + hex_digit_to_int(*(p + 3));
                        current = sdscatlen(current, (char *)&byte, 1);
                        p += 3;
                    }
                    else if (*p == '\\' && *(p + 1)) {
                        char c;

                        p++;
                        switch (*p) {
                            case 'n':
                                c = '\n';
                                break;
                            case 'r':
                                c = '\r';
                                break;
                            case 't':
                                c = '\t';
                                break;
                            case 'b':
                                c = '\b';
                                break;
                            case 'a':
                                c = '\a';
                                break;
                            default:
                                c = *p;
                                break;
                        }
                        current = sdscatlen(current, &c, 1);
                    }
                    else if (*p == '"') {
                        /* closing quote must be followed by a space or
                         * nothing at all. */
                        if (*(p + 1) && !isspace(*(p + 1)))
                            goto err;
                        done = 1;
                    }
                    else if (!*p) {
                        /* unterminated quotes */
                        goto err;
                    }
                    else {
                        current = sdscatlen(current, p, 1);
                    }
                }
                else if (insq) {
                    if (*p == '\\' && *(p + 1) == '\'') {
                        p++;
                        current = sdscatlen(current, "'", 1);
                    }
                    else if (*p == '\'') {
                        /* closing quote must be followed by a space or
                         * nothing at all. */
                        if (*(p + 1) && !isspace(*(p + 1)))
                            goto err;
                        done = 1;
                    }
                    else if (!*p) {
                        /* unterminated quotes */
                        goto err;
                    }
                    else {
                        current = sdscatlen(current, p, 1);
                    }
                }
                else {
                    switch (*p) {
                        case ' ':
                        case '\n':
                        case '\r':
                        case '\t':
                        case '\0':
                            done = 1;
                            break;
                        case '"':
                            inq = 1;
                            break;
                        case '\'':
                            insq = 1;
                            break;
                        default:
                            current = sdscatlen(current, p, 1);
                            break;
                    }
                }
                if (*p)
                    p++;
            }
            /* add the token to the vector */
            vector = s_realloc(vector, ((*argc) + 1) * sizeof(char *));
            vector[*argc] = current;
            (*argc)++;
            current = NULL;
        }
        else {
            /* Even on empty input string return something not NULL. */
            if (vector == NULL)
                vector = s_malloc(sizeof(void *));
            return vector;
        }
    }

err:
    while ((*argc)--) sdsfree(vector[*argc]);
    s_free(vector);
    if (current)
        sdsfree(current);
    *argc = 0;
    return NULL;
}

/* Modify the string substituting all the occurrences of the set of
 * characters specified in the 'from' string to the corresponding character
 * in the 'to' array.
 *
 * For instance: sdsmapchars(mystring, "ho", "01", 2)
 * will have the effect of turning the string "hello" into "0ell1".
 *
 * The function returns the sds string pointer, that is always the same
 * as the input pointer since no resize is needed. */
sds sdsmapchars(sds s, const char *from, const char *to, size_t setlen) {
    size_t j, i, l = sdslen(s);

    for (j = 0; j < l; j++) {
        for (i = 0; i < setlen; i++) {
            if (s[j] == from[i]) {
                s[j] = to[i];
                break;
            }
        }
    }
    return s;
}

/* Join an array of C strings using the specified separator (also a C string).
 * Returns the result as an sds string. */
sds sdsjoin(char **argv, int argc, char *sep) {
    sds join = sdsempty();
    int j;

    for (j = 0; j < argc; j++) {
        join = sdscat(join, argv[j]);
        if (j != argc - 1)
            join = sdscat(join, sep);
    }
    return join;
}

/* Like sdsjoin, but joins an array of SDS strings. */
sds sdsjoinsds(sds *argv, int argc, const char *sep, size_t seplen) {
    sds join = sdsempty();
    int j;

    for (j = 0; j < argc; j++) {
        join = sdscatsds(join, argv[j]);
        if (j != argc - 1)
            join = sdscatlen(join, sep, seplen);
    }
    return join;
}

/* Wrappers to the allocators used by SDS. Note that SDS will actually
 * just use the macros defined into sdsalloc.h in order to avoid to pay
 * the overhead of function calls. Here we define these wrappers only for
 * the programs SDS is linked to, if they want to touch the SDS internals
 * even if they use a different allocator. */
void *sds_malloc(size_t size) {
    return s_malloc(size);
}

void *sds_realloc(void *ptr, size_t size) {
    return s_realloc(ptr, size);
}

void sds_free(void *ptr) {
    s_free(ptr);
}

/* Perform expansion of a template string and return the result as a newly
 * allocated sds.
 *
 * Template variables are specified using curly brackets, e.g. {variable}.
 * An opening bracket can be quoted by repeating it twice.
 */
sds sdstemplate(const char *template, sdstemplate_callback_t cb_func, void *cb_arg) {
    sds res = sdsempty();
    const char *p = template;

    while (*p) {
        /* Find next variable, copy everything until there */
        const char *sv = strchr(p, '{');
        if (!sv) {
            /* Not found: copy till rest of template and stop */
            res = sdscat(res, p);
            break;
        }
        else if (sv > p) {
            /* Found: copy anything up to the beginning of the variable */
            res = sdscatlen(res, p, sv - p);
        }

        /* Skip into variable name, handle premature end or quoting */
        sv++;
        if (!*sv)
            goto error; /* Premature end of template */
        if (*sv == '{') {
            /* Quoted '{' */
            p = sv + 1;
            res = sdscat(res, "{");
            continue;
        }

        /* Find end of variable name, handle premature end of template */
        const char *ev = strchr(sv, '}');
        if (!ev)
            goto error;

        /* Pass variable name to callback and obtain value. If callback failed,
         * abort. */
        sds varname = sdsnewlen(sv, ev - sv);
        sds value = cb_func(varname, cb_arg);
        sdsfree(varname);
        if (!value)
            goto error;

        /* Append value to result and continue */
        res = sdscat(res, value);
        sdsfree(value);
        p = ev + 1;
    }

    return res;

error:
    sdsfree(res);
    return NULL;
}

#ifdef REDIS_TEST
#    include <stdio.h>
#    include <limits.h>
#    include "testhelp.h"

#    define UNUSED(x) (void)(x)

static sds sdsTestTemplateCallback(sds varname, void *arg) {
    UNUSED(arg);
    static const char *_var1 = "variable1";
    static const char *_var2 = "variable2";

    if (!strcmp(varname, _var1))
        return sdsnew("value1");
    else if (!strcmp(varname, _var2))
        return sdsnew("value2");
    else
        return NULL;
}

int sdsTest(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    {
        sds x = sdsnew("foo"), y;

        test_cond("Create a string and obtain the length", sdslen(x) == 3 && memcmp(x, "foo\0", 4) == 0);

        sdsfree(x);
        x = sdsnewlen("foo", 2);
        test_cond("Create a string with specified length", sdslen(x) == 2 && memcmp(x, "fo\0", 3) == 0);

        x = sdscat(x, "bar");
        test_cond("Strings concatenation", sdslen(x) == 5 && memcmp(x, "fobar\0", 6) == 0);

        x = sdscpy(x, "a");
        test_cond("sdscpy() against an originally longer string", sdslen(x) == 1 && memcmp(x, "a\0", 2) == 0);

        x = sdscpy(x, "xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk");
        test_cond("sdscpy() against an originally shorter string", sdslen(x) == 33 && memcmp(x, "xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk\0", 33) == 0);

        sdsfree(x);
        x = sdscatprintf(sdsempty(), "%d", 123);
        test_cond("sdscatprintf() seems working in the base case", sdslen(x) == 3 && memcmp(x, "123\0", 4) == 0);

        sdsfree(x);
        x = sdscatprintf(sdsempty(), "a%cb", 0);
        test_cond(
            "sdscatprintf() seems working with \\0 inside of result", sdslen(x) == 3 && memcmp(
                                                                                            x,
                                                                                            "a\0"
                                                                                            "b\0",
                                                                                            4) == 0);

        {
            sdsfree(x);
            char etalon[1024 * 1024];
            for (size_t i = 0; i < sizeof(etalon); i++) {
                etalon[i] = '0';
            }
            x = sdscatprintf(sdsempty(), "%0*d", (int)sizeof(etalon), 0);
            test_cond("sdscatprintf() can print 1MB", sdslen(x) == sizeof(etalon) && memcmp(x, etalon, sizeof(etalon)) == 0);
        }

        sdsfree(x);
        x = sdsnew("--");
        x = sdscatfmt(x, "Hello %s World %I,%I--", "Hi!", LLONG_MIN, LLONG_MAX);
        test_cond(
            "sdscatfmt() seems working in the base case", sdslen(x) == 60 && memcmp(
                                                                                 x,
                                                                                 "--Hello Hi! World -9223372036854775808,"
                                                                                 "9223372036854775807--",
                                                                                 60) == 0);
        printf("[%s]\n", x);

        sdsfree(x);
        x = sdsnew("--");
        x = sdscatfmt(x, "%u,%U--", UINT_MAX, ULLONG_MAX);
        test_cond("sdscatfmt() seems working with unsigned numbers", sdslen(x) == 35 && memcmp(x, "--4294967295,18446744073709551615--", 35) == 0);

        sdsfree(x);
        x = sdsnew(" x ");
        sdstrim(x, " x");
        test_cond("sdstrim() works when all chars match", sdslen(x) == 0);

        sdsfree(x);
        x = sdsnew(" x ");
        sdstrim(x, " ");
        test_cond("sdstrim() works when a single char remains", sdslen(x) == 1 && x[0] == 'x');

        sdsfree(x);
        x = sdsnew("xxciaoyyy");
        sdstrim(x, "xy");
        test_cond("sdstrim() correctly trims characters", sdslen(x) == 4 && memcmp(x, "ciao\0", 5) == 0);

        y = sdsdup(x);
        sdsrange(y, 1, 1);
        test_cond("sdsrange(...,1,1)", sdslen(y) == 1 && memcmp(y, "i\0", 2) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y, 1, -1);
        test_cond("sdsrange(...,1,-1)", sdslen(y) == 3 && memcmp(y, "iao\0", 4) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y, -2, -1);
        test_cond("sdsrange(...,-2,-1)", sdslen(y) == 2 && memcmp(y, "ao\0", 3) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y, 2, 1);
        test_cond("sdsrange(...,2,1)", sdslen(y) == 0 && memcmp(y, "\0", 1) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y, 1, 100);
        test_cond("sdsrange(...,1,100)", sdslen(y) == 3 && memcmp(y, "iao\0", 4) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y, 100, 100);
        test_cond("sdsrange(...,100,100)", sdslen(y) == 0 && memcmp(y, "\0", 1) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y, 4, 6);
        test_cond("sdsrange(...,4,6)", sdslen(y) == 0 && memcmp(y, "\0", 1) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y, 3, 6);
        test_cond("sdsrange(...,3,6)", sdslen(y) == 1 && memcmp(y, "o\0", 2) == 0);

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("foo");
        y = sdsnew("foa");
        test_cond("sdscmp(foo,foa)", sdscmp(x, y) > 0);

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("bar");
        y = sdsnew("bar");
        test_cond("sdscmp(bar,bar)", sdscmp(x, y) == 0);

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("aar");
        y = sdsnew("bar");
        test_cond("sdscmp(bar,bar)", sdscmp(x, y) < 0);

        sdsfree(y);
        sdsfree(x);
        x = sdsnewlen("\a\n\0foo\r", 7);
        y = sdscatrepr(sdsempty(), x, sdslen(x));
        test_cond("sdscatrepr(...data...)", memcmp(y, "\"\\a\\n\\x00foo\\r\"", 15) == 0);

        {
            unsigned int oldfree;
            char *p;
            int i;
            size_t step = 10, j;

            sdsfree(x);
            sdsfree(y);
            x = sdsnew("0");
            test_cond("sdsnew() free/len buffers", sdslen(x) == 1 && sdsavail(x) == 0);

            /* Run the test a few times in order to hit the first two
             * SDS header types. */
            for (i = 0; i < 10; i++) {
                size_t oldlen = sdslen(x);
                x = sdsMakeRoomFor(x, step);
                int type = x[-1] & SDS_TYPE_MASK;

                test_cond("sdsMakeRoomFor() len", sdslen(x) == oldlen);
                if (type != SDS_TYPE_5) {
                    test_cond("sdsMakeRoomFor() free", sdsavail(x) >= step);
                    oldfree = sdsavail(x);
                    UNUSED(oldfree);
                }
                p = x + oldlen;
                for (j = 0; j < step; j++) {
                    p[j] = 'A' + j;
                }
                sdsIncrLen(x, step);
            }
            test_cond("sdsMakeRoomFor() content", memcmp("0ABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJ", x, 101) == 0);
            test_cond("sdsMakeRoomFor() final length", sdslen(x) == 101);

            sdsfree(x);
        }

        /* Simple template */
        x = sdstemplate("v1={variable1} v2={variable2}", sdsTestTemplateCallback, NULL);
        test_cond("sdstemplate() normal flow", memcmp(x, "v1=value1 v2=value2", 19) == 0);
        sdsfree(x);

        /* Template with callback error */
        x = sdstemplate("v1={variable1} v3={doesnotexist}", sdsTestTemplateCallback, NULL);
        test_cond("sdstemplate() with callback error", x == NULL);

        /* Template with empty var name */
        x = sdstemplate("v1={", sdsTestTemplateCallback, NULL);
        test_cond("sdstemplate() with empty var name", x == NULL);

        /* Template with truncated var name */
        x = sdstemplate("v1={start", sdsTestTemplateCallback, NULL);
        test_cond("sdstemplate() with truncated var name", x == NULL);

        /* Template with quoting */
        x = sdstemplate("v1={{{variable1}} {{} v2={variable2}", sdsTestTemplateCallback, NULL);
        test_cond("sdstemplate() with quoting", memcmp(x, "v1={value1} {} v2=value2", 24) == 0);
        sdsfree(x);

        /* Test sdsresize - extend */
        x = sdsnew("1234567890123456789012345678901234567890");
        x = sdsResize(x, 200);
        test_cond("sdsrezie() expand len", sdslen(x) == 40);
        test_cond("sdsrezie() expand strlen", strlen(x) == 40);
        test_cond("sdsrezie() expand alloc", sdsalloc(x) == 200);
        /* Test sdsresize - trim free space */
        x = sdsResize(x, 80);
        test_cond("sdsrezie() shrink len", sdslen(x) == 40);
        test_cond("sdsrezie() shrink strlen", strlen(x) == 40);
        test_cond("sdsrezie() shrink alloc", sdsalloc(x) == 80);
        /* Test sdsresize - crop used space */
        x = sdsResize(x, 30);
        test_cond("sdsrezie() crop len", sdslen(x) == 30);
        test_cond("sdsrezie() crop strlen", strlen(x) == 30);
        test_cond("sdsrezie() crop alloc", sdsalloc(x) == 30);
        /* Test sdsresize - extend to different class */
        x = sdsResize(x, 400);
        test_cond("sdsrezie() expand len", sdslen(x) == 30);
        test_cond("sdsrezie() expand strlen", strlen(x) == 30);
        test_cond("sdsrezie() expand alloc", sdsalloc(x) == 400);
        /* Test sdsresize - shrink to different class */
        x = sdsResize(x, 4);
        test_cond("sdsrezie() crop len", sdslen(x) == 4);
        test_cond("sdsrezie() crop strlen", strlen(x) == 4);
        test_cond("sdsrezie() crop alloc", sdsalloc(x) == 4);
        sdsfree(x);
    }
    return 0;
}
#endif
