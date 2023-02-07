/* anet.c -- Basic TCP socket stuff made a bit less boring
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include "over-fmacros.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

#include "over-anet.h"
#include "over-config.h"
#include "over-server.h"

// 打印错误信息
static void anetSetError(char *err, const char *fmt, ...) {
    va_list ap;

    if (!err)
        return;
    va_start(ap, fmt);
    vsnprintf(err, ANET_ERR_LEN, fmt, ap);
    va_end(ap);
}

int anetSetBlock(char *err, int fd, int non_block) {
    int flags;

    /* Set the socket blocking (if non_block is zero) or non-blocking.
     * Note that fcntl(2) for F_GETFL and F_SETFL can't be
     * interrupted by a signal. */
    if ((flags = fcntl(fd, F_GETFL)) == -1) {
        anetSetError(err, "fcntl(F_GETFL): %s", strerror(errno));
        return ANET_ERR;
    }

    /* Check if this flag has been set or unset, if so,
     * then there is no need to call fcntl to set/unset it again. */
    if (!!(flags & O_NONBLOCK) == !!non_block)
        return ANET_OK;

    if (non_block)
        flags |= O_NONBLOCK;
    else
        flags &= ~O_NONBLOCK;

    if (fcntl(fd, F_SETFL, flags) == -1) {
        anetSetError(err, "fcntl(F_SETFL,O_NONBLOCK): %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

// 将 fd 设置为非阻塞模式（O_NONBLOCK）
int anetNonBlock(char *err, int fd) {
    return anetSetBlock(err, fd, 1);
}

int anetBlock(char *err, int fd) {
    return anetSetBlock(err, fd, 0);
}

// 在给定的fd上启用FD_CLOEXEC以避免fd泄漏.这个函数应该在调用fork + execve系统调用的特定位置调用fd.
int anetCloexec(int fd) {
    int r;
    int flags;

    do {
        r = fcntl(fd, F_GETFD);
    } while (r == -1 && errno == EINTR);

    if (r == -1 || (r & FD_CLOEXEC))
        return r;

    flags = r | FD_CLOEXEC;

    do {
        r = fcntl(fd, F_SETFD, flags);
    } while (r == -1 && errno == EINTR);

    return r;
}

// 修改 TCP 连接的 keep alive 选项
// https://zhuanlan.zhihu.com/p/28894266
// https://blog.csdn.net/ComplexMaze/article/details/124201088
int anetKeepAlive(char *err, int fd, int interval) {
    int val = 1; // 1表示打开

    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) == -1) {
        anetSetError(err, "setsockopt SO_KEEPALIVE: %s", strerror(errno));
        return ANET_ERR;
    }

#ifdef __linux__
    /* Default settings are more or less garbage, with the keepalive time
     * set to 7200 by default on Linux. Modify settings to make the feature
     * actually useful. */

    /* Send first probe after interval. */
    val = interval;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val)) < 0) {
        anetSetError(err, "setsockopt TCP_KEEPIDLE: %s\n", strerror(errno));
        return ANET_ERR;
    }

    /* Send next probes after the specified interval. Note that we set the
     * delay as interval / 3, as we send three probes before detecting
     * an error (see the next setsockopt call). */
    val = interval / 3;
    if (val == 0)
        val = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val)) < 0) {
        anetSetError(err, "setsockopt TCP_KEEPINTVL: %s\n", strerror(errno));
        return ANET_ERR;
    }

    /* Consider the socket in error state after three we send three ACK
     * probes without getting a reply. */
    val = 3;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val)) < 0) {
        anetSetError(err, "setsockopt TCP_KEEPCNT: %s\n", strerror(errno));
        return ANET_ERR;
    }
#else
    ((void)interval); /* Avoid unused var warning for non Linux systems. */
#endif

    return ANET_OK;
}

// 打开或关闭 Nagle 算法
static int anetSetTcpNoDelay(char *err, int fd, int val) {
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) == -1) {
        anetSetError(err, "setsockopt TCP_NODELAY: %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

// 禁用 Nagle 算法
int anetEnableTcpNoDelay(char *err, int fd) {
    // 启动TCP_NODELAY,就意味着禁用了Nagle算法,允许小包的发送.
    // https://blog.csdn.net/qq_32907195/article/details/120287099
    return anetSetTcpNoDelay(err, fd, 1);
}

// 启用 Nagle 算法
int anetDisableTcpNoDelay(char *err, int fd) {
    return anetSetTcpNoDelay(err, fd, 0);
}

/* Set the socket send timeout (SO_SNDTIMEO socket option) to the specified
 * number of milliseconds, or disable it if the 'ms' argument is zero. */
int anetSendTimeout(char *err, int fd, long long ms) {
    struct timeval tv;

    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == -1) {
        anetSetError(err, "setsockopt SO_SNDTIMEO: %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

/* Set the socket receive timeout (SO_RCVTIMEO socket option) to the specified
 * number of milliseconds, or disable it if the 'ms' argument is zero. */
int anetRecvTimeout(char *err, int fd, long long ms) {
    struct timeval tv;

    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1) {
        anetSetError(err, "setsockopt SO_RCVTIMEO: %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

/* Resolve the hostname "host" and set the string representation of the
 * IP address into the buffer pointed by "ipbuf".
 *
 * If flags is set to ANET_IP_ONLY the function only resolves hostnames
 * that are actually already IPv4 or IPv6 addresses. This turns the function
 * into a validating / normalizing function. */
// 解释 host 的地址,并保存到 ipbuf 中
int anetResolve(char *err, char *host, char *ipbuf, size_t ipbuf_len, int flags) {
    struct addrinfo hints, *info;
    int rv;

    memset(&hints, 0, sizeof(hints));
    if (flags & ANET_IP_ONLY)
        hints.ai_flags = AI_NUMERICHOST;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM; /* specify socktype to avoid dups */

    if ((rv = getaddrinfo(host, NULL, &hints, &info)) != 0) {
        anetSetError(err, "%s", gai_strerror(rv));
        return ANET_ERR;
    }
    if (info->ai_family == AF_INET) {
        struct sockaddr_in *sa = (struct sockaddr_in *)info->ai_addr;
        inet_ntop(AF_INET, &(sa->sin_addr), ipbuf, ipbuf_len);
    }
    else {
        struct sockaddr_in6 *sa = (struct sockaddr_in6 *)info->ai_addr;
        inet_ntop(AF_INET6, &(sa->sin6_addr), ipbuf, ipbuf_len);
    }

    freeaddrinfo(info);
    return ANET_OK;
}

// 设置地址为可重用
static int anetSetReuseAddr(char *err, int fd) {
    int yes = 1;
    // 确保连接密集型的东西,如redis 性能测试将能够关闭/打开套接字无数次
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
        anetSetError(err, "setsockopt SO_REUSEADDR: %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

// 创建并返回 socket
static int anetCreateSocket(char *err, int domain) {
    int s;
    if ((s = socket(domain, SOCK_STREAM, 0)) == -1) {
        anetSetError(err, "creating socket: %s", strerror(errno));
        return ANET_ERR;
    }

    /* Make sure connection-intensive things like the redis benchmark
     * will be able to close/open sockets a zillion of times */
    if (anetSetReuseAddr(err, s) == ANET_ERR) {
        close(s);
        return ANET_ERR;
    }
    return s;
}

// 通用连接创建函数,被其他高层函数所调用
#define ANET_CONNECT_NONE 0
#define ANET_CONNECT_NONBLOCK 1
#define ANET_CONNECT_BE_BINDING 2 /* Best effort binding. */

// 创建阻塞 TCP 连接
static int anetTcpGenericConnect(char *err, const char *addr, int port, const char *source_addr, int flags) {
    int s = ANET_ERR, rv;
    char portstr[6]; /* strlen("65535") + 1; */
    struct addrinfo hints, *servinfo, *bservinfo, *p, *b;

    snprintf(portstr, sizeof(portstr), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if ((rv = getaddrinfo(addr, portstr, &hints, &servinfo)) != 0) {
        anetSetError(err, "%s", gai_strerror(rv));
        return ANET_ERR;
    }
    for (p = servinfo; p != NULL; p = p->ai_next) {
        /* Try to create the socket and to connect it.
         * If we fail in the socket() call, or on connect(), we retry with
         * the next entry in servinfo. */
        if ((s = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1)
            continue;
        if (anetSetReuseAddr(err, s) == ANET_ERR)
            goto error;
        if (flags & ANET_CONNECT_NONBLOCK && anetNonBlock(err, s) != ANET_OK)
            goto error;
        if (source_addr) {
            int bound = 0;
            /* Using getaddrinfo saves us from self-determining IPv4 vs IPv6 */
            if ((rv = getaddrinfo(source_addr, NULL, &hints, &bservinfo)) != 0) {
                anetSetError(err, "%s", gai_strerror(rv));
                goto error;
            }
            for (b = bservinfo; b != NULL; b = b->ai_next) {
                if (bind(s, b->ai_addr, b->ai_addrlen) != -1) {
                    bound = 1;
                    break;
                }
            }
            freeaddrinfo(bservinfo);
            if (!bound) {
                anetSetError(err, "bind: %s", strerror(errno));
                goto error;
            }
        }
        if (connect(s, p->ai_addr, p->ai_addrlen) == -1) {
            /* If the socket is non-blocking, it is ok for connect() to
             * return an EINPROGRESS error here. */
            if (errno == EINPROGRESS && flags & ANET_CONNECT_NONBLOCK)
                goto end;
            close(s);
            s = ANET_ERR;
            continue;
        }

        /* If we ended an iteration of the for loop without errors, we
         * have a connected socket. Let's return to the caller. */
        goto end;
    }
    if (p == NULL)
        anetSetError(err, "creating socket: %s", strerror(errno));

error:
    if (s != ANET_ERR) {
        close(s);
        s = ANET_ERR;
    }

end:
    freeaddrinfo(servinfo);

    /* Handle best effort binding: if a binding address was used, but it is
     * not possible to create a socket, try again without a binding address. */
    if (s == ANET_ERR && source_addr && (flags & ANET_CONNECT_BE_BINDING)) {
        return anetTcpGenericConnect(err, addr, port, NULL, flags);
    }
    else {
        return s;
    }
}

// 创建非阻塞 TCP 连接
int anetTcpNonBlockConnect(char *err, const char *addr, int port) {
    return anetTcpGenericConnect(err, addr, port, NULL, ANET_CONNECT_NONBLOCK);
}

int anetTcpNonBlockBestEffortBindConnect(char *err, const char *addr, int port, const char *source_addr) {
    return anetTcpGenericConnect(err, addr, port, source_addr, ANET_CONNECT_NONBLOCK | ANET_CONNECT_BE_BINDING);
}

// 创建阻塞、非阻塞  本地连接
int anetUnixGenericConnect(char *err, const char *path, int flags) {
    int s;
    struct sockaddr_un sa;

    if ((s = anetCreateSocket(err, AF_LOCAL)) == ANET_ERR)
        return ANET_ERR;

    sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    if (flags & ANET_CONNECT_NONBLOCK) {
        if (anetNonBlock(err, s) != ANET_OK) {
            close(s);
            return ANET_ERR;
        }
    }
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == -1) {
        if (errno == EINPROGRESS && flags & ANET_CONNECT_NONBLOCK)
            return s;

        anetSetError(err, "connect: %s", strerror(errno));
        close(s);
        return ANET_ERR;
    }
    return s;
}

// 绑定并创建监听套接字
static int anetListen(char *err, int s, struct sockaddr *sa, socklen_t len, int backlog) {
    if (bind(s, sa, len) == -1) {
        anetSetError(err, "bind: %s", strerror(errno));
        close(s);
        return ANET_ERR;
    }

    if (listen(s, backlog) == -1) {
        anetSetError(err, "listen: %s", strerror(errno));
        close(s);
        return ANET_ERR;
    }
    return ANET_OK;
}

static int anetV6Only(char *err, int s) {
    int yes = 1;
    if (setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes)) == -1) {
        anetSetError(err, "setsockopt: %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

// 创建TCP套接字
static int _anetTcpServer(char *err, int port, char *bindaddr, int af, int backlog) {
    int s = -1, rv;
    char _port[6]; /* strlen("65535") */
    struct addrinfo hints, *servinfo, *p;

    snprintf(_port, 6, "%d", port);
    // new一个结构体,c语言中,new一个对象比较麻烦,要先定义一个结构体类型的变量,如struct addrinfo hints,
    // ,然后调用memset来初始化内存,然后设置各个属性.总体来说,这里就是new了一个ipv4的地址
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = af;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; /* No effect if bindaddr != NULL */
    printf("_anetTcpServer--->  %s\n", bindaddr);
    if (bindaddr) {
        if (!strcmp("*", bindaddr)) { // strcmp相等返回0
            bindaddr = NULL;
        }
    }

    if (af == AF_INET6 && bindaddr && !strcmp("::*", bindaddr)) {
        bindaddr = NULL;
    }
    // 因为一般服务器都有多网卡,多个ip地址,还有环回网卡之类的,这里的getaddrinfo,是利用我们第一步的 hints,去帮助我们筛选出一个最终的网卡地址出来,然后赋值给 servinfo 变量.
    if ((rv = getaddrinfo(bindaddr, _port, &hints, &servinfo)) != 0) {
        anetSetError(err, "%s", gai_strerror(rv));
        return ANET_ERR;
    }
    for (p = servinfo; p != NULL; p = p->ai_next) {
        if ((s = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            continue;
        }

        if (af == AF_INET6 && anetV6Only(err, s) == ANET_ERR) {
            goto error;
        }
        if (anetSetReuseAddr(err, s) == ANET_ERR) { // 设置地址为可重用
            goto error;
        }
        if (anetListen(err, s, p->ai_addr, p->ai_addrlen, backlog) == ANET_ERR) {
            s = ANET_ERR;
        }
        goto end;
    }
    if (p == NULL) {
        anetSetError(err, "无法绑定 socket, 错误码: %d", errno);
        goto error;
    }

error:
    if (s != -1) {
        close(s);
    }
    s = ANET_ERR;
end:
    freeaddrinfo(servinfo);
    return s;
}

// 创建TCP链接
int anetTcpServer(char *err, int port, char *bindaddr, int backlog) {
    return _anetTcpServer(err, port, bindaddr, AF_INET, backlog);
}

// 创建TCPv6链接
int anetTcp6Server(char *err, int port, char *bindaddr, int backlog) {
    return _anetTcpServer(err, port, bindaddr, AF_INET6, backlog);
}

// 创建一个本地连接用的服务器监听套接字
int anetUnixServer(char *err, char *path, mode_t perm, int backlog) {
    int s;
    struct sockaddr_un sa;

    if (strlen(path) > sizeof(sa.sun_path) - 1) {
        anetSetError(err, "unix socket path too long (%zu), must be under %zu", strlen(path), sizeof(sa.sun_path));
        return ANET_ERR;
    }
    if ((s = anetCreateSocket(err, AF_LOCAL)) == ANET_ERR) {
        return ANET_ERR;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    if (anetListen(err, s, (struct sockaddr *)&sa, sizeof(sa), backlog) == ANET_ERR) {
        return ANET_ERR;
    }
    if (perm) {
        chmod(sa.sun_path, perm);
    }
    return s;
}

// 接受连接并确保套接字是非阻塞的,以及 CLOEXEC.返回新的套接字FD,错误时返回-1.
static int anetGenericAccept(char *err, int s, struct sockaddr *sa, socklen_t *len) {
    // 一般 s 都是6 ,从redis 6379端口接收链接
    int fd;
    do {
        /* 在linux上使用accept4()调用来同时接受并设置套接字为非阻塞的. */
#ifdef HAVE_ACCEPT4
        fd = accept4(s, sa, len, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
        fd = accept(s, sa, len);
#endif
    } while (fd == -1 && errno == EINTR);
    // fd 新创建的链接的 描述符
    if (fd == -1) {
        anetSetError(err, "accept: %s", strerror(errno)); // 没有获取到新的链接时,报错
        return ANET_ERR;
    }
#ifndef HAVE_ACCEPT4
    if (anetCloexec(fd) == -1) {
        anetSetError(err, "anetCloexec: %s", strerror(errno));
        close(fd);
        return ANET_ERR;
    }
    if (anetNonBlock(err, fd) != ANET_OK) {
        close(fd);
        return ANET_ERR;
    }
#endif
    return fd;
}

// TCP 连接 accept 函数
// 返回错误、客户端IP,本地使用的端口
int anetTcpAccept(char *err, int serversock, char *ip, size_t ip_len, int *port) {
    // ntohs  作用是将一个16位数由网络字节顺序转换为主机字节顺序.
    // inet_ntop 从数值格式(addrptr)转换到表达式(strptr)

    int fd;
    struct sockaddr_storage sa;
    socklen_t salen = sizeof(sa);
    // 从6379端口,接收新的链接
    if ((fd = anetGenericAccept(err, serversock, (struct sockaddr *)&sa, &salen)) == ANET_ERR) {
        return ANET_ERR;
    }

    if (sa.ss_family == AF_INET) { // ipv4
        struct sockaddr_in *s = (struct sockaddr_in *)&sa;
        if (ip) {
            // 将IPv4或IPv6 Internet网络地址转换为 Internet标准格式的字符串。
            inet_ntop(AF_INET, (void *)&(s->sin_addr), ip, ip_len);
        }
        if (port) {
            *port = ntohs(s->sin_port);
        }
    }
    else { // ipv6
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&sa;
        if (ip) {
            inet_ntop(AF_INET6, (void *)&(s->sin6_addr), ip, ip_len);
        }
        if (port) {
            *port = ntohs(s->sin6_port);
        }
    }
    return fd;
}

// 本地连接 accept 函数
int anetUnixAccept(char *err, int s) {
    int fd;
    struct sockaddr_un sa;
    socklen_t salen = sizeof(sa);
    if ((fd = anetGenericAccept(err, s, (struct sockaddr *)&sa, &salen)) == ANET_ERR)
        return ANET_ERR;

    return fd;
}

// 获取连接客户端的 IP 和端口号
int anetFdToString(int fd, char *ip, size_t ip_len, int *port, int fd_to_str_type) {
    struct sockaddr_storage sa;
    socklen_t salen = sizeof(sa);

    if (fd_to_str_type == FD_TO_PEER_NAME) {
        if (getpeername(fd, (struct sockaddr *)&sa, &salen) == -1)
            goto error;
    }
    else {
        if (getsockname(fd, (struct sockaddr *)&sa, &salen) == -1)
            goto error;
    }

    if (sa.ss_family == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)&sa;
        if (ip) {
            if (inet_ntop(AF_INET, (void *)&(s->sin_addr), ip, ip_len) == NULL)
                goto error;
        }
        if (port)
            *port = ntohs(s->sin_port);
    }
    else if (sa.ss_family == AF_INET6) {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&sa;
        if (ip) {
            if (inet_ntop(AF_INET6, (void *)&(s->sin6_addr), ip, ip_len) == NULL)
                goto error;
        }
        if (port)
            *port = ntohs(s->sin6_port);
    }
    else if (sa.ss_family == AF_UNIX) {
        if (ip) {
            int res = snprintf(ip, ip_len, "/unixsocket");
            if (res < 0 || (unsigned int)res >= ip_len)
                goto error;
        }
        if (port)
            *port = 0;
    }
    else {
        goto error;
    }
    return 0;

error:
    if (ip) {
        if (ip_len >= 2) {
            ip[0] = '?';
            ip[1] = '\0';
        }
        else if (ip_len == 1) {
            ip[0] = '\0';
        }
    }
    if (port)
        *port = 0;
    return -1;
}

/* Format an IP,port pair into something easy to parse. If IP is IPv6
 * (matches for ":"), the ip is surrounded by []. IP and port are just
 * separated by colons. This the standard to display addresses within Redis. */
int anetFormatAddr(char *buf, size_t buf_len, char *ip, int port) {
    return snprintf(buf, buf_len, strchr(ip, ':') ? "[%s]:%d" : "%s:%d", ip, port);
}

/* Like anetFormatAddr() but extract ip and port from the socket's peer/sockname. */
int anetFormatFdAddr(int fd, char *buf, size_t buf_len, int fd_to_str_type) {
    char ip[INET6_ADDRSTRLEN];
    int port;

    anetFdToString(fd, ip, sizeof(ip), &port, fd_to_str_type);
    return anetFormatAddr(buf, buf_len, ip, port);
}

/* Create a pipe buffer with given flags for read end and write end.
 * Note that it supports the file flags defined by pipe2() and fcntl(F_SETFL),
 * and one of the use cases is O_CLOEXEC|O_NONBLOCK. */
int anetPipe(int fds[2], int read_flags, int write_flags) {
    int pipe_flags = 0;
#if defined(__linux__) || defined(__FreeBSD__)
    /* When possible, try to leverage pipe2() to apply flags that are common to both ends.
     * There is no harm to set O_CLOEXEC to prevent fd leaks. */
    pipe_flags = O_CLOEXEC | (read_flags & write_flags);
    if (pipe2(fds, pipe_flags)) {
        /* Fail on real failures, and fallback to simple pipe if pipe2 is unsupported. */
        if (errno != ENOSYS && errno != EINVAL)
            return -1;
        pipe_flags = 0;
    }
    else {
        /* If the flags on both ends are identical, no need to do anything else. */
        if ((O_CLOEXEC | read_flags) == (O_CLOEXEC | write_flags))
            return 0;
        /* Clear the flags which have already been set using pipe2. */
        read_flags &= ~pipe_flags;
        write_flags &= ~pipe_flags;
    }
#endif

    /* When we reach here with pipe_flags of 0, it means pipe2 failed (or was not attempted),
     * so we try to use pipe. Otherwise, we skip and proceed to set specific flags below. */
    if (pipe_flags == 0 && pipe(fds))
        return -1;

    /* File descriptor flags.
     * Currently, only one such flag is defined: FD_CLOEXEC, the close-on-exec flag. */
    if (read_flags & O_CLOEXEC)
        if (fcntl(fds[0], F_SETFD, FD_CLOEXEC))
            goto error;
    if (write_flags & O_CLOEXEC)
        if (fcntl(fds[1], F_SETFD, FD_CLOEXEC))
            goto error;

    /* File status flags after clearing the file descriptor flag O_CLOEXEC. */
    read_flags &= ~O_CLOEXEC;
    if (read_flags)
        if (fcntl(fds[0], F_SETFL, read_flags))
            goto error;
    write_flags &= ~O_CLOEXEC;
    if (write_flags)
        if (fcntl(fds[1], F_SETFL, write_flags))
            goto error;

    return 0;

error:
    close(fds[0]);
    close(fds[1]);
    return -1;
}

int anetSetSockMarkId(char *err, int fd, uint32_t id) {
#ifdef HAVE_SOCKOPTMARKID
    if (setsockopt(fd, SOL_SOCKET, SOCKOPTMARKID, (void *)&id, sizeof(id)) == -1) {
        anetSetError(err, "setsockopt: %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
#else
    UNUSED(fd);
    UNUSED(id);
    anetSetError(err, "anetSetSockMarkid 不支持这个平台");
    return ANET_OK;
#endif
}
