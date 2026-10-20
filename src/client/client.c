#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/*终端主线：main连接并发送问题 -> receive_answer循环收帧 -> CHUNK立即打印并刷新。
 * 不加载模型、不保存历史；同一连接一次发一个请求，完成后再读下一行。*/
//接收一轮响应；BEGIN后接收CHUNK，END或ERROR结束本轮
static int receive_answer(int fd, uint32_t request_id)
{
    int started = 0; //本轮是否收到BEGIN，用于检查片段和结束帧的顺序
    for (;;) {
        Message message;
        int received = recv_message(fd, &message);
        if (received <= 0) {
            fprintf(stderr, "\n服务器连接已关闭或消息接收失败。\n");
            return -1;
        }
        if (message.request_id != request_id) { //响应必须属于当前请求，不能混入其他轮次
            free_message(&message);
            fprintf(stderr, "\n服务器返回了不匹配的请求编号。\n");
            return -1;
        }
        int finished = 0; //本轮已结束，可以回到输入循环
        int failed = 0; //协议、连接或输出异常，需要结束客户端
        if (message.type == MSG_STREAM_BEGIN && !started) {
            printf("回答：");
            fflush(stdout);
            started = 1;
        } else if (message.type == MSG_STREAM_CHUNK && started) { //收到片段就按字节输出并刷新
            if (fwrite(message.data, 1, message.length, stdout) != message.length ||
                fflush(stdout) < 0)
                failed = 1;
        } else if (message.type == MSG_STREAM_END && started) {
            printf("\n");
            finished = 1;
        } else if (message.type == MSG_SESSION_INFO || message.type == MSG_CLEAR_HISTORY) {
            printf("%s", message.data);
            finished = 1;
        } else if (message.type == MSG_ERROR) { //有效错误帧只结束本轮，允许用户继续提问
            fprintf(stderr, "\n错误：%s", message.data);
            finished = 1;
        } else {
            fprintf(stderr, "\n服务器返回了不符合当前状态的消息。\n");
            failed = 1;
        }
        free_message(&message); //每个成功接收的帧都要释放正文，包含结束帧和错误提示
        if (failed)
            return -1;
        if (finished)
            return 0;
    }
}

int main(int argc, char **argv)
{
    //第一步：解析并检查IPv4地址和端口，未传参数时连接本机5000端口
    const char *ip = argc > 1 ? argv[1] : "127.0.0.1";
    const char *port_text = argc > 2 ? argv[2] : "5000";
    char *end;
    errno = 0;
    unsigned long port = strtoul(port_text, &end, 10); //end检查数字后是否混入其他字符
    struct sockaddr_in address = {.sin_family = AF_INET};
    if (argc > 3 || errno || end == port_text || *end != '\0' || port == 0 || port > 65535 ||
        inet_pton(AF_INET, ip, &address.sin_addr) != 1) { //验证IP并把二进制地址写入address，不解析域名
        fprintf(stderr, "用法：%s [服务器IPv4地址] [端口]\n", argv[0]);
        return 2;
    }
    address.sin_port = htons((uint16_t)port); //端口按16位网络字节序保存，address描述连接目标
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0); //第二步：创建TCP socket并连接C服务端
    struct timeval send_timeout = {5, 0};
    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout)) < 0 ||
        connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("连接服务器");
        if (fd >= 0)
            close(fd);
        return 1;
    }
    Message hello; //第三步：接收HELLO，取得会话编号、后端和日志位置
    if (recv_message(fd, &hello) != 1) {
        fprintf(stderr, "服务器未返回有效会话信息。\n");
        close(fd);
        return 1;
    }
    if (hello.type != MSG_HELLO || hello.request_id != 0) {
        fprintf(stderr, "服务器拒绝连接：%s\n", hello.data);
        free_message(&hello);
        close(fd);
        return 1;
    }
    printf("Linux 多客户端流式对话\n%s"
           "输入问题开始对话。/clear 清空上下文，/info 查看会话，/quit 退出。\n", hello.data);
    free_message(&hello);
    /*关闭stdin预读，避免getline已缓存下一行，但poll只看到底层描述符已无数据。*/
    setvbuf(stdin, NULL, _IONBF, 0);
    char *line = NULL;
    size_t capacity = 0;
    uint32_t request_id = 0; //第四步：等待用户输入，同时监控服务端断连；每次请求编号递增
    int exit_status = 0;
    for (;;) {
        printf("你：");
        fflush(stdout);
        struct pollfd fds[2] = { //等待用户输入时也监控服务端关闭，避免只卡在键盘读取
            {.fd = STDIN_FILENO, .events = POLLIN},
            {.fd = fd, .events = POLLIN | POLLRDHUP}
        };
        int ready;
        do {
            ready = poll(fds, 2, -1);
        } while (ready < 0 && errno == EINTR);
        if (ready < 0) {
            perror("等待输入");
            exit_status = 1;
            break;
        }
        if (fds[1].revents != 0) {
            fprintf(stderr, "\n服务器连接已关闭。\n");
            exit_status = 1;
            break;
        }
        if (!(fds[0].revents & (POLLIN | POLLHUP))) {
            fprintf(stderr, "输入不可用。\n");
            exit_status = 1;
            break;
        }
        ssize_t length = getline(&line, &capacity, stdin); //自动分配或扩充line，结束后统一free
        if (length < 0) {
            printf("\n");
            break;
        }
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) //去掉行尾回车换行
            line[--length] = '\0';
        if (length == 0)
            continue;
        if ((size_t)length > MAX_QUESTION_BYTES || memchr(line, '\0', (size_t)length) != NULL) {
            fprintf(stderr, "输入需为文本，最多4096字节。\n");
            continue;
        }
        if (request_id == UINT32_MAX) { //防止编号溢出回到0，服务端要求请求编号非0且递增
            fprintf(stderr, "请求编号已用完，请重新连接。\n");
            exit_status = 1;
            break;
        }
        ++request_id;
        uint32_t type = MSG_QUESTION; //普通文本作为问题，斜杠命令转成不带正文的控制消息
        if (strcmp(line, "/quit") == 0)
            type = MSG_QUIT;
        else if (strcmp(line, "/info") == 0)
            type = MSG_SESSION_INFO;
        else if (strcmp(line, "/clear") == 0)
            type = MSG_CLEAR_HISTORY;
        if (send_message(fd, type, request_id, type == MSG_QUESTION ? line : NULL,
                         type == MSG_QUESTION ? (uint32_t)length : 0) < 0) {
            perror("发送失败");
            exit_status = 1;
            break;
        }
        if (type == MSG_QUIT) //QUIT没有确认响应，发送后直接结束连接
            break;
        if (receive_answer(fd, request_id) < 0) { //第五步：等本轮结束后才读下一条输入
            exit_status = 1;
            break;
        }
    }
    free(line); //释放getline的动态缓冲区，关闭socket后服务端会结束对应会话
    close(fd);
    return exit_status;
}
