#ifndef __INTSET_H
#define __INTSET_H

#include <stdint.h>

typedef struct intset {
    uint32_t encoding;   // 编码方式  INTSET_ENC_INT64、INTSET_ENC_INT32、INTSET_ENC_INT16
    uint32_t length;     // 集合包含的元素数量
    int8_t   contents[]; // 保存元素的数组,  实际存储的值可能是int16、int32、int64 ..  但是使用int8存储 ,例如 int16用2个int8存储
    // 只支持升级、不支持降级
} intset;

// 创建一个新的压缩列表
intset *intsetNew(void);

// 将给顶元素添加到整数集合中
intset *intsetAdd(intset *is, int64_t value, uint8_t *success);

// 从整数集合中移除给定元素
intset *intsetRemove(intset *is, int64_t value, int *success);

// 检查给定值是否存在于集合  二分查找
uint8_t intsetFind(intset *is, int64_t value);

// 从整数集合中随机返回一个元素
int64_t intsetRandom(intset *is);

// 取出底层数组在给定索引上的元素
uint8_t intsetGet(intset *is, uint32_t pos, int64_t *value);

// 返回整数集合包含的元素个数
uint32_t intsetLen(const intset *is);

// 返回整数集合占用的内存字节数
size_t intsetBlobLen(intset *is);

int intsetValidateIntegrity(const unsigned char *is, size_t size, int deep);

#ifdef REDIS_TEST
int intsetTest(int argc, char *argv[], int flags);
#endif

#endif // __INTSET_H
