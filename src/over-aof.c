#include "over-server.h"
#include "over-bio.h"
#include "rio.h"
#include "over-functions.h"

#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/param.h>

// 释放客户端参数
void freeClientArgv(client *c);

// 获取aof文件大小
off_t getAppendOnlyFileSize(sds filename, int *status);

off_t getBaseAndIncrAppendOnlyFilesSize(aofManifest *am, int *status);

int getBaseAndIncrAppendOnlyFilesNum(aofManifest *am);

int aofFileExist(char *filename);

int aofFileExist(char *filename) {
    sds file_path = makePath(server.aof_dirname, filename);
    int ret = fileExist(file_path);
    sdsfree(file_path);
    return ret;
}
int rewriteAppendOnlyFile(char *filename);

aofManifest *aofLoadManifestFromFile(sds am_filepath);

void aofManifestFreeAndUpdate(aofManifest *am);

/* Naming rules. */
#define BASE_FILE_SUFFIX ".base"
#define INCR_FILE_SUFFIX ".incr"
#define RDB_FORMAT_SUFFIX ".rdb"
#define AOF_FORMAT_SUFFIX ".aof"
#define MANIFEST_NAME_SUFFIX ".manifest"
#define TEMP_FILE_NAME_PREFIX "temp-"

/* AOF manifest key. */
#define AOF_MANIFEST_KEY_FILE_NAME "file"
#define AOF_MANIFEST_KEY_FILE_SEQ "seq"
#define AOF_MANIFEST_KEY_FILE_TYPE "type"

aofInfo *aofInfoCreate(void) {
    return zcalloc(sizeof(aofInfo));
}

// 释放aoinfo结构(由ai指向)及其嵌入的file_name.
void aofInfoFree(aofInfo *ai) {
    serverAssert(ai != NULL);
    if (ai->file_name)
        sdsfree(ai->file_name);
    zfree(ai);
}

// 深度复制一个aofInfo.
aofInfo *aofInfoDup(aofInfo *orig) {
    serverAssert(orig != NULL);
    aofInfo *ai = aofInfoCreate();
    ai->file_name = sdsdup(orig->file_name);
    ai->file_seq = orig->file_seq;
    ai->file_type = orig->file_type;
    return ai;
}

// 将aofInfo格式化为字符串,它将是清单中的一行.
sds aofInfoFormat(sds buf, aofInfo *ai) {
    sds filename_repr = NULL;

    if (sdsneedsrepr(ai->file_name))
        filename_repr = sdscatrepr(sdsempty(), ai->file_name, sdslen(ai->file_name));

    sds ret = sdscatprintf(
        buf, "%s %s %s %lld %s %c\n",                  //
        AOF_MANIFEST_KEY_FILE_NAME,                    //
        filename_repr ? filename_repr : ai->file_name, //
        AOF_MANIFEST_KEY_FILE_SEQ,                     //
        ai->file_seq,                                  //
        AOF_MANIFEST_KEY_FILE_TYPE,                    //
        ai->file_type                                  //
    );
    sdsfree(filename_repr);

    return ret;
}

void aofListFree(void *item) {
    aofInfo *ai = (aofInfo *)item;
    aofInfoFree(ai);
}

void *aofListDup(void *item) {
    return aofInfoDup(item);
}

aofManifest *aofManifestCreate(void) {
    aofManifest *am = zcalloc(sizeof(aofManifest));
    am->incr_aof_list = listCreate();
    am->history_aof_list = listCreate();
    listSetFreeMethod(am->incr_aof_list, aofListFree);
    listSetDupMethod(am->incr_aof_list, aofListDup);
    listSetFreeMethod(am->history_aof_list, aofListFree);
    listSetDupMethod(am->history_aof_list, aofListDup);
    return am;
}

void aofManifestFree(aofManifest *am) {
    if (am->base_aof_info)
        aofInfoFree(am->base_aof_info);
    if (am->incr_aof_list)
        listRelease(am->incr_aof_list);
    if (am->history_aof_list)
        listRelease(am->history_aof_list);
    zfree(am);
}

sds getAofManifestFileName() {
    return sdscatprintf(sdsempty(), "%s%s", server.aof_filename, MANIFEST_NAME_SUFFIX);
}

sds getTempAofManifestFileName() {
    return sdscatprintf(sdsempty(), "%s%s%s", TEMP_FILE_NAME_PREFIX, server.aof_filename, MANIFEST_NAME_SUFFIX);
}

// 获取清单文件信息
// 'b' (base), 'h' (history) or 'i' (incr)
sds getAofManifestAsString(aofManifest *am) {
    serverAssert(am != NULL);
    sds buf = sdsempty();
    listNode *ln;
    listIter li;

    // 1、添加BASE文件信息,它总是在清单文件的开头.
    if (am->base_aof_info) {
        buf = aofInfoFormat(buf, am->base_aof_info);
    }

    // 2. 添加HISTORY类型AOF信息.
    listRewind(am->history_aof_list, &li);
    while ((ln = listNext(&li)) != NULL) {
        aofInfo *ai = (aofInfo *)ln->value;
        buf = aofInfoFormat(buf, ai);
    }

    // 3.添加INCR类型AOF信息.
    listRewind(am->incr_aof_list, &li);
    while ((ln = listNext(&li)) != NULL) {
        aofInfo *ai = (aofInfo *)ln->value;
        buf = aofInfoFormat(buf, ai);
    }

    return buf;
}

// 将清单信息从磁盘加载到“服务器”.当Redis服务器启动时,aof_manifest '.
// 在加载过程中,这个函数执行严格的错误检查,并将在错误(I/O错误,无效格式等)时中止整个Redis服务器进程.
// 如果AOF目录或清单文件不存在,这将被忽略,以支持从以前没有使用它们的版本进行无缝升级.
void aofLoadManifestFromDisk(void) {
    server.aof_manifest = aofManifestCreate(); // aofLoadManifestFromDisk
    if (!dirExists(server.aof_dirname)) {
        serverLog(LL_NOTICE, "AOF目录不存在: %s", server.aof_dirname);
        return;
    }

    sds am_name = getAofManifestFileName();
    sds am_filepath = makePath(server.aof_dirname, am_name);
    if (!fileExist(am_filepath)) {
        serverLog(LL_NOTICE, "AOF清单文件不存在: %s", am_name);
        sdsfree(am_name);
        sdsfree(am_filepath);
        return;
    }

    aofManifest *am = aofLoadManifestFromFile(am_filepath);
    if (am)
        aofManifestFreeAndUpdate(am);
    sdsfree(am_name);
    sdsfree(am_filepath);
}

// 文件清单
#define MANIFEST_MAX_LINE 1024

aofManifest *aofLoadManifestFromFile(sds am_filepath) {
    const char *err = NULL;
    long long maxseq = 0;

    aofManifest *am = aofManifestCreate(); // aofLoadManifestFromFile
    FILE *fp = fopen(am_filepath, "r");
    if (fp == NULL) {
        serverLog(LL_WARNING, "Fatal error: 不能打开aof日志文件%s: %s", am_filepath, strerror(errno));
        exit(1);
    }

    char buf[MANIFEST_MAX_LINE + 1];
    sds *argv = NULL;
    int argc;
    aofInfo *ai = NULL;

    sds line = NULL;
    int linenum = 0;

    while (1) {
        // 读入文件内容到缓存
        if (fgets(buf, MANIFEST_MAX_LINE + 1, fp) == NULL) {
            if (feof(fp)) {
                if (linenum == 0) {
                    err = "发现空的AOF清单文件";
                    goto loaderr;
                }
                else {
                    break;
                }
            }
            else {
                err = "读取AOF清单文件失败";
                goto loaderr;
            }
        }

        linenum++;

        // 跳过注释行
        if (buf[0] == '#')
            continue;

        if (strchr(buf, '\n') == NULL) {
            err = "AOF清单文件行太长";
            goto loaderr;
        }

        line = sdstrim(sdsnew(buf), " \t\r\n");
        if (!sdslen(line)) {
            err = "无效的AOF清单文件格式";
            goto loaderr;
        }

        argv = sdssplitargs(line, &argc);
        /* 'argc < 6' was done for forward compatibility. */
        if (argv == NULL || argc < 6 || (argc % 2)) {
            err = "无效的AOF清单文件格式";
            goto loaderr;
        }

        ai = aofInfoCreate();
        for (int i = 0; i < argc; i += 2) {
            if (!strcasecmp(argv[i], AOF_MANIFEST_KEY_FILE_NAME)) {
                ai->file_name = sdsnew(argv[i + 1]);
                if (!pathIsBaseName(ai->file_name)) {
                    err = "File不能是路径,只能是文件名";
                    goto loaderr;
                }
            }
            else if (!strcasecmp(argv[i], AOF_MANIFEST_KEY_FILE_SEQ)) {
                ai->file_seq = atoll(argv[i + 1]);
            }
            else if (!strcasecmp(argv[i], AOF_MANIFEST_KEY_FILE_TYPE)) {
                ai->file_type = (argv[i + 1])[0];
            }
            /* else if (!strcasecmp(argv[i], AOF_MANIFEST_KEY_OTHER)) {} */
        }

        /* We have to make sure we load all the information. */
        if (!ai->file_name || !ai->file_seq || !ai->file_type) {
            err = "Invalid AOF manifest file format";
            goto loaderr;
        }

        sdsfreesplitres(argv, argc);
        argv = NULL;

        if (ai->file_type == AOF_FILE_TYPE_BASE) {
            if (am->base_aof_info) {
                err = "Found duplicate base file information";
                goto loaderr;
            }
            am->base_aof_info = ai;
            am->curr_base_file_seq = ai->file_seq;
        }
        else if (ai->file_type == AOF_FILE_TYPE_HIST) {
            listAddNodeTail(am->history_aof_list, ai);
        }
        else if (ai->file_type == AOF_FILE_TYPE_INCR) {
            if (ai->file_seq <= maxseq) {
                err = "Found a non-monotonic sequence number";
                goto loaderr;
            }
            listAddNodeTail(am->incr_aof_list, ai);
            am->curr_incr_file_seq = ai->file_seq;
            maxseq = ai->file_seq;
        }
        else {
            err = "Unknown AOF file type";
            goto loaderr;
        }

        sdsfree(line);
        line = NULL;
        ai = NULL;
    }

    fclose(fp);
    return am;

loaderr:
    if (argv)
        sdsfreesplitres(argv, argc);
    if (ai)
        aofInfoFree(ai);

    serverLog(LL_WARNING, "\n*** 致命的aof manifest文件错误 ***\n");
    if (line) {
        serverLog(LL_WARNING, "读取清单文件,行数 %d\n", linenum);
        serverLog(LL_WARNING, ">>> '%s'\n", line);
    }
    serverLog(LL_WARNING, "%s\n", err);
    exit(1);
}

aofManifest *aofManifestDup(aofManifest *orig) {
    serverAssert(orig != NULL);
    aofManifest *am = zcalloc(sizeof(aofManifest));

    am->curr_base_file_seq = orig->curr_base_file_seq;
    am->curr_incr_file_seq = orig->curr_incr_file_seq;
    am->dirty = orig->dirty;

    if (orig->base_aof_info) {
        am->base_aof_info = aofInfoDup(orig->base_aof_info);
    }

    am->incr_aof_list = listDup(orig->incr_aof_list);
    am->history_aof_list = listDup(orig->history_aof_list);
    serverAssert(am->incr_aof_list != NULL);
    serverAssert(am->history_aof_list != NULL);
    return am;
}

void aofManifestFreeAndUpdate(aofManifest *am) {
    serverAssert(am != NULL);
    if (server.aof_manifest)
        aofManifestFree(server.aof_manifest);
    server.aof_manifest = am;
}

/*
 * 获得一个新的BASE文件名,并将之前的BASE文件(如果我们有)标记为HISTORY类型.
 * BASE file naming rules: `server.aof_filename`.seq.base.format
 * for example:
 *  appendonly.aof.1.base.aof  (server.aof_use_rdb_preamble is no)
 *  appendonly.aof.1.base.rdb  (server.aof_use_rdb_preamble is yes)
 */
sds getNewBaseFileNameAndMarkPreAsHistory(aofManifest *am) {
    serverAssert(am != NULL);
    if (am->base_aof_info) {
        serverAssert(am->base_aof_info->file_type == AOF_FILE_TYPE_BASE);
        am->base_aof_info->file_type = AOF_FILE_TYPE_HIST;
        listAddNodeHead(am->history_aof_list, am->base_aof_info);
    }

    char *format_suffix = server.aof_use_rdb_preamble ? RDB_FORMAT_SUFFIX : AOF_FORMAT_SUFFIX;

    aofInfo *ai = aofInfoCreate();
    ai->file_name = sdscatprintf(sdsempty(), "%s.%lld%s%s", server.aof_filename, ++am->curr_base_file_seq, BASE_FILE_SUFFIX, format_suffix);
    ai->file_seq = am->curr_base_file_seq;
    ai->file_type = AOF_FILE_TYPE_BASE;
    am->base_aof_info = ai;
    am->dirty = 1;
    return am->base_aof_info->file_name;
}

/* 获取一个新的增量类型的AOF文件名
 *  appendonly.aof.1.incr.aof
 */
sds getNewIncrAofName(aofManifest *am) {
    aofInfo *ai = aofInfoCreate();
    ai->file_type = AOF_FILE_TYPE_INCR;
    ai->file_name = sdscatprintf(sdsempty(), "%s.%lld%s%s", server.aof_filename, ++am->curr_incr_file_seq, INCR_FILE_SUFFIX, AOF_FORMAT_SUFFIX);
    ai->file_seq = am->curr_incr_file_seq;
    listAddNodeTail(am->incr_aof_list, ai);
    am->dirty = 1;
    return ai->file_name;
}

sds getTempIncrAofName() {
    return sdscatprintf(sdsempty(), "%s%s%s", TEMP_FILE_NAME_PREFIX, server.aof_filename, INCR_FILE_SUFFIX);
}

sds getLastIncrAofName(aofManifest *am) {
    serverAssert(am != NULL);

    if (!listLength(am->incr_aof_list)) {
        return getNewIncrAofName(am);
    }

    listNode *lastnode = listIndex(am->incr_aof_list, -1);
    aofInfo *ai = listNodeValue(lastnode);
    return ai->file_name;
}

void markRewrittenIncrAofAsHistory(aofManifest *am) {
    serverAssert(am != NULL);
    if (!listLength(am->incr_aof_list)) {
        return;
    }
    listNode *ln;
    listIter li;
    listRewindTail(am->incr_aof_list, &li);
    if (server.aof_fd != -1) {
        ln = listNext(&li);
        serverAssert(ln != NULL);
    }

    while ((ln = listNext(&li)) != NULL) {
        aofInfo *ai = (aofInfo *)ln->value;
        serverAssert(ai->file_type == AOF_FILE_TYPE_INCR);

        aofInfo *hai = aofInfoDup(ai);
        hai->file_type = AOF_FILE_TYPE_HIST;
        listAddNodeHead(am->history_aof_list, hai);
        listDelNode(am->incr_aof_list, ln);
    }

    am->dirty = 1;
}

int writeAofManifestFile(sds buf) {
    int ret = C_OK;
    ssize_t nwritten;
    int len;

    sds am_name = getAofManifestFileName();
    sds am_filepath = makePath(server.aof_dirname, am_name);
    sds tmp_am_name = getTempAofManifestFileName();
    sds tmp_am_filepath = makePath(server.aof_dirname, tmp_am_name);

    int fd = open(tmp_am_filepath, O_WRONLY | O_TRUNC | O_CREAT, 0644);
    if (fd == -1) {
        serverLog(LL_WARNING, "打开AOF清单文件失败 %s: %s", tmp_am_name, strerror(errno));

        ret = C_ERR;
        goto cleanup;
    }

    len = sdslen(buf);
    while (len) {
        nwritten = write(fd, buf, len);

        if (nwritten < 0) {
            if (errno == EINTR)
                continue;

            serverLog(LL_WARNING, "试图写入临时AOF清单文件时出错 %s: %s", tmp_am_name, strerror(errno));

            ret = C_ERR;
            goto cleanup;
        }

        len -= nwritten;
        buf += nwritten;
    }

    if (redis_fsync(fd) == -1) {
        serverLog(LL_WARNING, "同步AOF文件数据失败 %s: %s.", tmp_am_name, strerror(errno));

        ret = C_ERR;
        goto cleanup;
    }

    if (rename(tmp_am_filepath, am_filepath) != 0) {
        serverLog(LL_WARNING, "尝试重命名临时AOF清单文件时出错 %s into %s: %s", tmp_am_name, am_name, strerror(errno));

        ret = C_ERR;
    }

cleanup:
    if (fd != -1)
        close(fd);
    sdsfree(am_name);
    sdsfree(am_filepath);
    sdsfree(tmp_am_name);
    sdsfree(tmp_am_filepath);
    return ret;
}

int persistAofManifest(aofManifest *am) {
    if (am->dirty == 0) {
        return C_OK;
    }

    sds amstr = getAofManifestAsString(am);
    int ret = writeAofManifestFile(amstr);
    sdsfree(amstr);
    if (ret == C_OK)
        am->dirty = 0;
    return ret;
}

void aofUpgradePrepare(aofManifest *am) {
    serverAssert(!aofFileExist(server.aof_filename));

    if (dirCreateIfMissing(server.aof_dirname) == -1) {
        serverLog(LL_WARNING, "不能打开或创建 append-only 目录 %s: %s", server.aof_dirname, strerror(errno));
        exit(1);
    }

    if (am->base_aof_info)
        aofInfoFree(am->base_aof_info);
    aofInfo *ai = aofInfoCreate();
    ai->file_name = sdsnew(server.aof_filename);
    ai->file_seq = 1;
    ai->file_type = AOF_FILE_TYPE_BASE;
    am->base_aof_info = ai;
    am->curr_base_file_seq = 1;
    am->dirty = 1;

    if (persistAofManifest(am) != C_OK) {
        exit(1);
    }

    sds aof_filepath = makePath(server.aof_dirname, server.aof_filename);
    if (rename(server.aof_filename, aof_filepath) == -1) {
        serverLog(LL_WARNING, "试图移动旧的AOF文件 %s 到目录 %s:出错 %s", server.aof_filename, server.aof_dirname, strerror(errno));
        sdsfree(aof_filepath);
        exit(1);
    }
    sdsfree(aof_filepath);

    serverLog(LL_NOTICE, "已成功将旧式AOF文件(%s)迁移到AOF目录(%s).", server.aof_filename, server.aof_dirname);
}

int aofDelHistoryFiles(void) {
    if (server.aof_manifest == NULL || server.aof_disable_auto_gc == 1 || !listLength(server.aof_manifest->history_aof_list)) {
        return C_OK;
    }

    listNode *ln;
    listIter li;

    listRewind(server.aof_manifest->history_aof_list, &li);
    while ((ln = listNext(&li)) != NULL) {
        aofInfo *ai = (aofInfo *)ln->value;
        serverAssert(ai->file_type == AOF_FILE_TYPE_HIST);
        serverLog(LL_NOTICE, "后台移除历史文件 %s ", ai->file_name);
        sds aof_filepath = makePath(server.aof_dirname, ai->file_name);
        bg_unlink(aof_filepath);
        sdsfree(aof_filepath);
        listDelNode(server.aof_manifest->history_aof_list, ln);
    }

    server.aof_manifest->dirty = 1;
    return persistAofManifest(server.aof_manifest);
}

// 用于在AOF RW失败时清理临时的INCR AOF.
void aofDelTempIncrAofFile() {
    sds aof_filename = getTempIncrAofName();
    sds aof_filepath = makePath(server.aof_dirname, aof_filename);
    serverLog(LL_NOTICE, "删除后台的临时incr aof文件%s", aof_filename);
    bg_unlink(aof_filepath);
    sdsfree(aof_filepath);
    sdsfree(aof_filename);
    return;
}

/*
 * 当redis启动后调用' loadDataFromDisk '.如果server.aof_state”是'AOF_ON',它将做三件事:
 * 1.当redis开始一个空数据集时,强制创建一个BASE文件
 * 2.打开最近打开的INCR类型AOF进行写入,如果没有,创建一个新的
 * 3.同步地将清单文件更新到磁盘
 * 如果以上任何步骤失败,redis进程将退出.
 */
void aofOpenIfNeededOnServerStart(void) {
    if (server.aof_state != AOF_ON) {
        return;
    }

    serverAssert(server.aof_manifest != NULL);
    serverAssert(server.aof_fd == -1);

    if (dirCreateIfMissing(server.aof_dirname) == -1) {
        serverLog(LL_WARNING, "不能打开或创建只能追加的目录 %s: %s", server.aof_dirname, strerror(errno));
        exit(1);
    }

    // 如果我们从一个空数据集开始,我们将强制创建一个BASE文件.
    if (!server.aof_manifest->base_aof_info &&          // BASE文件信息.如果没有BASE文件,则为NULL.
        !listLength(server.aof_manifest->incr_aof_list) // INCR AOFs清单.重写失败时,我们可能有多个INCR AOF.
    ) {
        sds base_name = getNewBaseFileNameAndMarkPreAsHistory(server.aof_manifest);
        sds base_filepath = makePath(server.aof_dirname, base_name);
        if (rewriteAppendOnlyFile(base_filepath) != C_OK) {
            exit(1);
        }
        sdsfree(base_filepath);
    }

    sds aof_name = getLastIncrAofName(server.aof_manifest);

    sds aof_filepath = makePath(server.aof_dirname, aof_name);
    server.aof_fd = open(aof_filepath, O_WRONLY | O_APPEND | O_CREAT, 0644);
    sdsfree(aof_filepath);
    if (server.aof_fd == -1) {
        serverLog(LL_WARNING, "不能打开追加文件: %s", strerror(errno));
        exit(1);
    }

    int ret = persistAofManifest(server.aof_manifest);
    if (ret != C_OK) {
        exit(1);
    }

    server.aof_last_incr_size = getAppendOnlyFileSize(aof_name, NULL);
}

int openNewIncrAofForAppend(void) {
    serverAssert(server.aof_manifest != NULL);
    int newfd = -1;
    aofManifest *temp_am = NULL;
    sds new_aof_name = NULL;

    if (server.aof_state == AOF_OFF)
        return C_OK;

    if (server.aof_state == AOF_WAIT_REWRITE) {
        new_aof_name = getTempIncrAofName();
    }
    else {
        temp_am = aofManifestDup(server.aof_manifest);
        new_aof_name = sdsdup(getNewIncrAofName(temp_am));
    }
    sds new_aof_filepath = makePath(server.aof_dirname, new_aof_name);
    newfd = open(new_aof_filepath, O_WRONLY | O_TRUNC | O_CREAT, 0644);
    sdsfree(new_aof_filepath);
    if (newfd == -1) {
        serverLog(LL_WARNING, "Can't open the append-only file %s: %s", new_aof_name, strerror(errno));
        goto cleanup;
    }

    if (temp_am) {
        if (persistAofManifest(temp_am) == C_ERR) {
            goto cleanup;
        }
    }
    sdsfree(new_aof_name);

    if (server.aof_fd != -1)
        bioCreateCloseJob(server.aof_fd);
    server.aof_fd = newfd;

    server.aof_last_incr_size = 0;
    if (temp_am)
        aofManifestFreeAndUpdate(temp_am);
    return C_OK;

cleanup:
    if (new_aof_name)
        sdsfree(new_aof_name);
    if (newfd != -1)
        close(newfd);
    if (temp_am)
        aofManifestFree(temp_am);
    return C_ERR;
}

/* Whether to limit the execution of Background AOF rewrite.
 *
 * At present, if AOFRW fails, redis will automatically retry. If it continues
 * to fail, we may get a lot of very small INCR files. so we need an AOFRW
 * limiting measure.
 *
 * We can't directly use `server.aof_current_size` and `server.aof_last_incr_size`,
 * because there may be no new writes after AOFRW fails.
 *
 * So, we use time delay to achieve our goal. When AOFRW fails, we delay the execution
 * of the next AOFRW by 1 minute. If the next AOFRW also fails, it will be delayed by 2
 * minutes. The next is 4, 8, 16, the maximum delay is 60 minutes (1 hour).
 *
 * During the limit period, we can still use the 'bgrewriteaof' command to execute AOFRW
 * immediately.
 *
 * Return 1 means that AOFRW is limited and cannot be executed. 0 means that we can execute
 * AOFRW, which may be that we have reached the 'next_rewrite_time' or the number of INCR
 * AOFs has not reached the limit threshold.
 * */
#define AOF_REWRITE_LIMITE_THRESHOLD 3
#define AOF_REWRITE_LIMITE_MAX_MINUTES 60 /* 1 hour */

// 判断AOF文件是否超出阈值
int aofRewriteLimited(void) {
    static int next_delay_minutes = 0;
    static time_t next_rewrite_time = 0;

    if (server.stat_aofrw_consecutive_failures < AOF_REWRITE_LIMITE_THRESHOLD) {
        next_delay_minutes = 0;
        next_rewrite_time = 0;
        return 0;
    }

    if (next_rewrite_time != 0) {
        if (server.unixtime < next_rewrite_time) {
            return 1;
        }
        else {
            next_rewrite_time = 0;
            return 0;
        }
    }

    next_delay_minutes = (next_delay_minutes == 0) ? 1 : (next_delay_minutes * 2);
    if (next_delay_minutes > AOF_REWRITE_LIMITE_MAX_MINUTES) {
        next_delay_minutes = AOF_REWRITE_LIMITE_MAX_MINUTES;
    }

    next_rewrite_time = server.unixtime + next_delay_minutes * 60;
    serverLog(LL_WARNING, "后台AOF重写已多次失败并触发限制,将在%d分钟内重试", next_delay_minutes);
    return 1;
}

/* ----------------------------------------------------------------------------
 * AOF file implementation
 * ------------------------------------------------------------------------- */

// 如果一个AOf fsync当前正在BIO线程中进行,则返回true.
int aofFsyncInProgress(void) {
    return bioPendingJobsOfType(BIO_AOF_FSYNC) != 0;
}

/* Starts a background task that performs fsync() against the specified
 * file descriptor (the one of the AOF file) in another thread. */
// 在另一个线程中,对给定的描述符 fd （指向 AOF 文件）执行一个后台 fsync() 操作.
void aof_background_fsync(int fd) {
    bioCreateFsyncJob(fd);
}

/* Kills an AOFRW child process if exists */
void killAppendOnlyChild(void) {
    int statloc;
    /* No AOFRW child? return. */
    if (server.child_type != CHILD_TYPE_AOF)
        return;
    /* Kill AOFRW child, wait for child exit. */
    serverLog(LL_NOTICE, "Killing running AOF rewrite child: %ld", (long)server.child_pid);
    // * 如果 BGREWRITEAOF 正在执行,那么杀死它
    // * 并等待子进程退出
    // 杀死子进程

    if (kill(server.child_pid, SIGUSR1) != -1) {
        while (waitpid(-1, &statloc, 0) != server.child_pid) {
        }
    }
    aofRemoveTempFile(server.child_pid);
    resetChildState();
    server.aof_rewrite_time_start = -1;
}

/* Called when the user switches from "appendonly yes" to "appendonly no"
 * at runtime using the CONFIG command. */
// 在用户通过 CONFIG 命令在运行时关闭 AOF 持久化时调用
void stopAppendOnly(void) {
    // AOF 必须正在启用,才能调用这个函数
    serverAssert(server.aof_state != AOF_OFF);
    // 将 AOF 缓存的内容写入并冲洗到 AOF 文件中
    // 参数 1 表示强制模式
    flushAppendOnlyFile(1);
    // 冲洗 AOF 文件
    if (redis_fsync(server.aof_fd) == -1) {
        serverLog(LL_WARNING, "Fail to fsync the AOF file: %s", strerror(errno));
    }
    else {
        server.aof_fsync_offset = server.aof_current_size;
        server.aof_last_fsync = server.unixtime;
    }
    // 关闭 AOF 文件
    close(server.aof_fd);
    // 清空 AOF 状态
    server.aof_fd = -1;
    server.aof_selected_db = -1;
    server.aof_state = AOF_OFF;
    server.aof_rewrite_scheduled = 0;
    server.aof_last_incr_size = 0;
    // * 如果 BGREWRITEAOF 正在执行,那么杀死它
    // * 并等待子进程退出
    killAppendOnlyChild();
    sdsfree(server.aof_buf);
    server.aof_buf = sdsempty();
}

// 当 config set appendonly yes    状态发生改变,会执行AOF重写
int startAppendOnly(void) {
    serverAssert(server.aof_state == AOF_OFF);

    server.aof_state = AOF_WAIT_REWRITE;
    if (hasActiveChildProcess() && server.child_type != CHILD_TYPE_AOF) {
        server.aof_rewrite_scheduled = 1;
        serverLog(LL_WARNING, "AOF was enabled but there is already another background operation. An AOF background was scheduled to start when possible.");
    }
    else if (server.in_exec) {
        server.aof_rewrite_scheduled = 1;
        serverLog(LL_WARNING, "AOF was enabled during a transaction. An AOF background was scheduled to start when possible.");
    }
    else {
        /* If there is a pending AOF rewrite, we need to switch it off and
         * start a new one: the old one cannot be reused because it is not
         * accumulating the AOF buffer. */
        if (server.child_type == CHILD_TYPE_AOF) {
            serverLog(LL_WARNING, "AOF was enabled but there is already an AOF rewriting in background. Stopping background AOF and starting a rewrite now.");
            killAppendOnlyChild();
        }

        if (rewriteAppendOnlyFileBackground() == C_ERR) {
            // AOF 后台重写失败,关闭 AOF 文件
            server.aof_state = AOF_OFF;
            serverLog(LL_WARNING, "Redis needs to enable the AOF but can't trigger a background AOF rewrite operation. Check the above logs for more info about the error.");
            return C_ERR;
        }
    }
    // 将开始时间设为 AOF 最后一次 fsync 时间
    server.aof_last_fsync = server.unixtime;
    /* If AOF fsync error in bio job, we just ignore it and log the event. */
    int aof_bio_fsync_status;
    atomicGet(server.aof_bio_fsync_status, aof_bio_fsync_status);
    if (aof_bio_fsync_status == C_ERR) {
        serverLog(LL_WARNING, "AOF reopen, just ignore the AOF fsync error in bio job");
        atomicSet(server.aof_bio_fsync_status, C_OK);
    }

    /* If AOF was in error state, we just ignore it and log the event. */
    if (server.aof_last_write_status == C_ERR) {
        serverLog(LL_WARNING, "AOF reopen, just ignore the last error.");
        server.aof_last_write_status = C_OK;
    }
    return C_OK;
}

/* This is a wrapper to the write syscall in order to retry on short writes
 * or if the syscall gets interrupted. It could look strange that we retry
 * on short writes given that we are writing to a block device: normally if
 * the first call is short, there is a end-of-space condition, so the next
 * is likely to fail. However apparently in modern systems this is no longer
 * true, and in general it looks just more resilient to retry the write. If
 * there is an actual error condition we'll get it at the next try. */
ssize_t aofWrite(int fd, const char *buf, size_t len) {
    ssize_t nwritten = 0, totwritten = 0;

    while (len) {
        nwritten = write(fd, buf, len);

        if (nwritten < 0) {
            if (errno == EINTR)
                continue;
            return totwritten ? totwritten : -1;
        }

        len -= nwritten;
        buf += nwritten;
        totwritten += nwritten;
    }

    return totwritten;
}

/*
 * * 将 AOF 缓存写入到文件中.
*
* 因为程序需要在回复客户端之前对 AOF 执行写操作.
* 而客户端能执行写操作的唯一机会就是在事件 loop 中,
* 因此,程序将所有 AOF 写累积到缓存中,
* 并在重新进入事件 loop 之前,将缓存写入到文件中.
*
 *  * 关于 force 参数：

* 当 fsync 策略为每秒钟保存一次时,如果后台线程仍然有 fsync 在执行,
* 那么我们可能会延迟执行冲洗（flush）操作,
* 因为 Linux 上的 write(2) 会被后台的 fsync 阻塞.
*
* 当这种情况发生时,说明需要尽快冲洗 aof 缓存,
* 程序会尝试在 serverCron() 函数中对缓存进行冲洗.
*
* 不过,如果 force 为 1 的话,那么不管后台是否正在 fsync ,
* 程序都直接进行写入.
 *
 * */
#define AOF_WRITE_LOG_ERROR_RATE 30 /* Seconds between errors logging. */

// 将aof缓冲区里的数据刷到磁盘
void flushAppendOnlyFile(int force) {
    ssize_t nwritten;
    int sync_in_progress = 0;
    mstime_t latency;
    // 缓冲区中没有任何内容,直接返回
    if (sdslen(server.aof_buf) == 0) {
        // 策略为每秒 FSYNC
        if (server.aof_fsync == AOF_FSYNC_EVERYSEC &&             //
            server.aof_fsync_offset != server.aof_current_size && //
            server.unixtime > server.aof_last_fsync &&            //
            !(sync_in_progress = aofFsyncInProgress())            // 是否AOF子进程正在运行
        ) {
            goto try_fsync;
        }
        else {
            return;
        }
    }

    if (server.aof_fsync == AOF_FSYNC_EVERYSEC)
        sync_in_progress = aofFsyncInProgress(); // 是否有 SYNC 正在后台进行？
    // 每秒 fsync ,并且强制写入为假
    if (server.aof_fsync == AOF_FSYNC_EVERYSEC && !force) {
        /* With this append fsync policy we do background fsyncing.
         *
         * 当 fsync 策略为每秒钟一次时, fsync 在后台执行.
         *
         * If the fsync is still in progress we can try to delay
         * the write for a couple of seconds.
         *
         * 如果后台仍在执行 FSYNC ,那么我们可以延迟写操作一两秒
         * （如果强制执行 write 的话,服务器主线程将阻塞在 write 上面）
         */
        if (sync_in_progress) {
            // 有 fsync 正在后台进行 ...
            if (server.aof_flush_postponed_start == 0) {
                /* No previous write postponinig, remember that we are
                 * postponing the flush and return.
                 *
                 * 前面没有推迟过 write 操作,这里将推迟写操作的时间记录下来
                 * 然后就返回,不执行 write 或者 fsync
                 */
                server.aof_flush_postponed_start = server.unixtime;
                return;
            }
            else if (server.unixtime - server.aof_flush_postponed_start < 2) {
                /* We were already waiting for fsync to finish, but for less
                 * than two seconds this is still ok. Postpone again.
                 *
                 * 如果之前已经因为 fsync 而推迟了 write 操作
                 * 但是推迟的时间不超过 2 秒,那么直接返回
                 * 不执行 write 或者 fsync
                 */
                return;
            }

            /* Otherwise fall trough, and go write since we can't wait
             * over two seconds.
             *
             * 如果后台还有 fsync 在执行,并且 write 已经推迟 >= 2 秒
             * 那么执行写操作（write 将被阻塞）
             */
            server.aof_delayed_fsync++;
            serverLog(LL_NOTICE, "Asynchronous AOF fsync is taking too long (disk is busy?). Writing the AOF buffer without waiting for fsync to complete, this may slow down Redis.");
        }
    }
    /* We want to perform a single write. This should be guaranteed atomic
     * at least if the filesystem we are writing is a real physical one.
     *
     * 执行单个 write 操作,如果写入设备是物理的话,那么这个操作应该是原子的
     *
     * While this will save us against the server being killed I don't think
     * there is much to do about the whole server stopping for power problems
     * or alike
     *
     * 当然,如果出现像电源中断这样的不可抗现象,那么 AOF 文件也是可能会出现问题的
     * 这时就要用 redis-check-aof 程序来进行修复.
     */

    if (server.aof_flush_sleep && sdslen(server.aof_buf)) {
        usleep(server.aof_flush_sleep);
    }

    latencyStartMonitor(latency);
    nwritten = aofWrite(server.aof_fd, server.aof_buf, sdslen(server.aof_buf));
    latencyEndMonitor(latency);
    /* We want to capture different events for delayed writes:
     * when the delay happens with a pending fsync, or with a saving child
     * active, and when the above two conditions are missing.
     * We also use an additional event name to save all samples which is
     * useful for graphing / monitoring purposes. */
    if (sync_in_progress) {
        latencyAddSampleIfNeeded("aof-write-pending-fsync", latency);
    }
    else if (hasActiveChildProcess()) {
        latencyAddSampleIfNeeded("aof-write-active-child", latency);
    }
    else {
        latencyAddSampleIfNeeded("aof-write-alone", latency);
    }
    latencyAddSampleIfNeeded("aof-write", latency);

    /* We performed the write so reset the postponed flush sentinel to zero. */
    server.aof_flush_postponed_start = 0;

    if (nwritten != (ssize_t)sdslen(server.aof_buf)) {
        static time_t last_write_error_log = 0;
        int can_log = 0;

        /* Limit logging rate to 1 line per AOF_WRITE_LOG_ERROR_RATE seconds. */
        // 将日志的记录频率限制在每行 AOF_WRITE_LOG_ERROR_RATE 秒

        if ((server.unixtime - last_write_error_log) > AOF_WRITE_LOG_ERROR_RATE) {
            can_log = 1;
            last_write_error_log = server.unixtime;
        }

        /* Log the AOF write error and record the error code. */
        // 如果写入出错,那么尝试将该情况写入到日志里面

        if (nwritten == -1) {
            if (can_log) {
                serverLog(LL_WARNING, "Error writing to the AOF file: %s", strerror(errno));
                server.aof_last_write_errno = errno;
            }
        }
        else {
            if (can_log) {
                serverLog(LL_WARNING, "Short write while writing to the AOF file: (nwritten=%lld, expected=%lld)", (long long)nwritten, (long long)sdslen(server.aof_buf));
            }
            // 尝试移除新追加的不完整内容
            if (ftruncate(server.aof_fd, server.aof_last_incr_size) == -1) {
                if (can_log) {
                    serverLog(LL_WARNING, "Could not remove short write from the append-only file.  Redis may refuse to load the AOF the next time it starts.  ftruncate: %s", strerror(errno));
                }
            }
            else {
                /* If the ftruncate() succeeded we can set nwritten to
                 * -1 since there is no longer partial data into the AOF. */
                nwritten = -1;
            }
            server.aof_last_write_errno = ENOSPC;
        }

        /* Handle the AOF write error. */
        // 处理写入 AOF 文件时出现的错误

        if (server.aof_fsync == AOF_FSYNC_ALWAYS) {
            /* We can't recover when the fsync policy is ALWAYS since the reply
             * for the client is already in the output buffers (both writes and
             * reads), and the changes to the db can't be rolled back. Since we
             * have a contract with the user that on acknowledged or observed
             * writes are is synced on disk, we must exit. */
            serverLog(LL_WARNING, "Can't recover from AOF write error when the AOF fsync policy is 'always'. Exiting...");
            exit(1);
        }
        else {
            /* Recover from failed write leaving data into the buffer. However
             * set an error to stop accepting writes as long as the error
             * condition is not cleared. */
            server.aof_last_write_status = C_ERR;

            /* Trim the sds buffer if there was a partial write, and there
             * was no way to undo it with ftruncate(2). */
            if (nwritten > 0) {
                server.aof_current_size += nwritten;
                server.aof_last_incr_size += nwritten;
                sdsrange(server.aof_buf, nwritten, -1);
            }
            return; /* We'll try again on the next call... */
        }
    }
    else {
        /* Successful write(2). If AOF was in error state, restore the
         * OK state and log the event. */
        // 写入成功,更新最后写入状态

        if (server.aof_last_write_status == C_ERR) {
            serverLog(LL_WARNING, "AOF write error looks solved, Redis can write again.");
            server.aof_last_write_status = C_OK;
        }
    }
    // 更新写入后的 AOF 文件大小

    server.aof_current_size += nwritten;
    server.aof_last_incr_size += nwritten;

    /* Re-use AOF buffer when it is small enough. The maximum comes from the
     * arena size of 4k minus some overhead (but is otherwise arbitrary).
     *
     * 如果 AOF 缓存的大小足够小的话,那么重用这个缓存,
     * 否则的话,释放 AOF 缓存.
     */
    if ((sdslen(server.aof_buf) + sdsavail(server.aof_buf)) < 4000) {
        // 清空缓存中的内容,等待重用
        sdsclear(server.aof_buf);
    }
    else {
        // 释放缓存
        sdsfree(server.aof_buf);
        server.aof_buf = sdsempty();
    }

try_fsync:
    //     * 如果 no-appendfsync-on-rewrite 选项为开启状态,
    //     * 并且有 BGSAVE 或者 BGREWRITEAOF 正在进行的话,
    //     * 那么不执行 fsync
    if (server.aof_no_fsync_on_rewrite && hasActiveChildProcess())
        return;

    // 总是执行 fsnyc
    if (server.aof_fsync == AOF_FSYNC_ALWAYS) {
        // redis_fsync在Linux中定义为fdatasync(),以避免刷新元数据.
        latencyStartMonitor(latency); // 开始计时

        // 让我们试着把这些数据放到磁盘上.当AOF的fsync策略为'always'时,为了保证数据的安全,如果fsync AOF失败,我们应该退出(参见上面写错误后exit(1)旁边的注释).
        if (redis_fsync(server.aof_fd) == -1) { // 实际数据写入磁盘
            serverLog(LL_WARNING, "当AOF fsync策略为'always'时,不能持久化AOF错误：%s.正在退出...", strerror(errno));
            exit(1);
        }
        latencyEndMonitor(latency); // 结束计时,并计算时长
        latencyAddSampleIfNeeded("aof-fsync-always", latency);
        server.aof_fsync_offset = server.aof_current_size;
        // 更新最后一次执行 fsnyc 的时间
        server.aof_last_fsync = server.unixtime;
    } // 策略为每秒 fsnyc ,并且距离上次 fsync 已经超过 1 秒
    else if ((server.aof_fsync == AOF_FSYNC_EVERYSEC && server.unixtime > server.aof_last_fsync)) {
        if (!sync_in_progress) {
            // 放到后台执行
            aof_background_fsync(server.aof_fd);
            server.aof_fsync_offset = server.aof_current_size;
        }
        // 更新最后一次执行 fsync 的时间
        server.aof_last_fsync = server.unixtime;
    }
}

// 根据传入的命令和命令参数,将它们还原成协议格式.
sds catAppendOnlyGenericCommand(sds dst, int argc, robj **argv) {
    char buf[32];
    int len, j;
    robj *o;
    // 重建命令的个数,格式为 *<count>\r\n
    // 例如 *3\r\n
    buf[0] = '*';
    len = 1 + ll2string(buf + 1, sizeof(buf) - 1, argc);
    buf[len++] = '\r';
    buf[len++] = '\n';
    dst = sdscatlen(dst, buf, len);
    // 重建命令和命令参数,格式为 $<length>\r\n<content>\r\n
    // 例如 $3\r\nSET\r\n$3\r\nKEY\r\n$5\r\nVALUE\r\n
    for (j = 0; j < argc; j++) {
        o = getDecodedObject(argv[j]);
        // 组合 $<length>\r\n
        buf[0] = '$';
        len = 1 + ll2string(buf + 1, sizeof(buf) - 1, sdslen(o->ptr));
        buf[len++] = '\r';
        buf[len++] = '\n';
        dst = sdscatlen(dst, buf, len);
        // 组合 <content>\r\n
        dst = sdscatlen(dst, o->ptr, sdslen(o->ptr));
        dst = sdscatlen(dst, "\r\n", 2);
        decrRefCount(o);
    }
    // 返回重建后的协议内容
    return dst;
}

// 生成AOF日志中的时间戳
sds genAofTimestampAnnotationIfNeeded(int force) {
    sds ts = NULL;

    if (force || server.aof_cur_timestamp < server.unixtime) {
        server.aof_cur_timestamp = force ? time(NULL) : server.unixtime;
        ts = sdscatfmt(sdsempty(), "#TS:%I\r\n", server.aof_cur_timestamp);
        serverAssert(sdslen(ts) <= AOF_ANNOTATION_LINE_MAX_LEN);
    }
    return ts;
}

// 将命令追加到 AOF 文件中,如果 AOF 重写正在进行,那么也将命令追加到 AOF 重写缓存中.
void feedAppendOnlyFile(int dictid, robj **argv, int argc) {
    sds buf = sdsempty();
    serverAssert(dictid >= 0 && dictid < server.dbnum);

    if (server.aof_timestamp_enabled) { // 如果需要,提供时间戳
        sds ts = genAofTimestampAnnotationIfNeeded(0);
        if (ts != NULL) {
            buf = sdscatsds(buf, ts);
            sdsfree(ts);
        }
    }
    // 使用 SELECT 命令,显式设置数据库,确保之后的命令被设置到正确的数据库
    if (dictid != server.aof_selected_db) {
        char seldb[64];
        snprintf(seldb, sizeof(seldb), "%d", dictid);
        buf = sdscatprintf(buf, "*2\r\n$6\r\nSELECT\r\n$%lu\r\n%s\r\n", (unsigned long)strlen(seldb), seldb);
        server.aof_selected_db = dictid;
    }

    // 所有命令在AOF中的传播方式都应该与在复制中的传播方式相同.不需要aof特定的翻译.
    buf = catAppendOnlyGenericCommand(buf, argc, argv);

    //      * 将命令追加到 AOF 缓存中,
    //     * 在重新进入事件循环之前,这些命令会被冲洗到磁盘上,
    //     * 并向客户端返回一个回复.
    if (server.aof_state == AOF_ON ||            //
        (server.aof_state == AOF_WAIT_REWRITE && //
         server.child_type == CHILD_TYPE_AOF)    //
    ) {
        server.aof_buf = sdscatlen(server.aof_buf, buf, sdslen(buf)); // 将日志加入到缓存中
    }
    // 释放
    sdsfree(buf);
}

/* ----------------------------------------------------------------------------
 * AOF loading
 * ------------------------------------------------------------------------- */

// * Redis 命令必须由客户端执行,
// * 所以 AOF 装载程序需要创建一个无网络连接的客户端来执行 AOF 文件中的命令.
struct client *createAOFClient(void) {
    struct client *c = createClient(NULL);

    c->id = CLIENT_ID_AOF; /* So modules can identify it's the AOF client. */

    /*
     * The AOF client should never be blocked (unlike master
     * replication connection).
     * This is because blocking the AOF client might cause
     * deadlock (because potentially no one will unblock it).
     * Also, if the AOF client will be blocked just for
     * background processing there is a chance that the
     * command execution order will be violated.
     */
    c->flags = CLIENT_DENY_BLOCKING;

    /* We set the fake client as a slave waiting for the synchronization
     * so that Redis will not try to send replies to this client.
     *
     * 将客户端设置为正在等待同步的附属节点,这样客户端就不会发送回复了.
     */
    c->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
    return c;
}

/* 重放追加日志文件.如果成功返回AOF_OK或AOF_TRUNCATED,否则返回以下其中之一:
 * AOF_OPEN_ERR: Failed to open the AOF file.
 * AOF_NOT_EXIST: AOF file doesn't exist.
 * AOF_EMPTY: The AOF file is empty (nothing to load).
 * AOF_FAILED: Failed to load the AOF file. */
int loadSingleAppendOnlyFile(char *filename) {
    struct client *fakeClient;
    struct redis_stat sb;
    int old_aof_state = server.aof_state;
    long loops = 0;
    off_t valid_up_to = 0;        /* Offset of latest well-formed command loaded. */
    off_t valid_before_multi = 0; /* Offset before MULTI command loaded. */
    off_t last_progress_report_size = 0;
    int ret = C_OK;

    sds aof_filepath = makePath(server.aof_dirname, filename);
    FILE *fp = fopen(aof_filepath, "r");
    if (fp == NULL) {
        int en = errno;
        if (redis_stat(aof_filepath, &sb) == 0 || errno != ENOENT) {
            serverLog(LL_WARNING, "致命错误:无法打开附加日志文件%s以读取:%s", filename, strerror(en));
            sdsfree(aof_filepath);
            return AOF_OPEN_ERR;
        }
        else {
            serverLog(LL_WARNING, "追加日志文件%s不存在:%s", filename, strerror(errno));
            sdsfree(aof_filepath);
            return AOF_NOT_EXIST;
        }
    }
    // 检查文件的正确性
    if (fp && redis_fstat(fileno(fp), &sb) != -1 && sb.st_size == 0) {
        fclose(fp);
        sdsfree(aof_filepath);
        return AOF_EMPTY;
    }

    // 暂时性地关闭 AOF ,防止在执行 MULTI 时,EXEC 命令被传播到正在打开的 AOF 文件中.
    server.aof_state = AOF_OFF;

    client *old_client = server.current_client;
    fakeClient = server.current_client = createAOFClient();

    /* Check if the AOF file is in RDB format (it may be RDB encoded base AOF
     * or old style RDB-preamble AOF). In that case we need to load the RDB file
     * and later continue loading the AOF tail if it is an old style RDB-preamble AOF. */
    char sig[5]; /* "REDIS" */
    if (fread(sig, 1, 5, fp) != 5 || memcmp(sig, "REDIS", 5) != 0) {
        /* Not in RDB format, seek back at 0 offset. */
        if (fseek(fp, 0, SEEK_SET) == -1)
            goto readerr;
    }
    else {
        /* RDB format. Pass loading the RDB functions. */
        rio rdb;
        int old_style = !strcmp(filename, server.aof_filename);
        if (old_style)
            serverLog(LL_NOTICE, "从AOF文件读取RDB序言…");
        else
            serverLog(LL_NOTICE, "读取RDB基础文件在AOF加载…");

        if (fseek(fp, 0, SEEK_SET) == -1)
            goto readerr;
        rioInitWithFile(&rdb, fp);
        if (rdbLoadRio(&rdb, RDBFLAGS_AOF_PREAMBLE, NULL) != C_OK) {
            if (old_style)
                serverLog(LL_WARNING, "读取AOF文件%s的RDB序文错误,AOF加载中止", filename);
            else
                serverLog(LL_WARNING, "读取RDB基本文件%s时出错,AOF加载中止", filename);

            goto readerr;
        }
        else {
            loadingAbsProgress(ftello(fp));
            last_progress_report_size = ftello(fp);
            if (old_style)
                serverLog(LL_NOTICE, "Reading the remaining AOF tail...");
        }
    }

    while (1) {
        int argc, j;
        unsigned long len;
        robj **argv;
        char buf[AOF_ANNOTATION_LINE_MAX_LEN];
        sds argsds;
        struct redisCommand *cmd;

        // 间隔性地处理客户端发送来的请求
        // 因为服务器正处于载入状态,所以能正常执行的只有 PUBSUB 等模块
        if (!(loops++ % 1024)) {
            off_t progress_delta = ftello(fp) - last_progress_report_size;
            loadingIncrProgress(progress_delta);
            last_progress_report_size += progress_delta;
            processEventsWhileBlocked();
            processModuleLoadingProgressEvent(1);
        }
        // 读入文件内容到缓存
        if (fgets(buf, sizeof(buf), fp) == NULL) {
            if (feof(fp)) { // 文件已经读完,跳出
                break;
            }
            else {
                goto readerr;
            }
        }
        if (buf[0] == '#')
            continue;

        if (buf[0] != '*') // 确认协议格式,比如 *3\r\n

            goto fmterr;
        if (buf[1] == '\0')
            goto readerr;
        argc = atoi(buf + 1); // 取出命令参数,比如 *3\r\n 中的 3

        if (argc < 1)
            goto fmterr; // 至少要有一个参数（被调用的命令）

        if ((size_t)argc > SIZE_MAX / sizeof(robj *))
            goto fmterr;

        // 从文本中创建字符串对象:包括命令,以及命令参数
        // 例如 $3\r\nSET\r\n$3\r\nKEY\r\n$5\r\nVALUE\r\n
        // 将创建三个包含以下内容的字符串对象:
        // SET 、 KEY 、 VALUE
        argv = zmalloc(sizeof(robj *) * argc);
        fakeClient->argc = argc;
        fakeClient->argv = argv;
        fakeClient->argv_len = argc;

        for (j = 0; j < argc; j++) {
            /* Parse the argument len. */
            char *readres = fgets(buf, sizeof(buf), fp);
            if (readres == NULL || buf[0] != '$') {
                fakeClient->argc = j; /* Free up to j-1. */
                freeClientArgv(fakeClient);
                if (readres == NULL)
                    goto readerr;
                else
                    goto fmterr;
            }
            len = strtol(buf + 1, NULL, 10); // 读取参数值的长度
            // 读取参数值
            argsds = sdsnewlen(SDS_NOINIT, len);
            if (len && fread(argsds, len, 1, fp) == 0) {
                sdsfree(argsds);
                fakeClient->argc = j; /* Free up to j-1. */
                freeClientArgv(fakeClient);
                goto readerr;
            }
            // 为参数创建对象
            argv[j] = createObject(OBJ_STRING, argsds);

            /* Discard CRLF. */
            if (fread(buf, 2, 1, fp) == 0) {
                fakeClient->argc = j + 1; /* Free up to j. */
                freeClientArgv(fakeClient);
                goto readerr;
            }
        }
        // 查找命令
        cmd = lookupCommand(argv, argc);
        if (!cmd) {
            serverLog(LL_WARNING, "Unknown command '%s' reading the append only file %s", (char *)argv[0]->ptr, filename);
            freeClientArgv(fakeClient);
            ret = AOF_FAILED;
            goto cleanup;
        }

        if (cmd->proc == multiCommand)
            valid_before_multi = valid_up_to;

        // 调用伪客户端,执行命令
        fakeClient->cmd = fakeClient->lastcmd = cmd;
        // 清理命令和命令参数对象
        if (fakeClient->flags & CLIENT_MULTI && fakeClient->cmd->proc != execCommand) {
            queueMultiCommand(fakeClient);
        }
        else {
            cmd->proc(fakeClient);
        }

        /* The fake client should not have a reply */
        serverAssert(fakeClient->bufpos == 0 && listLength(fakeClient->reply) == 0);

        /* The fake client should never get blocked */
        serverAssert((fakeClient->flags & CLIENT_BLOCKED) == 0);

        /* Clean up. Command code may have changed argv/argc so we use the
         * argv/argc of the client instead of the local variables. */
        // 清理命令和命令参数对象
        freeClientArgv(fakeClient);
        if (server.aof_load_truncated)
            valid_up_to = ftello(fp);
        if (server.key_load_delay)
            debugDelay(server.key_load_delay);
    }

    /* This point can only be reached when EOF is reached without errors.
     * If the client is in the middle of a MULTI/EXEC, handle it as it was
     * a short read, even if technically the protocol is correct: we want
     * to remove the unprocessed tail and continue. */
    // 如果能执行到这里,说明 AOF 文件的全部内容都可以正确地读取,
    // 但是,还要检查 AOF 是否包含未正确结束的事务
    if (fakeClient->flags & CLIENT_MULTI) {
        serverLog(LL_WARNING, "Revert incomplete MULTI/EXEC transaction in AOF file %s", filename);
        valid_up_to = valid_before_multi;
        goto uxeof;
    }

loaded_ok: /* DB loaded, cleanup and return C_OK to the caller. */
    loadingIncrProgress(ftello(fp) - last_progress_report_size);
    server.aof_state = old_aof_state; // 复原 AOF 状态
    goto cleanup;

readerr: /* Read error. If feof(fp) is true, fall through to unexpected EOF. */
    // 非预期的末尾,可能是 AOF 文件在写入的中途遭遇了停机

    if (!feof(fp)) {
        // 文件内容出错
        serverLog(LL_WARNING, "Unrecoverable error reading the append only file %s: %s", filename, strerror(errno));
        ret = AOF_FAILED;
        goto cleanup;
    }

uxeof: /* Unexpected AOF end of file. */
    if (server.aof_load_truncated) {
        serverLog(LL_WARNING, "!!! Warning: short read while loading the AOF file %s!!!", filename);
        serverLog(LL_WARNING, "!!! Truncating the AOF %s at offset %llu !!!", filename, (unsigned long long)valid_up_to);
        if (valid_up_to == -1 || truncate(aof_filepath, valid_up_to) == -1) {
            if (valid_up_to == -1) {
                serverLog(LL_WARNING, "Last valid command offset is invalid");
            }
            else {
                serverLog(LL_WARNING, "Error truncating the AOF file %s: %s", filename, strerror(errno));
            }
        }
        else {
            /* Make sure the AOF file descriptor points to the end of the
             * file after the truncate call. */
            if (server.aof_fd != -1 && lseek(server.aof_fd, 0, SEEK_END) == -1) {
                serverLog(LL_WARNING, "Can't seek the end of the AOF file %s: %s", filename, strerror(errno));
            }
            else {
                serverLog(LL_WARNING, "AOF %s loaded anyway because aof-load-truncated is enabled", filename);
                ret = AOF_TRUNCATED;
                goto loaded_ok;
            }
        }
    }
    serverLog(
        LL_WARNING,
        "Unexpected end of file reading the append only file %s. You can: "
        "1) Make a backup of your AOF file, then use ./redis-check-aof --fix <filename.manifest>. "
        "2) Alternatively you can set the 'aof-load-truncated' configuration option to yes and restart the server.",
        filename);
    ret = AOF_FAILED;
    goto cleanup;

fmterr: // 内容格式错误

    serverLog(LL_WARNING, "Bad file format reading the append only file %s: make a backup of your AOF file, then use ./redis-check-aof --fix <filename.manifest>", filename);
    ret = AOF_FAILED;
    /* fall through to cleanup. */

cleanup:
    if (fakeClient)
        freeClient(fakeClient);
    server.current_client = old_client;
    fclose(fp); // 关闭 AOF 文件

    sdsfree(aof_filepath);
    return ret;
}

// 执行 AOF 文件中的命令.
int loadAppendOnlyFiles(aofManifest *am) {
    serverAssert(am != NULL);
    int status, ret = C_OK;
    long long start;
    off_t total_size = 0, base_size = 0;
    sds aof_name;
    int total_num, aof_num = 0, last_file;

    /* If the 'server.aof_filename' file exists in dir, we may be starting
     * from an old redis version. We will use enter upgrade mode in three situations.
     *
     * 1. If the 'server.aof_dirname' directory not exist
     * 2. If the 'server.aof_dirname' directory exists but the manifest file is missing
     * 3. If the 'server.aof_dirname' directory exists and the manifest file it contains
     *    has only one base AOF record, and the file name of this base AOF is 'server.aof_filename',
     *    and the 'server.aof_filename' file not exist in 'server.aof_dirname' directory
     * */
    if (fileExist(server.aof_filename)) {
        if (!dirExists(server.aof_dirname) || (am->base_aof_info == NULL && listLength(am->incr_aof_list) == 0) || (am->base_aof_info != NULL && listLength(am->incr_aof_list) == 0 && !strcmp(am->base_aof_info->file_name, server.aof_filename) && !aofFileExist(server.aof_filename))) {
            aofUpgradePrepare(am);
        }
    }

    if (am->base_aof_info == NULL && listLength(am->incr_aof_list) == 0) {
        return AOF_NOT_EXIST;
    }

    total_num = getBaseAndIncrAppendOnlyFilesNum(am);
    serverAssert(total_num > 0);

    /* Here we calculate the total size of all BASE and INCR files in
     * advance, it will be set to `server.loading_total_bytes`. */
    total_size = getBaseAndIncrAppendOnlyFilesSize(am, &status);
    if (status != AOF_OK) {
        /* If an AOF exists in the manifest but not on the disk, we consider this to be a fatal error. */
        if (status == AOF_NOT_EXIST)
            status = AOF_FAILED;

        return status;
    }
    else if (total_size == 0) {
        return AOF_EMPTY;
    }
    // 设置服务器的状态为:正在载入
    startLoading(total_size, RDBFLAGS_AOF_PREAMBLE, 0);

    /* Load BASE AOF if needed. */
    if (am->base_aof_info) {
        serverAssert(am->base_aof_info->file_type == AOF_FILE_TYPE_BASE);
        aof_name = (char *)am->base_aof_info->file_name;
        updateLoadingFileName(aof_name);
        base_size = getAppendOnlyFileSize(aof_name, NULL);
        last_file = ++aof_num == total_num;
        start = ustime();
        ret = loadSingleAppendOnlyFile(aof_name);
        if (ret == AOF_OK || (ret == AOF_TRUNCATED && last_file)) {
            serverLog(LL_NOTICE, "DB loaded from base file %s: %.3f seconds", aof_name, (float)(ustime() - start) / 1000000);
        }

        /* If the truncated file is not the last file, we consider this to be a fatal error. */
        if (ret == AOF_TRUNCATED && !last_file) {
            ret = AOF_FAILED;
        }

        if (ret == AOF_OPEN_ERR || ret == AOF_FAILED) {
            goto cleanup;
        }
    }

    /* Load INCR AOFs if needed. */
    if (listLength(am->incr_aof_list)) {
        listNode *ln;
        listIter li;

        listRewind(am->incr_aof_list, &li);
        while ((ln = listNext(&li)) != NULL) {
            aofInfo *ai = (aofInfo *)ln->value;
            serverAssert(ai->file_type == AOF_FILE_TYPE_INCR);
            aof_name = (char *)ai->file_name;
            updateLoadingFileName(aof_name);
            last_file = ++aof_num == total_num;
            start = ustime();
            ret = loadSingleAppendOnlyFile(aof_name);
            if (ret == AOF_OK || (ret == AOF_TRUNCATED && last_file)) {
                serverLog(LL_NOTICE, "DB loaded from incr file %s: %.3f seconds", aof_name, (float)(ustime() - start) / 1000000);
            }

            /* We know that (at least) one of the AOF files has data (total_size > 0),
             * so empty incr AOF file doesn't count as a AOF_EMPTY result */
            if (ret == AOF_EMPTY)
                ret = AOF_OK;

            if (ret == AOF_TRUNCATED && !last_file) {
                ret = AOF_FAILED;
            }

            if (ret == AOF_OPEN_ERR || ret == AOF_FAILED) {
                goto cleanup;
            }
        }
    }

    server.aof_current_size = total_size;
    /* Ideally, the aof_rewrite_base_size variable should hold the size of the
     * AOF when the last rewrite ended, this should include the size of the
     * incremental file that was created during the rewrite since otherwise we
     * risk the next automatic rewrite to happen too soon (or immediately if
     * auto-aof-rewrite-percentage is low). However, since we do not persist
     * aof_rewrite_base_size information anywhere, we initialize it on restart
     * to the size of BASE AOF file. This might cause the first AOFRW to be
     * executed early, but that shouldn't be a problem since everything will be
     * fine after the first AOFRW. */
    server.aof_rewrite_base_size = base_size; // 记录前一次重写时的大小

    server.aof_fsync_offset = server.aof_current_size;

cleanup:
    // 停止载入
    stopLoading(ret == AOF_OK || ret == AOF_TRUNCATED);
    return ret;
}

/* ----------------------------------------------------------------------------
 * AOF rewrite
 * ------------------------------------------------------------------------- */

/* Delegate writing an object to writing a bulk string or bulk long long.
 * This is not placed in rio.c since that adds the server.h dependency. */
// 将 obj 所指向的整数对象或字符串对象的值写入到 r 当中.
int rioWriteBulkObject(rio *r, robj *obj) {
    /* Avoid using getDecodedObject to help copy-on-write (we are often
     * in a child process when this function is called). */
    if (obj->encoding == OBJ_ENCODING_INT) {
        return rioWriteBulkLongLong(r, (long)obj->ptr);
    }
    else if (sdsEncodedObject(obj)) {
        return rioWriteBulkString(r, obj->ptr, sdslen(obj->ptr));
    }
    else {
        serverPanic("Unknown string encoding");
    }
}

/* Emit the commands needed to rebuild a list object.
 * The function returns 0 on error, 1 on success. */
// 将重建列表对象所需的命令写入到 r .
// 出错返回 0 ,成功返回 1 .
// 命令的形式如下：  RPUSH item1 item2 ... itemN
int rewriteListObject(rio *r, robj *key, robj *o) {
    long long count = 0, items = listTypeLength(o);

    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklist *list = o->ptr;
        quicklistIter *li = quicklistGetIterator(list, AL_START_HEAD);
        quicklistEntry entry;
        // 先构建一个 RPUSH key
        // 然后从 ZIPLIST 中取出最多 REDIS_AOF_REWRITE_ITEMS_PER_CMD 个元素
        // 之后重复第一步,直到 ZIPLIST 为空
        while (quicklistNext(li, &entry)) {
            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ? AOF_REWRITE_ITEMS_PER_CMD : items;
                if (!rioWriteBulkCount(r, '*', 2 + cmd_items) || !rioWriteBulkString(r, "RPUSH", 5) || !rioWriteBulkObject(r, key)) {
                    quicklistReleaseIterator(li);
                    return 0;
                }
            }
            // 取出值

            if (entry.value) {
                if (!rioWriteBulkString(r, (char *)entry.value, entry.sz)) {
                    quicklistReleaseIterator(li);
                    return 0;
                }
            }
            else {
                if (!rioWriteBulkLongLong(r, entry.longval)) {
                    quicklistReleaseIterator(li);
                    return 0;
                }
            }
            // 元素计数
            if (++count == AOF_REWRITE_ITEMS_PER_CMD)
                count = 0;
            items--;
        }
        quicklistReleaseIterator(li);
    }
    else {
        serverPanic("Unknown list encoding");
    }
    return 1;
}

/* Emit the commands needed to rebuild a set object.
 * The function returns 0 on error, 1 on success.
 *
 * 将重建集合对象所需的命令写入到 r .
 * 出错返回 0 ,成功返回 1 .
 * 命令的形式如下：  SADD item1 item2 ... itemN
 */
int rewriteSetObject(rio *r, robj *key, robj *o) {
    long long count = 0, items = setTypeSize(o);

    if (o->encoding == OBJ_ENCODING_INTSET) {
        int ii = 0;
        int64_t llval;

        while (intsetGet(o->ptr, ii++, &llval)) {
            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ? AOF_REWRITE_ITEMS_PER_CMD : items;

                if (!rioWriteBulkCount(r, '*', 2 + cmd_items) || !rioWriteBulkString(r, "SADD", 4) || !rioWriteBulkObject(r, key)) {
                    return 0;
                }
            }
            if (!rioWriteBulkLongLong(r, llval))
                return 0;
            if (++count == AOF_REWRITE_ITEMS_PER_CMD)
                count = 0;
            items--;
        }
    }
    else if (o->encoding == OBJ_ENCODING_HT) {
        dictIterator *di = dictGetIterator(o->ptr);
        dictEntry *de;

        while ((de = dictNext(di)) != NULL) {
            sds ele = dictGetKey(de);
            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ? AOF_REWRITE_ITEMS_PER_CMD : items;

                if (!rioWriteBulkCount(r, '*', 2 + cmd_items) || !rioWriteBulkString(r, "SADD", 4) || !rioWriteBulkObject(r, key)) {
                    dictReleaseIterator(di);
                    return 0;
                }
            }
            if (!rioWriteBulkString(r, ele, sdslen(ele))) {
                dictReleaseIterator(di);
                return 0;
            }
            if (++count == AOF_REWRITE_ITEMS_PER_CMD)
                count = 0;
            items--;
        }
        dictReleaseIterator(di);
    }
    else {
        serverPanic("Unknown set encoding");
    }
    return 1;
}

/* Emit the commands needed to rebuild a sorted set object.
 * The function returns 0 on error, 1 on success.
 *
 * 将重建有序集合对象所需的命令写入到 r .
 * 出错返回 0 ,成功返回 1 .
 * 命令的形式如下：  ZADD score1 member1 score2 member2 ... scoreN memberN
 */
int rewriteSortedSetObject(rio *r, robj *key, robj *o) {
    long long count = 0, items = zsetLength(o);

    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = o->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vll;
        double score;

        eptr = lpSeek(zl, 0);
        serverAssert(eptr != NULL);
        sptr = lpNext(zl, eptr);
        serverAssert(sptr != NULL);

        while (eptr != NULL) {
            vstr = lpGetValue(eptr, &vlen, &vll);
            score = zzlGetScore(sptr);

            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ? AOF_REWRITE_ITEMS_PER_CMD : items;

                if (!rioWriteBulkCount(r, '*', 2 + cmd_items * 2) || !rioWriteBulkString(r, "ZADD", 4) || !rioWriteBulkObject(r, key)) {
                    return 0;
                }
            }
            if (!rioWriteBulkDouble(r, score))
                return 0;
            if (vstr != NULL) {
                if (!rioWriteBulkString(r, (char *)vstr, vlen))
                    return 0;
            }
            else {
                if (!rioWriteBulkLongLong(r, vll))
                    return 0;
            }
            zzlNext(zl, &eptr, &sptr);
            if (++count == AOF_REWRITE_ITEMS_PER_CMD)
                count = 0;
            items--;
        }
    }
    else if (o->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = o->ptr;
        dictIterator *di = dictGetIterator(zs->dict);
        dictEntry *de;

        while ((de = dictNext(di)) != NULL) {
            sds ele = dictGetKey(de);
            double *score = dictGetVal(de);

            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ? AOF_REWRITE_ITEMS_PER_CMD : items;

                if (!rioWriteBulkCount(r, '*', 2 + cmd_items * 2) || !rioWriteBulkString(r, "ZADD", 4) || !rioWriteBulkObject(r, key)) {
                    dictReleaseIterator(di);
                    return 0;
                }
            }
            if (!rioWriteBulkDouble(r, *score) || !rioWriteBulkString(r, ele, sdslen(ele))) {
                dictReleaseIterator(di);
                return 0;
            }
            if (++count == AOF_REWRITE_ITEMS_PER_CMD)
                count = 0;
            items--;
        }
        dictReleaseIterator(di);
    }
    else {
        serverPanic("Unknown sorted zset encoding");
    }
    return 1;
}

/* Write either the key or the value of the currently selected item of a hash.
 *
 * 选择写入哈希的 key 或者 value 到 r 中.
 *
 *
 * hi 为 Redis 哈希迭代器
 *
 * The 'what' filed specifies if to write a key or a value and can be
 * either REDIS_HASH_KEY or REDIS_HASH_VALUE.
 *
 * what 决定了要写入的部分,可以是 REDIS_HASH_KEY 或 REDIS_HASH_VALUE
 *
 * The function returns 0 on error, non-zero on success.
 *
 * 出错返回 0 ,成功返回非 0 .
 */
static int rioWriteHashIteratorCursor(rio *r, hashTypeIterator *hi, int what) {
    if (hi->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        hashTypeCurrentFromListpack(hi, what, &vstr, &vlen, &vll);
        if (vstr)
            return rioWriteBulkString(r, (char *)vstr, vlen);
        else
            return rioWriteBulkLongLong(r, vll);
    }
    else if (hi->encoding == OBJ_ENCODING_HT) {
        sds value = hashTypeCurrentFromHashTable(hi, what);
        return rioWriteBulkString(r, value, sdslen(value));
    }

    serverPanic("Unknown hash encoding");
    return 0;
}

/* Emit the commands needed to rebuild a hash object.
 * The function returns 0 on error, 1 on success.
 *
 * 将重建哈希对象所需的命令写入到 r .
 *
 * 出错返回 0 ,成功返回 1 .
 *
 * 命令的形式如下：HMSET field1 value1 field2 value2 ... fieldN valueN
 */
int rewriteHashObject(rio *r, robj *key, robj *o) {
    hashTypeIterator *hi;
    long long count = 0, items = hashTypeLength(o);

    hi = hashTypeInitIterator(o);
    while (hashTypeNext(hi) != C_ERR) {
        if (count == 0) {
            int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ? AOF_REWRITE_ITEMS_PER_CMD : items;

            if (!rioWriteBulkCount(r, '*', 2 + cmd_items * 2) || !rioWriteBulkString(r, "HMSET", 5) || !rioWriteBulkObject(r, key)) {
                hashTypeReleaseIterator(hi);
                return 0;
            }
        }

        if (!rioWriteHashIteratorCursor(r, hi, OBJ_HASH_KEY) || !rioWriteHashIteratorCursor(r, hi, OBJ_HASH_VALUE)) {
            hashTypeReleaseIterator(hi);
            return 0;
        }
        if (++count == AOF_REWRITE_ITEMS_PER_CMD)
            count = 0;
        items--;
    }

    hashTypeReleaseIterator(hi);

    return 1;
}

/* Helper for rewriteStreamObject() that generates a bulk string into the
 * AOF representing the ID 'id'. */
int rioWriteBulkStreamID(rio *r, streamID *id) {
    int retval;

    sds replyid = sdscatfmt(sdsempty(), "%U-%U", id->ms, id->seq);
    retval = rioWriteBulkString(r, replyid, sdslen(replyid));
    sdsfree(replyid);
    return retval;
}

/* Helper for rewriteStreamObject(): emit the XCLAIM needed in order to
 * add the message described by 'nack' having the id 'rawid', into the pending
 * list of the specified consumer. All this in the context of the specified
 * key and group. */
int rioWriteStreamPendingEntry(rio *r, robj *key, const char *groupname, size_t groupname_len, streamConsumer *consumer, unsigned char *rawid, streamNACK *nack) {
    /* XCLAIM <key> <group> <consumer> 0 <id> TIME <milliseconds-unix-time>
              RETRYCOUNT <count> JUSTID FORCE. */
    streamID id;
    streamDecodeID(rawid, &id);
    if (rioWriteBulkCount(r, '*', 12) == 0)
        return 0;
    if (rioWriteBulkString(r, "XCLAIM", 6) == 0)
        return 0;
    if (rioWriteBulkObject(r, key) == 0)
        return 0;
    if (rioWriteBulkString(r, groupname, groupname_len) == 0)
        return 0;
    if (rioWriteBulkString(r, consumer->name, sdslen(consumer->name)) == 0)
        return 0;
    if (rioWriteBulkString(r, "0", 1) == 0)
        return 0;
    if (rioWriteBulkStreamID(r, &id) == 0)
        return 0;
    if (rioWriteBulkString(r, "TIME", 4) == 0)
        return 0;
    if (rioWriteBulkLongLong(r, nack->delivery_time) == 0)
        return 0;
    if (rioWriteBulkString(r, "RETRYCOUNT", 10) == 0)
        return 0;
    if (rioWriteBulkLongLong(r, nack->delivery_count) == 0)
        return 0;
    if (rioWriteBulkString(r, "JUSTID", 6) == 0)
        return 0;
    if (rioWriteBulkString(r, "FORCE", 5) == 0)
        return 0;
    return 1;
}

/* Helper for rewriteStreamObject(): emit the XGROUP CREATECONSUMER is
 * needed in order to create consumers that do not have any pending entries.
 * All this in the context of the specified key and group. */
int rioWriteStreamEmptyConsumer(rio *r, robj *key, const char *groupname, size_t groupname_len, streamConsumer *consumer) {
    /* XGROUP CREATECONSUMER <key> <group> <consumer> */
    if (rioWriteBulkCount(r, '*', 5) == 0)
        return 0;
    if (rioWriteBulkString(r, "XGROUP", 6) == 0)
        return 0;
    if (rioWriteBulkString(r, "CREATECONSUMER", 14) == 0)
        return 0;
    if (rioWriteBulkObject(r, key) == 0)
        return 0;
    if (rioWriteBulkString(r, groupname, groupname_len) == 0)
        return 0;
    if (rioWriteBulkString(r, consumer->name, sdslen(consumer->name)) == 0)
        return 0;
    return 1;
}

/* Emit the commands needed to rebuild a stream object.
 * The function returns 0 on error, 1 on success. */
int rewriteStreamObject(rio *r, robj *key, robj *o) {
    stream *s = o->ptr;
    streamIterator si;
    streamIteratorStart(&si, s, NULL, NULL, 0);
    streamID id;
    int64_t numfields;

    if (s->length) {
        /* Reconstruct the stream data using XADD commands. */
        while (streamIteratorGetID(&si, &id, &numfields)) {
            /* Emit a two elements array for each item. The first is
             * the ID, the second is an array of field-value pairs. */

            /* Emit the XADD <key> <id> ...fields... command. */
            if (!rioWriteBulkCount(r, '*', 3 + numfields * 2) || !rioWriteBulkString(r, "XADD", 4) || !rioWriteBulkObject(r, key) || !rioWriteBulkStreamID(r, &id)) {
                streamIteratorStop(&si);
                return 0;
            }
            while (numfields--) {
                unsigned char *field, *value;
                int64_t field_len, value_len;
                streamIteratorGetField(&si, &field, &value, &field_len, &value_len);
                if (!rioWriteBulkString(r, (char *)field, field_len) || !rioWriteBulkString(r, (char *)value, value_len)) {
                    streamIteratorStop(&si);
                    return 0;
                }
            }
        }
    }
    else {
        /* Use the XADD MAXLEN 0 trick to generate an empty stream if
         * the key we are serializing is an empty string, which is possible
         * for the Stream type. */
        id.ms = 0;
        id.seq = 1;
        if (!rioWriteBulkCount(r, '*', 7) || !rioWriteBulkString(r, "XADD", 4) || !rioWriteBulkObject(r, key) || !rioWriteBulkString(r, "MAXLEN", 6) || !rioWriteBulkString(r, "0", 1) || !rioWriteBulkStreamID(r, &id) || !rioWriteBulkString(r, "x", 1) || !rioWriteBulkString(r, "y", 1)) {
            streamIteratorStop(&si);
            return 0;
        }
    }

    /* Append XSETID after XADD, make sure lastid is correct,
     * in case of XDEL lastid. */
    if (!rioWriteBulkCount(r, '*', 7) || !rioWriteBulkString(r, "XSETID", 6) || !rioWriteBulkObject(r, key) || !rioWriteBulkStreamID(r, &s->last_id) || !rioWriteBulkString(r, "ENTRIESADDED", 12) || !rioWriteBulkLongLong(r, s->entries_added) || !rioWriteBulkString(r, "MAXDELETEDID", 12) || !rioWriteBulkStreamID(r, &s->max_deleted_entry_id)) {
        streamIteratorStop(&si);
        return 0;
    }

    /* Create all the stream consumer groups. */
    if (s->cgroups) {
        raxIterator ri;
        raxStart(&ri, s->cgroups);
        raxSeek(&ri, "^", NULL, 0);
        while (raxNext(&ri)) {
            streamCG *group = ri.data;
            /* Emit the XGROUP CREATE in order to create the group. */
            if (!rioWriteBulkCount(r, '*', 7) || !rioWriteBulkString(r, "XGROUP", 6) || !rioWriteBulkString(r, "CREATE", 6) || !rioWriteBulkObject(r, key) || !rioWriteBulkString(r, (char *)ri.key, ri.key_len) || !rioWriteBulkStreamID(r, &group->last_id) || !rioWriteBulkString(r, "ENTRIESREAD", 11) || !rioWriteBulkLongLong(r, group->entries_read)) {
                raxStop(&ri);
                streamIteratorStop(&si);
                return 0;
            }

            /* Generate XCLAIMs for each consumer that happens to
             * have pending entries. Empty consumers would be generated with
             * XGROUP CREATECONSUMER. */
            raxIterator ri_cons;
            raxStart(&ri_cons, group->consumers);
            raxSeek(&ri_cons, "^", NULL, 0);
            while (raxNext(&ri_cons)) {
                streamConsumer *consumer = ri_cons.data;
                /* If there are no pending entries, just emit XGROUP CREATECONSUMER */
                if (raxSize(consumer->pel) == 0) {
                    if (rioWriteStreamEmptyConsumer(r, key, (char *)ri.key, ri.key_len, consumer) == 0) {
                        raxStop(&ri_cons);
                        raxStop(&ri);
                        streamIteratorStop(&si);
                        return 0;
                    }
                    continue;
                }
                /* For the current consumer, iterate all the PEL entries
                 * to emit the XCLAIM protocol. */
                raxIterator ri_pel;
                raxStart(&ri_pel, consumer->pel);
                raxSeek(&ri_pel, "^", NULL, 0);
                while (raxNext(&ri_pel)) {
                    streamNACK *nack = ri_pel.data;
                    if (rioWriteStreamPendingEntry(r, key, (char *)ri.key, ri.key_len, consumer, ri_pel.key, nack) == 0) {
                        raxStop(&ri_pel);
                        raxStop(&ri_cons);
                        raxStop(&ri);
                        streamIteratorStop(&si);
                        return 0;
                    }
                }
                raxStop(&ri_pel);
            }
            raxStop(&ri_cons);
        }
        raxStop(&ri);
    }

    streamIteratorStop(&si);
    return 1;
}

/* Call the module type callback in order to rewrite a data type
 * that is exported by a module and is not handled by Redis itself.
 * The function returns 0 on error, 1 on success. */
int rewriteModuleObject(rio *r, robj *key, robj *o, int dbid) {
    RedisModuleIO io;
    moduleValue *mv = o->ptr;
    moduleType *mt = mv->type;
    moduleInitIOContext(io, mt, r, key, dbid);
    mt->aof_rewrite(&io, key, mv->value);
    if (io.ctx) {
        moduleFreeContext(io.ctx);
        zfree(io.ctx);
    }
    return io.error ? 0 : 1;
}

static int rewriteFunctions(rio *aof) {
    dict *functions = functionsLibGet();
    dictIterator *iter = dictGetIterator(functions);
    dictEntry *entry = NULL;
    while ((entry = dictNext(iter))) {
        functionLibInfo *li = dictGetVal(entry);
        if (rioWrite(aof, "*3\r\n", 4) == 0)
            goto werr;
        char function_load[] = "$8\r\nFUNCTION\r\n$4\r\nLOAD\r\n";
        if (rioWrite(aof, function_load, sizeof(function_load) - 1) == 0)
            goto werr;
        if (rioWriteBulkString(aof, li->code, sdslen(li->code)) == 0)
            goto werr;
    }
    dictReleaseIterator(iter);
    return 1;

werr:
    dictReleaseIterator(iter);
    return 0;
}

int rewriteAppendOnlyFileRio(rio *aof) {
    dictIterator *di = NULL;
    dictEntry *de;
    int j;
    long key_count = 0;
    long long updated_time = 0;

    if (server.aof_timestamp_enabled) {
        sds ts = genAofTimestampAnnotationIfNeeded(1);
        if (rioWrite(aof, ts, sdslen(ts)) == 0) {
            sdsfree(ts);
            goto werr;
        }
        sdsfree(ts);
    }

    if (rewriteFunctions(aof) == 0)
        goto werr;
    // 遍历所有数据库
    for (j = 0; j < server.dbnum; j++) {
        char selectcmd[] = "*2\r\n$6\r\nSELECT\r\n";
        redisDb *db = server.db + j;
        dict *d = db->dict; // 指向键空间
        if (dictSize(d) == 0)
            continue;
        di = dictGetSafeIterator(d); // 创建键空间迭代器

        /* SELECT the new DB */
        /// 首先写入 SELECT 命令,确保之后的数据会被插入到正确的数据库上
        if (rioWrite(aof, selectcmd, sizeof(selectcmd) - 1) == 0)
            goto werr;
        if (rioWriteBulkLongLong(aof, j) == 0)
            goto werr;

        /* Iterate this DB writing every entry */
        /// 遍历数据库所有键,并通过命令将它们的当前状态（值）记录到新 AOF 文件中
        while ((de = dictNext(di)) != NULL) {
            sds keystr;
            robj key, *o;
            long long expiretime;
            size_t aof_bytes_before_key = aof->processed_bytes;

            keystr = dictGetKey(de); // 取出键
            o = dictGetVal(de);      // 取出值
            initStaticStringObject(key, keystr);

            expiretime = getExpire(db, &key); // 取出过期时间

            /* Save the key and associated value */
            /// 根据值的类型,选择适当的命令来保存值
            if (o->type == OBJ_STRING) {
                /* Emit a SET command */
                char cmd[] = "*3\r\n$3\r\nSET\r\n";
                if (rioWrite(aof, cmd, sizeof(cmd) - 1) == 0)
                    goto werr;
                /* Key and value */
                if (rioWriteBulkObject(aof, &key) == 0)
                    goto werr;
                if (rioWriteBulkObject(aof, o) == 0)
                    goto werr;
            }
            else if (o->type == OBJ_LIST) {
                if (rewriteListObject(aof, &key, o) == 0)
                    goto werr;
            }
            else if (o->type == OBJ_SET) {
                if (rewriteSetObject(aof, &key, o) == 0)
                    goto werr;
            }
            else if (o->type == OBJ_ZSET) {
                if (rewriteSortedSetObject(aof, &key, o) == 0)
                    goto werr;
            }
            else if (o->type == OBJ_HASH) {
                if (rewriteHashObject(aof, &key, o) == 0)
                    goto werr;
            }
            else if (o->type == OBJ_STREAM) {
                if (rewriteStreamObject(aof, &key, o) == 0)
                    goto werr;
            }
            else if (o->type == OBJ_MODULE) {
                if (rewriteModuleObject(aof, &key, o, j) == 0)
                    goto werr;
            }
            else {
                serverPanic("Unknown object type");
            }

            /* In fork child process, we can try to release memory back to the
             * OS and possibly avoid or decrease COW. We guve the dismiss
             * mechanism a hint about an estimated size of the object we stored. */
            size_t dump_size = aof->processed_bytes - aof_bytes_before_key;
            if (server.in_fork_child)
                dismissObject(o, dump_size);

            /* Save the expire time */
            //        * 保存键的过期时间

            if (expiretime != -1) {
                char cmd[] = "*3\r\n$9\r\nPEXPIREAT\r\n";
                //                            // 写入 PEXPIREAT expiretime 命令
                if (rioWrite(aof, cmd, sizeof(cmd) - 1) == 0)
                    goto werr;
                if (rioWriteBulkObject(aof, &key) == 0)
                    goto werr;
                if (rioWriteBulkLongLong(aof, expiretime) == 0)
                    goto werr;
            }

            /* 大约每1秒更新一次信息.为了避免每次迭代都调用mstime(),我们将每1024个键检查一次差异 */
            if ((key_count++ & 1023) == 0) {
                long long now = mstime();
                if (now - updated_time >= 1000) {
                    sendChildInfo(CHILD_INFO_TYPE_CURRENT_INFO, key_count, "AOF rewrite");
                    updated_time = now;
                }
            }

            /* Delay before next key if required (for testing) */
            if (server.rdb_key_save_delay)
                debugDelay(server.rdb_key_save_delay);
        }
        // 释放迭代器
        dictReleaseIterator(di);
        di = NULL;
    }
    return C_OK;

werr:
    if (di)
        dictReleaseIterator(di);
    return C_ERR;
}

/*
 * 编写一系列命令,能够完全将数据集重建为“filename”.REWRITEAOF和BGREWRITEAOF都使用.
 * 这个函数被 REWRITEAOF 和 BGREWRITEAOF 两个命令调用.
 * （REWRITEAOF 似乎已经是一个废弃的命令）
 * 为了最小化重建数据集所需执行的命令数量,
 * Redis 会尽可能地使用接受可变参数数量的命令,比如 RPUSH 、SADD 和 ZADD 等.
 * 不过单个命令每次处理的元素数量不能超过 REDIS_AOF_REWRITE_ITEMS_PER_CMD .
 */
int rewriteAppendOnlyFile(char *filename) {
    rio aof;
    FILE *fp = NULL;
    char tmpfile[256];
    /*
     * 创建临时文件
     * 注意这里创建的文件名和 rewriteAppendOnlyFileBackground() 创建的文件名稍有不同
     */
    snprintf(tmpfile, 256, "temp-rewriteaof-%d.aof", (int)getpid());
    fp = fopen(tmpfile, "w");
    if (!fp) {
        serverLog(LL_WARNING, "打开AOF重写的临时文件 rewriteAppendOnlyFile(): %s", strerror(errno));
        return C_ERR;
    }
    // 初始化文件 io
    rioInitWithFile(&aof, fp);
    // 设置每写入 REDIS_AOF_AUTOSYNC_BYTES 字节
    // 就执行一次 FSYNC
    // 防止缓存中积累太多命令内容,造成 I/O 阻塞时间过长
    if (server.aof_rewrite_incremental_fsync)
        rioSetAutoSync(&aof, REDIS_AUTOSYNC_BYTES);

    startSaving(RDBFLAGS_AOF_PREAMBLE);

    if (server.aof_use_rdb_preamble) {
        int error;
        if (rdbSaveRio(SLAVE_REQ_NONE, &aof, &error, RDBFLAGS_AOF_PREAMBLE, NULL) == C_ERR) { // rewriteAppendOnlyFile->rdbSaveRio
            errno = error;
            goto werr;
        }
    }
    else {
        if (rewriteAppendOnlyFileRio(&aof) == C_ERR)
            goto werr;
    }

    // 冲洗并关闭新 AOF 文件
    if (fflush(fp))
        goto werr;
    if (fsync(fileno(fp)))
        goto werr;
    if (fclose(fp)) {
        fp = NULL;
        goto werr;
    }
    fp = NULL;

    // 原子地改名,用重写后的新 AOF 文件覆盖旧 AOF 文件
    if (rename(tmpfile, filename) == -1) {
        serverLog(LL_WARNING, "移动临时AOF文件出现错误: %s", strerror(errno));
        unlink(tmpfile);
        stopSaving(0);
        return C_ERR;
    }
    serverLog(LL_NOTICE, "同步AOF文件");
    stopSaving(1);

    return C_OK;

werr:
    serverLog(LL_WARNING, "写AOF文件出现错误: %s", strerror(errno));
    if (fp)
        fclose(fp);
    unlink(tmpfile);
    stopSaving(0);
    return C_ERR;
}

/*
 * AOF 重写的四个触发时机
 *    时机一：bgrewriteaof 命令被执行.
 *    时机二：主从复制完成 RDB 文件解析和加载（无论是否成功）.
 *    时机三：AOF 重写被设置为待调度执行.
 *    时机四：AOF 被启用,同时 AOF 文件的大小比例超出阈值,以及 AOF 文件的大小绝对值超出阈值.
 */
int rewriteAppendOnlyFileBackground(void) {
    pid_t childpid;
    // 已经有进程在进行 AOF 重写了
    if (hasActiveChildProcess())
        return C_ERR;

    if (dirCreateIfMissing(server.aof_dirname) == -1) { // 创建目录
        serverLog(LL_WARNING, "不能打开或创建只能追加的目录 %s: %s", server.aof_dirname, strerror(errno));
        return C_ERR;
    }

    // 我们将aof_selected_db设置为-1,以便强制下一次调用feedAppendOnlyFile()发出SELECT命令.
    server.aof_selected_db = -1;
    flushAppendOnlyFile(1);
    if (openNewIncrAofForAppend() != C_OK) {
        return C_ERR; // 创建了一个新的AOF文件,并更新了对应的索引信息
    }
    server.stat_aof_rewrites++;
    //    childpid = 0;
    childpid = redisFork(CHILD_TYPE_AOF);
    if (childpid == 0) { // 创建子进程
        char tmpfile[256];
        // 为进程设置名字,方便记认
        redisSetProcTitle("redis-aof-rewrite");
        redisSetCpuAffinity(server.aof_rewrite_cpulist);
        // 创建临时文件,并进行 AOF 重写
        snprintf(tmpfile, 256, "temp-rewriteaof-bg-%d.aof", (int)getpid());
        // 子进程调用rewriteAppendOnlyFile进行AOF重写
        if (rewriteAppendOnlyFile(tmpfile) == C_OK) {                      // 主要逻辑
            sendChildCowInfo(CHILD_INFO_TYPE_AOF_COW_SIZE, "AOF rewrite"); // 给父进程发送消息
            exitFromChild(0);                                              // 发送重写成功信号
        }
        else {
            exitFromChild(1); // 发送重写失败信号
        }
    }
    else {
        /* Parent */
        if (childpid == -1) {
            server.aof_lastbgrewrite_status = C_ERR;
            serverLog(LL_WARNING, "后台AOF重写进程FORK失败: fork: %s", strerror(errno));
            return C_ERR;
        }
        serverLog(LL_NOTICE, "后台进程 %ld 开始AOF 重写", (long)childpid);
        // 记录 AOF 重写的信息
        server.aof_rewrite_scheduled = 0;
        server.aof_rewrite_time_start = time(NULL);
        return C_OK;
    }
    return C_OK; /* unreached */
}

void bgrewriteaofCommand(client *c) {
    // 不能重复运行 BGREWRITEAOF
    if (server.child_type == CHILD_TYPE_AOF) {
        addReplyError(c, "已经有一个AOF重写进程在工作");
    }
    else if (hasActiveChildProcess() || server.in_exec) {
        // 有RDB子进程,将AOF重写设置为待调度运行
        server.aof_rewrite_scheduled = 1;
        // 当手动触发AOF RW时,我们重置计数,以便它可以立即执行.
        server.stat_aofrw_consecutive_failures = 0;
        addReplyStatus(c, "后台AOF重写开始调度");
    }
    else if (rewriteAppendOnlyFileBackground() == C_OK) { // 实际执行AOF重写
        addReplyStatus(c, "后台AOF重写已开启");
    }
    else {
        addReplyError(c, "执行AOF后台重写失败,请检查日志");
    }
}

// 删除 AOF 重写所产生的临时文件
void aofRemoveTempFile(pid_t childpid) {
    char tmpfile[256];

    snprintf(tmpfile, 256, "temp-rewriteaof-bg-%d.aof", (int)childpid);
    bg_unlink(tmpfile);

    snprintf(tmpfile, 256, "temp-rewriteaof-%d.aof", (int)childpid);
    bg_unlink(tmpfile);
}

// 获取AOF文件的大小.
// status参数是一个可选输出参数,用AOF_status值之一填充.
off_t getAppendOnlyFileSize(sds filename, int *status) {
    struct redis_stat sb;
    off_t size;
    mstime_t latency;

    sds aof_filepath = makePath(server.aof_dirname, filename);
    latencyStartMonitor(latency);
    if (redis_stat(aof_filepath, &sb) == -1) {
        if (status)
            *status = errno == ENOENT ? AOF_NOT_EXIST : AOF_OPEN_ERR;
        serverLog(LL_WARNING, "无法获取AOF文件%s的长度.统计:%s", filename, strerror(errno));
        size = 0;
    }
    else {
        if (status)
            *status = AOF_OK;
        size = sb.st_size; // 文件大小
    }
    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("aof-fstat", latency);
    sdsfree(aof_filepath);
    return size;
}

/* Get size of all AOF files referred by the manifest (excluding history).
 * The status argument is an output argument to be filled with
 * one of the AOF_ status values. */
off_t getBaseAndIncrAppendOnlyFilesSize(aofManifest *am, int *status) {
    off_t size = 0;
    listNode *ln;
    listIter li;

    if (am->base_aof_info) {
        serverAssert(am->base_aof_info->file_type == AOF_FILE_TYPE_BASE);

        size += getAppendOnlyFileSize(am->base_aof_info->file_name, status);
        if (*status != AOF_OK)
            return 0;
    }

    listRewind(am->incr_aof_list, &li);
    while ((ln = listNext(&li)) != NULL) {
        aofInfo *ai = (aofInfo *)ln->value;
        serverAssert(ai->file_type == AOF_FILE_TYPE_INCR);
        size += getAppendOnlyFileSize(ai->file_name, status);
        if (*status != AOF_OK)
            return 0;
    }

    return size;
}

int getBaseAndIncrAppendOnlyFilesNum(aofManifest *am) {
    int num = 0;
    if (am->base_aof_info)
        num++;
    if (am->incr_aof_list)
        num += listLength(am->incr_aof_list);
    return num;
}

/*  当子线程完成 AOF 重写时,父进程调用这个函数.
 */
void backgroundRewriteDoneHandler(int exitcode, int bysignal) {
    if (!bysignal && exitcode == 0) {
        char tmpfile[256];
        long long now = ustime();
        sds new_base_filepath = NULL;
        sds new_incr_filepath = NULL;
        aofManifest *temp_am;
        mstime_t latency;

        serverLog(LL_NOTICE, "AOF 后台重写进程成功终止");
        // 创建临时文件,并进行 AOF 重写
        snprintf(tmpfile, 256, "temp-rewriteaof-bg-%d.aof", (int)server.child_pid);
        serverAssert(server.aof_manifest != NULL);
        temp_am = aofManifestDup(server.aof_manifest);

        // 获取一个新的BASE文件名,并将之前的(如果有的话)标记为HISTORY类型.
        sds new_base_filename = getNewBaseFileNameAndMarkPreAsHistory(temp_am);
        serverAssert(new_base_filename != NULL);
        new_base_filepath = makePath(server.aof_dirname, new_base_filename);

        // 将临时aof文件重命名为“new_base_filename”.
        latencyStartMonitor(latency);
        if (rename(tmpfile, new_base_filepath) == -1) {
            serverLog(LL_WARNING, "试图将临时AOF文件%s重命名为 %s: 产生错误 %s", tmpfile, new_base_filepath, strerror(errno));
            aofManifestFree(temp_am);
            sdsfree(new_base_filepath);
            goto cleanup;
        }
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("aof-rename", latency);

        // 重命名临时增量 AOF文件
        if (server.aof_state == AOF_WAIT_REWRITE) {
            // 获取临时aof增量文件名称
            sds temp_incr_aof_name = getTempIncrAofName();
            sds temp_incr_filepath = makePath(server.aof_dirname, temp_incr_aof_name);
            sdsfree(temp_incr_aof_name);
            // 获取临时aof增量文件名称
            sds new_incr_filename = getNewIncrAofName(temp_am);
            new_incr_filepath = makePath(server.aof_dirname, new_incr_filename);
            latencyStartMonitor(latency);
            if (rename(temp_incr_filepath, new_incr_filepath) == -1) {
                serverLog(LL_WARNING, "试图将临时AOF文件%s重命名为 %s: 产生错误 %s", temp_incr_filepath, new_incr_filepath, strerror(errno));
                bg_unlink(new_base_filepath);
                sdsfree(new_base_filepath);
                aofManifestFree(temp_am);
                sdsfree(temp_incr_filepath);
                sdsfree(new_incr_filepath);
                goto cleanup;
            }
            latencyEndMonitor(latency);
            latencyAddSampleIfNeeded("aof-rename", latency);
            sdsfree(temp_incr_filepath);
        }

        // 将'incr_aof_list'中的AOF文件类型从AOF_FILE_TYPE_INCR更改为AOF_FILE_TYPE_HIST,并将其移动到'history_aof_list'.
        markRewrittenIncrAofAsHistory(temp_am);

        // 持久化我们的修改
        if (persistAofManifest(temp_am) == C_ERR) {
            bg_unlink(new_base_filepath);
            aofManifestFree(temp_am);
            sdsfree(new_base_filepath);
            if (new_incr_filepath) {
                bg_unlink(new_incr_filepath);
                sdsfree(new_incr_filepath);
            }
            goto cleanup;
        }
        sdsfree(new_base_filepath);
        if (new_incr_filepath)
            sdsfree(new_incr_filepath);

        /* We can safely let `server.aof_manifest` point to 'temp_am' and free the previous one. */
        aofManifestFreeAndUpdate(temp_am);

        if (server.aof_fd != -1) {
            // 强制引发 SELECT
            server.aof_selected_db = -1; /* Make sure SELECT is re-issued */
            server.aof_current_size = getAppendOnlyFileSize(new_base_filename, NULL) + server.aof_last_incr_size;
            server.aof_rewrite_base_size = server.aof_current_size; // 记录前一次重写时的大小
            server.aof_fsync_offset = server.aof_current_size;
            server.aof_last_fsync = server.unixtime;
        }

        aofDelHistoryFiles(); // 删除历史AOF文件

        server.aof_lastbgrewrite_status = C_OK;
        server.stat_aofrw_consecutive_failures = 0;

        serverLog(LL_NOTICE, "后台 AOF重写成功");
        // 如果是第一次创建 AOF 文件,那么更新 AOF 状态
        if (server.aof_state == AOF_WAIT_REWRITE)
            server.aof_state = AOF_ON;

        serverLog(LL_VERBOSE, "后台AOF重写信号处理程序 花费 %lldus", ustime() - now);
    }
    else if (!bysignal && exitcode != 0) { // BGREWRITEAOF 重写出错
        server.aof_lastbgrewrite_status = C_ERR;
        server.stat_aofrw_consecutive_failures++;
        serverLog(LL_WARNING, "后台AOF重写 错误终止");
    }
    else { // 未知错误
        /* SIGUSR1 is whitelisted, so we have a way to kill a child without
         * triggering an error condition. */
        if (bysignal != SIGUSR1) {
            server.aof_lastbgrewrite_status = C_ERR;
            server.stat_aofrw_consecutive_failures++;
        }

        serverLog(LL_WARNING, "Background AOF rewrite terminated by signal %d", bysignal);
    }

cleanup:
    aofRemoveTempFile(server.child_pid); // 移除临时文件

    // 清理AOF缓冲 清除增量AOF文件
    if (server.aof_state == AOF_WAIT_REWRITE) {
        sdsfree(server.aof_buf);
        server.aof_buf = sdsempty();
        aofDelTempIncrAofFile();
    }
    server.aof_rewrite_time_last = time(NULL) - server.aof_rewrite_time_start;
    server.aof_rewrite_time_start = -1;
    // 如果我们正在等待它打开AOF,计划一个新的重写.
    if (server.aof_state == AOF_WAIT_REWRITE)
        server.aof_rewrite_scheduled = 1;
}
