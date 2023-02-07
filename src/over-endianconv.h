#ifndef __ENDIANCONV_H
#define __ENDIANCONV_H

#include "over-config.h"
#include <stdint.h>

void memrev16(void *p);

void memrev32(void *p);

void memrev64(void *p);

uint16_t intrev16(uint16_t v);

uint32_t intrev32(uint32_t v);

uint64_t intrev64(uint64_t v);

// 只有当目标主机为大端时,函数的变体才执行实际转换
#if (BYTE_ORDER == LITTLE_ENDIAN)
#    define memrev16ifbe(p) ((void)(0))
#    define memrev32ifbe(p) ((void)(0))
#    define memrev64ifbe(p) ((void)(0))
#    define intrev16ifbe(v) (v)
#    define intrev32ifbe(v) (v)
#    define intrev64ifbe(v) (v)
#else
#    define memrev16ifbe(p) memrev16(p)
#    define memrev32ifbe(p) memrev32(p)
#    define memrev64ifbe(p) memrev64(p)
#    define intrev16ifbe(v) intrev16(v)
#    define intrev32ifbe(v) intrev32(v)
#    define intrev64ifbe(v) intrev64(v)
#endif

// 函数htonu64()和ntohu64()将指定的值转换为网络字节顺序和反向。在大型终端系统中，它们是无操作的。
#if (BYTE_ORDER == BIG_ENDIAN)
#    define htonu64(v) (v)
#    define ntohu64(v) (v)
#else
#    define htonu64(v) intrev64(v)
#    define ntohu64(v) intrev64(v)
#endif

#ifdef REDIS_TEST
int endianconvTest(int argc, char *argv[], int flags);
#endif

#endif
