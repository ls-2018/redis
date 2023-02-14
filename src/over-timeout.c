#include "over-server.h"
#include "cluster.h"

/* ========================== Clients timeouts ============================= */

// 检查这个被阻塞的客户端是否超时(如果客户端现在没有被阻塞,则不执行任何操作).
// 如果是,发送回复,解除屏蔽,并返回1.否则返回0,不做任何操作.
int checkBlockedClientTimeout(client *c, mstime_t now) {
    if (c->flags & CLIENT_BLOCKED && c->bpop.timeout != 0 && c->bpop.timeout < now) {
        replyToBlockedClientTimedOut(c);
        unblockClient(c);
        return 1;
    }
    else {
        return 0;
    }
}

// 检查客户端是否已经超时,如果超时就关闭客户端,并返回 1 ;否则返回 0 .
int clientsCronHandleTimeout(client *c, mstime_t now_ms) {
    time_t now = now_ms / 1000; // 获取当前时间
    if (server.maxidletime &&                           // 服务器设置了 maxidletime 时间
        !(c->flags & CLIENT_SLAVE) &&                   // 不检查作为从服务器的客户端
        !mustObeyClient(c) &&                           // 不检查主节点 和AOF
        !(c->flags & CLIENT_BLOCKED) &&                 // 不检查被阻塞的客户端
        !(c->flags & CLIENT_PUBSUB) &&                  // 不检查订阅了频道的客户端
        (now - c->lastinteraction > server.maxidletime) // 客户端最后一次与服务器通讯的时间已经超过了 maxidletime 时间
    ) {
        serverLog(LL_VERBOSE, "关闭客户端");
        freeClient(c); // 关闭超时客户端
        return 1;
    }
    else if (c->flags & CLIENT_BLOCKED) {
        if (server.cluster_enabled) {
            if (clusterRedirectBlockedClientIfNeeded(c))
                unblockClient(c); // 取消客户端的阻塞状态
        }
    }
    return 0; // 客户度没有被关闭
}


/*对于阻塞的客户端超时,我们填充一个128位键的基树像这样组成的:
＊
*[8字节big endian过期时间]+[8字节客户端ID]
＊
*我们不做任何清理Radix树:当我们运行客户端
*已达到超时,如果它们不再存在或不再存在
*阻塞了这样的超时,我们只是继续前进.
＊
*每次客户端出现超时阻塞时,我们都会添加该客户端
*树.在beforeSleep()中,我们调用handleBlockedClientsTimeout()来运行
*树和解除阻塞客户端.*/

#define CLIENT_ST_KEYLEN 16 /* 8 bytes mstime + 8 bytes client ID. */

void encodeTimeoutKey(unsigned char *buf, uint64_t timeout, client *c) {
    timeout = htonu64(timeout);
    memcpy(buf, &timeout, sizeof(timeout));
    memcpy(buf + 8, &c, sizeof(c));
    if (sizeof(c) == 4)
        memset(buf + 12, 0, 4); /* Zero padding for 32bit target. */
}

void decodeTimeoutKey(unsigned char *buf, uint64_t *toptr, client **cptr) {
    memcpy(toptr, buf, sizeof(*toptr));
    *toptr = ntohu64(*toptr);
    memcpy(cptr, buf + 8, sizeof(*cptr));
}

/* Add the specified client id / timeout as a key in the radix tree we use
 * to handle blocked clients timeouts. The client is not added to the list
 * if its timeout is zero (block forever). */
void addClientToTimeoutTable(client *c) {
    if (c->bpop.timeout == 0)
        return;
    uint64_t timeout = c->bpop.timeout;
    unsigned char buf[CLIENT_ST_KEYLEN];
    encodeTimeoutKey(buf, timeout, c);
    if (raxTryInsert(server.clients_timeout_table, buf, sizeof(buf), NULL, NULL))
        c->flags |= CLIENT_IN_TO_TABLE;
}


void removeClientFromTimeoutTable(client *c) {
    if (!(c->flags & CLIENT_IN_TO_TABLE))
        return;
    c->flags &= ~CLIENT_IN_TO_TABLE;
    uint64_t timeout = c->bpop.timeout;
    unsigned char buf[CLIENT_ST_KEYLEN];
    encodeTimeoutKey(buf, timeout, c);
    raxRemove(server.clients_timeout_table, buf, sizeof(buf), NULL);
}

/* 这个函数在beforeSleep()中被调用,以解除在阻塞操作中等待超时的客户端阻塞.*/
void handleBlockedClientsTimeout(void) {
    if (raxSize(server.clients_timeout_table) == 0) { // 返回基数树中元素的数量.
        return;
    }
    uint64_t now = mstime(); // 毫秒数
    raxIterator ri;
    raxStart(&ri, server.clients_timeout_table);
    raxSeek(&ri, "^", NULL, 0);

    while (raxNext(&ri)) {
        uint64_t timeout;
        client *c;
        decodeTimeoutKey(ri.key, &timeout, &c);
        if (timeout >= now) {
            break; /* 所有的暂停都在未来.*/
        }
        c->flags &= ~CLIENT_IN_TO_TABLE;
        checkBlockedClientTimeout(c, now);
        raxRemove(server.clients_timeout_table, ri.key, ri.key_len, NULL);
        raxSeek(&ri, "^", NULL, 0);
    }
    raxStop(&ri); // 释放迭代器
}

// 从对象中获取一个超时值,并将其存储到'timeout'中.
// 最终超时总是以毫秒的形式存储,作为超时到期的时间,但是解析是根据“单位”执行的,可以是秒或毫秒.
// 注意,如果超时为零(通常从命令API的角度来看,这意味着没有超时),存储到'timeout'的值为零.
int getTimeoutFromObjectOrReply(client *c, robj *object, mstime_t *timeout, int unit) {
    long long tval;
    long double ftval;

    if (unit == UNIT_SECONDS) {
        if (getLongDoubleFromObjectOrReply(c, object, &ftval, "Timeout不是浮点数,或超出范围") != C_OK)
            return C_ERR;
        tval = (long long)(ftval * 1000.0);
    }
    else {
        if (getLongLongFromObjectOrReply(c, object, &tval, "超时时间不是整数,或超出范围") != C_OK)
            return C_ERR;
    }

    if (tval < 0) {
        addReplyError(c, "超时参数为负");
        return C_ERR;
    }

    if (tval > 0) {
        tval += mstime();
    }
    *timeout = tval;

    return C_OK;
}
