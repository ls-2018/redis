#ifndef __REDIS_RIO_H
#define __REDIS_RIO_H

#include <stdio.h>
#include <stdint.h>
#include "over-sds.h"
#include "over-connection.h"

#define RIO_FLAG_READ_ERROR (1 << 0)
#define RIO_FLAG_WRITE_ERROR (1 << 1)

// RIO API 接口和状态
struct rio {
    size_t (*read)(struct rio *, void *buf, size_t len);             //
    size_t (*write)(struct rio *, const void *buf, size_t len);      //
    off_t (*tell)(struct rio *);                                     //
    int (*flush)(struct rio *);                                      //
    void (*update_cksum)(struct rio *, const void *buf, size_t len); // 校验和计算函数,每次有写入/读取新数据时都要计算一次
    uint64_t cksum, flags;                                           // 当前校验和
    size_t processed_bytes;                                          // 读或写的字节数
    size_t max_processing_chunk;                                     // 最大单个读或写块大小
    union
    {
        struct {
            sds ptr;   // 缓存指针
            off_t pos; // 偏移量
        } buffer;      // 内存缓冲区目标。
        struct {
            FILE *fp;       // 被打开文件的指针
            off_t buffered; // 最近一次 fsync() 以来,写入的字节量
            off_t autosync; // 写入多少字节之后,才会自动执行一次 fsync()
        } file;             // Stdio文件指针目标。
        struct {
            connection *conn;
            off_t pos;          // 返回的buf中的Pos
            sds buf;            // 缓冲数据
            size_t read_limit;  // 不允许缓冲/读取超过这个值
            size_t read_so_far;
        } conn;                 // 连接对象(用于从套接字读取)
        struct {
            int fd; /* File descriptor. */
            off_t pos;
            sds buf;
        } fd; // FD目标(用于写入管道)。
    } io;
};

typedef struct rio rio;

/* The following functions are our interface with the stream. They'll call the
 * actual implementation of read / write / tell, and will update the checksum
 * if needed. */
// 将 buf 中的 len 字节写入到 r 中.
// 写入成功返回实际写入的字节数,写入失败返回 -1 .
static inline size_t rioWrite(rio *r, const void *buf, size_t len) {
    if (r->flags & RIO_FLAG_WRITE_ERROR)
        return 0;
    while (len) {
        size_t bytes_to_write = (r->max_processing_chunk && r->max_processing_chunk < len) ? r->max_processing_chunk : len;
        if (r->update_cksum)
            r->update_cksum(r, buf, bytes_to_write);
        if (r->write(r, buf, bytes_to_write) == 0) {
            r->flags |= RIO_FLAG_WRITE_ERROR;
            return 0;
        }
        buf = (char *)buf + bytes_to_write;
        len -= bytes_to_write;
        r->processed_bytes += bytes_to_write;
    }
    return 1;
}

// 从 r 中读取 len 字节,并将内容保存到 buf 中.
// 读取成功返回 1 ,失败返回 0 .
static inline size_t rioRead(rio *r, void *buf, size_t len) {
    if (r->flags & RIO_FLAG_READ_ERROR)
        return 0;
    while (len) {
        size_t bytes_to_read = (r->max_processing_chunk && r->max_processing_chunk < len) ? r->max_processing_chunk : len;
        if (r->read(r, buf, bytes_to_read) == 0) {
            r->flags |= RIO_FLAG_READ_ERROR;
            return 0;
        }
        if (r->update_cksum)
            r->update_cksum(r, buf, bytes_to_read);
        buf = (char *)buf + bytes_to_read;
        len -= bytes_to_read;
        r->processed_bytes += bytes_to_read;
    }
    return 1;
}

// 返回 r 的当前偏移量.
static inline off_t rioTell(rio *r) {
    return r->tell(r);
}

static inline int rioFlush(rio *r) {
    return r->flush(r);
}

/* This function allows to know if there was a read error in any past
 * operation, since the rio stream was created or since the last call
 * to rioClearError(). */
static inline int rioGetReadError(rio *r) {
    return (r->flags & RIO_FLAG_READ_ERROR) != 0;
}

/* Like rioGetReadError() but for write errors. */
static inline int rioGetWriteError(rio *r) {
    return (r->flags & RIO_FLAG_WRITE_ERROR) != 0;
}

static inline void rioClearErrors(rio *r) {
    r->flags &= ~(RIO_FLAG_READ_ERROR | RIO_FLAG_WRITE_ERROR);
}

void rioInitWithFile(rio *r, FILE *fp);

void rioInitWithBuffer(rio *r, sds s);

void rioInitWithConn(rio *r, connection *conn, size_t read_limit);

void rioInitWithFd(rio *r, int fd);

void rioFreeFd(rio *r);

void rioFreeConn(rio *r, sds *out_remainingBufferedData);

size_t rioWriteBulkCount(rio *r, char prefix, long count);

size_t rioWriteBulkString(rio *r, const char *buf, size_t len);

size_t rioWriteBulkLongLong(rio *r, long long l);

size_t rioWriteBulkDouble(rio *r, double d);

struct redisObject;

int rioWriteBulkObject(rio *r, struct redisObject *obj);

void rioGenericUpdateChecksum(rio *r, const void *buf, size_t len);

void rioSetAutoSync(rio *r, off_t bytes);

#endif
