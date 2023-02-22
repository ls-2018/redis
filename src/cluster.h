// 高可扩展性
#ifndef __CLUSTER_H
#define __CLUSTER_H

/*-----------------------------------------------------------------------------
 * Redis cluster data structures, defines, exported API.
 *----------------------------------------------------------------------------*/

#define CLUSTER_SLOTS 16384     // 槽数量
#define CLUSTER_OK 0            // 集群在线
#define CLUSTER_FAIL 1          // 集群下线
#define CLUSTER_NAMELEN 40      // 节点名字的长度
#define CLUSTER_PORT_INCR 10000 // 集群的实际端口号 = 用户指定的端口号 + REDIS_CLUSTER_PORT_INCR

/* The following defines are amount of time, sometimes expressed as
 * multiplicators of the node timeout value (when ending with MULT). */
#define CLUSTER_FAIL_REPORT_VALIDITY_MULT 2 /* Fail report validity. */
#define CLUSTER_FAIL_UNDO_TIME_MULT 2       /* Undo fail if master is back. */
#define CLUSTER_MF_TIMEOUT 5000             /* Milliseconds to do a manual failover. */
#define CLUSTER_MF_PAUSE_MULT 2             /* Master pause manual failover mult. */
#define CLUSTER_SLAVE_MIGRATION_DELAY 5000  /* Delay for slave migration. */

/* Redirection errors returned by getNodeByQuery(). */
#define CLUSTER_REDIR_NONE 0          // 节点可以为请求提供服务
#define CLUSTER_REDIR_CROSS_SLOT 1    // 表示客户端请求的 keys 并没有在同一个哈希 slot 中.../* -CROSSSLOT request. */
#define CLUSTER_REDIR_UNSTABLE 2      /* -TRYAGAIN redirection required */
#define CLUSTER_REDIR_ASK 3           // 此时表示客户端请求的 key 正在迁移中   /* -ASK redirection required. */
#define CLUSTER_REDIR_MOVED 4         // 此时表示客户端请求的 key 已经迁移到其他集群节点./* -MOVED redirection required. */
#define CLUSTER_REDIR_DOWN_STATE 5    /* -CLUSTERDOWN, global state. */
#define CLUSTER_REDIR_DOWN_UNBOUND 6  /* -CLUSTERDOWN, 没绑定 slot. */
#define CLUSTER_REDIR_DOWN_RO_STATE 7 /* -CLUSTERDOWN, 允许读 */

struct clusterNode;

/* clusterLink encapsulates everything needed to talk with a remote node. */
typedef struct clusterLink {
    mstime_t ctime;           /* Link creation time */
    connection *conn;         /* Connection to remote node */
    sds sndbuf;               // 输出缓冲区,保存着等待发送给其他节点的消息（message）.
    char *rcvbuf;             // 输入缓冲区,保存着从其他节点接收到的消息.
    size_t rcvbuf_len;        /* Used size of rcvbuf */
    size_t rcvbuf_alloc;      /* Allocated size of rcvbuf */
    struct clusterNode *node; /* Node related to this link. Initialized to NULL when unknown */
    int inbound;              /* 1 if this link is an inbound link accepted from the related node */
} clusterLink;

/* Cluster node flags and macros. */
#define CLUSTER_NODE_MASTER 1       /* The node is a master */
#define CLUSTER_NODE_SLAVE 2        /* The node is a slave */
#define CLUSTER_NODE_PFAIL 4        /* Failure? Need acknowledge */
#define CLUSTER_NODE_FAIL 8         /* The node is believed to be malfunctioning */
#define CLUSTER_NODE_MYSELF 16      /* This node is myself */
#define CLUSTER_NODE_HANDSHAKE 32   /* We have still to exchange the first ping */
#define CLUSTER_NODE_NOADDR 64      /* We don't know the address of this node */
#define CLUSTER_NODE_MEET 128       /* Send a MEET message to this node */
#define CLUSTER_NODE_MIGRATE_TO 256 /* Master eligible for replica migration. */
#define CLUSTER_NODE_NOFAILOVER 512 /* Slave will not try to failover. */
#define CLUSTER_NODE_NULL_NAME "\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"

#define nodeIsMaster(n) ((n)->flags & CLUSTER_NODE_MASTER)
#define nodeIsSlave(n) ((n)->flags & CLUSTER_NODE_SLAVE)
#define nodeInHandshake(n) ((n)->flags & CLUSTER_NODE_HANDSHAKE)
#define nodeHasAddr(n) (!((n)->flags & CLUSTER_NODE_NOADDR))
#define nodeWithoutAddr(n) ((n)->flags & CLUSTER_NODE_NOADDR)
#define nodeTimedOut(n) ((n)->flags & CLUSTER_NODE_PFAIL)
#define nodeFailed(n) ((n)->flags & CLUSTER_NODE_FAIL)
#define nodeCantFailover(n) ((n)->flags & CLUSTER_NODE_NOFAILOVER)

/* Reasons why a slave is not able to failover. */
#define CLUSTER_CANT_FAILOVER_NONE 0
#define CLUSTER_CANT_FAILOVER_DATA_AGE 1
#define CLUSTER_CANT_FAILOVER_WAITING_DELAY 2
#define CLUSTER_CANT_FAILOVER_EXPIRED 3
#define CLUSTER_CANT_FAILOVER_WAITING_VOTES 4
#define CLUSTER_CANT_FAILOVER_RELOG_PERIOD (60 * 5) /* seconds. */

/* clusterState todo_before_sleep flags. */
#define CLUSTER_TODO_HANDLE_FAILOVER (1 << 0)
#define CLUSTER_TODO_UPDATE_STATE (1 << 1)
#define CLUSTER_TODO_SAVE_CONFIG (1 << 2)
#define CLUSTER_TODO_FSYNC_CONFIG (1 << 3)
#define CLUSTER_TODO_HANDLE_MANUALFAILOVER (1 << 4)

// 注意,PING 、 PONG 和 MEET 实际上是同一种消息.
// PONG 是对 PING 的回复,它的实际格式也为 PING 消息,
// 而 MEET 则是一种特殊的 PING 消息,用于强制消息的接收者将消息的发送者添加到集群中（如果节点尚未在节点列表中的话）
// 通过宏定义定义的节点间通信的消息类型,
#define CLUSTERMSG_TYPE_PING 0                  // Ping 消息
#define CLUSTERMSG_TYPE_PONG 1                  // Pong 用于回复Ping
#define CLUSTERMSG_TYPE_MEET 2                  // Meet 请求将某个节点添加到集群中
#define CLUSTERMSG_TYPE_FAIL 3                  // Fail 将某个节点标记为 FAIL
#define CLUSTERMSG_TYPE_PUBLISH 4               // 通过发布与订阅功能广播消息
#define CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST 5 // 请求进行故障转移操作,要求消息的接收者通过投票来支持消息的发送者
#define CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK 6     // 消息的接收者同意向消息的发送者投票
#define CLUSTERMSG_TYPE_UPDATE 7                // slots 已经发生变化,消息发送者要求消息接收者进行相应的更新
#define CLUSTERMSG_TYPE_MFSTART 8               // 为了进行手动故障转移,暂停各个客户端
#define CLUSTERMSG_TYPE_MODULE 9                // 消息总数
#define CLUSTERMSG_TYPE_PUBLISHSHARD 10         // /* Pub/Sub Publish shard propagation */
#define CLUSTERMSG_TYPE_COUNT 11                // /* Total number of message types. */

// 模块可以设置的标志,以防止某些Redis Cluster功能被启用.当使用模块在Redis Cluster消息总线上实现不同的分布式系统时,是非常有用的.
#define CLUSTER_MODULE_FLAG_NONE 0
#define CLUSTER_MODULE_FLAG_NO_FAILOVER (1 << 1)
#define CLUSTER_MODULE_FLAG_NO_REDIRECTION (1 << 2) // 集群key重定向功能被禁止

/* This structure represent elements of node->fail_reports. */
typedef struct clusterNodeFailReport {
    struct clusterNode *node; // 报告当前节点已经下线的节点
    mstime_t time;            // 报告时间
} clusterNodeFailReport;

typedef struct clusterNode {
    mstime_t ctime;                         // 创建节点的时间
    char name[CLUSTER_NAMELEN];             // 节点的名字
    int flags;                              // 节点标识,标记节点角色或者状态,比如主节点从节点或者在线和下线
    uint64_t configEpoch;                   // 当前节点已知的集群统一epoch
    unsigned char slots[CLUSTER_SLOTS / 8]; /* slots handled by this node */
    uint16_t *slot_info_pairs;              /* Slots info represented as (start/end) pair (consecutive index). */
    int slot_info_pairs_count;              /* Used number of slots in slot_info_pairs */
    int numslots;                           /* Number of slots handled by this node */
    int numslaves;                          /* Number of slave nodes, if this is a master */
    struct clusterNode **slaves;            /* pointers to slave nodes */
    struct clusterNode *slaveof;            /* pointer to the master node. Note that it
                                               may be NULL even if the node is a slave
                                               if we don't have the master node in our
                                               tables. */
    mstime_t ping_sent;                     // 当前节点最后一次向该节点发送 PING 消息的时间
    mstime_t pong_received;                 // 当前节点最后一次收到该节点 PONG 消息的时间
    mstime_t data_received;                 /* Unix time we received any data */
    mstime_t fail_time;                     // FAIL 标志位被设置的时间
    mstime_t voted_time;                    /* Last time we voted for a slave of this master */
    mstime_t repl_offset_time;              // 当前节点的repl时间偏移
    mstime_t orphaned_time;                 /* Starting time of orphaned master condition */
    long long repl_offset;                  /* Last known repl offset for this node. */
    char ip[NET_IP_STR_LEN];                // 节点的IP 地址
    sds hostname;                           /* The known hostname for this node */
    int port;                               //
    int pport;                              /* Latest known clients plaintext port. Only used
                                               if the main clients port is for TLS. */
    int cport;                              // 集群通信端口,一般是端口+1000
    clusterLink *link;                      // 和该节点的 tcp 连接
    clusterLink *inbound_link;              /* TCP/IP link accepted from this node */
    list *fail_reports;                     // 下线记录列表
} clusterNode;

// 相同slot内的key的链表
typedef struct slotToKeys {
    uint64_t count;  // slot中的key数量。
    dictEntry *head; // 槽中的第一个键-值条目。
} slotToKeys;

// 所有槽的槽到键映射，在此文件外部不透明。
struct clusterSlotToKeyMapping {
    slotToKeys by_slot[CLUSTER_SLOTS];
};

// 用于集群模式的Dict条目元数据,用于Slot to Key API,以形成属于同一插槽的条目的链表.
typedef struct clusterDictEntryMetadata {
    dictEntry *prev;
    dictEntry *next;
} clusterDictEntryMetadata;

typedef struct clusterState {
    clusterNode *myself;                              // 当前节点的clusterNode信息
    uint64_t currentEpoch;                            //
    int state;                                        // CLUSTER_OK, CLUSTER_FAIL, ... */
    int size;                                         // Num of master nodes with at least one slot */
    dict *nodes;                                      // name到clusterNode的字典
    dict *nodes_black_list;                           // Nodes we don't re-add for a few seconds. */
    clusterNode *migrating_slots_to[CLUSTER_SLOTS];   //
    clusterNode *importing_slots_from[CLUSTER_SLOTS]; //
    clusterNode *slots[CLUSTER_SLOTS];                // slot 和节点的对应关系
    rax *slots_to_channels;
    /* The following fields are used to take the slave state on elections. */
    mstime_t failover_auth_time;  /* Time of previous or next election. */
    int failover_auth_count;      /* Number of votes received so far. */
    int failover_auth_sent;       /* True if we already asked for votes. */
    int failover_auth_rank;       /* This slave rank for current auth request. */
    uint64_t failover_auth_epoch; /* Epoch of the current election. */
    int cant_failover_reason;     /* Why a slave is currently not able to
                                     failover. See the CANT_FAILOVER_* macros. */
    /* Manual failover state in common. */
    mstime_t mf_end; /* Manual failover time limit (ms unixtime).
                        It is zero if there is no MF in progress. */
    /* Manual failover state of master. */
    clusterNode *mf_slave; /* Slave performing the manual failover. */
    /* Manual failover state of slave. */
    long long mf_master_offset; /* Master offset the slave needs to start MF
                                   or -1 if still not received. */
    int mf_can_start;           /* If non-zero signal that the manual failover
                                   can start requesting masters vote. */
    /* The following fields are used by masters to take state on elections. */
    uint64_t lastVoteEpoch; /* Epoch of the last vote granted. */
    int todo_before_sleep;  /* Things to do in clusterBeforeSleep(). */
    /* Stats */
    /* Messages received and sent by type. */
    long long stats_bus_messages_sent[CLUSTERMSG_TYPE_COUNT];
    long long stats_bus_messages_received[CLUSTERMSG_TYPE_COUNT];
    long long stats_pfail_nodes;                                 /* Number of nodes in PFAIL status,
                                                                    excluding nodes without address. */
    unsigned long long stat_cluster_links_buffer_limit_exceeded; /* Total number of cluster links freed due to exceeding buffer limit */
} clusterState;

/* Redis cluster messages header */

/* Initially we don't know our "name", but we'll find it once we connect
 * to the first node, using the getsockname() function. Then we'll use this
 * address for all the next messages. */
typedef struct {
    char nodename[CLUSTER_NAMELEN]; // 节点的名字,默认是随机的,MEET消息发送并得到回复后,集群会为该节点设置正式的名称
    uint32_t ping_sent;             // 发送节点最后一次给接收节点发送 PING 消息的时间戳,收到对应 PONG 回复后会被赋值为0
    uint32_t pong_received;         // 发送节点最后一次收到接收节点发送 PONG 消息的时间戳
    char ip[NET_IP_STR_LEN];        /* IP address last time it was seen */
    uint16_t port;                  /* base port last time it was seen */
    uint16_t cport;                 /* cluster port last time it was seen */
    uint16_t flags;                 /* node->flags copy */
    uint16_t pport;                 /* plaintext-port, when base port is TLS */
    uint16_t notused1;              // 对齐字符
} clusterMsgDataGossip;

typedef struct {
    char nodename[CLUSTER_NAMELEN]; // 下线节点的名字
} clusterMsgDataFail;

typedef struct {
    uint32_t channel_len;
    uint32_t message_len;
    unsigned char bulk_data[8]; /* 8 bytes just as placeholder. */
} clusterMsgDataPublish;

typedef struct {
    uint64_t configEpoch;                   /* Config epoch of the specified instance. */
    char nodename[CLUSTER_NAMELEN];         /* Name of the slots owner. */
    unsigned char slots[CLUSTER_SLOTS / 8]; /* Slots bitmap. */
} clusterMsgDataUpdate;

typedef struct {
    uint64_t module_id;         /* ID of the sender module. */
    uint32_t len;               /* ID of the sender module. */
    uint8_t type;               /* Type from 0 to 255. */
    unsigned char bulk_data[3]; /* 3 bytes just as placeholder. */
} clusterMsgModule;

/* The cluster supports optional extension messages that can be sent
 * along with ping/pong/meet messages to give additional info in a
 * consistent manner. */
typedef enum {
    CLUSTERMSG_EXT_TYPE_HOSTNAME,
} clusterMsgPingtypes;

/* Helper function for making sure extensions are eight byte aligned. */
#define EIGHT_BYTE_ALIGN(size) ((((size) + 7) / 8) * 8)

typedef struct {
    char hostname[1]; /* The announced hostname, ends with \0. */
} clusterMsgPingExtHostname;

typedef struct {
    uint32_t length; /* Total length of this extension message (including this header) */
    uint16_t type;   /* Type of this extension message (see clusterMsgPingExtTypes) */
    uint16_t unused; /* 16 bits of padding to make this structure 8 byte aligned. */
    union
    {
        clusterMsgPingExtHostname hostname;
    } ext[]; /* Actual extension information, formatted so that the data is 8
              * byte aligned, regardless of its content. */
} clusterMsgPingExt;

union clusterMsgData
{
    // PING, MEET 或者 PONG 消息时,ping 字段被赋值
    struct {
        /* Array of N clusterMsgDataGossip structures */
        clusterMsgDataGossip gossip[1];
        /* Extension data that can optionally be sent for ping/meet/pong
         * messages. We can't explicitly define them here though, since
         * the gossip array isn't the real length of the gossip data. */
    } ping;

    // FAIL 消息时,fail 被赋值
    struct {
        clusterMsgDataFail about;
    } fail;

    /* PUBLISH */
    struct {
        clusterMsgDataPublish msg;
    } publish;

    /* UPDATE */
    struct {
        clusterMsgDataUpdate nodecfg;
    } update;

    /* MODULE */
    struct {
        clusterMsgModule msg;
    } module;
};

#define CLUSTER_PROTO_VER 1 /* Cluster bus protocol version. */

typedef struct {
    char sig[4];                              // 标志位,"RCmb" (Redis Cluster message bus).
    uint32_t totlen;                          // 消息总长度
    uint16_t ver;                             // 消息协议版本,当前为1
    uint16_t port;                            // 端口
    uint16_t type;                            // 消息类型
    uint16_t count;                           /* Only used for some kind of messages. */
    uint64_t currentEpoch;                    // 表示本节点当前记录的整个集群的统一的epoch,用来决策选举投票等,与configEpoch不同的是：configEpoch表示的是master节点的唯一标志,currentEpoch是集群的唯一标志.
    uint64_t configEpoch;                     // 每个master节点都有一个唯一的configEpoch做标志,如果和其他master节点冲突,会强制自增使本节点在集群中唯一
    uint64_t offset;                          // 主从复制偏移相关信息,主节点和从节点含义不同
    char sender[CLUSTER_NAMELEN];             // 发送节点的名称
    unsigned char myslots[CLUSTER_SLOTS / 8]; // 本节点负责的slots信息,16384/8个char数组,一共为16384bit
    char slaveof[CLUSTER_NAMELEN];            // master信息,假如本节点是slave节点的话,协议带有master信息
    char myip[NET_IP_STR_LEN];                // 发送方的IP
    uint16_t extensions;                      /* Number of extensions sent along with this packet. */
    char notused1[30];                        // 30 bytes    保留字段
    uint16_t pport;                           /* Sender TCP plaintext port, if base port is TLS */
    uint16_t cport;                           // 集群的通信端口
    uint16_t flags;                           // 本节点当前的状态,比如 CLUSTER_NODE_HANDSHAKE、CLUSTER_NODE_MEET
    unsigned char state;                      /* Cluster state from the POV of the sender */
    unsigned char mflags[3];                  // 本条消息的类型,目前只有两类：CLUSTERMSG_FLAG0_PAUSED、CLUSTERMSG_FLAG0_FORCEACK
    union clusterMsgData data;
} clusterMsg;

/* clusterMsg defines the gossip wire protocol exchanged among Redis cluster
 * members, which can be running different versions of redis-server bits,
 * especially during cluster rolling upgrades.
 *
 * Therefore, fields in this struct should remain at the same offset from
 * release to release. The static asserts below ensures that incompatible
 * changes in clusterMsg be caught at compile time.
 */

static_assert(offsetof(clusterMsg, sig) == 0, "unexpected field offset");
static_assert(offsetof(clusterMsg, totlen) == 4, "unexpected field offset");
static_assert(offsetof(clusterMsg, ver) == 8, "unexpected field offset");
static_assert(offsetof(clusterMsg, port) == 10, "unexpected field offset");
static_assert(offsetof(clusterMsg, type) == 12, "unexpected field offset");
static_assert(offsetof(clusterMsg, count) == 14, "unexpected field offset");
static_assert(offsetof(clusterMsg, currentEpoch) == 16, "unexpected field offset");
static_assert(offsetof(clusterMsg, configEpoch) == 24, "unexpected field offset");
static_assert(offsetof(clusterMsg, offset) == 32, "unexpected field offset");
static_assert(offsetof(clusterMsg, sender) == 40, "unexpected field offset");
static_assert(offsetof(clusterMsg, myslots) == 80, "unexpected field offset");
static_assert(offsetof(clusterMsg, slaveof) == 2128, "unexpected field offset");
static_assert(offsetof(clusterMsg, myip) == 2168, "unexpected field offset");
static_assert(offsetof(clusterMsg, extensions) == 2214, "unexpected field offset");
static_assert(offsetof(clusterMsg, notused1) == 2216, "unexpected field offset");
static_assert(offsetof(clusterMsg, pport) == 2246, "unexpected field offset");
static_assert(offsetof(clusterMsg, cport) == 2248, "unexpected field offset");
static_assert(offsetof(clusterMsg, flags) == 2250, "unexpected field offset");
static_assert(offsetof(clusterMsg, state) == 2252, "unexpected field offset");
static_assert(offsetof(clusterMsg, mflags) == 2253, "unexpected field offset");
static_assert(offsetof(clusterMsg, data) == 2256, "unexpected field offset");

#define CLUSTERMSG_MIN_LEN (sizeof(clusterMsg) - sizeof(union clusterMsgData))

/* Message flags better specify the packet content or are used to
 * provide some information about the node state. */
#define CLUSTERMSG_FLAG0_PAUSED (1 << 0) /* Master paused for manual failover. */
#define CLUSTERMSG_FLAG0_FORCEACK                                              \
    (1 << 1)                               /* Give ACK to AUTH_REQUEST even if \
master is up. */
#define CLUSTERMSG_FLAG0_EXT_DATA (1 << 2) /* Message contains extension data */

/* ---------------------- API exported outside cluster.c -------------------- */
void clusterInit(void);

void clusterCron(void);

void clusterBeforeSleep(void);

clusterNode *getNodeByQuery(client *c, struct redisCommand *cmd, robj **argv, int argc, int *hashslot, int *ask);

int verifyClusterNodeId(const char *name, int length);

clusterNode *clusterLookupNode(const char *name, int length);

int clusterRedirectBlockedClientIfNeeded(client *c);

void clusterRedirectClient(client *c, clusterNode *n, int hashslot, int error_code);

void migrateCloseTimedoutSockets(void);

int verifyClusterConfigWithData(void);

unsigned long getClusterConnectionsCount(void);

int clusterSendModuleMessageToTarget(const char *target, uint64_t module_id, uint8_t type, const char *payload, uint32_t len);

void clusterPropagatePublish(robj *channel, robj *message, int sharded);

unsigned int keyHashSlot(char *key, int keylen);

void slotToKeyAddEntry(dictEntry *entry, redisDb *db);

void slotToKeyDelEntry(dictEntry *entry, redisDb *db);

void slotToKeyReplaceEntry(dictEntry *entry, redisDb *db);

void slotToKeyInit(redisDb *db);

void slotToKeyFlush(redisDb *db);

void slotToKeyDestroy(redisDb *db);

void clusterUpdateMyselfFlags(void);

void clusterUpdateMyselfIp(void);

void slotToChannelAdd(sds channel);

void slotToChannelDel(sds channel);

void clusterUpdateMyselfHostname(void);

#endif /* __CLUSTER_H */
