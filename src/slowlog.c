/* Slowlog implements a system that is able to remember the latest N
 * queries that took more than M microseconds to execute.
 *
 * The execution time to reach to be logged in the slow log is set
 * using the 'slowlog-log-slower-than' config directive, that is also
 * readable and writable using the CONFIG SET/GET command.
 *
 * The slow queries log is actually not "logged" in the Redis log file
 * but is accessible thanks to the SLOWLOG command.
 *
 * ----------------------------------------------------------------------------
 *
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
#include "slowlog.h"

// 创建慢日志
slowlogEntry *slowlogCreateEntry(client *c, robj **argv, int argc, long long duration) {
    slowlogEntry *se = zmalloc(sizeof(*se)); // 分配日志项空间
    int j, slargc = argc;                    // 待记录的参数个数,默认为当前命令的参数个数

    // 如果当前命令参数个数超出阈值,则只记录阈值个数的参数
    if (slargc > SLOWLOG_ENTRY_MAX_ARGC) {
        slargc = SLOWLOG_ENTRY_MAX_ARGC;
    }
    se->argc = slargc;
    se->argv = zmalloc(sizeof(robj *) * slargc);
    for (j = 0; j < slargc; j++) { // 逐一记录命令及参数
        // 如果命令参数个数超出阈值,使用最后一个参数记录当前命令实际剩余的参数个数
        if (slargc != argc && j == slargc - 1) {
            se->argv[j] = createObject(OBJ_STRING, sdscatprintf(sdsempty(), "... (%d more arguments)", argc - slargc + 1));
        }
        else {
            /* Trim too long strings as well... */
            if (argv[j]->type == OBJ_STRING && sdsEncodedObject(argv[j]) && sdslen(argv[j]->ptr) > SLOWLOG_ENTRY_MAX_STRING) {
                sds s = sdsnewlen(argv[j]->ptr, SLOWLOG_ENTRY_MAX_STRING);

                s = sdscatprintf(s, "... (%lu more bytes)", (unsigned long)sdslen(argv[j]->ptr) - SLOWLOG_ENTRY_MAX_STRING);
                se->argv[j] = createObject(OBJ_STRING, s);
            }
            else if (argv[j]->refcount == OBJ_SHARED_REFCOUNT) {
                se->argv[j] = argv[j];
            }
            else {
                // 将命令参数填充到日志项中
                se->argv[j] = dupStringObject(argv[j]);
            }
        }
    }
    // 将命令执行时长、客户端地址等信息填充到日志项中
    se->time = time(NULL);
    se->duration = duration;
    se->id = server.slowlog_entry_id++;
    se->peerid = sdsnew(getClientPeerId(c));
    se->cname = c->name ? sdsnew(c->name->ptr) : sdsempty();
    return se;
}

/* 释放慢速日志条目.参数为void,因此该函数的原型与adlist.c中的'free'方法匹配.这个函数将负责释放所有被保留的对象. */
void slowlogFreeEntry(void *septr) {
    slowlogEntry *se = septr;
    int j;

    for (j = 0; j < se->argc; j++) {
        decrRefCount(se->argv[j]);
    }
    zfree(se->argv);
    sdsfree(se->peerid);
    sdsfree(se->cname);
    zfree(se);
}

// 初始化慢日志.这个函数应该在服务器启动时一次调用.
void slowlogInit(void) {
    server.slowlog = listCreate();
    server.slowlog_entry_id = 0;
    listSetFreeMethod(server.slowlog, slowlogFreeEntry);
}

// 将日志项 放入 slow log
void slowlogPushEntryIfNeeded(client *c, robj **argv, int argc, long long duration) {
    if (server.slowlog_log_slower_than < 0)
        return;
    if (duration >= server.slowlog_log_slower_than) {
        listAddNodeHead(server.slowlog, slowlogCreateEntry(c, argv, argc, duration));
    }

    // 移除最新的慢日志  ,只保留
    while (listLength(server.slowlog) > server.slowlog_max_len) {
        listDelNode(server.slowlog, listLast(server.slowlog));
    }
}

/* Remove all the entries from the current slow log. */
void slowlogReset(void) {
    while (listLength(server.slowlog) > 0) listDelNode(server.slowlog, listLast(server.slowlog));
}

/* The SLOWLOG command. Implements all the subcommands needed to handle the
 * Redis slow log. */
void slowlogCommand(client *c) {
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr, "help")) {
        const char *help[] = {"GET [<count>]", "    Return top <count> entries from the slowlog (default: 10, -1 mean all).", "    Entries are made of:", "    id, timestamp, time in microseconds, arguments array, client IP and port,", "    client name", "LEN", "    Return the length of the slowlog.", "RESET", "    Reset the slowlog.", NULL};
        addReplyHelp(c, help);
    }
    else if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr, "reset")) {
        slowlogReset();
        addReply(c, shared.ok);
    }
    else if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr, "len")) {
        addReplyLongLong(c, listLength(server.slowlog));
    }
    else if ((c->argc == 2 || c->argc == 3) && !strcasecmp(c->argv[1]->ptr, "get")) {
        long count = 10, sent = 0;
        listIter li;
        void *totentries;
        listNode *ln;
        slowlogEntry *se;

        if (c->argc == 3) {
            /* Consume count arg. */
            if (getRangeLongFromObjectOrReply(c, c->argv[2], -1, LONG_MAX, &count, "count should be greater than or equal to -1") != C_OK)
                return;

            if (count == -1) {
                /* We treat -1 as a special value, which means to get all slow logs.
                 * Simply set count to the length of server.slowlog.*/
                count = listLength(server.slowlog);
            }
        }

        listRewind(server.slowlog, &li);
        totentries = addReplyDeferredLen(c);
        while (count-- && (ln = listNext(&li))) {
            int j;

            se = ln->value;
            addReplyArrayLen(c, 6);
            addReplyLongLong(c, se->id);
            addReplyLongLong(c, se->time);
            addReplyLongLong(c, se->duration);
            addReplyArrayLen(c, se->argc);
            for (j = 0; j < se->argc; j++) addReplyBulk(c, se->argv[j]);
            addReplyBulkCBuffer(c, se->peerid, sdslen(se->peerid));
            addReplyBulkCBuffer(c, se->cname, sdslen(se->cname));
            sent++;
        }
        setDeferredArrayLen(c, totentries, sent);
    }
    else {
        addReplySubcommandSyntaxError(c);
    }
}
