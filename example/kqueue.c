#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/event.h>
#include <errno.h>
#include <string.h>

// 为文件描述符打开对应状态位的工具函数
void turn_on_flags(int fd, int flags) {
    int current_flags;
    // 获取给定文件描述符现有的flag
    // 其中fcntl的第二个参数F_GETFL表示要获取fd的状态
    if ((current_flags = fcntl(fd, F_GETFL)) < 0)
        exit(1);

    // 施加新的状态位
    current_flags |= flags;
    if (fcntl(fd, F_SETFL, current_flags) < 0)
        exit(1);
}

// 错误退出的工具函数
int quit(const char *msg) {
    perror(msg);
    exit(1);
}

const static int FD_NUM = 2;         // 两个文件描述符,分别为标准输入与输出
const static int BUFFER_SIZE = 1024; // 缓冲区大小

// 完全以IO复用的方式读入标准输入流数据,输出到标准输出流中
int main() {
    struct kevent changes[FD_NUM];
    struct kevent events[FD_NUM];

    // 创建一个kqueue
    int kq;
    if ((kq = kqueue()) == -1)
        quit("kqueue()");

    // 准备从标准输入流中读数据
    int stdin_fd = STDIN_FILENO;
    int stdout_fd = STDOUT_FILENO;

    // 设置为非阻塞
    turn_on_flags(stdin_fd, O_NONBLOCK);
    turn_on_flags(stdout_fd, O_NONBLOCK);

    // 注册监听事件
    int k = 0;
    EV_SET(&changes[k++], stdin_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, &stdin_fd);
    EV_SET(&changes[k++], stdout_fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, &stdout_fd);

    int nev, nread, nwrote = 0; // 发生事件的数量, 已读字节数, 已写字节数
    char buffer[BUFFER_SIZE];

    while (1) {
        nev = kevent(kq, changes, FD_NUM, events, FD_NUM, NULL); // 已经就绪的文件描述符数量
        if (nev <= 0)
            quit("kevent()");

        int i;
        for (i = 0; i < nev; i++) {
            struct kevent event = events[i];
            if (event.flags & EV_ERROR)
                quit("Event error");

            int ev_fd = *((int *)event.udata);

            // 输入流就绪 且 缓冲区还有空间能继续读
            if (ev_fd == stdin_fd && nread < BUFFER_SIZE) {
                int new_nread;
                if ((new_nread = read(ev_fd, buffer + nread, sizeof(buffer) - nread)) <= 0)
                    quit("read()"); // 由于可读事件已经发生,因此如果读出0个字节也是不正常的

                nread += new_nread; // 递增已读数据字节数
            }

            // 输出流就绪 且 缓冲区有内容可以写出
            if (ev_fd == stdout_fd && nread > 0) {
                if ((nwrote = write(stdout_fd, buffer, nread)) <= 0)
                    quit("write()");

                memmove(buffer, buffer + nwrote, nwrote); // 为了使实现的代码更简洁,这里把还没有写出去的数据往前移动
                nread -= nwrote;                          // 减去已经写出去的字节数
            }
        }
    }

    return 0;
}