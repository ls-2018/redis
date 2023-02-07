/*
 * RIO 是一个可以面向流、可用于对多种不同的输入 （目前是文件和内存字节）进行编程的抽象.
 * 比如说,RIO 可以同时对内存或文件中的 RDB 格式进行读写.
 *
 *
 * 一个 RIO 对象提供以下方法：
 *        从流中读取
 *         写入到流中
 *        获取当前的偏移量
 *
 * 还可以通过设置 checksum 函数,计算写入或读取内容的校验和,
 * 或者为当前的校验和查询 rio 对象.
 */

#include "over-fmacros.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "rio.h"
#include "over-util.h"
#include "over-crc64.h"
#include "over-config.h"
#include "over-server.h"

/* ------------------------- Buffer I/O implementation ----------------------- */

/* Returns 1 or 0 for success/failure. */
// 将给定内容 buf 追加到缓存中,长度为 len .
// 成功返回 1 ,失败返回 0 .
static size_t rioBufferWrite(rio *r, const void *buf, size_t len) {
    // 追加
    r->io.buffer.ptr = sdscatlen(r->io.buffer.ptr, (char *)buf, len);
    // 更新偏移量
    r->io.buffer.pos += len;
    return 1;
}

/* Returns 1 or 0 for success/failure. */
// 从 r 中读取长度为 len 的内容到 buf 中.
// 读取成功返回 1 ,否则返回 0 .
static size_t rioBufferRead(rio *r, void *buf, size_t len) {
    // r 中的内容的长度不足 len
    if (sdslen(r->io.buffer.ptr) - r->io.buffer.pos < len)
        return 0; /* not enough buffer to return len bytes. */
    // 复制 r 中的内容到 buf
    memcpy(buf, r->io.buffer.ptr + r->io.buffer.pos, len);
    r->io.buffer.pos += len;
    return 1;
}

/* Returns read/write position in buffer. */
// 返回缓存的当前偏移量
static off_t rioBufferTell(rio *r) {
    return r->io.buffer.pos;
}

/* Flushes any buffer to target device if applicable. Returns 1 on success
 * and 0 on failures. */
static int rioBufferFlush(rio *r) {
    UNUSED(r);
    return 1; /* Nothing to do, our write just appends to the buffer. */
}

static const rio rioBufferIO = {
    rioBufferRead,
    rioBufferWrite,
    rioBufferTell,
    rioBufferFlush,
    NULL,       /* update_checksum */
    0,          /* current checksum */
    0,          /* flags */
    0,          /* bytes read or written */
    0,          /* read/write chunk size */
    {{NULL, 0}} /* union for io-specific vars */
};

// 初始化内存流
void rioInitWithBuffer(rio *r, sds s) {
    *r = rioBufferIO;
    r->io.buffer.ptr = s;
    r->io.buffer.pos = 0;
}

/* --------------------- Stdio file pointer implementation ------------------- */

// 将长度为 len 的内容 buf 写入到文件 r 中.
// 成功返回 1 ,失败返回 0 .
static size_t rioFileWrite(rio *r, const void *buf, size_t len) {
    if (!r->io.file.autosync)
        return fwrite(buf, len, 1, r->io.file.fp);

    size_t nwritten = 0;
    /* Incrementally write data to the file, avoid a single write larger than
     * the autosync threshold (so that the kernel's buffer cache never has too
     * many dirty pages at once). */
    while (len != nwritten) {
        serverAssert(r->io.file.autosync > r->io.file.buffered);
        size_t nalign = (size_t)(r->io.file.autosync - r->io.file.buffered);
        size_t towrite = nalign > len - nwritten ? len - nwritten : nalign;

        if (fwrite((char *)buf + nwritten, towrite, 1, r->io.file.fp) == 0)
            return 0;
        nwritten += towrite;
        r->io.file.buffered += towrite;
        // 检查写入的字节数,看是否需要执行自动 sync
        if (r->io.file.buffered >= r->io.file.autosync) {
            fflush(r->io.file.fp);

            size_t processed = r->processed_bytes + nwritten;
            serverAssert(processed % r->io.file.autosync == 0);
            serverAssert(r->io.file.buffered == r->io.file.autosync);

#if HAVE_SYNC_FILE_RANGE
            /* Start writeout asynchronously. */
            if (sync_file_range(fileno(r->io.file.fp), processed - r->io.file.autosync, r->io.file.autosync, SYNC_FILE_RANGE_WRITE) == -1)
                return 0;

            if (processed >= (size_t)r->io.file.autosync * 2) {
                /* To keep the promise to 'autosync', we should make sure last
                 * asynchronous writeout persists into disk. This call may block
                 * if last writeout is not finished since disk is slow. */
                if (sync_file_range(fileno(r->io.file.fp), processed - r->io.file.autosync * 2, r->io.file.autosync, SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER) == -1)
                    return 0;
            }
#else
            if (redis_fsync(fileno(r->io.file.fp)) == -1)
                return 0;
#endif
            r->io.file.buffered = 0;
        }
    }
    return 1;
}

// 从文件 r 中读取 len 字节到 buf 中.
// 返回值为读取的字节数.
static size_t rioFileRead(rio *r, void *buf, size_t len) {
    return fread(buf, len, 1, r->io.file.fp);
}

// 返回文件当前的偏移量
static off_t rioFileTell(rio *r) {
    return ftello(r->io.file.fp);
}

/* Flushes any buffer to target device if applicable. Returns 1 on success
 * and 0 on failures. */
static int rioFileFlush(rio *r) {
    return (fflush(r->io.file.fp) == 0) ? 1 : 0;
}
// 流为内存时所使用的结构
static const rio rioFileIO = {
    rioFileRead,  // 读函数
    rioFileWrite, // 写函数
    rioFileTell,  // 偏移量函数
    rioFileFlush, //
    NULL,         // /* update_checksum */
    0,            //    /* current checksum */
    0,            // /* flags */
    0,            //        /* bytes read or written */
    0,            //        /* read/write chunk size */
    {{NULL, 0}}   /* union for io-specific vars */
};
// 初始化文件流
void rioInitWithFile(rio *r, FILE *fp) {
    *r = rioFileIO;
    r->io.file.fp = fp;
    r->io.file.buffered = 0;
    r->io.file.autosync = 0;
}

/* ------------------- Connection implementation -------------------
 * We use this RIO implementation when reading an RDB file directly from
 * the connection to the memory via rdbLoadRio(), thus this implementation
 * only implements reading from a connection that is, normally,
 * just a socket. */

static size_t rioConnWrite(rio *r, const void *buf, size_t len) {
    UNUSED(r);
    UNUSED(buf);
    UNUSED(len);
    return 0; /* Error, this target does not yet support writing. */
}

/* Returns 1 or 0 for success/failure. */
static size_t rioConnRead(rio *r, void *buf, size_t len) {
    size_t avail = sdslen(r->io.conn.buf) - r->io.conn.pos;

    /* If the buffer is too small for the entire request: realloc. */
    if (sdslen(r->io.conn.buf) + sdsavail(r->io.conn.buf) < len)
        r->io.conn.buf = sdsMakeRoomFor(r->io.conn.buf, len - sdslen(r->io.conn.buf));

    /* If the remaining unused buffer is not large enough: memmove so that we
     * can read the rest. */
    if (len > avail && sdsavail(r->io.conn.buf) < len - avail) {
        sdsrange(r->io.conn.buf, r->io.conn.pos, -1);
        r->io.conn.pos = 0;
    }

    /* Make sure the caller didn't request to read past the limit.
     * If they didn't we'll buffer till the limit, if they did, we'll
     * return an error. */
    if (r->io.conn.read_limit != 0 && r->io.conn.read_limit < r->io.conn.read_so_far + len) {
        errno = EOVERFLOW;
        return 0;
    }

    /* If we don't already have all the data in the sds, read more */
    while (len > sdslen(r->io.conn.buf) - r->io.conn.pos) {
        size_t buffered = sdslen(r->io.conn.buf) - r->io.conn.pos;
        size_t needs = len - buffered;
        /* Read either what's missing, or PROTO_IOBUF_LEN, the bigger of
         * the two. */
        size_t toread = needs < PROTO_IOBUF_LEN ? PROTO_IOBUF_LEN : needs;
        if (toread > sdsavail(r->io.conn.buf))
            toread = sdsavail(r->io.conn.buf);
        if (r->io.conn.read_limit != 0 && r->io.conn.read_so_far + buffered + toread > r->io.conn.read_limit) {
            toread = r->io.conn.read_limit - r->io.conn.read_so_far - buffered;
        }
        int retval = connRead(r->io.conn.conn, (char *)r->io.conn.buf + sdslen(r->io.conn.buf), toread);
        if (retval == 0) {
            return 0;
        }
        else if (retval < 0) {
            if (connLastErrorRetryable(r->io.conn.conn))
                continue;
            if (errno == EWOULDBLOCK)
                errno = ETIMEDOUT;
            return 0;
        }
        sdsIncrLen(r->io.conn.buf, retval);
    }

    memcpy(buf, (char *)r->io.conn.buf + r->io.conn.pos, len);
    r->io.conn.read_so_far += len;
    r->io.conn.pos += len;
    return len;
}

/* Returns read/write position in file. */
static off_t rioConnTell(rio *r) {
    return r->io.conn.read_so_far;
}

/* Flushes any buffer to target device if applicable. Returns 1 on success
 * and 0 on failures. */
static int rioConnFlush(rio *r) {
    /* Our flush is implemented by the write method, that recognizes a
     * buffer set to NULL with a count of zero as a flush request. */
    return rioConnWrite(r, NULL, 0);
}

static const rio rioConnIO = {
    rioConnRead, rioConnWrite, rioConnTell, rioConnFlush, NULL, /* update_checksum */
    0,                                                          /* current checksum */
    0,                                                          /* flags */
    0,                                                          /* bytes read or written */
    0,                                                          /* read/write chunk size */
    {{NULL, 0}}                                                 /* union for io-specific vars */
};

/* Create an RIO that implements a buffered read from an fd
 * read_limit argument stops buffering when the reaching the limit. */
void rioInitWithConn(rio *r, connection *conn, size_t read_limit) {
    *r = rioConnIO;
    r->io.conn.conn = conn;
    r->io.conn.pos = 0;
    r->io.conn.read_limit = read_limit;
    r->io.conn.read_so_far = 0;
    r->io.conn.buf = sdsnewlen(NULL, PROTO_IOBUF_LEN);
    sdsclear(r->io.conn.buf);
}

/* Release the RIO stream. Optionally returns the unread buffered data
 * when the SDS pointer 'remaining' is passed. */
void rioFreeConn(rio *r, sds *remaining) {
    if (remaining && (size_t)r->io.conn.pos < sdslen(r->io.conn.buf)) {
        if (r->io.conn.pos > 0)
            sdsrange(r->io.conn.buf, r->io.conn.pos, -1);
        *remaining = r->io.conn.buf;
    }
    else {
        sdsfree(r->io.conn.buf);
        if (remaining)
            *remaining = NULL;
    }
    r->io.conn.buf = NULL;
}

/* ------------------- File descriptor implementation ------------------
 * This target is used to write the RDB file to pipe, when the master just
 * streams the data to the replicas without creating an RDB on-disk image
 * (diskless replication option).
 * It only implements writes. */

/* Returns 1 or 0 for success/failure.
 *
 * When buf is NULL and len is 0, the function performs a flush operation
 * if there is some pending buffer, so this function is also used in order
 * to implement rioFdFlush(). */
static size_t rioFdWrite(rio *r, const void *buf, size_t len) {
    ssize_t retval;
    unsigned char *p = (unsigned char *)buf;
    int doflush = (buf == NULL && len == 0);

    /* For small writes, we rather keep the data in user-space buffer, and flush
     * it only when it grows. however for larger writes, we prefer to flush
     * any pre-existing buffer, and write the new one directly without reallocs
     * and memory copying. */
    if (len > PROTO_IOBUF_LEN) {
        /* First, flush any pre-existing buffered data. */
        if (sdslen(r->io.fd.buf)) {
            if (rioFdWrite(r, NULL, 0) == 0)
                return 0;
        }
        /* Write the new data, keeping 'p' and 'len' from the input. */
    }
    else {
        if (len) {
            r->io.fd.buf = sdscatlen(r->io.fd.buf, buf, len);
            if (sdslen(r->io.fd.buf) > PROTO_IOBUF_LEN)
                doflush = 1;
            if (!doflush)
                return 1;
        }
        /* Flushing the buffered data. set 'p' and 'len' accordingly. */
        p = (unsigned char *)r->io.fd.buf;
        len = sdslen(r->io.fd.buf);
    }

    size_t nwritten = 0;
    while (nwritten != len) {
        retval = write(r->io.fd.fd, p + nwritten, len - nwritten);
        if (retval <= 0) {
            if (retval == -1 && errno == EINTR)
                continue;
            /* With blocking io, which is the sole user of this
             * rio target, EWOULDBLOCK is returned only because of
             * the SO_SNDTIMEO socket option, so we translate the error
             * into one more recognizable by the user. */
            if (retval == -1 && errno == EWOULDBLOCK)
                errno = ETIMEDOUT;
            return 0; /* error. */
        }
        nwritten += retval;
    }

    r->io.fd.pos += len;
    sdsclear(r->io.fd.buf);
    return 1;
}

/* Returns 1 or 0 for success/failure. */
static size_t rioFdRead(rio *r, void *buf, size_t len) {
    UNUSED(r);
    UNUSED(buf);
    UNUSED(len);
    return 0; /* Error, this target does not support reading. */
}

/* Returns read/write position in file. */
static off_t rioFdTell(rio *r) {
    return r->io.fd.pos;
}

/* Flushes any buffer to target device if applicable. Returns 1 on success
 * and 0 on failures. */
static int rioFdFlush(rio *r) {
    /* Our flush is implemented by the write method, that recognizes a
     * buffer set to NULL with a count of zero as a flush request. */
    return rioFdWrite(r, NULL, 0);
}

// 流为文件时所使用的结构
static const rio rioFdIO = {
    rioFdRead,  rioFdWrite, rioFdTell, rioFdFlush, NULL, /* update_checksum */
    0,                                                   /* current checksum */
    0,                                                   /* flags */
    0,                                                   /* bytes read or written */
    0,                                                   /* read/write chunk size */
    {{NULL, 0}}                                          /* union for io-specific vars */
};

void rioInitWithFd(rio *r, int fd) {
    *r = rioFdIO;
    r->io.fd.fd = fd;
    r->io.fd.pos = 0;
    r->io.fd.buf = sdsempty();
}

/* release the rio stream. */
void rioFreeFd(rio *r) {
    sdsfree(r->io.fd.buf);
}

/* ---------------------------- Generic functions ---------------------------- */

// 通用校验和计算函数
void rioGenericUpdateChecksum(rio *r, const void *buf, size_t len) {
    r->cksum = crc64(r->cksum, buf, len);
}

// 每次通过 rio 写入 bytes 指定的字节数量时,执行一次自动的 fsync .
// 默认情况下, bytes 被设为 0 ,表示不执行自动 fsync .
// 这个函数是为了防止一次写入过多内容而设置的.
// 通过显示地、间隔性地调用 fsync ,
// 可以将写入的 I/O 压力分担到多次 fsync 调用中.
void rioSetAutoSync(rio *r, off_t bytes) {
    if (r->write != rioFileIO.write)
        return;
    r->io.file.autosync = bytes;
}

/// --------------------------- Higher level interface --------------------------

// 以下高层函数通过调用前面的底层函数来生成 AOF 文件所需的协议

// 以带 '\r\n' 后缀的形式写入字符串表示的 count 到 RIO
// 成功返回写入的数量,失败返回 0 .
size_t rioWriteBulkCount(rio *r, char prefix, long count) {
    char cbuf[128];
    int clen;

    // cbuf = prefix ++ count ++ '\r\n'
    // 例如： *123\r\n
    cbuf[0] = prefix;
    clen = 1 + ll2string(cbuf + 1, sizeof(cbuf) - 1, count);
    cbuf[clen++] = '\r';
    cbuf[clen++] = '\n';
    if (rioWrite(r, cbuf, clen) == 0) // 写入

        return 0;
    return clen; // 返回写入字节数
}

/* Write binary-safe string in the format: "$<count>\r\n<payload>\r\n". */
// 以 "$<count>\r\n<payload>\r\n" 的形式写入二进制安全字符
// 例如 $3\r\nSET\r\n
size_t rioWriteBulkString(rio *r, const char *buf, size_t len) {
    size_t nwritten;
    // 写入 $<count>\r\n

    if ((nwritten = rioWriteBulkCount(r, '$', len)) == 0)
        return 0;
    // 写入 <payload>

    if (len > 0 && rioWrite(r, buf, len) == 0)
        return 0;
    // 写入 \r\n

    if (rioWrite(r, "\r\n", 2) == 0)
        return 0;
    return nwritten + len + 2; // 返回写入总量
}

// 以 "$<count>\r\n<payload>\r\n" 的格式写入 long long 值
size_t rioWriteBulkLongLong(rio *r, long long l) {
    char lbuf[32];
    unsigned int llen;
    // 取出 long long 值的字符串形式
    // 并计算该字符串的长度
    llen = ll2string(lbuf, sizeof(lbuf), l);
    // 写入 $llen\r\nlbuf\r\n
    return rioWriteBulkString(r, lbuf, llen);
}

// 以 "$<count>\r\n<payload>\r\n" 的格式写入 double 值
size_t rioWriteBulkDouble(rio *r, double d) {
    char dbuf[128];
    unsigned int dlen;
    // 取出 double 值的字符串表示（小数点后只保留 17 位）
    // 并计算字符串的长度
    dlen = snprintf(dbuf, sizeof(dbuf), "%.17g", d);
    // 写入 $dlen\r\ndbuf\r\n
    return rioWriteBulkString(r, dbuf, dlen);
}
