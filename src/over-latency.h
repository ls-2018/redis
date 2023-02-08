// 实现了异步删除的功能,所以这样,我们就可以使用后台 IO 线程来完成删除,以避免对 Redis 主线程的影响.
// 实现了操作延迟监控的功能
#ifndef __LATENCY_H
#define __LATENCY_H

#define LATENCY_TS_LEN 160 // 每种事件最多保存160个记录

struct latencySample {
    int32_t time;     // 事件的采样时间
    uint32_t latency; // 事件的执行时长(以毫秒为单位)
};

struct latencyTimeSeries {
    int idx;                                      // 采样事件数组的写入位置
    uint32_t max;                                 // 当前事件的最大延迟
    struct latencySample samples[LATENCY_TS_LEN]; // 采样事件 数组
};

struct latencyStats {
    uint32_t all_time_high; /* Absolute max observed since latest reset. */
    uint32_t avg;           /* Average of current samples. */
    uint32_t min;           /* Min of current samples. */
    uint32_t max;           /* Max of current samples. */
    uint32_t mad;           /* Mean absolute deviation. */
    uint32_t samples;       /* Number of non-zero samples. */
    time_t period;          /* Number of seconds since first event and now. */
};

void latencyMonitorInit(void);

void latencyAddSample(const char *event, mstime_t latency);

int THPIsEnabled(void);

int THPDisable(void);

/* Latency monitoring macros. */

// 开始监测一个事件。我们只是设置了当前的时间。
#define latencyStartMonitor(var)            \
    if (server.latency_monitor_threshold) { \
        var = mstime();                     \
    }                                       \
    else {                                  \
        var = 0;                            \
    }

/* End monitoring an event, compute the difference with the current time
 * to check the amount of time elapsed. */
#define latencyEndMonitor(var)              \
    if (server.latency_monitor_threshold) { \
        var = mstime() - var;               \
    }

// 添加延迟样本数据
#define latencyAddSampleIfNeeded(event, var)                                           \
    if (server.latency_monitor_threshold && (var) >= server.latency_monitor_threshold) \
        latencyAddSample((event), (var));

/* Remove time from a nested event. */
#define latencyRemoveNestedEvent(event_var, nested_var) event_var += nested_var;

#endif /* __LATENCY_H */
