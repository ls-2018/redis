#ifndef __REDIS_CONNECTION_H
#define __REDIS_CONNECTION_H

#include <errno.h>
#include <sys/uio.h>

#define CONN_INFO_LEN 32

struct aeEventLoop;
typedef struct connection connection;

typedef enum {
    CONN_STATE_NONE = 0,   //
    CONN_STATE_CONNECTING, // 链接中
    CONN_STATE_ACCEPTING,  // 接收数据中
    CONN_STATE_CONNECTED,  // 已建立连接
    CONN_STATE_CLOSED,     // 已关闭连接
    CONN_STATE_ERROR       // 错误
} ConnectionState;

#define CONN_FLAG_CLOSE_SCHEDULED (1 << 0) // 由处理程序计划关闭
#define CONN_FLAG_WRITE_BARRIER (1 << 1)   // 已请求 写屏障
#define CONN_TYPE_SOCKET 1                 //
#define CONN_TYPE_TLS 2                    //

typedef void (*ConnectionCallbackFunc)(struct connection *conn);

// 这个结构是核心，后续的各种网络操作，都是通过ConnectionType中的指针调用的
typedef struct ConnectionType {
    void (*ae_handler)(struct aeEventLoop *el, int fd, void *clientData, int mask);
    int (*connect)(struct connection *conn, const char *addr, int port, const char *source_addr, ConnectionCallbackFunc connect_handler);
    int (*write)(struct connection *conn, const void *data, size_t data_len);
    int (*writev)(struct connection *conn, const struct iovec *iov, int iovcnt);
    int (*read)(struct connection *conn, void *buf, size_t buf_len);
    void (*close)(struct connection *conn);
    int (*accept)(struct connection *conn, ConnectionCallbackFunc accept_handler);
    int (*set_write_handler)(struct connection *conn, ConnectionCallbackFunc handler, int barrier);
    int (*set_read_handler)(struct connection *conn, ConnectionCallbackFunc handler);
    const char *(*get_last_error)(struct connection *conn);
    int (*blocking_connect)(struct connection *conn, const char *addr, int port, long long timeout);
    ssize_t (*sync_write)(struct connection *conn, char *ptr, ssize_t size, long long timeout);
    ssize_t (*sync_read)(struct connection *conn, char *ptr, ssize_t size, long long timeout);
    ssize_t (*sync_readline)(struct connection *conn, char *ptr, ssize_t size, long long timeout);
    int (*get_type)(struct connection *conn);
} ConnectionType;

// 具体的操作对象
struct connection {
    ConnectionType *type;                 // 链接类型
    ConnectionState state;                // 链接状态
    short int flags;                      // 标志位
    short int refs;                       // 引用数
    int last_errno;                       // 最新的错误号
    void *private_data;                   // 私有数据
    ConnectionCallbackFunc conn_handler;  // 链接创建时的触发的函数
    ConnectionCallbackFunc write_handler; // 写完数据时的触发的函数
    ConnectionCallbackFunc read_handler;  // 读完数据时的触发的函数
    int fd;                               // 具体描述符
};

// 链接建立好之后的回调函数
static inline int connAccept(connection *conn, ConnectionCallbackFunc accept_handler) {
    return conn->type->accept(conn, accept_handler); // connSocketAccept
}

static inline int connConnect(connection *conn, const char *addr, int port, const char *src_addr, ConnectionCallbackFunc connect_handler) {
    return conn->type->connect(conn, addr, port, src_addr, connect_handler);
}

// 阻塞链接
// 实现这一点是为了简化到抽象连接的转换，但是可能应该从cluster.c和replication.c中重构出来，以支持纯异步实现。
static inline int connBlockingConnect(connection *conn, const char *addr, int port, long long timeout) {
    return conn->type->blocking_connect(conn, addr, port, timeout);
}

// 写入连接，行为与Write(2)相同。
static inline int connWrite(connection *conn, const void *data, size_t data_len) {
    return conn->type->write(conn, data, data_len);
}

// 从iov成员指定的iovcnt缓冲区中收集输出数据
// 测试类似eagain的条件，使用connGetState()查看连接状态是否仍然为CONN_STATE_CONNECTED。
static inline int connWritev(connection *conn, const struct iovec *iov, int iovcnt) {
    //    返回值-1表示错误
    return conn->type->writev(conn, iov, iovcnt);
}

// 从连接中读取数据
static inline int connRead(connection *conn, void *buf, size_t buf_len) {
    int ret = conn->type->read(conn, buf, buf_len);
    return ret;
}

/*注册一个写处理程序,在连接可写时调用.如果为NULL,则删除现有的处理程序.*/
static inline int connSetWriteHandler(connection *conn, ConnectionCallbackFunc func) {
    return conn->type->set_write_handler(conn, func, 0);
}

/*注册一个读处理程序,在连接可读时调用.如果为NULL,则删除现有的处理程序.*/
static inline int connSetReadHandler(connection *conn, ConnectionCallbackFunc func) { // clusterReadHandler、readQueryFromClient、syncWithMaster、readSyncBulkPayload
    return conn->type->set_read_handler(conn, func);
}

// 设置一个写处理程序，并可能启用一个写障碍，
// 当您希望在发送回复之前将内容持久化到磁盘，并且希望以组的方式执行此操作时非常有用。
static inline int connSetWriteHandlerWithBarrier(connection *conn, ConnectionCallbackFunc func, int barrier) {
    // 创建可写事件的监听,以及设置回调函数
    return conn->type->set_write_handler(conn, func, barrier);
}

static inline void connClose(connection *conn) {
    conn->type->close(conn);
}

// 以字符串形式返回连接遇到的最后一个错误。如果没有错误，返回NULL
static inline const char *connGetLastError(connection *conn) {
    return conn->type->get_last_error(conn);
}

static inline ssize_t connSyncWrite(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return conn->type->sync_write(conn, ptr, size, timeout);
}

static inline ssize_t connSyncRead(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return conn->type->sync_read(conn, ptr, size, timeout);
}

static inline ssize_t connSyncReadLine(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return conn->type->sync_readline(conn, ptr, size, timeout);
}

// 返回 CONN_TYPE_*
static inline int connGetType(connection *conn) {
    return conn->type->get_type(conn);
}

static inline int connLastErrorRetryable(connection *conn) {
    return conn->last_errno == EINTR;
}

connection *connCreateSocket();

connection *connCreateAcceptedSocket(int fd);

connection *connCreateTLS();

connection *connCreateAcceptedTLS(int fd, int require_auth);

void connSetPrivateData(connection *conn, void *data);

void *connGetPrivateData(connection *conn);

int connGetState(connection *conn);

int connHasWriteHandler(connection *conn);

int connHasReadHandler(connection *conn);

int connGetSocketError(connection *conn);

/* anet-style wrappers to conns */
int connBlock(connection *conn);
// 将连接设为非阻塞模式
int connNonBlock(connection *conn);

int connEnableTcpNoDelay(connection *conn);

int connDisableTcpNoDelay(connection *conn);

int connKeepAlive(connection *conn, int interval);

int connSendTimeout(connection *conn, long long ms);

int connRecvTimeout(connection *conn, long long ms);

int connPeerToString(connection *conn, char *ip, size_t ip_len, int *port);

int connFormatFdAddr(connection *conn, char *buf, size_t buf_len, int fd_to_str_type);

int connSockName(connection *conn, char *ip, size_t ip_len, int *port);

const char *connGetInfo(connection *conn, char *buf, size_t buf_len);

sds connTLSGetPeerCert(connection *conn);

int tlsHasPendingData();

int tlsProcessPendingData();

#endif
