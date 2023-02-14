/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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
#include "over-atomicvar.h"
#include "cluster.h"
#include "script.h"
#include <sys/socket.h>
#include <sys/uio.h>
#include <math.h>
#include <ctype.h>

static void setProtocolError(const char *errstr, client *c);

int postponeClientRead(client *c);

int ProcessingEventsWhileBlocked = 0; /* See processEventsWhileBlocked(). */

// 返回指定SDS字符串从分配器消耗的大小,包括内部碎片.这个函数用于计算客户端输出缓冲区大小.
size_t sdsZmallocSize(sds s) {
    void *sh = sdsAllocPtr(s);
    return zmalloc_size(sh);
}

/* Return the amount of memory used by the sds string at object->ptr
 * for a string object. This includes internal fragmentation. */
// 返回 object->ptr 所指向的字符串对象所使用的内存数量.
size_t getStringObjectSdsUsedMemory(robj *o) {
    serverAssertWithInfo(NULL, o, o->type == OBJ_STRING);
    switch (o->encoding) {
        case OBJ_ENCODING_RAW:
            return sdsZmallocSize(o->ptr);
        case OBJ_ENCODING_EMBSTR:
            return zmalloc_size(o) - sizeof(robj);
        default:
            return 0; /* Just integer encoding for now. */
    }
}

/* Return the length of a string object.
 * This does NOT includes internal fragmentation or sds unused space. */
size_t getStringObjectLen(robj *o) {
    serverAssertWithInfo(NULL, o, o->type == OBJ_STRING);
    switch (o->encoding) {
        case OBJ_ENCODING_RAW:
            return sdslen(o->ptr);
        case OBJ_ENCODING_EMBSTR:
            return sdslen(o->ptr);
        default:
            return 0; /* Just integer encoding for now. */
    }
}

// 回复内容复制函数
void *dupClientReplyValue(void *o) {
    clientReplyBlock *old = o;
    clientReplyBlock *buf = zmalloc(sizeof(clientReplyBlock) + old->size);
    memcpy(buf, o, sizeof(clientReplyBlock) + old->size);
    return buf;
}

void freeClientReplyValue(void *o) {
    zfree(o);
}

// 订阅模式对比函数
int listMatchObjects(void *a, void *b) {
    return equalStringObjects(a, b);
}

/* 这个函数将客户端链接到客户端的全局链接列表.unlinkClient()的作用恰恰相反. */
void linkClient(client *c) {
    listAddNodeTail(server.clients, c); // 将自己添加到末尾
    // 请注意,我们记住了存储客户机的链表节点,这种方法在unlinkClient()中删除客户机将不需要线性扫描,而只需要一个固定时间的操作.
    c->client_list_node = listLast(server.clients); // client记录自身所在list的listNode地址
    uint64_t id = htonu64(c->id);                   // 大端传输

    // 将客户端ID插入前缀树
    raxInsert(server.clients_index, (unsigned char *)&id, sizeof(id), c, NULL);
}

/* 初始化客户端认证状态 */
static void clientSetDefaultAuth(client *c) {
    /* 如果缺省用户不需要认证,则直接进行认证.*/
    c->user = DefaultUser;
    c->authenticated = (c->user->flags & USER_FLAG_NOPASS) && !(c->user->flags & USER_FLAG_DISABLED);
}

// 检查是否需要认证
int authRequired(client *c) {
    int auth_required = (!(DefaultUser->flags & USER_FLAG_NOPASS) || // 需要密码
                         (DefaultUser->flags & USER_FLAG_DISABLED)   // 用户已禁用
                         ) &&
                        !c->authenticated; // 没有认证过
    return auth_required;
}

// 创建客户端链接的数据结构,初始化client对应的数据结构
client *createClient(connection *conn) {
    client *c = zmalloc(sizeof(client));

    //  传递NULL作为conn可以创建一个未连接的客户端.这很有用,因为所有命令都需要在客户机上下文中执行.当命令在其他上下文中执行时(例如Lua脚本),我们需要一个非连接的客户端.

    if (conn) {
        connEnableTcpNoDelay(conn); // 启动TCP_NODELAY,就意味着禁用了Nagle算法,允许小包的发送.
        if (server.tcpkeepalive) {
            connKeepAlive(conn, server.tcpkeepalive); // 设置 TCP keepalive
        }
        connSetReadHandler(conn, readQueryFromClient); // 注册读处理器 一旦客户端请求发送到server,框架就会调用readQueryFromClient
        connSetPrivateData(conn, c);                   // 让 conn->private_data 指向 client 对象
    }
    c->buf = zmalloc(PROTO_REPLY_CHUNK_BYTES); // 输出缓冲区 16k
    selectDb(c, 0);                            // 客户端默认使用0号数据库
    uint64_t client_id;                        // 当前的ID号
    atomicGetIncr(server.next_client_id, client_id, 1);
    c->id = client_id;
    c->resp = 2; // resp版本2
    c->conn = conn;
    c->name = NULL;
    c->bufpos = 0;
    c->buf_usable_size = zmalloc_usable_size(c->buf); // 获取malloc实际分配的内存大小
    c->buf_peak = c->buf_usable_size;                 // 在最近5秒间隔内使用的缓冲器的峰值大小.
    c->buf_peak_last_reset_time = server.unixtime;    // 保留上次重置缓冲区峰值的时间
    c->ref_repl_buf_node = NULL;
    c->ref_block_pos = 0;
    c->qb_pos = 0;
    c->querybuf = sdsempty();
    c->querybuf_peak = 0;
    c->reqtype = 0;
    c->argc = 0;
    c->argv = NULL;
    c->argv_len = 0;
    c->argv_len_sum = 0;
    c->original_argc = 0;
    c->original_argv = NULL;
    c->cmd = c->lastcmd = c->realcmd = NULL;
    c->multibulklen = 0;
    c->bulklen = -1;
    c->sentlen = 0;
    c->flags = 0;
    c->slot = -1;
    c->ctime = c->lastinteraction = server.unixtime;
    clientSetDefaultAuth(c);
    c->replstate = REPL_STATE_NONE;
    c->repl_start_cmd_stream_on_ack = 0;
    c->reploff = 0;
    c->read_reploff = 0;
    c->repl_applied = 0;
    c->repl_ack_off = 0;
    c->repl_ack_time = 0;
    c->repl_last_partial_write = 0;
    c->slave_listening_port = 0;
    c->slave_addr = NULL;
    c->slave_capa = SLAVE_CAPA_NONE;
    c->slave_req = SLAVE_REQ_NONE;
    c->reply = listCreate();
    c->deferred_reply_errors = NULL;
    c->reply_bytes = 0;
    c->obuf_soft_limit_reached_time = 0;
    listSetFreeMethod(c->reply, freeClientReplyValue);
    listSetDupMethod(c->reply, dupClientReplyValue);
    c->btype = BLOCKED_NONE;
    c->bpop.timeout = 0;
    c->bpop.keys = dictCreate(&objectKeyHeapPointerValueDictType);
    c->bpop.target = NULL;
    c->bpop.xread_group = NULL;
    c->bpop.xread_consumer = NULL;
    c->bpop.xread_group_noack = 0;
    c->bpop.numreplicas = 0;
    c->bpop.reploffset = 0;
    c->woff = 0;
    c->watched_keys = listCreate();
    c->pubsub_channels = dictCreate(&objectKeyPointerValueDictType);
    c->pubsub_patterns = listCreate();
    c->pubsubshard_channels = dictCreate(&objectKeyPointerValueDictType);
    c->peerid = NULL;
    c->sockname = NULL;
    c->client_list_node = NULL;
    c->postponed_list_node = NULL;
    c->pending_read_list_node = NULL;
    c->client_tracking_redirection = 0;
    c->client_tracking_prefixes = NULL;
    c->last_memory_usage = 0;
    c->last_memory_type = CLIENT_TYPE_NORMAL;
    c->auth_callback = NULL;
    c->auth_callback_privdata = NULL;
    c->auth_module = NULL;
    listSetFreeMethod(c->pubsub_patterns, decrRefCountVoid);
    listSetMatchMethod(c->pubsub_patterns, listMatchObjects);
    c->mem_usage_bucket = NULL;
    c->mem_usage_bucket_node = NULL;
    // 首先会有一个 lua的客户端走这里

    if (conn) {
        linkClient(c); // 维护了一棵客户端前缀树
    }
    initClientMultiState(c);
    return c;
}

void installClientWriteHandler(client *c) {
    int ae_barrier = 0;
    /* For the fsync=always policy, we want that a given FD is never
     * served for reading and writing in the same event loop iteration,
     * so that in the middle of receiving the query, and serving it
     * to the client, we'll call beforeSleep() that will do the
     * actual fsync of AOF to disk. the write barrier ensures that. */
    if (server.aof_state == AOF_ON && server.aof_fsync == AOF_FSYNC_ALWAYS) {
        ae_barrier = 1;
    }
    if (connSetWriteHandlerWithBarrier(c->conn, sendReplyToClient, ae_barrier) == C_ERR) {
        freeClientAsync(c);
    }
}

/* This function puts the client in the queue of clients that should write
 * their output buffers to the socket. Note that it does not *yet* install
 * the write handler, to start clients are put in a queue of clients that need
 * to write, so we try to do that before returning in the event loop (see the
 * handleClientsWithPendingWrites() function).
 * If we fail and there is more data to write, compared to what the socket
 * buffers can hold, then we'll really install the handler. */
void putClientInPendingWriteQueue(client *c) {
    /* Schedule the client to write the output buffers to the socket only
     * if not already done and, for slaves, if the slave can actually receive
     * writes at this stage. */
    if (!(c->flags & CLIENT_PENDING_WRITE) && (c->replstate == REPL_STATE_NONE || (c->replstate == SLAVE_STATE_ONLINE && !c->repl_start_cmd_stream_on_ack))) {
        /* Here instead of installing the write handler, we just flag the
         * client and put it into a list of clients that have something
         * to write to the socket. This way before re-entering the event
         * loop, we can try to directly write to the client sockets avoiding
         * a system call. We'll only really install the write handler if
         * we'll not be able to write the whole reply at once. */
        c->flags |= CLIENT_PENDING_WRITE; // 设置标志位并添加数据到链表的末尾
        listAddNodeHead(server.clients_pending_write, c);
    }
}

/* This function is called every time we are going to transmit new data
 * to the client. The behavior is the following:
 *
 * If the client should receive new data (normal clients will) the function
 * returns C_OK, and make sure to install the write handler in our event
 * loop so that when the socket is writable new data gets written.
 *
 * If the client should not receive new data, because it is a fake client
 * (used to load AOF in memory), a master or because the setup of the write
 * handler failed, the function returns C_ERR.
 *
 * The function may return C_OK without actually installing the write
 * event handler in the following cases:
 *
 * 1) The event handler should already be installed since the output buffer
 *    already contains something.
 * 2) The client is a slave but not yet online, so we want to just accumulate
 *    writes in the buffer but not actually sending them yet.
 *
 * Typically gets called every time a reply is built, before adding more
 * data to the clients output buffers. If the function returns C_ERR no
 * data should be appended to the output buffers. */
int prepareClientToWrite(client *c) {
    /* If it's the Lua client we always return ok without installing any
     * handler since there is no socket at all. */
    if (c->flags & (CLIENT_SCRIPT | CLIENT_MODULE))
        return C_OK;

    /* If CLIENT_CLOSE_ASAP flag is set, we need not write anything. */
    if (c->flags & CLIENT_CLOSE_ASAP)
        return C_ERR;

    /* CLIENT REPLY OFF / SKIP handling: don't send replies. */
    if (c->flags & (CLIENT_REPLY_OFF | CLIENT_REPLY_SKIP))
        return C_ERR;

    /* Masters don't receive replies, unless CLIENT_MASTER_FORCE_REPLY flag
     * is set. */
    if ((c->flags & CLIENT_MASTER) && !(c->flags & CLIENT_MASTER_FORCE_REPLY))
        return C_ERR;

    if (!c->conn)
        return C_ERR; /* Fake client for AOF loading. */

    /* Schedule the client to write the output buffers to the socket, unless
     * it should already be setup to do so (it has already pending data).
     *
     * If CLIENT_PENDING_READ is set, we're in an IO thread and should
     * not put the client in pending write queue. Instead, it will be
     * done by handleClientsWithPendingReadsUsingThreads() upon return.
     */
    if (!clientHasPendingReplies(c) && io_threads_op == IO_THREADS_OP_IDLE) {
        putClientInPendingWriteQueue(c); // 保存到clients_pending_write
    }

    /* Authorize the caller to queue in the output buffer of this client. */
    return C_OK;
}

/* -----------------------------------------------------------------------------
 * Low level functions to add more data to output buffers.
 * -------------------------------------------------------------------------- */

/* Attempts to add the reply to the static buffer in the client struct.
 * Returns the length of data that is added to the reply buffer.
 *
 * Sanitizer suppression: client->buf_usable_size determined by
 * zmalloc_usable_size() call. Writing beyond client->buf boundaries confuses
 * sanitizer and generates a false positive out-of-bounds error */
REDIS_NO_SANITIZE("bounds")

size_t _addReplyToBuffer(client *c, const char *s, size_t len) {
    size_t available = c->buf_usable_size - c->bufpos;

    /* If there already are entries in the reply list, we cannot
     * add anything more to the static buffer. */
    if (listLength(c->reply) > 0)
        return 0;

    size_t reply_len = len > available ? available : len;
    memcpy(c->buf + c->bufpos, s, reply_len);
    c->bufpos += reply_len;
    /* We update the buffer peak after appending the reply to the buffer */
    if (c->buf_peak < (size_t)c->bufpos)
        c->buf_peak = (size_t)c->bufpos;
    return reply_len;
}

/* Adds the reply to the reply linked list.
 * Note: some edits to this function need to be relayed to AddReplyFromClient. */
void _addReplyProtoToList(client *c, const char *s, size_t len) {
    listNode *ln = listLast(c->reply);
    clientReplyBlock *tail = ln ? listNodeValue(ln) : NULL;

    /* Note that 'tail' may be NULL even if we have a tail node, because when
     * addReplyDeferredLen() is used, it sets a dummy node to NULL just
     * to fill it later, when the size of the bulk length is set. */

    /* Append to tail string when possible. */
    if (tail) {
        /* Copy the part we can fit into the tail, and leave the rest for a
         * new node */
        size_t avail = tail->size - tail->used;
        size_t copy = avail >= len ? len : avail;
        memcpy(tail->buf + tail->used, s, copy);
        tail->used += copy;
        s += copy;
        len -= copy;
    }
    if (len) {
        /* Create a new node, make sure it is allocated to at
         * least PROTO_REPLY_CHUNK_BYTES */
        size_t usable_size;
        size_t size = len < PROTO_REPLY_CHUNK_BYTES ? PROTO_REPLY_CHUNK_BYTES : len;
        tail = zmalloc_usable(size + sizeof(clientReplyBlock), &usable_size);
        /* take over the allocation's internal fragmentation */
        tail->size = usable_size - sizeof(clientReplyBlock);
        tail->used = len;
        memcpy(tail->buf, s, len);
        listAddNodeTail(c->reply, tail);
        c->reply_bytes += tail->size;

        closeClientOnOutputBufferLimitReached(c, 1);
    }
}

void _addReplyToBufferOrList(client *c, const char *s, size_t len) {
    if (c->flags & CLIENT_CLOSE_AFTER_REPLY)
        return;

    /* Replicas should normally not cause any writes to the reply buffer. In case a rogue replica sent a command on the
     * replication link that caused a reply to be generated we'll simply disconnect it.
     * Note this is the simplest way to check a command added a response. Replication links are used to write data but
     * not for responses, so we should normally never get here on a replica client. */
    if (getClientType(c) == CLIENT_TYPE_SLAVE) {
        sds cmdname = c->lastcmd ? c->lastcmd->fullname : NULL;
        logInvalidUseAndFreeClientAsync(c, "Replica generated a reply to command '%s'", cmdname ? cmdname : "<unknown>");
        return;
    }

    size_t reply_len = _addReplyToBuffer(c, s, len);
    if (len > reply_len)
        _addReplyProtoToList(c, s + reply_len, len - reply_len);
}

/* -----------------------------------------------------------------------------
 * Higher level functions to queue data on the client output buffer.
 * The following functions are the ones that commands implementations will call.
 * -------------------------------------------------------------------------- */

// 添加对象'obj'字符串表示到客户端输出缓冲区.
void addReply(client *c, robj *obj) {
    // 把client对象添加到server.clients_pending_write链表的末尾
    if (prepareClientToWrite(c) != C_OK)
        return;

    if (sdsEncodedObject(obj)) { // OBJ_ENCODING_RAW  || OBJ_ENCODING_EMBSTR
        // 把数据添加到buf缓冲区
        _addReplyToBufferOrList(c, obj->ptr, sdslen(obj->ptr));
    }
    else if (obj->encoding == OBJ_ENCODING_INT) {
        // 对于整数编码的字符串,我们只需使用优化的函数将其转换为字符串,并将生成的字符串附加到输出缓冲区.
        char buf[32];
        size_t len = ll2string(buf, sizeof(buf), (long)obj->ptr);
        _addReplyToBufferOrList(c, buf, len);
    }
    else {
        serverPanic("obj->encoding 类型错误 in addReply()");
    }
}

/* Add the SDS 's' string to the client output buffer, as a side effect
 * the SDS string is freed. */
void addReplySds(client *c, sds s) {
    if (prepareClientToWrite(c) != C_OK) {
        /* The caller expects the sds to be free'd. */
        sdsfree(s);
        return;
    }
    _addReplyToBufferOrList(c, s, sdslen(s));
    sdsfree(s);
}

// 这个低级函数只是添加您发送给客户端缓冲区的任何协议,最初尝试静态缓冲区,如果不可能,则使用对象字符串.
// 它是高效的,因为如果不需要,它不会创建SDS对象或Redis对象.只有在扩展对象列表中现有的尾部对象失败时,才会通过调用_addReplyProtoToList()创建该对象.
void addReplyProto(client *c, const char *s, size_t len) {
    //  if (s[len - 1] == '\n') {
    //      printf("%s", s);
    //  }
    //  else {
    //      printf("%s\n", s);
    //  }
    if (prepareClientToWrite(c) != C_OK)
        return;
    _addReplyToBufferOrList(c, s, len);
}

/* Low level function called by the addReplyError...() functions.
 * It emits the protocol for a Redis error, in the form:
 *
 * -ERRORCODE Error Message<CR><LF>
 *
 * If the error code is already passed in the string 's', the error
 * code provided is used, otherwise the string "-ERR " for the generic
 * error code is automatically added.
 * Note that 's' must NOT end with \r\n. */
void addReplyErrorLength(client *c, const char *s, size_t len) {
    /* If the string already starts with "-..." then the error code
     * is provided by the caller. Otherwise we use "-ERR". */
    if (!len || s[0] != '-')
        addReplyProto(c, "-ERR ", 5);
    addReplyProto(c, s, len);
    addReplyProto(c, "\r\n", 2);
}

/* Do some actions after an error reply was sent (Log if needed, updates stats, etc.)
 * Possible flags:
 * * ERR_REPLY_FLAG_NO_STATS_UPDATE - indicate not to update any error stats. */
void afterErrorReply(client *c, const char *s, size_t len, int flags) {
    /* Module clients fall into two categories:
     * Calls to RM_Call, in which case the error isn't being returned to a client, so should not be counted.
     * Module thread safe context calls to RM_ReplyWithError, which will be added to a real client by the main thread later. */
    if (c->flags & CLIENT_MODULE) {
        if (!c->deferred_reply_errors) {
            c->deferred_reply_errors = listCreate();
            listSetFreeMethod(c->deferred_reply_errors, (void (*)(void *))sdsfree);
        }
        listAddNodeTail(c->deferred_reply_errors, sdsnewlen(s, len));
        return;
    }

    if (!(flags & ERR_REPLY_FLAG_NO_STATS_UPDATE)) {
        /* Increment the global error counter */
        server.stat_total_error_replies++;
        /* Increment the error stats
         * If the string already starts with "-..." then the error prefix
         * is provided by the caller ( we limit the search to 32 chars). Otherwise we use "-ERR". */
        if (s[0] != '-') {
            incrementErrorCount("ERR", 3);
        }
        else {
            char *spaceloc = memchr(s, ' ', len < 32 ? len : 32);
            if (spaceloc) {
                const size_t errEndPos = (size_t)(spaceloc - s);
                incrementErrorCount(s + 1, errEndPos - 1);
            }
            else {
                /* Fallback to ERR if we can't retrieve the error prefix */
                incrementErrorCount("ERR", 3);
            }
        }
    }
    else {
        /* stat_total_error_replies will not be updated, which means that
         * the cmd stats will not be updated as well, we still want this command
         * to be counted as failed so we update it here. We update c->realcmd in
         * case c->cmd was changed (like in GEOADD). */
        c->realcmd->failed_calls++;
    }

    /* Sometimes it could be normal that a slave replies to a master with
     * an error and this function gets called. Actually the error will never
     * be sent because addReply*() against master clients has no effect...
     * A notable example is:
     *
     *    EVAL 'redis.call("incr",KEYS[1]); redis.call("nonexisting")' 1 x
     *
     * Where the master must propagate the first change even if the second
     * will produce an error. However it is useful to log such events since
     * they are rare and may hint at errors in a script or a bug in Redis. */
    int ctype = getClientType(c);
    if (ctype == CLIENT_TYPE_MASTER || ctype == CLIENT_TYPE_SLAVE || c->id == CLIENT_ID_AOF) {
        char *to, *from;

        if (c->id == CLIENT_ID_AOF) {
            to = "AOF-loading-client";
            from = "server";
        }
        else if (ctype == CLIENT_TYPE_MASTER) {
            to = "master";
            from = "replica";
        }
        else {
            to = "replica";
            from = "master";
        }

        if (len > 4096)
            len = 4096;
        sds cmdname = c->lastcmd ? c->lastcmd->fullname : NULL;
        serverLog(
            LL_WARNING,
            "== CRITICAL == This %s is sending an error "
            "to its %s: '%.*s' after processing the command "
            "'%s'",
            from, to, (int)len, s, cmdname ? cmdname : "<unknown>");
        if (ctype == CLIENT_TYPE_MASTER && server.repl_backlog && server.repl_backlog->histlen > 0) {
            showLatestBacklog();
        }
        server.stat_unexpected_error_replies++;

        /* Based off the propagation error behavior, check if we need to panic here. There
         * are currently two checked cases:
         * * If this command was from our master and we are not a writable replica.
         * * We are reading from an AOF file. */
        int panic_in_replicas = (ctype == CLIENT_TYPE_MASTER && server.repl_slave_ro) && (server.propagation_error_behavior == PROPAGATION_ERR_BEHAVIOR_PANIC || server.propagation_error_behavior == PROPAGATION_ERR_BEHAVIOR_PANIC_ON_REPLICAS);
        int panic_in_aof = c->id == CLIENT_ID_AOF && server.propagation_error_behavior == PROPAGATION_ERR_BEHAVIOR_PANIC;
        if (panic_in_replicas || panic_in_aof) {
            serverPanic(
                "This %s panicked sending an error to its %s"
                " after processing the command '%s'",
                from, to, cmdname ? cmdname : "<unknown>");
        }
    }
}

/* The 'err' object is expected to start with -ERRORCODE and end with \r\n.
 * Unlike addReplyErrorSds and others alike which rely on addReplyErrorLength. */
void addReplyErrorObject(client *c, robj *err) {
    addReply(c, err);
    afterErrorReply(c, err->ptr, sdslen(err->ptr) - 2, 0); /* Ignore trailing \r\n */
}

/* Sends either a reply or an error reply by checking the first char.
 * If the first char is '-' the reply is considered an error.
 * In any case the given reply is sent, if the reply is also recognize
 * as an error we also perform some post reply operations such as
 * logging and stats update. */
void addReplyOrErrorObject(client *c, robj *reply) {
    serverAssert(sdsEncodedObject(reply));
    sds rep = reply->ptr;
    if (sdslen(rep) > 1 && rep[0] == '-') {
        addReplyErrorObject(c, reply);
    }
    else {
        addReply(c, reply);
    }
}

/* See addReplyErrorLength for expectations from the input string. */
void addReplyError(client *c, const char *err) {
    addReplyErrorLength(c, err, strlen(err));
    afterErrorReply(c, err, strlen(err), 0);
}

/* Add error reply to the given client.
 * Supported flags:
 * * ERR_REPLY_FLAG_NO_STATS_UPDATE - indicate not to perform any error stats updates */
void addReplyErrorSdsEx(client *c, sds err, int flags) {
    addReplyErrorLength(c, err, sdslen(err));
    afterErrorReply(c, err, sdslen(err), flags);
    sdsfree(err);
}

/* See addReplyErrorLength for expectations from the input string. */
/* As a side effect the SDS string is freed. */
void addReplyErrorSds(client *c, sds err) {
    addReplyErrorSdsEx(c, err, 0);
}

/* Internal function used by addReplyErrorFormat and addReplyErrorFormatEx.
 * Refer to afterErrorReply for more information about the flags. */
static void addReplyErrorFormatInternal(client *c, int flags, const char *fmt, va_list ap) {
    va_list cpy;
    va_copy(cpy, ap);
    sds s = sdscatvprintf(sdsempty(), fmt, cpy);
    va_end(cpy);
    /* Trim any newlines at the end (ones will be added by addReplyErrorLength) */
    s = sdstrim(s, "\r\n");
    /* Make sure there are no newlines in the middle of the string, otherwise
     * invalid protocol is emitted. */
    s = sdsmapchars(s, "\r\n", "  ", 2);
    addReplyErrorLength(c, s, sdslen(s));
    afterErrorReply(c, s, sdslen(s), flags);
    sdsfree(s);
}

void addReplyErrorFormatEx(client *c, int flags, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    addReplyErrorFormatInternal(c, flags, fmt, ap);
    va_end(ap);
}

/* See addReplyErrorLength for expectations from the formatted string.
 * The formatted string is safe to contain \r and \n anywhere. */
void addReplyErrorFormat(client *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    addReplyErrorFormatInternal(c, 0, fmt, ap);
    va_end(ap);
}

void addReplyErrorArity(client *c) {
    addReplyErrorFormat(c, "'%s'命令的参数数量错了", c->cmd->fullname);
}

void addReplyErrorExpireTime(client *c) {
    addReplyErrorFormat(c, " '%s'命令无效的过期时间", c->cmd->fullname);
}

void addReplyStatusLength(client *c, const char *s, size_t len) {
    addReplyProto(c, "+", 1);
    addReplyProto(c, s, len);
    addReplyProto(c, "\r\n", 2);
}

void addReplyStatus(client *c, const char *status) {
    addReplyStatusLength(c, status, strlen(status));
}

void addReplyStatusFormat(client *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sds s = sdscatvprintf(sdsempty(), fmt, ap);
    va_end(ap);
    addReplyStatusLength(c, s, sdslen(s));
    sdsfree(s);
}

// 有时我们被迫创建一个新的回复节点,而我们不能追加到前一个,当这种情况发生时,我们想要修剪最后一个回复节点末尾不再使用的未使用空间.
void trimReplyUnusedTailSpace(client *c) {
    listNode *ln = listLast(c->reply);
    clientReplyBlock *tail = ln ? listNodeValue(ln) : NULL;

    /* Note that 'tail' may be NULL even if we have a tail node, because when
     * addReplyDeferredLen() is used */
    if (!tail)
        return;

    /* We only try to trim the space is relatively high (more than a 1/4 of the
     * allocation), otherwise there's a high chance realloc will NOP.
     * Also, to avoid large memmove which happens as part of realloc, we only do
     * that if the used part is small.  */
    if (tail->size - tail->used > tail->size / 4 && tail->used < PROTO_REPLY_CHUNK_BYTES) {
        size_t old_size = tail->size;
        tail = zrealloc(tail, tail->used + sizeof(clientReplyBlock));
        /* take over the allocation's internal fragmentation (at least for
         * memory usage tracking) */
        tail->size = zmalloc_usable_size(tail) - sizeof(clientReplyBlock);
        c->reply_bytes = c->reply_bytes + tail->size - old_size;
        listNodeValue(ln) = tail;
    }
}

// reply 列表中新增一个 dummy 节点, 来占位, 方便后续函数来补充这个数值
void *addReplyDeferredLen(client *c) {
    //  注意,即使对象还没有准备好发送,我们也会在这里安装write事件,因为我们确信在返回事件循环之前会调用setDeferredAggregateLen().
    if (prepareClientToWrite(c) != C_OK)
        return NULL;

    // 复制通常不会引起对应答缓冲区的写操作.如果一个流氓副本在复制链路上发送了一个命令,导致生成了一个回复,我们将简单地断开它.
    // 注意,这是检查添加了响应的命令的最简单方法.复制链接是用来写数据的,而不是用来响应的,所以我们通常不会在复制客户端上到达这里.
    if (getClientType(c) == CLIENT_TYPE_SLAVE) {
        sds cmdname = c->lastcmd ? c->lastcmd->fullname : NULL;
        logInvalidUseAndFreeClientAsync(c, "生成一个回复命令的副本 '%s'", cmdname ? cmdname : "<unknown>");
        return NULL;
    }

    trimReplyUnusedTailSpace(c);
    listAddNodeTail(c->reply, NULL); // 添加一个空值
    return listLast(c->reply);
}

// 查看 next 节点是否有足够的空间来存储 [s, s+len-1], 如果有 将 next 的已有的数据向后移动 len, 复制 [s, s+len-1] 到 next 头部, 移除 dummy 节点
// 否则新建一个 replyBlock, 来存储 [s, s+len-1] 的数据, 将 replyBlock 关联到 dummy 节点上面[不再是 dummy 节点了]
void setDeferredReply(client *c, void *node, const char *s, size_t length) {
    listNode *ln = (listNode *)node;
    clientReplyBlock *next, *prev;

    if (node == NULL)
        return;
    serverAssert(!listNodeValue(ln));

    /* 通常,我们用一个新的缓冲区结构填充这个由addReplyDeferredLen()添加的虚拟NULL节点,该结构包含指定后面数组长度所需的协议.
     * 然而,有时可能在上一个/下一个节点中有空间,所以我们可以删除这个NULL节点,并在它的前面/后面的节点中添加/前缀我们的数据,以节省以后的write(2)系统调用.
     * 所需条件:
     * -prev节点非null,其中有空间或
     * -下一个节点是非null,
     * -已经分配了足够的空间
     * -不要太大(避免大的memmove)
     */
    if (ln->prev != NULL && (prev = listNodeValue(ln->prev)) && prev->size - prev->used > 0) {
        size_t len_to_copy = prev->size - prev->used;
        if (len_to_copy > length)
            len_to_copy = length;
        memcpy(prev->buf + prev->used, s, len_to_copy);
        prev->used += len_to_copy;
        length -= len_to_copy;
        if (length == 0) {
            listDelNode(c->reply, ln);
            return;
        }
        s += len_to_copy;
    }

    if (ln->next != NULL && (next = listNodeValue(ln->next)) && next->size - next->used >= length && next->used < PROTO_REPLY_CHUNK_BYTES * 4) {
        memmove(next->buf + length, next->buf, next->used);
        memcpy(next->buf, s, length);
        next->used += length;
        listDelNode(c->reply, ln);
    }
    else {
        /* Create a new node */
        clientReplyBlock *buf = zmalloc(length + sizeof(clientReplyBlock));
        /* Take over the allocation's internal fragmentation */
        buf->size = zmalloc_usable_size(buf) - sizeof(clientReplyBlock);
        buf->used = length;
        memcpy(buf->buf, s, length);
        listNodeValue(ln) = buf;
        c->reply_bytes += buf->size;

        closeClientOnOutputBufferLimitReached(c, 1);
    }
}

/* Populate the length object and try gluing it to the next chunk. */
// 添加指定前缀 + [s, s+len-1] 的内容到 reply 列表中
void setDeferredAggregateLen(client *c, void *node, long length, char prefix) {
    serverAssert(length >= 0);
    if (node == NULL) {
        return;
    }

    // 像*2\r\n、%3\r\n或~4\r\n这样的东西经常由协议发出,所以如果整数很小,我们就有几个共享对象可以使用,就像大多数时候一样.
    const size_t hdr_len = OBJ_SHARED_HDR_STRLEN(length);
    const int opt_hdr = length < OBJ_SHARED_BULKHDR_LEN; // 32
    if (prefix == '*' && opt_hdr) {
        setDeferredReply(c, node, shared.mbulkhdr[length]->ptr, hdr_len);
        return;
    }
    if (prefix == '%' && opt_hdr) {
        setDeferredReply(c, node, shared.maphdr[length]->ptr, hdr_len);
        return;
    }
    if (prefix == '~' && opt_hdr) {
        setDeferredReply(c, node, shared.sethdr[length]->ptr, hdr_len);
        return;
    }

    char lenstr[128];
    size_t lenstr_len = sprintf(lenstr, "%c%ld\r\n", prefix, length);
    setDeferredReply(c, node, lenstr, lenstr_len);
}

void setDeferredArrayLen(client *c, void *node, long length) {
    setDeferredAggregateLen(c, node, length, '*');
}

// 延迟设置 map 长度
void setDeferredMapLen(client *c, void *node, long length) {
    int prefix = c->resp == 2 ? '*' : '%';
    if (c->resp == 2)
        length *= 2;
    setDeferredAggregateLen(c, node, length, prefix);
}

void setDeferredSetLen(client *c, void *node, long length) {
    int prefix = c->resp == 2 ? '*' : '~';
    setDeferredAggregateLen(c, node, length, prefix);
}

void setDeferredAttributeLen(client *c, void *node, long length) {
    serverAssert(c->resp >= 3);
    setDeferredAggregateLen(c, node, length, '|');
}

void setDeferredPushLen(client *c, void *node, long length) {
    serverAssert(c->resp >= 3);
    setDeferredAggregateLen(c, node, length, '>');
}

/* Add a double as a bulk reply */
void addReplyDouble(client *c, double d) {
    if (isinf(d)) {
        /* Libc in odd systems (Hi Solaris!) will format infinite in a
         * different way, so better to handle it in an explicit way. */
        if (c->resp == 2) {
            addReplyBulkCString(c, d > 0 ? "inf" : "-inf");
        }
        else {
            addReplyProto(c, d > 0 ? ",inf\r\n" : ",-inf\r\n", d > 0 ? 6 : 7);
        }
    }
    else {
        char dbuf[MAX_LONG_DOUBLE_CHARS + 3], sbuf[MAX_LONG_DOUBLE_CHARS + 32];
        int dlen, slen;
        if (c->resp == 2) {
            dlen = snprintf(dbuf, sizeof(dbuf), "%.17g", d);
            slen = snprintf(sbuf, sizeof(sbuf), "$%d\r\n%s\r\n", dlen, dbuf);
            addReplyProto(c, sbuf, slen);
        }
        else {
            dlen = snprintf(dbuf, sizeof(dbuf), ",%.17g\r\n", d);
            addReplyProto(c, dbuf, dlen);
        }
    }
}

void addReplyBigNum(client *c, const char *num, size_t len) {
    if (c->resp == 2) {
        addReplyBulkCBuffer(c, num, len);
    }
    else {
        addReplyProto(c, "(", 1);
        addReplyProto(c, num, len);
        addReply(c, shared.crlf);
    }
}

/* Add a long double as a bulk reply, but uses a human readable formatting
 * of the double instead of exposing the crude behavior of doubles to the
 * dear user. */
void addReplyHumanLongDouble(client *c, long double d) {
    if (c->resp == 2) {
        robj *o = createStringObjectFromLongDouble(d, 1);
        addReplyBulk(c, o);
        decrRefCount(o);
    }
    else {
        char buf[MAX_LONG_DOUBLE_CHARS];
        int len = ld2string(buf, sizeof(buf), d, LD_STR_HUMAN);
        addReplyProto(c, ",", 1);
        addReplyProto(c, buf, len);
        addReplyProto(c, "\r\n", 2);
    }
}

// crlf 回车换行
// 添加一个长整型回复或批量len /multi 多个批量计数. 基本用于输出<prefix><long long><crlf>.
void addReplyLongLongWithPrefix(client *c, long long ll, char prefix) {
    char buf[128];
    int len;
    // dictSize(server.commands) 一般有240个命令
    // 协议经常会回复 $3\r\n或*2\r\n这样的值,所以如果整数很小,我们就有几个共享对象可以使用,就像大多数时候一样.
    const int opt_hdr = ll < OBJ_SHARED_BULKHDR_LEN && ll >= 0; // 0~31
    const size_t hdr_len = OBJ_SHARED_HDR_STRLEN(ll);           // 4290772992 == 11111111110000000000000000000000   小于这个数是4 否则是5

    // opt_hdr 一般为0
    // resp2  '*' ; resp3  '%' ;
    if (prefix == '*' && opt_hdr) { // 数组类型
        addReplyProto(c, shared.mbulkhdr[ll]->ptr, hdr_len);
        return;
    }
    else if (prefix == '$' && opt_hdr) { // 字符串类型
        addReplyProto(c, shared.bulkhdr[ll]->ptr, hdr_len);
        return;
    }
    else if (prefix == '%' && opt_hdr) {
        addReplyProto(c, shared.maphdr[ll]->ptr, hdr_len);
        return;
    }
    else if (prefix == '~' && opt_hdr) {
        addReplyProto(c, shared.sethdr[ll]->ptr, hdr_len);
        return;
    }

    buf[0] = prefix;
    len = ll2string(buf + 1, sizeof(buf) - 1, ll); // long long 转换成字符,480 --> '480' 并写入buf[1:]
    buf[len + 1] = '\r';
    buf[len + 2] = '\n'; // *480\r\n
    addReplyProto(c, buf, len + 3);
}

/*
 * 返回一个整数回复
 *
 * 格式为 :10086\r\n
 */
void addReplyLongLong(client *c, long long ll) {
    if (ll == 0) {
        addReply(c, shared.czero);
    }
    else if (ll == 1) {
        addReply(c, shared.cone);
    }
    else {
        addReplyLongLongWithPrefix(c, ll, ':');
    }
}

// 返回聚合元素的 个数   ,prefix 在resp为2时,返回*  3返回%
void addReplyAggregateLen(client *c, long length, int prefix) {
    serverAssert(length >= 0);
    addReplyLongLongWithPrefix(c, length, prefix);
}

// 返回参数个数
void addReplyArrayLen(client *c, long length) {
    addReplyAggregateLen(c, length, '*');
}

// 返回数组长度
void addReplyMapLen(client *c, long length) {
    int prefix = c->resp == 2 ? '*' : '%';
    if (c->resp == 2) {
        length *= 2; // 所有命令个数的2倍  , 因为每一项 都是由两行内容
    }
    addReplyAggregateLen(c, length, prefix);
}

void addReplySetLen(client *c, long length) {
    int prefix = c->resp == 2 ? '*' : '~';
    addReplyAggregateLen(c, length, prefix);
}

void addReplyAttributeLen(client *c, long length) {
    serverAssert(c->resp >= 3);
    addReplyAggregateLen(c, length, '|');
}

void addReplyPushLen(client *c, long length) {
    serverAssert(c->resp >= 3);
    addReplyAggregateLen(c, length, '>');
}

void addReplyNull(client *c) {
    if (c->resp == 2) {
        addReplyProto(c, "$-1\r\n", 5);
    }
    else {
        addReplyProto(c, "_\r\n", 3);
    }
}

void addReplyBool(client *c, int b) {
    if (c->resp == 2) {
        addReply(c, b ? shared.cone : shared.czero);
    }
    else {
        addReplyProto(c, b ? "#t\r\n" : "#f\r\n", 4);
    }
}

/* A null array is a concept that no longer exists in RESP3. However
 * RESP2 had it, so API-wise we have this call, that will emit the correct
 * RESP2 protocol, however for RESP3 the reply will always be just the
 * Null type "_\r\n". */
void addReplyNullArray(client *c) {
    if (c->resp == 2) {
        addReplyProto(c, "*-1\r\n", 5);
    }
    else {
        addReplyProto(c, "_\r\n", 3);
    }
}

/* Create the length prefix of a bulk reply, example: $2234 */
void addReplyBulkLen(client *c, robj *obj) {
    size_t len = stringObjectLen(obj);

    addReplyLongLongWithPrefix(c, len, '$');
}

// 返回一个 Redis 对象作为回复
void addReplyBulk(client *c, robj *obj) {
    addReplyBulkLen(c, obj);
    addReply(c, obj);
    addReply(c, shared.crlf); // 添加对象'obj'字符串表示到客户端输出缓冲区.
}

// 返回一个 C 缓冲区作为回复   字符
void addReplyBulkCBuffer(client *c, const void *p, size_t len) {
    addReplyLongLongWithPrefix(c, len, '$'); // 命令长度
    addReplyProto(c, p, len);                // 命令
    addReply(c, shared.crlf);                // 添加 \r\n 到客户端输出缓冲区.
}

/* Add sds to reply (takes ownership of sds and frees it) */
void addReplyBulkSds(client *c, sds s) {
    addReplyLongLongWithPrefix(c, sdslen(s), '$');
    addReplySds(c, s);
    addReply(c, shared.crlf); // 添加对象'obj'字符串表示到客户端输出缓冲区.
}

/* Set sds to a deferred reply (for symmetry with addReplyBulkSds it also frees the sds) */
void setDeferredReplyBulkSds(client *c, void *node, sds s) {
    sds reply = sdscatprintf(sdsempty(), "$%d\r\n%s\r\n", (unsigned)sdslen(s), s);
    setDeferredReply(c, node, reply, sdslen(reply));
    sdsfree(reply);
    sdsfree(s);
}

// 返回一个 C 字符串作为回复
void addReplyBulkCString(client *c, const char *s) {
    if (s == NULL) {
        addReplyNull(c);
    }
    else {
        addReplyBulkCBuffer(c, s, strlen(s));
    }
}

// 返回一个 long long 值作为回复
void addReplyBulkLongLong(client *c, long long ll) {
    char buf[64];
    int len;

    len = ll2string(buf, 64, ll);
    addReplyBulkCBuffer(c, buf, len);
}

/* Reply with a verbatim type having the specified extension.
 *
 * The 'ext' is the "extension" of the file, actually just a three
 * character type that describes the format of the verbatim string.
 * For instance "txt" means it should be interpreted as a text only
 * file by the receiver, "md " as markdown, and so forth. Only the
 * three first characters of the extension are used, and if the
 * provided one is shorter than that, the remaining is filled with
 * spaces. */
void addReplyVerbatim(client *c, const char *s, size_t len, const char *ext) {
    if (c->resp == 2) {
        addReplyBulkCBuffer(c, s, len);
    }
    else {
        char buf[32];
        size_t preflen = snprintf(buf, sizeof(buf), "=%zu\r\nxxx:", len + 4);
        char *p = buf + preflen - 4;
        for (int i = 0; i < 3; i++) {
            if (*ext == '\0') {
                p[i] = ' ';
            }
            else {
                p[i] = *ext++;
            }
        }
        addReplyProto(c, buf, preflen);
        addReplyProto(c, s, len);
        addReplyProto(c, "\r\n", 2);
    }
}

/* Add an array of C strings as status replies with a heading.
 * This function is typically invoked by from commands that support
 * subcommands in response to the 'help' subcommand. The help array
 * is terminated by NULL sentinel. */
void addReplyHelp(client *c, const char **help) {
    sds cmd = sdsnew((char *)c->argv[0]->ptr);
    void *blenp = addReplyDeferredLen(c);
    int blen = 0;

    sdstoupper(cmd);
    addReplyStatusFormat(c, "%s <subcommand> [<arg> [value] [opt] ...]. Subcommands are:", cmd);
    sdsfree(cmd);

    while (help[blen]) addReplyStatus(c, help[blen++]);

    addReplyStatus(c, "HELP");
    addReplyStatus(c, "    打印 help.");

    blen += 1; /* Account for the header. */
    blen += 2; /* Account for the footer. */
    setDeferredArrayLen(c, blenp, blen);
}

/* Add a suggestive error reply.
 * This function is typically invoked by from commands that support
 * subcommands in response to an unknown subcommand or argument error. */
void addReplySubcommandSyntaxError(client *c) {
    sds cmd = sdsnew((char *)c->argv[0]->ptr);
    sdstoupper(cmd);
    addReplyErrorFormat(c, "unknown subcommand or wrong number of arguments for '%.128s'. Try %s HELP.", (char *)c->argv[1]->ptr, cmd);
    sdsfree(cmd);
}

/* Append 'src' client output buffers into 'dst' client output buffers.
 * This function clears the output buffers of 'src' */
void AddReplyFromClient(client *dst, client *src) {
    /* If the source client contains a partial response due to client output
     * buffer limits, propagate that to the dest rather than copy a partial
     * reply. We don't wanna run the risk of copying partial response in case
     * for some reason the output limits don't reach the same decision (maybe
     * they changed) */
    if (src->flags & CLIENT_CLOSE_ASAP) {
        sds client = catClientInfoString(sdsempty(), dst);
        freeClientAsync(dst);
        serverLog(LL_WARNING, "Client %s scheduled to be closed ASAP for overcoming of output buffer limits.", client);
        sdsfree(client);
        return;
    }

    /* First add the static buffer (either into the static buffer or reply list) */
    addReplyProto(dst, src->buf, src->bufpos);

    /* We need to check with prepareClientToWrite again (after addReplyProto)
     * since addReplyProto may have changed something (like CLIENT_CLOSE_ASAP) */
    if (prepareClientToWrite(dst) != C_OK)
        return;

    /* We're bypassing _addReplyProtoToList, so we need to add the pre/post
     * checks in it. */
    if (dst->flags & CLIENT_CLOSE_AFTER_REPLY)
        return;

    /* Concatenate the reply list into the dest */
    if (listLength(src->reply))
        listJoin(dst->reply, src->reply);
    dst->reply_bytes += src->reply_bytes;
    src->reply_bytes = 0;
    src->bufpos = 0;

    if (src->deferred_reply_errors) {
        deferredAfterErrorReply(dst, src->deferred_reply_errors);
        listRelease(src->deferred_reply_errors);
        src->deferred_reply_errors = NULL;
    }

    /* Check output buffer limits */
    closeClientOnOutputBufferLimitReached(dst, 1);
}

/* Append the listed errors to the server error statistics. the input
 * list is not modified and remains the responsibility of the caller. */
void deferredAfterErrorReply(client *c, list *errors) {
    listIter li;
    listNode *ln;
    listRewind(errors, &li);
    while ((ln = listNext(&li))) {
        sds err = ln->value;
        afterErrorReply(c, err, sdslen(err), 0);
    }
}

/* Logically copy 'src' replica client buffers info to 'dst' replica.
 * Basically increase referenced buffer block node reference count. */
void copyReplicaOutputBuffer(client *dst, client *src) {
    serverAssert(src->bufpos == 0 && listLength(src->reply) == 0);

    if (src->ref_repl_buf_node == NULL)
        return;
    dst->ref_repl_buf_node = src->ref_repl_buf_node;
    dst->ref_block_pos = src->ref_block_pos;
    ((replBufBlock *)listNodeValue(dst->ref_repl_buf_node))->refcount++;
}

/* Return true if the specified client has pending reply buffers to write to
 * the socket. */
int clientHasPendingReplies(client *c) {
    if (getClientType(c) == CLIENT_TYPE_SLAVE) {
        /* Replicas use global shared replication buffer instead of
         * private output buffer. */
        serverAssert(c->bufpos == 0 && listLength(c->reply) == 0);
        if (c->ref_repl_buf_node == NULL)
            return 0;

        /* If the last replication buffer block content is totally sent,
         * we have nothing to send. */
        listNode *ln = listLast(server.repl_buffer_blocks);
        replBufBlock *tail = listNodeValue(ln);
        if (ln == c->ref_repl_buf_node && c->ref_block_pos == tail->used)
            return 0;

        return 1;
    }
    else {
        return c->bufpos || listLength(c->reply);
    }
}

// 返回 客户端 是不是使用使用的127.0.0.1  ::1
int islocalClient(client *c) {
    /* unix-socket */
    if (c->flags & CLIENT_UNIX_SOCKET)
        return 1;

    /* tcp */
    char cip[NET_IP_STR_LEN + 1] = {0}; // 客户端IP
    connPeerToString(c->conn, cip, sizeof(cip) - 1, NULL);

    return !strcmp(cip, "127.0.0.1") || !strcmp(cip, "::1"); // 是127.0.0.1 是 ::1  返回true  其余都返回false
}

// 建立客户端链接要干的一部分活
void clientAcceptHandler(connection *conn) {
    client *c = connGetPrivateData(conn); // 拿到client结构体

    if (connGetState(conn) != CONN_STATE_CONNECTED) {
        serverLog(LL_WARNING, "接受客户端连接错误: %s", connGetLastError(conn));
        freeClientAsync(c);
        return;
    }

    // 如果服务器运行在保护模式(默认),并且没有设置密码,
    if (server.protected_mode && DefaultUser->flags & USER_FLAG_NOPASS) { // protected_mode默认为1;

        // 也没有绑定特定的接口,我们不会接受来自非环回接口的请求.
        // 相反,如果需要的话,我们会向用户解释如何修复它.
        if (!islocalClient(c)) {
            char *err =
                "-DENIED Redis运行在保护模式,因为保护模式已经开启,默认用户没有设置密码.在这种模式下,只接受来自loopback接口的连接.如果您想从外部计算机连接到Redis,您可以采用以下解决方案之一:\n"
                "1)只要禁用保护模式发送命令'CONFIG SET protected-mode no'从环回接口连接到服务器正在运行的同一主机,但要确保Redis不是公开从互联网访问,如果你这样做.使用CONFIG REWRITE使此更改永久生效.\n"
                "2)或者你可以通过编辑Redis的配置文件来禁用保护模式,并将保护模式选项设置为“no”,然后重新启动服务器.\n"
                "3)如果你只是为了测试而手动启动服务器,用“--protected-mode no”选项重新启动它.\n"
                "4)为默认用户设置认证密码.注意:为了让服务器开始接受来自外部的连接,您只需要做上述的一件事.\n";

            if (connWrite(c->conn, err, strlen(err)) == -1) { /* 没什么可做的,只是为了避开警告… */
            }
            server.stat_rejected_conn++;
            freeClientAsync(c);
            return;
        }
    }
    server.stat_numconnections++;
    // client 发生变化事件
    moduleFireServerEvent(REDISMODULE_EVENT_CLIENT_CHANGE, REDISMODULE_SUBEVENT_CLIENT_CHANGE_CONNECTED, c); // 事件通知
}

#define MAX_ACCEPTS_PER_CALL 1000 // 一次调用epoll wait,最多创建1000个链接       TODO 超过1000的事件怎么处理

// 接受客户端连接,并创建已连接套接字cfd
static void acceptCommonHandler(connection *conn, int flags, char *ip) {
    //  flags 套接字类型,目前包含tcp:0,unix_socket:1 << 11
    client *c;
    char conninfo[100];
    UNUSED(ip);

    if (connGetState(conn) != CONN_STATE_ACCEPTING) {
        serverLog(LL_VERBOSE, "接受的客户端连接处于错误状态: %s (conn: %s)", connGetLastError(conn), connGetInfo(conn, conninfo, sizeof(conninfo)));
        connClose(conn);
        return;
    }

    // 限制我们在同一时间连接的次数.
    // 允许控制将在创建客户端和调用connAccept()之前发生,因为如果被拒绝,我们甚至不希望开始传输层协商.
    if (listLength(server.clients) + getClusterConnectionsCount() >= server.maxclients) {
        char *err;
        if (server.cluster_enabled)
            err = "-ERR 已到达最大的客户端链接数[cluster+client]\r\n";
        else
            err = "-ERR 已到达最大的客户端链接数[client]\r\n";

        //      这是最好的错误消息,不要检查写错误.注意,对于TLS连接,还没有完成握手,所以没有写入任何内容,连接将被删除.
        if (connWrite(conn, err, strlen(err)) == -1) { // 往客户端写入错误信息
        }
        server.stat_rejected_conn++;
        connClose(conn);
        return;
    }

    // 创建链接、客户端,以及将client加入前缀树
    if ((c = createClient(conn)) == NULL) { // ✅
        serverLog(LL_WARNING, "为新客户端注册fd事件时出错: %s (conn: %s)", connGetLastError(conn), connGetInfo(conn, conninfo, sizeof(conninfo)));
        connClose(conn); /*可能已经关闭,只是忽略错误*/
        return;
    }

    // 保留最新的标志
    c->flags |= flags; // 套接字类型

    if (connAccept(conn, clientAcceptHandler) == C_ERR) { // 只要是执行clientAcceptHandler
        char conninfo[100];
        if (connGetState(conn) == CONN_STATE_ERROR)
            serverLog(LL_WARNING, "接受客户端连接时出错: %s (conn: %s)", connGetLastError(conn), connGetInfo(conn, conninfo, sizeof(conninfo)));
        freeClient(connGetPrivateData(conn));
        return;
    }
}

// 当有tcp链接到来时,会调用 .用于对连接服务监听套接字的客户端进行应答 与其AE_READABLE事件相关联,epoll 事件 回调
void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    // 连接
    int cport = MAX_ACCEPTS_PER_CALL;
    int cfd = MAX_ACCEPTS_PER_CALL;
    int max = MAX_ACCEPTS_PER_CALL; // 一次调用epoll wait,最多创建1000个链接
    char cip[NET_IP_STR_LEN];       // ip 地址,最长为46
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    while (max--) {
        // neterr 传进入指针,存储错误,
        // fd 一般是listen监听的描述符
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport); // 还没加入到epoll里
        // 获取不到链接 会返回错误
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK) {
                serverLog(LL_WARNING, "接收客户端连接...: %s", server.neterr);
            }
            return;
        }
        serverLog(LL_VERBOSE, "接收客户端连接 %s:%d,fd编号%d", cip, cport, cfd); //
        connection *c = connCreateAcceptedSocket(cfd);                           // 组装成内部的connection结构体
        acceptCommonHandler(c, 0, cip);                                          // 接受客户端连接,并创建已连接套接字cfd
    }
}

void acceptTLSHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd, max = MAX_ACCEPTS_PER_CALL;
    char cip[NET_IP_STR_LEN];
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    while (max--) {
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                serverLog(LL_WARNING, "Accepting client connection: %s", server.neterr);
            return;
        }
        serverLog(LL_VERBOSE, "Accepted %s:%d", cip, cport);
        acceptCommonHandler(connCreateAcceptedTLS(cfd, server.tls_auth_clients), 0, cip);
    }
}

void acceptUnixHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cfd, max = MAX_ACCEPTS_PER_CALL;
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    while (max--) {
        cfd = anetUnixAccept(server.neterr, fd);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                serverLog(LL_WARNING, "Accepting client connection: %s", server.neterr);
            return;
        }
        serverLog(LL_VERBOSE, "Accepted connection to %s", server.unixsocket);
        acceptCommonHandler(connCreateAcceptedSocket(cfd), CLIENT_UNIX_SOCKET, NULL);
    }
}

// 释放original_argv变量占用的内存
void freeClientOriginalArgv(client *c) {
    if (!c->original_argv) {
        return;
    }

    for (int j = 0; j < c->original_argc; j++) {
        decrRefCount(c->original_argv[j]);
    }
    zfree(c->original_argv);
    c->original_argv = NULL;
    c->original_argc = 0;
}

void freeClientArgv(client *c) {
    int j;
    for (j = 0; j < c->argc; j++) decrRefCount(c->argv[j]);
    c->argc = 0;
    c->cmd = NULL;
    c->argv_len_sum = 0;
    c->argv_len = 0;
    zfree(c->argv);
    c->argv = NULL;
}

/* Close all the slaves connections. This is useful in chained replication
 * when we resync with our own master and want to force all our slaves to
 * resync with us as well. */
void disconnectSlaves(void) {
    listIter li;
    listNode *ln;
    listRewind(server.slaves, &li);
    while ((ln = listNext(&li))) {
        freeClient((client *)ln->value);
    }
}

/* Check if there is any other slave waiting dumping RDB finished expect me.
 * This function is useful to judge current dumping RDB can be used for full
 * synchronization or not. */
int anyOtherSlaveWaitRdb(client *except_me) {
    listIter li;
    listNode *ln;

    listRewind(server.slaves, &li);
    while ((ln = listNext(&li))) {
        client *slave = ln->value;
        if (slave != except_me && slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END) {
            return 1;
        }
    }
    return 0;
}

/* Remove the specified client from global lists where the client could
 * be referenced, not including the Pub/Sub channels.
 * This is used by freeClient() and replicationCacheMaster(). */
void unlinkClient(client *c) {
    listNode *ln;

    /* If this is marked as current client unset it. */
    if (server.current_client == c)
        server.current_client = NULL;

    /* Certain operations must be done only if the client has an active connection.
     * If the client was already unlinked or if it's a "fake client" the
     * conn is already set to NULL. */
    if (c->conn) {
        /* Remove from the list of active clients. */
        if (c->client_list_node) {
            uint64_t id = htonu64(c->id);
            raxRemove(server.clients_index, (unsigned char *)&id, sizeof(id), NULL);
            listDelNode(server.clients, c->client_list_node);
            c->client_list_node = NULL;
        }

        /* Check if this is a replica waiting for diskless replication (rdb pipe),
         * in which case it needs to be cleaned from that list */
        if (c->flags & CLIENT_SLAVE && c->replstate == SLAVE_STATE_WAIT_BGSAVE_END && server.rdb_pipe_conns) {
            int i;
            for (i = 0; i < server.rdb_pipe_numconns; i++) {
                if (server.rdb_pipe_conns[i] == c->conn) {
                    rdbPipeWriteHandlerConnRemoved(c->conn);
                    server.rdb_pipe_conns[i] = NULL;
                    break;
                }
            }
        }
        connClose(c->conn);
        c->conn = NULL;
    }

    /* Remove from the list of pending writes if needed. */
    if (c->flags & CLIENT_PENDING_WRITE) {
        ln = listSearchKey(server.clients_pending_write, c);
        serverAssert(ln != NULL);
        listDelNode(server.clients_pending_write, ln);
        c->flags &= ~CLIENT_PENDING_WRITE;
    }

    /* Remove from the list of pending reads if needed. */
    serverAssert(io_threads_op == IO_THREADS_OP_IDLE);
    if (c->pending_read_list_node != NULL) {
        listDelNode(server.clients_pending_read, c->pending_read_list_node);
        c->pending_read_list_node = NULL;
    }

    /* When client was just unblocked because of a blocking operation,
     * remove it from the list of unblocked clients. */
    if (c->flags & CLIENT_UNBLOCKED) {
        ln = listSearchKey(server.unblocked_clients, c);
        serverAssert(ln != NULL);
        listDelNode(server.unblocked_clients, ln);
        c->flags &= ~CLIENT_UNBLOCKED;
    }

    /* Clear the tracking status. */
    if (c->flags & CLIENT_TRACKING)
        disableTracking(c);
}

/* Clear the client state to resemble a newly connected client. */
void clearClientConnectionState(client *c) {
    listNode *ln;

    /* MONITOR clients are also marked with CLIENT_SLAVE, we need to
     * distinguish between the two.
     */
    if (c->flags & CLIENT_MONITOR) {
        ln = listSearchKey(server.monitors, c);
        serverAssert(ln != NULL);
        listDelNode(server.monitors, ln);

        c->flags &= ~(CLIENT_MONITOR | CLIENT_SLAVE);
    }

    serverAssert(!(c->flags & (CLIENT_SLAVE | CLIENT_MASTER)));

    if (c->flags & CLIENT_TRACKING)
        disableTracking(c);
    selectDb(c, 0);
    c->resp = 2;

    clientSetDefaultAuth(c);
    moduleNotifyUserChanged(c);
    discardTransaction(c);

    pubsubUnsubscribeAllChannels(c, 0);
    pubsubUnsubscribeShardAllChannels(c, 0);
    pubsubUnsubscribeAllPatterns(c, 0);

    if (c->name) {
        decrRefCount(c->name);
        c->name = NULL;
    }

    /* Selectively clear state flags not covered above */
    c->flags &= ~(CLIENT_ASKING | CLIENT_READONLY | CLIENT_PUBSUB | CLIENT_REPLY_OFF | CLIENT_REPLY_SKIP_NEXT);
}

// 释放客户端
void freeClient(client *c) {
    listNode *ln;

    /* 如果客户端受到保护,但我们现在需要释放它,请确保至少使用异步释放.*/
    if (c->flags & CLIENT_PROTECTED) {
        freeClientAsync(c);
        return;
    }

    /* For connected clients, call the disconnection event of modules hooks. */
    if (c->conn) {
        moduleFireServerEvent(REDISMODULE_EVENT_CLIENT_CHANGE, REDISMODULE_SUBEVENT_CLIENT_CHANGE_DISCONNECTED, c);
    }

    /* Notify module system that this client auth status changed. */
    moduleNotifyUserChanged(c);

    /* If this client was scheduled for async freeing we need to remove it
     * from the queue. Note that we need to do this here, because later
     * we may call replicationCacheMaster() and the client should already
     * be removed from the list of clients to free. */
    if (c->flags & CLIENT_CLOSE_ASAP) {
        ln = listSearchKey(server.clients_to_close, c);
        serverAssert(ln != NULL);
        listDelNode(server.clients_to_close, ln);
    }

    /* If it is our master that's being disconnected we should make sure
     * to cache the state to try a partial resynchronization later.
     *
     * Note that before doing this we make sure that the client is not in
     * some unexpected state, by checking its flags. */
    if (server.master && c->flags & CLIENT_MASTER) {
        serverLog(LL_WARNING, "Connection with master lost.");
        if (!(c->flags & (CLIENT_PROTOCOL_ERROR | CLIENT_BLOCKED))) {
            c->flags &= ~(CLIENT_CLOSE_ASAP | CLIENT_CLOSE_AFTER_REPLY);
            replicationCacheMaster(c);
            return;
        }
    }

    /* Log link disconnection with slave */
    if (getClientType(c) == CLIENT_TYPE_SLAVE) {
        serverLog(LL_WARNING, "Connection with replica %s lost.", replicationGetSlaveName(c));
    }

    /* Free the query buffer */
    sdsfree(c->querybuf);
    c->querybuf = NULL;

    /* Deallocate structures used to block on blocking ops. */
    if (c->flags & CLIENT_BLOCKED)
        unblockClient(c);
    dictRelease(c->bpop.keys);

    /* UNWATCH all the keys */
    unwatchAllKeys(c);
    listRelease(c->watched_keys);

    /* Unsubscribe from all the pubsub channels */
    pubsubUnsubscribeAllChannels(c, 0);
    pubsubUnsubscribeShardAllChannels(c, 0);
    pubsubUnsubscribeAllPatterns(c, 0);
    dictRelease(c->pubsub_channels);
    listRelease(c->pubsub_patterns);
    dictRelease(c->pubsubshard_channels);

    /* Free data structures. */
    listRelease(c->reply);
    zfree(c->buf);
    freeReplicaReferencedReplBuffer(c);
    freeClientArgv(c);
    freeClientOriginalArgv(c);
    if (c->deferred_reply_errors)
        listRelease(c->deferred_reply_errors);

    /* Unlink the client: this will close the socket, remove the I/O
     * handlers, and remove references of the client from different
     * places where active clients may be referenced. */
    unlinkClient(c);

    /* Master/slave cleanup Case 1:
     * we lost the connection with a slave. */
    if (c->flags & CLIENT_SLAVE) {
        /* If there is no any other slave waiting dumping RDB finished, the
         * current child process need not continue to dump RDB, then we kill it.
         * So child process won't use more memory, and we also can fork a new
         * child process asap to dump rdb for next full synchronization or bgsave.
         * But we also need to check if users enable 'save' RDB, if enable, we
         * should not remove directly since that means RDB is important for users
         * to keep data safe and we may delay configured 'save' for full sync. */
        if (server.saveparamslen == 0 && c->replstate == SLAVE_STATE_WAIT_BGSAVE_END && server.child_type == CHILD_TYPE_RDB && server.rdb_child_type == RDB_CHILD_TYPE_DISK && anyOtherSlaveWaitRdb(c) == 0) {
            killRDBChild();
        }
        if (c->replstate == SLAVE_STATE_SEND_BULK) {
            if (c->repldbfd != -1)
                close(c->repldbfd);
            if (c->replpreamble)
                sdsfree(c->replpreamble);
        }
        list *l = (c->flags & CLIENT_MONITOR) ? server.monitors : server.slaves;
        ln = listSearchKey(l, c);
        serverAssert(ln != NULL);
        listDelNode(l, ln);
        /* We need to remember the time when we started to have zero
         * attached slaves, as after some time we'll free the replication
         * backlog. */
        if (getClientType(c) == CLIENT_TYPE_SLAVE && listLength(server.slaves) == 0)
            server.repl_no_slaves_since = server.unixtime;
        refreshGoodSlavesCount();
        /* Fire the replica change modules event. */
        if (c->replstate == SLAVE_STATE_ONLINE)
            moduleFireServerEvent(REDISMODULE_EVENT_REPLICA_CHANGE, REDISMODULE_SUBEVENT_REPLICA_CHANGE_OFFLINE, NULL);
    }

    /* Master/slave cleanup Case 2:
     * we lost the connection with the master. */
    if (c->flags & CLIENT_MASTER)
        replicationHandleMasterDisconnection();

    /* Remove the contribution that this client gave to our
     * incrementally computed memory usage. */
    server.stat_clients_type_memory[c->last_memory_type] -= c->last_memory_usage;
    /* Remove client from memory usage buckets */
    if (c->mem_usage_bucket) {
        c->mem_usage_bucket->mem_usage_sum -= c->last_memory_usage;
        listDelNode(c->mem_usage_bucket->clients, c->mem_usage_bucket_node);
    }

    /* Release other dynamically allocated client structure fields,
     * and finally release the client structure itself. */
    if (c->name)
        decrRefCount(c->name);
    freeClientMultiState(c);
    sdsfree(c->peerid);
    sdsfree(c->sockname);
    sdsfree(c->slave_addr);
    zfree(c);
}

/* 在serverCron()函数中安排客户机在安全时间释放它.
 * 当我们需要终止客户端,但在不可能调用freeClient()的情况下,这个函数很有用,因为客户端应该对程序流的继续有效.*/
void freeClientAsync(client *c) {
    /*我们需要处理并发访问服务器.clients_to_close列表只在freeClientAsync()函数中,因为它是唯一的函数,可以访问列表,
     * 而Redis使用I/O线程.当其他线程空闲时,所有其他访问都在主线程的上下文中.*/

    // CLIENT_CLOSE_ASAP (1 << 10)         // 表示客户端的输出缓冲区大小超出了服务器允许的范围,会在下一次serverCron时关闭这个客户端,
    // 积存在输出缓冲区中的所有内容会直接被释放,不会返回客户端
    // CLIENT_SCRIPT (1 << 8)              /* 这是lua使用的非连接 客户端 */
    if (c->flags & CLIENT_CLOSE_ASAP || c->flags & CLIENT_SCRIPT) {
        return;
    }
    c->flags |= CLIENT_CLOSE_ASAP;
    if (server.io_threads_num == 1) {
        /* 如果只有一个线程(主线程),就不需要锁了*/
        listAddNodeTail(server.clients_to_close, c);
        return;
    }
    // PTHREAD_MUTEX_INITIALIZER 用在静态类型的互斥量中,
    // 而且应该在互斥量定义的时候就用 PTHREAD_MUTEX_INITIALIZER 进行初始化,否则用 pthread_mutex_init 进行初始化.
    static pthread_mutex_t async_free_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&async_free_queue_mutex); // 加锁
    listAddNodeTail(server.clients_to_close, c);
    pthread_mutex_unlock(&async_free_queue_mutex); // 解锁
}

/* Log errors for invalid use and free the client in async way.
 * We will add additional information about the client to the message. */
void logInvalidUseAndFreeClientAsync(client *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sds info = sdscatvprintf(sdsempty(), fmt, ap);
    va_end(ap);

    sds client = catClientInfoString(sdsempty(), c);
    serverLog(LL_WARNING, "%s, disconnecting it: %s", info, client);

    sdsfree(info);
    sdsfree(client);
    freeClientAsync(c);
}

/* 在处理下一个客户端之前执行客户端处理,这对于执行影响全局状态的操作很有用,但不能等到我们处理完所有客户端.
 * 换句话说,不能等到beforeSleep()返回C_ERR,以防客户端在调用后不再有效.
 * 输入客户端参数:c,如果在调用之前释放了前一个客户端,则可以为NULL.*/
int beforeNextClient(client *c) {
    /* 如果在IO线程中,则跳过客户端处理,在这种情况下,我们将在线程机制的扇入阶段执行此操作(此函数将再次调用) */
    if (io_threads_op != IO_THREADS_OP_IDLE)
        return C_OK;

    // 应该是在 IO_THREADS_OP_IDLE 情况下才执行
    if (c && (c->flags & CLIENT_CLOSE_ASAP)) {
        freeClient(c);
        return C_ERR;
    }
    return C_OK;
}

/* Free the clients marked as CLOSE_ASAP, return the number of clients
 * freed. */
int freeClientsInAsyncFreeQueue(void) {
    int freed = 0;
    listIter li;
    listNode *ln;

    listRewind(server.clients_to_close, &li);
    while ((ln = listNext(&li)) != NULL) {
        client *c = listNodeValue(ln);

        if (c->flags & CLIENT_PROTECTED)
            continue;

        c->flags &= ~CLIENT_CLOSE_ASAP;
        freeClient(c);
        listDelNode(server.clients_to_close, ln);
        freed++;
    }
    return freed;
}

/* Return a client by ID, or NULL if the client ID is not in the set
 * of registered clients. Note that "fake clients", created with -1 as FD,
 * are not registered clients. */
client *lookupClientByID(uint64_t id) {
    id = htonu64(id);
    client *c = raxFind(server.clients_index, (unsigned char *)&id, sizeof(id));
    return (c == raxNotFound) ? NULL : c;
}

/* This function should be called from _writeToClient when the reply list is not empty,
 * it gathers the scattered buffers from reply list and sends them away with connWritev.
 * If we write successfully, it returns C_OK, otherwise, C_ERR is returned,
 * and 'nwritten' is an output parameter, it means how many bytes server write
 * to client. */
static int _writevToClient(client *c, ssize_t *nwritten) {
    struct iovec iov[IOV_MAX];
    int iovcnt = 0;
    size_t iov_bytes_len = 0;
    /* If the static reply buffer is not empty,
     * add it to the iov array for writev() as well. */
    if (c->bufpos > 0) {
        iov[iovcnt].iov_base = c->buf + c->sentlen;
        iov[iovcnt].iov_len = c->bufpos - c->sentlen;
        iov_bytes_len += iov[iovcnt++].iov_len;
    }
    /* The first node of reply list might be incomplete from the last call,
     * thus it needs to be calibrated to get the actual data address and length. */
    size_t offset = c->bufpos > 0 ? 0 : c->sentlen;
    listIter iter;
    listNode *next;
    clientReplyBlock *o;
    listRewind(c->reply, &iter);
    while ((next = listNext(&iter)) && iovcnt < IOV_MAX && iov_bytes_len < NET_MAX_WRITES_PER_EVENT) {
        o = listNodeValue(next);
        if (o->used == 0) { /* empty node, just release it and skip. */
            c->reply_bytes -= o->size;
            listDelNode(c->reply, next);
            offset = 0;
            continue;
        }

        iov[iovcnt].iov_base = o->buf + offset;
        iov[iovcnt].iov_len = o->used - offset;
        iov_bytes_len += iov[iovcnt++].iov_len;
        offset = 0;
    }
    if (iovcnt == 0)
        return C_OK;
    *nwritten = connWritev(c->conn, iov, iovcnt);
    if (*nwritten <= 0)
        return C_ERR;

    /* Locate the new node which has leftover data and
     * release all nodes in front of it. */
    ssize_t remaining = *nwritten;
    if (c->bufpos > 0) { /* deal with static reply buffer first. */
        int buf_len = c->bufpos - c->sentlen;
        c->sentlen += remaining;
        /* If the buffer was sent, set bufpos to zero to continue with
         * the remainder of the reply. */
        if (remaining >= buf_len) {
            c->bufpos = 0;
            c->sentlen = 0;
        }
        remaining -= buf_len;
    }
    listRewind(c->reply, &iter);
    while (remaining > 0) {
        next = listNext(&iter);
        o = listNodeValue(next);
        if (remaining < (ssize_t)(o->used - c->sentlen)) {
            c->sentlen += remaining;
            break;
        }
        remaining -= (ssize_t)(o->used - c->sentlen);
        c->reply_bytes -= o->size;
        listDelNode(c->reply, next);
        c->sentlen = 0;
    }

    return C_OK;
}

/* This function does actual writing output buffers to different types of
 * clients, it is called by writeToClient.
 * If we write successfully, it returns C_OK, otherwise, C_ERR is returned,
 * and 'nwritten' is an output parameter, it means how many bytes server write
 * to client. */
int _writeToClient(client *c, ssize_t *nwritten) {
    *nwritten = 0;
    if (getClientType(c) == CLIENT_TYPE_SLAVE) {
        serverAssert(c->bufpos == 0 && listLength(c->reply) == 0);

        replBufBlock *o = listNodeValue(c->ref_repl_buf_node);
        serverAssert(o->used >= c->ref_block_pos);
        /* Send current block if it is not fully sent. */
        if (o->used > c->ref_block_pos) {
            *nwritten = connWrite(c->conn, o->buf + c->ref_block_pos, o->used - c->ref_block_pos);
            if (*nwritten <= 0)
                return C_ERR;
            c->ref_block_pos += *nwritten;
        }

        /* If we fully sent the object on head, go to the next one. */
        listNode *next = listNextNode(c->ref_repl_buf_node);
        if (next && c->ref_block_pos == o->used) {
            o->refcount--;
            ((replBufBlock *)(listNodeValue(next)))->refcount++;
            c->ref_repl_buf_node = next;
            c->ref_block_pos = 0;
            incrementalTrimReplicationBacklog(REPL_BACKLOG_TRIM_BLOCKS_PER_CALL);
        }
        return C_OK;
    }

    /* When the reply list is not empty, it's better to use writev to save us some
     * system calls and TCP packets. */
    if (listLength(c->reply) > 0) {
        int ret = _writevToClient(c, nwritten);
        if (ret != C_OK)
            return ret;

        /* If there are no longer objects in the list, we expect
         * the count of reply bytes to be exactly zero. */
        if (listLength(c->reply) == 0)
            serverAssert(c->reply_bytes == 0);
    }
    else if (c->bufpos > 0) {
        *nwritten = connWrite(c->conn, c->buf + c->sentlen, c->bufpos - c->sentlen);
        if (*nwritten <= 0)
            return C_ERR;
        c->sentlen += *nwritten;

        /* If the buffer was sent, set bufpos to zero to continue with
         * the remainder of the reply. */
        if ((int)c->sentlen == c->bufpos) {
            c->bufpos = 0;
            c->sentlen = 0;
        }
    }

    return C_OK;
}

/* Write data in output buffers to client. Return C_OK if the client
 * is still valid after the call, C_ERR if it was freed because of some
 * error.  If handler_installed is set, it will attempt to clear the
 * write event.
 *
 * This function is called by threads, but always with handler_installed
 * set to 0. So when handler_installed is set to 0 the function must be
 * thread safe. */
int writeToClient(client *c, int handler_installed) {
    /* Update total number of writes on server */
    atomicIncr(server.stat_total_writes_processed, 1);

    ssize_t nwritten = 0, totwritten = 0;

    while (clientHasPendingReplies(c)) {
        int ret = _writeToClient(c, &nwritten);
        if (ret == C_ERR)
            break;
        totwritten += nwritten;
        /* Note that we avoid to send more than NET_MAX_WRITES_PER_EVENT
         * bytes, in a single threaded server it's a good idea to serve
         * other clients as well, even if a very large request comes from
         * super fast link that is always able to accept data (in real world
         * scenario think about 'KEYS *' against the loopback interface).
         *
         * However if we are over the maxmemory limit we ignore that and
         * just deliver as much data as it is possible to deliver.
         *
         * Moreover, we also send as much as possible if the client is
         * a slave or a monitor (otherwise, on high-speed traffic, the
         * replication/output buffer will grow indefinitely) */
        if (totwritten > NET_MAX_WRITES_PER_EVENT && (server.maxmemory == 0 || zmalloc_used_memory() < server.maxmemory) && !(c->flags & CLIENT_SLAVE))
            break;
    }
    atomicIncr(server.stat_net_output_bytes, totwritten);
    if (nwritten == -1) {
        if (connGetState(c->conn) != CONN_STATE_CONNECTED) {
            serverLog(LL_VERBOSE, "Error writing to client: %s", connGetLastError(c->conn));
            freeClientAsync(c);
            return C_ERR;
        }
    }
    if (totwritten > 0) {
        /* For clients representing masters we don't count sending data
         * as an interaction, since we always send REPLCONF ACK commands
         * that take some time to just fill the socket output buffer.
         * We just rely on data / pings received for timeout detection. */
        if (!(c->flags & CLIENT_MASTER))
            c->lastinteraction = server.unixtime;
    }
    if (!clientHasPendingReplies(c)) {
        c->sentlen = 0;
        /* Note that writeToClient() is called in a threaded way, but
         * aeDeleteFileEvent() is not thread safe: however writeToClient()
         * is always called with handler_installed set to 0 from threads
         * so we are fine. */
        if (handler_installed) {
            serverAssert(io_threads_op == IO_THREADS_OP_IDLE);
            connSetWriteHandler(c->conn, NULL);
        }

        /* Close connection after entire reply has been sent. */
        if (c->flags & CLIENT_CLOSE_AFTER_REPLY) {
            freeClientAsync(c);
            return C_ERR;
        }
    }
    /* Update client's memory usage after writing.
     * Since this isn't thread safe we do this conditionally. In case of threaded writes this is done in
     * handleClientsWithPendingWritesUsingThreads(). */
    if (io_threads_op == IO_THREADS_OP_IDLE)
        updateClientMemUsage(c);
    return C_OK;
}

// 负责将服务器质性命令后得到的命令回复通过套接字返回给客户端  AE_WRITABLE
void sendReplyToClient(connection *conn) {
    client *c = connGetPrivateData(conn);
    writeToClient(c, 1);
}

/* This function is called just before entering the event loop, in the hope
 * we can just write the replies to the client output buffer without any
 * need to use a syscall in order to install the writable event handler,
 * get it called, and so forth. */
int handleClientsWithPendingWrites(void) {
    listIter li;
    listNode *ln;
    int processed = listLength(server.clients_pending_write);
    // 获取待写回的客户端列表

    listRewind(server.clients_pending_write, &li);
    // 遍历每一个待写回的客户端

    while ((ln = listNext(&li))) {
        // 遍历server.clients_pending_write链表,对链表中的每个client执行写数据操作
        client *c = listNodeValue(ln);
        c->flags &= ~CLIENT_PENDING_WRITE;
        listDelNode(server.clients_pending_write, ln);

        /* If a client is protected, don't do anything,
         * that may trigger write error or recreate handler. */
        if (c->flags & CLIENT_PROTECTED)
            continue;

        /* Don't write to clients that are going to be closed anyway. */
        if (c->flags & CLIENT_CLOSE_ASAP)
            continue;
        // 调用writeToClient将当前客户端的输出缓冲区数据写回

        if (writeToClient(c, 0) == C_ERR)
            continue;

        // 如果还有待写回数据

        if (clientHasPendingReplies(c)) {
            installClientWriteHandler(c);
        }
    }
    return processed;
}

// 在客户端执行完命令之后执行：重置客户端以准备执行下个命令
void resetClient(client *c) {
    redisCommandProc *prevcmd = c->cmd ? c->cmd->proc : NULL;

    freeClientArgv(c);
    c->reqtype = 0;
    c->multibulklen = 0;
    c->bulklen = -1;
    c->slot = -1;

    if (c->deferred_reply_errors) {
        listRelease(c->deferred_reply_errors);
    }
    c->deferred_reply_errors = NULL;

    // 如果我们不在MULTI中,如果我们刚刚执行的不是ask命令本身,我们也会清除ask标志.
    if (!(c->flags & CLIENT_MULTI) && prevcmd != askingCommand)
        c->flags &= ~CLIENT_ASKING;

    /* We do the same for the CACHING command as well. It also affects
     * the next command or transaction executed, in a way very similar
     * to ASKING. */
    if (!(c->flags & CLIENT_MULTI) && prevcmd != clientCommand)
        c->flags &= ~CLIENT_TRACKING_CACHING;

    /* Remove the CLIENT_REPLY_SKIP flag if any so that the reply
     * to the next command will be sent, but set the flag if the command
     * we just processed was "CLIENT REPLY SKIP". */
    c->flags &= ~CLIENT_REPLY_SKIP;
    if (c->flags & CLIENT_REPLY_SKIP_NEXT) {
        c->flags |= CLIENT_REPLY_SKIP;
        c->flags &= ~CLIENT_REPLY_SKIP_NEXT;
    }
}

/* This function is used when we want to re-enter the event loop but there
 * is the risk that the client we are dealing with will be freed in some
 * way. This happens for instance in:
 *
 * * DEBUG RELOAD and similar.
 * * When a Lua script is in -BUSY state.
 *
 * So the function will protect the client by doing two things:
 *
 * 1) It removes the file events. This way it is not possible that an
 *    error is signaled on the socket, freeing the client.
 * 2) Moreover it makes sure that if the client is freed in a different code
 *    path, it is not really released, but only marked for later release. */
void protectClient(client *c) {
    c->flags |= CLIENT_PROTECTED;
    if (c->conn) {
        connSetReadHandler(c->conn, NULL);
        connSetWriteHandler(c->conn, NULL);
    }
}

/* This will undo the client protection done by protectClient() */
void unprotectClient(client *c) {
    if (c->flags & CLIENT_PROTECTED) {
        c->flags &= ~CLIENT_PROTECTED;
        if (c->conn) {
            connSetReadHandler(c->conn, readQueryFromClient);
            if (clientHasPendingReplies(c))
                putClientInPendingWriteQueue(c);
        }
    }
}

/* Like processMultibulkBuffer(), but for the inline protocol instead of RESP,
 * this function consumes the client query buffer and creates a command ready
 * to be executed inside the client structure. Returns C_OK if the command
 * is ready to be executed, or C_ERR if there is still protocol to read to
 * have a well formed command. The function also returns C_ERR when there is
 * a protocol error: in such a case the client structure is setup to reply
 * with the error and close the connection. */
int processInlineBuffer(client *c) {
    char *newline;
    int argc, j, linefeed_chars = 1;
    sds *argv, aux;
    size_t querylen;

    /* Search for end of line */
    newline = strchr(c->querybuf + c->qb_pos, '\n');

    /* Nothing to do without a \r\n */
    if (newline == NULL) {
        if (sdslen(c->querybuf) - c->qb_pos > PROTO_INLINE_MAX_SIZE) {
            addReplyError(c, "Protocol error: too big inline request");
            setProtocolError("too big inline request", c);
        }
        return C_ERR;
    }

    /* Handle the \r\n case. */
    if (newline != c->querybuf + c->qb_pos && *(newline - 1) == '\r')
        newline--, linefeed_chars++;

    /* Split the input buffer up to the \r\n */
    querylen = newline - (c->querybuf + c->qb_pos);
    aux = sdsnewlen(c->querybuf + c->qb_pos, querylen);
    argv = sdssplitargs(aux, &argc);
    sdsfree(aux);
    if (argv == NULL) {
        addReplyError(c, "Protocol error: unbalanced quotes in request");
        setProtocolError("unbalanced quotes in inline request", c);
        return C_ERR;
    }

    /* Newline from slaves can be used to refresh the last ACK time.
     * This is useful for a slave to ping back while loading a big
     * RDB file. */
    if (querylen == 0 && getClientType(c) == CLIENT_TYPE_SLAVE)
        c->repl_ack_time = server.unixtime;

    /* Masters should never send us inline protocol to run actual
     * commands. If this happens, it is likely due to a bug in Redis where
     * we got some desynchronization in the protocol, for example
     * because of a PSYNC gone bad.
     *
     * However there is an exception: masters may send us just a newline
     * to keep the connection active. */
    if (querylen != 0 && c->flags & CLIENT_MASTER) {
        sdsfreesplitres(argv, argc);
        serverLog(LL_WARNING, "WARNING: Receiving inline protocol from master, master stream corruption? Closing the master connection and discarding the cached master.");
        setProtocolError("Master using the inline protocol. Desync?", c);
        return C_ERR;
    }

    /* Move querybuffer position to the next query in the buffer. */
    c->qb_pos += querylen + linefeed_chars;

    /* Setup argv array on client structure */
    if (argc) {
        if (c->argv)
            zfree(c->argv);
        c->argv_len = argc;
        c->argv = zmalloc(sizeof(robj *) * c->argv_len);
        c->argv_len_sum = 0;
    }

    /* Create redis objects for all arguments. */
    for (c->argc = 0, j = 0; j < argc; j++) {
        c->argv[c->argc] = createObject(OBJ_STRING, argv[j]);
        c->argc++;
        c->argv_len_sum += sdslen(argv[j]);
    }
    zfree(argv);
    return C_OK;
}

/* Helper function. Record protocol error details in server log,
 * and set the client as CLIENT_CLOSE_AFTER_REPLY and
 * CLIENT_PROTOCOL_ERROR. */
#define PROTO_DUMP_LEN 128

static void setProtocolError(const char *errstr, client *c) {
    if (server.verbosity <= LL_VERBOSE || c->flags & CLIENT_MASTER) {
        sds client = catClientInfoString(sdsempty(), c);

        /* Sample some protocol to given an idea about what was inside. */
        char buf[256];
        if (sdslen(c->querybuf) - c->qb_pos < PROTO_DUMP_LEN) {
            snprintf(buf, sizeof(buf), "Query buffer during protocol error: '%s'", c->querybuf + c->qb_pos);
        }
        else {
            snprintf(buf, sizeof(buf), "Query buffer during protocol error: '%.*s' (... more %zu bytes ...) '%.*s'", PROTO_DUMP_LEN / 2, c->querybuf + c->qb_pos, sdslen(c->querybuf) - c->qb_pos - PROTO_DUMP_LEN, PROTO_DUMP_LEN / 2, c->querybuf + sdslen(c->querybuf) - PROTO_DUMP_LEN / 2);
        }

        /* Remove non printable chars. */
        char *p = buf;
        while (*p != '\0') {
            if (!isprint(*p))
                *p = '.';
            p++;
        }

        /* Log all the client and protocol info. */
        int loglevel = (c->flags & CLIENT_MASTER) ? LL_WARNING : LL_VERBOSE;
        serverLog(loglevel, "Protocol error (%s) from client: %s. %s", errstr, client, buf);
        sdsfree(client);
    }
    c->flags |= (CLIENT_CLOSE_AFTER_REPLY | CLIENT_PROTOCOL_ERROR);
}

/*
 * 将 c->querybuf 中的协议内容转换成 c->argv 中的参数对象
 *
 * 比如 *3\r\n$3\r\nSET\r\n$3\r\nMSG\r\n$5\r\nHELLO\r\n
 * 将被转换为：
 * argv[0] = SET
 * argv[1] = MSG
 * argv[2] = HELLO
 */
int processMultibulkBuffer(client *c) {
    char *newline = NULL;
    int ok;
    long long ll; // 参数个数、每个参数的长度
    // 读入命令的参数个数
    // 比如 *3\r\n$3\r\nSET\r\n... 将令 c->multibulklen = 3
    if (c->multibulklen == 0) {
        /* 客户端应该已经重置 */
        serverAssertWithInfo(c, NULL, c->argc == 0);

        // strchr(const char *str, int c),即在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置
        // 检查缓冲区的内容第一个 "\r\n"
        newline = strchr(c->querybuf + c->qb_pos, '\r');
        if (newline == NULL) {
            if (sdslen(c->querybuf) - c->qb_pos > PROTO_INLINE_MAX_SIZE) { // 64Kb
                addReplyError(c, "Protocol error: too big mbulk count string");
                setProtocolError("too big mbulk count string", c);
            }
            return C_ERR;
        }
        // 判断有没有\n
        if (newline - (c->querybuf + c->qb_pos) > (ssize_t)(sdslen(c->querybuf) - c->qb_pos - 2)) {
            return C_ERR;
        }

        // 协议的第一个字符必须是 '*'
        serverAssertWithInfo(c, NULL, c->querybuf[c->qb_pos] == '*');
        // 将参数个数,也即是 * 之后, \r\n 之前的数字取出并保存到 ll 中
        // 比如对于 *3\r\n ,那么 ll 将等于 3
        ok = string2ll(c->querybuf + 1 + c->qb_pos, newline - (c->querybuf + 1 + c->qb_pos), &ll);
        // 参数的数量超出限制
        if (!ok || ll > INT_MAX) {
            addReplyError(c, "Protocol error: invalid multibulk length");
            setProtocolError("invalid mbulk count", c);
            return C_ERR;
        }
        else if (ll > 10 && authRequired(c)) {
            addReplyError(c, "Protocol error: unauthenticated multibulk length");
            setProtocolError("unauth mbulk count", c);
            return C_ERR;
        }
        // 参数数量之后的位置
        // 比如对于 *3\r\n$3\r\n$SET\r\n... 来说,
        // pos 指向 *3\r\n$3\r\n$SET\r\n...
        //              ^
        //              |
        //             pos
        c->qb_pos = (newline - c->querybuf) + 2; // 跳过了*3\r\n
        // 如果 ll <= 0 ,那么这个命令是一个空白命令
        // 那么将这段内容从查询缓冲区中删除,只保留未阅读的那部分内容
        // 为什么参数可以是空的呢？
        // processInputBuffer 中有注释到 "Multibulk processing could see a <= 0 length"
        // 但并没有详细说明原因
        if (ll <= 0) {
            return C_OK;
        }

        c->multibulklen = ll; // 设置参数数量

        // 根据参数数量,为各个参数对象分配空间
        if (c->argv) {
            zfree(c->argv);
        }

        c->argv_len = min(c->multibulklen, 1024);
        c->argv = zmalloc(sizeof(robj *) * c->argv_len);
        c->argv_len_sum = 0;
    }

    serverAssertWithInfo(c, NULL, c->multibulklen > 0);
    while (c->multibulklen) { // 从 c->querybuf 中读入参数,并创建各个参数对象到 c->argv
        // 读入参数长度
        if (c->bulklen == -1) {
            // 确保 "\r\n" 存在
            newline = strchr(c->querybuf + c->qb_pos, '\r');
            if (newline == NULL) {
                if (sdslen(c->querybuf) - c->qb_pos > PROTO_INLINE_MAX_SIZE) {
                    addReplyError(c, "Protocol error: too big bulk count string");
                    setProtocolError("too big bulk count string", c);
                    return C_ERR;
                }
                break;
            }

            /* Buffer should also contain \n */
            if (newline - (c->querybuf + c->qb_pos) > (ssize_t)(sdslen(c->querybuf) - c->qb_pos - 2))
                break;

            // 确保协议符合参数格式,检查其中的 $...
            // 比如 $3\r\nSET\r\n
            if (c->querybuf[c->qb_pos] != '$') {
                addReplyErrorFormat(c, "Protocol error: expected '$', got '%c'", c->querybuf[c->qb_pos]);
                setProtocolError("expected $ but got something else", c);
                return C_ERR;
            }
            // 读取长度
            // 比如 $3\r\nSET\r\n 将会让 ll 的值设置 3
            ok = string2ll(c->querybuf + c->qb_pos + 1, newline - (c->querybuf + c->qb_pos + 1), &ll);
            if (!ok || ll < 0 || (!(c->flags & CLIENT_MASTER) && ll > server.proto_max_bulk_len)) {
                addReplyError(c, "Protocol error: invalid bulk length");
                setProtocolError("invalid bulk length", c);
                return C_ERR;
            }
            else if (ll > 16384 && authRequired(c)) {
                addReplyError(c, "Protocol error: unauthenticated bulk length");
                setProtocolError("unauth bulk length", c);
                return C_ERR;
            }
            // 定位到参数的开头
            // 比如
            // $3\r\nSET\r\n...
            //     ^
            //     |
            //    pos
            c->qb_pos = newline - c->querybuf + 2;
            // 如果参数非常长,那么做一些预备措施来优化接下来的参数复制操作
            if (!(c->flags & CLIENT_MASTER) && ll >= PROTO_MBULK_BIG_ARG) {
                /* When the client is not a master client (because master
                 * client's querybuf can only be trimmed after data applied
                 * and sent to replicas).
                 *
                 * If we are going to read a large object from network
                 * try to make it likely that it will start at c->querybuf
                 * boundary so that we can optimize object creation
                 * avoiding a large copy of data.
                 *
                 * But only when the data we have not parsed is less than
                 * or equal to ll+2. If the data length is greater than
                 * ll+2, trimming querybuf is just a waste of time, because
                 * at this time the querybuf contains not only our bulk. */
                if (sdslen(c->querybuf) - c->qb_pos <= (size_t)ll + 2) {
                    sdsrange(c->querybuf, c->qb_pos, -1);
                    c->qb_pos = 0;
                    /* Hint the sds library about the amount of bytes this string is
                     * going to contain. */
                    c->querybuf = sdsMakeRoomForNonGreedy(c->querybuf, ll + 2 - sdslen(c->querybuf)); // 会给剩余参数分配空间
                }
            }
            // 参数的长度
            c->bulklen = ll;
        }

        // 读入参数 ,querybuf里的数据不全,还有没有读进程序里来的
        if (sdslen(c->querybuf) - c->qb_pos < (size_t)(c->bulklen + 2)) {
            // 没有足够的参数数据
            /* Not enough data (+2 == trailing \r\n) */
            break;
        }
        else {
            /* Check if we have space in argv, grow if needed */
            if (c->argc >= c->argv_len) {
                c->argv_len = min(c->argv_len < INT_MAX / 2 ? c->argv_len * 2 : INT_MAX, c->argc + c->multibulklen);
                c->argv = zrealloc(c->argv, sizeof(robj *) * c->argv_len);
            }
            // 为参数创建字符串对象
            /* Optimization: if a non-master client's buffer contains JUST our bulk element
             * instead of creating a new object by *copying* the sds we
             * just use the current sds string. */
            if (!(c->flags & CLIENT_MASTER) && c->qb_pos == 0 && c->bulklen >= PROTO_MBULK_BIG_ARG && sdslen(c->querybuf) == (size_t)(c->bulklen + 2)) {
                c->argv[c->argc++] = createObject(OBJ_STRING, c->querybuf);
                c->argv_len_sum += c->bulklen;
                sdsIncrLen(c->querybuf, -2); /* remove CRLF */
                /* Assume that if we saw a fat argument we'll see another one
                 * likely... */
                c->querybuf = sdsnewlen(SDS_NOINIT, c->bulklen + 2);
                sdsclear(c->querybuf);
            }
            else {
                c->argv[c->argc++] = createStringObject(c->querybuf + c->qb_pos, c->bulklen);
                c->argv_len_sum += c->bulklen;
                c->qb_pos += c->bulklen + 2;
            }
            c->bulklen = -1;   // 清空参数长度
            c->multibulklen--; // 减少还需读入的参数个数
        }
    }
    // 如果本条命令的所有参数都已读取完,那么返回
    if (c->multibulklen == 0) {
        return C_OK; // 没有要读取的参数了
    }

    // 仍然没有准备好处理命令
    return C_ERR;
}

/* Perform necessary tasks after a command was executed:
 *
 * 1. The client is reset unless there are reasons to avoid doing it.
 * 2. In the case of master clients, the replication offset is updated.
 * 3. Propagate commands we got from our master to replicas down the line. */
void commandProcessed(client *c) {
    /* If client is blocked(including paused), just return avoid reset and replicate.
     *
     * 1. Don't reset the client structure for blocked clients, so that the reply
     *    callback will still be able to access the client argv and argc fields.
     *    The client will be reset in unblockClient().
     * 2. Don't update replication offset or propagate commands to replicas,
     *    since we have not applied the command. */
    if (c->flags & CLIENT_BLOCKED)
        return;

    resetClient(c);

    long long prev_offset = c->reploff;
    if (c->flags & CLIENT_MASTER && !(c->flags & CLIENT_MULTI)) {
        /* Update the applied replication offset of our master. */
        c->reploff = c->read_reploff - sdslen(c->querybuf) + c->qb_pos;
    }

    /* If the client is a master we need to compute the difference
     * between the applied offset before and after processing the buffer,
     * to understand how much of the replication stream was actually
     * applied to the master state: this quantity, and its corresponding
     * part of the replication stream, will be propagated to the
     * sub-replicas and to the replication backlog. */
    if (c->flags & CLIENT_MASTER) {
        long long applied = c->reploff - prev_offset;
        if (applied) {
            replicationFeedStreamFromMasterStream(c->querybuf + c->repl_applied, applied);
            c->repl_applied += applied;
        }
    }
}

// 这个函数执行时,我们已经读入了一个完整的命令到客户端,
// 这个函数负责执行这个命令,或者服务器准备从客户端中进行一次读取.
int processCommandAndResetClient(client *c) {
    int deadclient = 0;
    client *old_client = server.current_client;
    server.current_client = c;
    if (processCommand(c) == C_OK) { // 执行命令
        commandProcessed(c);
        /*更新客户端的内存,以包括处理后的命令之后的输出缓冲区增长.*/
        updateClientMemUsage(c);
    }

    if (server.current_client == NULL)
        deadclient = 1;
    /*
     * Restore the old client, this is needed because when a script
     * times out, we will get into this code from processEventsWhileBlocked.
     * Which will cause to set the server.current_client. If not restored
     * we will return 1 to our caller which will falsely indicate the client
     * is dead and will stop reading from its buffer.
     */
    server.current_client = old_client;
    /* performEvictions may flush slave output buffers. This may
     * result in a slave, that may be the active client, to be
     * freed. */
    return deadclient ? C_ERR : C_OK;
}

/* This function will execute any fully parsed commands pending on
 * the client. Returns C_ERR if the client is no longer valid after executing
 * the command, and C_OK for all other cases. */
int processPendingCommandAndInputBuffer(client *c) {
    if (c->flags & CLIENT_PENDING_COMMAND) {
        c->flags &= ~CLIENT_PENDING_COMMAND;
        if (processCommandAndResetClient(c) == C_ERR) {
            return C_ERR;
        }
    }

    /* Now process client if it has more data in it's buffer.
     *
     * Note: when a master client steps into this function,
     * it can always satisfy this condition, because its querbuf
     * contains data not applied. */
    if (c->querybuf && sdslen(c->querybuf) > 0) {
        return processInputBuffer(c);
    }
    return C_OK;
}

// 处理客户端输入的命令内容
int processInputBuffer(client *c) {
    /* 当输入缓冲区中有东西时,继续处理 */
    // 尽可能地处理查询缓冲区中的内容
    // 如果读取出现 short read ,那么可能会有内容滞留在读取缓冲区里面
    // 这些滞留内容也许不能完整构成一个符合协议的命令,
    // 需要等待下次读事件的就绪
    while (c->qb_pos < sdslen(c->querybuf)) {
        // REDIS_BLOCKED 状态表示客户端正在被阻塞
        if (c->flags & CLIENT_BLOCKED)
            break;

        // 不要处理来自客户端的更多缓冲区,这些缓冲区已经在c-argv中执行了挂起的命令
        if (c->flags & CLIENT_PENDING_COMMAND) {
            break;
        }

        // 当slave机器上有繁忙的脚本运行时,不要处理master节点上的输入.
        // 我们只希望积累复制流(而不是像我们对其他客户端所做的那样回复-BUSY),然后恢复处理.
        if (scriptIsTimedout() && c->flags & CLIENT_MASTER) {
            break;
        }

        // 客户端已经设置了关闭 FLAG ,没有必要处理命令了
        if (c->flags & (CLIENT_CLOSE_AFTER_REPLY | CLIENT_CLOSE_ASAP)) {
            break;
        }
        // 判断请求的类型
        // 两种类型的区别可以在 Redis 的通讯协议上查到：
        // http://redis.readthedocs.org/en/latest/topic/protocol.html
        // 简单来说,多条查询是一般客户端发送来的,
        // 而内联查询则是 TELNET 发送来的
        if (!c->reqtype) { // 根据第一个字符判断交互方式
            if (c->querybuf[c->qb_pos] == '*') {
                c->reqtype = PROTO_REQ_MULTIBULK; // 多条查询
            }
            else {
                c->reqtype = PROTO_REQ_INLINE; // 内联查询
            }
        }
        // 将缓冲区中的内容转换成命令,以及命令参数
        if (c->reqtype == PROTO_REQ_INLINE) {
            if (processInlineBuffer(c) != C_OK) { // 多次读取客户端传过来的数据,保证数据的完整性
                break;
            }
        }
        else if (c->reqtype == PROTO_REQ_MULTIBULK) {
            if (processMultibulkBuffer(c) != C_OK) { // 多次读取客户端传过来的数据,保证数据的完整性
                break;
            }
        }
        else {
            serverPanic("未知的请求类型");
        }
        sds bytes = sdsempty();
        serverLog(LL_DEBUG, "请求参数长度%d:%s", c->argc, sdscatrepr(bytes, c->querybuf, sdslen(c->querybuf)));
        // Multibulk 处理可能看到a<=0   // 例如通过 nc 直接敲空格
        if (c->argc == 0) {
            resetClient(c); // ✅
        }
        else {
            // 如果我们在一个I/O线程的上下文中,我们不能真正执行这里的命令.我们所能做的就是将客户端标记为需要处理命令的客户端.
            if (io_threads_op != IO_THREADS_OP_IDLE) {
                serverAssert(io_threads_op == IO_THREADS_OP_READ);
                c->flags |= CLIENT_PENDING_COMMAND;
                break;
            }

            if (processCommandAndResetClient(c) == C_ERR) { // 执行命令,并重置客户端
                return C_ERR;
            }
        }
    }
    // 客户端有 CLIENT_MASTER 标记
    if (c->flags & CLIENT_MASTER) {
        // 如果客户端是主客户端,则将querybuf修剪为repl_applied,因为主客户端非常特殊,它的querybuf不仅用于解析命令,还可以代理到子副本.
        // 下面是一些我们不能修剪到qb_pos的场景:
        //* 1.我们没有收到主人的完整命令
        //* 2.主客户端被阻塞导致客户端暂停
        //* 3.io线程操作读,主客户端标记为CLIENT_PENDING_COMMAND
        //       在这些场景中,qb_pos指向当前命令的部分
        //*或下一个命令的开头,当前命令还没有应用,
        //*因此repl_applied不等于qb_pos.
        if (c->repl_applied) {
            sdsrange(c->querybuf, c->repl_applied, -1);
            c->qb_pos -= c->repl_applied;
            c->repl_applied = 0;
        }
    }
    else if (c->qb_pos) {
        // 修建c->querybuf
        sdsrange(c->querybuf, c->qb_pos, -1);
        c->qb_pos = 0;
    }

    /* 在处理完查询缓冲区后更新客户端内存使用情况,这在查询缓冲区很大并且在上述循环期间没有被耗尽的情况下是很重要的(因为部分发送了大的命令).*/
    if (io_threads_op == IO_THREADS_OP_IDLE) {
        updateClientMemUsage(c);
    }

    return C_OK;
}

// 读取客户端的查询缓冲区内容
void readQueryFromClient(connection *conn) {
    client *c = connGetPrivateData(conn);
    int nread, big_arg = 0;
    size_t qblen;     // 缓冲区中存在的数据长度
    size_t old_qblen; // 缓冲区中存在的数据长度,备份
    size_t readlen;   // 缓冲区长度

    //  主线程将 待读客户端 添加到Read任务队列（生产者）
    if (postponeClientRead(c)) { // 延迟读
        return;
    }

    // 更新 已处理的读事件总数
    atomicIncr(server.stat_total_reads_processed, 1);

    readlen = PROTO_IOBUF_LEN; // 读入长度（默认为 16 MB）

    // #define PROTO_REQ_INLINE 1    // 内联型  命令不是以“*”开头
    // #define PROTO_REQ_MULTIBULK 2 // 协议型  命令是以“*”开头

    if (c->reqtype == PROTO_REQ_MULTIBULK && // 命令是以“*”开头
        c->multibulklen &&                   // 剩余未读取的命令内容数量
        c->bulklen != -1                     // 命令内容的长度 在一个批量请求中
        && c->bulklen >= PROTO_MBULK_BIG_ARG //
    ) {
        // 协议型  消息

        // multibulklen表示待从读取的参数的个数
        // bulklen表示当前参数的长度
        // 客户端为redis-cli
        // 如果还有待读取的bulk（参数）,并且上次读取的最后一个参数的数据不完整,

        // 待读取的数据长度   524271650
        ssize_t remaining = (size_t)(c->bulklen + 2) - (sdslen(c->querybuf) - c->qb_pos);
        big_arg = 1;

        /* Note that the 'remaining' variable may be zero in some edge case,
         * for example once we resume a blocked client after CLIENT PAUSE. */
        // 确定该次读取的数据长度
        if (remaining > 0) {
            readlen = remaining;
        }

        /* Master client needs expand the readlen when meet BIG_ARG(see #9100),
         * but doesn't need align to the next arg, we can read more data. */
        if (c->flags & CLIENT_MASTER && readlen < PROTO_IOBUF_LEN)
            readlen = PROTO_IOBUF_LEN;
    }
    // 获取查询缓冲区当前内容的长度
    // 如果读取出现 short read ,那么可能会有内容滞留在读取缓冲区里面
    // 这些滞留内容也许不能完整构成一个符合协议的命令,
    qblen = sdslen(c->querybuf); // 输入缓冲区,保存客户端发送的命令请求;会根据输入内容动态地缩小或者扩大,但它的最 大大小不能超过1GB,否则服务器将关闭这个客户端.
    old_qblen = sdslen(c->querybuf);

    if (!(c->flags & CLIENT_MASTER) &&                       // 该客户端不是一个主节点
        (big_arg || sdsalloc(c->querybuf) < PROTO_IOBUF_LEN) // 客户端发送的数据过大或者小于默认值, 都使用默认分配策略
    ) {
        // 当读取BIG_ARG时,我们不会向查询缓冲区中读取超过一个的参数,所以我们不需要预先分配比我们需要的更多的参数,所以使用非贪婪增长.
        // 对于查询缓冲区的初始分配,我们也不想使用贪婪增长,以避免与RESIZE_THRESHOLD机制发生冲突.
        c->querybuf = sdsMakeRoomForNonGreedy(c->querybuf, readlen); // 分配空间
    }
    else {
        c->querybuf = sdsMakeRoomFor(c->querybuf, readlen); // 为查询缓冲区分配空间
        readlen = sdsavail(c->querybuf);                    // 从套接字读取尽可能多的数据以保存Read(2)系统调用.
    }
    // 读入内容到查询缓存,从qblen开始存储数据,
    nread = connRead(c->conn, c->querybuf + qblen, readlen); // "*2\r\n$7\r\nCOMMAND\r\n$4\r\nDOCS\r\n\001"
    if (nread == -1) {                                       // 读入出错
        if (connGetState(conn) == CONN_STATE_CONNECTED) {
            return;
        }
        else {
            serverLog(LL_VERBOSE, "从客户端读取...: %s", connGetLastError(c->conn));
            freeClientAsync(c);
            goto done;
        }
    }
    else if (nread == 0) { // 遇到 EOF
        if (server.verbosity <= LL_VERBOSE) {
            sds info = catClientInfoString(sdsempty(), c); // 获取客户端的各项信息
            serverLog(LL_VERBOSE, "客户端关闭了链接 %s", info);
            sdsfree(info);
        }
        freeClientAsync(c);
        goto done;
    }

    sdsIncrLen(c->querybuf, nread);
    // 根据内容,更新查询缓冲区（SDS） free 和 len 属性
    // 并将 '\0' 正确地放到内容的最后

    qblen = sdslen(c->querybuf);
    if (c->querybuf_peak < qblen) { // 如果有需要,更新缓冲区内容长度的峰值（peak）
        c->querybuf_peak = qblen;
    }
    serverLog(LL_DEBUG, "c->querybuf --> %zu  %zu\n", sdslen(c->querybuf) - old_qblen, readlen);
    c->lastinteraction = server.unixtime; // 记录服务器和客户端最后一次互动的时间

    if (c->flags & CLIENT_MASTER) {
        c->read_reploff += nread; // 如果客户端是 master 的话,更新它的复制偏移量
    }
    atomicIncr(server.stat_net_input_bytes, nread);
    // 查询缓冲区长度超出服务器最大缓冲区长度
    // 清空缓冲区并释放客户端
    if (!(c->flags & CLIENT_MASTER) && sdslen(c->querybuf) > server.client_max_querybuf_len) { // 1GB
        sds ci = catClientInfoString(sdsempty(), c), bytes = sdsempty();

        bytes = sdscatrepr(bytes, c->querybuf, 64);
        serverLog(LL_WARNING, "正在关闭的客户端已达到最大查询缓冲区长度: %s (qbuf initial bytes: %s)", ci, bytes);
        sdsfree(ci);
        sdsfree(bytes);
        freeClientAsync(c);
        goto done;
    }

    // 从查询缓存重读取内容,创建参数,并执行命令
    // 函数会执行到缓存中的所有内容都被处理完为止
    if (processInputBuffer(c) == C_ERR) { // ✅
        c = NULL;
    }

done:
    beforeNextClient(c);
}

/* A Redis "Address String" is a colon separated ip:port pair.
 * For IPv4 it's in the form x.y.z.k:port, example: "127.0.0.1:1234".
 * For IPv6 addresses we use [] around the IP part, like in "[::1]:1234".
 * For Unix sockets we use path:0, like in "/tmp/redis:0".
 *
 * An Address String always fits inside a buffer of NET_ADDR_STR_LEN bytes,
 * including the null term.
 *
 * On failure the function still populates 'addr' with the "?:0" string in case
 * you want to relax error checking or need to display something anyway (see
 * anetFdToString implementation for more info). */
void genClientAddrString(client *client, char *addr, size_t addr_len, int fd_to_str_type) {
    if (client->flags & CLIENT_UNIX_SOCKET) {
        /* Unix socket client. */
        snprintf(addr, addr_len, "%s:0", server.unixsocket);
    }
    else {
        /* TCP client. */
        connFormatFdAddr(client->conn, addr, addr_len, fd_to_str_type);
    }
}

/* This function returns the client peer id, by creating and caching it
 * if client->peerid is NULL, otherwise returning the cached value.
 * The Peer ID never changes during the life of the client, however it
 * is expensive to compute. */
char *getClientPeerId(client *c) {
    char peerid[NET_ADDR_STR_LEN];

    if (c->peerid == NULL) {
        genClientAddrString(c, peerid, sizeof(peerid), FD_TO_PEER_NAME);
        c->peerid = sdsnew(peerid);
    }
    return c->peerid;
}

// 返回IP:port
char *getClientSockname(client *c) {
    char sockname[NET_ADDR_STR_LEN];

    if (c->sockname == NULL) {
        genClientAddrString(c, sockname, sizeof(sockname), FD_TO_SOCK_NAME);
        c->sockname = sdsnew(sockname);
    }
    return c->sockname;
}

// 获取客户端的各项信息,将它们储存到 s 里面,并返回.
sds catClientInfoString(sds s, client *client) {
    char flags[16];
    char events[3];
    char conninfo[CONN_INFO_LEN];
    char *p;

    p = flags;
    if (client->flags & CLIENT_SLAVE) {
        if (client->flags & CLIENT_MONITOR)
            *p++ = 'O';
        else
            *p++ = 'S';
    }
    if (client->flags & CLIENT_MASTER)
        *p++ = 'M';
    if (client->flags & CLIENT_PUBSUB)
        *p++ = 'P';
    if (client->flags & CLIENT_MULTI)
        *p++ = 'x';
    if (client->flags & CLIENT_BLOCKED)
        *p++ = 'b';
    if (client->flags & CLIENT_TRACKING)
        *p++ = 't';
    if (client->flags & CLIENT_TRACKING_BROKEN_REDIR)
        *p++ = 'R';
    if (client->flags & CLIENT_TRACKING_BCAST)
        *p++ = 'B';
    if (client->flags & CLIENT_DIRTY_CAS)
        *p++ = 'd';
    if (client->flags & CLIENT_CLOSE_AFTER_REPLY)
        *p++ = 'c';
    if (client->flags & CLIENT_UNBLOCKED)
        *p++ = 'u';
    if (client->flags & CLIENT_CLOSE_ASAP)
        *p++ = 'A';
    if (client->flags & CLIENT_UNIX_SOCKET)
        *p++ = 'U';
    if (client->flags & CLIENT_READONLY)
        *p++ = 'r';
    if (client->flags & CLIENT_NO_EVICT)
        *p++ = 'e';
    if (p == flags)
        *p++ = 'N';
    *p++ = '\0';

    p = events;
    if (client->conn) {
        if (connHasReadHandler(client->conn)) {
            *p++ = 'r';
        }
        if (connHasWriteHandler(client->conn)) {
            *p++ = 'w';
        }
    }
    *p = '\0';

    size_t obufmem = 0;
    // 计算此客户端消耗的总内存.
    size_t total_mem = getClientMemoryUsage(client, &obufmem);

    size_t used_blocks_of_repl_buf = 0;
    if (client->ref_repl_buf_node) {
        replBufBlock *last = listNodeValue(listLast(server.repl_buffer_blocks));
        replBufBlock *cur = listNodeValue(client->ref_repl_buf_node);
        used_blocks_of_repl_buf = last->id - cur->id + 1;
    }
    sds ret;
    ret = sdscatfmt(s, "id=%U", (unsigned long long)client->id);                     //
    ret = sdscatfmt(s, "addr=%s", getClientPeerId(client));                          // 只有IP
    ret = sdscatfmt(s, "laddr=%s", getClientSockname(client));                       // IP:PORT
    ret = sdscatfmt(s, "%s", connGetInfo(client->conn, conninfo, sizeof(conninfo))); // 返回描述连接的文本 fd=7
    ret = sdscatfmt(s, "名称=%s", client->name ? (char *)client->name->ptr : "");
    ret = sdscatfmt(s, "存活时间=%I", (long long)(server.unixtime - client->ctime)); // 服务端当前时间-客户端创建时间
    ret = sdscatfmt(s, "空闲时间=%I", (long long)(server.unixtime - client->lastinteraction));
    ret = sdscatfmt(s, "flags=%s", flags);
    ret = sdscatfmt(s, "db=%i", client->db->id);
    ret = sdscatfmt(s, "订阅频道的数量=%i", (int)dictSize(client->pubsub_channels));
    ret = sdscatfmt(s, "订阅频道正则模式的数量=%i", (int)listLength(client->pubsub_patterns));
    ret = sdscatfmt(s, "正在执行的事务数量=%i", (client->flags & CLIENT_MULTI) ? client->mstate.count : -1);

    // 缓冲区溢出会导致客户端连接关闭
    ret = sdscatfmt(s, "输入缓冲区大小已用空间=%U", (unsigned long long)sdslen(client->querybuf));
    ret = sdscatfmt(s, "输入缓冲区大小可用空间=%U", (unsigned long long)sdsavail(client->querybuf));
    ret = sdscatfmt(s, "argv列表中对象长度的和=%U", (unsigned long long)client->argv_len_sum);
    ret = sdscatfmt(s, "multi-mem=%U", (unsigned long long)client->mstate.argv_len_sums);
    ret = sdscatfmt(s, "可使用的内存大小=%U", (unsigned long long)client->buf_usable_size);
    ret = sdscatfmt(s, "最近5秒间隔内使用的缓冲器的峰值=%U", (unsigned long long)client->buf_peak);
    ret = sdscatfmt(s, "记录了buf数组目前已使用的字节数量=%U", (unsigned long long)client->bufpos);
    ret = sdscatfmt(s, "oll=%U", (unsigned long long)listLength(client->reply) + used_blocks_of_repl_buf);
    ret = sdscatfmt(s, "客户端输出缓冲区使用的内存=%U", (unsigned long long)obufmem);
    ret = sdscatfmt(s, "计算此客户端消耗的总内存=%U", (unsigned long long)total_mem);
    ret = sdscatfmt(s, "客户端注册的回调函数[rw]=%s", events);
    ret = sdscatfmt(s, "最近执行的命令=%s", client->lastcmd ? client->lastcmd->fullname : "NULL"); // 表示客户端最新执行的命令
    ret = sdscatfmt(s, "用户=%s", client->user ? client->user->name : "(superuser)");
    ret = sdscatfmt(s, "客户端追踪者ID=%I", (client->flags & CLIENT_TRACKING) ? (long long)client->client_tracking_redirection : -1);
    ret = sdscatfmt(s, "resp协议版本=%i", client->resp);

    return ret;
}

// 打印出所有连接到服务器的客户端的信息
sds getAllClientsInfoString(int type) {
    listNode *ln;
    listIter li;
    client *client;
    sds o = sdsnewlen(SDS_NOINIT, 200 * listLength(server.clients));
    sdsclear(o);
    listRewind(server.clients, &li);
    while ((ln = listNext(&li)) != NULL) {
        client = listNodeValue(ln);
        if (type != -1 && getClientType(client) != type)
            continue;
        o = catClientInfoString(o, client);
        o = sdscatlen(o, "\n", 1);
    }
    return o;
}

/* This function implements CLIENT SETNAME, including replying to the
 * user with an error if the charset is wrong (in that case C_ERR is
 * returned). If the function succeeded C_OK is returned, and it's up
 * to the caller to send a reply if needed.
 *
 * Setting an empty string as name has the effect of unsetting the
 * currently set name: the client will remain unnamed.
 *
 * This function is also used to implement the HELLO SETNAME option. */
int clientSetNameOrReply(client *c, robj *name) {
    int len = sdslen(name->ptr);
    char *p = name->ptr;

    /* Setting the client name to an empty string actually removes
     * the current name. */
    if (len == 0) {
        if (c->name)
            decrRefCount(c->name);
        c->name = NULL;
        return C_OK;
    }

    /* Otherwise check if the charset is ok. We need to do this otherwise
     * CLIENT LIST format will break. You should always be able to
     * split by space to get the different fields. */
    for (int j = 0; j < len; j++) {
        if (p[j] < '!' || p[j] > '~') { /* ASCII is assumed. */
            addReplyError(
                c,
                "Client names cannot contain spaces, "
                "newlines or special characters.");
            return C_ERR;
        }
    }
    if (c->name)
        decrRefCount(c->name);
    c->name = name;
    incrRefCount(name);
    return C_OK;
}

/* Reset the client state to resemble a newly connected client.
 */
void resetCommand(client *c) {
    /* MONITOR clients are also marked with CLIENT_SLAVE, we need to
     * distinguish between the two.
     */
    uint64_t flags = c->flags;
    if (flags & CLIENT_MONITOR)
        flags &= ~(CLIENT_MONITOR | CLIENT_SLAVE);

    if (flags & (CLIENT_SLAVE | CLIENT_MASTER | CLIENT_MODULE)) {
        addReplyError(c, "can only reset normal client connections");
        return;
    }

    clearClientConnectionState(c);
    addReplyStatus(c, "RESET");
}

/* Disconnect the current client */
void quitCommand(client *c) {
    addReply(c, shared.ok);
    c->flags |= CLIENT_CLOSE_AFTER_REPLY;
}

void clientCommand(client *c) {
    listNode *ln;
    listIter li;

    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr, "help")) {
        const char *help[] = {
            "CACHING (YES|NO)",
            "    Enable/disable tracking of the keys for next command in OPTIN/OPTOUT modes.",
            "GETREDIR",
            "    Return the client ID we are redirecting to when tracking is enabled.",
            "GETNAME",
            "    Return the name of the current connection.",
            "ID",
            "    Return the ID of the current connection.",
            "INFO",
            "    Return information about the current client connection.",
            "KILL <ip:port>",
            "    Kill connection made from <ip:port>.",
            "KILL <option> <value> [<option> <value> [...]]",
            "    Kill connections. Options are:",
            "    * ADDR (<ip:port>|<unixsocket>:0)",
            "      Kill connections made from the specified address",
            "    * LADDR (<ip:port>|<unixsocket>:0)",
            "      Kill connections made to specified local address",
            "    * TYPE (NORMAL|MASTER|REPLICA|PUBSUB)",
            "      Kill connections by type.",
            "    * USER <username>",
            "      Kill connections authenticated by <username>.",
            "    * SKIPME (YES|NO)",
            "      Skip killing current connection (default: yes).",
            "LIST [options ...]",
            "    Return information about client connections. Options:",
            "    * TYPE (NORMAL|MASTER|REPLICA|PUBSUB)",
            "      Return clients of specified type.",
            "UNPAUSE",
            "    Stop the current client pause, resuming traffic.",
            "PAUSE <timeout> [WRITE|ALL]",
            "    Suspend all, or just write, clients for <timeout> milliseconds.",
            "REPLY (ON|OFF|SKIP)",
            "    Control the replies sent to the current connection.",
            "SETNAME <name>",
            "    Assign the name <name> to the current connection.",
            "UNBLOCK <clientid> [TIMEOUT|ERROR]",
            "    Unblock the specified blocked client.",
            "TRACKING (ON|OFF) [REDIRECT <id>] [BCAST] [PREFIX <prefix> [...]]",
            "         [OPTIN] [OPTOUT] [NOLOOP]",
            "    Control server assisted client side caching.",
            "TRACKINGINFO",
            "    Report tracking status for the current connection.",
            "NO-EVICT (ON|OFF)",
            "    Protect current client connection from eviction.",
            NULL};
        addReplyHelp(c, help);
    }
    else if (!strcasecmp(c->argv[1]->ptr, "id") && c->argc == 2) {
        /* CLIENT ID */
        addReplyLongLong(c, c->id);
    }
    else if (!strcasecmp(c->argv[1]->ptr, "info") && c->argc == 2) {
        /* CLIENT INFO */
        sds o = catClientInfoString(sdsempty(), c);
        o = sdscatlen(o, "\n", 1);
        addReplyVerbatim(c, o, sdslen(o), "txt");
        sdsfree(o);
    }
    else if (!strcasecmp(c->argv[1]->ptr, "list")) {
        /* CLIENT LIST */
        int type = -1;
        sds o = NULL;
        if (c->argc == 4 && !strcasecmp(c->argv[2]->ptr, "type")) {
            type = getClientTypeByName(c->argv[3]->ptr);
            if (type == -1) {
                addReplyErrorFormat(c, "Unknown client type '%s'", (char *)c->argv[3]->ptr);
                return;
            }
        }
        else if (c->argc > 3 && !strcasecmp(c->argv[2]->ptr, "id")) {
            int j;
            o = sdsempty();
            for (j = 3; j < c->argc; j++) {
                long long cid;
                if (getLongLongFromObjectOrReply(c, c->argv[j], &cid, "Invalid client ID")) {
                    sdsfree(o);
                    return;
                }
                client *cl = lookupClientByID(cid);
                if (cl) {
                    o = catClientInfoString(o, cl);
                    o = sdscatlen(o, "\n", 1);
                }
            }
        }
        else if (c->argc != 2) {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }

        if (!o)
            o = getAllClientsInfoString(type);
        addReplyVerbatim(c, o, sdslen(o), "txt");
        sdsfree(o);
    }
    else if (!strcasecmp(c->argv[1]->ptr, "reply") && c->argc == 3) {
        /* CLIENT REPLY ON|OFF|SKIP */
        if (!strcasecmp(c->argv[2]->ptr, "on")) {
            c->flags &= ~(CLIENT_REPLY_SKIP | CLIENT_REPLY_OFF);
            addReply(c, shared.ok);
        }
        else if (!strcasecmp(c->argv[2]->ptr, "off")) {
            c->flags |= CLIENT_REPLY_OFF;
        }
        else if (!strcasecmp(c->argv[2]->ptr, "skip")) {
            if (!(c->flags & CLIENT_REPLY_OFF))
                c->flags |= CLIENT_REPLY_SKIP_NEXT;
        }
        else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
    }
    else if (!strcasecmp(c->argv[1]->ptr, "no-evict") && c->argc == 3) {
        /* CLIENT NO-EVICT ON|OFF */
        if (!strcasecmp(c->argv[2]->ptr, "on")) {
            c->flags |= CLIENT_NO_EVICT;
            addReply(c, shared.ok);
        }
        else if (!strcasecmp(c->argv[2]->ptr, "off")) {
            c->flags &= ~CLIENT_NO_EVICT;
            addReply(c, shared.ok);
        }
        else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
    }
    else if (!strcasecmp(c->argv[1]->ptr, "kill")) {
        /* CLIENT KILL <ip:port>
         * CLIENT KILL <option> [value] ... <option> [value] */
        char *addr = NULL;
        char *laddr = NULL;
        user *user = NULL;
        int type = -1;
        uint64_t id = 0;
        int skipme = 1;
        int killed = 0, close_this_client = 0;

        if (c->argc == 3) {
            /* Old style syntax: CLIENT KILL <addr> */
            addr = c->argv[2]->ptr;
            skipme = 0; /* With the old form, you can kill yourself. */
        }
        else if (c->argc > 3) {
            int i = 2; /* Next option index. */

            /* New style syntax: parse options. */
            while (i < c->argc) {
                int moreargs = c->argc > i + 1;

                if (!strcasecmp(c->argv[i]->ptr, "id") && moreargs) {
                    long tmp;

                    if (getRangeLongFromObjectOrReply(c, c->argv[i + 1], 1, LONG_MAX, &tmp, "client-id should be greater than 0") != C_OK)
                        return;
                    id = tmp;
                }
                else if (!strcasecmp(c->argv[i]->ptr, "type") && moreargs) {
                    type = getClientTypeByName(c->argv[i + 1]->ptr);
                    if (type == -1) {
                        addReplyErrorFormat(c, "Unknown client type '%s'", (char *)c->argv[i + 1]->ptr);
                        return;
                    }
                }
                else if (!strcasecmp(c->argv[i]->ptr, "addr") && moreargs) {
                    addr = c->argv[i + 1]->ptr;
                }
                else if (!strcasecmp(c->argv[i]->ptr, "laddr") && moreargs) {
                    laddr = c->argv[i + 1]->ptr;
                }
                else if (!strcasecmp(c->argv[i]->ptr, "user") && moreargs) {
                    user = ACLGetUserByName(c->argv[i + 1]->ptr, sdslen(c->argv[i + 1]->ptr));
                    if (user == NULL) {
                        addReplyErrorFormat(c, "No such user '%s'", (char *)c->argv[i + 1]->ptr);
                        return;
                    }
                }
                else if (!strcasecmp(c->argv[i]->ptr, "skipme") && moreargs) {
                    if (!strcasecmp(c->argv[i + 1]->ptr, "yes")) {
                        skipme = 1;
                    }
                    else if (!strcasecmp(c->argv[i + 1]->ptr, "no")) {
                        skipme = 0;
                    }
                    else {
                        addReplyErrorObject(c, shared.syntaxerr);
                        return;
                    }
                }
                else {
                    addReplyErrorObject(c, shared.syntaxerr);
                    return;
                }
                i += 2;
            }
        }
        else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }

        /* Iterate clients killing all the matching clients. */
        listRewind(server.clients, &li);
        while ((ln = listNext(&li)) != NULL) {
            client *client = listNodeValue(ln);
            if (addr && strcmp(getClientPeerId(client), addr) != 0)
                continue;
            if (laddr && strcmp(getClientSockname(client), laddr) != 0)
                continue;
            if (type != -1 && getClientType(client) != type)
                continue;
            if (id != 0 && client->id != id)
                continue;
            if (user && client->user != user)
                continue;
            if (c == client && skipme)
                continue;

            /* Kill it. */
            if (c == client) {
                close_this_client = 1;
            }
            else {
                freeClient(client);
            }
            killed++;
        }

        /* Reply according to old/new format. */
        if (c->argc == 3) {
            if (killed == 0)
                addReplyError(c, "No such client");
            else
                addReply(c, shared.ok);
        }
        else {
            addReplyLongLong(c, killed);
        }

        /* If this client has to be closed, flag it as CLOSE_AFTER_REPLY
         * only after we queued the reply to its output buffers. */
        if (close_this_client)
            c->flags |= CLIENT_CLOSE_AFTER_REPLY;
    }
    else if (!strcasecmp(c->argv[1]->ptr, "unblock") && (c->argc == 3 || c->argc == 4)) {
        /* CLIENT UNBLOCK <id> [timeout|error] */
        long long id;
        int unblock_error = 0;

        if (c->argc == 4) {
            if (!strcasecmp(c->argv[3]->ptr, "timeout")) {
                unblock_error = 0;
            }
            else if (!strcasecmp(c->argv[3]->ptr, "error")) {
                unblock_error = 1;
            }
            else {
                addReplyError(c, "CLIENT UNBLOCK reason should be TIMEOUT or ERROR");
                return;
            }
        }
        if (getLongLongFromObjectOrReply(c, c->argv[2], &id, NULL) != C_OK)
            return;
        struct client *target = lookupClientByID(id);
        /* Note that we never try to unblock a client blocked on a module command, which
         * doesn't have a timeout callback (even in the case of UNBLOCK ERROR).
         * The reason is that we assume that if a command doesn't expect to be timedout,
         * it also doesn't expect to be unblocked by CLIENT UNBLOCK */
        if (target && target->flags & CLIENT_BLOCKED && moduleBlockedClientMayTimeout(target)) {
            if (unblock_error)
                addReplyError(target, "-UNBLOCKED client unblocked via CLIENT UNBLOCK");
            else
                replyToBlockedClientTimedOut(target);
            unblockClient(target);
            updateStatsOnUnblock(target, 0, 0, 1);
            addReply(c, shared.cone);
        }
        else {
            addReply(c, shared.czero);
        }
    }
    else if (!strcasecmp(c->argv[1]->ptr, "setname") && c->argc == 3) {
        /* CLIENT SETNAME */
        if (clientSetNameOrReply(c, c->argv[2]) == C_OK)
            addReply(c, shared.ok);
    }
    else if (!strcasecmp(c->argv[1]->ptr, "getname") && c->argc == 2) {
        /* CLIENT GETNAME */
        if (c->name)
            addReplyBulk(c, c->name);
        else
            addReplyNull(c);
    }
    else if (!strcasecmp(c->argv[1]->ptr, "unpause") && c->argc == 2) {
        /* CLIENT UNPAUSE */
        unpauseClients(PAUSE_BY_CLIENT_COMMAND);
        addReply(c, shared.ok);
    }
    else if (!strcasecmp(c->argv[1]->ptr, "pause") && (c->argc == 3 || c->argc == 4)) {
        /* CLIENT PAUSE TIMEOUT [WRITE|ALL] */
        mstime_t end;
        int type = CLIENT_PAUSE_ALL;
        if (c->argc == 4) {
            if (!strcasecmp(c->argv[3]->ptr, "write")) {
                type = CLIENT_PAUSE_WRITE;
            }
            else if (!strcasecmp(c->argv[3]->ptr, "all")) {
                type = CLIENT_PAUSE_ALL;
            }
            else {
                addReplyError(c, "CLIENT PAUSE mode must be WRITE or ALL");
                return;
            }
        }

        if (getTimeoutFromObjectOrReply(c, c->argv[2], &end, UNIT_MILLISECONDS) != C_OK)
            return;
        pauseClients(PAUSE_BY_CLIENT_COMMAND, end, type);
        addReply(c, shared.ok);
    }
    else if (!strcasecmp(c->argv[1]->ptr, "tracking") && c->argc >= 3) {
        /* CLIENT TRACKING (on|off) [REDIRECT <id>] [BCAST] [PREFIX first]
         *                          [PREFIX second] [OPTIN] [OPTOUT] [NOLOOP]... */
        long long redir = 0;
        uint64_t options = 0;
        robj **prefix = NULL;
        size_t numprefix = 0;

        /* Parse the options. */
        for (int j = 3; j < c->argc; j++) {
            int moreargs = (c->argc - 1) - j;

            if (!strcasecmp(c->argv[j]->ptr, "redirect") && moreargs) {
                j++;
                if (redir != 0) {
                    addReplyError(c, "A client can only redirect to a single other client");
                    zfree(prefix);
                    return;
                }

                if (getLongLongFromObjectOrReply(c, c->argv[j], &redir, NULL) != C_OK) {
                    zfree(prefix);
                    return;
                }
                /* We will require the client with the specified ID to exist
                 * right now, even if it is possible that it gets disconnected
                 * later. Still a valid sanity check. */
                if (lookupClientByID(redir) == NULL) {
                    addReplyError(c, "The client ID you want redirect to does not exist");
                    zfree(prefix);
                    return;
                }
            }
            else if (!strcasecmp(c->argv[j]->ptr, "bcast")) {
                options |= CLIENT_TRACKING_BCAST;
            }
            else if (!strcasecmp(c->argv[j]->ptr, "optin")) {
                options |= CLIENT_TRACKING_OPTIN;
            }
            else if (!strcasecmp(c->argv[j]->ptr, "optout")) {
                options |= CLIENT_TRACKING_OPTOUT;
            }
            else if (!strcasecmp(c->argv[j]->ptr, "noloop")) {
                options |= CLIENT_TRACKING_NOLOOP;
            }
            else if (!strcasecmp(c->argv[j]->ptr, "prefix") && moreargs) {
                j++;
                prefix = zrealloc(prefix, sizeof(robj *) * (numprefix + 1));
                prefix[numprefix++] = c->argv[j];
            }
            else {
                zfree(prefix);
                addReplyErrorObject(c, shared.syntaxerr);
                return;
            }
        }

        /* Options are ok: enable or disable the tracking for this client. */
        if (!strcasecmp(c->argv[2]->ptr, "on")) {
            /* Before enabling tracking, make sure options are compatible
             * among each other and with the current state of the client. */
            if (!(options & CLIENT_TRACKING_BCAST) && numprefix) {
                addReplyError(c, "PREFIX option requires BCAST mode to be enabled");
                zfree(prefix);
                return;
            }

            if (c->flags & CLIENT_TRACKING) {
                int oldbcast = !!(c->flags & CLIENT_TRACKING_BCAST);
                int newbcast = !!(options & CLIENT_TRACKING_BCAST);
                if (oldbcast != newbcast) {
                    addReplyError(
                        c,
                        "You can't switch BCAST mode on/off before disabling "
                        "tracking for this client, and then re-enabling it with "
                        "a different mode.");
                    zfree(prefix);
                    return;
                }
            }

            if (options & CLIENT_TRACKING_BCAST && options & (CLIENT_TRACKING_OPTIN | CLIENT_TRACKING_OPTOUT)) {
                addReplyError(c, "OPTIN and OPTOUT are not compatible with BCAST");
                zfree(prefix);
                return;
            }

            if (options & CLIENT_TRACKING_OPTIN && options & CLIENT_TRACKING_OPTOUT) {
                addReplyError(c, "You can't specify both OPTIN mode and OPTOUT mode");
                zfree(prefix);
                return;
            }

            if ((options & CLIENT_TRACKING_OPTIN && c->flags & CLIENT_TRACKING_OPTOUT) || (options & CLIENT_TRACKING_OPTOUT && c->flags & CLIENT_TRACKING_OPTIN)) {
                addReplyError(
                    c,
                    "You can't switch OPTIN/OPTOUT mode before disabling "
                    "tracking for this client, and then re-enabling it with "
                    "a different mode.");
                zfree(prefix);
                return;
            }

            if (options & CLIENT_TRACKING_BCAST) {
                if (!checkPrefixCollisionsOrReply(c, prefix, numprefix)) {
                    zfree(prefix);
                    return;
                }
            }

            enableTracking(c, redir, options, prefix, numprefix);
        }
        else if (!strcasecmp(c->argv[2]->ptr, "off")) {
            disableTracking(c);
        }
        else {
            zfree(prefix);
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
        zfree(prefix);
        addReply(c, shared.ok);
    }
    else if (!strcasecmp(c->argv[1]->ptr, "caching") && c->argc >= 3) {
        if (!(c->flags & CLIENT_TRACKING)) {
            addReplyError(
                c,
                "CLIENT CACHING can be called only when the "
                "client is in tracking mode with OPTIN or "
                "OPTOUT mode enabled");
            return;
        }

        char *opt = c->argv[2]->ptr;
        if (!strcasecmp(opt, "yes")) {
            if (c->flags & CLIENT_TRACKING_OPTIN) {
                c->flags |= CLIENT_TRACKING_CACHING;
            }
            else {
                addReplyError(c, "CLIENT CACHING YES is only valid when tracking is enabled in OPTIN mode.");
                return;
            }
        }
        else if (!strcasecmp(opt, "no")) {
            if (c->flags & CLIENT_TRACKING_OPTOUT) {
                c->flags |= CLIENT_TRACKING_CACHING;
            }
            else {
                addReplyError(c, "CLIENT CACHING NO is only valid when tracking is enabled in OPTOUT mode.");
                return;
            }
        }
        else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }

        /* Common reply for when we succeeded. */
        addReply(c, shared.ok);
    }
    else if (!strcasecmp(c->argv[1]->ptr, "getredir") && c->argc == 2) {
        /* CLIENT GETREDIR */
        if (c->flags & CLIENT_TRACKING) {
            addReplyLongLong(c, c->client_tracking_redirection);
        }
        else {
            addReplyLongLong(c, -1);
        }
    }
    else if (!strcasecmp(c->argv[1]->ptr, "trackinginfo") && c->argc == 2) {
        addReplyMapLen(c, 3);

        /* Flags */
        addReplyBulkCString(c, "flags");
        void *arraylen_ptr = addReplyDeferredLen(c);
        int numflags = 0;
        addReplyBulkCString(c, c->flags & CLIENT_TRACKING ? "on" : "off");
        numflags++;
        if (c->flags & CLIENT_TRACKING_BCAST) {
            addReplyBulkCString(c, "bcast");
            numflags++;
        }
        if (c->flags & CLIENT_TRACKING_OPTIN) {
            addReplyBulkCString(c, "optin");
            numflags++;
            if (c->flags & CLIENT_TRACKING_CACHING) {
                addReplyBulkCString(c, "caching-yes");
                numflags++;
            }
        }
        if (c->flags & CLIENT_TRACKING_OPTOUT) {
            addReplyBulkCString(c, "optout");
            numflags++;
            if (c->flags & CLIENT_TRACKING_CACHING) {
                addReplyBulkCString(c, "caching-no");
                numflags++;
            }
        }
        if (c->flags & CLIENT_TRACKING_NOLOOP) {
            addReplyBulkCString(c, "noloop");
            numflags++;
        }
        if (c->flags & CLIENT_TRACKING_BROKEN_REDIR) {
            addReplyBulkCString(c, "broken_redirect");
            numflags++;
        }
        setDeferredSetLen(c, arraylen_ptr, numflags);

        /* Redirect */
        addReplyBulkCString(c, "redirect");
        if (c->flags & CLIENT_TRACKING) {
            addReplyLongLong(c, c->client_tracking_redirection);
        }
        else {
            addReplyLongLong(c, -1);
        }

        /* Prefixes */
        addReplyBulkCString(c, "prefixes");
        if (c->client_tracking_prefixes) {
            addReplyArrayLen(c, raxSize(c->client_tracking_prefixes));
            raxIterator ri;
            raxStart(&ri, c->client_tracking_prefixes);
            raxSeek(&ri, "^", NULL, 0);
            while (raxNext(&ri)) {
                addReplyBulkCBuffer(c, ri.key, ri.key_len);
            }
            raxStop(&ri);
        }
        else {
            addReplyArrayLen(c, 0);
        }
    }
    else {
        addReplySubcommandSyntaxError(c);
    }
}

/* HELLO [<protocol-version> [AUTH <user> <password>] [SETNAME <name>] ] */
void helloCommand(client *c) {
    long long ver = 0;
    int next_arg = 1;

    if (c->argc >= 2) {
        if (getLongLongFromObjectOrReply(c, c->argv[next_arg++], &ver, "Protocol version is not an integer or out of range") != C_OK) {
            return;
        }

        if (ver < 2 || ver > 3) {
            addReplyError(c, "-NOPROTO unsupported protocol version");
            return;
        }
    }

    for (int j = next_arg; j < c->argc; j++) {
        int moreargs = (c->argc - 1) - j;
        const char *opt = c->argv[j]->ptr;
        if (!strcasecmp(opt, "AUTH") && moreargs >= 2) {
            redactClientCommandArgument(c, j + 1);
            redactClientCommandArgument(c, j + 2);
            if (ACLAuthenticateUser(c, c->argv[j + 1], c->argv[j + 2]) == C_ERR) {
                addReplyError(c, "-WRONGPASS invalid username-password pair or user is disabled.");
                return;
            }
            j += 2;
        }
        else if (!strcasecmp(opt, "SETNAME") && moreargs) {
            if (clientSetNameOrReply(c, c->argv[j + 1]) == C_ERR)
                return;
            j++;
        }
        else {
            addReplyErrorFormat(c, "Syntax error in HELLO option '%s'", opt);
            return;
        }
    }

    /* At this point we need to be authenticated to continue. */
    if (!c->authenticated) {
        addReplyError(
            c,
            "-NOAUTH HELLO must be called with the client already "
            "authenticated, otherwise the HELLO AUTH <user> <pass> "
            "option can be used to authenticate the client and "
            "select the RESP protocol version at the same time");
        return;
    }

    /* Let's switch to the specified RESP mode. */
    if (ver)
        c->resp = ver;
    addReplyMapLen(c, 6 + !server.sentinel_mode);

    addReplyBulkCString(c, "server");
    addReplyBulkCString(c, "redis");

    addReplyBulkCString(c, "version");
    addReplyBulkCString(c, REDIS_VERSION);

    addReplyBulkCString(c, "proto");
    addReplyLongLong(c, c->resp);

    addReplyBulkCString(c, "id");
    addReplyLongLong(c, c->id);

    addReplyBulkCString(c, "mode");
    if (server.sentinel_mode)
        addReplyBulkCString(c, "sentinel");
    else if (server.cluster_enabled)
        addReplyBulkCString(c, "cluster");
    else
        addReplyBulkCString(c, "standalone");

    if (!server.sentinel_mode) {
        addReplyBulkCString(c, "role");
        addReplyBulkCString(c, server.masterhost ? "replica" : "master");
    }

    addReplyBulkCString(c, "modules");
    addReplyLoadedModules(c);
}

// 这个回调被绑定到POST和"Host:"命令名.这些并不是真正的命令,而是在安全攻击中使用的,
// 目的是通过HTTP与Redis实例对话,使用一种称为“跨协议脚本”的技术,它利用了Redis这样的服务会丢弃无效的HTTP报头,并将处理接下来的内容的事实.
// 作为对这种攻击的一种保护,当看到POST或"Host:"报头时,Redis会终止连接,并会不时地记录事件(以避免由于日志太多而创建DOS).
void securityWarningCommand(client *c) {
    static time_t logged_time = 0;
    time_t now = time(NULL);

    if (llabs(now - logged_time) > 60) {
        serverLog(LL_WARNING, "检测到可能的安全攻击.它看起来像有人发送POST或主机:命令到Redis.这可能是由于攻击者试图使用跨协议脚本攻击你的Redis实例.连接失败");
        logged_time = now;
    }
    freeClientAsync(c);
}

/* Keep track of the original command arguments so that we can generate
 * an accurate slowlog entry after the command has been executed. */
static void retainOriginalCommandVector(client *c) {
    /* We already rewrote this command, so don't rewrite it again */
    if (c->original_argv) {
        return;
    }
    c->original_argc = c->argc;
    c->original_argv = zmalloc(sizeof(robj *) * (c->argc));
    for (int j = 0; j < c->argc; j++) {
        c->original_argv[j] = c->argv[j];
        incrRefCount(c->argv[j]);
    }
}

/* Redact a given argument to prevent it from being shown
 * in the slowlog. This information is stored in the
 * original_argv array. */
void redactClientCommandArgument(client *c, int argc) {
    retainOriginalCommandVector(c);
    if (c->original_argv[argc] == shared.redacted) {
        /* This argument has already been redacted */
        return;
    }
    decrRefCount(c->original_argv[argc]);
    c->original_argv[argc] = shared.redacted;
}

/* Rewrite the command vector of the client. All the new objects ref count
 * is incremented. The old command vector is freed, and the old objects
 * ref count is decremented. */
void rewriteClientCommandVector(client *c, int argc, ...) {
    va_list ap;
    int j;
    robj **argv; /* The new argument vector */

    argv = zmalloc(sizeof(robj *) * argc);
    va_start(ap, argc);
    for (j = 0; j < argc; j++) {
        robj *a;

        a = va_arg(ap, robj *);
        argv[j] = a;
        incrRefCount(a);
    }
    replaceClientCommandVector(c, argc, argv);
    va_end(ap);
}

/* Completely replace the client command vector with the provided one. */
void replaceClientCommandVector(client *c, int argc, robj **argv) {
    int j;
    retainOriginalCommandVector(c);
    freeClientArgv(c);
    zfree(c->argv);
    c->argv = argv;
    c->argc = argc;
    c->argv_len_sum = 0;
    for (j = 0; j < c->argc; j++)
        if (c->argv[j])
            c->argv_len_sum += getStringObjectLen(c->argv[j]);
    c->cmd = lookupCommandOrOriginal(c->argv, c->argc);
    serverAssertWithInfo(c, NULL, c->cmd != NULL);
}

/* Rewrite a single item in the command vector.
 * The new val ref count is incremented, and the old decremented.
 *
 * It is possible to specify an argument over the current size of the
 * argument vector: in this case the array of objects gets reallocated
 * and c->argc set to the max value. However it's up to the caller to
 *
 * 1. Make sure there are no "holes" and all the arguments are set.
 * 2. If the original argument vector was longer than the one we
 *    want to end with, it's up to the caller to set c->argc and
 *    free the no longer used objects on c->argv. */
void rewriteClientCommandArgument(client *c, int i, robj *newval) {
    robj *oldval;
    retainOriginalCommandVector(c);

    /* We need to handle both extending beyond argc (just update it and
     * initialize the new element) or beyond argv_len (realloc is needed).
     */
    if (i >= c->argc) {
        if (i >= c->argv_len) {
            c->argv = zrealloc(c->argv, sizeof(robj *) * (i + 1));
            c->argv_len = i + 1;
        }
        c->argc = i + 1;
        c->argv[i] = NULL;
    }
    oldval = c->argv[i];
    if (oldval)
        c->argv_len_sum -= getStringObjectLen(oldval);
    if (newval)
        c->argv_len_sum += getStringObjectLen(newval);
    c->argv[i] = newval;
    incrRefCount(newval);
    if (oldval)
        decrRefCount(oldval);

    /* If this is the command name make sure to fix c->cmd. */
    if (i == 0) {
        c->cmd = lookupCommandOrOriginal(c->argv, c->argc);
        serverAssertWithInfo(c, NULL, c->cmd != NULL);
    }
}

// 这个函数返回Redis用来存储客户端还没有读取的要回复的字节数.
// 注意:这个函数非常快.这个函数目前的主要用途是强制客户端输出长度限制.
size_t getClientOutputBufferMemoryUsage(client *c) {
    if (getClientType(c) == CLIENT_TYPE_SLAVE) { // 从节点
        size_t repl_buf_size = 0;
        size_t repl_node_num = 0;
        size_t repl_node_size = sizeof(listNode) + sizeof(replBufBlock);
        if (c->ref_repl_buf_node) {                                                  // 复制缓冲区块的引用节点
            replBufBlock *last = listNodeValue(listLast(server.repl_buffer_blocks)); // server端最新的数据
            // server端的复制日志,是用链表存储,每个节点都有一定量的数据
            replBufBlock *cur = listNodeValue(c->ref_repl_buf_node);           // 从节点要发送的数据
            repl_buf_size = last->repl_offset + last->size - cur->repl_offset; // 从节点距离最新的数据  的一个差值
            repl_node_num = last->id - cur->id + 1;                            // 节点数
        }
        return repl_buf_size + (repl_node_size * repl_node_num); // 数据量+ 存储的节点大小*数量
    }
    else {
        size_t list_item_size = sizeof(listNode) + sizeof(clientReplyBlock);
        return c->reply_bytes + (list_item_size * listLength(c->reply)); // todo 回复链表中对象的总大小 +
    }
}

// 返回客户端总的内存使用情况
size_t getClientMemoryUsage(client *c, size_t *output_buffer_mem_usage) {
    // 内存包括输出缓冲区、查询缓冲区、client数据结构本身占用、事务、pubsub、redis6.0中支持客户端本地缓存功能占用的内存等
    size_t mem = getClientOutputBufferMemoryUsage(c);
    if (output_buffer_mem_usage != NULL) {
        *output_buffer_mem_usage = mem;
    }
    mem += sdsZmallocSize(c->querybuf);
    mem += zmalloc_size(c);
    mem += c->buf_usable_size;
    /* For efficiency (less work keeping track of the argv memory), it doesn't include the used memory
     * i.e. unused sds space and internal fragmentation, just the string length. but this is enough to
     * spot problematic clients. */
    mem += c->argv_len_sum + sizeof(robj *) * c->argc;
    mem += multiStateMemOverhead(c);

    /* Add memory overhead of pubsub channels and patterns. Note: this is just the overhead of the robj pointers
     * to the strings themselves because they aren't stored per client. */
    mem += listLength(c->pubsub_patterns) * sizeof(listNode);
    mem += dictSize(c->pubsub_channels) * sizeof(dictEntry) + dictSlots(c->pubsub_channels) * sizeof(dictEntry *);

    /* Add memory overhead of the tracking prefixes, this is an underestimation so we don't need to traverse the entire rax */
    if (c->client_tracking_prefixes)
        mem += c->client_tracking_prefixes->numnodes * (sizeof(raxNode) * sizeof(raxNode *));

    return mem;
}

int getClientType(client *c) {
    if (c->flags & CLIENT_MASTER)
        return CLIENT_TYPE_MASTER;
    if ((c->flags & CLIENT_SLAVE) && !(c->flags & CLIENT_MONITOR))
        return CLIENT_TYPE_SLAVE;
    if (c->flags & CLIENT_PUBSUB)
        return CLIENT_TYPE_PUBSUB;
    return CLIENT_TYPE_NORMAL;
}

int getClientTypeByName(char *name) {
    if (!strcasecmp(name, "normal"))
        return CLIENT_TYPE_NORMAL;
    else if (!strcasecmp(name, "slave"))
        return CLIENT_TYPE_SLAVE;
    else if (!strcasecmp(name, "replica"))
        return CLIENT_TYPE_SLAVE;
    else if (!strcasecmp(name, "pubsub"))
        return CLIENT_TYPE_PUBSUB;
    else if (!strcasecmp(name, "master"))
        return CLIENT_TYPE_MASTER;
    else
        return -1;
}

char *getClientTypeName(int class) {
    switch (class) {
        case CLIENT_TYPE_NORMAL:
            return "normal";
        case CLIENT_TYPE_SLAVE:
            return "slave";
        case CLIENT_TYPE_PUBSUB:
            return "pubsub";
        case CLIENT_TYPE_MASTER:
            return "master";
        default:
            return NULL;
    }
}

/* The function checks if the client reached output buffer soft or hard
 * limit, and also update the state needed to check the soft limit as
 * a side effect.
 *
 * Return value: non-zero if the client reached the soft or the hard limit.
 *               Otherwise zero is returned. */
int checkClientOutputBufferLimits(client *c) {
    int soft = 0, hard = 0, class;
    unsigned long used_mem = getClientOutputBufferMemoryUsage(c);

    class = getClientType(c);
    /* For the purpose of output buffer limiting, masters are handled
     * like normal clients. */
    if (class == CLIENT_TYPE_MASTER)
        class = CLIENT_TYPE_NORMAL;

    /* Note that it doesn't make sense to set the replica clients output buffer
     * limit lower than the repl-backlog-size config (partial sync will succeed
     * and then replica will get disconnected).
     * Such a configuration is ignored (the size of repl-backlog-size will be used).
     * This doesn't have memory consumption implications since the replica client
     * will share the backlog buffers memory. */
    size_t hard_limit_bytes = server.client_obuf_limits[class].hard_limit_bytes;
    if (class == CLIENT_TYPE_SLAVE && hard_limit_bytes && (long long)hard_limit_bytes < server.repl_backlog_size)
        hard_limit_bytes = server.repl_backlog_size;
    if (server.client_obuf_limits[class].hard_limit_bytes && used_mem >= hard_limit_bytes)
        hard = 1;
    if (server.client_obuf_limits[class].soft_limit_bytes && used_mem >= server.client_obuf_limits[class].soft_limit_bytes)
        soft = 1;

    /* We need to check if the soft limit is reached continuously for the
     * specified amount of seconds. */
    if (soft) {
        if (c->obuf_soft_limit_reached_time == 0) {
            c->obuf_soft_limit_reached_time = server.unixtime;
            soft = 0; /* First time we see the soft limit reached */
        }
        else {
            time_t elapsed = server.unixtime - c->obuf_soft_limit_reached_time;

            if (elapsed <= server.client_obuf_limits[class].soft_limit_seconds) {
                soft = 0; /* The client still did not reached the max number of
                             seconds for the soft limit to be considered
                             reached. */
            }
        }
    }
    else {
        c->obuf_soft_limit_reached_time = 0;
    }
    return soft || hard;
}

// 如果达到输出缓冲区大小的软限制或硬限制,异步关闭客户端.调用者可以检查客户机是否将被关闭,检查客户机CLIENT_CLOSE_ASAP标志是否被设置.
// 注意:我们需要异步关闭客户端,因为这个函数是在客户端不能安全释放的情况下调用的,即从较低级别的函数在客户端输出缓冲区中推送数据.
// 当 'async' 被设置为0时,我们立即关闭客户端,这在从cron调用时非常有用.
// 如果客户端被(标记)关闭,返回1.
int closeClientOnOutputBufferLimitReached(client *c, int async) {
    if (!c->conn)
        return 0; /* It is unsafe to free fake clients. */
    serverAssert(c->reply_bytes < SIZE_MAX - (1024 * 64));
    /* Note that c->reply_bytes is irrelevant for replica clients
     * (they use the global repl buffers). */
    if ((c->reply_bytes == 0 && getClientType(c) != CLIENT_TYPE_SLAVE) || c->flags & CLIENT_CLOSE_ASAP)
        return 0;
    if (checkClientOutputBufferLimits(c)) {
        sds client = catClientInfoString(sdsempty(), c);

        if (async) {
            freeClientAsync(c);
            serverLog(LL_WARNING, "Client %s scheduled to be closed ASAP for overcoming of output buffer limits.", client);
        }
        else {
            freeClient(c);
            serverLog(LL_WARNING, "Client %s closed for overcoming of output buffer limits.", client);
        }
        sdsfree(client);
        return 1;
    }
    return 0;
}

/* Helper function used by performEvictions() in order to flush slaves
 * output buffers without returning control to the event loop.
 * This is also called by SHUTDOWN for a best-effort attempt to send
 * slaves the latest writes. */
void flushSlavesOutputBuffers(void) {
    listIter li;
    listNode *ln;

    listRewind(server.slaves, &li);
    while ((ln = listNext(&li))) {
        client *slave = listNodeValue(ln);
        int can_receive_writes = connHasWriteHandler(slave->conn) || (slave->flags & CLIENT_PENDING_WRITE);

        /* We don't want to send the pending data to the replica in a few
         * cases:
         *
         * 1. For some reason there is neither the write handler installed
         *    nor the client is flagged as to have pending writes: for some
         *    reason this replica may not be set to receive data. This is
         *    just for the sake of defensive programming.
         *
         * 2. The put_online_on_ack flag is true. To know why we don't want
         *    to send data to the replica in this case, please grep for the
         *    flag for this flag.
         *
         * 3. Obviously if the slave is not ONLINE.
         */
        if (slave->replstate == SLAVE_STATE_ONLINE && can_receive_writes && !slave->repl_start_cmd_stream_on_ack && clientHasPendingReplies(slave)) {
            writeToClient(slave, 0);
        }
    }
}

// TODO 计算当前最具限制性的暂停类型及其结束时间,为所有暂停目的聚合.
static void updateClientPauseTypeAndEndTime(void) {
    pause_type old_type = server.client_pause_type;
    pause_type type = CLIENT_PAUSE_OFF;
    mstime_t end = 0;
    for (int i = 0; i < NUM_PAUSE_PURPOSES; i++) {
        pause_event *p = server.client_pause_per_purpose[i];
        if (p == NULL) { /* Nothing to do. */
        }
        else if (p->end < server.mstime) {
            /* This one expired. */
            zfree(p);
            server.client_pause_per_purpose[i] = NULL;
        }
        else if (p->type > type) {
            /* This type is the most restrictive so far. */
            type = p->type;
        }
    }

    /* Find the furthest end time among the pause purposes of the most
     * restrictive type */
    for (int i = 0; i < NUM_PAUSE_PURPOSES; i++) {
        pause_event *p = server.client_pause_per_purpose[i];
        if (p != NULL && p->type == type && p->end > end)
            end = p->end;
    }
    server.client_pause_type = type;
    server.client_pause_end_time = end;

    /* If the pause type is less restrictive than before, we unblock all clients
     * so they are reprocessed (may get re-paused). */
    if (type < old_type) {
        unblockPostponedClients();
    }
}

/* Unblock all paused clients (ones that where blocked by BLOCKED_POSTPONE (possibly in processCommand).
 * This means they'll get re-processed in beforeSleep, and may get paused again if needed. */
void unblockPostponedClients() {
    listNode *ln;
    listIter li;
    listRewind(server.postponed_clients, &li);
    while ((ln = listNext(&li)) != NULL) {
        client *c = listNodeValue(ln);
        unblockClient(c);
    }
}

// 对于给定类型的命令,将客户端暂停到指定的unixtime(毫秒).
// 此功能的一个主要用例是允许暂停复制流量,以便在不发生数据丢失的情况下进行故障转移.副本将继续接收流量以促进此功能.
// 此功能也被Redis集群内部用于由Cluster failover实现的手动故障转移过程.
// 函数总是成功的,即使在进程中已经有一个暂停.在这种情况下,将持续时间设置为最大和新的结束时间,并将类型设置为更严格的暂停类型.
void pauseClients(pause_purpose purpose, mstime_t end, pause_type type) {
    // 管理每个暂停目的的暂停类型和结束时间.
    if (server.client_pause_per_purpose[purpose] == NULL) {
        server.client_pause_per_purpose[purpose] = zmalloc(sizeof(pause_event));
        server.client_pause_per_purpose[purpose]->type = type;
        server.client_pause_per_purpose[purpose]->end = end;
    }
    else {
        pause_event *p = server.client_pause_per_purpose[purpose];
        p->type = max(p->type, type);
        p->end = max(p->end, end);
    }
    updateClientPauseTypeAndEndTime();

    /* We allow write commands that were queued
     * up before and after to execute. We need
     * to track this state so that we don't assert
     * in propagateNow(). */
    if (server.in_exec) {
        server.client_pause_in_transaction = 1;
    }
}

/* Unpause clients and queue them for reprocessing. */
void unpauseClients(pause_purpose purpose) {
    if (server.client_pause_per_purpose[purpose] == NULL)
        return;
    zfree(server.client_pause_per_purpose[purpose]);
    server.client_pause_per_purpose[purpose] = NULL;
    updateClientPauseTypeAndEndTime();
}

// 如果客户端暂停则返回true,否则返回false.
int areClientsPaused(void) {
    return server.client_pause_type != CLIENT_PAUSE_OFF;
}

// 检查当前客户端暂停是否已经结束,如果已经结束则取消暂停.如果客户端现在暂停,也返回true,否则返回false.
int checkClientPauseTimeoutAndReturnIfPaused(void) {
    if (!areClientsPaused()) {
        return 0; // 没暂停
    }
    if (server.client_pause_end_time < server.mstime) {
        updateClientPauseTypeAndEndTime();
    }
    return areClientsPaused();
}

/* This function is called by Redis in order to process a few events from
 * time to time while blocked into some not interruptible operation.
 * This allows to reply to clients with the -LOADING error while loading the
 * data set at startup or after a full resynchronization with the master
 * and so forth.
 *
 * It calls the event loop in order to process a few events. Specifically we
 * try to call the event loop 4 times as long as we receive acknowledge that
 * some event was processed, in order to go forward with the accept, read,
 * write, close sequence needed to serve a client.
 *
 * The function returns the total number of events processed. */
void processEventsWhileBlocked(void) {
    int iterations = 4; /* See the function top-comment. */

    /* Update our cached time since it is used to create and update the last
     * interaction time with clients and for other important things. */
    updateCachedTime(0);

    /* Note: when we are processing events while blocked (for instance during
     * busy Lua scripts), we set a global flag. When such flag is set, we
     * avoid handling the read part of clients using threaded I/O.
     * See https://github.com/redis/redis/issues/6988 for more info.
     * Note that there could be cases of nested calls to this function,
     * specifically on a busy script during async_loading rdb, and scripts
     * that came from AOF. */
    ProcessingEventsWhileBlocked++;
    while (iterations--) {
        long long startval = server.events_processed_while_blocked;
        long long ae_events = aeProcessEvents(server.el, AE_FILE_EVENTS | AE_DONT_WAIT | AE_CALL_BEFORE_SLEEP | AE_CALL_AFTER_SLEEP);
        /* Note that server.events_processed_while_blocked will also get
         * incremented by callbacks called by the event loop handlers. */
        server.events_processed_while_blocked += ae_events;
        long long events = server.events_processed_while_blocked - startval;
        if (!events)
            break;
    }

    whileBlockedCron();

    ProcessingEventsWhileBlocked--;
    serverAssert(ProcessingEventsWhileBlocked >= 0);
}

/* ==========================================================================
 * Threaded I/O
 * ========================================================================== */

#define IO_THREADS_MAX_NUM 128

pthread_t io_threads[IO_THREADS_MAX_NUM];                         // 记录线程描述符的数组
pthread_mutex_t io_threads_mutex[IO_THREADS_MAX_NUM];             // 记录线程互斥锁的数组
redisAtomic unsigned long io_threads_pending[IO_THREADS_MAX_NUM]; // 记录线程待处理的客户端个数     ,主线程向每个IO线程分配客户端

/* IO_THREADS_OP_IDLE, IO_THREADS_OP_READ or IO_THREADS_OP_WRITE. */ // TODO: should access to this be atomic??!
int io_threads_op;

list *io_threads_list[IO_THREADS_MAX_NUM]; // 记录每个IO线程 要处理的客户端  [read+parse]

// 获取每个IO线程 仍在阻塞的客户端
static inline unsigned long getIOPendingCount(int i) {
    unsigned long count = 0;
    atomicGetWithSync(io_threads_pending[i], count); // 原子取, 不成功一直循环
    return count;
}

// OK
static inline void setIOPendingCount(int i, unsigned long count) {
    atomicSetWithSync(io_threads_pending[i], count); // 原子设置, 不成功一直循环
}

void *IOThreadMain(void *myid) {
    /* The ID is the thread number (from 0 to server.iothreads_num-1), and is
     * used by the thread to just manipulate a single sub-array of clients. */
    long id = (unsigned long)myid;
    char thdname[16]; // io_thd_1

    snprintf(thdname, sizeof(thdname), "io_thd_%ld", id);
    redis_set_thread_title(thdname);
    redisSetCpuAffinity(server.server_cpulist);
    makeThreadKillable();
    while (1) {
        // 等待启动
        for (int j = 0; j < 1000000; j++) {
            if (getIOPendingCount(id) != 0) {
                break;
            }
        }

        /* 让主线程有机会停止这个线程. */
        if (getIOPendingCount(id) == 0) {
            pthread_mutex_lock(&io_threads_mutex[id]); // 子线程内
            pthread_mutex_unlock(&io_threads_mutex[id]);
            continue;
        }

        serverAssert(getIOPendingCount(id) != 0);

        /* Process: note that the main thread will never touch our list
         * before we drop the pending count to 0. */
        listIter li;
        listNode *ln;
        // 获取IO线程要处理的客户端列表
        listRewind(io_threads_list[id], &li);
        // 遍历线程 id 获取线程对应的待处理连接列表
        while ((ln = listNext(&li))) {
            client *c = listNodeValue(ln); // 从客户端列表中获取一个客户端
            if (io_threads_op == IO_THREADS_OP_WRITE) {
                // 如果线程操作是写操作,则调用writeToClient将数据写回客户端
                writeToClient(c, 0);
            }
            else if (io_threads_op == IO_THREADS_OP_READ) {
                // 如果线程操作是读操作,则调用readQueryFromClient从客户端读取数据
                readQueryFromClient(c->conn);
            }
            else {
                serverPanic("io_threads_op value is unknown");
            }
        }
        // 处理完所有客户端后,清空该线程的客户端列表
        listEmpty(io_threads_list[id]);
        // 将该线程的待处理任务数量设置为0
        setIOPendingCount(id, 0);
    }
}

// 初始化多线程io所需的数据结构
void initThreadedIO(void) {
    server.io_threads_active = 0; // 表示IO线程还没有被激活

    /* Indicate that io-threads are currently idle */
    io_threads_op = IO_THREADS_OP_IDLE;
    if (server.io_threads_num == 1) {
        // 设置只有1个主IO线程,和6.0之前效果一样
        return;
    }

    if (server.io_threads_num > IO_THREADS_MAX_NUM) { // 128
        serverLog(LL_WARNING, "配置了太多的IO线程,数量为:%d", IO_THREADS_MAX_NUM);
        exit(1);
    }

    // 生成并初始化I/O线程.
    for (int i = 0; i < server.io_threads_num; i++) {
        // 链表结构,保存客户端
        io_threads_list[i] = listCreate();
        if (i == 0) {
            continue; // 线程0是主线程
        }

        pthread_t tid;
        pthread_mutex_init(&io_threads_mutex[i], NULL); // 初始化io_threads_mutex数组
        setIOPendingCount(i, 0);                        // 初始化io_threads_pending数组
        pthread_mutex_lock(&io_threads_mutex[i]);       // 初始时 线程会阻塞
        // 把创建的 proc 绑在编号为arg的逻辑核上
        // 调用pthread_create函数创建IO线程,线程运行函数为IOThreadMain
        if (pthread_create(&tid, NULL, IOThreadMain, (void *)(long)i) != 0) {
            serverLog(LL_WARNING, "Fatal: 不能初始化IO线程");
            exit(1);
        }
        io_threads[i] = tid; // 初始化io_threads数组,设置值为线程标识
    }
}

// 只由主线程调用
void killIOThreads(void) {
    int err, j;
    for (j = 0; j < server.io_threads_num; j++) {
        if (io_threads[j] == pthread_self())
            continue;
        if (io_threads[j] && pthread_cancel(io_threads[j]) == 0) {
            if ((err = pthread_join(io_threads[j], NULL)) != 0) {
                serverLog(LL_WARNING, "IO线程(tid:%lu) 无法joined: %s", (unsigned long)io_threads[j], strerror(err));
            }
            else {
                serverLog(LL_WARNING, "IO线程(tid:%lu) 已终止", (unsigned long)io_threads[j]);
            }
        }
    }
}

// 只由主线程调用
void startThreadedIO(void) {
    serverAssert(server.io_threads_active == 0);
    for (int j = 1; j < server.io_threads_num; j++) {
        pthread_mutex_unlock(&io_threads_mutex[j]); // 启动每个线程
    }
    server.io_threads_active = 1;
}

// 只由主线程调用
void stopThreadedIO(void) {
    /* 当调用此函数时,可能仍然有客户端挂起读取:在停止线程之前处理它们.*/
    handleClientsWithPendingReadsUsingThreads();
    serverAssert(server.io_threads_active == 1);
    for (int j = 1; j < server.io_threads_num; j++) {
        pthread_mutex_lock(&io_threads_mutex[j]); // 停止子线程
    }
    server.io_threads_active = 0;
}

// OK
int stopThreadedIOIfNeeded(void) {
    int pending = listLength(server.clients_pending_write);

    // 立即返回,如果io threads 被禁用
    if (server.io_threads_num == 1)
        return 1;

    if (pending < (server.io_threads_num * 2)) {
        if (server.io_threads_active)
            stopThreadedIO();
        return 1;
    }
    else {
        return 0;
    }
}

/* This function achieves thread safety using a fan-out -> fan-in paradigm:
 * Fan out: The main thread fans out work to the io-threads which block until
 * setIOPendingCount() is called with a value larger than 0 by the main thread.
 * Fan in: The main thread waits until getIOPendingCount() returns 0. Then
 * it can safely perform post-processing and return to normal synchronous
 * work. */
int handleClientsWithPendingWritesUsingThreads(void) {
    int processed = listLength(server.clients_pending_write);
    if (processed == 0)
        return 0; /* Return ASAP if there are no clients. */

    /* If I/O threads are disabled or we have few clients to serve, don't
     * use I/O threads, but the boring synchronous code. */
    if (server.io_threads_num == 1 || stopThreadedIOIfNeeded()) {
        //      stopThreadedIOIfNeeded 函数一旦发现待处理任务数,不足 IO 线程数的 2 倍,它就会调用 stopThreadedIO 函数来暂停 IO 线程.
        return handleClientsWithPendingWrites();
    }

    // 如果有必要的话,启动所有线程
    if (!server.io_threads_active) {
        startThreadedIO();
    }

    /* Distribute the clients across N different lists. */
    listIter li;
    listNode *ln;
    listRewind(server.clients_pending_write, &li);
    int item_id = 0;
    while ((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        c->flags &= ~CLIENT_PENDING_WRITE;

        /* Remove clients from the list of pending writes since
         * they are going to be closed ASAP. */
        if (c->flags & CLIENT_CLOSE_ASAP) {
            listDelNode(server.clients_pending_write, ln);
            continue;
        }

        /* Since all replicas and replication backlog use global replication
         * buffer, to guarantee data accessing thread safe, we must put all
         * replicas client into io_threads_list[0] i.e. main thread handles
         * sending the output buffer of all replicas. */
        if (getClientType(c) == CLIENT_TYPE_SLAVE) {
            listAddNodeTail(io_threads_list[0], c);
            continue;
        }

        int target_id = item_id % server.io_threads_num;
        listAddNodeTail(io_threads_list[target_id], c);
        item_id++;
    }

    /* Give the start condition to the waiting threads, by setting the
     * start condition atomic var. */
    io_threads_op = IO_THREADS_OP_WRITE;
    for (int j = 1; j < server.io_threads_num; j++) {
        int count = listLength(io_threads_list[j]);
        setIOPendingCount(j, count);
    }

    /* 也可以使用主线程来处理客户端的一个切片.*/
    listRewind(io_threads_list[0], &li);
    while ((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        writeToClient(c, 0);
    }
    listEmpty(io_threads_list[0]);

    /* 等待所有其他线程结束它们的工作.*/
    while (1) {
        unsigned long pending = 0;
        for (int j = 1; j < server.io_threads_num; j++) pending += getIOPendingCount(j);
        if (pending == 0)
            break;
    }

    io_threads_op = IO_THREADS_OP_IDLE;

    listRewind(server.clients_pending_write, &li);
    while ((ln = listNext(&li))) {
        client *c = listNodeValue(ln);

        /* Update the client in the mem usage after we're done processing it in the io-threads */
        updateClientMemUsage(c);

        /* Install the write handler if there are pending writes in some
         * of the clients. */
        if (clientHasPendingReplies(c)) {
            installClientWriteHandler(c);
        }
    }
    listEmpty(server.clients_pending_write);

    /* Update processed count on server */
    server.stat_io_writes_processed += processed;

    return processed;
}

// 如果我们想稍后使用线程I/O处理客户端读取,返回1.
// 这由事件循环的可读处理程序调用.作为调用这个函数的一个副作用,客户端会被放入挂起的读客户端中并被标记.
int postponeClientRead(client *c) {
    // 延迟读,不能是master,不能是slave,不能是阻塞的客户端
    if (server.io_threads_active &&
        // 多线程 IO 是否在开启状态,在待处理请求较少时会停止 IO多线程
        server.io_threads_do_reads && // 读是否开启多线程 IO
        !ProcessingEventsWhileBlocked && !(c->flags & (CLIENT_MASTER | CLIENT_SLAVE | CLIENT_BLOCKED)) &&
        // 主从库复制请求不使用多线程 IO
        io_threads_op == IO_THREADS_OP_IDLE // 尚没有启动多线程
    ) {
        listAddNodeHead(server.clients_pending_read, c); // 连接加入到等待读处理队列
        c->pending_read_list_node = listFirst(server.clients_pending_read);
        return 1;
    }
    else {
        return 0;
    }
}

// 使用线程 处理阻塞在读得客户端          处理包括[读数据,解析数据,将解析好异步的放入队列]  使用线程队列
int handleClientsWithPendingReadsUsingThreads(void) {
    if (!server.io_threads_active || !server.io_threads_do_reads) {
        // 没被激活、禁止从IO线程读取解析数据
        return 0;
    }

    int processed = listLength(server.clients_pending_read);
    if (processed == 0)
        return 0;

    // 将客户端分布在N个不同的列表中.
    listIter li;
    listNode *ln;
    listRewind(server.clients_pending_read, &li);
    int item_id = 0; // 记录阻塞的客户端数量
    // 将等待处理队列的连接按照 RR 的方式分配给多个 IO 线程
    while ((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        int target_id = item_id % server.io_threads_num; //
        listAddNodeTail(io_threads_list[target_id], c);
        item_id++;
    }

    /* 通过设置启动条件原子变量,为等待的线程提供启动条件.*/
    io_threads_op = IO_THREADS_OP_READ; // IO线程要开始读了
    for (int j = 1; j < server.io_threads_num; j++) {
        int count = listLength(io_threads_list[j]);
        setIOPendingCount(j, count);
    }

    /*也可以使用主线程来处理客户端的一个切片.*/
    listRewind(io_threads_list[0], &li); // 重用一个迭代器,
    while ((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        readQueryFromClient(c->conn);
    }
    listEmpty(io_threads_list[0]);

    /*等待所有其他线程结束它们的工作.*/
    while (1) {
        unsigned long pending = 0;
        for (int j = 1; j < server.io_threads_num; j++) {
            pending += getIOPendingCount(j);
        }
        if (pending == 0) {
            break;
        }
    }

    io_threads_op = IO_THREADS_OP_IDLE; // 空闲了

    /* 再次run clients_pending_read 来处理新的缓冲区. */
    while (listLength(server.clients_pending_read)) {
        ln = listFirst(server.clients_pending_read);
        client *c = listNodeValue(ln);
        listDelNode(server.clients_pending_read, ln);
        c->pending_read_list_node = NULL;

        serverAssert(!(c->flags & CLIENT_BLOCKED));

        if (beforeNextClient(c) == C_ERR) {
            /* If the client is no longer valid, we avoid processing the client later. So we just go to the next. */
            continue;
        }

        // 一旦IO线程空闲, 更新客户端的内存使用
        updateClientMemUsage(c);

        if (processPendingCommandAndInputBuffer(c) == C_ERR) {
            /* If the client is no longer valid, we avoid processing the client later. So we just go  to the next. */
            continue;
        }

        /* We may have pending replies if a thread readQueryFromClient() produced
         * replies and did not put the client in pending write queue (it can't).
         */
        if (!(c->flags & CLIENT_PENDING_WRITE) && clientHasPendingReplies(c))
            putClientInPendingWriteQueue(c);
    }

    // 更新计数
    server.stat_io_reads_processed += processed;

    return processed;
}

// 返回要驱逐的客户端数量限制
size_t getClientEvictionLimit(void) {
    size_t maxmemory_clients_actual = SIZE_MAX; // 18446744073709551615UL

    /* 处理maxmemory的百分比 */
    if (server.maxmemory_clients < 0 && server.maxmemory > 0) {
        unsigned long long maxmemory_clients_bytes = (unsigned long long)((double)server.maxmemory * -(double)server.maxmemory_clients / 100);
        if (maxmemory_clients_bytes <= SIZE_MAX) {
            maxmemory_clients_actual = maxmemory_clients_bytes;
        }
    }
    else if (server.maxmemory_clients > 0) {
        maxmemory_clients_actual = server.maxmemory_clients;
    }
    else
        return 0;

    /* 不要允许一个太小的最大内存客户端,以避免由于糟糕的配置而无法与服务器进行通信的情况 */
    if (maxmemory_clients_actual < 1024 * 128) {
        maxmemory_clients_actual = 1024 * 128;
    }

    return maxmemory_clients_actual;
}

// 驱逐客户端
void evictClients(void) {
    /* 从最顶层桶(最大的客户端)开始退出 */
    int curr_bucket = CLIENT_MEM_USAGE_BUCKETS - 1; // 18
    listIter bucket_iter;
    listRewind(server.client_mem_usage_buckets[curr_bucket].clients, &bucket_iter);
    size_t client_eviction_limit = getClientEvictionLimit();
    if (client_eviction_limit == 0) {
        return;
    }

    // CLIENT_TYPE_NORMAL 0     // 正常的 请求-应答客户端、监视器
    // CLIENT_TYPE_PUBSUB 2     // 客户端订阅了PubSub频道.

    while (server.stat_clients_type_memory[CLIENT_TYPE_NORMAL] + server.stat_clients_type_memory[CLIENT_TYPE_PUBSUB] >= client_eviction_limit) {
        listNode *ln = listNext(&bucket_iter);
        if (ln) {
            client *c = ln->value;
            sds ci = catClientInfoString(sdsempty(), c);
            serverLog(LL_NOTICE, "驱逐客户端ing : %s", ci);
            freeClient(c); // 有可能会将 server.current_client = NULL
            sdsfree(ci);
            server.stat_evictedclients++;
        }
        else {
            curr_bucket--;
            if (curr_bucket < 0) {
                serverLog(LL_WARNING, "在退出所有可退出客户端后,超过客户端maxmemory");
                break;
            }
            listRewind(server.client_mem_usage_buckets[curr_bucket].clients, &bucket_iter);
        }
    }
}
