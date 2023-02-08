#ifndef _REDIS_DEBUGMACRO_H_
#define _REDIS_DEBUGMACRO_H_

#include <stdio.h>

#define D(...)                                                    \
    do {                                                          \
        FILE *fp = fopen("/tmp/log.txt", "a");                    \
        fprintf(fp, "%s:%s:%d:\t", __FILE__, __func__, __LINE__); \
        fprintf(fp, __VA_ARGS__);                                 \
        fprintf(fp, "\n");                                        \
        fclose(fp);                                               \
    } while (0)

#endif /* _REDIS_DEBUGMACRO_H_ */
