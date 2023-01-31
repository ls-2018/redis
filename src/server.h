#ifndef __REDIS_H
#define __REDIS_H

#include "fmacros.h"
#include "config.h"
#include "solarisfixes.h"
#include "rio.h"
#include "atomicvar.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <syslog.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <lua.h>
#include <signal.h>
#include "hdr_histogram.h"

#ifdef HAVE_LIBSYSTEMD
#    include <systemd/sd-daemon.h>
#endif

#ifndef static_assert
#    define static_assert(expr, lit) extern char __static_assert_failure[(expr) ? 1 : -1]
#endif

typedef long long mstime_t; /* millisecond time type. */
typedef long long ustime_t; /* microsecond time type. */

#include "over-ae.h"    /* Event driven programming library */
#include "sds.h"        /* Dynamic safe strings */
#include "dict.h"       /* Hash tables */
#include "adlist.h"     /* Linked lists */
#include "zmalloc.h"    /* total memory usage aware version of malloc/free */
#include "anet.h"       /* Networking the easy way */
#include "ziplist.h"    /* Compact list data structure */
#include "intset.h"     /* Compact integer set structure */
#include "version.h"    /* Version macro */
#include "util.h"       /* Misc functions useful in many places */
#include "latency.h"    /* Latency monitor API */
#include "sparkline.h"  /* ASCII graphs API */
#include "quicklist.h"  /* Lists are encoded as linked lists of N-elements flat arrays */
#include "rax.h"        /* Radix tree */
#include "connection.h" /* Connection abstraction */

#define REDISMODULE_CORE 1

#include "redismodule.h" /* Redis modules API defines. */

/* Following includes allow test functions to be called from Redis main() */
#include "zipmap.h"
#include "sha1.h"
#include "endianconv.h"
#include "crc64.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

#define C_OK 0
#define C_ERR (-1)

// 默认的服务器配置值
#define CONFIG_DEFAULT_HZ 10                                  // server后台任务的默认运行频率
#define CONFIG_MIN_HZ 1                                       // server后台任务的最小运行频率
#define CONFIG_MAX_HZ 500                                     // server后台任务的最大运行频率
#define MAX_CLIENTS_PER_CLOCK_TICK 200                        /* HZ is adapted based on that. */
#define CRON_DBS_PER_CALL 16                                  //
#define NET_MAX_WRITES_PER_EVENT (1024 * 64)                  //
#define PROTO_SHARED_SELECT_CMDS 10                           //
#define OBJ_SHARED_INTEGERS 10000                             //
#define OBJ_SHARED_BULKHDR_LEN 32                             // 11110000   把32以内（不包括32）的长度的字符串做成了 常用池
#define OBJ_SHARED_HDR_STRLEN(_len_) (((_len_) < 10) ? 4 : 5) // TODO 暂时不知？ see shared.mbulkhdr etc.
#define LOG_MAX_LEN 1024                                      /* Default maximum length of syslog messages.*/
#define AOF_REWRITE_ITEMS_PER_CMD 64                          //
#define AOF_ANNOTATION_LINE_MAX_LEN 1024                      //
#define CONFIG_AUTHPASS_MAX_LEN 512                           //
#define CONFIG_RUN_ID_SIZE 40                                 // 运行时ID的长度
#define RDB_EOF_MARK_SIZE 40                                  //
#define CONFIG_REPL_BACKLOG_MIN_SIZE (1024 * 16)              /* 16k */
#define CONFIG_BGSAVE_RETRY_DELAY 5                           /* Wait a few secs before trying again. */
#define CONFIG_DEFAULT_PID_FILE "/var/run/redis.pid"          //
#define CONFIG_DEFAULT_BINDADDR_COUNT 2                       // 默认绑定的地址数量
#define CONFIG_DEFAULT_BINDADDR \
    { "*", "-::*" }          // 默认绑定的地址
#define NET_HOST_STR_LEN 256 /* Longest valid hostname */
// 0000:0000:0000:0000:0000:FFFF:111.222.212.222
// 其内容包括：
// 6组16进制整数,每组4个;
// 6个冒号;
// Ipv4地址;
// 还有一个Null结束符;
// 因此, 最大可能长度 = 6 * 4 + 6 + 16 = 46
// 所以, INET6_ADDRSTRLEN定义为46.
#define NET_IP_STR_LEN 46                             //
#define NET_ADDR_STR_LEN (NET_IP_STR_LEN + 32)        /* Must be enough for ip:port */
#define NET_HOST_PORT_STR_LEN (NET_HOST_STR_LEN + 32) /* Must be enough for hostname:port */
#define CONFIG_BINDADDR_MAX 16                        //
#define CONFIG_MIN_RESERVED_FDS 32                    // 预留的最少的文件描述符
#define CONFIG_DEFAULT_PROC_TITLE_TEMPLATE "{title} {listen-addr} {server-mode}"

/* 客户端驱逐池的桶大小.每个桶存储的客户端内存使用率高达其以下桶大小的两倍. */
#define CLIENT_MEM_USAGE_BUCKET_MIN_LOG 15                                                               /* 桶大小最高为32KB (2^15) */
#define CLIENT_MEM_USAGE_BUCKET_MAX_LOG 33                                                               /* 用于最大客户端的桶:大小超过4GB (2^32) */
#define CLIENT_MEM_USAGE_BUCKETS (1 + CLIENT_MEM_USAGE_BUCKET_MAX_LOG - CLIENT_MEM_USAGE_BUCKET_MIN_LOG) // 19

#define ACTIVE_EXPIRE_CYCLE_SLOW 0
#define ACTIVE_EXPIRE_CYCLE_FAST 1

/* Children process will exit with this status code to signal that the
 * process terminated without an error: this is useful in order to kill
 * a saving child (RDB or AOF one), without triggering in the parent the
 * write protection that is normally turned on on write errors.
 * Usually children that are terminated with SIGUSR1 will exit with this
 * special code. */
#define SERVER_CHILD_NOERROR_RETVAL 255

/* Reading copy-on-write info is sometimes expensive and may slow down child
 * processes that report it continuously. We measure the cost of obtaining it
 * and hold back additional reading based on this factor. */
#define CHILD_COW_DUTY_CYCLE 100

/* Instantaneous metrics tracking. */
#define STATS_METRIC_SAMPLES 16   /* Number of samples per metric. */
#define STATS_METRIC_COMMAND 0    /* Number of commands executed. */
#define STATS_METRIC_NET_INPUT 1  /* Bytes read to network .*/
#define STATS_METRIC_NET_OUTPUT 2 /* Bytes written to network. */
#define STATS_METRIC_COUNT 3

// 协议和IO相关的定义
#define PROTO_IOBUF_LEN (1024 * 16)         // 从客户端读取的默认数据大小
#define PROTO_REPLY_CHUNK_BYTES (16 * 1024) // 输出缓冲区 16k
#define PROTO_INLINE_MAX_SIZE (1024 * 64)   // 内联读的最大大小
#define PROTO_MBULK_BIG_ARG (1024 * 32)     //
#define PROTO_RESIZE_THRESHOLD (1024 * 32)  // 用于确定是否调整查询缓冲区大小的阈值
#define PROTO_REPLY_MIN_BYTES (1024)        // 回复缓冲区大小的下限

// AOF程序每累积这个量的写入数据就执行一次显式的 fsync
#define REDIS_AUTOSYNC_BYTES (1024 * 1024 * 4)    // 每4MB日志,强制落一次盘
#define REPLY_BUFFER_DEFAULT_PEAK_RESET_TIME 5000 // 5s

// 由于 RESERVED_FDS 默认为 32,我们增加了 96,以确保不超过 128个文件描述符.
#define CONFIG_FDSET_INCR (CONFIG_MIN_RESERVED_FDS + 96)

// OOM分数调整类
#define CONFIG_OOM_MASTER 0
#define CONFIG_OOM_REPLICA 1
#define CONFIG_OOM_BGCHILD 2
#define CONFIG_OOM_COUNT 3

extern int configOOMScoreAdjValuesDefaults[CONFIG_OOM_COUNT];

// hash表参数
#define HASHTABLE_MIN_FILL 10           // hash表最小的 填充率是10%, 小于这个值就缩容
#define HASHTABLE_MAX_LOAD_FACTOR 1.618 // 负载因子最大值,超过就扩容

// 命令标志,与redisCommand有关
#define CMD_WRITE (1ULL << 0)               // 写入命令,可能会修改 key space
#define CMD_READONLY (1ULL << 1)            // 读命令,不修改 key space
#define CMD_DENYOOM (1ULL << 2)             // 可能会占用大量内存,执行前需要先检查服务器的内存使用情况,如果内存紧缺的话就禁止执行这个命令
#define CMD_MODULE (1ULL << 3)              // 模块命令
#define CMD_ADMIN (1ULL << 4)               // 管理命令   SAVE BGSAVE  SHUTDOWN
#define CMD_PUBSUB (1ULL << 5)              // 发布于订阅功能方面的命令
#define CMD_NOSCRIPT (1ULL << 6)            // 不允许在脚本中使用的命令
#define CMD_BLOCKING (1ULL << 8)            // 该命令有可能阻塞客户端.
#define CMD_LOADING (1ULL << 9)             // 允许在载入数据库时使用的命令
#define CMD_STALE (1ULL << 10)              // 允许在从节点带有过期数据时执行的命令. 这类命令很少有,只有几个.
#define CMD_SKIP_MONITOR (1ULL << 11)       // 不要在 MONITOR 模式下自动广播的命令.
#define CMD_SKIP_SLOWLOG (1ULL << 12)       // 不要在 SLOWLOG 模式下自动广播的命令.
#define CMD_ASKING (1ULL << 13)             // 为这个命令执行一个显式的 ASKING,使得在集群模式下,一个被标示为 importing 的槽可以接收这命令.
#define CMD_FAST (1ULL << 14)               // 快速命令.O(1)或O(log(N))的命令.请注意,那些可能触发DEL作为副作用的命令（如SET）不是快速命令. (如SET)不是快速命令.
#define CMD_NO_AUTH (1ULL << 15)            // 命令不需要认证
#define CMD_MAY_REPLICATE (1ULL << 16)      // 命令可能会产生复制流量,但在不允许写命令的情况下应该允许.例如,PUBLISH（复制pubsub消息）和EVAL（可能执行复制的写命令,也可能只执行读命令）.一个命令不能同时标记为CMD_WRITE和CMD_MAY_REPLICATE.
#define CMD_SENTINEL (1ULL << 17)           // 这个命令在哨兵模式下也存在.
#define CMD_ONLY_SENTINEL (1ULL << 18)      // 这个命令仅在哨兵模式下存在.
#define CMD_NO_MANDATORY_KEYS (1ULL << 19)  // 这条命令的关键参数是可选的.
#define CMD_PROTECTED (1ULL << 20)          //
#define CMD_MODULE_GETKEYS (1ULL << 21)     /* Use the modules getkeys interface. */
#define CMD_MODULE_NO_CLUSTER (1ULL << 22)  // Redis集群时 拒绝
#define CMD_NO_ASYNC_LOADING (1ULL << 23)   // 在异步加载期间拒绝（当副本使用无盘同步swapdb时,允许访问旧数据集）.
#define CMD_NO_MULTI (1ULL << 24)           // 该命令不允许在事务中使用
#define CMD_MOVABLE_KEYS (1ULL << 25)       /* populateCommandMovableKeys函数填充的key */
#define CMD_ALLOW_BUSY ((1ULL << 26))       //
#define CMD_MODULE_GETCHANNELS (1ULL << 27) /* Use the modules getchannels interface. */

// 下面的附加标志只用于将命令放在特定的ACL类别中.
#define ACL_CATEGORY_KEYSPACE (1ULL << 0)
#define ACL_CATEGORY_READ (1ULL << 1)
#define ACL_CATEGORY_WRITE (1ULL << 2)
#define ACL_CATEGORY_SET (1ULL << 3)
#define ACL_CATEGORY_SORTEDSET (1ULL << 4)
#define ACL_CATEGORY_LIST (1ULL << 5)
#define ACL_CATEGORY_HASH (1ULL << 6)
#define ACL_CATEGORY_STRING (1ULL << 7)
#define ACL_CATEGORY_BITMAP (1ULL << 8)
#define ACL_CATEGORY_HYPERLOGLOG (1ULL << 9)
#define ACL_CATEGORY_GEO (1ULL << 10)
#define ACL_CATEGORY_STREAM (1ULL << 11)
#define ACL_CATEGORY_PUBSUB (1ULL << 12)
#define ACL_CATEGORY_ADMIN (1ULL << 13)
#define ACL_CATEGORY_FAST (1ULL << 14)
#define ACL_CATEGORY_SLOW (1ULL << 15)
#define ACL_CATEGORY_BLOCKING (1ULL << 16)
#define ACL_CATEGORY_DANGEROUS (1ULL << 17)
#define ACL_CATEGORY_CONNECTION (1ULL << 18)
#define ACL_CATEGORY_TRANSACTION (1ULL << 19)
#define ACL_CATEGORY_SCRIPTING (1ULL << 20)

/* Key-spec flags *
 * -------------- */
/* The following refer what the command actually does with the value or metadata
 * of the key, and not necessarily the user data or how it affects it.
 * Each key-spec may must have exactly one of these. Any operation that's not
 * distinctly deletion, overwrite or read-only would be marked as RW. */
#define CMD_KEY_RO (1ULL << 0) /* Read-Only - Reads the value of the key, but doesn't necessarily returns it. */
#define CMD_KEY_RW (1ULL << 1) /* Read-Write - Modifies the data stored in the value of the key or its metadata. */
#define CMD_KEY_OW (1ULL << 2) /* Overwrite - Overwrites the data stored in the value of the key. */
#define CMD_KEY_RM (1ULL << 3) /* Deletes the key. */
/* The following refer to user data inside the value of the key, not the metadata
 * like LRU, type, cardinality. It refers to the logical operation on the user's
 * data (actual input strings / TTL), being used / returned / copied / changed,
 * It doesn't refer to modification or returning of metadata (like type, count,
 * presence of data). Any write that's not INSERT or DELETE, would be an UPDATE.
 * Each key-spec may have one of the writes with or without access, or none: */
#define CMD_KEY_ACCESS (1ULL << 4) /* Returns, copies or uses the user data from the value of the key. */
#define CMD_KEY_UPDATE (1ULL << 5) /* Updates data to the value, new value may depend on the old value. */
#define CMD_KEY_INSERT (1ULL << 6) /* Adds data to the value with no chance of modification or deletion of existing data. */
#define CMD_KEY_DELETE (1ULL << 7) /* Explicitly deletes some content from the value of the key. */
/* Other flags: */
#define CMD_KEY_NOT_KEY (1ULL << 8)         /* A 'fake' key that should be routed like a key in cluster mode but is  excluded from other key checks. */
#define CMD_KEY_INCOMPLETE (1ULL << 9)      /* Means that the keyspec might not point out to all keys it should cover */
#define CMD_KEY_VARIABLE_FLAGS (1ULL << 10) /* Means that some keys might have  different flags depending on arguments */

/* Key flags for when access type is unknown */
#define CMD_KEY_FULL_ACCESS (CMD_KEY_RW | CMD_KEY_ACCESS | CMD_KEY_UPDATE)

/* Channel flags share the same flag space as the key flags */
#define CMD_CHANNEL_PATTERN (1ULL << 11)     /* The argument is a channel pattern */
#define CMD_CHANNEL_SUBSCRIBE (1ULL << 12)   /* The command subscribes to channels */
#define CMD_CHANNEL_UNSUBSCRIBE (1ULL << 13) /* The command unsubscribes to channels */
#define CMD_CHANNEL_PUBLISH (1ULL << 14)     /* The command publishes to channels. */

// AOF 状态（开启/关闭/等待重写）
#define AOF_OFF 0          // 关闭
#define AOF_ON 1           // 打开
#define AOF_WAIT_REWRITE 2 // AOF等待重写开始追加

/* AOF return values for loadAppendOnlyFile() */
#define AOF_OK 0
#define AOF_NOT_EXIST 1
#define AOF_EMPTY 2
#define AOF_OPEN_ERR 3
#define AOF_FAILED 4
#define AOF_TRUNCATED 5

// 命令标志
#define CMD_DOC_NONE 0              //
#define CMD_DOC_DEPRECATED (1 << 0) // 弃用
#define CMD_DOC_SYSCMD (1 << 1)     // 内部系统命令

// 客户端角色 标志位
#define CLIENT_SLAVE (1 << 0)               // 表示客户端是一个从节点
#define CLIENT_MASTER (1 << 1)              // 表示客户端是一个主节点
#define CLIENT_MONITOR (1 << 2)             // 表示客户端正在执行MONITOR命令
#define CLIENT_MULTI (1 << 3)               // 表示客户端正在执行事务
#define CLIENT_BLOCKED (1 << 4)             // 表示客户端正在被BRPOP、BLPOP等命令阻塞
#define CLIENT_DIRTY_CAS (1 << 5)           // 表示事务使用WATCH命令监视的数据库键已经被修改
#define CLIENT_CLOSE_AFTER_REPLY (1 << 6)   // 表示有用户对这个客户端执行了CLIENT KILL命令;或者客户端发送给服务端的命令请求中包含了错误的协议内容.服务端会将客户端积存在输出缓冲区的所有内容发送给客户端,然后关闭客户端
#define CLIENT_UNBLOCKED (1 << 7)           // 表示客户端已经从CLIENT_BLOCKED状态中脱离出来
#define CLIENT_SCRIPT (1 << 8)              // 这是lua使用的非连接 客户端
#define CLIENT_ASKING (1 << 9)              // 表示客户端向集群节点发送了ASKING命令
#define CLIENT_CLOSE_ASAP (1 << 10)         // 表示客户端的输出缓冲区大小超出了服务器允许的范围,会在下一次serverCron时关闭这个客户端, 积存在输出缓冲区中的所有内容会直接被释放,不会返回客户端
#define CLIENT_UNIX_SOCKET (1 << 11)        // 服务器使用UNIX套接字来连接客户端
#define CLIENT_DIRTY_EXEC (1 << 12)         // 表示事务在命令入队时出现了错误,  CLIENT_DIRTY_CAS|CLIENT_DIRTY_EXEC 两个标志都表示事务的安全性已被破坏,即事务执行失败  .该标志只在打开了CLIENT_MULTI情况下使用
#define CLIENT_MASTER_FORCE_REPLY (1 << 13) // 即使是主 也要队列回复
#define CLIENT_FORCE_AOF (1 << 14)          // 表示强制服务器将当前执行的命令写入到AOF文件里; 执行PUBSUB命令会打开这个标志
// 执行SCRIPT LOAD 会使客户端打开  CLIENT_FORCE_AOF、CLIENT_PRE_PSYNC这两个标志
// ·在主从服务器进行命令传播期间,从服务器需要向主服务器发送 REPLICATION ACK命令,在发送这个命令之前,从服务器必须打开主服务器对应的客户端的REDIS_MASTER_FORCE_REPLY标志,否则发 送操作会被拒绝执行.
#define CLIENT_FORCE_REPL (1 << 15)        // 表示强制服务器将当前执行的命令复制给所有从服务器;
#define CLIENT_PRE_PSYNC (1 << 16)         // 客户端是一个版本低于2.8的从服务器,主服务器不能使用psync与这个从服务器进行同步
#define CLIENT_READONLY (1 << 17)          /* Cluster client is in read-only state. */
#define CLIENT_PUBSUB (1 << 18)            /* Client is in Pub/Sub mode. */
#define CLIENT_PREVENT_AOF_PROP (1 << 19)  // 不传播到AOF文件
#define CLIENT_PREVENT_REPL_PROP (1 << 20) // 不传播到SLAVE
#define CLIENT_PREVENT_PROP (CLIENT_PREVENT_AOF_PROP | CLIENT_PREVENT_REPL_PROP)
#define CLIENT_PENDING_WRITE (1 << 21)   /* Client has output to send but a write handler is yet not installed. */
#define CLIENT_REPLY_OFF (1 << 22)       /* Don't send replies to client. */
#define CLIENT_REPLY_SKIP_NEXT (1 << 23) /* Set CLIENT_REPLY_SKIP for next cmd */
#define CLIENT_REPLY_SKIP (1 << 24)      /* Don't send just this reply. */
#define CLIENT_LUA_DEBUG (1 << 25)       /* Run EVAL in debug mode. */
#define CLIENT_LUA_DEBUG_SYNC (1 << 26)  /* EVAL debugging without fork() */
#define CLIENT_MODULE (1 << 27)          /* Non connected client used by some module. */
#define CLIENT_PROTECTED (1 << 28)       /* Client should not be freed for now. */
/* #define CLIENT_... (1<<29) currently unused, feel free to use in the future */
#define CLIENT_PENDING_COMMAND (1 << 30)          /* 指示客户端已准备好执行已完全解析的命令 */
#define CLIENT_TRACKING (1ULL << 31)              // 客户端启用了key跟踪,以便执行客户端缓存.
#define CLIENT_TRACKING_BROKEN_REDIR (1ULL << 32) /* Target client is invalid. */
#define CLIENT_TRACKING_BCAST (1ULL << 33)        // 在BCAST模式下跟踪.
#define CLIENT_TRACKING_OPTIN (1ULL << 34)        /* Tracking in opt-in mode. */
#define CLIENT_TRACKING_OPTOUT (1ULL << 35)       /* Tracking in opt-out mode. */
#define CLIENT_TRACKING_CACHING (1ULL << 36)      /* CACHING yes/no was given, depending on optin/optout mode. */
#define CLIENT_TRACKING_NOLOOP (1ULL << 37)       /* Don't send invalidation messages about writes performed by myself.*/
#define CLIENT_IN_TO_TABLE (1ULL << 38)           /* This client is in the timeout table. */
#define CLIENT_PROTOCOL_ERROR (1ULL << 39)        /* Protocol error chatting with it. */
#define CLIENT_CLOSE_AFTER_COMMAND (1ULL << 40)   /* Close after executing commands and writing entire reply. */
#define CLIENT_DENY_BLOCKING (1ULL << 41)         /* Indicate that the client should not be blocked. currently, turned on inside MULTI, Lua, RM_Call, and AOF client */
#define CLIENT_REPL_RDBONLY (1ULL << 42)          /* This client is a replica that only wants RDB without replication buffer. */
#define CLIENT_NO_EVICT (1ULL << 43)              /* This client is protected against client memory eviction. */

// 客户端阻塞类型
#define BLOCKED_NONE 0     // 没有阻塞
#define BLOCKED_LIST 1     // BLPOP等操作
#define BLOCKED_WAIT 2     // 等待同步复制
#define BLOCKED_MODULE 3   // 加载模块时阻塞
#define BLOCKED_STREAM 4   /* XREAD. */
#define BLOCKED_ZSET 5     /* BZPOP et al. */
#define BLOCKED_POSTPONE 6 /* 被processCommand函数 阻塞,稍后重试处理*/
#define BLOCKED_SHUTDOWN 7 // 停止
#define BLOCKED_NUM 8      // 阻塞状态

// 请求类型
#define PROTO_REQ_INLINE 1    // 内联型
#define PROTO_REQ_MULTIBULK 2 // 协议型

// 用于客户端限制的客户端类型,目前只用于 max-client-output-buffer
#define CLIENT_TYPE_NORMAL 0     // 正常的 请求-应答客户端、监视器
#define CLIENT_TYPE_SLAVE 1      // 从节点
#define CLIENT_TYPE_PUBSUB 2     // 客户端订阅了PubSub频道.
#define CLIENT_TYPE_MASTER 3     // 主节点
#define CLIENT_TYPE_COUNT 4      // 客户端类型数量
#define CLIENT_TYPE_OBUF_COUNT 3 // client 暴露的输出缓冲区配置 .目前只有前三个：normal, slave, pubsub.

// slave 复制的状态.在server.rep_state中使用,以便slave 记住接下来要做什么.
typedef enum {
    REPL_STATE_NONE = 0,   // 没有积极的复制
    REPL_STATE_CONNECT,    // 必须连接到master
    REPL_STATE_CONNECTING, // 链接master ing

    // 握手状态,必须下达命令
    REPL_STATE_RECEIVE_PING_REPLY,  // 等待ping的回复
    REPL_STATE_SEND_HANDSHAKE,      // 向master 发送握手序列
    REPL_STATE_RECEIVE_AUTH_REPLY,  // 等待认证回复
    REPL_STATE_RECEIVE_PORT_REPLY,  // 等待REPLCONF回复
    REPL_STATE_RECEIVE_IP_REPLY,    // 等待REPLCONF回复
    REPL_STATE_RECEIVE_CAPA_REPLY,  // 等待REPLCONF回复
    REPL_STATE_SEND_PSYNC,          // 发送 PSYNC命令
    REPL_STATE_RECEIVE_PSYNC_REPLY, // 等待PSYNC回复

    // 捂手状态 结束
    REPL_STATE_TRANSFER,  // 从master接收rdb文件
    REPL_STATE_CONNECTED, // 链接到master
} repl_state;

// 故障转移的状态
typedef enum {
    NO_FAILOVER = 0,        // 没有进行故障转移
    FAILOVER_WAIT_FOR_SYNC, // 等待目标机器追赶上进度
    FAILOVER_IN_PROGRESS    // 等待目标副本接受 PSYNC FAILOVER 请求.
} failover_state;

/* State of slaves from the POV of the master. Used in client->replstate.
 * In SEND_BULK and ONLINE state the slave receives new updates
 * in its output queue. In the WAIT_BGSAVE states instead the server is waiting
 * to start the next background saving in order to send updates to it. */
#define SLAVE_STATE_WAIT_BGSAVE_START 6 /* We need to produce a new RDB file. */
#define SLAVE_STATE_WAIT_BGSAVE_END 7   /* Waiting RDB file creation to finish. */
#define SLAVE_STATE_SEND_BULK 8         /* Sending RDB file to slave. */
#define SLAVE_STATE_ONLINE 9            /* RDB file transmitted, sending just updates. */

/* Slave capabilities. */
#define SLAVE_CAPA_NONE 0
#define SLAVE_CAPA_EOF (1 << 0)    /* Can parse the RDB EOF streaming format. */
#define SLAVE_CAPA_PSYNC2 (1 << 1) /* Supports PSYNC2 protocol. */

/* Slave requirements */
#define SLAVE_REQ_NONE 0
#define SLAVE_REQ_RDB_EXCLUDE_DATA (1 << 0)      /* Exclude data from RDB */
#define SLAVE_REQ_RDB_EXCLUDE_FUNCTIONS (1 << 1) /* Exclude functions from RDB */
/* Mask of all bits in the slave requirements bitfield that represent non-standard (filtered) RDB requirements */
#define SLAVE_REQ_RDB_MASK (SLAVE_REQ_RDB_EXCLUDE_DATA | SLAVE_REQ_RDB_EXCLUDE_FUNCTIONS)

/* Synchronous read timeout - slave side */
#define CONFIG_REPL_SYNCIO_TIMEOUT 5

/* The default number of replication backlog blocks to trim per call. */
#define REPL_BACKLOG_TRIM_BLOCKS_PER_CALL 64

/* In order to quickly find the requested offset for PSYNC requests,
 * we index some nodes in the replication buffer linked list into a rax. */
#define REPL_BACKLOG_INDEX_PER_BLOCKS 64

/* List related stuff */
#define LIST_HEAD 0
#define LIST_TAIL 1
#define ZSET_MIN 0
#define ZSET_MAX 1

/* Sort operations */
#define SORT_OP_GET 0

// 日志等级
#define LL_DEBUG 0       // （记录大量日志信息,适用于开发、测试阶段）
#define LL_VERBOSE 1     // 较多日志信息）
#define LL_NOTICE 2      // （适量日志信息,使用于生产环境）
#define LL_WARNING 3     // （仅有部分重要、关键信息才会被记录）
#define LL_RAW (1 << 10) // 不带时间戳的日志的修饰符

/* Supervision options */
#define SUPERVISED_NONE 0
#define SUPERVISED_AUTODETECT 1
#define SUPERVISED_SYSTEMD 2
#define SUPERVISED_UPSTART 3

// 用于一些未使用的变量
#define UNUSED(V) ((void)(V))
// https://github.com/redis/redis/pull/6818      64->32
//  The optimal value given 2^64 elements and p=0.25 is:
//  log base[1/p] 2^64 = 32

// Using ZSKIPLIST_MAXLEVEL = 64 instead of 32, we are:
//- allocating 512 bytes per sorted set that are never used. This is on the header node.
//- a similar waste on the stack on every function where we have zskiplistNode *update[ZSKIPLIST_MAXLEVEL]
//- the chance a node would get an unnecessarily high level because we are not capping where we should
#define ZSKIPLIST_MAXLEVEL 32 // 最大层数为32
#define ZSKIPLIST_P 0.25      // 随机数的值为0.25

// aof 落盘策略
#define AOF_FSYNC_NO 0       // 将aof_buf缓冲区中的所有内容写入到AOF文件中,但并不对AOF文件进行同步,合适同步由操作系统决定
#define AOF_FSYNC_ALWAYS 1   // 将aof_buf缓冲区中的所有内容写入并同步到AOF文件,最慢、最安全
#define AOF_FSYNC_EVERYSEC 2 // [默认值]将aof_buf缓冲区中的所有内容写入到AOF文件,如果上次同步AOF文件的时间距离现在超过一秒钟,
// 那么再次对AOF文件进行同步,并且这个同步操作是由一个线程专门负责执行的 ,每隔一秒进行一次同步

/* Replication diskless load defines */
#define REPL_DISKLESS_LOAD_DISABLED 0
#define REPL_DISKLESS_LOAD_WHEN_DB_EMPTY 1
#define REPL_DISKLESS_LOAD_SWAPDB 2

/* TLS Client Authentication */
#define TLS_CLIENT_AUTH_NO 0
#define TLS_CLIENT_AUTH_YES 1
#define TLS_CLIENT_AUTH_OPTIONAL 2

/* Sanitize dump payload */
#define SANITIZE_DUMP_NO 0
#define SANITIZE_DUMP_YES 1
#define SANITIZE_DUMP_CLIENTS 2

/* Enable protected config/command */
#define PROTECTED_ACTION_ALLOWED_NO 0
#define PROTECTED_ACTION_ALLOWED_YES 1
#define PROTECTED_ACTION_ALLOWED_LOCAL 2

/* Sets operations codes */
#define SET_OP_UNION 0
#define SET_OP_DIFF 1
#define SET_OP_INTER 2

/* oom-score-adj defines */
#define OOM_SCORE_ADJ_NO 0
#define OOM_SCORE_RELATIVE 1
#define OOM_SCORE_ADJ_ABSOLUTE 2

// maxmemory策略
#define MAXMEMORY_FLAG_LRU (1 << 0)     // 使用 LRU
#define MAXMEMORY_FLAG_LFU (1 << 1)     // 使用 LRU
#define MAXMEMORY_FLAG_ALLKEYS (1 << 2) // 所有key
#define MAXMEMORY_FLAG_NO_SHARED_INTEGERS (MAXMEMORY_FLAG_LRU | MAXMEMORY_FLAG_LFU)

#define MAXMEMORY_VOLATILE_LRU ((0 << 8) | MAXMEMORY_FLAG_LRU)
#define MAXMEMORY_VOLATILE_LFU ((1 << 8) | MAXMEMORY_FLAG_LFU)
#define MAXMEMORY_VOLATILE_TTL (2 << 8)
#define MAXMEMORY_VOLATILE_RANDOM (3 << 8)
#define MAXMEMORY_ALLKEYS_LRU ((4 << 8) | MAXMEMORY_FLAG_LRU | MAXMEMORY_FLAG_ALLKEYS)
#define MAXMEMORY_ALLKEYS_LFU ((5 << 8) | MAXMEMORY_FLAG_LFU | MAXMEMORY_FLAG_ALLKEYS)
#define MAXMEMORY_ALLKEYS_RANDOM ((6 << 8) | MAXMEMORY_FLAG_ALLKEYS)
#define MAXMEMORY_NO_EVICTION (7 << 8)

/* Units */
#define UNIT_SECONDS 0
#define UNIT_MILLISECONDS 1

/* SHUTDOWN flags */
#define SHUTDOWN_NOFLAGS 0 /* No flags. */
#define SHUTDOWN_SAVE 1    /* Force SAVE on SHUTDOWN even if no save points are configured. */
#define SHUTDOWN_NOSAVE 2  /* Don't SAVE on SHUTDOWN. */
#define SHUTDOWN_NOW 4     /* Don't wait for replicas to catch up. */
#define SHUTDOWN_FORCE 8   /* Don't let errors prevent shutdown. */

// 命令执行 的标志
#define CMD_CALL_NONE 0
#define CMD_CALL_SLOWLOG (1 << 0)                                              // 慢日志
#define CMD_CALL_STATS (1 << 1)                                                // 状态
#define CMD_CALL_PROPAGATE_AOF (1 << 2)                                        // AOF 日志传播
#define CMD_CALL_PROPAGATE_REPL (1 << 3)                                       // 复制 日志 REPLICATION 传播
#define CMD_CALL_PROPAGATE (CMD_CALL_PROPAGATE_AOF | CMD_CALL_PROPAGATE_REPL)  // 如果数据库有被修改,那么启用 REPL 和 AOF 传播
#define CMD_CALL_FULL (CMD_CALL_SLOWLOG | CMD_CALL_STATS | CMD_CALL_PROPAGATE) // 所有类型
#define CMD_CALL_FROM_MODULE (1 << 4)                                          // 模块内调用的 From RM_Call

// 命令同步的标志  propagateNow()
#define PROPAGATE_NONE 0
#define PROPAGATE_AOF 1  // 强制 AOF 传播
#define PROPAGATE_REPL 2 // 强制 REPL 传播

// 客户端暂停类型,较大的暂停类型 比 较小的暂停类型更有限制性.
typedef enum {
    CLIENT_PAUSE_OFF = 0, // 没有暂停 命令
    CLIENT_PAUSE_WRITE,   // 暂停所有写命令
    CLIENT_PAUSE_ALL      // 暂停所有命令
} pause_type;

/* Client pause purposes. Each purpose has its own end time and pause type. */
typedef enum {
    PAUSE_BY_CLIENT_COMMAND = 0,
    PAUSE_DURING_SHUTDOWN,
    PAUSE_DURING_FAILOVER,
    NUM_PAUSE_PURPOSES /* This value is the number of purposes above. */
} pause_purpose;

typedef struct {
    pause_type type;
    mstime_t end;
} pause_event;

/* Ways that a clusters endpoint can be described */
typedef enum {
    CLUSTER_ENDPOINT_TYPE_IP = 0,          /* Show IP address */
    CLUSTER_ENDPOINT_TYPE_HOSTNAME,        /* Show hostname */
    CLUSTER_ENDPOINT_TYPE_UNKNOWN_ENDPOINT /* Show NULL or empty */
} cluster_endpoint_type;

/* RDB active child save type. */
#define RDB_CHILD_TYPE_NONE 0
#define RDB_CHILD_TYPE_DISK 1   /* RDB is written to disk. */
#define RDB_CHILD_TYPE_SOCKET 2 /* RDB is written to slave socket. */

// keyspace 变更通知的 类型
#define NOTIFY_KEYSPACE (1 << 0)                                                                                                                                             /* K */
#define NOTIFY_KEYEVENT (1 << 1)                                                                                                                                             /* E */
#define NOTIFY_GENERIC (1 << 2)                                                                                                                                              /* g */
#define NOTIFY_STRING (1 << 3)                                                                                                                                               /* $ */
#define NOTIFY_LIST (1 << 4)                                                                                                                                                 /* l */
#define NOTIFY_SET (1 << 5)                                                                                                                                                  /* s */
#define NOTIFY_HASH (1 << 6)                                                                                                                                                 /* h */
#define NOTIFY_ZSET (1 << 7)                                                                                                                                                 /* z */
#define NOTIFY_EXPIRED (1 << 8)                                                                                                                                              /* x */
#define NOTIFY_EVICTED (1 << 9)                                                                                                                                              /* e */
#define NOTIFY_STREAM (1 << 10)                                                                                                                                              /* t */
#define NOTIFY_KEY_MISS (1 << 11)                                                                                                                                            /* m (Note: This one is excluded from NOTIFY_ALL on purpose) */
#define NOTIFY_LOADED (1 << 12)                                                                                                                                              /* module only key space notification, indicate a key loaded from rdb */
#define NOTIFY_MODULE (1 << 13)                                                                                                                                              /* d, module key space notification */
#define NOTIFY_NEW (1 << 14)                                                                                                                                                 /* n, new key notification */
#define NOTIFY_ALL (NOTIFY_GENERIC | NOTIFY_STRING | NOTIFY_LIST | NOTIFY_SET | NOTIFY_HASH | NOTIFY_ZSET | NOTIFY_EXPIRED | NOTIFY_EVICTED | NOTIFY_STREAM | NOTIFY_MODULE) /* A flag */

// 因为 serverCron 函数中的所有代码都会每秒调用 server.hz 次,
// 为了对部分代码的调用次数进行限制,
// 使用了一个宏 run_with_period(milliseconds) { ... } ,
// 这个宏可以将被包含代码的执行次数降低为每 milliseconds 执行一次.
#define run_with_period(_ms_) if (((_ms_) <= 1000 / server.hz) || !(server.cronloops % ((_ms_) / (1000 / server.hz))))

// 我们可以打印堆栈跟踪,所以我们的断言是这样定义的.
#define serverAssertWithInfo(_c, _o, _e) ((_e) ? (void)0 : (_serverAssertWithInfo(_c, _o, #_e, __FILE__, __LINE__), redis_unreachable()))
// 断言的_e需要为true
#define serverAssert(_e) ((_e) ? (void)0 : (_serverAssert(#_e, __FILE__, __LINE__), redis_unreachable()))
#define serverPanic(...) _serverPanic(__FILE__, __LINE__, __VA_ARGS__), redis_unreachable()

// 每个命令初始化的延迟直方图
#define LATENCY_HISTOGRAM_MIN_VALUE 1L          // >= 1纳秒
#define LATENCY_HISTOGRAM_MAX_VALUE 1000000000L // <= 1秒
#define LATENCY_HISTOGRAM_PRECISION 2           //
// 在LATENCY_HISTOGRAM_MIN_VALUE和LATENCY_HISTOGRAM_MAX_VALUE范围内保持2位有效数字的值精度.
//  因此,该范围内的数值量化将不大于任何值的1/100(或1%).每个直方图的总大小应该在40 KiB字节左右.

// 模块繁忙的标志,busy_module_yield_flags
#define BUSY_MODULE_YIELD_NONE (0)         // 0
#define BUSY_MODULE_YIELD_EVENTS (1 << 0)  // 01
#define BUSY_MODULE_YIELD_CLIENTS (1 << 1) // 10

/*-----------------------------------------------------------------------------
 * Data types
 *----------------------------------------------------------------------------*/

/* A redis object, that is a type able to hold a string / list / set */

// 一个Redis对象,它是一个能够保存string/list/set的类型
#define OBJ_STRING 0 /* String object. */
#define OBJ_LIST 1   /* List object. */
#define OBJ_SET 2    /* Set object. */
#define OBJ_ZSET 3   /* Sorted set object. */
#define OBJ_HASH 4   /* Hash object. */

/* The "module" object type is a special one that signals that the object
 * is one directly managed by a Redis module. In this case the value points
 * to a moduleValue struct, which contains the object value (which is only
 * handled by the module itself) and the RedisModuleType struct which lists
 * function pointers in order to serialize, deserialize, AOF-rewrite and
 * free the object.
 *
 * Inside the RDB file, module types are encoded as OBJ_MODULE followed
 * by a 64 bit module type ID, which has 54 bits module-specific signature
 * in order to dispatch the loading to the right module, plus a 10 bits
 * encoding version. */
#define OBJ_MODULE 5 /* Module object. */
#define OBJ_STREAM 6 /* Stream object. */

/* Extract encver / signature from a module type ID. */
#define REDISMODULE_TYPE_ENCVER_BITS 10
#define REDISMODULE_TYPE_ENCVER_MASK ((1 << REDISMODULE_TYPE_ENCVER_BITS) - 1)
#define REDISMODULE_TYPE_ENCVER(id) ((id)&REDISMODULE_TYPE_ENCVER_MASK)
#define REDISMODULE_TYPE_SIGN(id) (((id) & ~((uint64_t)REDISMODULE_TYPE_ENCVER_MASK)) >> REDISMODULE_TYPE_ENCVER_BITS)

/* Bit flags for moduleTypeAuxSaveFunc */
#define REDISMODULE_AUX_BEFORE_RDB (1 << 0)
#define REDISMODULE_AUX_AFTER_RDB (1 << 1)

struct RedisModule;
struct RedisModuleIO;
struct RedisModuleDigest;
struct RedisModuleCtx;
struct moduleLoadQueueEntry;
struct redisObject;
struct RedisModuleDefragCtx;
struct RedisModuleInfoCtx;
struct RedisModuleKeyOptCtx;

/* Each module type implementation should export a set of methods in order
 * to serialize and deserialize the value in the RDB file, rewrite the AOF
 * log, create the digest for "DEBUG DIGEST", and free the value when a key
 * is deleted. */
typedef void *(*moduleTypeLoadFunc)(struct RedisModuleIO *io, int encver);

typedef void (*moduleTypeSaveFunc)(struct RedisModuleIO *io, void *value);

typedef int (*moduleTypeAuxLoadFunc)(struct RedisModuleIO *rdb, int encver, int when);

typedef void (*moduleTypeAuxSaveFunc)(struct RedisModuleIO *rdb, int when);

typedef void (*moduleTypeRewriteFunc)(struct RedisModuleIO *io, struct redisObject *key, void *value);

typedef void (*moduleTypeDigestFunc)(struct RedisModuleDigest *digest, void *value);

typedef size_t (*moduleTypeMemUsageFunc)(const void *value);

typedef void (*moduleTypeFreeFunc)(void *value);

typedef size_t (*moduleTypeFreeEffortFunc)(struct redisObject *key, const void *value);

typedef void (*moduleTypeUnlinkFunc)(struct redisObject *key, void *value);

typedef void *(*moduleTypeCopyFunc)(struct redisObject *fromkey, struct redisObject *tokey, const void *value);

typedef int (*moduleTypeDefragFunc)(struct RedisModuleDefragCtx *ctx, struct redisObject *key, void **value);

typedef void (*RedisModuleInfoFunc)(struct RedisModuleInfoCtx *ctx, int for_crash_report);

typedef void (*RedisModuleDefragFunc)(struct RedisModuleDefragCtx *ctx);

typedef size_t (*moduleTypeMemUsageFunc2)(struct RedisModuleKeyOptCtx *ctx, const void *value, size_t sample_size);

typedef void (*moduleTypeFreeFunc2)(struct RedisModuleKeyOptCtx *ctx, void *value);

typedef size_t (*moduleTypeFreeEffortFunc2)(struct RedisModuleKeyOptCtx *ctx, const void *value);

typedef void (*moduleTypeUnlinkFunc2)(struct RedisModuleKeyOptCtx *ctx, void *value);

typedef void *(*moduleTypeCopyFunc2)(struct RedisModuleKeyOptCtx *ctx, const void *value);

/* This callback type is called by moduleNotifyUserChanged() every time
 * a user authenticated via the module API is associated with a different
 * user or gets disconnected. This needs to be exposed since you can't cast
 * a function pointer to (void *). */
typedef void (*RedisModuleUserChangedFunc)(uint64_t client_id, void *privdata);

/* The module type, which is referenced in each value of a given type, defines
 * the methods and links to the module exporting the type. */
typedef struct RedisModuleType {
    uint64_t id; /* Higher 54 bits of type ID + 10 lower bits of encoding ver. */
    struct RedisModule *module;
    moduleTypeLoadFunc rdb_load;
    moduleTypeSaveFunc rdb_save;
    moduleTypeRewriteFunc aof_rewrite;
    moduleTypeMemUsageFunc mem_usage;
    moduleTypeDigestFunc digest;
    moduleTypeFreeFunc free;
    moduleTypeFreeEffortFunc free_effort;
    moduleTypeUnlinkFunc unlink;
    moduleTypeCopyFunc copy;
    moduleTypeDefragFunc defrag;
    moduleTypeAuxLoadFunc aux_load;
    moduleTypeAuxSaveFunc aux_save;
    moduleTypeMemUsageFunc2 mem_usage2;
    moduleTypeFreeEffortFunc2 free_effort2;
    moduleTypeUnlinkFunc2 unlink2;
    moduleTypeCopyFunc2 copy2;
    int aux_save_triggers;
    char name[10]; /* 9 bytes name + null term. Charset: A-Z a-z 0-9 _- */
} moduleType;

/* In Redis objects 'robj' structures of type OBJ_MODULE, the value pointer
 * is set to the following structure, referencing the moduleType structure
 * in order to work with the value, and at the same time providing a raw
 * pointer to the value, as created by the module commands operating with
 * the module type.
 *
 * So for example in order to free such a value, it is possible to use
 * the following code:
 *
 *  if (robj->type == OBJ_MODULE) {
 *      moduleValue *mt = robj->ptr;
 *      mt->type->free(mt->value);
 *      zfree(mt); // We need to release this in-the-middle struct as well.
 *  }
 */
typedef struct moduleValue {
    moduleType *type;
    void *value;
} moduleValue;

/* This structure represents a module inside the system. */
struct RedisModule {
    void *handle;                         /* Module dlopen() handle. */
    char *name;                           /* Module name. */
    int ver;                              /* Module version. We use just progressive integers. */
    int apiver;                           /* Module API version as requested during initialization.*/
    list *types;                          /* Module data types. */
    list *usedby;                         /* List of modules using APIs from this one. */
    list *using;                          /* List of modules we use some APIs of. */
    list *filters;                        /* List of filters the module has registered. */
    list *module_configs;                 /* List of configurations the module has registered */
    int configs_initialized;              /* Have the module configurations been initialized? */
    int in_call;                          /* RM_Call() nesting level */
    int in_hook;                          /* Hooks callback nesting level for this module (0 or 1). */
    int options;                          /* Module options and capabilities. */
    int blocked_clients;                  /* Count of RedisModuleBlockedClient in this module. */
    RedisModuleInfoFunc info_cb;          /* Callback for module to add INFO fields. */
    RedisModuleDefragFunc defrag_cb;      /* Callback for global data defrag. */
    struct moduleLoadQueueEntry *loadmod; /* Module load arguments for config rewrite. */
};
typedef struct RedisModule RedisModule;

/* This is a wrapper for the 'rio' streams used inside rdb.c in Redis, so that
 * the user does not have to take the total count of the written bytes nor
 * to care about error conditions. */
typedef struct RedisModuleIO {
    size_t bytes;               /* Bytes read / written so far. */
    rio *rio;                   /* Rio stream. */
    moduleType *type;           /* Module type doing the operation. */
    int error;                  /* True if error condition happened. */
    int ver;                    /* Module serialization version: 1 (old),
                                 * 2 (current version with opcodes annotation). */
    struct RedisModuleCtx *ctx; /* Optional context, see RM_GetContextFromIO()*/
    struct redisObject *key;    /* Optional name of key processed */
    int dbid;                   /* The dbid of the key being processed, -1 when unknown. */
} RedisModuleIO;

/* Macro to initialize an IO context. Note that the 'ver' field is populated
 * inside rdb.c according to the version of the value to load. */
#define moduleInitIOContext(iovar, mtype, rioptr, keyptr, db) \
    do {                                                      \
        (iovar).rio = rioptr;                                 \
        (iovar).type = mtype;                                 \
        (iovar).bytes = 0;                                    \
        (iovar).error = 0;                                    \
        (iovar).ver = 0;                                      \
        (iovar).key = keyptr;                                 \
        (iovar).dbid = db;                                    \
        (iovar).ctx = NULL;                                   \
    } while (0)

/* This is a structure used to export DEBUG DIGEST capabilities to Redis
 * modules. We want to capture both the ordered and unordered elements of
 * a data structure, so that a digest can be created in a way that correctly
 * reflects the values. See the DEBUG DIGEST command implementation for more
 * background. */
typedef struct RedisModuleDigest {
    unsigned char o[20];     /* Ordered elements. */
    unsigned char x[20];     /* Xored elements. */
    struct redisObject *key; /* Optional name of key processed */
    int dbid;                /* The dbid of the key being processed */
} RedisModuleDigest;

// 只需从一个由0 字节组成的摘要开始.
#define moduleInitDigestContext(mdvar)           \
    do {                                         \
        memset((mdvar).o, 0, sizeof((mdvar).o)); \
        memset((mdvar).x, 0, sizeof((mdvar).x)); \
    } while (0)

// 对象编码
#define OBJ_ENCODING_RAW 0        // 大于 44 字节,redisObject 和 SDS 分开存储,需分配 2 次内存
#define OBJ_ENCODING_INT 1        // 整数存储（小于 10000,使用共享对象池存储,但有个前提：Redis 没有设置淘汰策略,详见 object.c 的 tryObjectEncoding 函数）
#define OBJ_ENCODING_HT 2         // 字典
#define OBJ_ENCODING_ZIPMAP 3     /* Encoded as zipmap */
#define OBJ_ENCODING_LINKEDLIST 4 // 双端链表  弃用
#define OBJ_ENCODING_ZIPLIST 5    // 压缩列表
#define OBJ_ENCODING_INTSET 6     // 整数集合
#define OBJ_ENCODING_SKIPLIST 7   // 跳跃表和字典
#define OBJ_ENCODING_EMBSTR 8     // 小于 44 字节,嵌入式存储,redisObject 和 SDS 一起分配内存,只分配 1 次内存
#define OBJ_ENCODING_QUICKLIST 9  // listpacks的
#define OBJ_ENCODING_STREAM 10    // listpacks的前缀树
#define OBJ_ENCODING_LISTPACK 11  // hash底层实现之一

//  Redis 对象
#define LRU_BITS 24
#define LRU_CLOCK_MAX ((1 << LRU_BITS) - 1) // obj->lru 的最大值, 4字节的最大值
#define LRU_CLOCK_RESOLUTION 1000           // LRU检查最大时间  以毫秒为单位的 LRU 时钟精度

#define OBJ_SHARED_REFCOUNT INT_MAX       /* Global object never destroyed. */
#define OBJ_STATIC_REFCOUNT (INT_MAX - 1) /* Object allocated in the stack. */
#define OBJ_FIRST_SPECIAL_REFCOUNT OBJ_STATIC_REFCOUNT
// unsigned  4字节
typedef struct redisObject { // 16个字节
    unsigned type : 4;       // 类型 4bits  表示值的类型,涵盖了我们前面学习的五大基本类型;
    unsigned encoding : 4;   // 编码 4bits  基本类型的底层数据结构,例如 SDS、压缩列表、哈希表、跳表等;
    unsigned lru : LRU_BITS; // 24bits      对象最后一次被访问的时间,在服务器启用了maxmemory时 空转时间较大的哪些键可能会优先被服务器删除
    // 前16位 存储数据的访问时间戳、后8位 表示数据的访问次数

    int refcount; // 引用计数 4bytes
    void *ptr;    // 指向对象的底层实现数据结构,而这些结构由encoding属性决定 8bytes
} robj;

/* The a string name for an object's type as listed above
 * Native types are checked against the OBJ_STRING, OBJ_LIST, OBJ_* defines,
 * and Module types have their registered name returned. */
char *getObjectTypeName(robj *);

/* Macro used to initialize a Redis object allocated on the stack.
 * Note that this macro is taken near the structure definition to make sure
 * we'll update it when the structure is changed, to avoid bugs like
 * bug #85 introduced exactly in this way. */
#define initStaticStringObject(_var, _ptr)     \
    do {                                       \
        (_var).refcount = OBJ_STATIC_REFCOUNT; \
        (_var).type = OBJ_STRING;              \
        (_var).encoding = OBJ_ENCODING_RAW;    \
        (_var).ptr = _ptr;                     \
    } while (0)

struct evictionPoolEntry; /* Defined in evict.c */

/* This structure is used in order to represent the output buffer of a client,
 * which is actually a linked list of blocks like that, that is: client->reply. */
typedef struct clientReplyBlock {
    size_t size, used;
    char buf[];
} clientReplyBlock;

/* Replication buffer blocks is the list of replBufBlock.
 *
 * +--------------+       +--------------+       +--------------+
 * | refcount = 1 |  ...  | refcount = 0 |  ...  | refcount = 2 |
 * +--------------+       +--------------+       +--------------+
 *      |                                            /       \
 *      |                                           /         \
 *      |                                          /           \
 *  Repl Backlog                               Replia_A      Replia_B
 *
 * Each replica or replication backlog increments only the refcount of the
 * 'ref_repl_buf_node' which it points to. So when replica walks to the next
 * node, it should first increase the next node's refcount, and when we trim
 * the replication buffer nodes, we remove node always from the head node which
 * refcount is 0. If the refcount of the head node is not 0, we must stop
 * trimming and never iterate the next node. */

/* Similar with 'clientReplyBlock', it is used for shared buffers between
 * all replica clients and replication backlog. */
typedef struct replBufBlock {
    int refcount;          /* Number of replicas or repl backlog using. */
    long long id;          /* The unique incremental number. */
    long long repl_offset; /* Start replication offset of the block. */
    size_t size, used;
    char buf[];
} replBufBlock;

/* Opaque type for the Slot to Key API. */
typedef struct clusterSlotToKeyMapping clusterSlotToKeyMapping;

/* Redis数据库.有多个数据库,用从0(默认数据库)到最大配置数据库的整数标识.数据库号是结构中的'id'字段. */
typedef struct redisDb {
    dict *dict;                             // 全局哈希表,数据键值对存在这
    dict *expires;                          // 过期 key + 过期时间 存在这
    dict *blocking_keys;                    // 正处于阻塞状态的键 例如BLPOP
    dict *ready_keys;                       // 可以解除阻塞的键
    dict *watched_keys;                     // 正在被 WATCH 命令监视的键
    int id;                                 // 数据库号码
    long long avg_ttl;                      // 数据库的键的平均 TTL ,统计信息
    unsigned long expires_cursor;           /* Cursor of the active expire cycle. */
    list *defrag_later;                     /* List of key names to attempt to defrag one by one, gradually. */
    clusterSlotToKeyMapping *slots_to_keys; /* Array of slots to keys. Only used in cluster mode (db 0). */
} redisDb;

/* forward declaration for functions ctx */
typedef struct functionsLibCtx functionsLibCtx;

/* Holding object that need to be populated during
 * rdb loading. On loading end it is possible to decide
 * whether not to set those objects on their rightful place.
 * For example: dbarray need to be set as main database on
 *              successful loading and dropped on failure. */
typedef struct rdbLoadingCtx {
    redisDb *dbarray;
    functionsLibCtx *functions_lib_ctx;
} rdbLoadingCtx;

// 事务命令
typedef struct multiCmd {
    robj **argv;              // 参数
    int argv_len;             // 参数数量
    int argc;                 //
    struct redisCommand *cmd; // 命令指针
} multiCmd;

// 事务状态
typedef struct multiState {
    multiCmd *commands;   // 事务中的命令队列,FIFO 顺序
    int count;            // 已入队命令计数
    int cmd_flags;        // 所有命令中是否有被设置  CMD_READONLY|CMD_WRITE|CMD_MAY_REPLICATE|CMD_NO_ASYNC_LOADING
    int cmd_inv_flags;    // 与cmd_flags相同,    CMD_STALE|CMD_LOADING */
    size_t argv_len_sums; // 所有命令参数使用的内存*/
} multiState;

// 阻塞状态
typedef struct blockingState {
    //    一般字段
    long count;       /* Elements to pop if count was specified (BLMPOP/BZMPOP), -1 otherwise. */
    mstime_t timeout; // 阻塞时限[时间点]

    dict *keys;   // 造成阻塞的键   、可以是list、zset、stream
    robj *target; // 在被阻塞的键有新元素进入时,需要将这些新元素添加到哪里的目标键  用于 BLMOVE 命令
    struct blockPos {
        int wherefrom; /* Where to pop from */
        int whereto;   /* Where to push to */
    } blockpos;        /* The positions in the src/dst lists/zsets where we want to pop/push an element for BLPOP, BRPOP, BLMOVE and BZMPOP. */

    /* BLOCK_STREAM */
    size_t xread_count;   /* XREAD COUNT option. */
    robj *xread_group;    /* XREADGROUP group name. */
    robj *xread_consumer; /* XREADGROUP consumer name. */
    int xread_group_noack;

    /* BLOCKED_WAIT */
    int numreplicas;      // 等待 ACK 的复制节点数量
    long long reploffset; // 复制偏移量

    /* BLOCKED_MODULE */
    void *module_blocked_handle; /* RedisModuleBlockedClient structure.which is opaque for the Redis core, only handled in module.c. */
} blockingState;

/* The following structure represents a node in the server.ready_keys list,
 * where we accumulate all the keys that had clients blocked with a blocking
 * operation such as B[LR]POP, but received new data in the context of the
 * last executed command.
 *
 * After the execution of every command or script, we run this list to check
 * if as a result we should serve data to clients blocked, unblocking them.
 * Note that server.ready_keys will not have duplicates as there dictionary
 * also called ready_keys in every structure representing a Redis database,
 * where we make sure to remember if a given key was already added in the
 * server.ready_keys list. */
// 记录解除了客户端的阻塞状态的键,以及键所在的数据库.
typedef struct readyList {
    redisDb *db;
    robj *key;
} readyList;

/* This structure represents a Redis user. This is useful for ACLs, the
 * user is associated to the connection after the connection is authenticated.
 * If there is no associated user, the connection uses the default user. */
#define USER_COMMAND_BITS_COUNT 1024             /* The total number of command bits in the user structure. The last valid command ID we can set in the user is USER_COMMAND_BITS_COUNT-1. */
#define USER_FLAG_ENABLED (1 << 0)               /* 用户是活跃的*/
#define USER_FLAG_DISABLED (1 << 1)              /* 用户已禁用*/
#define USER_FLAG_NOPASS (1 << 2)                /* 用户不需要密码,任何提供的密码都可以工作.对于默认用户,这也意味着不需要AUTH,每个连接都立即进行身份验证*/
#define USER_FLAG_SANITIZE_PAYLOAD (1 << 3)      /* 用户需要深度的RESTORE有效负载清理.*/
#define USER_FLAG_SANITIZE_PAYLOAD_SKIP (1 << 4) /* The user should skip the deep sanitization of RESTORE  payload. */

#define SELECTOR_FLAG_ROOT (1 << 0)        /* This is the root user permission selector. */
#define SELECTOR_FLAG_ALLKEYS (1 << 1)     /* The user can mention any key. */
#define SELECTOR_FLAG_ALLCOMMANDS (1 << 2) /* The user can run all commands. */
#define SELECTOR_FLAG_ALLCHANNELS (1 << 3) /* The user can mention any Pub/Sub channel. */

typedef struct {
    sds name;        /* The username as an SDS string. */
    uint32_t flags;  /* See USER_FLAG_* */
    list *passwords; /* A list of SDS valid passwords for this user. */
    list *selectors; /* A list of selectors this user validates commands against. This list will always contain at least one selector for backwards compatibility. */
} user;

/* With multiplexing we need to take per-client state.
 * Clients are taken in a linked list. */

#define CLIENT_ID_AOF (UINT64_MAX) /* Reserved ID for the AOF client. If you need more reserved IDs use UINT64_MAX-1, -2, ... and so forth. */

/* Replication backlog is not separate memory, it just is one consumer of
 * the global replication buffer. This structure records the reference of
 * replication buffers. Since the replication buffer block list may be very long,
 * it would cost much time to search replication offset on partial resync, so
 * we use one rax tree to index some blocks every REPL_BACKLOG_INDEX_PER_BLOCKS
 * to make searching offset from replication buffer blocks list faster. */
typedef struct replBacklog {
    listNode *ref_repl_buf_node; /* Referenced node of replication buffer blocks, see the definition of replBufBlock. */
    size_t unindexed_count;      /* The count from last creating index block. */
    rax *blocks_index;           /* The index of recorded blocks of replication buffer for quickly searching replication offset on partial resynchronization. */
    long long histlen;           // 表示循环缓冲区中当前累积的数据的长度
    long long offset;            // 表示循环缓冲区最早保存的数据的首字节在全局范围内的偏移
} replBacklog;

typedef struct {
    list *clients;        // 客户端列表
    size_t mem_usage_sum; // 内存使用量
} clientMemUsageBucket;

// 因为 I/O 复用的缘故,需要为每个客户端维持一个状态. 多个客户端状态被服务器用链表连接起来.
typedef struct client {
    uint64_t id;                              // 客户端ID,自增ID
    connection *conn;                         //
    int resp;                                 // 客户端 支持的RESP 协议版本,2或者3
    redisDb *db;                              // 记录客户端当前正在使用的数据库
    robj *name;                               // 客户端的名字
    sds querybuf;                             // 输入缓冲区,保存客户端发送的命令请求;会根据输入内容动态地缩小或者扩大,但它的最 大大小不能超过1GB,否则服务器将关闭这个客户端.
    size_t qb_pos;                            // 从输入缓冲区的哪个位置开始读
    size_t querybuf_peak;                     // 最近（100ms或更多）的querybuf大小的峰值.
    int argc;                                 // 命令行参数的个数
    robj **argv;                              // 命令行参数;argv[0] 是要执行的命令
    int argv_len;                             // 一次client操作的参数 argv数组的大小（可能多于argc）.
    int original_argc;                        //
    robj **original_argv;                     // 如果这里存储了参数s, 就说明参数 被重写了
    size_t argv_len_sum;                      // argv列表中对象长度的和
    struct redisCommand *cmd;                 //
    struct redisCommand *lastcmd;             // 最新已经执行的执行命令
    struct redisCommand *realcmd;             // 客户端执行的原始命令,用于更新错误统计,以防c->cmd在命令调用过程中被修改（例如在GEOADD上）.
    user *user;                               // 与此连接相关的用户.如果该用户被设置为NULL,该连接可以做任何事情（管理）.
    int reqtype;                              // 请求的类型：内联命令还是多条命令
    int multibulklen;                         // 剩余未读取的命令内容数量
    long bulklen;                             // 命令内容的长度 在一个批量请求中
    list *reply;                              // 回复链表  可变大小缓冲区由reply链表和一个或多个字符串对象组成
    unsigned long long reply_bytes;           // 回复链表中对象的总大小
    list *deferred_reply_errors;              /* Used for module thread safe contexts. */
    size_t sentlen;                           // 已发送字节,处理 short write 用
    time_t ctime;                             // 记录了创建客户端的时间
    time_t lastinteraction;                   // 记录了客户端与服务器最后一次进行互动的时间
    time_t obuf_soft_limit_reached_time;      // 客户端的输出缓冲区超过软性限制的时间
    long duration;                            // 当前命令的执行时间
    int slot;                                 /* The slot the client is executing against. Set to -1 if no slot is being used */
    uint64_t flags;                           // 客户端的标志值
    int authenticated;                        // 当 server.requirepass 不为 NULL 时 代表认证的状态  0 代表未认证,1 代表已认证
    int replstate;                            // 复制状态,如果是slave
    int repl_start_cmd_stream_on_ack;         /* Install slave write handler on first ACK. */
    int repldbfd;                             // 用于保存主服务器传来的 RDB 文件的文件描述符
    off_t repldboff;                          // 读取主服务器传来的 RDB 文件的偏移量
    off_t repldbsize;                         // 主服务器传来的 RDB 文件的大小
    sds replpreamble;                         /* Replication DB preamble. */
    long long read_reploff;                   /* 如果这是master,读取复制偏移量.*/
    long long reploff;                        /* Applied replication offset if this is a master. */
    long long repl_applied;                   /* Applied replication data count in querybuf, if this is a replica. */
    long long repl_ack_off;                   // 从服务器最后一次发送 REPLCONF ACK 时的偏移量
    long long repl_ack_time;                  // 从服务器最后一次发送 REPLCONF ACK 的时间
    long long repl_last_partial_write;        /* The last time the server did a partial write from the RDB child pipe to this replica  */
    long long psync_initial_offset;           /* FULLRESYNC reply offset other slaves copying this slave output buffer should use. */
    char replid[CONFIG_RUN_ID_SIZE + 1];      // 主服务器的 master run ID,保存在客户端,用于执行部分重同步
    int slave_listening_port;                 // 从服务器的监听端口号
    char *slave_addr;                         /* 可以由REPLCONF ip-address指定 */
    int slave_capa;                           /* Slave capabilities: SLAVE_CAPA_* bitwise OR. */
    int slave_req;                            /* Slave requirements: SLAVE_REQ_* */
    multiState mstate;                        // 事务状态
    int btype;                                // 阻塞类型 如果客户端阻塞了
    blockingState bpop;                       // 阻塞状态
    long long woff;                           // 最后被写入的全局复制偏移量
    list *watched_keys;                       // 被监视的键s
    dict *pubsub_channels;                    // 记录了客户端所有订阅的频道 键为频道名字,值为 NULL 也即是,一个频道的集合
    list *pubsub_patterns;                    // 记录了客户端所有订阅频道的信息 新结构总是被添加到表尾
    dict *pubsubshard_channels;               /* shard level channels a client is interested in (SSUBSCRIBE) */
    sds peerid;                               /* Cached peer ID. */
    sds sockname;                             /* Cached connection target address. */
    listNode *client_list_node;               /* list node in client list */
    listNode *postponed_list_node;            /* list node within the postponed list */
    listNode *pending_read_list_node;         /* 使用多线程时 等待读的客户端*/
    RedisModuleUserChangedFunc auth_callback; /* Module callback to execute
                                               * when the authenticated user
                                               * changes. */
    void *auth_callback_privdata;             /* Private data that is passed when the auth
                                               * changed callback is executed. Opaque for
                                               * Redis Core. */
    void *auth_module;                        /* The module that owns the callback, which is used
                                               * to disconnect the client if the module is
                                               * unloaded for cleanup. Opaque for Redis Core.*/

    /* If this client is in tracking mode and this field is non zero, invalidation messages for keys fetched by this client will be send to the specified client ID. */
    uint64_t client_tracking_redirection;
    rax *client_tracking_prefixes; /* A dictionary of prefixes we are already subscribed to in BCAST mode, in the context of client side caching. */
    /* In updateClientMemUsage() we track the memory usage of
     * each client and add it to the sum of all the clients of a given type,
     * however we need to remember what was the old contribution of each
     * client, and in which category the client was, in order to remove it
     * before adding it the new value. */
    size_t last_memory_usage;
    int last_memory_type;

    listNode *mem_usage_bucket_node;
    clientMemUsageBucket *mem_usage_bucket;

    listNode *ref_repl_buf_node; // 复制缓冲区块的引用节点 值是 replBufBlock*
    size_t ref_block_pos;        /* Access position of referenced buffer block, i.e. the next offset to send. */

    // 响应堆栈
    size_t buf_peak;                   /* 在最近5秒间隔内使用的缓冲器的峰值大小.*/
    mstime_t buf_peak_last_reset_time; /* 保留上次重置缓冲区峰值的时间 */
    int bufpos;                        // 记录了buf数组目前已使用的字节数量
    size_t buf_usable_size;            // 可使用的内存大小
    char *buf;                         // 回复缓冲区  默认16K 用于保存那些长度比较小的回复,比如OK、简 短的字符串值、整数值、错误回复等等.
} client;

// 服务器的保存条件（BGSAVE 自动执行的条件）
struct saveparam {
    time_t seconds; // 秒数
    int changes;    // 修改数
};

struct moduleLoadQueueEntry {
    sds path;
    int argc;
    robj **argv;
};

struct sentinelLoadQueueEntry {
    int argc;
    sds *argv;
    int linenum;
    sds line;
};

struct sentinelConfig {
    list *pre_monitor_cfg;
    list *monitor_cfg;
    list *post_monitor_cfg;
};

// 通过复用来减少内存碎片,以及减少操作耗时的共享对象
struct sharedObjectsStruct {
    robj *crlf;                             // \r\n
    robj *ok;                               // +OK\r\n
    robj *err;                              // -ERR\r\n
    robj *emptybulk;                        // $0\r\n\r\n
    robj *czero;                            // :0\r\n
    robj *cone;                             // :1\r\n
    robj *pong;                             // +PONG\r\n
    robj *space;                            // " "
    robj *queued;                           // +QUEUED\r\n
    robj *null[4];                          // [null,null,    $-1\r\n ,    _\r\n ]
    robj *nullarray[4];                     // [null,null,    *-1\r\n ,    _\r\n ]
    robj *emptymap[4];                      // [null,null,    *0\r\n ,    %0\r\n ]
    robj *emptyset[4];                      // [null,null,    *0\r\n ,    ~0\r\n ]
    robj *emptyarray;                       // *0\r\n
    robj *wrongtypeerr;                     //
    robj *nokeyerr;                         //
    robj *syntaxerr;                        //
    robj *sameobjecterr;                    //
    robj *outofrangeerr;                    //
    robj *noscripterr;                      //
    robj *loadingerr;                       //
    robj *slowevalerr;                      //
    robj *slowscripterr;                    //
    robj *slowmoduleerr;                    //
    robj *bgsaveerr;                        //
    robj *masterdownerr;                    //
    robj *roslaveerr;                       //
    robj *execaborterr;                     //
    robj *noautherr;                        //
    robj *noreplicaserr;                    //
    robj *busykeyerr;                       //
    robj *oomerr;                           //
    robj *plus;                             //
    robj *messagebulk;                      //
    robj *pmessagebulk;                     //
    robj *subscribebulk;                    //
    robj *unsubscribebulk;                  //
    robj *psubscribebulk;                   //
    robj *punsubscribebulk;                 //
    robj *del;                              //
    robj *unlink;                           //
    robj *rpop;                             //
    robj *lpop;                             //
    robj *lpush;                            //
    robj *rpoplpush;                        //
    robj *lmove;                            //
    robj *blmove;                           //
    robj *zpopmin;                          //
    robj *zpopmax;                          //
    robj *emptyscan;                        //
    robj *multi;                            //
    robj *exec;                             //
    robj *left;                             //
    robj *right;                            //
    robj *hset;                             //
    robj *srem;                             //
    robj *xgroup;                           //
    robj *xclaim;                           //
    robj *script;                           //
    robj *replconf;                         //
    robj *eval;                             //
    robj *persist;                          //
    robj *set;                              //
    robj *pexpireat;                        //
    robj *pexpire;                          //
    robj *time;                             //
    robj *pxat;                             //
    robj *absttl;                           //
    robj *retrycount;                       //
    robj *force;                            //
    robj *justid;                           //
    robj *entriesread;                      //
    robj *lastid;                           //
    robj *ping;                             //
    robj *setid;                            //
    robj *keepttl;                          //
    robj *load;                             //
    robj *createconsumer;                   //
    robj *getack;                           //
    robj *special_asterick;                 //
    robj *special_equals;                   //
    robj *default_username;                 //
    robj *redacted;                         //
    robj *ssubscribebulk;                   //
    robj *sunsubscribebulk;                 //
    robj *select[PROTO_SHARED_SELECT_CMDS]; //
    robj *integers[OBJ_SHARED_INTEGERS];    // 10000
    robj *mbulkhdr[OBJ_SHARED_BULKHDR_LEN]; // 数组类型   *<value>\r\n"
    robj *bulkhdr[OBJ_SHARED_BULKHDR_LEN];  // 字符串类型  $<value>\r\n
    robj *maphdr[OBJ_SHARED_BULKHDR_LEN];   /* "%<value>\r\n" */
    robj *sethdr[OBJ_SHARED_BULKHDR_LEN];   /* "~<value>\r\n" */
    sds minstring, maxstring;               //
};

// 跳跃表节点
typedef struct zskiplistNode {
    sds ele;                           // 对象的元素值
    double score;                      // 元素权重值
    struct zskiplistNode *backward;    // 后退指针
    struct zskiplistLevel {            // 层
        struct zskiplistNode *forward; // 前进指针
        unsigned long span;            // 跨度,记录了两个节点之间的距离  {实际上是用来计算排位的:查找到目标节点的路径上所有span之和最短路径}
    } level[];                         // 理论上来说层数越多,访问其他节点的速度就越快
} zskiplistNode;

// 跳跃表,用于保存跳跃表节点的相关信息
typedef struct zskiplist {
    struct zskiplistNode *header; // 表头节点指针
    struct zskiplistNode *tail;   // 表尾节点指针
    unsigned long length;         // 节点数量
    int level;                    // 表中层数最大的节点的层数,
    // 用于在O(1)内获取跳跃表中层高最大的那个节点的层数量
} zskiplist;

// 有序集合: 跳表
typedef struct zset {
    // 字典,键为成员,值为分值
    // 用于支持 O(1) 复杂度的按成员取分值操作
    dict *dict;
    // 跳跃表,按分值排序成员
    // 用于支持平均复杂度为 O(log N) 的按分值定位成员操作
    // 以及范围操作
    zskiplist *zsl;
} zset;

// 客户端输出缓冲区限制
typedef struct clientBufferLimitsConfig {
    unsigned long long hard_limit_bytes; // 硬限制 如果输出缓冲区的大小超过了硬性限制 所设置的大小,那么服务器立即关闭客户端.

    //    如果输出缓冲区的大小超过了软性限制所设置的大小,但还没超过硬性限制,那么服务器将使用客户端状态结构
    //    的 obuf_soft_limit_reached_time 属性记录下客户端到达软性限制的起始时间;之后服务器会继续监视客户端,
    //    如果输出缓冲区的大小一直超出软性限制,并且持续时间超过服务器设定的时长,那么服务器将关闭客户端;
    //    相反地,如果输出缓冲区的大小在指定时间之内,不再超出软性限制,那么客户端就不会被关闭,
    //    并且obuf_soft_limit_reached_time属性的值也会被清零.
    unsigned long long soft_limit_bytes; // 软限制
    time_t soft_limit_seconds;           // 软限制时限
} clientBufferLimitsConfig;

// 限制可以有多个
extern clientBufferLimitsConfig clientBufferLimitsDefaults[CLIENT_TYPE_OBUF_COUNT]; // 客户端输出缓冲区限制

// redisOp 结构定义了一个 Redis 操作,
// 目前只用于在传播被执行命令之后,传播附加的其他命令到 AOF 或 Replication 中.
typedef struct redisOp {
    robj **argv; // 参数
    int argc;    // 命令的参数
    int dbid;    // 数据库 ID
    int target;  // 传播目标
} redisOp;

/* Defines an array of Redis operations. There is an API to add to this
 * structure in an easy way.
 *
 * redisOpArrayInit();
 * redisOpArrayAppend();
 * redisOpArrayFree();
 */
typedef struct redisOpArray {
    redisOp *ops;
    int numops;
    int capacity;
} redisOpArray;

/* This structure is returned by the getMemoryOverheadData() function in
 * order to return memory overhead information. */
struct redisMemOverhead {
    size_t peak_allocated;
    size_t total_allocated;
    size_t startup_allocated;
    size_t repl_backlog;
    size_t clients_slaves;
    size_t clients_normal;
    size_t cluster_links;
    size_t aof_buffer;
    size_t lua_caches;
    size_t functions_caches;
    size_t overhead_total;
    size_t dataset;
    size_t total_keys;
    size_t bytes_per_key;
    float dataset_perc;
    float peak_perc;
    float total_frag;
    ssize_t total_frag_bytes;
    float allocator_frag;
    ssize_t allocator_frag_bytes;
    float allocator_rss;
    ssize_t allocator_rss_bytes;
    float rss_extra;
    size_t rss_extra_bytes;
    size_t num_dbs;
    struct {
        size_t dbid;
        size_t overhead_ht_main;
        size_t overhead_ht_expires;
        size_t overhead_ht_slot_to_keys;
    } *db;
};

/* Replication error behavior determines the replica behavior
 * when it receives an error over the replication stream. In
 * either case the error is logged. */
typedef enum { PROPAGATION_ERR_BEHAVIOR_IGNORE = 0, PROPAGATION_ERR_BEHAVIOR_PANIC, PROPAGATION_ERR_BEHAVIOR_PANIC_ON_REPLICAS } replicationErrorBehavior;

/* This structure can be optionally passed to RDB save/load functions in
 * order to implement additional functionalities, by storing and loading
 * metadata to the RDB file.
 *
 * For example, to use select a DB at load time, useful in
 * replication in order to make sure that chained slaves (slaves of slaves)
 * select the correct DB and are able to accept the stream coming from the
 * top-level master. */
// 快照信息
typedef struct rdbSaveInfo {
    /* Used saving and loading. */
    int repl_stream_db; /* DB to select in server.master client. */

    /* Used only loading. */
    int repl_id_is_set;                   /* True if repl_id field is set. */
    char repl_id[CONFIG_RUN_ID_SIZE + 1]; /* Replication ID. */
    long long repl_offset;                /* Replication offset. */
} rdbSaveInfo;

#define RDB_SAVE_INFO_INIT \
    { -1, 0, "0000000000000000000000000000000000000000", -1 }

struct malloc_stats {
    size_t zmalloc_used;
    size_t process_rss;
    size_t allocator_allocated;
    size_t allocator_active;
    size_t allocator_resident;
};

typedef struct socketFds {
    int fd[CONFIG_BINDADDR_MAX]; // 描述符  16
    int count;                   // 描述符数量
} socketFds;

/*-----------------------------------------------------------------------------
 * TLS Context Configuration
 *----------------------------------------------------------------------------*/
// 证书配置信息

typedef struct redisTLSContextConfig {
    char *cert_file;            /* Server side and optionally client side cert file name */
    char *key_file;             /* Private key filename for cert_file */
    char *key_file_pass;        /* Optional password for key_file */
    char *client_cert_file;     /* Certificate to use as a typedef struct rdbSaveInfo {client; if none, use cert_file */
    char *client_key_file;      /* Private key filename for client_cert_file */
    char *client_key_file_pass; /* Optional password for client_key_file */
    char *dh_params_file;
    char *ca_cert_file;
    char *ca_cert_dir;
    char *protocols;
    char *ciphers;
    char *ciphersuites;
    int prefer_server_ciphers;
    int session_caching;
    int session_cache_size;
    int session_cache_timeout;
} redisTLSContextConfig;

/*-----------------------------------------------------------------------------
 * AOF manifest definition
 *----------------------------------------------------------------------------*/
typedef enum {
    AOF_FILE_TYPE_BASE = 'b', /* BASE file */
    AOF_FILE_TYPE_HIST = 'h', /* HISTORY file */
    AOF_FILE_TYPE_INCR = 'i', /* INCR file */
} aof_file_type;

typedef struct {
    sds file_name;           /* file name */
    long long file_seq;      /* file sequence */
    aof_file_type file_type; /* file type */
} aofInfo;

typedef struct {
    aofInfo *base_aof_info;       /* BASE file information. NULL if there is no BASE file. */
    list *incr_aof_list;          /* INCR AOFs list. We may have multiple INCR AOF when rewrite fails. */
    list *history_aof_list;       /* HISTORY AOF list. When the AOFRW success, The aofInfo contained in
                                               `base_aof_info` and `incr_aof_list` will be moved to this list. We
                                               will delete these AOF files when AOFRW finish. */
    long long curr_base_file_seq; /* The sequence number used by the current BASE file. */
    long long curr_incr_file_seq; /* The sequence number used by the current INCR file. */
    int dirty;                    /* 1 Indicates that the aofManifest in the memory is inconsistent with
                                                   disk, we need to persist it immediately. */
} aofManifest;

/*-----------------------------------------------------------------------------
 * 服务器全局状态
 *----------------------------------------------------------------------------*/

/* AIX defines hz to __hz, we don't use this define and in order to allow Redis build on AIX we need to undef it. */
#ifdef _AIX
#    undef hz
#endif

#define CHILD_TYPE_NONE 0
#define CHILD_TYPE_RDB 1
#define CHILD_TYPE_AOF 2
#define CHILD_TYPE_LDB 3
#define CHILD_TYPE_MODULE 4

typedef enum childInfoType { CHILD_INFO_TYPE_CURRENT_INFO, CHILD_INFO_TYPE_AOF_COW_SIZE, CHILD_INFO_TYPE_RDB_COW_SIZE, CHILD_INFO_TYPE_MODULE_COW_SIZE } childInfoType;

struct redisServer {
    /* General */
    pid_t pid;                                                               // 主进程pid
    pthread_t main_thread_id;                                                // 主线程id
    char *configfile;                                                        // 配置文件的绝对路径
    char *executable;                                                        // 程序的绝对路径
    char **exec_argv;                                                        // 保存程序内的运行参数
    int dynamic_hz;                                                          /* Change hz value depending on # of clients. */
    int config_hz;                                                           // 调整serverCron的每秒执行次数.如果启用了动态HZ,可能与实际的 "HZ "字段值不同.
    mode_t umask;                                                            /* The umask value of the process on startup */
    int hz;                                                                  // serverCron() 每秒调用的次数
    int in_fork_child;                                                       // 表明这是一个fork得到的server
    redisDb *db;                                                             // 一个数组,保存这服务器中所有的数据库
    dict *commands;                                                          // 命令表（受到 rename 配置选项的作用）
    dict *orig_commands;                                                     // 命令表（无 rename 配置选项的作用）
    aeEventLoop *el;                                                         // 事件循环
    rax *errors;                                                             // 错误表
    redisAtomic unsigned int lru_clock;                                      // 最近一次使用时钟
    volatile sig_atomic_t shutdown_asap;                                     // 关闭服务器的标识
    mstime_t shutdown_mstime;                                                // 优雅关闭限制的时间
    int last_sig_received;                                                   /* Indicates the last SIGNAL received, if any (e.g., SIGINT or SIGTERM). */
    int shutdown_flags;                                                      // 传递给prepareForShutdown()的标志.
    int active_rehashing;                                                    // 在执行 serverCron() 时进行渐进式 rehash
    int active_defrag_running;                                               // 运行中的主动碎片整理
    char *pid_file;                                                          // PID 文件
    int arch_bits;                                                           // 架构类型 32 or 64 depending on sizeof(long) */
    int cronloops;                                                           // serverCron() 函数的运行次数计数器
    char runid[CONFIG_RUN_ID_SIZE + 1];                                      // 本服务器的 RUN ID
    int sentinel_mode;                                                       // 服务器是否运行在 SENTINEL 模式
    size_t initial_memory_usage;                                             // 初始化后使用的字节
    int always_show_logo;                                                    // 即使是非stdout日志也要显示logo
    int in_script;                                                           // 在使用EVAL执行脚本中
    int in_exec;                                                             // 在使用EXEC执行脚本中
    int busy_module_yield_flags;                                             /* 我们在一个繁忙的模块中吗?(由RM_Yield触发).看到BUSY_MODULE_YIELD_ 标志.*/
    const char *busy_module_yield_reply;                                     // 非空,意味着处于RM_Yield状态  ,等会回复消息
    int core_propagates;                                                     // 是核心(相对于模块子系统)负责调用 propagatePendingCommands 吗?
    int propagate_no_multi;                                                  /* True if propagatePendingCommands should avoid wrapping command in MULTI/EXEC */
    int module_ctx_nesting;                                                  /* moduleCreateContext() nesting level */
    char *ignore_warnings;                                                   /* Config: warnings that should be ignored. */
    int client_pause_in_transaction;                                         // 在执行期间是否执行了客户端暂停?
    int thp_enabled;                                                         // true 代表启用 THP
    size_t page_size;                                                        // 操作系统 页大小
    dict *moduleapi;                                                         //  /* Exported core APIs dictionary for modules. */
    dict *sharedapi;                                                         /* Like moduleapi but containing the APIs that modules share with each other. */
    dict *module_configs_queue;                                              /* Dict that stores module configurations from .conf file until after modules are loaded during startup or arguments to loadex. */
    list *loadmodule_queue;                                                  /* List of modules to load at startup. */
    int module_pipe[2];                                                      // 用来唤醒模块线程的事件循环的管道.
    pid_t child_pid;                                                         // 子进程 ID
    int child_type;                                                          // 负责进行AOF重写、负责执行 BGSAVE 的子进程的ID
    int port;                                                                // TCP 监听端口
    int tls_port;                                                            /* TLS listening port */
    int tcp_backlog;                                                         // 控制的是三次握手的时候server端收到client ack确认号之后的队列值,即全连接队列
    char *bindaddr[CONFIG_BINDADDR_MAX];                                     // 最多16个绑定地址
    int bindaddr_count;                                                      // 要绑定的地址数量
    char *bind_source_addr;                                                  /* Source address to bind on for outgoing connections */
    char *unixsocket;                                                        // UNIX 套接字
    unsigned int unixsocketperm;                                             // UNIX 套接字的权限
    socketFds ipfd;                                                          // tcp 套接字数组
    socketFds tlsfd;                                                         // tls 套接字
    int sofd;                                                                // unix套接字  文件描述符, 因为只会有一个unix.socket 这里就直接放了一个编号
    uint32_t socket_mark_id;                                                 // 监听套接字标记的ID
    socketFds cfd;                                                           /* Cluster bus listening socket */
    list *clients;                                                           // 一个链表、保存了所有客户端
    list *clients_to_close;                                                  // 链表,保存了所有待关闭的客户端          , 添加连接时,可能需要锁
    list *clients_pending_write;                                             // 要写或安装的处理程序.
    list *clients_pending_read;                                              // 客户端有待读的套接字缓冲区.
    list *slaves;                                                            // 保存了所有从服务器
    list *monitors;                                                          // 保存了所有监视器
    client *current_client;                                                  // 服务器的当前客户端,仅用于崩溃报告
    int clients_paused;                                                      // 如果客户端当前处于暂停状态,则为True
    clientMemUsageBucket client_mem_usage_buckets[CLIENT_MEM_USAGE_BUCKETS]; // 19  客户端使用的内存 进行存储
    rax *clients_timeout_table;                                              // 存放 超时的 阻塞客户端 的 前缀树.
    long fixed_time_expire;                                                  // 如果>0,则根据server.mstime对密钥进行过期.
    int in_nested_call;                                                      // If > 0, 嵌套调用中
    rax *clients_index;                                                      // 活跃的客户按客户ID进行字典排列.
    pause_type client_pause_type;                                            // 如果客户端当前处于暂停状态,则为True
    list *postponed_clients;                                                 /* List of postponed clients */
    mstime_t client_pause_end_time;                                          // 撤销暂停的时间
    pause_event *client_pause_per_purpose[NUM_PAUSE_PURPOSES];               //
    char neterr[ANET_ERR_LEN];                                               // 网络错误
    dict *migrate_cached_sockets;                                            // MIGRATE 缓存
    redisAtomic uint64_t next_client_id;                                     /* Next client unique ID. Incremental. */
    int protected_mode;                                                      /* 不接受外部链接 */
    int io_threads_num;                                                      // 可以使用的总IO线程数
    int io_threads_do_reads;                                                 // 是否从IO线程 读取并解析数据
    int io_threads_active;                                                   // io线程是否被激活
    long long events_processed_while_blocked;                                // 处理阻塞时的事件
    int enable_protected_configs;                                            /* Enable the modification of protected configs, see PROTECTED_ACTION_ALLOWED_* */
    int enable_debug_cmd;                                                    /* Enable DEBUG commands, see PROTECTED_ACTION_ALLOWED_* */
    int enable_module_cmd;                                                   /* Enable MODULE commands, see PROTECTED_ACTION_ALLOWED_* */

    // AOF、RDB加载信息
    volatile sig_atomic_t loading;               // 这个值为真时,表示服务器正在进行载入
    volatile sig_atomic_t async_loading;         // 异步的加载数据中？
    off_t loading_total_bytes;                   // 正在载入的数据的大小
    off_t loading_rdb_used_mem;                  // 加载rdb文件使用的内存
    off_t loading_loaded_bytes;                  // 已载入数据的大小
    time_t loading_start_time;                   // 开始进行载入的时间
    off_t loading_process_events_interval_bytes; //

    // 仅用于统计的字段
    time_t stat_starttime;                         // 服务器启动时间
    long long stat_numcommands;                    // 已处理命令的数量
    long long stat_numconnections;                 // 服务器接到的连接请求数量
    long long stat_expiredkeys;                    // 已过期的键数量
    double stat_expired_stale_perc;                /* Percentage of keys probably expired */
    long long stat_expired_time_cap_reached_count; /* Early expire cycle stops.*/
    long long stat_expire_cycle_time_used;         /* Cumulative microseconds used. */
    long long stat_evictedkeys;                    // 因为回收内存而被释放的过期键的数量
    long long stat_evictedclients;                 /* Number of evicted clients */
    long long stat_total_eviction_exceeded_time;   /* Total time over the memory limit, unit us */
    monotime stat_last_eviction_exceeded_time;     /* Timestamp of current eviction start, unit us */
    long long stat_keyspace_hits;                  // 成功查找键的次数
    long long stat_keyspace_misses;                // 查找键失败的次数
    long long stat_active_defrag_hits;             /* number of allocations moved */
    long long stat_active_defrag_misses;           /* number of allocations scanned but not moved */
    long long stat_active_defrag_key_hits;         /* number of keys with moved allocations */
    long long stat_active_defrag_key_misses;       /* number of keys scanned and not moved */
    long long stat_active_defrag_scanned;          /* number of dictEntries scanned */
    long long stat_total_active_defrag_time;       /* Total time memory fragmentation over the limit, unit us */
    monotime stat_last_active_defrag_time;         /* Timestamp of current active defrag start */
    size_t stat_peak_memory;                       // 已使用内存峰值
    long long stat_aof_rewrites;                   /* number of aof file rewrites performed */
    long long stat_aofrw_consecutive_failures;     /* The number of consecutive failures of aofrw */
    long long stat_rdb_saves;                      /* number of rdb saves performed */
    long long stat_fork_time;                      // 最后一次执行 fork() 时消耗的时间
    double stat_fork_rate;                         /* Fork rate in GB/sec. */
    long long stat_total_forks;                    /* Total count of fork. */
    long long stat_rejected_conn;                  // 服务器因为客户端数量过多而拒绝客户端连接的次数
    long long stat_sync_full;                      // 执行 full sync 的次数
    long long stat_sync_partial_ok;                // PSYNC 成功执行的次数
    long long stat_sync_partial_err;               // PSYNC 失败执行的次数

    //  慢日志
    list *slowlog;                                      // 保存了所有慢查询日志的链表
    long long slowlog_entry_id;                         // 慢查询日志的 ID
    long long slowlog_log_slower_than;                  // 慢日志事件判断 slowlog-log-slower-than 选项的值
    unsigned long slowlog_max_len;                      // 慢日志数量 slowlog-max-len 选项的值
    struct malloc_stats cron_malloc_stats;              /* sampled in serverCron(). */
    redisAtomic long long stat_net_input_bytes;         /* 从网络读取到的字节数 */
    redisAtomic long long stat_net_output_bytes;        /* 写到网络的字节数 */
    size_t stat_current_cow_peak;                       /* Peak size of copy on write bytes. */
    size_t stat_current_cow_bytes;                      /* Copy on write bytes while child is active. */
    monotime stat_current_cow_updated;                  /* Last update time of stat_current_cow_bytes */
    size_t stat_current_save_keys_processed;            /* Processed keys while child is active. */
    size_t stat_current_save_keys_total;                /* Number of keys when child started. */
    size_t stat_rdb_cow_bytes;                          // rdb 过程中 COW 消耗d内存大小
    size_t stat_aof_cow_bytes;                          /* Copy on write bytes during AOF rewrite. */
    size_t stat_module_cow_bytes;                       /* Copy on write bytes during module fork. */
    double stat_module_progress;                        /* Module save progress. */
    size_t stat_clients_type_memory[CLIENT_TYPE_COUNT]; // 记录每种客户端类型的数量
    size_t stat_cluster_links_memory;                   /* Mem usage by cluster links */
    long long stat_unexpected_error_replies;            /* Number of unexpected (aof-loading, replica to master, etc.) error replies */
    long long stat_total_error_replies;                 // 命令失败计数器(rejected_calls或failed_calls).
    long long stat_dump_payload_sanitizations;          /* Number deep dump payloads integrity validations. */
    long long stat_io_reads_processed;                  /* Number of read events processed by IO / Main threads */
    long long stat_io_writes_processed;                 /* Number of write events processed by IO / Main threads */
    redisAtomic long long stat_total_reads_processed;   /* Total number of read events processed */
    redisAtomic long long stat_total_writes_processed;  /* Total number of write events processed */
    /* The following two are used to track instantaneous metrics, like
     * number of operations per second, network traffic. */
    struct {
        long long last_sample_time;              // 最后一次进行抽样的时间
        long long last_sample_count;             // 最后一次抽样时,服务器已执行命令的数量
        long long samples[STATS_METRIC_SAMPLES]; // 抽样结果
        int idx;                                 // 数组索引,用于保存抽样结果,并在需要时回绕到 0
    } inst_metric[STATS_METRIC_COUNT];
    long long stat_reply_buffer_shrinks; /* Total number of output buffer shrinks */
    long long stat_reply_buffer_expands; /* Total number of output buffer expands */

    // 配置
    int verbosity;                                                       // 日志可见性,即日志等级
    int maxidletime;                                                     // 客户端最大空闲时间
    int tcpkeepalive;                                                    // 是否开启 SO_KEEPALIVE 选项
    int active_expire_enabled;                                           // 当测试时,可以禁用
    int active_expire_effort;                                            // 每次定期删除影响的数据库个数  1(默认)-10
    int active_defrag_enabled;                                           //
    int sanitize_dump_payload;                                           /* Enables deep sanitization for ziplist and listpack in RDB and RESTORE. */
    int skip_checksum_validation;                                        // 禁用RDB和RESTORE负载的校验和验证功能.
    int jemalloc_bg_thread;                                              /* Enable jemalloc background thread */
    size_t active_defrag_ignore_bytes;                                   /* minimum amount of fragmentation waste to start active defrag */
    int active_defrag_threshold_lower;                                   /* minimum percentage of fragmentation to start active defrag */
    int active_defrag_threshold_upper;                                   /* maximum percentage of fragmentation at which we use maximum effort */
    int active_defrag_cycle_min;                                         /* minimal effort for defrag in CPU percentage */
    int active_defrag_cycle_max;                                         /* maximal effort for defrag in CPU percentage */
    unsigned long active_defrag_max_scan_fields;                         /* maximum number of fields of set/hash/zset/list to process from within the main dict scan */
    size_t client_max_querybuf_len;                                      /* 限制客户端查询缓冲区的长度  1GB */
    int dbnum;                                                           // 服务器的数据库数量,默认16
    int supervised;                                                      // 程序后台运行时的模式
    int supervised_mode;                                                 /* See SUPERVISED_* */
    int daemonize;                                                       // 是否已后台进程运行
    int set_proc_title;                                                  /* 如果更改proc标题,则为True */
    char *proc_title_template;                                           /* Process title template format */
    clientBufferLimitsConfig client_obuf_limits[CLIENT_TYPE_OBUF_COUNT]; // 客户端输出缓冲区限制,根据用途有不同限制
    int pause_cron;                                                      // 不要运行cron任务（调试）.
    int latency_tracking_enabled;                                        // 1,如果启用扩展延迟跟踪,否则为0.
    double *latency_tracking_info_percentiles;                           // 扩展的延迟跟踪信息输出百分位列表配置.
    int latency_tracking_info_percentiles_len;                           // 延迟跟踪信息几个分位 ,默认3个 值是 50 95 99

    // AOF持久化
    int aof_enabled;                      // 是否启用AOF
    int aof_state;                        // AOF 状态（开启/关闭/等待重写）,默认是关闭
    int aof_fsync;                        // aof落盘策略  fsync
    char *aof_filename;                   /* Basename of the AOF file and manifest file */
    char *aof_dirname;                    /* Name of the AOF directory */
    int aof_no_fsync_on_rewrite;          /* Don't fsync if a rewrite is in prog. */
    int aof_rewrite_perc;                 /* Rewrite AOF if % growth is > M and... */
    off_t aof_rewrite_min_size;           /* the AOF file is at least N bytes. */
    off_t aof_rewrite_base_size;          // 最后一次执行 BGREWRITEAOF 时,AOF 文件的大小
    off_t aof_current_size;               // AOF 文件的当前字节大小
    off_t aof_last_incr_size;             // 最新的AOF文件大小增量
    off_t aof_fsync_offset;               // AOF offset which is already synced to disk. */
    int aof_flush_sleep;                  // 仅用于测试. 在flush前稍微sleep一下
    int aof_rewrite_scheduled;            // 有 AOF 重写操作被设置为了待调度执行=1
    sds aof_buf;                          // aof写缓冲区 ,在每次进入事件循环前
    int aof_fd;                           // AOF 文件的描述符
    int aof_selected_db;                  // AOF 的当前目标数据库
    time_t aof_flush_postponed_start;     // 推迟 write 操作的时间
    time_t aof_last_fsync;                // 最后一直执行 fsync 的时间
    time_t aof_rewrite_time_last;         // AOF 上一次重写的开始时间
    time_t aof_rewrite_time_start;        // AOF 重写开始的时间
    time_t aof_cur_timestamp;             // AOF当前记录的时间戳
    int aof_timestamp_enabled;            // AOF是否允许记录时间戳
    int aof_lastbgrewrite_status;         // 最后一次执行 BGREWRITEAOF 的结果
    unsigned long aof_delayed_fsync;      // 记录 AOF 的 write 操作被推迟了多少次
    int aof_rewrite_incremental_fsync;    // 指示是否需要每写入一定量的数据,就主动执行一次 fsync()
    int rdb_save_incremental_fsync;       // fsync incrementally while rdb saving? */
    int aof_last_write_status;            // C_OK or C_ERR */
    int aof_last_write_errno;             // 如果 aof write/fsync 状态是 ERR ,则无效
    int aof_load_truncated;               // Don't stop on unexpected AOF EOF. */
    int aof_use_rdb_preamble;             // Specify base AOF to use RDB encoding on AOF rewrites. */
    redisAtomic int aof_bio_fsync_status; // bio任务中的AOF同步状态
    redisAtomic int aof_bio_fsync_errno;  // bio任务中的AOF同步错误
    aofManifest *aof_manifest;            // 用于追踪AOF
    int aof_disable_auto_gc;              // 如果禁用自动删除历史类型AOFs?没有违约.(试验).

    // RDB持久化
    long long dirty;                      // 上次 rdb 结束到本次rdb 开始时键值对改变的个数
    long long dirty_before_bgsave;        // BGSAVE 执行前的数据库被修改次数
    long long rdb_last_load_keys_expired; // number of expired keys when loading RDB */
    long long rdb_last_load_keys_loaded;  // number of loaded keys when loading RDB */
    struct saveparam *saveparams;         // save 参数保存的rdb 生成策略
    int saveparamslen;                    // save 参数的个数
    char *rdb_filename;                   // rdb 文件名称
    int rdb_compression;                  // 是否对rdb文件进行压缩 LZF 算法
    int rdb_checksum;                     // 是否进行rdb 文件进行校验
    int rdb_del_sync_files;               // Remove RDB files used only for SYNC if the instance does not use persistence. */
    time_t lastsave;                      // 上一次进行save成功的时间
    time_t lastbgsave_try;                // 最后一次尝试执行 BGSAVE 的时间
    time_t rdb_save_time_last;            // 最近一次 BGSAVE 执行耗费的时间
    time_t rdb_save_time_start;           // 当前正在执行的rdb开始时间
    int rdb_bgsave_scheduled;             // bgsave 调度状态,为1时才能进行bgsave
    int rdb_child_type;                   // rdb 执行类型,本地rdb持久化还是通过socket进行对外发送
    int lastbgsave_status;                // 上一次执行bgsav的状态
    int stop_writes_on_bgsave_err;        // 如果上一次bgsave失败,增redis不在支持数据写入
    int rdb_pipe_read;                    // RDB pipe used to transfer the rdb data */
    // to the parent process in diskless repl. */
    int rdb_child_exit_pipe;       // Used by the diskless parent allow child exit. */
    connection **rdb_pipe_conns;   // Connections which are currently the */
    int rdb_pipe_numconns;         // target of diskless rdb fork child. */
    int rdb_pipe_numconns_writing; // Number of rdb conns with pending writes. */
    char *rdb_pipe_buff;           // In diskless replication, this buffer holds data */
    int rdb_pipe_bufflen;          // that was read from the rdb pipe. */
    int rdb_key_save_delay;        /* Delay in microseconds between keys while
                                    * writing the RDB. (for testings). negative
                                    * value means fractions of microseconds (on average). */
    int key_load_delay;            /* Delay in microseconds between keys while
                                    * loading aof or rdb. (for testings). negative
                                    * value means fractions of microseconds (on average). */
    // Pipe and data structures for child -> parent info sharing. */
    int child_info_pipe[2]; // Pipe used to write the child_info_data. */
    int child_info_nread;   // Num of bytes of the last read from pipe */
    // Propagation of commands in AOF / replication */
    redisOpArray also_propagate; // Additional command to propagate. */
    int replication_allowed;     // 是否允许复制到从节点
    // Logging */
    char *logfile;         // Path of log file */
    int syslog_enabled;    // 是否启用系统日志
    char *syslog_ident;    // Syslog标识
    int syslog_facility;   // Syslog设备
    int crashlog_enabled;  /* Enable signal handler for crashlog.
                            * disable for clean core dumps. */
    int memcheck_enabled;  // Enable memory check on crash. */
    int use_exit_on_panic; /* Use exit() on panic and assert rather than
                            * abort(). useful for Valgrind. */
    // Shutdown */
    int shutdown_timeout;    // Graceful shutdown time limit in seconds. */
    int shutdown_on_sigint;  // Shutdown flags configured for SIGINT. */
    int shutdown_on_sigterm; // Shutdown flags configured for SIGTERM. */

    // master上存储的复制信息
    char replid[CONFIG_RUN_ID_SIZE + 1];  // master当前的复制ID
    char replid2[CONFIG_RUN_ID_SIZE + 1]; // 从master继承的复制ID
    long long master_repl_offset;         // 全局复制偏移量（一个累计值）
    long long second_replid_offset;       // 接受的偏移量
    int slaveseldb;                       // 在复制输出中最后选择的DB
    int repl_ping_slave_period;           // 主服务器发送 PING 到从服务器的频率
    replBacklog *repl_backlog;            // backlog 环形缓冲复制队列
    long long repl_backlog_size;          // 循环缓冲区总长度  backlog 的长度   m->s
    time_t repl_backlog_time_limit;       // backlog 的过期时间
    time_t repl_no_slaves_since;          // 距离上一次有从服务器的时间
    int repl_min_slaves_to_write;         // 表示当有效从服务器的数目小于该值时,主服务器会拒绝执行写命令.
    int repl_min_slaves_max_lag;          // 最少节点最大延迟 10
    int repl_good_slaves_count;           // 延迟良好的从服务器的数量,lag<max_lag
    int repl_diskless_sync;               // Master send RDB to slaves sockets directly. */
    int repl_diskless_load;               // Slave parse RDB directly from the socket. see REPL_DISKLESS_LOAD_* enum */
    int repl_diskless_sync_delay;         // Delay to start a diskless repl BGSAVE. */
    int repl_diskless_sync_max_replicas;  // Max replicas for diskless repl BGSAVE delay (start sooner if they all connect). */
    size_t repl_buffer_mem;               // The memory of replication buffer. */
    list *repl_buffer_blocks;             // Replication buffers blocks list (serving replica clients and repl backlog) */

    // slave 复制
    char *masteruser;                   // 用于和主库进行验证的用户
    sds masterauth;                     // 用于和主库进行验证的密码
    char *masterhost;                   // 主服务器的地址
    int masterport;                     // 主服务器的端口
    int repl_timeout;                   // 超时时间
    client *master;                     // 从库上用于和主库链接的客户端
    client *cached_master;              // 从库上缓存的主库信息
    int repl_syncio_timeout;            // 从库io同步调用时的超时
    int repl_state;                     // 从库的复制状态机
    off_t repl_transfer_size;           // RDB 文件的大小
    off_t repl_transfer_read;           // 已读 RDB 文件内容的字节数
    off_t repl_transfer_last_fsync_off; // 最近一次执行 fsync 时的偏移量
    connection *repl_transfer_s;        // 主服务器的套接字
    int repl_transfer_fd;               // 保存 RDB 文件的临时文件的描述符
    char *repl_transfer_tmpfile;        // 保存 RDB 文件的临时文件名字
    time_t repl_transfer_lastio;        // 最近一次读 RDB 内容的时间
    int repl_serve_stale_data;          // 当链路中断时,提供陈旧的数据？
    int repl_slave_ro;                  // slave 是否只读 ,默认1 ？  read only -> ro
    int repl_slave_ignore_maxmemory;    // 如果是真的slave,就不要驱逐.
    time_t repl_down_since;             // 连接断开的时长
    int repl_disable_tcp_nodelay;       // 是否要在 SYNC 之后关闭 NODELAY ？
    int slave_priority;                 // 从服务器优先级

    int replica_announced;            // If true, replica is announced by Sentinel */
    int slave_announce_port;          // Give the master this listening port. */
    char *slave_announce_ip;          // Give the master this ip address. */
    int propagation_error_behavior;   // 副本在复制流上接收到错误时的行为
    int repl_ignore_disk_write_error; // 副本在无法持续写入AOF时是否出现panic.

    // 下面的两个字段是我们在PSYNC进行中 存储主PSYNC 副本/偏移量的地方.
    // 最后我们将这些字段复制到  server->master client 结构中.
    char master_replid[CONFIG_RUN_ID_SIZE + 1]; // Master PSYNC runid. */
    long long master_initial_offset;            // master PSYNC 的偏移量
    int repl_slave_lazy_flush;                  // Lazy FLUSHALL before loading DB? */
    // 同步复制.
    list *clients_waiting_acks; // 在WAIT命令中等待的客户端
    int get_ack_from_slaves;    // 如果为真,我们发送REPLCONF GETACK.
    // 限流
    unsigned int maxclients;                    // 最大并发客户端数  【集群使用的链接数、以及client使用的链接】
    unsigned long long maxmemory;               // 内存使用的最大字节数 */
    ssize_t maxmemory_clients;                  // 客户端总缓冲区的内存限制 */
    int maxmemory_policy;                       // key驱逐策略 */
    int maxmemory_samples;                      // 缓存淘汰时, 随机抽样的个数
    int maxmemory_eviction_tenacity;            // 驱逐处理的侵略性 */
    int lfu_log_factor;                         // LFU 计数器因子,默认值是10
    int lfu_decay_time;                         // LFU 衰减因子,用于控制计数器是255后,不在访问的key的衰减,以便删除
    long long proto_max_bulk_len;               // Protocol bulk length maximum size. */
    int oom_score_adj_values[CONFIG_OOM_COUNT]; // Linux oom_score_adj configuration */
    int oom_score_adj;                          // If true, oom_score_adj is managed */
    int disable_thp;                            // If true, disable THP by syscall */
    // 阻塞的客户端
    unsigned int blocked_clients;                      // 执行阻塞cmd的客户端
    unsigned int blocked_clients_by_type[BLOCKED_NUM]; //
    list *unblocked_clients;                           // 在下一个循环之前要解除阻塞的客户端列表
    list *ready_keys;                                  // BLPOP & co的readyList结构的列表
    // 客户端缓存
    unsigned int tracking_clients;  // 启用跟踪的客户端.
    size_t tracking_table_max_keys; // 跟踪表中的最大key数
    list *tracking_pending_keys;    // tracking invalidation keys pending to flush */
    /* Sort parameters - qsort_r() is only available under BSD so we
     * have to take this state global, in order to pass it to sortCompare() */
    int sort_desc;
    int sort_alpha;
    int sort_bypattern;
    int sort_store;
    // Zip structure config, see redis.conf for more information  */
    size_t hash_max_listpack_entries;
    size_t hash_max_listpack_value;
    size_t set_max_intset_entries;
    size_t zset_max_listpack_entries;
    size_t zset_max_listpack_value;
    size_t hll_sparse_max_bytes;
    size_t stream_node_max_bytes;
    long long stream_node_max_entries;
    // List parameters */
    int list_max_listpack_size;
    int list_compress_depth;
    // time cache */
    redisAtomic time_t unixtime; // Unix time sampled every cron cycle. */
    time_t timezone;             // 时区
    int daylight_active;         // 夏令时标志(大于0说明夏令时有效,等于0说明无效,小于0说明信息不可用)
    mstime_t mstime;             // 毫秒
    ustime_t ustime;             // 毫秒
    size_t blocking_op_nesting;  // Nesting level of blocking operation, used to reset blocked_last_cron. */
    long long blocked_last_cron; // Indicate the mstime of the last time we did cron jobs from a blocking operation */

    // Pubsub
    dict *pubsub_channels;      // {频道:链表[客户端]}
    dict *pubsub_patterns;      // 记录客户端订阅的所有模式的名字
    int notify_keyspace_events; // 设置通过Pub/Sub传播的事件 , 允许bit置1
    dict *pubsubshard_channels; // Map channels to list of subscribed clients */

    // 集群
    int cluster_enabled;                                 // Is cluster enabled? */
    int cluster_port;                                    // Set the cluster port for a node. */
    mstime_t cluster_node_timeout;                       // Cluster node timeout. */
    char *cluster_configfile;                            // Cluster auto-generated config file name. */
    struct clusterState *cluster;                        // State of the cluster */
    int cluster_migration_barrier;                       // Cluster replicas migration barrier. */
    int cluster_allow_replica_migration;                 // Automatic replica migrations to orphaned masters and from empty masters */
    int cluster_slave_validity_factor;                   // Slave max data age for failover. */
    int cluster_require_full_coverage;                   // If true, put the cluster down if there is at least an uncovered slot.*/
    int cluster_slave_no_failover;                       // Prevent slave from starting a failover if the master is in failure state. */
    char *cluster_announce_ip;                           // IP address to announce on cluster bus. */
    char *cluster_announce_hostname;                     // hostname to announce on cluster bus. */
    int cluster_preferred_endpoint_type;                 // Use the announced hostname when available. */
    int cluster_announce_port;                           // base port to announce on cluster bus. */
    int cluster_announce_tls_port;                       // TLS port to announce on cluster bus. */
    int cluster_announce_bus_port;                       // bus port to announce on cluster bus. */
    int cluster_module_flags;                            // Set of flags that Redis modules are able to set in order to suppress certain native Redis Cluster features. Check the REDISMODULE_CLUSTER_FLAG_*. */
    int cluster_allow_reads_when_down;                   // Are reads allowed when the cluster is down? */
    int cluster_config_file_lock_fd;                     // cluster config fd, will be flock */
    unsigned long long cluster_link_sendbuf_limit_bytes; // Memory usage limit on individual link send buffers*/
    int cluster_drop_packet_filter;                      // Debug config that allows tactically dropping packets of a specific type */

    // 脚本
    client *script_caller;          // 正在运行脚本的客户端
    mstime_t busy_reply_threshold;  // Script / module 超时 毫秒
    int script_oom;                 // 脚本启动时检测到OOM
    int script_disable_deny_script; // Allow running commands marked "no-script" inside a script. */

    // 关于不同阶段赖删除的控制参数,1启用
    int lazyfree_lazy_eviction;
    int lazyfree_lazy_expire; // 是否惰性过期   expire
    int lazyfree_lazy_server_del;
    int lazyfree_lazy_user_del;
    int lazyfree_lazy_user_flush;

    // 延迟监测
    long long latency_monitor_threshold; // fast-command 、command  执行的最长时间
    dict *latency_events;                //

    // ACLs */
    char *acl_filename;           // ACL Users file. NULL if not configured. */
    unsigned long acllog_max_len; // Maximum length of the ACL LOG list. */
    sds requirepass;              // 明文密码
    int acl_pubsub_default;       // Default ACL pub/sub channels flag */
    // Assert & bug reporting */
    int watchdog_period; // Software watchdog period in ms. 0 = off */
    // 系统硬件信息
    size_t system_memory_size; // 系统中由操作系统报告的总内存
    // 证书配置
    int tls_cluster;
    int tls_replication;
    int tls_auth_clients;
    redisTLSContextConfig tls_ctx_config;
    // CPU亲和性列表 */
    char *server_cpulist;      // redis服务器主/io线程的CPU亲和性列表.*/
    char *bio_cpulist;         // 后台线程的CPU亲和性列表. */
    char *aof_rewrite_cpulist; // aof rewrite线程的CPU亲和性列表 */
    char *bgsave_cpulist;      // bgsave 线程的CPU亲和性列表 */
    // 哨兵配置 */
    struct sentinelConfig *sentinel_config; // sentinel config to load at startup time. */

    // 故障转移的信息
    mstime_t failover_end_time;              // 故障转移命令的最后期限.
    int force_failover;                      // 如果为true,那么故障转移将在最后期限强制进行,否则故障转移将被中止.
    char *target_replica_host;               // 故障转移的目标主机.如果在故障转移期间为空,那么可以使用任何副本.
    int target_replica_port;                 // 故障转移时的目标主机端口
    int failover_state;                      // 故障转移的状态
    int cluster_allow_pubsubshard_when_down; // 当集群关闭时,是否允许pubsubshard ,并不影响pubsub
    long reply_buffer_peak_reset_time;       // 等待 回复缓冲区峰值重置的时间（以毫秒为单位）.
    int reply_buffer_resizing_enabled;       // 是否启用了回复缓冲区的大小调整（默认为1）.
};

#define MAX_KEYS_BUFFER 256

typedef struct {
    int pos;   // The position of the key within the client array */
    int flags; /* The flags associated with the key access, see
                  CMD_KEY_* for more information */
} keyReference;

/* A result structure for the various getkeys function calls. It lists the
 * keys as indices to the provided argv. This functionality is also re-used
 * for returning channel information.
 */
typedef struct {
    keyReference keysbuf[MAX_KEYS_BUFFER]; // Pre-allocated buffer, to save heap allocations */
    keyReference *keys;                    // Key indices array, points to keysbuf or heap */
    int numkeys;                           // Number of key indices return */
    int size;                              // Available array size */
} getKeysResult;
#define GETKEYS_RESULT_INIT \
    { {{0}}, NULL, 0, MAX_KEYS_BUFFER }

/* 主要规格定义.
 *
 * Brief: 这个方案试图比旧的[first,last,step]方案更好地描述关键参数的位置,旧的[first,last,step]方案有局限性,不适合很多命令.
 *
 * 有两个步骤:
 * 1. begin_search (BS): 我们应该在哪个索引中开始搜索key?
 * 2. find_keys (FK): 相对于BS的输出,我们如何知道哪些参数是key?
 *
 * There are two types of BS:
 * 1. index: 关键参数从一个常量索引开始
 * 2. keyword: 关键参数开始于特定的关键字之后
 *
 * There are two kinds of FK:
 * 1. range: 键以特定索引结束(或相对于最后一个参数)
 * 2. keynum: 在键本身之前有一个参数,它包含键参数的数量
 */

typedef enum {
    KSPEC_BS_INVALID = 0, /* Must be 0 */
    KSPEC_BS_UNKNOWN,
    KSPEC_BS_INDEX,
    KSPEC_BS_KEYWORD
} kspec_bs_type;

typedef enum {
    KSPEC_FK_INVALID = 0, /* Must be 0 */
    KSPEC_FK_UNKNOWN,
    KSPEC_FK_RANGE,
    KSPEC_FK_KEYNUM
} kspec_fk_type;

//
typedef struct {
    const char *notes;
    uint64_t flags;
    kspec_bs_type begin_search_type;
    union
    {
        struct {
            /* The index from which we start the search for keys */
            int pos;
        } index;
        struct {
            /* The keyword that indicates the beginning of key args */
            const char *keyword;
            /* An index in argv from which to start searching.
             * Can be negative, which means start search from the end, in reverse
             * (Example: -2 means to start in reverse from the penultimate arg) */
            int startfrom;
        } keyword;
    } bs;
    kspec_fk_type find_keys_type;
    union
    {
        /* NOTE: Indices in this struct are relative to the result of the begin_search step!
         * These are: range.lastkey, keynum.keynumidx, keynum.firstkey */
        struct {
            /* Index of the last key.
             * Can be negative, in which case it's not relative. -1 indicating till the last argument,
             * -2 one before the last and so on. */
            int lastkey;
            /* How many args should we skip after finding a key, in order to find the next one. */
            int keystep;
            /* If lastkey is -1, we use limit to stop the search by a factor. 0 and 1 mean no limit.
             * 2 means 1/2 of the remaining args, 3 means 1/3, and so on. */
            int limit;
        } range;
        struct {
            /* Index of the argument containing the number of keys to come */
            int keynumidx;
            /* Index of the fist key (Usually it's just after keynumidx, in
             * which case it should be set to keynumidx+1). */
            int firstkey; // 指定哪些参数是 key
            /* How many args should we skip after finding a key, in order to find the next one. */
            int keystep;
        } keynum;
    } fk;
} keySpec;

/* Number of static key specs */
#define STATIC_KEY_SPECS_NUM 4

// https://redis.io/docs/reference/command-arguments/
typedef enum {
    ARG_TYPE_STRING,     //
    ARG_TYPE_INTEGER,    //
    ARG_TYPE_DOUBLE,     //
    ARG_TYPE_KEY,        // 表示键名的字符串
    ARG_TYPE_PATTERN,    //
    ARG_TYPE_UNIX_TIME,  //
    ARG_TYPE_PURE_TOKEN, // 参数是一个标记,表示保留关键字,可能提供也可能不提供.
    ARG_TYPE_ONEOF,      // 有子参数
    ARG_TYPE_BLOCK       // 有子参数
} redisCommandArgType;

#define CMD_ARG_NONE (0)
#define CMD_ARG_OPTIONAL (1 << 0)       // 表示参数是可选的
#define CMD_ARG_MULTIPLE (1 << 1)       // 表示参数可以重复
#define CMD_ARG_MULTIPLE_TOKEN (1 << 2) // 表示参数与其前面的标记可能重复(参见SORT的GET模式子句).

// 命令参数
typedef struct redisCommandArg {
    const char *name;                // 参数名
    redisCommandArgType type;        // 命令参数的类型
    int key_spec_index;              // 该值可用于键类型的每个参数
    const char *token;               // 参数（用户输入）本身之前的常量文字.
    const char *summary;             // 参数的简短描述
    const char *since;               //
    int flags;                       //
    const char *deprecated_since;    // 弃用时的版本
    struct redisCommandArg *subargs; //
    int num_args;                    // 运行时填充数据
} redisCommandArg;

/* Must be synced with RESP2_TYPE_STR and generate-command-code.py */
typedef enum {
    RESP2_SIMPLE_STRING,
    RESP2_ERROR,
    RESP2_INTEGER,
    RESP2_BULK_STRING,
    RESP2_NULL_BULK_STRING,
    RESP2_ARRAY,
    RESP2_NULL_ARRAY,
} redisCommandRESP2Type;

/* Must be synced with RESP3_TYPE_STR and generate-command-code.py */
typedef enum {
    RESP3_SIMPLE_STRING,
    RESP3_ERROR,
    RESP3_INTEGER,
    RESP3_DOUBLE,
    RESP3_BULK_STRING,
    RESP3_ARRAY,
    RESP3_MAP,
    RESP3_SET,
    RESP3_BOOL,
    RESP3_NULL,
} redisCommandRESP3Type;

typedef struct {
    const char *since;
    const char *changes;
} commandHistory;

// generate-command-code.py 与COMMAND_GROUP_STR 同步
typedef enum {
    COMMAND_GROUP_GENERIC, // 一般
    COMMAND_GROUP_STRING,  //
    COMMAND_GROUP_LIST,
    COMMAND_GROUP_SET,
    COMMAND_GROUP_SORTED_SET,
    COMMAND_GROUP_HASH,
    COMMAND_GROUP_PUBSUB,
    COMMAND_GROUP_TRANSACTIONS,
    COMMAND_GROUP_CONNECTION,
    COMMAND_GROUP_SERVER,
    COMMAND_GROUP_SCRIPTING,
    COMMAND_GROUP_HYPERLOGLOG,
    COMMAND_GROUP_CLUSTER,
    COMMAND_GROUP_SENTINEL,
    COMMAND_GROUP_GEO,
    COMMAND_GROUP_STREAM,
    COMMAND_GROUP_BITMAP,
    COMMAND_GROUP_MODULE,
} redisCommandGroup;

typedef void redisCommandProc(client *c);

typedef int redisGetKeysProc(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

// Redis 命令
struct redisCommand {
    const char *declared_name;                      // 代表命令声明的名称的字符串 比如set
    const char *summary;                            // 命令总结
    const char *complexity;                         // 复杂性描述(可选).
    const char *since;                              // 该命令的首次版本(可选).
    int doc_flags;                                  // 文档标志(参见CMD_DOC_*).
    const char *replaced_by;                        // 如果命令已弃用,这是后续命令.
    const char *deprecated_since;                   // 如果命令被弃用,它是什么时候发生的?
    redisCommandGroup group;                        // 命令属于哪个组
    commandHistory *history;                        // 命令历史
    const char **tips;                              // 一个字符串数组,用于提示客户端/代理使用此命令
    redisCommandProc *proc;                         // 一个指向命令的实现函数的指针
    int arity;                                      // 命令参数的个数,用于检查命令请求的格式是否正确. 注意命令本身也是一个参数 可以用 -N 表示 >= N
    uint64_t flags;                                 // 位掩码形式的 FLAG
    uint64_t acl_categories;                        /* ACl categories, see ACL_CATEGORY_*. */
    keySpec key_specs_static[STATIC_KEY_SPECS_NUM]; // key规格
    /* Use a function to determine keys arguments in a command line.
     * Used for Redis Cluster redirect (may be NULL) */
    redisGetKeysProc *getkeys_proc;   // 从命令中判断命令的键参数.在 Redis 集群转向时使用.
    struct redisCommand *subcommands; // 子命令数组或者NULL
    struct redisCommandArg *args;     // 参数数组,可能为NULL

    // 统计信息
    long long microseconds;   // 记录了命令执行耗费的总毫微秒数
    long long calls;          // calls 是命令被执行的总次数
    long long rejected_calls; // failed_calls 是命令失败的总次数
    long long failed_calls;   // rejected_calls 是命令被拒绝的总次数

    int id;                                  // 命令ID.这是一个从0开始的渐进ID,在运行时分配,用于acl检查.如果与连接相关联的用户在允许的命令位图中设置了此命令位,则连接能够执行给定的命令.
    sds fullname;                            // 命令的名字
    struct hdr_histogram *latency_histogram; // 命令延迟直方图(时间单位纳秒)
    keySpec *key_specs;
    keySpec legacy_range_key_spec; // 遗留的(第一个、最后一个、第一步)键规范仍然被维护(如果适用的话),以便我们仍然可以支持COMMAND INFO和COMMAND GETKEYS的回复格式
    int num_args;
    int num_history;
    int num_tips;
    int key_specs_num; // 检查用户可以显式地执行命令的数量
    int key_specs_max;
    dict *subcommands_dict; // 保存子命令的字典,键是子命令的sds名(不是全名),值是redisCommand结构指针.
    struct redisCommand *parent;
};

struct redisError {
    long long count;
};

struct redisFunctionSym {
    char *name;
    unsigned long pointer;
};
// 用于保存被排序值及其权重的结构
typedef struct _redisSortObject {
    robj *obj; // 被排序键的值

    union // 权重
    {
        double score; // 排序数字值时使用
        robj *cmpobj; // 排序字符串时使用
    } u;
} redisSortObject;
// 排序操作
typedef struct _redisSortOperation {
    int type;      // 操作的类型,可以是 GET 、 DEL 、INCR 或者 DECR
    robj *pattern; // 用户给定的模式

} redisSortOperation;

// 列表迭代器对象
typedef struct {
    robj *subject;           // 列表对象
    unsigned char encoding;  // 对象所使用的编码
    unsigned char direction; // 迭代的方向
    quicklistIter *iter;
} listTypeIterator;

// 迭代列表时使用的记录结构,
// 用于保存迭代器,以及迭代器返回的列表节点.
typedef struct {
    listTypeIterator *li; // 列表迭代器
    quicklistEntry entry; /* Entry in quicklist */
} listTypeEntry;

// 多态集合迭代器
typedef struct {
    robj *subject;    // 被迭代的对象
    int encoding;     // 对象的编码
    int ii;           // 索引值,编码为 intset 时使用
    dictIterator *di; // 字典迭代器,编码为 HT 时使用
} setTypeIterator;

/*  */
// 哈希对象的迭代器
typedef struct {
    robj *subject;              // 被迭代的哈希对象
    int encoding;               // 哈希对象的编码
    unsigned char *fptr, *vptr; // 域指针和值指针  在迭代 ZIPLIST 编码的哈希对象时使用
    dictIterator *di;           // 字典迭代器和指向当前迭代字典节点的指针
    dictEntry *de;              // 在迭代 HT 编码的哈希对象时使用
} hashTypeIterator;

#include "stream.h"

#define OBJ_HASH_KEY 1
#define OBJ_HASH_VALUE 2

#define IO_THREADS_OP_IDLE 0
#define IO_THREADS_OP_READ 1
#define IO_THREADS_OP_WRITE 2
extern int io_threads_op; // IO线程现在要干的事件类型，来控制当前 IO 线程应该处理读还是写事件

/*-----------------------------------------------------------------------------
 * Extern declarations
 *----------------------------------------------------------------------------*/

extern struct redisServer server;
extern struct sharedObjectsStruct shared;
extern dictType objectKeyPointerValueDictType;
extern dictType objectKeyHeapPointerValueDictType;
extern dictType setDictType;
extern dictType BenchmarkDictType;
extern dictType zsetDictType;
extern dictType dbDictType;
extern double R_Zero, R_PosInf, R_NegInf, R_Nan;
extern dictType hashDictType;
extern dictType stringSetDictType;
extern dictType externalStringType;
extern dictType sdsHashDictType;
extern dictType dbExpiresDictType;
extern dictType modulesDictType;
extern dictType sdsReplyDictType;
extern dict *modules;

/*-----------------------------------------------------------------------------
 * Functions prototypes
 *----------------------------------------------------------------------------*/

/* Command metadata */
void populateCommandLegacyRangeSpec(struct redisCommand *c);

int populateArgsStructure(struct redisCommandArg *args);

/* Modules */
void moduleInitModulesSystem(void);

void moduleInitModulesSystemLast(void);

void modulesCron(void);

int moduleLoad(const char *path, void **argv, int argc, int is_loadex);

int moduleUnload(sds name);

void moduleLoadFromQueue(void);

int moduleGetCommandKeysViaAPI(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

int moduleGetCommandChannelsViaAPI(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

moduleType *moduleTypeLookupModuleByID(uint64_t id);

void moduleTypeNameByID(char *name, uint64_t moduleid);

const char *moduleTypeModuleName(moduleType *mt);

const char *moduleNameFromCommand(struct redisCommand *cmd);

void moduleFreeContext(struct RedisModuleCtx *ctx);

void unblockClientFromModule(client *c);

void moduleHandleBlockedClients(void);

void moduleBlockedClientTimedOut(client *c);

void modulePipeReadable(aeEventLoop *el, int fd, void *privdata, int mask);

size_t moduleCount(void);

void moduleAcquireGIL(void);

int moduleTryAcquireGIL(void);

void moduleReleaseGIL(void);

void moduleNotifyKeyspaceEvent(int type, const char *event, robj *key, int dbid);

void moduleCallCommandFilters(client *c);

void ModuleForkDoneHandler(int exitcode, int bysignal);

int TerminateModuleForkChild(int child_pid, int wait);

ssize_t rdbSaveModulesAux(rio *rdb, int when);

int moduleAllDatatypesHandleErrors();

int moduleAllModulesHandleReplAsyncLoad();

sds modulesCollectInfo(sds info, dict *sections_dict, int for_crash_report, int sections);

void moduleFireServerEvent(uint64_t eid, int subid, void *data);

void processModuleLoadingProgressEvent(int is_aof);

int moduleTryServeClientBlockedOnKey(client *c, robj *key);

void moduleUnblockClient(client *c);

int moduleBlockedClientMayTimeout(client *c);

int moduleClientIsBlockedOnKeys(client *c);

void moduleNotifyUserChanged(client *c);

void moduleNotifyKeyUnlink(robj *key, robj *val, int dbid);

size_t moduleGetFreeEffort(robj *key, robj *val, int dbid);

size_t moduleGetMemUsage(robj *key, robj *val, size_t sample_size, int dbid);

robj *moduleTypeDupOrReply(client *c, robj *fromkey, robj *tokey, int todb, robj *value);

int moduleDefragValue(robj *key, robj *obj, long *defragged, int dbid);

int moduleLateDefrag(robj *key, robj *value, unsigned long *cursor, long long endtime, long long *defragged, int dbid);

long moduleDefragGlobals(void);

void *moduleGetHandleByName(char *modulename);

int moduleIsModuleCommand(void *module_handle, struct redisCommand *cmd);

/* Utils */
long long ustime(void);

long long mstime(void);

void getRandomHexChars(char *p, size_t len);

void getRandomBytes(unsigned char *p, size_t len);

uint64_t crc64(uint64_t crc, const unsigned char *s, uint64_t l);

void exitFromChild(int retcode);

long long redisPopcount(void *s, long count);

int redisSetProcTitle(char *title);

int validateProcTitleTemplate(const char *template);

int redisCommunicateSystemd(const char *sd_notify_msg);

void redisSetCpuAffinity(const char *cpulist);

/* afterErrorReply flags */
#define ERR_REPLY_FLAG_NO_STATS_UPDATE (1ULL << 0) /* Indicating that we should not update error stats after sending error reply */

/* networking.c -- Networking and Client related operations */
client *createClient(connection *conn);

void freeClient(client *c);

void freeClientAsync(client *c);

void logInvalidUseAndFreeClientAsync(client *c, const char *fmt, ...);

int beforeNextClient(client *c);

void clearClientConnectionState(client *c);

void resetClient(client *c);

void freeClientOriginalArgv(client *c);

void freeClientArgv(client *c);

void sendReplyToClient(connection *conn);

void *addReplyDeferredLen(client *c);

void setDeferredArrayLen(client *c, void *node, long length);

void setDeferredMapLen(client *c, void *node, long length);

void setDeferredSetLen(client *c, void *node, long length);

void setDeferredAttributeLen(client *c, void *node, long length);

void setDeferredPushLen(client *c, void *node, long length);

int processInputBuffer(client *c);

void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask);

void acceptTLSHandler(aeEventLoop *el, int fd, void *privdata, int mask);

void acceptUnixHandler(aeEventLoop *el, int fd, void *privdata, int mask);

void readQueryFromClient(connection *conn);

int prepareClientToWrite(client *c);

void addReplyNull(client *c);

void addReplyNullArray(client *c);

void addReplyBool(client *c, int b);

void addReplyVerbatim(client *c, const char *s, size_t len, const char *ext);

void addReplyProto(client *c, const char *s, size_t len);

void AddReplyFromClient(client *c, client *src);

void addReplyBulk(client *c, robj *obj);

void addReplyBulkCString(client *c, const char *s);

void addReplyBulkCBuffer(client *c, const void *p, size_t len);

void addReplyBulkLongLong(client *c, long long ll);

void addReply(client *c, robj *obj);

void addReplySds(client *c, sds s);

void addReplyBulkSds(client *c, sds s);

void setDeferredReplyBulkSds(client *c, void *node, sds s);

void addReplyErrorObject(client *c, robj *err);

void addReplyOrErrorObject(client *c, robj *reply);

void afterErrorReply(client *c, const char *s, size_t len, int flags);

void addReplyErrorSdsEx(client *c, sds err, int flags);

void addReplyErrorSds(client *c, sds err);

void addReplyError(client *c, const char *err);

void addReplyErrorArity(client *c);

void addReplyErrorExpireTime(client *c);

void addReplyStatus(client *c, const char *status);

void addReplyDouble(client *c, double d);

void addReplyLongLongWithPrefix(client *c, long long ll, char prefix);

void addReplyBigNum(client *c, const char *num, size_t len);

void addReplyHumanLongDouble(client *c, long double d);

void addReplyLongLong(client *c, long long ll);

void addReplyArrayLen(client *c, long length);

void addReplyMapLen(client *c, long length);

void addReplySetLen(client *c, long length);

void addReplyAttributeLen(client *c, long length);

void addReplyPushLen(client *c, long length);

void addReplyHelp(client *c, const char **help);

void addReplySubcommandSyntaxError(client *c);

void addReplyLoadedModules(client *c);

void copyReplicaOutputBuffer(client *dst, client *src);

void addListRangeReply(client *c, robj *o, long start, long end, int reverse);

void deferredAfterErrorReply(client *c, list *errors);

size_t sdsZmallocSize(sds s);

size_t getStringObjectSdsUsedMemory(robj *o);

void freeClientReplyValue(void *o);

void *dupClientReplyValue(void *o);

char *getClientPeerId(client *client);

char *getClientSockName(client *client);

sds catClientInfoString(sds s, client *client);

sds getAllClientsInfoString(int type);

void rewriteClientCommandVector(client *c, int argc, ...);

void rewriteClientCommandArgument(client *c, int i, robj *newval);

void replaceClientCommandVector(client *c, int argc, robj **argv);

void redactClientCommandArgument(client *c, int argc);

size_t getClientOutputBufferMemoryUsage(client *c);

size_t getClientMemoryUsage(client *c, size_t *output_buffer_mem_usage);

int freeClientsInAsyncFreeQueue(void);

int closeClientOnOutputBufferLimitReached(client *c, int async);

int getClientType(client *c);

int getClientTypeByName(char *name);

char *getClientTypeName(int class);

void flushSlavesOutputBuffers(void);

void disconnectSlaves(void);

void evictClients(void);

int listenToPort(int port, socketFds *fds);

void pauseClients(pause_purpose purpose, mstime_t end, pause_type type);

void unpauseClients(pause_purpose purpose);

int areClientsPaused(void);

int checkClientPauseTimeoutAndReturnIfPaused(void);

void unblockPostponedClients();

void processEventsWhileBlocked(void);

void whileBlockedCron();

void blockingOperationStarts();

void blockingOperationEnds();

int handleClientsWithPendingWrites(void);

int handleClientsWithPendingWritesUsingThreads(void);

int handleClientsWithPendingReadsUsingThreads(void);

int stopThreadedIOIfNeeded(void);

int clientHasPendingReplies(client *c);

int islocalClient(client *c);

int updateClientMemUsage(client *c);

void updateClientMemUsageBucket(client *c);

void unlinkClient(client *c);

int writeToClient(client *c, int handler_installed);

void linkClient(client *c);

void protectClient(client *c);

void unprotectClient(client *c);

void initThreadedIO(void);

client *lookupClientByID(uint64_t id);

int authRequired(client *c);

void putClientInPendingWriteQueue(client *c);

#ifdef __GNUC__

void addReplyErrorFormatEx(client *c, int flags, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

void addReplyErrorFormat(client *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

void addReplyStatusFormat(client *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#else
void addReplyErrorFormatEx(client *c, int flags, const char *fmt, ...);
void addReplyErrorFormat(client *c, const char *fmt, ...);
void addReplyStatusFormat(client *c, const char *fmt, ...);
#endif

/* Client side caching (tracking mode) */
void enableTracking(client *c, uint64_t redirect_to, uint64_t options, robj **prefix, size_t numprefix);

void disableTracking(client *c);

void trackingRememberKeys(client *c);

void trackingInvalidateKey(client *c, robj *keyobj, int bcast);

void trackingScheduleKeyInvalidation(uint64_t client_id, robj *keyobj);

void trackingHandlePendingKeyInvalidations(void);

void trackingInvalidateKeysOnFlush(int async);

void freeTrackingRadixTree(rax *rt);

void freeTrackingRadixTreeAsync(rax *rt);

void trackingLimitUsedSlots(void);

uint64_t trackingGetTotalItems(void);

uint64_t trackingGetTotalKeys(void);

uint64_t trackingGetTotalPrefixes(void);

void trackingBroadcastInvalidationMessages(void);

int checkPrefixCollisionsOrReply(client *c, robj **prefix, size_t numprefix);

/* List data type */
void listTypePush(robj *subject, robj *value, int where);

robj *listTypePop(robj *subject, int where);

unsigned long listTypeLength(const robj *subject);

listTypeIterator *listTypeInitIterator(robj *subject, long index, unsigned char direction);

void listTypeReleaseIterator(listTypeIterator *li);

void listTypeSetIteratorDirection(listTypeIterator *li, unsigned char direction);

int listTypeNext(listTypeIterator *li, listTypeEntry *entry);

robj *listTypeGet(listTypeEntry *entry);

void listTypeInsert(listTypeEntry *entry, robj *value, int where);

void listTypeReplace(listTypeEntry *entry, robj *value);

int listTypeEqual(listTypeEntry *entry, robj *o);

void listTypeDelete(listTypeIterator *iter, listTypeEntry *entry);

robj *listTypeDup(robj *o);

int listTypeDelRange(robj *o, long start, long stop);

void unblockClientWaitingData(client *c);

void popGenericCommand(client *c, int where);

void listElementsRemoved(client *c, robj *key, int where, robj *o, long count, int *deleted);

/* MULTI/EXEC/WATCH... */
void unwatchAllKeys(client *c);

void initClientMultiState(client *c);

void freeClientMultiState(client *c);

void queueMultiCommand(client *c);

size_t multiStateMemOverhead(client *c);

void touchWatchedKey(redisDb *db, robj *key);

int isWatchedKeyExpired(client *c);

void touchAllWatchedKeysInDb(redisDb *emptied, redisDb *replaced_with);

void discardTransaction(client *c);

void flagTransaction(client *c);

void execCommandAbort(client *c, sds error);

/* Redis object implementation */
void decrRefCount(robj *o);

void decrRefCountVoid(void *o);

void incrRefCount(robj *o);

robj *makeObjectShared(robj *o);

robj *resetRefCount(robj *obj);

void freeStringObject(robj *o);

void freeListObject(robj *o);

void freeSetObject(robj *o);

void freeZsetObject(robj *o);

void freeHashObject(robj *o);

void dismissObject(robj *o, size_t dump_size);

robj *createObject(int type, void *ptr);

robj *createStringObject(const char *ptr, size_t len);

robj *createRawStringObject(const char *ptr, size_t len);

robj *createEmbeddedStringObject(const char *ptr, size_t len);

robj *tryCreateRawStringObject(const char *ptr, size_t len);

robj *tryCreateStringObject(const char *ptr, size_t len);

robj *dupStringObject(const robj *o);

int isSdsRepresentableAsLongLong(sds s, long long *llval);

int isObjectRepresentableAsLongLong(robj *o, long long *llongval);

robj *tryObjectEncoding(robj *o);

robj *getDecodedObject(robj *o);

size_t stringObjectLen(robj *o);

robj *createStringObjectFromLongLong(long long value);

robj *createStringObjectFromLongLongForValue(long long value);

robj *createStringObjectFromLongDouble(long double value, int humanfriendly);

robj *createQuicklistObject(void);

robj *createSetObject(void);

robj *createIntsetObject(void);

robj *createHashObject(void);

robj *createZsetObject(void);

robj *createZsetListpackObject(void);

robj *createStreamObject(void);

robj *createModuleObject(moduleType *mt, void *value);

int getLongFromObjectOrReply(client *c, robj *o, long *target, const char *msg);

int getPositiveLongFromObjectOrReply(client *c, robj *o, long *target, const char *msg);

int getRangeLongFromObjectOrReply(client *c, robj *o, long min, long max, long *target, const char *msg);

int checkType(client *c, robj *o, int type);

int getLongLongFromObjectOrReply(client *c, robj *o, long long *target, const char *msg);

int getDoubleFromObjectOrReply(client *c, robj *o, double *target, const char *msg);

int getDoubleFromObject(const robj *o, double *target);

int getLongLongFromObject(robj *o, long long *target);

int getLongDoubleFromObject(robj *o, long double *target);

int getLongDoubleFromObjectOrReply(client *c, robj *o, long double *target, const char *msg);

int getIntFromObjectOrReply(client *c, robj *o, int *target, const char *msg);

char *strEncoding(int encoding);

int compareStringObjects(robj *a, robj *b);

int collateStringObjects(robj *a, robj *b);

int equalStringObjects(robj *a, robj *b);

unsigned long long estimateObjectIdleTime(robj *o);

void trimStringObjectIfNeeded(robj *o);

#define sdsEncodedObject(objptr) (objptr->encoding == OBJ_ENCODING_RAW || objptr->encoding == OBJ_ENCODING_EMBSTR)

/* Synchronous I/O with timeout */
ssize_t syncWrite(int fd, char *ptr, ssize_t size, long long timeout);

ssize_t syncRead(int fd, char *ptr, ssize_t size, long long timeout);

ssize_t syncReadLine(int fd, char *ptr, ssize_t size, long long timeout);

/* Replication */
void replicationFeedSlaves(list *slaves, int dictid, robj **argv, int argc);

void replicationFeedStreamFromMasterStream(char *buf, size_t buflen);

void resetReplicationBuffer(void);

void feedReplicationBuffer(char *buf, size_t len);

void freeReplicaReferencedReplBuffer(client *replica);

void replicationFeedMonitors(client *c, list *monitors, int dictid, robj **argv, int argc);

void updateSlavesWaitingBgsave(int bgsaveerr, int type);

void replicationCron(void);

void replicationStartPendingFork(void);

void replicationHandleMasterDisconnection(void);

void replicationCacheMaster(client *c);

void resizeReplicationBacklog();

void replicationSetMaster(char *ip, int port);

void replicationUnsetMaster(void);

void refreshGoodSlavesCount(void);

int checkGoodReplicasStatus(void);

void processClientsWaitingReplicas(void);

void unblockClientWaitingReplicas(client *c);

int replicationCountAcksByOffset(long long offset);

void replicationSendNewlineToMaster(void);

long long replicationGetSlaveOffset(void);

char *replicationGetSlaveName(client *c);

long long getPsyncInitialOffset(void);

int replicationSetupSlaveForFullResync(client *slave, long long offset);

void changeReplicationId(void);

void clearReplicationId2(void);

void createReplicationBacklog(void);

void freeReplicationBacklog(void);

void replicationCacheMasterUsingMyself(void);

void feedReplicationBacklog(void *ptr, size_t len);

void incrementalTrimReplicationBacklog(size_t blocks);

int canFeedReplicaReplBuffer(client *replica);

void rebaseReplicationBuffer(long long base_repl_offset);

void showLatestBacklog(void);

void rdbPipeReadHandler(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);

void rdbPipeWriteHandlerConnRemoved(struct connection *conn);

void clearFailoverState(void);

void updateFailoverStatus(void);

void abortFailover(const char *err);

const char *getFailoverStateString();

/* Generic persistence functions */
void startLoadingFile(size_t size, char *filename, int rdbflags);

void startLoading(size_t size, int rdbflags, int async);

void loadingAbsProgress(off_t pos);

void loadingIncrProgress(off_t size);

void stopLoading(int success);

void updateLoadingFileName(char *filename);

void startSaving(int rdbflags);

void stopSaving(int success);

int allPersistenceDisabled(void);

#define DISK_ERROR_TYPE_AOF 1
#define DISK_ERROR_TYPE_RDB 2
#define DISK_ERROR_TYPE_NONE 0

int writeCommandsDeniedByDiskError(void);

sds writeCommandsGetDiskErrorMessage(int);

/* RDB persistence */
#include "rdb.h"

void killRDBChild(void);

int bg_unlink(const char *filename);

/* AOF persistence */
void flushAppendOnlyFile(int force);

void feedAppendOnlyFile(int dictid, robj **argv, int argc);

void aofRemoveTempFile(pid_t childpid);

int rewriteAppendOnlyFileBackground(void);

int loadAppendOnlyFiles(aofManifest *am);

void stopAppendOnly(void);

int startAppendOnly(void);

void backgroundRewriteDoneHandler(int exitcode, int bysignal);

ssize_t aofReadDiffFromParent(void);

void killAppendOnlyChild(void);

void restartAOFAfterSYNC();

void aofLoadManifestFromDisk(void);

void aofOpenIfNeededOnServerStart(void);

void aofManifestFree(aofManifest *am);

int aofDelHistoryFiles(void);

int aofRewriteLimited(void);

/* Child info */
void openChildInfoPipe(void);

void closeChildInfoPipe(void);

void sendChildInfoGeneric(childInfoType info_type, size_t keys, double progress, char *pname);

void sendChildCowInfo(childInfoType info_type, char *pname);

void sendChildInfo(childInfoType info_type, size_t keys, char *pname);

void receiveChildInfo(void);

/* Fork helpers */
int redisFork(int type);

int hasActiveChildProcess();

void resetChildState();

int isMutuallyExclusiveChildType(int type);

/* acl.c -- 认证相关属性 */
extern rax *Users;
extern user *DefaultUser;

void ACLInit(void);

// ACLCheckAllPerm() 函数返回值
#define ACL_OK 0
#define ACL_DENIED_CMD 1     // 命令被拒绝
#define ACL_DENIED_KEY 2     // key不允许操作
#define ACL_DENIED_AUTH 3    /* 仅用于ACL LOG表项. */
#define ACL_DENIED_CHANNEL 4 /* 仅用于发布/订阅命令 */

// acl日志项里的 上下文类型
#define ACL_LOG_CTX_TOPLEVEL 0
#define ACL_LOG_CTX_LUA 1
#define ACL_LOG_CTX_MULTI 2 // 事务
#define ACL_LOG_CTX_MODULE 3

/* ACL key permission types */
#define ACL_READ_PERMISSION (1 << 0)
#define ACL_WRITE_PERMISSION (1 << 1)
#define ACL_ALL_PERMISSION (ACL_READ_PERMISSION | ACL_WRITE_PERMISSION)

int ACLCheckUserCredentials(robj *username, robj *password);

int ACLAuthenticateUser(client *c, robj *username, robj *password);

unsigned long ACLGetCommandID(sds cmdname);

void ACLClearCommandID(void);

user *ACLGetUserByName(const char *name, size_t namelen);

int ACLUserCheckKeyPerm(user *u, const char *key, int keylen, int flags);

int ACLUserCheckChannelPerm(user *u, sds channel, int literal);

int ACLCheckAllUserCommandPerm(user *u, struct redisCommand *cmd, robj **argv, int argc, int *idxptr);

int ACLUserCheckCmdWithUnrestrictedKeyAccess(user *u, struct redisCommand *cmd, robj **argv, int argc, int flags);

int ACLCheckAllPerm(client *c, int *idxptr);

int ACLSetUser(user *u, const char *op, ssize_t oplen);

uint64_t ACLGetCommandCategoryFlagByName(const char *name);

int ACLAppendUserForLoading(sds *argv, int argc, int *argc_err);

const char *ACLSetUserStringError(void);

int ACLLoadConfiguredUsers(void);

sds ACLDescribeUser(user *u);

void ACLLoadUsersAtStartup(void);

void addReplyCommandCategories(client *c, struct redisCommand *cmd);

user *ACLCreateUnlinkedUser();

void ACLFreeUserAndKillClients(user *u);

void addACLLogEntry(client *c, int reason, int context, int argpos, sds username, sds object);

const char *getAclErrorMessage(int acl_res);

void ACLUpdateDefaultUserPassword(sds password);

/* Sorted sets data type */

/* Input flags. */
#define ZADD_IN_NONE 0
#define ZADD_IN_INCR (1 << 0) /* Increment the score instead of setting it. */
#define ZADD_IN_NX (1 << 1)   /* Don't touch elements not already existing. */
#define ZADD_IN_XX (1 << 2)   /* Only touch elements already existing. */
#define ZADD_IN_GT (1 << 3)   /* Only update existing when new scores are higher. */
#define ZADD_IN_LT (1 << 4)   /* Only update existing when new scores are lower. */

/* Output flags. */
#define ZADD_OUT_NOP (1 << 0)     /* Operation not performed because of conditionals.*/
#define ZADD_OUT_NAN (1 << 1)     /* Only touch elements already existing. */
#define ZADD_OUT_ADDED (1 << 2)   /* The element was new and was added. */
#define ZADD_OUT_UPDATED (1 << 3) /* The element already existed, score updated. */

// 表示开区间/闭区间范围的结构
typedef struct {
    double min, max;  // 最小值和最大值
    int minex, maxex; // 指示最小值和最大值是否*不*包含在范围之内
    // 值为 1 表示不包含,值为 0 表示包含
} zrangespec;

/* Struct to hold an inclusive/exclusive range spec by lexicographic comparison. */
typedef struct {
    sds min, max;     /* May be set to shared.(minstring|maxstring) */
    int minex, maxex; /* are min or max exclusive? */
} zlexrangespec;

/* flags for incrCommandFailedCalls */
#define ERROR_COMMAND_REJECTED (1 << 0) /* Indicate to update the command rejected stats */
#define ERROR_COMMAND_FAILED (1 << 1)   /* Indicate to update the command failed stats */

zskiplist *zslCreate(void); // 创建一个新的跳跃表

void zslFree(zskiplist *zsl); // 释放给定跳跃表,以及表中的所有节点
// 创建一个成员,并插入
zskiplistNode *zslInsert(zskiplist *zsl, double score, sds ele);
// 删除跳跃表中包含给定成员和分支的节点

int zslDelete(zskiplist *zsl, double score, sds ele, zskiplistNode **node);
// 给定一个分值范围,返回跳跃表中第一个符合这个范围的节点 ,从high level 开始

zskiplistNode *zslFirstInRange(zskiplist *zsl, zrangespec *range);
// 给定一个分值范围,返回跳跃表中最后一个符合这个范围的节点 ,从high level 开始

zskiplistNode *zslLastInRange(zskiplist *zsl, zrangespec *range);
// 返回包含给定成员和分值的节点在跳跃表中的排位

unsigned long zslGetRank(zskiplist *zsl, double score, sds o);

unsigned char *zzlInsert(unsigned char *zl, sds ele, double score);

double zzlGetScore(unsigned char *sptr);

void zzlNext(unsigned char *zl, unsigned char **eptr, unsigned char **sptr);

void zzlPrev(unsigned char *zl, unsigned char **eptr, unsigned char **sptr);

unsigned char *zzlFirstInRange(unsigned char *zl, zrangespec *range);

unsigned char *zzlLastInRange(unsigned char *zl, zrangespec *range);

unsigned long zsetLength(const robj *zobj);

void zsetConvert(robj *zobj, int encoding);

void zsetConvertToListpackIfNeeded(robj *zobj, size_t maxelelen, size_t totelelen);

int zsetScore(robj *zobj, sds member, double *score);

unsigned long zslGetRank(zskiplist *zsl, double score, sds o);

int zsetAdd(robj *zobj, double score, sds ele, int in_flags, int *out_flags, double *newscore);

long zsetRank(robj *zobj, sds ele, int reverse);

int zsetDel(robj *zobj, sds ele);

robj *zsetDup(robj *o);

void genericZpopCommand(client *c, robj **keyv, int keyc, int where, int emitkey, long count, int use_nested_array, int reply_nil_when_empty, int *deleted);

sds lpGetObject(unsigned char *sptr);

int zslValueGteMin(double value, zrangespec *spec);

int zslValueLteMax(double value, zrangespec *spec);

void zslFreeLexRange(zlexrangespec *spec);

int zslParseLexRange(robj *min, robj *max, zlexrangespec *spec);

unsigned char *zzlFirstInLexRange(unsigned char *zl, zlexrangespec *range);

unsigned char *zzlLastInLexRange(unsigned char *zl, zlexrangespec *range);

zskiplistNode *zslFirstInLexRange(zskiplist *zsl, zlexrangespec *range);

zskiplistNode *zslLastInLexRange(zskiplist *zsl, zlexrangespec *range);

int zzlLexValueGteMin(unsigned char *p, zlexrangespec *spec);

int zzlLexValueLteMax(unsigned char *p, zlexrangespec *spec);

int zslLexValueGteMin(sds value, zlexrangespec *spec);

int zslLexValueLteMax(sds value, zlexrangespec *spec);

/* Core functions */
int getMaxmemoryState(size_t *total, size_t *logical, size_t *tofree, float *level);

size_t freeMemoryGetNotCountedMemory();

int overMaxmemoryAfterAlloc(size_t moremem);

int processCommand(client *c);

int processPendingCommandAndInputBuffer(client *c);

void setupSignalHandlers(void);

void removeSignalHandlers(void);

int createSocketAcceptHandler(socketFds *sfd, aeFileProc *accept_handler);

int changeListenPort(int port, socketFds *sfd, aeFileProc *accept_handler);

int changeBindAddr(void);

struct redisCommand *lookupSubcommand(struct redisCommand *container, sds sub_name);

struct redisCommand *lookupCommand(robj **argv, int argc);

struct redisCommand *lookupCommandBySdsLogic(dict *commands, sds s);

struct redisCommand *lookupCommandBySds(sds s);

struct redisCommand *lookupCommandByCStringLogic(dict *commands, const char *s);

struct redisCommand *lookupCommandByCString(const char *s);

struct redisCommand *lookupCommandOrOriginal(robj **argv, int argc);

int commandCheckExistence(client *c, sds *err);

int commandCheckArity(client *c, sds *err);

void startCommandExecution();

int incrCommandStatsOnError(struct redisCommand *cmd, int flags);

void call(client *c, int flags);

void alsoPropagate(int dbid, robj **argv, int argc, int target);

void propagatePendingCommands();

void redisOpArrayInit(redisOpArray *oa);

void redisOpArrayFree(redisOpArray *oa);

void forceCommandPropagation(client *c, int flags);

void preventCommandPropagation(client *c);

void preventCommandAOF(client *c);

void preventCommandReplication(client *c);

void slowlogPushCurrentCommand(client *c, struct redisCommand *cmd, ustime_t duration);

void updateCommandLatencyHistogram(struct hdr_histogram **latency_histogram, int64_t duration_hist);

int prepareForShutdown(int flags);

void replyToClientsBlockedOnShutdown(void);

int abortShutdown(void);

void afterCommand(client *c);

int mustObeyClient(client *c);

#ifdef __GNUC__

void _serverLog(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#else
void _serverLog(int level, const char *fmt, ...);
#endif

void serverLogRaw(int level, const char *msg);

void serverLogFromHandler(int level, const char *msg);

void usage(void);

void updateDictResizePolicy(void);

int htNeedsResize(dict *dict);

void populateCommandTable(void);

void resetCommandTableStats(dict *commands);

void resetErrorTableStats(void);

void adjustOpenFilesLimit(void);

void incrementErrorCount(const char *fullerr, size_t namelen);

void closeListeningSockets(int unlink_unix_socket);

void updateCachedTime(int update_daylight_info);

void resetServerStats(void);

void activeDefragCycle(void);

unsigned int getLRUClock(void);

unsigned int LRU_CLOCK(void);

const char *evictPolicyToString(void);

struct redisMemOverhead *getMemoryOverheadData(void);

void freeMemoryOverheadData(struct redisMemOverhead *mh);

void checkChildrenDone(void);

int setOOMScoreAdj(int process_class);

void rejectCommandFormat(client *c, const char *fmt, ...);

void *activeDefragAlloc(void *ptr);

robj *activeDefragStringOb(robj *ob, long *defragged);

void dismissSds(sds s);

void dismissMemory(void *ptr, size_t size_hint);

void dismissMemoryInChild(void);

#define RESTART_SERVER_NONE 0
#define RESTART_SERVER_GRACEFULLY (1 << 0)     /* Do proper shutdown. */
#define RESTART_SERVER_CONFIG_REWRITE (1 << 1) /* CONFIG REWRITE before restart.*/

int restartServer(int flags, mstime_t delay);

/* Set data type */
robj *setTypeCreate(sds value);

int setTypeAdd(robj *subject, sds value);

int setTypeRemove(robj *subject, sds value);

int setTypeIsMember(robj *subject, sds value);

setTypeIterator *setTypeInitIterator(robj *subject);

void setTypeReleaseIterator(setTypeIterator *si);

int setTypeNext(setTypeIterator *si, sds *sdsele, int64_t *llele);

sds setTypeNextObject(setTypeIterator *si);

int setTypeRandomElement(robj *setobj, sds *sdsele, int64_t *llele);

unsigned long setTypeRandomElements(robj *set, unsigned long count, robj *aux_set);

unsigned long setTypeSize(const robj *subject);

void setTypeConvert(robj *subject, int enc);

robj *setTypeDup(robj *o);

/* Hash data type */
#define HASH_SET_TAKE_FIELD (1 << 0)
#define HASH_SET_TAKE_VALUE (1 << 1)
#define HASH_SET_COPY 0

void hashTypeConvert(robj *o, int enc);

void hashTypeTryConversion(robj *subject, robj **argv, int start, int end);

int hashTypeExists(robj *o, sds key);

int hashTypeDelete(robj *o, sds key);

unsigned long hashTypeLength(const robj *o);

hashTypeIterator *hashTypeInitIterator(robj *subject);

void hashTypeReleaseIterator(hashTypeIterator *hi);

int hashTypeNext(hashTypeIterator *hi);

void hashTypeCurrentFromListpack(hashTypeIterator *hi, int what, unsigned char **vstr, unsigned int *vlen, long long *vll);

sds hashTypeCurrentFromHashTable(hashTypeIterator *hi, int what);

void hashTypeCurrentObject(hashTypeIterator *hi, int what, unsigned char **vstr, unsigned int *vlen, long long *vll);

sds hashTypeCurrentObjectNewSds(hashTypeIterator *hi, int what);

robj *hashTypeLookupWriteOrCreate(client *c, robj *key);

robj *hashTypeGetValueObject(robj *o, sds field);

int hashTypeSet(robj *o, sds field, sds value, int flags);

robj *hashTypeDup(robj *o);

/* Pub / Sub */
int pubsubUnsubscribeAllChannels(client *c, int notify);

int pubsubUnsubscribeShardAllChannels(client *c, int notify);

void pubsubUnsubscribeShardChannels(robj **channels, unsigned int count);

int pubsubUnsubscribeAllPatterns(client *c, int notify);

int pubsubPublishMessage(robj *channel, robj *message, int sharded);

int pubsubPublishMessageAndPropagateToCluster(robj *channel, robj *message, int sharded);

void addReplyPubsubMessage(client *c, robj *channel, robj *msg);

int serverPubsubSubscriptionCount();

int serverPubsubShardSubscriptionCount();

/* Keyspace events notification */
void notifyKeyspaceEvent(int type, char *event, robj *key, int dbid);

int keyspaceEventsStringToFlags(char *classes);

sds keyspaceEventsFlagsToString(int flags);

// 配置标志位
#define MODIFIABLE_CONFIG 0             /* This is the implied default for a standard config, which is mutable. */
#define IMMUTABLE_CONFIG (1ULL << 0)    /* Can this value only be set at startup? */
#define SENSITIVE_CONFIG (1ULL << 1)    /* Does this value contain sensitive information */
#define DEBUG_CONFIG (1ULL << 2)        /* Values that are useful for debugging. */
#define MULTI_ARG_CONFIG (1ULL << 3)    /* This config receives multiple arguments. */
#define HIDDEN_CONFIG (1ULL << 4)       /* This config is hidden in `config get <pattern>` (used for tests/debugging) */
#define PROTECTED_CONFIG (1ULL << 5)    /* Becomes immutable if enable-protected-configs is enabled. */
#define DENY_LOADING_CONFIG (1ULL << 6) /* This config is forbidden during loading. */
#define ALIAS_CONFIG (1ULL << 7)        // 对于有多个名字的配置,这个标志被设置在别名上.
#define MODULE_CONFIG (1ULL << 8)       /* This config is a module config */

// 不同配置的类型
#define INTEGER_CONFIG 0        // 整数
#define MEMORY_CONFIG (1 << 0)  // 内存值
#define PERCENT_CONFIG (1 << 1) // 百分比
#define OCTAL_CONFIG (1 << 2)   // 8进制

/* Enum Configs contain an array of configEnum objects that match a string with an integer. */
typedef struct configEnum {
    char *name;
    int val;
} configEnum;

/* Type of configuration. */
typedef enum {
    BOOL_CONFIG,
    NUMERIC_CONFIG,
    STRING_CONFIG,
    SDS_CONFIG,
    ENUM_CONFIG,
    SPECIAL_CONFIG,
} configType;

void loadServerConfig(char *filename, char config_from_stdin, char *options);

void appendServerSaveParams(time_t seconds, int changes);

void resetServerSaveParams(void);

struct rewriteConfigState; /* Forward declaration to export API. */
void rewriteConfigRewriteLine(struct rewriteConfigState *state, const char *option, sds line, int force);

void rewriteConfigMarkAsProcessed(struct rewriteConfigState *state, const char *option);

int rewriteConfig(char *path, int force_write);

void initConfigValues();

void removeConfig(sds name);

sds getConfigDebugInfo();

int allowProtectedAction(int config, client *c);

/* Module Configuration */
typedef struct ModuleConfig ModuleConfig;

int performModuleConfigSetFromName(sds name, sds value, const char **err);

int performModuleConfigSetDefaultFromName(sds name, const char **err);

void addModuleBoolConfig(const char *module_name, const char *name, int flags, void *privdata, int default_val);

void addModuleStringConfig(const char *module_name, const char *name, int flags, void *privdata, sds default_val);

void addModuleEnumConfig(const char *module_name, const char *name, int flags, void *privdata, int default_val, configEnum *enum_vals);

void addModuleNumericConfig(const char *module_name, const char *name, int flags, void *privdata, long long default_val, int conf_flags, long long lower, long long upper);

void addModuleConfigApply(list *module_configs, ModuleConfig *module_config);

int moduleConfigApplyConfig(list *module_configs, const char **err, const char **err_arg_name);

int getModuleBoolConfig(ModuleConfig *module_config);

int setModuleBoolConfig(ModuleConfig *config, int val, const char **err);

sds getModuleStringConfig(ModuleConfig *module_config);

int setModuleStringConfig(ModuleConfig *config, sds strval, const char **err);

int getModuleEnumConfig(ModuleConfig *module_config);

int setModuleEnumConfig(ModuleConfig *config, int val, const char **err);

long long getModuleNumericConfig(ModuleConfig *module_config);

int setModuleNumericConfig(ModuleConfig *config, long long val, const char **err);

/* db.c -- Keyspace access API */
int removeExpire(redisDb *db, robj *key);

void deleteExpiredKeyAndPropagate(redisDb *db, robj *keyobj);

void propagateDeletion(redisDb *db, robj *key, int lazy);

int keyIsExpired(redisDb *db, robj *key);

long long getExpire(redisDb *db, robj *key);

void setExpire(client *c, redisDb *db, robj *key, long long when);

int checkAlreadyExpired(long long when);

robj *lookupKeyRead(redisDb *db, robj *key);

robj *lookupKeyWrite(redisDb *db, robj *key);

robj *lookupKeyReadOrReply(client *c, robj *key, robj *reply);

robj *lookupKeyWriteOrReply(client *c, robj *key, robj *reply);

robj *lookupKeyReadWithFlags(redisDb *db, robj *key, int flags);

robj *lookupKeyWriteWithFlags(redisDb *db, robj *key, int flags);

robj *objectCommandLookup(client *c, robj *key);

robj *objectCommandLookupOrReply(client *c, robj *key, robj *reply);

int objectSetLRUOrLFU(robj *val, long long lfu_freq, long long lru_idle, long long lru_clock, int lru_multiplier);

#define LOOKUP_NONE 0            // 没有传递特殊的 flag .
#define LOOKUP_NOTOUCH (1 << 0)  // 不要更改 key 的LRU
#define LOOKUP_NONOTIFY (1 << 1) // 不要在key miss时 触发 keyspace事件
#define LOOKUP_NOSTATS (1 << 2)  // 不增加 key hits/misses 计数.
#define LOOKUP_WRITE (1 << 3)    // 删除过期key 即是是slave

void dbAdd(redisDb *db, robj *key, robj *val);

int dbAddRDBLoad(redisDb *db, sds key, robj *val);

void dbOverwrite(redisDb *db, robj *key, robj *val);

#define SETKEY_KEEPTTL 1
#define SETKEY_NO_SIGNAL 2
#define SETKEY_ALREADY_EXIST 4
#define SETKEY_DOESNT_EXIST 8

void setKey(client *c, redisDb *db, robj *key, robj *val, int flags);

robj *dbRandomKey(redisDb *db);

int dbSyncDelete(redisDb *db, robj *key);

int dbDelete(redisDb *db, robj *key);

robj *dbUnshareStringValue(redisDb *db, robj *key, robj *o);

#define EMPTYDB_NO_FLAGS 0           /* No flags. */
#define EMPTYDB_ASYNC (1 << 0)       /* Reclaim memory in another thread. */
#define EMPTYDB_NOFUNCTIONS (1 << 1) /* Indicate not to flush the functions. */

long long emptyData(int dbnum, int flags, void(callback)(dict *));

long long emptyDbStructure(redisDb *dbarray, int dbnum, int async, void(callback)(dict *));

void flushAllDataAndResetRDB(int flags);

long long dbTotalServerKeyCount();

redisDb *initTempDb(void);

void discardTempDb(redisDb *tempDb, void(callback)(dict *));

int selectDb(client *c, int id);

void signalModifiedKey(client *c, redisDb *db, robj *key);

void signalFlushedDb(int dbid, int async);

void scanGenericCommand(client *c, robj *o, unsigned long cursor);

int parseScanCursorOrReply(client *c, robj *o, unsigned long *cursor);

int dbAsyncDelete(redisDb *db, robj *key);

void emptyDbAsync(redisDb *db);

size_t lazyfreeGetPendingObjectsCount(void);

size_t lazyfreeGetFreedObjectsCount(void);

void lazyfreeResetStats(void);

void freeObjAsync(robj *key, robj *obj, int dbid);

void freeReplicationBacklogRefMemAsync(list *blocks, rax *index);

/* API to get key arguments from commands */
#define GET_KEYSPEC_DEFAULT 0
#define GET_KEYSPEC_INCLUDE_NOT_KEYS (1 << 0) /* Consider 'fake' keys as keys */
#define GET_KEYSPEC_RETURN_PARTIAL (1 << 1)   /* Return all keys that can be found */

int getKeysFromCommandWithSpecs(struct redisCommand *cmd, robj **argv, int argc, int search_flags, getKeysResult *result);

keyReference *getKeysPrepareResult(getKeysResult *result, int numkeys);

int getKeysFromCommand(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

int doesCommandHaveKeys(struct redisCommand *cmd);

int getChannelsFromCommand(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

int doesCommandHaveChannelsWithFlags(struct redisCommand *cmd, int flags);

void getKeysFreeResult(getKeysResult *result);

int sintercardGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

int zunionInterDiffGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

int zunionInterDiffStoreGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

int evalGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

int functionGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

int sortGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

int sortROGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

int migrateGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

int georadiusGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

int xreadGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

int lmpopGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

int blmpopGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

int zmpopGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

int bzmpopGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

int setGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

int bitfieldGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

unsigned short crc16(const char *buf, int len);

/* Sentinel */
void initSentinelConfig(void);

void initSentinel(void);

void sentinelTimer(void);

const char *sentinelHandleConfiguration(char **argv, int argc);

void queueSentinelConfig(sds *argv, int argc, int linenum, sds line);

void loadSentinelConfigFromQueue(void);

void sentinelIsRunning(void);

void sentinelCheckConfigFile(void);

void sentinelCommand(client *c);

void sentinelInfoCommand(client *c);

void sentinelPublishCommand(client *c);

void sentinelRoleCommand(client *c);

/* redis-check-rdb & aof */
int redis_check_rdb(char *rdbfilename, FILE *fp);

int redis_check_rdb_main(int argc, char **argv, FILE *fp);

int redis_check_aof_main(int argc, char **argv);

/* Scripting */
void scriptingInit(int setup);

int ldbRemoveChild(pid_t pid);

void ldbKillForkedSessions(void);

int ldbPendingChildren(void);

sds luaCreateFunction(client *c, robj *body);

void luaLdbLineHook(lua_State *lua, lua_Debug *ar);

void freeLuaScriptsAsync(dict *lua_scripts);

void freeFunctionsAsync(functionsLibCtx *lib_ctx);

int ldbIsEnabled();

void ldbLog(sds entry);

void ldbLogRedisReply(char *reply);

void sha1hex(char *digest, char *script, size_t len);

unsigned long evalMemory();

dict *evalScriptsDict();

unsigned long evalScriptsMemory();

typedef struct luaScript {
    uint64_t flags;
    robj *body;
} luaScript;

/* Blocked clients */
void processUnblockedClients(void);

void blockClient(client *c, int btype);

void unblockClient(client *c);

void queueClientForReprocessing(client *c);

void replyToBlockedClientTimedOut(client *c);

int getTimeoutFromObjectOrReply(client *c, robj *object, mstime_t *timeout, int unit);

void disconnectAllBlockedClients(void);

void handleClientsBlockedOnKeys(void);

void signalKeyAsReady(redisDb *db, robj *key, int type);

void blockForKeys(client *c, int btype, robj **keys, int numkeys, long count, mstime_t timeout, robj *target, struct blockPos *blockpos, streamID *ids);

void updateStatsOnUnblock(client *c, long blocked_us, long reply_us, int had_errors);

void scanDatabaseForDeletedStreams(redisDb *emptied, redisDb *replaced_with);

/* timeout.c -- Blocked clients timeout and connections timeout. */
void addClientToTimeoutTable(client *c);

void removeClientFromTimeoutTable(client *c);

void handleBlockedClientsTimeout(void);

int clientsCronHandleTimeout(client *c, mstime_t now_ms);

/* expire.c -- Handling of expired keys */
// 过期键的定期删除
void activeExpireCycle(int type);

void expireSlaveKeys(void);

void rememberSlaveKeyWithExpire(redisDb *db, robj *key);

void flushSlaveKeysWithExpireList(void);

size_t getSlaveKeyWithExpireCount(void);

/* evict.c -- maxmemory handling and LRU eviction. */
void evictionPoolAlloc(void);

#define LFU_INIT_VAL 5

unsigned long LFUGetTimeInMinutes(void);

uint8_t LFULogIncr(uint8_t value);

unsigned long LFUDecrAndReturn(robj *o);

#define EVICT_OK 0
#define EVICT_RUNNING 1
#define EVICT_FAIL 2

int performEvictions(void);

void startEvictionTimeProc(void);

/* Keys hashing / comparison functions for dict.c hash tables. */
uint64_t dictSdsHash(const void *key);

uint64_t dictSdsCaseHash(const void *key);

int dictSdsKeyCompare(dict *d, const void *key1, const void *key2);

int dictSdsKeyCaseCompare(dict *d, const void *key1, const void *key2);

void dictSdsDestructor(dict *d, void *val);

void *dictSdsDup(dict *d, const void *key);

/* Git SHA1 */
char *redisGitSHA1(void);

char *redisGitDirty(void);

uint64_t redisBuildId(void);

char *redisBuildIdString(void);

/* Commands prototypes */
void authCommand(client *c);

void pingCommand(client *c);

void echoCommand(client *c);

void commandCommand(client *c);

void commandCountCommand(client *c);

void commandListCommand(client *c);

void commandInfoCommand(client *c);

void commandGetKeysCommand(client *c);

void commandGetKeysAndFlagsCommand(client *c);

void commandHelpCommand(client *c);

void commandDocsCommand(client *c);

void setCommand(client *c);

void setnxCommand(client *c);

void setexCommand(client *c);

void psetexCommand(client *c);

void getCommand(client *c);

void getexCommand(client *c);

void getdelCommand(client *c);

void delCommand(client *c);

void unlinkCommand(client *c);

void existsCommand(client *c);

void setbitCommand(client *c);

void getbitCommand(client *c);

void bitfieldCommand(client *c);

void bitfieldroCommand(client *c);

void setrangeCommand(client *c);

void getrangeCommand(client *c);

void incrCommand(client *c);

void decrCommand(client *c);

void incrbyCommand(client *c);

void decrbyCommand(client *c);

void incrbyfloatCommand(client *c);

void selectCommand(client *c);

void swapdbCommand(client *c);

void randomkeyCommand(client *c);

void keysCommand(client *c);

void scanCommand(client *c);

void dbsizeCommand(client *c);

void lastsaveCommand(client *c);

void saveCommand(client *c);

void bgsaveCommand(client *c);

void bgrewriteaofCommand(client *c);

void shutdownCommand(client *c);

void slowlogCommand(client *c);

void moveCommand(client *c);

void copyCommand(client *c);

void renameCommand(client *c);

void renamenxCommand(client *c);

void lpushCommand(client *c);

void rpushCommand(client *c);

void lpushxCommand(client *c);

void rpushxCommand(client *c);

void linsertCommand(client *c);

void lpopCommand(client *c);

void rpopCommand(client *c);

void lmpopCommand(client *c);

void llenCommand(client *c);

void lindexCommand(client *c);

void lrangeCommand(client *c);

void ltrimCommand(client *c);

void typeCommand(client *c);

void lsetCommand(client *c);

void saddCommand(client *c);

void sremCommand(client *c);

void smoveCommand(client *c);

void sismemberCommand(client *c);

void smismemberCommand(client *c);

void scardCommand(client *c);

void spopCommand(client *c);

void srandmemberCommand(client *c);

void sinterCommand(client *c);

void sinterCardCommand(client *c);

void sinterstoreCommand(client *c);

void sunionCommand(client *c);

void sunionstoreCommand(client *c);

void sdiffCommand(client *c);

void sdiffstoreCommand(client *c);

void sscanCommand(client *c);

void syncCommand(client *c);

void flushdbCommand(client *c);

void flushallCommand(client *c);

void sortCommand(client *c);

void sortroCommand(client *c);

void lremCommand(client *c);

void lposCommand(client *c);

void rpoplpushCommand(client *c);

void lmoveCommand(client *c);

void infoCommand(client *c);

void mgetCommand(client *c);

void monitorCommand(client *c);

void expireCommand(client *c);

void expireatCommand(client *c);

void pexpireCommand(client *c);

void pexpireatCommand(client *c);

void getsetCommand(client *c);

void ttlCommand(client *c);

void touchCommand(client *c);

void pttlCommand(client *c);

void expiretimeCommand(client *c);

void pexpiretimeCommand(client *c);

void persistCommand(client *c);

void replicaofCommand(client *c);

void roleCommand(client *c);

void debugCommand(client *c);

void msetCommand(client *c);

void msetnxCommand(client *c);

void zaddCommand(client *c);

void zincrbyCommand(client *c);

void zrangeCommand(client *c);

void zrangebyscoreCommand(client *c);

void zrevrangebyscoreCommand(client *c);

void zrangebylexCommand(client *c);

void zrevrangebylexCommand(client *c);

void zcountCommand(client *c);

void zlexcountCommand(client *c);

void zrevrangeCommand(client *c);

void zcardCommand(client *c);

void zremCommand(client *c);

void zscoreCommand(client *c);

void zmscoreCommand(client *c);

void zremrangebyscoreCommand(client *c);

void zremrangebylexCommand(client *c);

void zpopminCommand(client *c);

void zpopmaxCommand(client *c);

void zmpopCommand(client *c);

void bzpopminCommand(client *c);

void bzpopmaxCommand(client *c);

void bzmpopCommand(client *c);

void zrandmemberCommand(client *c);

void multiCommand(client *c);

void execCommand(client *c);

void discardCommand(client *c);

void blpopCommand(client *c);

void brpopCommand(client *c);

void blmpopCommand(client *c);

void brpoplpushCommand(client *c);

void blmoveCommand(client *c);

void appendCommand(client *c);

void strlenCommand(client *c);

void zrankCommand(client *c);

void zrevrankCommand(client *c);

void hsetCommand(client *c);

void hsetnxCommand(client *c);

void hgetCommand(client *c);

void hmgetCommand(client *c);

void hdelCommand(client *c);

void hlenCommand(client *c);

void hstrlenCommand(client *c);

void zremrangebyrankCommand(client *c);

void zunionstoreCommand(client *c);

void zinterstoreCommand(client *c);

void zdiffstoreCommand(client *c);

void zunionCommand(client *c);

void zinterCommand(client *c);

void zinterCardCommand(client *c);

void zrangestoreCommand(client *c);

void zdiffCommand(client *c);

void zscanCommand(client *c);

void hkeysCommand(client *c);

void hvalsCommand(client *c);

void hgetallCommand(client *c);

void hexistsCommand(client *c);

void hscanCommand(client *c);

void hrandfieldCommand(client *c);

void configSetCommand(client *c);

void configGetCommand(client *c);

void configResetStatCommand(client *c);

void configRewriteCommand(client *c);

void configHelpCommand(client *c);

void hincrbyCommand(client *c);

void hincrbyfloatCommand(client *c);

void subscribeCommand(client *c);

void unsubscribeCommand(client *c);

void psubscribeCommand(client *c);

void punsubscribeCommand(client *c);

void publishCommand(client *c);

void pubsubCommand(client *c);

void spublishCommand(client *c);

void ssubscribeCommand(client *c);

void sunsubscribeCommand(client *c);

void watchCommand(client *c);

void unwatchCommand(client *c);

void clusterCommand(client *c);

void restoreCommand(client *c);

void migrateCommand(client *c);

void askingCommand(client *c);

void readonlyCommand(client *c);

void readwriteCommand(client *c);

int verifyDumpPayload(unsigned char *p, size_t len, uint16_t *rdbver_ptr);

void dumpCommand(client *c);

void objectCommand(client *c);

void memoryCommand(client *c);

void clientCommand(client *c);

void helloCommand(client *c);

void evalCommand(client *c);

void evalRoCommand(client *c);

void evalShaCommand(client *c);

void evalShaRoCommand(client *c);

void scriptCommand(client *c);

void fcallCommand(client *c);

void fcallroCommand(client *c);

void functionLoadCommand(client *c);

void functionDeleteCommand(client *c);

void functionKillCommand(client *c);

void functionStatsCommand(client *c);

void functionListCommand(client *c);

void functionHelpCommand(client *c);

void functionFlushCommand(client *c);

void functionRestoreCommand(client *c);

void functionDumpCommand(client *c);

void timeCommand(client *c);

void bitopCommand(client *c);

void bitcountCommand(client *c);

void bitposCommand(client *c);

void replconfCommand(client *c);

void waitCommand(client *c);

void georadiusbymemberCommand(client *c);

void georadiusbymemberroCommand(client *c);

void georadiusCommand(client *c);

void georadiusroCommand(client *c);

void geoaddCommand(client *c);

void geohashCommand(client *c);

void geoposCommand(client *c);

void geodistCommand(client *c);

void geosearchCommand(client *c);

void geosearchstoreCommand(client *c);

void pfselftestCommand(client *c);

void pfaddCommand(client *c);

void pfcountCommand(client *c);

void pfmergeCommand(client *c);

void pfdebugCommand(client *c);

void latencyCommand(client *c);

void moduleCommand(client *c);

void securityWarningCommand(client *c);

void xaddCommand(client *c);

void xrangeCommand(client *c);

void xrevrangeCommand(client *c);

void xlenCommand(client *c);

void xreadCommand(client *c);

void xgroupCommand(client *c);

void xsetidCommand(client *c);

void xackCommand(client *c);

void xpendingCommand(client *c);

void xclaimCommand(client *c);

void xautoclaimCommand(client *c);

void xinfoCommand(client *c);

void xdelCommand(client *c);

void xtrimCommand(client *c);

void lolwutCommand(client *c);

void aclCommand(client *c);

void lcsCommand(client *c);

void quitCommand(client *c);

void resetCommand(client *c);

void failoverCommand(client *c);

#if defined(__GNUC__)

void *calloc(size_t count, size_t size) __attribute__((deprecated));

void free(void *ptr) __attribute__((deprecated));

void *malloc(size_t size) __attribute__((deprecated));

void *realloc(void *ptr, size_t size) __attribute__((deprecated));

#endif

/* Debugging stuff */
void _serverAssertWithInfo(const client *c, const robj *o, const char *estr, const char *file, int line);

void _serverAssert(const char *estr, const char *file, int line);

#ifdef __GNUC__

void _serverPanic(const char *file, int line, const char *msg, ...) __attribute__((format(printf, 3, 4)));

#else
void _serverPanic(const char *file, int line, const char *msg, ...);
#endif

void serverLogObjectDebugInfo(const robj *o);

void sigsegvHandler(int sig, siginfo_t *info, void *secret);

const char *getSafeInfoString(const char *s, size_t len, char **tmp);

dict *genInfoSectionDict(robj **argv, int argc, char **defaults, int *out_all, int *out_everything);

void releaseInfoSectionDict(dict *sec);

sds genRedisInfoString(dict *section_dict, int all_sections, int everything);

sds genModulesInfoString(sds info);

void applyWatchdogPeriod();

void watchdogScheduleSignal(int period);

void serverLogHexDump(int level, char *descr, void *value, size_t len);

int memtest_preserving_test(unsigned long *m, size_t bytes, int passes);

void mixDigest(unsigned char *digest, const void *ptr, size_t len);

void xorDigest(unsigned char *digest, const void *ptr, size_t len);

sds catSubCommandFullname(const char *parent_name, const char *sub_name);

void commandAddSubcommand(struct redisCommand *parent, struct redisCommand *subcommand, const char *declared_name);

void populateCommandMovableKeys(struct redisCommand *cmd);

void debugDelay(int usec);

void killIOThreads(void);

void killThreads(void);

void makeThreadKillable(void);

void swapMainDbWithTempDb(redisDb *tempDb);

// 像serverLogRaw()一样,但支持printf-alike.这是在整个代码中使用的函数.serverLogRaw只用于在崩溃时转储INFO输出.
#define serverLog(level, ...)                  \
    do {                                       \
        if (((level)&0xff) < server.verbosity) \
            break;                             \
        _serverLog(level, __VA_ARGS__);        \
    } while (0)

/* TLS stuff */
void tlsInit(void);

void tlsCleanup(void);

int tlsConfigure(redisTLSContextConfig *ctx_config);

int isTlsConfigured(void);

#define redisDebug(fmt, ...) printf("DEBUG %s:%d > " fmt "\n", __FILE__, __LINE__, __VA_ARGS__)
#define redisDebugMark() printf("-- MARK %s:%d --\n", __FILE__, __LINE__)

int iAmMaster(void);

#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)

#endif
