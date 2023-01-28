
#include <stdio.h>
#include <signal.h>
#include <sys/time.h>

// 返回UNIX时间,单位为微秒  // 1 秒 = 1 000 000 微秒
long long ustime(void) {
    struct timeval tv;
    long long      ust;

    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec) * 1000000;
    ust += tv.tv_usec;
    return ust;
}

void signalHandler(int signo) {
    long long x;
    switch (signo) {
        case SIGALRM: {
            x = ustime();
            printf("Caught the SIGALRM signal!   %lld\n", x);
            break;
        }
    }
}

int main(int argc, char *argv[]) {
    signal(SIGALRM, signalHandler);

    struct itimerval new_value, old_value;
    new_value.it_value.tv_sec = 0;
    new_value.it_value.tv_usec = 0;
    new_value.it_interval.tv_sec = 0;
    new_value.it_interval.tv_usec = 0;
    setitimer(ITIMER_REAL, &new_value, &old_value);

    for (;;)
        ;

    return 0;
}