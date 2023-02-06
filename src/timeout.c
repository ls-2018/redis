/* Copyright (c) 2009-2020, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "over-server.h"
#include "cluster.h"

/* ========================== Clients timeouts ============================= */

/* Check if this blocked client timedout (does nothing if the client is
 * not blocked right now). If so send a reply, unblock it, and return 1.
 * Otherwise 0 is returned and no operation is performed. */
int checkBlockedClientTimeout(client *c, mstime_t now) {
    if (c->flags & CLIENT_BLOCKED && c->bpop.timeout != 0 && c->bpop.timeout < now) {
        /* Handle blocking operation specific timeout. */
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
        /* Cluster: handle unblock & redirect of clients blocked
         * into keys no longer served by this server. */
        if (server.cluster_enabled) {
            if (clusterRedirectBlockedClientIfNeeded(c))
                unblockClient(c); // 取消客户端的阻塞状态
        }
    }
    return 0; // 客户度没有被关闭
}

/* For blocked clients timeouts we populate a radix tree of 128 bit keys
 * composed as such:
 *
 *  [8 byte big endian expire time]+[8 byte client ID]
 *
 * We don't do any cleanup in the Radix tree: when we run the clients that
 * reached the timeout already, if they are no longer existing or no longer
 * blocked with such timeout, we just go forward.
 *
 * Every time a client blocks with a timeout, we add the client in
 * the tree. In beforeSleep() we call handleBlockedClientsTimeout() to run
 * the tree and unblock the clients. */

#define CLIENT_ST_KEYLEN 16 /* 8 bytes mstime + 8 bytes client ID. */

/* Given client ID and timeout, write the resulting radix tree key in buf. */
void encodeTimeoutKey(unsigned char *buf, uint64_t timeout, client *c) {
    timeout = htonu64(timeout);
    memcpy(buf, &timeout, sizeof(timeout));
    memcpy(buf + 8, &c, sizeof(c));
    if (sizeof(c) == 4)
        memset(buf + 12, 0, 4); /* Zero padding for 32bit target. */
}

/* Given a key encoded with encodeTimeoutKey(), resolve the fields and write
 * the timeout into *toptr and the client pointer into *cptr. */
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

/* Remove the client from the table when it is unblocked for reasons
 * different than timing out. */
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

/* Get a timeout value from an object and store it into 'timeout'.
 * The final timeout is always stored as milliseconds as a time where the
 * timeout will expire, however the parsing is performed according to
 * the 'unit' that can be seconds or milliseconds.
 *
 * Note that if the timeout is zero (usually from the point of view of
 * commands API this means no timeout) the value stored into 'timeout'
 * is zero. */
int getTimeoutFromObjectOrReply(client *c, robj *object, mstime_t *timeout, int unit) {
    long long tval;
    long double ftval;

    if (unit == UNIT_SECONDS) {
        if (getLongDoubleFromObjectOrReply(c, object, &ftval, "timeout is not a float or out of range") != C_OK)
            return C_ERR;
        tval = (long long)(ftval * 1000.0);
    }
    else {
        if (getLongLongFromObjectOrReply(c, object, &tval, "timeout is not an integer or out of range") != C_OK)
            return C_ERR;
    }

    if (tval < 0) {
        addReplyError(c, "timeout is negative");
        return C_ERR;
    }

    if (tval > 0) {
        tval += mstime();
    }
    *timeout = tval;

    return C_OK;
}
