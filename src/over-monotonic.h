#ifndef __MONOTONIC_H
#define __MONOTONIC_H

#include "over-fmacros.h"
#include <stdint.h>
#include <unistd.h>
typedef uint64_t monotime;

// 获取任意时间的微妙数
extern monotime (*getMonotonicUs)(void);

typedef enum monotonic_clock_type {
    MONOTONIC_CLOCK_POSIX,
    MONOTONIC_CLOCK_HW,
} monotonic_clock_type;

/*在启动时调用一次以初始化单调时钟.虽然这只是需要被调用一次,它可以被调用额外的时间没有影响.
返回一个可打印的字符串,指示初始化的时钟类型.(返回的字符串是静态的,不需要释放)*/
const char *monotonicInit();

// 返回一个字符串,指示所使用的单调时钟的类型.
const char *monotonicInfoString();

// 返回正在使用的单调时钟的类型.
monotonic_clock_type monotonicGetType();

static inline void elapsedStart(monotime *start_time) {
    *start_time = getMonotonicUs();
}

static inline uint64_t elapsedUs(monotime start_time) {
    return getMonotonicUs() - start_time;
}

static inline uint64_t elapsedMs(monotime start_time) {
    return elapsedUs(start_time) / 1000;
}

#endif
