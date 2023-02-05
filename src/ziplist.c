/*
 * Ziplist 是为了尽可能地节约内存而设计的特殊编码双端链表.
 * Ziplist 可以储存字符串值和整数值,
 * 其中,整数值被保存为实际的整数,而不是字符数组.
 * Ziplist 允许在列表的两端进行 O(1) 复杂度的 push 和 pop 操作.
 * 但是,因为这些操作都需要对整个 ziplist 进行内存重分配,
 * 所以实际的复杂度和 ziplist 占用的内存大小有关.
 *
 * ----------------------------------------------------------------------------
 *
 * Ziplist 的整体布局：
 * ======================
 * 以下是 ziplist 的一般布局：
 *
 * <zlbytes> <zltail> <zllen> <entry> <entry> ... <entry> <zlend>
 *
 * NOTE: 小端存储
 *
 * <uint32_t zlbytes> 四字节,是一个无符号整数,保存着ziplist使用的内存字节数  在对压缩列表进行内存重分配,或者计算zlend的位置时使用
 * 通过这个值,程序可以直接对 ziplist 的内存大小进行调整,而无须为了计算 ziplist 的内存大小而遍历整个列表.
 *
 * <uint32_t zltail> 记录压缩列表表尾节点距离压缩列表的起始地址有多少字节.
 * 这个偏移量使得对表尾的 pop 操作可以在无须遍历整个列表的情况下进行.
 *
 * <uint16_t zllen>  保存着列表中的节点数量.节点的长度由节点保存的内容决定
 * 当 zllen 保存的值大于 2**16-2 时,程序需要遍历整个列表才能知道列表实际包含了多少个节点.
 *
 * <uint8_t zlend> 的长度为 1 字节,值为 255 ,用于标识列表的末尾.
 *
 * ZIPLIST 节点：
 * ===============
 *
 * 每个 ziplist 节点的前面都带有一个 header ,这个 header 包含两部分信息：
 *
 * 1)前置节点的长度,在程序从后向前遍历时使用.
 * 2)当前节点所保存的值的类型和长度.
 *
 * <prevlen> <encoding> <entry-data>
 *
 * So practically an entry is encoded in the following way:
 *
 * <prevlen from 0 to 253> <encoding> <entry>
 * 编码前置节点的长度的方法如下：
 *
 * 1) 如果前置节点的长度小于 254 字节,那么程序将使用 1 个字节来保存这个长度值.
 *
 * 2) 如果前置节点的长度大于等于 254 字节,那么程序将使用 5 个字节来保存这个长度值：
 *    a) 第 1 个字节的值将被设为 254 ,用于标识这是一个 5 字节长的长度值.
 *    b) 之后的 4 个字节则用于保存前置节点的实际长度.
 *
 * header 另一部分的内容和节点所保存的值有关.
 *
 * 1) 如果节点保存的是字符串值,
 *    那么这部分 header 的头 2 个位将保存编码字符串长度所使用的类型,
 *    而之后跟着的内容则是字符串的实际长度.
 *
 * |00pppppp| - 1 byte
 *     字符串的长度小于或等于 63 字节.
 * |01pppppp|qqqqqqqq| - 2 bytes
 *      字符串的长度小于或等于 16383 字节.
 * |10000000|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt| - 5 bytes
 *      字符串的长度大于或等于 16384 字节.
 *      IMPORTANT: The 32 bit number is stored in big endian.
 * 2) 如果节点保存的是整数值,
 *    那么这部分 header 的头 2 位都将被设置为 1 ,
 *    而之后跟着的 2 位则用于标识节点所保存的整数的类型.
 * |11000000| - 3 bytes
 *      节点的值为 int16_t 类型的整数,长度为 2 字节.
 * |11010000| - 5 bytes
 *      节点的值为 int32_t 类型的整数,长度为 4 字节.
 * |11100000| - 9 bytes
 *      节点的值为 int64_t 类型的整数,长度为 8 字节.
 * |11110000| - 4 bytes
 *      节点的值为 24 位（3 字节）长的整数.
 * |11111110| - 2 bytes
 *       节点的值为 8 位（1 字节）长的整数.
 * |1111xxxx| - (with xxxx between 0001 and 1101) immediate 4 bit integer.
 *      节点的值为介于 0 至 12 之间的无符号整数.
 *      因为 0000 和 1111 都不能使用,所以位的实际值将是 1 至 13 .
 *      程序在取得这 4 个位的值之后,还需要减去 1 ,才能计算出正确的值.
 *      比如说,如果位的值为 0001 = 1 ,那么程序返回的值将是 1 - 1 = 0 .
 * |11111111| - ziplist 的结尾标识
 *
 * Like for the ziplist header, all the integers are represented in little
 * endian byte order, even when this code is compiled in big endian systems.
 *
 * EXAMPLES OF ACTUAL ZIPLISTS
 * ===========================
 *
 * The following is a ziplist containing the two elements representing
 * the strings "2" and "5". It is composed of 15 bytes, that we visually
 * split into sections:
 *
 *  [0f 00 00 00] [0c 00 00 00] [02 00] [00 f3] [02 f6] [ff]
 *        |             |          |       |       |     |
 *     zlbytes        zltail    entries   "2"     "5"   end
 *
 */
// 当一个列表键只包含 少量列表项、并且每个列表项要么是小整数值,要么是长度比较短的字符串

/*
空白 ziplist 示例图
area        |<---- ziplist header ---->|<-- end -->|
size          4 bytes   4 bytes 2 bytes  1 byte
            +---------+--------+-------+-----------+
component   | zlbytes | zltail | zllen | zlend     |
            |         |        |       |           |
value       |  1011   |  1010  |   0   | 1111 1111 |
            +---------+--------+-------+-----------+
                                       ^
                                       |
                               ZIPLIST_ENTRY_HEAD
                                       &
address                        ZIPLIST_ENTRY_TAIL
                                       &
                               ZIPLIST_ENTRY_END
非空 ziplist 示例图
area        |<---- ziplist header ---->|<----------- entries ------------->|<-end->|
size          4 bytes  4 bytes  2 bytes    ?        ?        ?        ?     1 byte
            +---------+--------+-------+--------+--------+--------+--------+-------+
component   | zlbytes | zltail | zllen | entry1 | entry2 |  ...   | entryN | zlend |
            +---------+--------+-------+--------+--------+--------+--------+-------+
                                       ^                          ^        ^
address                                |                          |        |
                                ZIPLIST_ENTRY_HEAD                |   ZIPLIST_ENTRY_END
                                                                  |
                                                        ZIPLIST_ENTRY_TAIL
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include "over-zmalloc.h"
#include "util.h"
#include "ziplist.h"
#include "config.h"
#include "over-endianconv.h"
#include "redisassert.h"

#define ZIP_END 255         // ziplist的列表尾字节内容
#define ZIP_BIG_PREVLEN 254 // 特殊的 "zipliZIP_BIG_PREVLEN的结束 - 1是前一条的最大字节数. 前一个条目的最大字节数,为 "prevlen "字段的前缀. 每个条目,只用一个字节来表示. 否则,它表示为FE AA BB CC DD,其中           AA BB CC DD是一个4字节的无符号整数    代表前一个条目的长度."St "条目.

/* Different encoding/length possibilities */
// 字符串编码和整数编码的掩码
#define ZIP_STR_MASK 0xc0
#define ZIP_INT_MASK 0x30

//  * 字符串编码类型
#define ZIP_STR_06B (0 << 6)
#define ZIP_STR_14B (1 << 6)
#define ZIP_STR_32B (2 << 6)
// 整数编码类型
#define ZIP_INT_16B (0xc0 | 0 << 4)
#define ZIP_INT_32B (0xc0 | 1 << 4)
#define ZIP_INT_64B (0xc0 | 2 << 4)
#define ZIP_INT_24B (0xc0 | 3 << 4)
#define ZIP_INT_8B 0xfe

/* 4 bit integer immediate encoding |1111xxxx| with xxxx between
 * 0001 and 1101. */
// 4 位整数编码的掩码和类型
#define ZIP_INT_IMM_MASK 0x0f /* Mask to extract the 4 bits value. To add one is needed to reconstruct the value. */
#define ZIP_INT_IMM_MIN 0xf1  /* 11110001 */
#define ZIP_INT_IMM_MAX 0xfd  /* 11111101 */

// 24 位整数的最大值和最小值
#define INT24_MAX 0x7fffff
#define INT24_MIN (-INT24_MAX - 1)

//  查看给定编码 enc 是否字符串编码
#define ZIP_IS_STR(enc) (((enc)&ZIP_STR_MASK) < ZIP_STR_MASK)

// 获取zl前4个字节代表的总字节数
#define ZIPLIST_BYTES(zl) (*((uint32_t *)(zl)))

/* Return the offset of the last item inside the ziplist. */
// 定位到 ziplist 的 offset 属性,该属性记录了到达表尾节点的偏移量
// 用于取出 offset 属性的现有值,或者为 offset 属性赋予新值
#define ZIPLIST_TAIL_OFFSET(zl) (*((uint32_t *)((zl) + sizeof(uint32_t))))

// 定位到 ziplist 的 length 属性,该属性记录了 ziplist 包含的节点数量
// 用于取出 length 属性的现有值,或者为 length 属性赋予新值
#define ZIPLIST_LENGTH(zl) (*((uint16_t *)((zl) + sizeof(uint32_t) * 2)))

// ziplist的列表头大小,包括2个32 bits整数和1个16bits整数,分别表示压缩列表的总字节数,列表最后一个元素的离列表头的偏移,以及列表中的元素个数
#define ZIPLIST_HEADER_SIZE (sizeof(uint32_t) * 2 + sizeof(uint16_t))
// ziplist的列表尾大小,包括1个8 bits整数,表示列表结束.
#define ZIPLIST_END_SIZE (sizeof(uint8_t))

/* Return the pointer to the first entry of a ziplist. */
// 返回指向 ziplist 第一个节点（的起始位置）的指针
#define ZIPLIST_ENTRY_HEAD(zl) ((zl) + ZIPLIST_HEADER_SIZE)

/* Return the pointer to the last entry of a ziplist, using the
 * last entry offset inside the ziplist header. */
// 返回指向 ziplist 最后一个节点（的起始位置）的指针

#define ZIPLIST_ENTRY_TAIL(zl) ((zl) + intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)))

/* Return the pointer to the last byte of a ziplist, which is, the
 * end of ziplist FF entry. */
// 返回指向 ziplist 末端 ZIP_END （的起始位置）的指针
#define ZIPLIST_ENTRY_END(zl) ((zl) + intrev32ifbe(ZIPLIST_BYTES(zl)) - 1)

// 增加 ziplist 的节点数
#define ZIPLIST_INCR_LENGTH(zl, incr)                                                   \
    {                                                                                   \
        if (intrev16ifbe(ZIPLIST_LENGTH(zl)) < UINT16_MAX)                              \
            ZIPLIST_LENGTH(zl) = intrev16ifbe(intrev16ifbe(ZIPLIST_LENGTH(zl)) + incr); \
    }

/* Don't let ziplists grow over 1GB in any case, don't wanna risk overflow in
 * zlbytes */
#define ZIPLIST_MAX_SAFETY_SIZE (1 << 30)

int ziplistSafeToAdd(unsigned char *zl, size_t add) {
    size_t len = zl ? ziplistBlobLen(zl) : 0;
    if (len + add > ZIPLIST_MAX_SAFETY_SIZE)
        return 0;
    return 1;
}

// 保存 ziplist 节点信息的结构
typedef struct zlentry {
    unsigned int prevrawlensize; // prevrawlensize ：编码 prevrawlen 所需的字节大小
    unsigned int prevrawlen;     // prevrawlen ：前置节点的长度    如果<254 则使用1byte ;>=254 之后四个字节用于保存前一节点的长度
    unsigned int lensize;        // lensize ：编码 len 所需的字节大小
    unsigned int len;            // len ：当前节点值的长度
    unsigned int headersize;     // 当前节点 header 的大小    等于 prevrawlensize + lensize
    unsigned char encoding;      // 当前节点值所使用的编码类型,以及长度
                                 // 1、2、5字节 ,值得最高位是00、01、或者10的是字节数组编码：这种编码表示节点的content属性保存着字节数组,数组的长度由编码出去最高两位之后的其它位记录
                                 // 1字节,值得最高位以11开头的是整数编码：这种编码表示节点的content属性保存着整数值,整数值的类型和长度由编码除去最高两位之后的其他位记录
    unsigned char *p;            // 指向当前节点真实数据的指针
} zlentry;
// 缺陷：
//     连锁更新一旦发生,就会导致压缩列表占用的内存空间要多次重新分配,这就会直接影响到压缩列表的访问性能
// 优点：
//     内存紧凑

#define ZIPLIST_ENTRY_ZERO(zle)                              \
    {                                                        \
        (zle)->prevrawlensize = (zle)->prevrawlen = 0;       \
        (zle)->lensize = (zle)->len = (zle)->headersize = 0; \
        (zle)->encoding = 0;                                 \
        (zle)->p = NULL;                                     \
    }

/* Extract the encoding from the byte pointed by 'ptr' and set it into
 * 'encoding' field of the zlentry structure. */
// 从 ptr 中取出节点值的编码类型,并将它保存到 encoding 变量中.
#define ZIP_ENTRY_ENCODING(ptr, encoding) \
    do {                                  \
        (encoding) = ((ptr)[0]);          \
        if ((encoding) < ZIP_STR_MASK)    \
            (encoding) &= ZIP_STR_MASK;   \
    } while (0)

#define ZIP_ENCODING_SIZE_INVALID 0xff

/* Return the number of bytes required to encode the entry type + length.
 * On error, return ZIP_ENCODING_SIZE_INVALID */
static inline unsigned int zipEncodingLenSize(unsigned char encoding) {
    if (encoding == ZIP_INT_16B || encoding == ZIP_INT_32B || encoding == ZIP_INT_24B || encoding == ZIP_INT_64B || encoding == ZIP_INT_8B)
        return 1;
    if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX)
        return 1;
    if (encoding == ZIP_STR_06B)
        return 1;
    if (encoding == ZIP_STR_14B)
        return 2;
    if (encoding == ZIP_STR_32B)
        return 5;
    return ZIP_ENCODING_SIZE_INVALID;
}

#define ZIP_ASSERT_ENCODING(encoding)                                      \
    do {                                                                   \
        assert(zipEncodingLenSize(encoding) != ZIP_ENCODING_SIZE_INVALID); \
    } while (0)

// 返回保存 encoding 编码的值所需的字节数量
static inline unsigned int zipIntSize(unsigned char encoding) {
    switch (encoding) {
        case ZIP_INT_8B:
            return 1;
        case ZIP_INT_16B:
            return 2;
        case ZIP_INT_24B:
            return 3;
        case ZIP_INT_32B:
            return 4;
        case ZIP_INT_64B:
            return 8;
    }
    if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX)
        return 0; /* 4 bit immediate */
    /* bad encoding, covered by a previous call to ZIP_ASSERT_ENCODING */
    redis_unreachable();
    return 0;
}

// 针对整数和字符串,就分别使用了不同字节长度的编码结果
unsigned int zipStoreEntryEncoding(unsigned char *p, unsigned char encoding, unsigned int rawlen) {
    // 对于字符串数据,只是记录了字符串本身的长度,
    // 还会调用 zipStoreEntryEncoding 函数,根据字符串的长度来计算相应的 encoding 大小,如下所示：
    unsigned char len = 1, buf[5];
    // 如果是字符串数据
    if (ZIP_IS_STR(encoding)) {
        if (rawlen <= 0x3f) { // 字符串长度小于等于63字节（16进制为0x3f）
            // 默认编码结果是1字节
            if (!p)
                return len;
            buf[0] = ZIP_STR_06B | rawlen;
        }
        else if (rawlen <= 0x3fff) { // 字符串长度小于等于16383字节（16进制为0x3fff）
            len += 1;                // 2个字节
            if (!p)
                return len;
            buf[0] = ZIP_STR_14B | ((rawlen >> 8) & 0x3f);
            buf[1] = rawlen & 0xff;
        }
        else {        // 字符串长度大于16383字节
            len += 4; // 编码结果是5字节
            if (!p)
                return len;
            buf[0] = ZIP_STR_32B;
            buf[1] = (rawlen >> 24) & 0xff;
            buf[2] = (rawlen >> 16) & 0xff;
            buf[3] = (rawlen >> 8) & 0xff;
            buf[4] = rawlen & 0xff;
        }
    }
    else { /* 如果数据是整数,编码结果是1字节*/
        if (!p)
            return len;
        buf[0] = encoding;
    }

    memcpy(p, buf, len); // 将编码后的长度写入 p
    return len;          // 返回编码所需的字节数
}

/* Decode the entry encoding type and data length (string length for strings,
 * number of bytes used for the integer for integer entries) encoded in 'ptr'.
 * The 'encoding' variable is input, extracted by the caller, the 'lensize'
 * variable will hold the number of bytes required to encode the entry
 * length, and the 'len' variable will hold the entry length.
 * On invalid encoding error, lensize is set to 0. */

/* Decode the length encoded in 'ptr'. The 'encoding' variable will hold the
 * entries encoding, the 'lensize' variable will hold the number of bytes
 * required to encode the entries length, and the 'len' variable will hold the
 * entries length.
 *
 * 解码 ptr 指针,取出列表节点的相关信息,并将它们保存在以下变量中：
 *
 * - encoding 保存节点值的编码类型.
 *
 * - lensize 保存编码节点长度所需的字节数.
 *
 * - len 保存节点的长度.
 *
 * T = O(1)
 */
#define ZIP_DECODE_LENGTH(ptr, encoding, lensize, len)                                                                              \
    do {                                                                                                                            \
        if ((encoding) < ZIP_STR_MASK) { /* 取出值的编码类型 */                                                             \
            if ((encoding) == ZIP_STR_06B) {                                                                                        \
                (lensize) = 1;                                                                                                      \
                (len) = (ptr)[0] & 0x3f;                                                                                            \
            }                                                                                                                       \
            else if ((encoding) == ZIP_STR_14B) {                                                                                   \
                (lensize) = 2;                                                                                                      \
                (len) = (((ptr)[0] & 0x3f) << 8) | (ptr)[1];                                                                        \
            }                                                                                                                       \
            else if ((encoding) == ZIP_STR_32B) {                                                                                   \
                (lensize) = 5;                                                                                                      \
                (len) = ((uint32_t)(ptr)[1] << 24) | ((uint32_t)(ptr)[2] << 16) | ((uint32_t)(ptr)[3] << 8) | ((uint32_t)(ptr)[4]); \
            }                                                                                                                       \
            else {                                                                                                                  \
                (lensize) = 0; /* bad encoding, should be covered by a previous */                                                  \
                (len) = 0;     /* ZIP_ASSERT_ENCODING / zipEncodingLenSize, or  */                                                  \
                               /* match the lensize after this macro with 0.    */                                                  \
            }                                                                                                                       \
        }                                                                                                                           \
        else { /* 整数编码 */                                                                                                   \
            (lensize) = 1;                                                                                                          \
            if ((encoding) == ZIP_INT_8B)                                                                                           \
                (len) = 1;                                                                                                          \
            else if ((encoding) == ZIP_INT_16B)                                                                                     \
                (len) = 2;                                                                                                          \
            else if ((encoding) == ZIP_INT_24B)                                                                                     \
                (len) = 3;                                                                                                          \
            else if ((encoding) == ZIP_INT_32B)                                                                                     \
                (len) = 4;                                                                                                          \
            else if ((encoding) == ZIP_INT_64B)                                                                                     \
                (len) = 8;                                                                                                          \
            else if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX)                                                    \
                (len) = 0; /* 4 bit immediate */                                                                                    \
            else                                                                                                                    \
                (lensize) = (len) = 0; /* bad encoding */                                                                           \
        }                                                                                                                           \
    } while (0)

// 将原本只需要 1 个字节来保存的前置节点长度 len 编码至一个 5 字节长的 header 中.
int zipStorePrevEntryLengthLarge(unsigned char *p, unsigned int len) {
    uint32_t u32;
    if (p != NULL) {
        // 11111110
        p[0] = ZIP_BIG_PREVLEN;           // 将prevlen的第1字节设置为ZIP_BIG_PREVLEN,即254
        u32 = len;                        //
        memcpy(p + 1, &u32, sizeof(u32)); // 将前一个列表项的长度值拷贝至prevlen的第2至第5字节,其中sizeof(len)的值为4
        memrev32ifbe(p + 1);              // 如果有必要的话,进行大小端转换
    }
    return 1 + sizeof(uint32_t); // 返回prevlen的大小,为5字节
}

// 避免prevlen空间浪费,由实际存储的数据长度决定使用多大空间
unsigned int zipStorePrevEntryLength(unsigned char *p, unsigned int len) {
    if (p == NULL) {
        // 判断prevlen的长度是否小于ZIP_BIG_PREVLEN,ZIP_BIG_PREVLEN等于254
        return (len < ZIP_BIG_PREVLEN) ? 1 : sizeof(uint32_t) + 1;
    }
    else {
        if (len < ZIP_BIG_PREVLEN) {
            p[0] = len;
            return 1;
        }
        else {
            // 否则,调用zipStorePrevEntryLengthLarge进行编码
            return zipStorePrevEntryLengthLarge(p, len);
        }
    }
}

/* Return the number of bytes used to encode the length of the previous
 * entry. The length is returned by setting the var 'prevlensize'. */
// 解码 ptr 指针,
// 取出编码前置节点长度所需的字节数,并将它保存到 prevlensize 变量中.
#define ZIP_DECODE_PREVLENSIZE(ptr, prevlensize) \
    do {                                         \
        if ((ptr)[0] < ZIP_BIG_PREVLEN) {        \
            (prevlensize) = 1;                   \
        }                                        \
        else {                                   \
            (prevlensize) = 5;                   \
        }                                        \
    } while (0)

/* Return the length of the previous element, and the number of bytes that
 * are used in order to encode the previous element length.
 * 'ptr' must point to the prevlen prefix of an entry (that encodes the
 * length of the previous entry in order to navigate the elements backward).
 * The length of the previous entry is stored in 'prevlen', the number of
 * bytes needed to encode the previous entry length are stored in
 * 'prevlensize'. */
// 解码 ptr 指针,
// 取出编码前置节点长度所需的字节数,
// 并将这个字节数保存到 prevlensize 中.
// *
// 然后根据 prevlensize ,从 ptr 中取出前置节点的长度值,
// 并将这个长度值保存到 prevlen 变量中.
#define ZIP_DECODE_PREVLEN(ptr, prevlensize, prevlen)                                              \
    do {                                                                                           \
        ZIP_DECODE_PREVLENSIZE(ptr, prevlensize); /* 先计算被编码长度值的字节数 */    \
        if ((prevlensize) == 1) {                 /* 再根据编码字节数来取出长度值 */ \
                                                                                                   \
            (prevlen) = (ptr)[0];                                                                  \
        }                                                                                          \
        else { /* prevlensize == 5 */                                                              \
            (prevlen) = ((ptr)[4] << 24) | ((ptr)[3] << 16) | ((ptr)[2] << 8) | ((ptr)[1]);        \
        }                                                                                          \
    } while (0)

/* Given a pointer 'p' to the prevlen info that prefixes an entry, this
 * function returns the difference in number of bytes needed to encode
 * the prevlen if the previous entry changes of size.
 *
 * So if A is the number of bytes used right now to encode the 'prevlen'
 * field.
 *
 * And B is the number of bytes that are needed in order to encode the
 * 'prevlen' if the previous element will be updated to one of size 'len'.
 *
 * Then the function returns B - A
 *
 * So the function returns a positive number if more space is needed,
 * a negative number if less space is needed, or zero if the same space
 * is needed. */
// 计算编码新的前置节点长度 len 所需的字节数,
// 减去编码 p 原来的前置节点长度所需的字节数之差.
int zipPrevLenByteDiff(unsigned char *p, unsigned int len) {
    unsigned int prevlensize;
    ZIP_DECODE_PREVLENSIZE(p, prevlensize); // 取出编码原来的前置节点长度所需的字节数

    return zipStorePrevEntryLength(NULL, len) - prevlensize; // 计算编码 len 所需的字节数,然后进行减法运算
}

// 检查 entry 中指向的字符串能否被编码为整数.
// 如果可以的话,将编码后的整数保存在指针 v 的值中,并将编码的方式保存在指针 encoding 的值中.
// 注意,这里的 entry 和前面代表节点的 entry 不是一个意思.
// T = O(N)
int zipTryEncoding(unsigned char *entry, unsigned int entrylen, long long *v, unsigned char *encoding) {
    long long value;
    // 忽略太长或太短的字符串

    if (entrylen >= 32 || entrylen == 0)
        return 0;

    // 尝试转换
    // T = O(N)
    if (string2ll((char *)entry, entrylen, &value)) {
        /* Great, the string can be encoded. Check what's the smallest
         * of our encoding types that can hold this value. */
        // 转换成功,以从小到大的顺序检查适合值 value 的编码方式
        if (value >= 0 && value <= 12) {
            *encoding = ZIP_INT_IMM_MIN + value;
        }
        else if (value >= INT8_MIN && value <= INT8_MAX) {
            *encoding = ZIP_INT_8B;
        }
        else if (value >= INT16_MIN && value <= INT16_MAX) {
            *encoding = ZIP_INT_16B;
        }
        else if (value >= INT24_MIN && value <= INT24_MAX) {
            *encoding = ZIP_INT_24B;
        }
        else if (value >= INT32_MIN && value <= INT32_MAX) {
            *encoding = ZIP_INT_32B;
        }
        else {
            *encoding = ZIP_INT_64B;
        }
        // 记录值到指针
        *v = value;
        return 1; // 返回转换成功标识
    }
    return 0; // 转换失败
}

/* Store integer 'value' at 'p', encoded as 'encoding' */
// 以 encoding 指定的编码方式,将整数值 value 写入到 p .
void zipSaveInteger(unsigned char *p, int64_t value, unsigned char encoding) {
    int16_t i16;
    int32_t i32;
    int64_t i64;
    if (encoding == ZIP_INT_8B) {
        ((int8_t *)p)[0] = (int8_t)value;
    }
    else if (encoding == ZIP_INT_16B) {
        i16 = value;
        memcpy(p, &i16, sizeof(i16));
        memrev16ifbe(p);
    }
    else if (encoding == ZIP_INT_24B) {
        i32 = ((uint64_t)value) << 8;
        memrev32ifbe(&i32);
        memcpy(p, ((uint8_t *)&i32) + 1, sizeof(i32) - sizeof(uint8_t));
    }
    else if (encoding == ZIP_INT_32B) {
        i32 = value;
        memcpy(p, &i32, sizeof(i32));
        memrev32ifbe(p);
    }
    else if (encoding == ZIP_INT_64B) {
        i64 = value;
        memcpy(p, &i64, sizeof(i64));
        memrev64ifbe(p);
    }
    else if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX) { /* Nothing to do, the value is stored in the encoding itself. */
    }
    else {
        assert(NULL);
    }
}

/* Read integer encoded as 'encoding' from 'p' */
// 以 encoding 指定的编码方式,读取并返回指针 p 中的整数值.
int64_t zipLoadInteger(unsigned char *p, unsigned char encoding) {
    int16_t i16;
    int32_t i32;
    int64_t i64, ret = 0;
    if (encoding == ZIP_INT_8B) {
        ret = ((int8_t *)p)[0];
    }
    else if (encoding == ZIP_INT_16B) {
        memcpy(&i16, p, sizeof(i16));
        memrev16ifbe(&i16);
        ret = i16;
    }
    else if (encoding == ZIP_INT_32B) {
        memcpy(&i32, p, sizeof(i32));
        memrev32ifbe(&i32);
        ret = i32;
    }
    else if (encoding == ZIP_INT_24B) {
        i32 = 0;
        memcpy(((uint8_t *)&i32) + 1, p, sizeof(i32) - sizeof(uint8_t));
        memrev32ifbe(&i32);
        ret = i32 >> 8;
    }
    else if (encoding == ZIP_INT_64B) {
        memcpy(&i64, p, sizeof(i64));
        memrev64ifbe(&i64);
        ret = i64;
    }
    else if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX) {
        ret = (encoding & ZIP_INT_IMM_MASK) - 1;
    }
    else {
        assert(NULL);
    }
    return ret;
}

/* Fills a struct with all information about an entry.
 * This function is the "unsafe" alternative to the one below.
 * Generally, all function that return a pointer to an element in the ziplist
 * will assert that this element is valid, so it can be freely used.
 * Generally functions such ziplistGet assume the input pointer is already
 * validated (since it's the return value of another function). */
// 将 p 所指向的列表节点的信息全部保存到 zlentry 中,并返回该 zlentry .
static inline void zipEntry(unsigned char *p, zlentry *e) {
    // e.prevrawlensize 保存着编码前一个节点的长度所需的字节数
    // e.prevrawlen 保存着前一个节点的长度
    // T = O(1)
    ZIP_DECODE_PREVLEN(p, e->prevrawlensize, e->prevrawlen);
    ZIP_ENTRY_ENCODING(p + e->prevrawlensize, e->encoding);
    // p + e.prevrawlensize 将指针移动到列表节点本身
    // e.encoding 保存着节点值的编码类型
    // e.lensize 保存着编码节点值长度所需的字节数
    // e.len 保存着节点值的长度
    // T = O(1)
    ZIP_DECODE_LENGTH(p + e->prevrawlensize, e->encoding, e->lensize, e->len);
    assert(e->lensize != 0); /* check that encoding was valid. */

    // 计算头结点的字节数
    e->headersize = e->prevrawlensize + e->lensize;
    // 记录指针

    e->p = p;
}

/* Fills a struct with all information about an entry.
 * This function is safe to use on untrusted pointers, it'll make sure not to
 * try to access memory outside the ziplist payload.
 * Returns 1 if the entry is valid, and 0 otherwise. */
static inline int zipEntrySafe(unsigned char *zl, size_t zlbytes, unsigned char *p, zlentry *e, int validate_prevlen) {
    unsigned char *zlfirst = zl + ZIPLIST_HEADER_SIZE;
    unsigned char *zllast = zl + zlbytes - ZIPLIST_END_SIZE;
#define OUT_OF_RANGE(p) (unlikely((p) < zlfirst || (p) > zllast))

    /* If there's no possibility for the header to reach outside the ziplist,
     * take the fast path. (max lensize and prevrawlensize are both 5 bytes) */
    if (p >= zlfirst && p + 10 < zllast) {
        ZIP_DECODE_PREVLEN(p, e->prevrawlensize, e->prevrawlen);
        ZIP_ENTRY_ENCODING(p + e->prevrawlensize, e->encoding);
        ZIP_DECODE_LENGTH(p + e->prevrawlensize, e->encoding, e->lensize, e->len);
        e->headersize = e->prevrawlensize + e->lensize;
        e->p = p;
        /* We didn't call ZIP_ASSERT_ENCODING, so we check lensize was set to 0. */
        if (unlikely(e->lensize == 0))
            return 0;
        /* Make sure the entry doesn't reach outside the edge of the ziplist */
        if (OUT_OF_RANGE(p + e->headersize + e->len))
            return 0;
        /* Make sure prevlen doesn't reach outside the edge of the ziplist */
        if (validate_prevlen && OUT_OF_RANGE(p - e->prevrawlen))
            return 0;
        return 1;
    }

    /* Make sure the pointer doesn't reach outside the edge of the ziplist */
    if (OUT_OF_RANGE(p))
        return 0;

    /* Make sure the encoded prevlen header doesn't reach outside the allocation */
    ZIP_DECODE_PREVLENSIZE(p, e->prevrawlensize);
    if (OUT_OF_RANGE(p + e->prevrawlensize))
        return 0;

    /* Make sure encoded entry header is valid. */
    ZIP_ENTRY_ENCODING(p + e->prevrawlensize, e->encoding);
    e->lensize = zipEncodingLenSize(e->encoding);
    if (unlikely(e->lensize == ZIP_ENCODING_SIZE_INVALID))
        return 0;

    /* Make sure the encoded entry header doesn't reach outside the allocation */
    if (OUT_OF_RANGE(p + e->prevrawlensize + e->lensize))
        return 0;

    /* Decode the prevlen and entry len headers. */
    ZIP_DECODE_PREVLEN(p, e->prevrawlensize, e->prevrawlen);
    ZIP_DECODE_LENGTH(p + e->prevrawlensize, e->encoding, e->lensize, e->len);
    e->headersize = e->prevrawlensize + e->lensize;

    /* Make sure the entry doesn't reach outside the edge of the ziplist */
    if (OUT_OF_RANGE(p + e->headersize + e->len))
        return 0;

    /* Make sure prevlen doesn't reach outside the edge of the ziplist */
    if (validate_prevlen && OUT_OF_RANGE(p - e->prevrawlen))
        return 0;

    e->p = p;
    return 1;
#undef OUT_OF_RANGE
}

/* Return the total number of bytes used by the entry pointed to by 'p'. */
static inline unsigned int zipRawEntryLengthSafe(unsigned char *zl, size_t zlbytes, unsigned char *p) {
    zlentry e;
    assert(zipEntrySafe(zl, zlbytes, p, &e, 0));
    return e.headersize + e.len;
}

/* Return the total number of bytes used by the entry pointed to by 'p'. */
// 返回指针 p 所指向的节点占用的字节数总和.
static inline unsigned int zipRawEntryLength(unsigned char *p) {
    zlentry e;
    zipEntry(p, &e);
    return e.headersize + e.len; // 计算节点占用的字节数总和
}

/* Validate that the entry doesn't reach outside the ziplist allocation. */
static inline void zipAssertValidEntry(unsigned char *zl, size_t zlbytes, unsigned char *p) {
    zlentry e;
    assert(zipEntrySafe(zl, zlbytes, p, &e, 1));
}

// 创建并返回一个新的 ziplist
unsigned char *ziplistNew(void) {
    // ZIPLIST_HEADER_SIZE 是 ziplist 表头的大小
    // 1 字节是表末端 ZIP_END 的大小
    // 初始分配的大小
    unsigned int bytes = ZIPLIST_HEADER_SIZE + ZIPLIST_END_SIZE; // 32+32 +16  + .... + 8
    // 为表头和表末端分配空间
    unsigned char *zl = zmalloc(bytes);
    // 初始化表属性
    ZIPLIST_BYTES(zl) = intrev32ifbe(bytes);
    ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(ZIPLIST_HEADER_SIZE);
    ZIPLIST_LENGTH(zl) = 0;
    // 设置表末端
    zl[bytes - 1] = ZIP_END;
    return zl;
}

// 调整 ziplist 的大小为 len 字节.
// 当 ziplist 原有的大小小于 len 时,扩展 ziplist 不会改变 ziplist 原有的元素.  O(N)
unsigned char *ziplistResize(unsigned char *zl, size_t len) {
    // 对zl进行重新内存空间分配,重新分配的大小是len
    assert(len < UINT32_MAX);
    zl = zrealloc(zl, len);                // 用 zrealloc ,扩展时不改变现有元素
    ZIPLIST_BYTES(zl) = intrev32ifbe(len); // 更新 bytes 属性
    zl[len - 1] = ZIP_END;                 // 重新设置表末端
    return zl;
}

/* When an entry is inserted, we need to set the prevlen field of the next
 * entry to equal the length of the inserted entry. It can occur that this
 * length cannot be encoded in 1 byte and the next entry needs to be grow
 * a bit larger to hold the 5-byte encoded prevlen. This can be done for free,
 * because this only happens when an entry is already being inserted (which
 * causes a realloc and memmove). However, encoding the prevlen may require
 * that this entry is grown as well. This effect may cascade throughout
 * the ziplist when there are consecutive entries with a size close to
 * ZIP_BIG_PREVLEN, so we need to check that the prevlen can be encoded in
 * every consecutive entry.
 *
 * *
 * 当将一个新节点添加到某个节点之前的时候,
 * 如果原节点的 header 空间不足以保存新节点的长度,
 * 那么就需要对原节点的 header 空间进行扩展（从 1 字节扩展到 5 字节）.
 *
 * 但是,当对原节点进行扩展之后,原节点的下一个节点的 prevlen 可能出现空间不足,
 * 这种情况在多个连续节点的长度都接近 ZIP_BIGLEN 时可能发生.
 *
 * 这个函数就用于检查并修复后续节点的空间问题.
 *
 * Note that this effect can also happen in reverse, where the bytes required
 * to encode the prevlen field can shrink. This effect is deliberately ignored,
 * because it can cause a "flapping" effect where a chain prevlen fields is
 * first grown and then shrunk again after consecutive inserts. Rather, the
 * field is allowed to stay larger than necessary, because a large prevlen
 * field implies the ziplist is holding large entries anyway.
 *
 * *
 * 反过来说,
 * 因为节点的长度变小而引起的连续缩小也是可能出现的,
 * 不过,为了避免扩展-缩小-扩展-缩小这样的情况反复出现（flapping,抖动）,
 * 我们不处理这种情况,而是任由 prevlen 比所需的长度更长.
 *
 * The pointer "p" points to the first entry that does NOT need to be
 * updated, i.e. consecutive fields MAY need an update.
 *
 * * 注意,程序的检查是针对 p 的后续节点,而不是 p 所指向的节点.
 * 因为节点 p 在传入之前已经完成了所需的空间扩展工作.
 *
 * T = O(N^2)
 * */
unsigned char *__ziplistCascadeUpdate(unsigned char *zl, unsigned char *p) {
    zlentry cur;
    size_t prevlen, prevlensize, prevoffset; /* Informat of the last changed entry. */
    size_t firstentrylen;                    /* Used to handle insert at head. */
    size_t rawlen, curlen = intrev32ifbe(ZIPLIST_BYTES(zl));
    size_t extra = 0, cnt = 0, offset;
    size_t delta = 4; /* Extra bytes needed to update a entry's prevlen (5-1). */
    unsigned char *tail = zl + intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl));

    /* Empty ziplist */
    if (p[0] == ZIP_END)
        return zl;
    zipEntry(p, &cur); /* no need for "safe" variant since the input pointer was validated by the function that returned it. */
    firstentrylen = prevlen = cur.headersize + cur.len;
    prevlensize = zipStorePrevEntryLength(NULL, prevlen);
    prevoffset = p - zl;
    p += prevlen;

    /* Iterate ziplist to find out how many extra bytes do we need to update it. */
    // T = O(N^2)
    while (p[0] != ZIP_END) {
        assert(zipEntrySafe(zl, curlen, p, &cur, 0));

        /* Abort when "prevlen" has not changed. */
        // 后续节点编码当前节点的空间已经足够,无须再进行任何处理,跳出
        // 可以证明,只要遇到一个空间足够的节点,
        // 那么这个节点之后的所有节点的空间都是足够的
        if (cur.prevrawlen == prevlen)
            break;

        /* Abort when entry's "prevlensize" is big enough. */
        if (cur.prevrawlensize >= prevlensize) {
            if (cur.prevrawlensize == prevlensize) {
                // 运行到这里,
                // 说明 cur 节点的长度正好可以编码到 next 节点的 header 中
                // T = O(1)
                zipStorePrevEntryLength(p, prevlen);
            }
            else {
                /* This would result in shrinking, which we want to avoid.
                 * So, set "prevlen" in the available bytes. */
                // 执行到这里,说明 next 节点编码前置节点的 header 空间有 5 字节
                // 而编码 rawlen 只需要 1 字节
                // 但是程序不会对 next 进行缩小,
                // 所以这里只将 rawlen 写入 5 字节的 header 中就算了.
                // T = O(1)
                zipStorePrevEntryLengthLarge(p, prevlen);
            }
            break;
        }

        /* cur.prevrawlen means cur is the former head entry. */
        assert(cur.prevrawlen == 0 || cur.prevrawlen + delta == prevlen);

        /* Update prev entry's info and advance the cursor. */
        // 当前节点的长度

        rawlen = cur.headersize + cur.len;
        prevlen = rawlen + delta;
        prevlensize = zipStorePrevEntryLength(NULL, prevlen);
        prevoffset = p - zl;
        p += rawlen;
        extra += delta;
        cnt++;
    }

    /* Extra bytes is zero all update has been done(or no need to update). */
    if (extra == 0)
        return zl;

    /* Update tail offset after loop. */
    if (tail == zl + prevoffset) {
        /* When the last entry we need to update is also the tail, update tail offset
         * unless this is the only entry that was updated (so the tail offset didn't change). */
        if (extra - delta != 0) {
            ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)) + extra - delta);
        }
    }
    else {
        /* Update the tail offset in cases where the last entry we updated is not the tail. */
        ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)) + extra);
    }

    /* Now "p" points at the first unchanged byte in original ziplist,
     * move data after that to new ziplist. */
    // 记录 p 的偏移量
    offset = p - zl;
    // 扩展 zl 的大小
    // T = O(N)
    zl = ziplistResize(zl, curlen + extra);
    // 还原指针 p

    p = zl + offset;
    memmove(p + extra, p, curlen - offset - 1);
    p += extra;

    /* Iterate all entries that need to be updated tail to head. */
    while (cnt) {
        zipEntry(zl + prevoffset, &cur); /* no need for "safe" variant since we already iterated on all these entries above. */
        rawlen = cur.headersize + cur.len;
        /* Move entry to tail and reset prevlen. */
        memmove(p - (rawlen - cur.prevrawlensize), zl + prevoffset + cur.prevrawlensize, rawlen - cur.prevrawlensize);
        p -= (rawlen + delta);
        if (cur.prevrawlen == 0) {
            /* "cur" is the previous head entry, update its prevlen with firstentrylen. */
            zipStorePrevEntryLength(p, firstentrylen);
        }
        else {
            /* An entry's prevlen can only increment 4 bytes. */
            zipStorePrevEntryLength(p, cur.prevrawlen + delta);
        }
        /* Forward to previous entry. */
        prevoffset -= cur.prevrawlen;
        cnt--;
    }
    return zl;
}

/* Delete "num" entries, starting at "p". Returns pointer to the ziplist. */
// 从位置 p 开始,连续删除 num 个节点.
// 函数的返回值为处理删除操作之后的 ziplist .
// T = O(N^2)
unsigned char *__ziplistDelete(unsigned char *zl, unsigned char *p, unsigned int num) {
    unsigned int i, totlen, deleted = 0;
    size_t offset;
    int nextdiff = 0;
    zlentry first, tail;
    size_t zlbytes = intrev32ifbe(ZIPLIST_BYTES(zl));

    zipEntry(p, &first); /* no need for "safe" variant since the input pointer was validated by the function that returned it. */
    for (i = 0; p[0] != ZIP_END && i < num; i++) {
        p += zipRawEntryLengthSafe(zl, zlbytes, p);
        deleted++;
    }

    assert(p >= first.p);
    // totlen 是所有被删除节点总共占用的内存字节数
    totlen = p - first.p; /* Bytes taken by the element(s) to delete. */
    if (totlen > 0) {
        uint32_t set_tail;
        if (p[0] != ZIP_END) {
            // 执行这里,表示被删除节点之后仍然有节点存在

            /* Storing `prevrawlen` in this entry may increase or decrease the
             * number of bytes required compare to the current `prevrawlen`.
             * There always is room to store this, because it was previously
             * stored by an entry that is now being deleted. */

            // 因为位于被删除范围之后的第一个节点的 header 部分的大小
            // 可能容纳不了新的前置节点,所以需要计算新旧前置节点之间的字节数差
            nextdiff = zipPrevLenByteDiff(p, first.prevrawlen);

            /* Note that there is always space when p jumps backward: if
             * the new previous entry is large, one of the deleted elements
             * had a 5 bytes prevlen header, so there is for sure at least
             * 5 bytes free and we need just 4. */
            // 如果有需要的话,将指针 p 后退 nextdiff 字节,为新 header 空出空间
            p -= nextdiff;
            assert(p >= first.p && p < zl + zlbytes - 1);
            // 将 first 的前置节点的长度编码至 p 中
            zipStorePrevEntryLength(p, first.prevrawlen);

            /* Update offset for tail */
            // 更新到达表尾的偏移量
            set_tail = intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)) - totlen;

            /* When the tail contains more than one entry, we need to take
             * "nextdiff" in account as well. Otherwise, a change in the
             * size of prevlen doesn't have an effect on the *tail* offset. */
            // 如果被删除节点之后,有多于一个节点
            // 那么程序需要将 nextdiff 记录的字节数也计算到表尾偏移量中
            // 这样才能让表尾偏移量正确对齐表尾节点
            // T = O(1)
            assert(zipEntrySafe(zl, zlbytes, p, &tail, 1));
            if (p[tail.headersize + tail.len] != ZIP_END) {
                set_tail = set_tail + nextdiff;
            }

            /* Move tail to the front of the ziplist */
            /* since we asserted that p >= first.p. we know totlen >= 0,
             * so we know that p > first.p and this is guaranteed not to reach
             * beyond the allocation, even if the entries lens are corrupted. */
            // 从表尾向表头移动数据,覆盖被删除节点的数据
            // T = O(N)
            size_t bytes_to_move = zlbytes - (p - zl) - 1;
            memmove(first.p, p, bytes_to_move);
        }
        else {
            /* The entire tail was deleted. No need to move memory. */
            set_tail = (first.p - zl) - first.prevrawlen;
        }

        /* Resize the ziplist */
        // 缩小并更新 ziplist 的长度
        offset = first.p - zl;
        zlbytes -= totlen - nextdiff;
        zl = ziplistResize(zl, zlbytes);
        p = zl + offset;

        /* Update record count */
        ZIPLIST_INCR_LENGTH(zl, -deleted);

        /* Set the tail offset computed above */
        assert(set_tail <= zlbytes - ZIPLIST_END_SIZE);
        // 执行这里,表示被删除节点之后已经没有其他节点了
        ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(set_tail);

        /* When nextdiff != 0, the raw length of the next entry has changed, so
         * we need to cascade the update throughout the ziplist */
        // 如果 p 所指向的节点的大小已经变更,那么进行级联更新
        // 检查 p 之后的所有节点是否符合 ziplist 的编码要求
        // T = O(N^2)
        if (nextdiff != 0)
            zl = __ziplistCascadeUpdate(zl, p);
    }
    return zl;
}

// 根据指针 p 所指定的位置,将长度为 slen 的字符串 s 插入到 zl 中.
// 函数的返回值为完成插入操作之后的 ziplist
// T = O(N^2)
// 将长度len的字符串s插入到zl(ziplist)中
unsigned char *__ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen) {
    // 获取当前ziplist长度curlen;声明reqlen变量,用来记录新插入元素所需的长度
    size_t curlen = intrev32ifbe(ZIPLIST_BYTES(zl)), reqlen, newlen;

    unsigned int prevlensize, prevlen = 0;
    size_t offset;
    int nextdiff = 0;
    unsigned char encoding = 0;
    long long value = 123456789; /* initialized to avoid warning. Using a value
                                    that is easy to see if for some reason
                                    we use it uninitialized. */
    zlentry tail;

    // 如果插入的位置不是ziplist末尾,则获取前一项长度
    if (p[0] != ZIP_END) {
        // 如果 p[0] 不指向列表末端,说明列表非空,并且 p 正指向列表的其中一个节点
        // 那么取出 p 所指向节点的信息,并将它保存到 entry 结构中
        // 然后用 prevlen 变量记录前置节点的长度
        // （当插入新节点之后 p 所指向的节点就成了新节点的前置节点）
        // T = O(1)

        ZIP_DECODE_PREVLEN(p, prevlensize, prevlen);
    }
    else {
        // 如果 p 指向表尾末端,那么程序需要检查列表是否为：
        // 1)如果 ptail 也指向 ZIP_END ,那么列表为空;
        // 2)如果列表不为空,那么 ptail 将指向列表的最后一个节点.
        unsigned char *ptail = ZIPLIST_ENTRY_TAIL(zl);
        if (ptail[0] != ZIP_END) {
            // 表尾节点为新节点的前置节点

            // 取出表尾节点的长度
            // T = O(1)
            prevlen = zipRawEntryLengthSafe(zl, curlen, ptail);
        }
    }

    // 1、计算实际插入元素的长度
    // 尝试看能否将输入字符串转换为整数,如果成功的话：
    // 1)value 将保存转换后的整数值
    // 2)encoding 则保存适用于 value 的编码方式
    // 无论使用什么编码, reqlen 都保存节点值的长度
    // T = O(N)
    if (zipTryEncoding(s, slen, &value, &encoding)) {
        /* 'encoding' is set to the appropriate integer encoding */
        reqlen = zipIntSize(encoding);
    }
    else {
        /* 'encoding' is untouched, however zipStoreEntryEncoding will use the
         * string length to figure out how to encode it. */
        reqlen = slen;
    }

    // 2、将插入位置元素的 prevlen 也计算到所需空间中
    reqlen += zipStorePrevEntryLength(NULL, prevlen);
    // 3、计算编码当前节点值所需的大小O(1)
    reqlen += zipStoreEntryEncoding(NULL, encoding, slen);

    /* When the insert position is not equal to the tail, we need to
     * make sure that the next entry can hold this entry's length in
     * its prevlen field. */
    // 只要新节点不是被添加到列表末端,
    // 那么程序就需要检查看 p 所指向的节点（的 header）能否编码新节点的长度.
    // nextdiff 保存了新旧编码之间的字节大小差,如果这个值大于 0
    // 那么说明需要对 p 所指向的节点（的 header ）进行扩展
    // T = O(1)
    int forcelarge = 0;

    // 4、判断插入位置元素的 prevlen 和实际所需的 prevlen 大小.
    nextdiff = (p[0] != ZIP_END) ? zipPrevLenByteDiff(p, reqlen) : 0;
    if (nextdiff == -4 && reqlen < 4) {
        nextdiff = 0;
        forcelarge = 1;
    }

    /* Store offset because a realloc may change the address of zl. */
    // 因为重分配空间可能会改变 zl 的地址
    // 所以在分配之前,需要记录 zl 到 p 的偏移量,然后在分配之后依靠偏移量还原 p
    offset = p - zl;
    newlen = curlen + reqlen + nextdiff;
    // curlen 是 ziplist 原来的长度
    // reqlen 是整个新节点的长度
    // nextdiff 是新节点的后继节点扩展 header 的长度（要么 0 字节,要么 4 个字节）
    // T = O(N)
    zl = ziplistResize(zl, newlen);
    p = zl + offset;

    /* Apply memory move when necessary and update tail offset. */
    if (p[0] != ZIP_END) {
        // 新元素之后还有节点,因为新元素的加入,需要对这些原有节点进行调整

        /* Subtract one because of the ZIP_END bytes */
        // 移动现有元素,为新元素的插入空间腾出位置
        // T = O(N)
        memmove(p + reqlen, p - nextdiff, curlen - offset - 1 + nextdiff);

        /* Encode this entry's raw length in the next entry. */
        // 将新节点的长度编码至后置节点
        // p+reqlen 定位到后置节点
        // reqlen 是新节点的长度
        // T = O(1)
        if (forcelarge)
            zipStorePrevEntryLengthLarge(p + reqlen, reqlen);
        else
            zipStorePrevEntryLength(p + reqlen, reqlen);

        /* Update offset for tail */
        // 更新到达表尾的偏移量,将新节点的长度也算上
        ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)) + reqlen);

        /* When the tail contains more than one entry, we need to take
         * "nextdiff" in account as well. Otherwise, a change in the
         * size of prevlen doesn't have an effect on the *tail* offset. */
        // 如果新节点的后面有多于一个节点
        // 那么程序需要将 nextdiff 记录的字节数也计算到表尾偏移量中
        // 这样才能让表尾偏移量正确对齐表尾节点
        // T = O(1)
        assert(zipEntrySafe(zl, newlen, p + reqlen, &tail, 1));
        if (p[reqlen + tail.headersize + tail.len] != ZIP_END) {
            ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)) + nextdiff);
        }
    }
    else {
        /* This element will be the new tail. */
        // 新元素是新的表尾节点
        ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(p - zl);
    }

    /* When nextdiff != 0, the raw length of the next entry has changed, so
     * we need to cascade the update throughout the ziplist */
    // 当 nextdiff != 0 时,新节点的后继节点的（header 部分）长度已经被改变,
    // 所以需要级联地更新后续的节点
    if (nextdiff != 0) {
        offset = p - zl;
        zl = __ziplistCascadeUpdate(zl, p + reqlen);
        p = zl + offset;
    }

    /* Write the entry */
    // 一切搞定,将前置节点的长度写入新节点的 header

    p += zipStorePrevEntryLength(p, prevlen);
    // 将节点值的长度写入新节点的 header

    p += zipStoreEntryEncoding(p, encoding, slen);
    // 写入节点值

    if (ZIP_IS_STR(encoding)) {
        memcpy(p, s, slen); // T = O(N)
    }
    else {
        zipSaveInteger(p, value, encoding); // T = O(1)
    }

    // 更新列表的节点数量计数器
    // T = O(1)
    ZIPLIST_INCR_LENGTH(zl, 1);
    return zl;
}

/* Merge ziplists 'first' and 'second' by appending 'second' to 'first'.
 *
 * NOTE: The larger ziplist is reallocated to contain the new merged ziplist.
 * Either 'first' or 'second' can be used for the result.  The parameter not
 * used will be free'd and set to NULL.
 *
 * After calling this function, the input parameters are no longer valid since
 * they are changed and free'd in-place.
 *
 * The result ziplist is the contents of 'first' followed by 'second'.
 *
 * On failure: returns NULL if the merge is impossible.
 * On success: returns the merged ziplist (which is expanded version of either
 * 'first' or 'second', also frees the other unused input ziplist, and sets the
 * input ziplist argument equal to newly reallocated ziplist return value. */
unsigned char *ziplistMerge(unsigned char **first, unsigned char **second) {
    /* If any params are null, we can't merge, so NULL. */
    if (first == NULL || *first == NULL || second == NULL || *second == NULL)
        return NULL;

    /* Can't merge same list into itself. */
    if (*first == *second)
        return NULL;

    size_t first_bytes = intrev32ifbe(ZIPLIST_BYTES(*first));
    size_t first_len = intrev16ifbe(ZIPLIST_LENGTH(*first));

    size_t second_bytes = intrev32ifbe(ZIPLIST_BYTES(*second));
    size_t second_len = intrev16ifbe(ZIPLIST_LENGTH(*second));

    int append;
    unsigned char *source, *target;
    size_t target_bytes, source_bytes;
    /* Pick the largest ziplist so we can resize easily in-place.
     * We must also track if we are now appending or prepending to
     * the target ziplist. */
    if (first_len >= second_len) {
        /* retain first, append second to first. */
        target = *first;
        target_bytes = first_bytes;
        source = *second;
        source_bytes = second_bytes;
        append = 1;
    }
    else {
        /* else, retain second, prepend first to second. */
        target = *second;
        target_bytes = second_bytes;
        source = *first;
        source_bytes = first_bytes;
        append = 0;
    }

    /* Calculate final bytes (subtract one pair of metadata) */
    size_t zlbytes = first_bytes + second_bytes - ZIPLIST_HEADER_SIZE - ZIPLIST_END_SIZE;
    size_t zllength = first_len + second_len;

    /* Combined zl length should be limited within UINT16_MAX */
    zllength = zllength < UINT16_MAX ? zllength : UINT16_MAX;

    /* larger values can't be stored into ZIPLIST_BYTES */
    assert(zlbytes < UINT32_MAX);

    /* Save offset positions before we start ripping memory apart. */
    size_t first_offset = intrev32ifbe(ZIPLIST_TAIL_OFFSET(*first));
    size_t second_offset = intrev32ifbe(ZIPLIST_TAIL_OFFSET(*second));

    /* Extend target to new zlbytes then append or prepend source. */
    target = zrealloc(target, zlbytes);
    if (append) {
        /* append == appending to target */
        /* Copy source after target (copying over original [END]):
         *   [TARGET - END, SOURCE - HEADER] */
        memcpy(target + target_bytes - ZIPLIST_END_SIZE, source + ZIPLIST_HEADER_SIZE, source_bytes - ZIPLIST_HEADER_SIZE);
    }
    else {
        /* !append == prepending to target */
        /* Move target *contents* exactly size of (source - [END]),
         * then copy source into vacated space (source - [END]):
         *   [SOURCE - END, TARGET - HEADER] */
        memmove(target + source_bytes - ZIPLIST_END_SIZE, target + ZIPLIST_HEADER_SIZE, target_bytes - ZIPLIST_HEADER_SIZE);
        memcpy(target, source, source_bytes - ZIPLIST_END_SIZE);
    }

    /* Update header metadata. */
    ZIPLIST_BYTES(target) = intrev32ifbe(zlbytes);
    ZIPLIST_LENGTH(target) = intrev16ifbe(zllength);
    /* New tail offset is:
     *   + N bytes of first ziplist
     *   - 1 byte for [END] of first ziplist
     *   + M bytes for the offset of the original tail of the second ziplist
     *   - J bytes for HEADER because second_offset keeps no header. */
    ZIPLIST_TAIL_OFFSET(target) = intrev32ifbe((first_bytes - ZIPLIST_END_SIZE) + (second_offset - ZIPLIST_HEADER_SIZE));

    /* __ziplistCascadeUpdate just fixes the prev length values until it finds a
     * correct prev length value (then it assumes the rest of the list is okay).
     * We tell CascadeUpdate to start at the first ziplist's tail element to fix
     * the merge seam. */
    target = __ziplistCascadeUpdate(target, target + first_offset);

    /* Now free and NULL out what we didn't realloc */
    if (append) {
        zfree(*second);
        *second = NULL;
        *first = target;
    }
    else {
        zfree(*first);
        *first = NULL;
        *second = target;
    }
    return target;
}
/*
 * 将长度为 slen 的字符串 s 推入到 zl 中.
 *
 * where 参数的值决定了推入的方向：
 * - 值为 ZIPLIST_HEAD 时,将新值推入到表头.
 * - 否则,将新值推入到表末端.
 *
 * 函数的返回值为添加新值后的 ziplist .
 *
 * T = O(N^2)
 */
unsigned char *ziplistPush(unsigned char *zl, unsigned char *s, unsigned int slen, int where) {
    // 根据 where 参数的值,决定将值推入到表头还是表尾
    unsigned char *p;
    p = (where == ZIPLIST_HEAD) ? ZIPLIST_ENTRY_HEAD(zl) : ZIPLIST_ENTRY_END(zl);
    // 返回添加新值后的 ziplist
    // T = O(N^2)
    return __ziplistInsert(zl, p, s, slen);
}

/* Returns an offset to use for iterating with ziplistNext. When the given
 * index is negative, the list is traversed back to front. When the list
 * doesn't contain an element at the provided index, NULL is returned. */
/*
 * 根据给定索引,遍历列表,并返回索引指定节点的指针.
 *
 * 如果索引为正,那么从表头向表尾遍历.
 * 如果索引为负,那么从表尾向表头遍历.
 * 正数索引从 0 开始,负数索引从 -1 开始.
 *
 * 如果索引超过列表的节点数量,或者列表为空,那么返回 NULL .
 *
 * T = O(N)
 */
unsigned char *ziplistIndex(unsigned char *zl, int index) {
    unsigned char *p;
    unsigned int prevlensize, prevlen = 0;
    size_t zlbytes = intrev32ifbe(ZIPLIST_BYTES(zl));
    if (index < 0) { // 处理负数索引

        index = (-index) - 1; // 将索引转换为正数

        p = ZIPLIST_ENTRY_TAIL(zl); // 定位到表尾节点

        if (p[0] != ZIP_END) { // 如果列表不为空,那么...

            /* No need for "safe" check: when going backwards, we know the header
             * we're parsing is in the range, we just need to assert (below) that
             * the size we take doesn't cause p to go outside the allocation. */
            ZIP_DECODE_PREVLENSIZE(p, prevlensize);
            assert(p + prevlensize < zl + zlbytes - ZIPLIST_END_SIZE);
            ZIP_DECODE_PREVLEN(p, prevlensize, prevlen);
            while (prevlen > 0 && index--) { // T = O(N)

                p -= prevlen; // 前移指针

                assert(p >= zl + ZIPLIST_HEADER_SIZE && p < zl + zlbytes - ZIPLIST_END_SIZE);
                // T = O(1)
                ZIP_DECODE_PREVLEN(p, prevlensize, prevlen);
            }
        }
    }
    else { // 处理正数索引

        p = ZIPLIST_ENTRY_HEAD(zl); // 定位到表头节点

        // T = O(N)
        while (index--) {
            /* Use the "safe" length: When we go forward, we need to be careful
             * not to decode an entry header if it's past the ziplist allocation. */
            // 后移指针
            // T = O(1)
            p += zipRawEntryLengthSafe(zl, zlbytes, p);
            if (p[0] == ZIP_END)
                break;
        }
    }
    // 返回结果

    if (p[0] == ZIP_END || index > 0)
        return NULL;
    zipAssertValidEntry(zl, zlbytes, p);
    return p;
}

/* Return pointer to next entry in ziplist.
 *
 * zl is the pointer to the ziplist
 * p is the pointer to the current element
 *
 * The element after 'p' is returned, otherwise NULL if we are at the end.
 * 返回 p 所指向节点的后置节点.
 *
 * 如果 p 为表末端,或者 p 已经是表尾节点,那么返回 NULL .
 *
 * T = O(1)
 */

unsigned char *ziplistNext(unsigned char *zl, unsigned char *p) {
    ((void)zl);
    size_t zlbytes = intrev32ifbe(ZIPLIST_BYTES(zl));

    /* "p" could be equal to ZIP_END, caused by ziplistDelete,
     * and we should return NULL. Otherwise, we should return NULL
     * when the *next* element is ZIP_END (there is no next entry). */
    // p 已经指向列表末端

    if (p[0] == ZIP_END) {
        return NULL;
    }
    // 指向后一节点

    p += zipRawEntryLength(p);
    if (p[0] == ZIP_END) {
        // p 已经是表尾节点,没有后置节点

        return NULL;
    }

    zipAssertValidEntry(zl, zlbytes, p);
    return p;
}

/* Return pointer to previous entry in ziplist. */
/*
 * 返回 p 所指向节点的前置节点.
 *
 * 如果 p 所指向为空列表,或者 p 已经指向表头节点,那么返回 NULL .
 *
 * T = O(1)
 */
unsigned char *ziplistPrev(unsigned char *zl, unsigned char *p) {
    unsigned int prevlensize, prevlen = 0;

    /* Iterating backwards from ZIP_END should return the tail. When "p" is
     * equal to the first element of the list, we're already at the head,
     * and should return NULL. */
    // 如果 p 指向列表末端（列表为空,或者刚开始从表尾向表头迭代）
    // 那么尝试取出列表尾端节点
    if (p[0] == ZIP_END) {
        p = ZIPLIST_ENTRY_TAIL(zl);
        // 尾端节点也指向列表末端,那么列表为空

        return (p[0] == ZIP_END) ? NULL : p;
    }
    else if (p == ZIPLIST_ENTRY_HEAD(zl)) { // 如果 p 指向列表头,那么说明迭代已经完成

        return NULL;
    }
    else { // 既不是表头也不是表尾,从表尾向表头移动指针
           // 计算前一个节点的节点数

        ZIP_DECODE_PREVLEN(p, prevlensize, prevlen);
        assert(prevlen > 0);
        p -= prevlen; // 移动指针,指向前一个节点

        size_t zlbytes = intrev32ifbe(ZIPLIST_BYTES(zl));
        zipAssertValidEntry(zl, zlbytes, p);
        return p;
    }
}

/* Get entry pointed to by 'p' and store in either '*sstr' or 'sval' depending
 * on the encoding of the entry. '*sstr' is always set to NULL to be able
 * to find out whether the string pointer or the integer value was set.
 * Return 0 if 'p' points to the end of the ziplist, 1 otherwise.
 * 取出 p 所指向节点的值：
 *
 * - 如果节点保存的是字符串,那么将字符串值指针保存到 *sstr 中,字符串长度保存到
 * *slen
 *
 * - 如果节点保存的是整数,那么将整数保存到 *sval
 *
 * 程序可以通过检查 *sstr 是否为 NULL 来检查值是字符串还是整数.
 *
 * 提取值成功返回 1 ,
 * 如果 p 为空,或者 p 指向的是列表末端,那么返回 0 ,提取值失败.
 *
 * T = O(1)
 */
unsigned int ziplistGet(unsigned char *p, unsigned char **sstr, unsigned int *slen, long long *sval) {
    zlentry entry;
    if (p == NULL || p[0] == ZIP_END)
        return 0;
    if (sstr)
        *sstr = NULL;
    // 取出 p 所指向的节点的各项信息,并保存到结构 entry 中
    // T = O(1)
    zipEntry(p, &entry); /* no need for "safe" variant since the input pointer was validated by the function that returned it. */

    // 节点的值为字符串,将字符串长度保存到 *slen ,字符串保存到 *sstr
    // T = O(1)
    if (ZIP_IS_STR(entry.encoding)) {
        if (sstr) {
            *slen = entry.len;
            *sstr = p + entry.headersize;
        }
        // 节点的值为整数,解码值,并将值保存到 *sval
        // T = O(1)
    }
    else {
        if (sval) {
            *sval = zipLoadInteger(p + entry.headersize, entry.encoding);
        }
    }
    return 1;
}

/* Insert an entry at "p".
 *
 * 将包含给定值 s 的新节点插入到给定的位置 p 中.
 *
 * 如果 p 指向一个节点,那么新节点将放在原有节点的前面.
 *
 * T = O(N^2)
 */
unsigned char *ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen) {
    return __ziplistInsert(zl, p, s, slen);
}

/* Delete a single entry from the ziplist, pointed to by *p.
 * Also update *p in place, to be able to iterate over the
 * ziplist, while deleting entries.
 *
 * 从 zl 中删除 *p 所指向的节点,
 * 并且原地更新 *p 所指向的位置,使得可以在迭代列表的过程中对节点进行删除.
 *
 * T = O(N^2)
 */
unsigned char *ziplistDelete(unsigned char *zl, unsigned char **p) {
    // 因为 __ziplistDelete 时会对 zl 进行内存重分配
    // 而内存充分配可能会改变 zl 的内存地址
    // 所以这里需要记录到达 *p 的偏移量
    // 这样在删除节点之后就可以通过偏移量来将 *p 还原到正确的位置
    size_t offset = *p - zl;
    zl = __ziplistDelete(zl, *p, 1);

    /* Store pointer to current element in p, because ziplistDelete will
     * do a realloc which might result in a different "zl"-pointer.
     * When the delete direction is back to front, we might delete the last
     * entry and end up with "p" pointing to ZIP_END, so check this. */
    *p = zl + offset;
    return zl;
}

/* Delete a range of entries from the ziplist.
 *
 * 从 index 索引指定的节点开始,连续地从 zl 中删除 num 个节点.
 *
 * T = O(N^2)
 */
unsigned char *ziplistDeleteRange(unsigned char *zl, int index, unsigned int num) {
    // 根据索引定位到节点
    // T = O(N)
    unsigned char *p = ziplistIndex(zl, index);

    // 连续删除 num 个节点
    // T = O(N^2)
    return (p == NULL) ? zl : __ziplistDelete(zl, p, num);
}

/* Replaces the entry at p. This is equivalent to a delete and an insert,
 * but avoids some overhead when replacing a value of the same size. */
unsigned char *ziplistReplace(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen) {
    /* get metadata of the current entry */
    zlentry entry;
    zipEntry(p, &entry);

    /* compute length of entry to store, excluding prevlen */
    unsigned int reqlen;
    unsigned char encoding = 0;
    long long value = 123456789; /* initialized to avoid warning. */
    if (zipTryEncoding(s, slen, &value, &encoding)) {
        reqlen = zipIntSize(encoding); /* encoding is set */
    }
    else {
        reqlen = slen; /* encoding == 0 */
    }
    reqlen += zipStoreEntryEncoding(NULL, encoding, slen);

    if (reqlen == entry.lensize + entry.len) {
        /* Simply overwrite the element. */
        p += entry.prevrawlensize;
        p += zipStoreEntryEncoding(p, encoding, slen);
        if (ZIP_IS_STR(encoding)) {
            memcpy(p, s, slen);
        }
        else {
            zipSaveInteger(p, value, encoding);
        }
    }
    else {
        /* Fallback. */
        zl = ziplistDelete(zl, &p);
        zl = ziplistInsert(zl, p, s, slen);
    }
    return zl;
}

/* Compare entry pointer to by 'p' with 'entry'. Return 1 if equal.
 *
 * 将 p 所指向的节点的值和 sstr 进行对比.
 *
 * 如果节点值和 sstr 的值相等,返回 1 ,不相等则返回 0 .
 *
 * T = O(N)
 */
unsigned int ziplistCompare(unsigned char *p, unsigned char *sstr, unsigned int slen) {
    zlentry entry;
    unsigned char sencoding;
    long long zval, sval;
    if (p[0] == ZIP_END)
        return 0;
    // 取出节点
    zipEntry(p, &entry); /* no need for "safe" variant since the input pointer was validated by the function that returned it. */
    if (ZIP_IS_STR(entry.encoding)) {
        /* Raw compare */
        // 节点值为字符串,进行字符串对比

        if (entry.len == slen) {
            return memcmp(p + entry.headersize, sstr, slen) == 0;
        }
        else {
            return 0;
        }
    }
    else {
        // 节点值为整数,进行整数对比

        /* Try to compare encoded values. Don't compare encoding because
         * different implementations may encoded integers differently. */
        if (zipTryEncoding(sstr, slen, &sval, &sencoding)) {
            zval = zipLoadInteger(p + entry.headersize, entry.encoding);
            return zval == sval;
        }
    }
    return 0;
}

/* Find pointer to the entry equal to the specified entry.
 *
 * 寻找节点值和 vstr 相等的列表节点,并返回该节点的指针.
 *
 * Skip 'skip' entries between every comparison.
 *
 * 每次比对之前都跳过 skip 个节点.
 *
 * Returns NULL when the field could not be found.
 *
 * 如果找不到相应的节点,则返回 NULL .
 *
 * T = O(N^2)
 */
unsigned char *ziplistFind(unsigned char *zl, unsigned char *p, unsigned char *vstr, unsigned int vlen, unsigned int skip) {
    int skipcnt = 0;
    unsigned char vencoding = 0;
    long long vll = 0;
    size_t zlbytes = ziplistBlobLen(zl);
    // 只要未到达列表末端,就一直迭代
    // T = O(N^2)
    while (p[0] != ZIP_END) {
        struct zlentry e;
        unsigned char *q;

        assert(zipEntrySafe(zl, zlbytes, p, &e, 1));
        q = p + e.prevrawlensize + e.lensize;

        if (skipcnt == 0) {
            /* Compare current entry with specified entry */
            // 对比字符串值
            // T = O(N)
            if (ZIP_IS_STR(e.encoding)) {
                if (e.len == vlen && memcmp(q, vstr, vlen) == 0) {
                    return p;
                }
            }
            else {
                /* Find out if the searched field can be encoded. Note that
                 * we do it only the first time, once done vencoding is set
                 * to non-zero and vll is set to the integer value. */
                // 因为传入值有可能被编码了,
                // 所以当第一次进行值对比时,程序会对传入值进行解码
                // 这个解码操作只会进行一次
                if (vencoding == 0) {
                    if (!zipTryEncoding(vstr, vlen, &vll, &vencoding)) {
                        /* If the entry can't be encoded we set it to
                         * UCHAR_MAX so that we don't retry again the next
                         * time. */
                        vencoding = UCHAR_MAX;
                    }
                    /* Must be non-zero by now */
                    assert(vencoding);
                }

                /* Compare current entry with specified entry, do it only
                 * if vencoding != UCHAR_MAX because if there is no encoding
                 * possible for the field it can't be a valid integer. */
                // 对比整数值
                if (vencoding != UCHAR_MAX) {
                    long long ll = zipLoadInteger(q, e.encoding);
                    if (ll == vll) {
                        return p;
                    }
                }
            }

            /* Reset skip count */
            skipcnt = skip;
        }
        else {
            /* Skip entry */
            skipcnt--;
        }

        /* Move to next entry */
        // 后移指针,指向后置节点
        p = q + e.len;
    }

    return NULL; // 没有找到指定的节点
}

// 返回 ziplist 中的节点个数
// T = O(N)
unsigned int ziplistLen(unsigned char *zl) {
    unsigned int len = 0;
    // 节点数小于 UINT16_MAX
    // T = O(1)
    if (intrev16ifbe(ZIPLIST_LENGTH(zl)) < UINT16_MAX) {
        len = intrev16ifbe(ZIPLIST_LENGTH(zl));
    }
    else {
        // 节点数大于 UINT16_MAX 时,需要遍历整个列表才能计算出节点数
        // T = O(N)
        unsigned char *p = zl + ZIPLIST_HEADER_SIZE;
        size_t zlbytes = intrev32ifbe(ZIPLIST_BYTES(zl));
        while (*p != ZIP_END) {
            p += zipRawEntryLengthSafe(zl, zlbytes, p);
            len++;
        }

        /* Re-store length if small enough */
        if (len < UINT16_MAX)
            ZIPLIST_LENGTH(zl) = intrev16ifbe(len);
    }
    return len;
}

// 返回整个 ziplist 占用的内存字节数
size_t ziplistBlobLen(unsigned char *zl) {
    return intrev32ifbe(ZIPLIST_BYTES(zl));
}

void ziplistRepr(unsigned char *zl) {
    unsigned char *p;
    int index = 0;
    zlentry entry;
    size_t zlbytes = ziplistBlobLen(zl);

    printf(
        "{total bytes %u} "
        "{num entries %u}\n"
        "{tail offset %u}\n",
        intrev32ifbe(ZIPLIST_BYTES(zl)), intrev16ifbe(ZIPLIST_LENGTH(zl)), intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)));
    p = ZIPLIST_ENTRY_HEAD(zl);
    while (*p != ZIP_END) {
        assert(zipEntrySafe(zl, zlbytes, p, &entry, 1));
        printf(
            "{\n"
            "\taddr 0x%08lx,\n"
            "\tindex %2d,\n"
            "\toffset %5lu,\n"
            "\thdr+entry len: %5u,\n"
            "\thdr len%2u,\n"
            "\tprevrawlen: %5u,\n"
            "\tprevrawlensize: %2u,\n"
            "\tpayload %5u\n",
            (long unsigned)p, index, (unsigned long)(p - zl), entry.headersize + entry.len, entry.headersize, entry.prevrawlen, entry.prevrawlensize, entry.len);
        printf("\tbytes: ");
        for (unsigned int i = 0; i < entry.headersize + entry.len; i++) {
            printf("%02x|", p[i]);
        }
        printf("\n");
        p += entry.headersize;
        if (ZIP_IS_STR(entry.encoding)) {
            printf("\t[str]");
            if (entry.len > 40) {
                if (fwrite(p, 40, 1, stdout) == 0)
                    perror("fwrite");
                printf("...");
            }
            else {
                if (entry.len && fwrite(p, entry.len, 1, stdout) == 0)
                    perror("fwrite");
            }
        }
        else {
            printf("\t[int]%lld", (long long)zipLoadInteger(p, entry.encoding));
        }
        printf("\n}\n");
        p += entry.len;
        index++;
    }
    printf("{end}\n\n");
}

/* Validate the integrity of the data structure.
 * when `deep` is 0, only the integrity of the header is validated.
 * when `deep` is 1, we scan all the entries one by one. */
int ziplistValidateIntegrity(unsigned char *zl, size_t size, int deep, ziplistValidateEntryCB entry_cb, void *cb_userdata) {
    /* check that we can actually read the header. (and ZIP_END) */
    if (size < ZIPLIST_HEADER_SIZE + ZIPLIST_END_SIZE)
        return 0;

    /* check that the encoded size in the header must match the allocated size. */
    size_t bytes = intrev32ifbe(ZIPLIST_BYTES(zl));
    if (bytes != size)
        return 0;

    /* the last byte must be the terminator. */
    if (zl[size - ZIPLIST_END_SIZE] != ZIP_END)
        return 0;

    /* make sure the tail offset isn't reaching outside the allocation. */
    if (intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)) > size - ZIPLIST_END_SIZE)
        return 0;

    if (!deep)
        return 1;

    unsigned int count = 0;
    unsigned int header_count = intrev16ifbe(ZIPLIST_LENGTH(zl));
    unsigned char *p = ZIPLIST_ENTRY_HEAD(zl);
    unsigned char *prev = NULL;
    size_t prev_raw_size = 0;
    while (*p != ZIP_END) {
        struct zlentry e;
        /* Decode the entry headers and fail if invalid or reaches outside the allocation */
        if (!zipEntrySafe(zl, size, p, &e, 1))
            return 0;

        /* Make sure the record stating the prev entry size is correct. */
        if (e.prevrawlen != prev_raw_size)
            return 0;

        /* Optionally let the caller validate the entry too. */
        if (entry_cb && !entry_cb(p, header_count, cb_userdata))
            return 0;

        /* Move to the next entry */
        prev_raw_size = e.headersize + e.len;
        prev = p;
        p += e.headersize + e.len;
        count++;
    }

    /* Make sure 'p' really does point to the end of the ziplist. */
    if (p != zl + bytes - ZIPLIST_END_SIZE)
        return 0;

    /* Make sure the <zltail> entry really do point to the start of the last entry. */
    if (prev != NULL && prev != ZIPLIST_ENTRY_TAIL(zl))
        return 0;

    /* Check that the count in the header is correct */
    if (header_count != UINT16_MAX && count != header_count)
        return 0;

    return 1;
}

/* Randomly select a pair of key and value.
 * total_count is a pre-computed length/2 of the ziplist (to avoid calls to ziplistLen)
 * 'key' and 'val' are used to store the result key value pair.
 * 'val' can be NULL if the value is not needed. */
void ziplistRandomPair(unsigned char *zl, unsigned long total_count, ziplistEntry *key, ziplistEntry *val) {
    int ret;
    unsigned char *p;

    /* Avoid div by zero on corrupt ziplist */
    assert(total_count);

    /* Generate even numbers, because ziplist saved K-V pair */
    int r = (rand() % total_count) * 2;
    p = ziplistIndex(zl, r);
    ret = ziplistGet(p, &key->sval, &key->slen, &key->lval);
    assert(ret != 0);

    if (!val)
        return;
    p = ziplistNext(zl, p);
    ret = ziplistGet(p, &val->sval, &val->slen, &val->lval);
    assert(ret != 0);
}

/* int compare for qsort */
int uintCompare(const void *a, const void *b) {
    return (*(unsigned int *)a - *(unsigned int *)b);
}

/* Helper method to store a string into from val or lval into dest */
static inline void ziplistSaveValue(unsigned char *val, unsigned int len, long long lval, ziplistEntry *dest) {
    dest->sval = val;
    dest->slen = len;
    dest->lval = lval;
}

/* Randomly select count of key value pairs and store into 'keys' and
 * 'vals' args. The order of the picked entries is random, and the selections
 * are non-unique (repetitions are possible).
 * The 'vals' arg can be NULL in which case we skip these. */
void ziplistRandomPairs(unsigned char *zl, unsigned int count, ziplistEntry *keys, ziplistEntry *vals) {
    unsigned char *p, *key, *value;
    unsigned int klen = 0, vlen = 0;
    long long klval = 0, vlval = 0;

    /* Notice: the index member must be first due to the use in uintCompare */
    typedef struct {
        unsigned int index;
        unsigned int order;
    } rand_pick;
    rand_pick *picks = zmalloc(sizeof(rand_pick) * count);
    unsigned int total_size = ziplistLen(zl) / 2;

    /* Avoid div by zero on corrupt ziplist */
    assert(total_size);

    /* create a pool of random indexes (some may be duplicate). */
    for (unsigned int i = 0; i < count; i++) {
        picks[i].index = (rand() % total_size) * 2; /* Generate even indexes */
        /* keep track of the order we picked them */
        picks[i].order = i;
    }

    /* sort by indexes. */
    qsort(picks, count, sizeof(rand_pick), uintCompare);

    /* fetch the elements form the ziplist into a output array respecting the original order. */
    unsigned int zipindex = picks[0].index, pickindex = 0;
    p = ziplistIndex(zl, zipindex);
    while (ziplistGet(p, &key, &klen, &klval) && pickindex < count) {
        p = ziplistNext(zl, p);
        assert(ziplistGet(p, &value, &vlen, &vlval));
        while (pickindex < count && zipindex == picks[pickindex].index) {
            int storeorder = picks[pickindex].order;
            ziplistSaveValue(key, klen, klval, &keys[storeorder]);
            if (vals)
                ziplistSaveValue(value, vlen, vlval, &vals[storeorder]);
            pickindex++;
        }
        zipindex += 2;
        p = ziplistNext(zl, p);
    }

    zfree(picks);
}

/* Randomly select count of key value pairs and store into 'keys' and
 * 'vals' args. The selections are unique (no repetitions), and the order of
 * the picked entries is NOT-random.
 * The 'vals' arg can be NULL in which case we skip these.
 * The return value is the number of items picked which can be lower than the
 * requested count if the ziplist doesn't hold enough pairs. */
unsigned int ziplistRandomPairsUnique(unsigned char *zl, unsigned int count, ziplistEntry *keys, ziplistEntry *vals) {
    unsigned char *p, *key;
    unsigned int klen = 0;
    long long klval = 0;
    unsigned int total_size = ziplistLen(zl) / 2;
    unsigned int index = 0;
    if (count > total_size)
        count = total_size;

    /* To only iterate once, every time we try to pick a member, the probability
     * we pick it is the quotient of the count left we want to pick and the
     * count still we haven't visited in the dict, this way, we could make every
     * member be equally picked.*/
    p = ziplistIndex(zl, 0);
    unsigned int picked = 0, remaining = count;
    while (picked < count && p) {
        double randomDouble = ((double)rand()) / RAND_MAX;
        double threshold = ((double)remaining) / (total_size - index);
        if (randomDouble <= threshold) {
            assert(ziplistGet(p, &key, &klen, &klval));
            ziplistSaveValue(key, klen, klval, &keys[picked]);
            p = ziplistNext(zl, p);
            assert(p);
            if (vals) {
                assert(ziplistGet(p, &key, &klen, &klval));
                ziplistSaveValue(key, klen, klval, &vals[picked]);
            }
            remaining--;
            picked++;
        }
        else {
            p = ziplistNext(zl, p);
            assert(p);
        }
        p = ziplistNext(zl, p);
        index++;
    }
    return picked;
}

#ifdef REDIS_TEST
#    include <sys/time.h>
#    include "adlist.h"
#    include "over-sds.h"
#    include "testhelp.h"

#    define debug(f, ...)               \
        {                               \
            if (DEBUG)                  \
                printf(f, __VA_ARGS__); \
        }

static unsigned char *createList() {
    unsigned char *zl = ziplistNew();
    zl = ziplistPush(zl, (unsigned char *)"foo", 3, ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char *)"quux", 4, ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char *)"hello", 5, ZIPLIST_HEAD);
    zl = ziplistPush(zl, (unsigned char *)"1024", 4, ZIPLIST_TAIL);
    return zl;
}

static unsigned char *createIntList() {
    unsigned char *zl = ziplistNew();
    char buf[32];

    sprintf(buf, "100");
    zl = ziplistPush(zl, (unsigned char *)buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "128000");
    zl = ziplistPush(zl, (unsigned char *)buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "-100");
    zl = ziplistPush(zl, (unsigned char *)buf, strlen(buf), ZIPLIST_HEAD);
    sprintf(buf, "4294967296");
    zl = ziplistPush(zl, (unsigned char *)buf, strlen(buf), ZIPLIST_HEAD);
    sprintf(buf, "non integer");
    zl = ziplistPush(zl, (unsigned char *)buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "much much longer non integer");
    zl = ziplistPush(zl, (unsigned char *)buf, strlen(buf), ZIPLIST_TAIL);
    return zl;
}

static long long usec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (((long long)tv.tv_sec) * 1000000) + tv.tv_usec;
}

static void stress(int pos, int num, int maxsize, int dnum) {
    int i, j, k;
    unsigned char *zl;
    char posstr[2][5] = {"HEAD", "TAIL"};
    long long start;
    for (i = 0; i < maxsize; i += dnum) {
        zl = ziplistNew();
        for (j = 0; j < i; j++) {
            zl = ziplistPush(zl, (unsigned char *)"quux", 4, ZIPLIST_TAIL);
        }

        /* Do num times a push+pop from pos */
        start = usec();
        for (k = 0; k < num; k++) {
            zl = ziplistPush(zl, (unsigned char *)"quux", 4, pos);
            zl = ziplistDeleteRange(zl, 0, 1);
        }
        printf("List size: %8d, bytes: %8d, %dx push+pop (%s): %6lld usec\n", i, intrev32ifbe(ZIPLIST_BYTES(zl)), num, posstr[pos], usec() - start);
        zfree(zl);
    }
}

static unsigned char *pop(unsigned char *zl, int where) {
    unsigned char *p, *vstr;
    unsigned int vlen;
    long long vlong;

    p = ziplistIndex(zl, where == ZIPLIST_HEAD ? 0 : -1);
    if (ziplistGet(p, &vstr, &vlen, &vlong)) {
        if (where == ZIPLIST_HEAD)
            printf("Pop head: ");
        else
            printf("Pop tail: ");

        if (vstr) {
            if (vlen && fwrite(vstr, vlen, 1, stdout) == 0)
                perror("fwrite");
        }
        else {
            printf("%lld", vlong);
        }

        printf("\n");
        return ziplistDelete(zl, &p);
    }
    else {
        printf("ERROR: Could not pop\n");
        exit(1);
    }
}

static int randstring(char *target, unsigned int min, unsigned int max) {
    int p = 0;
    int len = min + rand() % (max - min + 1);
    int minval, maxval;
    switch (rand() % 3) {
        case 0:
            minval = 0;
            maxval = 255;
            break;
        case 1:
            minval = 48;
            maxval = 122;
            break;
        case 2:
            minval = 48;
            maxval = 52;
            break;
        default:
            assert(NULL);
    }

    while (p < len) target[p++] = minval + rand() % (maxval - minval + 1);
    return len;
}

static void verify(unsigned char *zl, zlentry *e) {
    int len = ziplistLen(zl);
    zlentry _e;

    ZIPLIST_ENTRY_ZERO(&_e);

    for (int i = 0; i < len; i++) {
        memset(&e[i], 0, sizeof(zlentry));
        zipEntry(ziplistIndex(zl, i), &e[i]);

        memset(&_e, 0, sizeof(zlentry));
        zipEntry(ziplistIndex(zl, -len + i), &_e);

        assert(memcmp(&e[i], &_e, sizeof(zlentry)) == 0);
    }
}

static unsigned char *insertHelper(unsigned char *zl, char ch, size_t len, unsigned char *pos) {
    assert(len <= ZIP_BIG_PREVLEN);
    unsigned char data[ZIP_BIG_PREVLEN] = {0};
    memset(data, ch, len);
    return ziplistInsert(zl, pos, data, len);
}

static int compareHelper(unsigned char *zl, char ch, size_t len, int index) {
    assert(len <= ZIP_BIG_PREVLEN);
    unsigned char data[ZIP_BIG_PREVLEN] = {0};
    memset(data, ch, len);
    unsigned char *p = ziplistIndex(zl, index);
    assert(p != NULL);
    return ziplistCompare(p, data, len);
}

static size_t strEntryBytesSmall(size_t slen) {
    return slen + zipStorePrevEntryLength(NULL, 0) + zipStoreEntryEncoding(NULL, 0, slen);
}

static size_t strEntryBytesLarge(size_t slen) {
    return slen + zipStorePrevEntryLength(NULL, ZIP_BIG_PREVLEN) + zipStoreEntryEncoding(NULL, 0, slen);
}

/* ./redis-server test ziplist <randomseed> */
int ziplistTest(int argc, char **argv, int flags) {
    int accurate = (flags & REDIS_TEST_ACCURATE);
    unsigned char *zl, *p;
    unsigned char *entry;
    unsigned int elen;
    long long value;
    int iteration;

    /* If an argument is given, use it as the random seed. */
    if (argc >= 4)
        srand(atoi(argv[3]));

    zl = createIntList();
    ziplistRepr(zl);

    zfree(zl);

    zl = createList();
    ziplistRepr(zl);

    zl = pop(zl, ZIPLIST_TAIL);
    ziplistRepr(zl);

    zl = pop(zl, ZIPLIST_HEAD);
    ziplistRepr(zl);

    zl = pop(zl, ZIPLIST_TAIL);
    ziplistRepr(zl);

    zl = pop(zl, ZIPLIST_TAIL);
    ziplistRepr(zl);

    zfree(zl);

    printf("Get element at index 3:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 3);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("ERROR: Could not access index 3\n");
            return 1;
        }
        if (entry) {
            if (elen && fwrite(entry, elen, 1, stdout) == 0)
                perror("fwrite");
            printf("\n");
        }
        else {
            printf("%lld\n", value);
        }
        printf("\n");
        zfree(zl);
    }

    printf("Get element at index 4 (out of range):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 4);
        if (p == NULL) {
            printf("No entry\n");
        }
        else {
            printf("ERROR: Out of range index should return NULL, returned offset: %ld\n", (long)(p - zl));
            return 1;
        }
        printf("\n");
        zfree(zl);
    }

    printf("Get element at index -1 (last element):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -1);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("ERROR: Could not access index -1\n");
            return 1;
        }
        if (entry) {
            if (elen && fwrite(entry, elen, 1, stdout) == 0)
                perror("fwrite");
            printf("\n");
        }
        else {
            printf("%lld\n", value);
        }
        printf("\n");
        zfree(zl);
    }

    printf("Get element at index -4 (first element):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -4);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("ERROR: Could not access index -4\n");
            return 1;
        }
        if (entry) {
            if (elen && fwrite(entry, elen, 1, stdout) == 0)
                perror("fwrite");
            printf("\n");
        }
        else {
            printf("%lld\n", value);
        }
        printf("\n");
        zfree(zl);
    }

    printf("Get element at index -5 (reverse out of range):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -5);
        if (p == NULL) {
            printf("No entry\n");
        }
        else {
            printf("ERROR: Out of range index should return NULL, returned offset: %ld\n", (long)(p - zl));
            return 1;
        }
        printf("\n");
        zfree(zl);
    }

    printf("Iterate list from 0 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 0);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry, elen, 1, stdout) == 0)
                    perror("fwrite");
            }
            else {
                printf("%lld", value);
            }
            p = ziplistNext(zl, p);
            printf("\n");
        }
        printf("\n");
        zfree(zl);
    }

    printf("Iterate list from 1 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 1);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry, elen, 1, stdout) == 0)
                    perror("fwrite");
            }
            else {
                printf("%lld", value);
            }
            p = ziplistNext(zl, p);
            printf("\n");
        }
        printf("\n");
        zfree(zl);
    }

    printf("Iterate list from 2 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 2);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry, elen, 1, stdout) == 0)
                    perror("fwrite");
            }
            else {
                printf("%lld", value);
            }
            p = ziplistNext(zl, p);
            printf("\n");
        }
        printf("\n");
        zfree(zl);
    }

    printf("Iterate starting out of range:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 4);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("No entry\n");
        }
        else {
            printf("ERROR\n");
        }
        printf("\n");
        zfree(zl);
    }

    printf("Iterate from back to front:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -1);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry, elen, 1, stdout) == 0)
                    perror("fwrite");
            }
            else {
                printf("%lld", value);
            }
            p = ziplistPrev(zl, p);
            printf("\n");
        }
        printf("\n");
        zfree(zl);
    }

    printf("Iterate from back to front, deleting all items:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -1);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry, elen, 1, stdout) == 0)
                    perror("fwrite");
            }
            else {
                printf("%lld", value);
            }
            zl = ziplistDelete(zl, &p);
            p = ziplistPrev(zl, p);
            printf("\n");
        }
        printf("\n");
        zfree(zl);
    }

    printf("Delete inclusive range 0,0:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 0, 1);
        ziplistRepr(zl);
        zfree(zl);
    }

    printf("Delete inclusive range 0,1:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 0, 2);
        ziplistRepr(zl);
        zfree(zl);
    }

    printf("Delete inclusive range 1,2:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 1, 2);
        ziplistRepr(zl);
        zfree(zl);
    }

    printf("Delete with start index out of range:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 5, 1);
        ziplistRepr(zl);
        zfree(zl);
    }

    printf("Delete with num overflow:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 1, 5);
        ziplistRepr(zl);
        zfree(zl);
    }

    printf("Delete foo while iterating:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 0);
        while (ziplistGet(p, &entry, &elen, &value)) {
            if (entry && strncmp("foo", (char *)entry, elen) == 0) {
                printf("Delete foo\n");
                zl = ziplistDelete(zl, &p);
            }
            else {
                printf("Entry: ");
                if (entry) {
                    if (elen && fwrite(entry, elen, 1, stdout) == 0)
                        perror("fwrite");
                }
                else {
                    printf("%lld", value);
                }
                p = ziplistNext(zl, p);
                printf("\n");
            }
        }
        printf("\n");
        ziplistRepr(zl);
        zfree(zl);
    }

    printf("Replace with same size:\n");
    {
        zl = createList(); /* "hello", "foo", "quux", "1024" */
        unsigned char *orig_zl = zl;
        p = ziplistIndex(zl, 0);
        zl = ziplistReplace(zl, p, (unsigned char *)"zoink", 5);
        p = ziplistIndex(zl, 3);
        zl = ziplistReplace(zl, p, (unsigned char *)"yy", 2);
        p = ziplistIndex(zl, 1);
        zl = ziplistReplace(zl, p, (unsigned char *)"65536", 5);
        p = ziplistIndex(zl, 0);
        assert(!memcmp(
            (char *)p,
            "\x00\x05zoink"
            "\x07\xf0\x00\x00\x01" /* 65536 as int24 */
            "\x05\x04quux"
            "\x06\x02yy"
            "\xff",
            23));
        assert(zl == orig_zl); /* no reallocations have happened */
        zfree(zl);
        printf("SUCCESS\n\n");
    }

    printf("Replace with different size:\n");
    {
        zl = createList(); /* "hello", "foo", "quux", "1024" */
        p = ziplistIndex(zl, 1);
        zl = ziplistReplace(zl, p, (unsigned char *)"squirrel", 8);
        p = ziplistIndex(zl, 0);
        assert(!strncmp(
            (char *)p,
            "\x00\x05hello"
            "\x07\x08squirrel"
            "\x0a\x04quux"
            "\x06\xc0\x00\x04"
            "\xff",
            28));
        zfree(zl);
        printf("SUCCESS\n\n");
    }

    printf("Regression test for >255 byte strings:\n");
    {
        char v1[257] = {0}, v2[257] = {0};
        memset(v1, 'x', 256);
        memset(v2, 'y', 256);
        zl = ziplistNew();
        zl = ziplistPush(zl, (unsigned char *)v1, strlen(v1), ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)v2, strlen(v2), ZIPLIST_TAIL);

        /* Pop values again and compare their value. */
        p = ziplistIndex(zl, 0);
        assert(ziplistGet(p, &entry, &elen, &value));
        assert(strncmp(v1, (char *)entry, elen) == 0);
        p = ziplistIndex(zl, 1);
        assert(ziplistGet(p, &entry, &elen, &value));
        assert(strncmp(v2, (char *)entry, elen) == 0);
        printf("SUCCESS\n\n");
        zfree(zl);
    }

    printf("Regression test deleting next to last entries:\n");
    {
        char v[3][257] = {{0}};
        zlentry e[3] = {{.prevrawlensize = 0, .prevrawlen = 0, .lensize = 0, .len = 0, .headersize = 0, .encoding = 0, .p = NULL}};
        size_t i;

        for (i = 0; i < (sizeof(v) / sizeof(v[0])); i++) {
            memset(v[i], 'a' + i, sizeof(v[0]));
        }

        v[0][256] = '\0';
        v[1][1] = '\0';
        v[2][256] = '\0';

        zl = ziplistNew();
        for (i = 0; i < (sizeof(v) / sizeof(v[0])); i++) {
            zl = ziplistPush(zl, (unsigned char *)v[i], strlen(v[i]), ZIPLIST_TAIL);
        }

        verify(zl, e);

        assert(e[0].prevrawlensize == 1);
        assert(e[1].prevrawlensize == 5);
        assert(e[2].prevrawlensize == 1);

        /* Deleting entry 1 will increase `prevrawlensize` for entry 2 */
        unsigned char *p = e[1].p;
        zl = ziplistDelete(zl, &p);

        verify(zl, e);

        assert(e[0].prevrawlensize == 1);
        assert(e[1].prevrawlensize == 5);

        printf("SUCCESS\n\n");
        zfree(zl);
    }

    printf("Create long list and check indices:\n");
    {
        unsigned long long start = usec();
        zl = ziplistNew();
        char buf[32];
        int i, len;
        for (i = 0; i < 1000; i++) {
            len = sprintf(buf, "%d", i);
            zl = ziplistPush(zl, (unsigned char *)buf, len, ZIPLIST_TAIL);
        }
        for (i = 0; i < 1000; i++) {
            p = ziplistIndex(zl, i);
            assert(ziplistGet(p, NULL, NULL, &value));
            assert(i == value);

            p = ziplistIndex(zl, -i - 1);
            assert(ziplistGet(p, NULL, NULL, &value));
            assert(999 - i == value);
        }
        printf("SUCCESS. usec=%lld\n\n", usec() - start);
        zfree(zl);
    }

    printf("Compare strings with ziplist entries:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 0);
        if (!ziplistCompare(p, (unsigned char *)"hello", 5)) {
            printf("ERROR: not \"hello\"\n");
            return 1;
        }
        if (ziplistCompare(p, (unsigned char *)"hella", 5)) {
            printf("ERROR: \"hella\"\n");
            return 1;
        }

        p = ziplistIndex(zl, 3);
        if (!ziplistCompare(p, (unsigned char *)"1024", 4)) {
            printf("ERROR: not \"1024\"\n");
            return 1;
        }
        if (ziplistCompare(p, (unsigned char *)"1025", 4)) {
            printf("ERROR: \"1025\"\n");
            return 1;
        }
        printf("SUCCESS\n\n");
        zfree(zl);
    }

    printf("Merge test:\n");
    {
        /* create list gives us: [hello, foo, quux, 1024] */
        zl = createList();
        unsigned char *zl2 = createList();

        unsigned char *zl3 = ziplistNew();
        unsigned char *zl4 = ziplistNew();

        if (ziplistMerge(&zl4, &zl4)) {
            printf("ERROR: Allowed merging of one ziplist into itself.\n");
            return 1;
        }

        /* Merge two empty ziplists, get empty result back. */
        zl4 = ziplistMerge(&zl3, &zl4);
        ziplistRepr(zl4);
        if (ziplistLen(zl4)) {
            printf("ERROR: Merging two empty ziplists created entries.\n");
            return 1;
        }
        zfree(zl4);

        zl2 = ziplistMerge(&zl, &zl2);
        /* merge gives us: [hello, foo, quux, 1024, hello, foo, quux, 1024] */
        ziplistRepr(zl2);

        if (ziplistLen(zl2) != 8) {
            printf("ERROR: Merged length not 8, but: %u\n", ziplistLen(zl2));
            return 1;
        }

        p = ziplistIndex(zl2, 0);
        if (!ziplistCompare(p, (unsigned char *)"hello", 5)) {
            printf("ERROR: not \"hello\"\n");
            return 1;
        }
        if (ziplistCompare(p, (unsigned char *)"hella", 5)) {
            printf("ERROR: \"hella\"\n");
            return 1;
        }

        p = ziplistIndex(zl2, 3);
        if (!ziplistCompare(p, (unsigned char *)"1024", 4)) {
            printf("ERROR: not \"1024\"\n");
            return 1;
        }
        if (ziplistCompare(p, (unsigned char *)"1025", 4)) {
            printf("ERROR: \"1025\"\n");
            return 1;
        }

        p = ziplistIndex(zl2, 4);
        if (!ziplistCompare(p, (unsigned char *)"hello", 5)) {
            printf("ERROR: not \"hello\"\n");
            return 1;
        }
        if (ziplistCompare(p, (unsigned char *)"hella", 5)) {
            printf("ERROR: \"hella\"\n");
            return 1;
        }

        p = ziplistIndex(zl2, 7);
        if (!ziplistCompare(p, (unsigned char *)"1024", 4)) {
            printf("ERROR: not \"1024\"\n");
            return 1;
        }
        if (ziplistCompare(p, (unsigned char *)"1025", 4)) {
            printf("ERROR: \"1025\"\n");
            return 1;
        }
        printf("SUCCESS\n\n");
        zfree(zl);
    }

    printf("Stress with random payloads of different encoding:\n");
    {
        unsigned long long start = usec();
        int i, j, len, where;
        unsigned char *p;
        char buf[1024];
        int buflen;
        list *ref;
        listNode *refnode;

        /* Hold temp vars from ziplist */
        unsigned char *sstr;
        unsigned int slen;
        long long sval;

        iteration = accurate ? 20000 : 20;
        for (i = 0; i < iteration; i++) {
            zl = ziplistNew();
            ref = listCreate();
            listSetFreeMethod(ref, (void (*)(void *))sdsfree);
            len = rand() % 256;

            /* Create lists */
            for (j = 0; j < len; j++) {
                where = (rand() & 1) ? ZIPLIST_HEAD : ZIPLIST_TAIL;
                if (rand() % 2) {
                    buflen = randstring(buf, 1, sizeof(buf) - 1);
                }
                else {
                    switch (rand() % 3) {
                        case 0:
                            buflen = sprintf(buf, "%lld", (0LL + rand()) >> 20);
                            break;
                        case 1:
                            buflen = sprintf(buf, "%lld", (0LL + rand()));
                            break;
                        case 2:
                            buflen = sprintf(buf, "%lld", (0LL + rand()) << 20);
                            break;
                        default:
                            assert(NULL);
                    }
                }

                /* Add to ziplist */
                zl = ziplistPush(zl, (unsigned char *)buf, buflen, where);

                /* Add to reference list */
                if (where == ZIPLIST_HEAD) {
                    listAddNodeHead(ref, sdsnewlen(buf, buflen));
                }
                else if (where == ZIPLIST_TAIL) {
                    listAddNodeTail(ref, sdsnewlen(buf, buflen));
                }
                else {
                    assert(NULL);
                }
            }

            assert(listLength(ref) == ziplistLen(zl));
            for (j = 0; j < len; j++) {
                /* Naive way to get elements, but similar to the stresser
                 * executed from the Tcl test suite. */
                p = ziplistIndex(zl, j);
                refnode = listIndex(ref, j);

                assert(ziplistGet(p, &sstr, &slen, &sval));
                if (sstr == NULL) {
                    buflen = sprintf(buf, "%lld", sval);
                }
                else {
                    buflen = slen;
                    memcpy(buf, sstr, buflen);
                    buf[buflen] = '\0';
                }
                assert(memcmp(buf, listNodeValue(refnode), buflen) == 0);
            }
            zfree(zl);
            listRelease(ref);
        }
        printf("Done. usec=%lld\n\n", usec() - start);
    }

    printf("Stress with variable ziplist size:\n");
    {
        unsigned long long start = usec();
        int maxsize = accurate ? 16384 : 16;
        stress(ZIPLIST_HEAD, 100000, maxsize, 256);
        stress(ZIPLIST_TAIL, 100000, maxsize, 256);
        printf("Done. usec=%lld\n\n", usec() - start);
    }

    /* Benchmarks */
    {
        zl = ziplistNew();
        iteration = accurate ? 100000 : 100;
        for (int i = 0; i < iteration; i++) {
            char buf[4096] = "asdf";
            zl = ziplistPush(zl, (unsigned char *)buf, 4, ZIPLIST_TAIL);
            zl = ziplistPush(zl, (unsigned char *)buf, 40, ZIPLIST_TAIL);
            zl = ziplistPush(zl, (unsigned char *)buf, 400, ZIPLIST_TAIL);
            zl = ziplistPush(zl, (unsigned char *)buf, 4000, ZIPLIST_TAIL);
            zl = ziplistPush(zl, (unsigned char *)"1", 1, ZIPLIST_TAIL);
            zl = ziplistPush(zl, (unsigned char *)"10", 2, ZIPLIST_TAIL);
            zl = ziplistPush(zl, (unsigned char *)"100", 3, ZIPLIST_TAIL);
            zl = ziplistPush(zl, (unsigned char *)"1000", 4, ZIPLIST_TAIL);
            zl = ziplistPush(zl, (unsigned char *)"10000", 5, ZIPLIST_TAIL);
            zl = ziplistPush(zl, (unsigned char *)"100000", 6, ZIPLIST_TAIL);
        }

        printf("Benchmark ziplistFind:\n");
        {
            unsigned long long start = usec();
            for (int i = 0; i < 2000; i++) {
                unsigned char *fptr = ziplistIndex(zl, ZIPLIST_HEAD);
                fptr = ziplistFind(zl, fptr, (unsigned char *)"nothing", 7, 1);
            }
            printf("%lld\n", usec() - start);
        }

        printf("Benchmark ziplistIndex:\n");
        {
            unsigned long long start = usec();
            for (int i = 0; i < 2000; i++) {
                ziplistIndex(zl, 99999);
            }
            printf("%lld\n", usec() - start);
        }

        printf("Benchmark ziplistValidateIntegrity:\n");
        {
            unsigned long long start = usec();
            for (int i = 0; i < 2000; i++) {
                ziplistValidateIntegrity(zl, ziplistBlobLen(zl), 1, NULL, NULL);
            }
            printf("%lld\n", usec() - start);
        }

        printf("Benchmark ziplistCompare with string\n");
        {
            unsigned long long start = usec();
            for (int i = 0; i < 2000; i++) {
                unsigned char *eptr = ziplistIndex(zl, 0);
                while (eptr != NULL) {
                    ziplistCompare(eptr, (unsigned char *)"nothing", 7);
                    eptr = ziplistNext(zl, eptr);
                }
            }
            printf("Done. usec=%lld\n", usec() - start);
        }

        printf("Benchmark ziplistCompare with number\n");
        {
            unsigned long long start = usec();
            for (int i = 0; i < 2000; i++) {
                unsigned char *eptr = ziplistIndex(zl, 0);
                while (eptr != NULL) {
                    ziplistCompare(eptr, (unsigned char *)"99999", 5);
                    eptr = ziplistNext(zl, eptr);
                }
            }
            printf("Done. usec=%lld\n", usec() - start);
        }

        zfree(zl);
    }

    printf("Stress __ziplistCascadeUpdate:\n");
    {
        char data[ZIP_BIG_PREVLEN];
        zl = ziplistNew();
        iteration = accurate ? 100000 : 100;
        for (int i = 0; i < iteration; i++) {
            zl = ziplistPush(zl, (unsigned char *)data, ZIP_BIG_PREVLEN - 4, ZIPLIST_TAIL);
        }
        unsigned long long start = usec();
        zl = ziplistPush(zl, (unsigned char *)data, ZIP_BIG_PREVLEN - 3, ZIPLIST_HEAD);
        printf("Done. usec=%lld\n\n", usec() - start);
        zfree(zl);
    }

    printf("Edge cases of __ziplistCascadeUpdate:\n");
    {
        /* Inserting a entry with data length greater than ZIP_BIG_PREVLEN-4
         * will leads to cascade update. */
        size_t s1 = ZIP_BIG_PREVLEN - 4, s2 = ZIP_BIG_PREVLEN - 3;
        zl = ziplistNew();

        zlentry e[4] = {{.prevrawlensize = 0, .prevrawlen = 0, .lensize = 0, .len = 0, .headersize = 0, .encoding = 0, .p = NULL}};

        zl = insertHelper(zl, 'a', s1, ZIPLIST_ENTRY_HEAD(zl));
        verify(zl, e);

        assert(e[0].prevrawlensize == 1 && e[0].prevrawlen == 0);
        assert(compareHelper(zl, 'a', s1, 0));
        ziplistRepr(zl);

        /* No expand. */
        zl = insertHelper(zl, 'b', s1, ZIPLIST_ENTRY_HEAD(zl));
        verify(zl, e);

        assert(e[0].prevrawlensize == 1 && e[0].prevrawlen == 0);
        assert(compareHelper(zl, 'b', s1, 0));

        assert(e[1].prevrawlensize == 1 && e[1].prevrawlen == strEntryBytesSmall(s1));
        assert(compareHelper(zl, 'a', s1, 1));

        ziplistRepr(zl);

        /* Expand(tail included). */
        zl = insertHelper(zl, 'c', s2, ZIPLIST_ENTRY_HEAD(zl));
        verify(zl, e);

        assert(e[0].prevrawlensize == 1 && e[0].prevrawlen == 0);
        assert(compareHelper(zl, 'c', s2, 0));

        assert(e[1].prevrawlensize == 5 && e[1].prevrawlen == strEntryBytesSmall(s2));
        assert(compareHelper(zl, 'b', s1, 1));

        assert(e[2].prevrawlensize == 5 && e[2].prevrawlen == strEntryBytesLarge(s1));
        assert(compareHelper(zl, 'a', s1, 2));

        ziplistRepr(zl);

        /* Expand(only previous head entry). */
        zl = insertHelper(zl, 'd', s2, ZIPLIST_ENTRY_HEAD(zl));
        verify(zl, e);

        assert(e[0].prevrawlensize == 1 && e[0].prevrawlen == 0);
        assert(compareHelper(zl, 'd', s2, 0));

        assert(e[1].prevrawlensize == 5 && e[1].prevrawlen == strEntryBytesSmall(s2));
        assert(compareHelper(zl, 'c', s2, 1));

        assert(e[2].prevrawlensize == 5 && e[2].prevrawlen == strEntryBytesLarge(s2));
        assert(compareHelper(zl, 'b', s1, 2));

        assert(e[3].prevrawlensize == 5 && e[3].prevrawlen == strEntryBytesLarge(s1));
        assert(compareHelper(zl, 'a', s1, 3));

        ziplistRepr(zl);

        /* Delete from mid. */
        unsigned char *p = ziplistIndex(zl, 2);
        zl = ziplistDelete(zl, &p);
        verify(zl, e);

        assert(e[0].prevrawlensize == 1 && e[0].prevrawlen == 0);
        assert(compareHelper(zl, 'd', s2, 0));

        assert(e[1].prevrawlensize == 5 && e[1].prevrawlen == strEntryBytesSmall(s2));
        assert(compareHelper(zl, 'c', s2, 1));

        assert(e[2].prevrawlensize == 5 && e[2].prevrawlen == strEntryBytesLarge(s2));
        assert(compareHelper(zl, 'a', s1, 2));

        ziplistRepr(zl);

        zfree(zl);
    }

    printf("__ziplistInsert nextdiff == -4 && reqlen < 4 (issue #7170):\n");
    {
        zl = ziplistNew();

        /* We set some values to almost reach the critical point - 254 */
        char A_252[253] = {0}, A_250[251] = {0};
        memset(A_252, 'A', 252);
        memset(A_250, 'A', 250);

        /* After the rpush, the list look like: [one two A_252 A_250 three 10] */
        zl = ziplistPush(zl, (unsigned char *)"one", 3, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)"two", 3, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)A_252, strlen(A_252), ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)A_250, strlen(A_250), ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)"three", 5, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)"10", 2, ZIPLIST_TAIL);
        ziplistRepr(zl);

        p = ziplistIndex(zl, 2);
        if (!ziplistCompare(p, (unsigned char *)A_252, strlen(A_252))) {
            printf("ERROR: not \"A_252\"\n");
            return 1;
        }

        /* When we remove A_252, the list became: [one two A_250 three 10]
         * A_250's prev node became node two, because node two quite small
         * So A_250's prevlenSize shrink to 1, A_250's total size became 253(1+2+250)
         * The prev node of node three is still node A_250.
         * We will not shrink the node three's prevlenSize, keep it at 5 bytes */
        zl = ziplistDelete(zl, &p);
        ziplistRepr(zl);

        p = ziplistIndex(zl, 3);
        if (!ziplistCompare(p, (unsigned char *)"three", 5)) {
            printf("ERROR: not \"three\"\n");
            return 1;
        }

        /* We want to insert a node after A_250, the list became: [one two A_250 10 three 10]
         * Because the new node is quite small, node three prevlenSize will shrink to 1 */
        zl = ziplistInsert(zl, p, (unsigned char *)"10", 2);
        ziplistRepr(zl);

        /* Last element should equal 10 */
        p = ziplistIndex(zl, -1);
        if (!ziplistCompare(p, (unsigned char *)"10", 2)) {
            printf("ERROR: not \"10\"\n");
            return 1;
        }

        zfree(zl);
    }

    printf("ALL TESTS PASSED!\n");
    return 0;
}
#endif
