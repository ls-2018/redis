// 修改进程名字
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stddef.h> /* NULL size_t */
#include <stdarg.h> /* va_list va_start va_end */
#include <stdlib.h> /* malloc(3) setenv(3) clearenv(3) setproctitle(3) getprogname(3) */
#include <stdio.h>  /* vsnprintf(3) snprintf(3) */

#include <string.h> /* strlen(3) strchr(3) strdup(3) memset(3) memcpy(3) */

#include <errno.h> /* errno program_invocation_name program_invocation_short_name */

#if !defined(HAVE_SETPROCTITLE)
#if (defined __NetBSD__ || defined __FreeBSD__ || defined __OpenBSD__ || defined __DragonFly__)
#    define HAVE_SETPROCTITLE 1
#else
#    define HAVE_SETPROCTITLE 0
#endif
#endif

#if !HAVE_SETPROCTITLE
#if (defined __linux || defined __APPLE__)

#    ifdef __GLIBC__
#        define HAVE_CLEARENV
#    endif

extern char **environ; // 外部 环境变量

static struct {
    /* original value */
    const char *arg0;

    /* title space available */
    char *base, *end;

    /* pointer to original nul character within base */
    char *nul;

    _Bool reset;
    int error;
} SPT;

#    ifndef SPT_MIN
#        define SPT_MIN(a, b) (((a) < (b)) ? (a) : (b))
#    endif

static inline size_t spt_min(size_t a, size_t b) {
    return SPT_MIN(a, b);
} /* spt_min() */

/*
 * For discussion on the portability of the various methods, see
 * http:// lists.freebsd.org/pipermail/freebsd-stable/2008-June/043136.html
 */
// 将全局变量environ置空
int spt_clearenv(void) {
#    ifdef HAVE_CLEARENV
    return clearenv();
#    else
    extern char **environ;
    static char **tmp;

    if (!(tmp = malloc(sizeof *tmp)))
        return errno;

    tmp[0] = NULL;
    environ = tmp;

    return 0;
#    endif
} /* spt_clearenv() */

// 环境变量拷贝
static int spt_copyenv(int envc, char *oldenv[]) {
    extern char **environ;
    char **envcopy = NULL;
    char *eq;
    int i, error;
    int envsize;

    if (environ != oldenv)
        return 0;

    // 在清除之前,将environ复制到envcopy中.浅层拷贝是足够了,因为clearenv()只清除了environ数组.
    envsize = (envc + 1) * sizeof(char *);
    envcopy = malloc(envsize); // 内存分配
    if (!envcopy)
        return ENOMEM;
    memcpy(envcopy, oldenv, envsize);

    /* 注意,clearenv()失败后的状态是未定义的,但我们只假设错误意味着它被保持不变.
     */
    if ((error = spt_clearenv())) {
        environ = oldenv;
        free(envcopy);
        return error;
    }

    /* 从envcopy设置environ */
    for (i = 0; envcopy[i]; i++) {
        if (!(eq = strchr(envcopy[i], '=')))
            continue;

        *eq = '\0';
        error = (0 != setenv(envcopy[i], eq + 1, 1)) ? errno : 0;
        *eq = '=';

        /* On error, do our best to restore state */
        if (error) {
#    ifdef HAVE_CLEARENV
            /* We don't assume it is safe to free environ, so we
             * may leak it. As clearenv() was shallow using envcopy
             * here is safe.
             */
            environ = envcopy;
#    else
            free(envcopy);
            free(environ); /* Safe to free, we have just alloc'd it */
            environ = oldenv;
#    endif
            return error;
        }
    }

    free(envcopy);
    return 0;
} /* spt_copyenv() */

// 将args里的变量拷贝一份,然后保存到argv
static int spt_copyargs(int argc, char *argv[]) {
    char *tmp;
    int i;

    for (i = 1; i < argc || (i >= argc && argv[i]); i++) {
        if (!argv[i])
            continue;

        if (!(tmp = strdup(argv[i])))
            return errno;

        argv[i] = tmp;
    }

    return 0;
}

/*
初始化并填充SPT,以允许将来调用setp_roctitle().
以允许将来调用setp_roctitle()基本上需要覆盖argv[0],我们试图确定什么是最大的连续块,从argv[0]开始,我们可以用于这个目的.
由于这个范围将覆盖argv和environ的部分或全部字符串,所以要对这两个数组进行深度复制.
 */
void spt_init(int argc, char *argv[]) { // 2  ~/redis-cn/src/redis-server redis.conf
    char **envp = environ;
    char *base;
    char *end;
    char *nul;
    char *tmp;
    int i, error, envc;
    // "~/Desktop/redis-cn/src/redis-server"
    if (!(base = argv[0]))
        return;
    printf("%s\n", argv[0]);
    printf("%s\n", base);
    printf("%lu\n", strlen(base)); // 48

    nul = &base[strlen(base)];
    printf("%s\n", nul); // ""
    end = nul + 1;       // redis.conf

    // 试图尽可能地扩展end,同时确保base和end之间的范围只分配给argv,或者紧跟argv的任何东西（估计是envp）.
    // argc 参数个数
    for (i = 0; i < argc || (i >= argc && argv[i]); i++) {
        if (!argv[i] || argv[i] < end) {
            printf("argv[i] %s\n", base);
            continue;
        }

        if (end >= argv[i] && end <= argv[i] + strlen(argv[i]))
            end = argv[i] + strlen(argv[i]) + 1;
    }

    // 如果envp数组不是argv的直接扩展,就明确地扫描它.
    // 遍历环境变量
    for (i = 0; envp[i]; i++) {
        if (envp[i] < end)
            continue;

        if (end >= envp[i] && end <= envp[i] + strlen(envp[i])) {
            end = envp[i] + strlen(envp[i]) + 1;
            printf("end--->: %s\n", end);
        }
    }
    envc = i; // 统计环境变量个数？

    /* We're going to deep copy argv[], but argv[0] will still point to
     * the old memory for the purpose of updating the title so we need
     * to keep the original value elsewhere.
     */
    if (!(SPT.arg0 = strdup(argv[0])))
        goto syerr;

#    if __linux__
    if (!(tmp = strdup(program_invocation_name)))
        goto syerr;

    program_invocation_name = tmp;

    if (!(tmp = strdup(program_invocation_short_name)))
        goto syerr;

    program_invocation_short_name = tmp;
#    elif __APPLE__
    printf("%s\n", strdup(getprogname())); // redis-server
    if (!(tmp = strdup(getprogname())))
        goto syerr;

    setprogname(tmp);
// setprogname和getprogname.两者分别用于设置和获取程序名称.
#    endif

    /* 现在对环境和argv[]进行全面深入的复制. */
    if ((error = spt_copyenv(envc, envp))) // (数量、拷贝后的环境变量) ---> 设置 environ
        goto error;

    if ((error = spt_copyargs(argc, argv)))
        goto error;

    SPT.nul = nul;
    SPT.base = base;
    SPT.end = end;

    return;
syerr:
    error = errno;
error:
    SPT.error = error;
} /* spt_init() */

#    ifndef SPT_MAXTITLE
#        define SPT_MAXTITLE 255
#    endif

// ok 设置进程名称
void setproctitle(const char *fmt, ...) {
    char buf[SPT_MAXTITLE + 1]; // 在argv传递[0]时使用buffer
    va_list ap;
    char *nul;
    int len, error;

    if (!SPT.base)
        return;

    if (fmt) {
        va_start(ap, fmt);
        len = vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
    }
    else {
        len = snprintf(buf, sizeof buf, "%s", SPT.arg0);
    }

    if (len <= 0) {
        error = errno;
        goto error;
    }

    if (!SPT.reset) {
        memset(SPT.base, 0, SPT.end - SPT.base);
        SPT.reset = 1;
    }
    else {
        memset(SPT.base, 0, spt_min(sizeof buf, SPT.end - SPT.base));
    }

    len = spt_min(len, spt_min(sizeof buf, SPT.end - SPT.base) - 1);
    memcpy(SPT.base, buf, len);
    nul = &SPT.base[len];

    if (nul < SPT.nul) {
        *SPT.nul = '.';
    }
    else if (nul == SPT.nul && &nul[1] < SPT.end) {
        *SPT.nul = ' ';
        *++nul = '\0';
    }

    return;
error:
    SPT.error = error;
} /* setproctitle() */

#endif /* __linux || __APPLE__ */
#endif /* !HAVE_SETPROCTITLE */

#ifdef SETPROCTITLE_TEST_MAIN
int main(int argc, char *argv[]) {
    spt_init(argc, argv);

    printf("SPT.arg0: [%p] '%s'\n", SPT.arg0, SPT.arg0);
    printf("SPT.base: [%p] '%s'\n", SPT.base, SPT.base);
    printf("SPT.end: [%p] (%d bytes after base)'\n", SPT.end, (int)(SPT.end - SPT.base));
    return 0;
}
#endif
