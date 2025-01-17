// over
#include "over-monotonic.h"
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#undef NDEBUG

#include <assert.h>

// 获取时钟的函数指针
monotime (*getMonotonicUs)(void) = NULL;

static char monotonic_info_string[32];

// 使用处理器时钟(又名x86上的TSC)可以提高整个Redis的性能,无论在哪里使用onotonic时钟.
// 处理器时钟明显快于调用'clock_getting' (POSIX).

#if defined(USE_PROCESSOR_CLOCK) && defined(__x86_64__) && defined(__linux__)
#    include <regex.h>
#    include <x86intrin.h>

static long mono_ticksPerMicrosecond = 0;

static monotime getMonotonicUs_x86() {
    return __rdtsc() / mono_ticksPerMicrosecond;
}

static void monotonicInit_x86linux() {
    const int bufflen = 256;
    char buf[bufflen];
    regex_t cpuGhzRegex, constTscRegex;
    const size_t nmatch = 2;
    regmatch_t pmatch[nmatch];
    int constantTsc = 0;
    int rc;

    /* Determine the number of TSC ticks in a micro-second.  This is
     * a constant value matching the standard speed of the processor.
     * On modern processors, this speed remains constant even though
     * the actual clock speed varies dynamically for each core.  */
    rc = regcomp(&cpuGhzRegex, "^model name\\s+:.*@ ([0-9.]+)GHz", REG_EXTENDED);
    assert(rc == 0);

    /* Also check that the constant_tsc flag is present.  (It should be
     * unless this is a really old CPU.  */
    rc = regcomp(&constTscRegex, "^flags\\s+:.* constant_tsc", REG_EXTENDED);
    assert(rc == 0);

    FILE *cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo != NULL) {
        while (fgets(buf, bufflen, cpuinfo) != NULL) {
            if (regexec(&cpuGhzRegex, buf, nmatch, pmatch, 0) == 0) {
                buf[pmatch[1].rm_eo] = '\0';
                double ghz = atof(&buf[pmatch[1].rm_so]);
                mono_ticksPerMicrosecond = (long)(ghz * 1000);
                break;
            }
        }
        while (fgets(buf, bufflen, cpuinfo) != NULL) {
            if (regexec(&constTscRegex, buf, nmatch, pmatch, 0) == 0) {
                constantTsc = 1;
                break;
            }
        }

        fclose(cpuinfo);
    }
    regfree(&cpuGhzRegex);
    regfree(&constTscRegex);

    if (mono_ticksPerMicrosecond == 0) {
        fprintf(stderr, "monotonic: x86 linux, unable to determine clock rate");
        return;
    }
    if (!constantTsc) {
        fprintf(stderr, "monotonic: x86 linux, 'constant_tsc' flag not present");
        return;
    }

    snprintf(monotonic_info_string, sizeof(monotonic_info_string), "X86 TSC @ %ld ticks/us", mono_ticksPerMicrosecond);
    getMonotonicUs = getMonotonicUs_x86;
}
#endif

#if defined(USE_PROCESSOR_CLOCK) && defined(__aarch64__)
static long mono_ticksPerMicrosecond = 0;

/* Read the clock value.  */
static inline uint64_t __cntvct() {
    uint64_t virtual_timer_value;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(virtual_timer_value));
    return virtual_timer_value;
}

/* Read the Count-timer Frequency.  */
static inline uint32_t cntfrq_hz() {
    uint64_t virtual_freq_value;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(virtual_freq_value));
    return (uint32_t)virtual_freq_value; /* top 32 bits are reserved */
}

static monotime getMonotonicUs_aarch64() {
    return __cntvct() / mono_ticksPerMicrosecond;
}

static void monotonicInit_aarch64() {
    mono_ticksPerMicrosecond = (long)cntfrq_hz() / 1000L / 1000L;
    if (mono_ticksPerMicrosecond == 0) {
        fprintf(stderr, "monotonic: aarch64, unable to determine clock rate");
        return;
    }

    snprintf(monotonic_info_string, sizeof(monotonic_info_string), "ARM CNTVCT @ %ld ticks/us", mono_ticksPerMicrosecond);
    getMonotonicUs = getMonotonicUs_aarch64;
}
#endif

// 获取纳秒数,不会因为系统时间跳跃而有影响
static monotime getMonotonicUs_posix() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // 就是开机到现在的时间e
    return ((uint64_t)ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

// 初始化时间获取函数
static void monotonicInit_posix() {
    // 确保支持CLOCK_MONOTONIC.
    // 这应该在任何合理的当前操作系统上得到支持.
    // 如果下面的论断失败,请提供 一个适当的替代实现.
    struct timespec ts;

    // CLOCK_MONOTONIC: 从系统启动这一刻起开始计时,不受系统时间被用户改变的影响
    // clock_gettime: 此方法返回一个浮点值,该值表示指定时钟clk_id的时间(以秒为单位).
    int rc = clock_gettime(CLOCK_MONOTONIC, &ts);
    assert(rc == 0);
    //  snprintf(),函数原型为int snprintf(char *str, size_t size, const char *format, ...).
    //  将可变参数 “…” 按照format的格式格式化为字符串,然后再将其拷贝至str中.
    snprintf(monotonic_info_string, sizeof(monotonic_info_string), "POSIX clock_gettime");
    getMonotonicUs = getMonotonicUs_posix;
}

// 单调时钟初始化
const char *monotonicInit() {
#if defined(USE_PROCESSOR_CLOCK) && defined(__x86_64__) && defined(__linux__)
    if (getMonotonicUs == NULL)
        monotonicInit_x86linux();
#endif

#if defined(USE_PROCESSOR_CLOCK) && defined(__aarch64__)
    if (getMonotonicUs == NULL)
        monotonicInit_aarch64();
#endif

    if (getMonotonicUs == NULL) {
        monotonicInit_posix(); // 初始化时间获取函数
    }

    return monotonic_info_string;
}

const char *monotonicInfoString() {
    return monotonic_info_string;
}
// 时钟获取方式
monotonic_clock_type monotonicGetType() {
    if (getMonotonicUs == getMonotonicUs_posix)
        return MONOTONIC_CLOCK_POSIX;
    return MONOTONIC_CLOCK_HW;
}
