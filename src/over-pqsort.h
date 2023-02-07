// 快速排序算法
#ifndef __PQSORT_H
#define __PQSORT_H

void pqsort(void *a, size_t n, size_t es, int (*cmp)(const void *, const void *), size_t lrange, size_t rrange);

#endif
