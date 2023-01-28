/*
 * Copyright (c) 2013, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include "server.h"

// 解析允许事件通知类型
int keyspaceEventsStringToFlags(char *classes) {
    char *p = classes;
    int   c, flags = 0;

    while ((c = *p++) != '\0') {
        switch (c) {
            case 'A':
                flags |= NOTIFY_ALL;
                break;
            case 'g':
                flags |= NOTIFY_GENERIC;
                break;
            case '$':
                flags |= NOTIFY_STRING;
                break;
            case 'l':
                flags |= NOTIFY_LIST;
                break;
            case 's':
                flags |= NOTIFY_SET;
                break;
            case 'h':
                flags |= NOTIFY_HASH;
                break;
            case 'z':
                flags |= NOTIFY_ZSET;
                break;
            case 'x':
                flags |= NOTIFY_EXPIRED;
                break;
            case 'e':
                flags |= NOTIFY_EVICTED;
                break;
            case 'K':
                flags |= NOTIFY_KEYSPACE;
                break;
            case 'E':
                flags |= NOTIFY_KEYEVENT;
                break;
            case 't':
                flags |= NOTIFY_STREAM;
                break;
            case 'm':
                flags |= NOTIFY_KEY_MISS;
                break;
            case 'd':
                flags |= NOTIFY_MODULE;
                break;
            case 'n':
                flags |= NOTIFY_NEW;
                break;
            default:
                return -1;
        }
    }
    return flags;
}

// 它得到一个带有xored标志的整数作为输入,并返回一个代表所选类的字符串.
// 返回的字符串是一个sds字符串,需要用sdsfree()来释放.
sds keyspaceEventsFlagsToString(int flags) {
    sds res;

    res = sdsempty();
    if ((flags & NOTIFY_ALL) == NOTIFY_ALL) {
        res = sdscatlen(res, "A", 1);
    }
    else {
        if (flags & NOTIFY_GENERIC)
            res = sdscatlen(res, "g", 1);
        if (flags & NOTIFY_STRING)
            res = sdscatlen(res, "$", 1);
        if (flags & NOTIFY_LIST)
            res = sdscatlen(res, "l", 1);
        if (flags & NOTIFY_SET)
            res = sdscatlen(res, "s", 1);
        if (flags & NOTIFY_HASH)
            res = sdscatlen(res, "h", 1);
        if (flags & NOTIFY_ZSET)
            res = sdscatlen(res, "z", 1);
        if (flags & NOTIFY_EXPIRED)
            res = sdscatlen(res, "x", 1);
        if (flags & NOTIFY_EVICTED)
            res = sdscatlen(res, "e", 1);
        if (flags & NOTIFY_STREAM)
            res = sdscatlen(res, "t", 1);
        if (flags & NOTIFY_MODULE)
            res = sdscatlen(res, "d", 1);
        if (flags & NOTIFY_NEW)
            res = sdscatlen(res, "n", 1);
    }
    if (flags & NOTIFY_KEYSPACE)
        res = sdscatlen(res, "K", 1);
    if (flags & NOTIFY_KEYEVENT)
        res = sdscatlen(res, "E", 1);
    if (flags & NOTIFY_KEY_MISS)
        res = sdscatlen(res, "m", 1);
    return res;
}

// type 通知的类型, 据此判断是不是 notify-keyspace-events选项所酸丁的值,从而决定是否发送通知
// event、keys、dbid 分别是事件的名称、产生事件的键、产生事件的数据库号码,
// 构建事件通知内容、接收通知的频道名
void notifyKeyspaceEvent(int type, char *event, robj *key, int dbid) {
    sds   chan;
    robj *chanobj, *eventobj;
    int   len = -1;
    char  buf[24];
    /* If any modules are interested in events, notify the module system now.
     * This bypasses the notifications configuration, but the module engine
     * will only call event subscribers if the event type matches the types
     * they are interested in. */
    moduleNotifyKeyspaceEvent(type, event, key, dbid);

    // 如果给定的通知不是服务器允许发送的通知,那么直接返回
    if (!(server.notify_keyspace_events & type)) {
        return;
    }

    eventobj = createStringObject(event, strlen(event));

    /* __keyspace@<db>__:<key> <event> notifications. */
    // 检测服务器是否允许发送键空间通知
    if (server.notify_keyspace_events & NOTIFY_KEYSPACE) {
        //
        chan = sdsnewlen("__keyspace@", 11);
        len = ll2string(buf, sizeof(buf), dbid); //
        chan = sdscatlen(chan, buf, len);        //
        chan = sdscatlen(chan, "__:", 3);        //
        chan = sdscatsds(chan, key->ptr);        // 发送的频道
        printf("event.chan --> %s\n", print_str(chan));
        chanobj = createObject(OBJ_STRING, chan);   //
        pubsubPublishMessage(chanobj, eventobj, 0); // 发送通知
        decrRefCount(chanobj);                      // 对象的引用计数减1
    }

    /* __keyevent@<db>__:<event> <key> notifications. */
    // 检测服务器是否允许发送键事件通知
    if (server.notify_keyspace_events & NOTIFY_KEYEVENT) {
        chan = sdsnewlen("__keyevent@", 11);
        if (len == -1) {
            len = ll2string(buf, sizeof(buf), dbid); //
        }                                            //
        chan = sdscatlen(chan, buf, len);            //
        chan = sdscatlen(chan, "__:", 3);            //
        chan = sdscatsds(chan, eventobj->ptr);       // 发送的频道
        chanobj = createObject(OBJ_STRING, chan);    //
        printf("event.chan --> %s\n", print_str(chan));
        pubsubPublishMessage(chanobj, key, 0); // 发送通知
        decrRefCount(chanobj);                 // 对象的引用计数减1
    }
    decrRefCount(eventobj); // 对象的引用计数减1
}
