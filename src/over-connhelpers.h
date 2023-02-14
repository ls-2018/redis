#ifndef __REDIS_CONNHELPERS_H
#define __REDIS_CONNHELPERS_H

#include "over-connection.h"

// 增加连接引用.在连接处理程序中,我们保证refs >= 1,因此connClose()总是安全的.
static inline void connIncrRefs(connection *conn) {
    conn->refs++;
}

static inline void connDecrRefs(connection *conn) {
    conn->refs--;
}

static inline int connHasRefs(connection *conn) {
    return conn->refs;
}

// 当客户端链接 建立好以后,执行回调函数
static inline int callHandler(connection *conn, ConnectionCallbackFunc handler) {
    connIncrRefs(conn);
    if (handler) {
        // clientAcceptHandler
        handler(conn);
    }
    connDecrRefs(conn);
    if (conn->flags & CONN_FLAG_CLOSE_SCHEDULED) { // 由处理程序计划关闭
        if (!connHasRefs(conn)) {
            connClose(conn);
        }
        return 0;
    }
    return 1;
}

#endif
