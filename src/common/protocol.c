#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/*线上消息 = 12字节头部(type、request_id、length) + length字节正文。
 * TCP没有消息边界，必须累计实际收发字节数；Message中的data指针不在线上传输。*/
ssize_t send_all(int fd, const void *buffer, size_t length)
{
    const char *bytes = buffer; //指向要发送的字节数据
    size_t offset = 0; //成功发送的字节数
    while (offset < length) { //循环提交剩余字节，全部成功或遇到错误才结束，不代表对方应用已处理
        ssize_t count = send(fd, bytes + offset, length - offset, MSG_NOSIGNAL); //最多提交剩余字节，抑制本次SIGPIPE
        if (count < 0 && errno == EINTR) //信号中断时原位置重试，其他发送错误交给调用者
            continue;
        if (count <= 0) {
            if (count == 0)
                errno = EPIPE; //本次没有发送进展，主动按失败处理，避免无限循环
            return -1;
        }
        offset += (size_t)count; // 累计已经发送的字节
    }
    return (ssize_t)offset; //返回成功发送的字节数
}

ssize_t recv_all(int fd, void *buffer, size_t length)
{
    char *bytes = buffer;//指向接收缓冲区的字节数据
    size_t offset = 0;//成功接收的字节数
    while (offset < length) {//循环收满指定字节，或在错误、EOF时提前返回
        ssize_t count = recv(fd, bytes + offset, length - offset, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return -1;
        if (count == 0) { //未读到数据就EOF返回0，已经读了一部分则报告协议截断
            if (offset == 0)
                return 0;
            errno = EPROTO; //截断的头部/主体是一个错误
            return -1;
        }
        offset += (size_t)count;// 累计已经接收的字节
    }
    return (ssize_t)offset;//返回成功接收的字节数
}

int send_message(int fd, uint32_t type, uint32_t request_id,
                 const void *data, uint32_t length)
{
    if (type < MSG_HELLO || type > MSG_QUIT || length > MAX_MESSAGE_BYTES ||
        (length != 0 && data == NULL)) {
        errno = EINVAL;
        return -1;
    }//消息类型不合法，消息长度超过最大值，或者数据为空但长度不为0 则报错

    uint32_t header[3] = {htonl(type), htonl(request_id), htonl(length)}; //三个32位整数转大端序，共12字节，不包含data指针
    if (send_all(fd, header, sizeof(header)) < 0)
        return -1;
    return length == 0 || send_all(fd, data, length) >= 0 ? 0 : -1;
}

int recv_message(int fd, Message *message)
{
    uint32_t header[3]; //直接读接下来的12字节，依靠上一条消息已读完整来维持边界
    memset(message, 0, sizeof(*message)); //先清空输出结构体，避免失败时残留未初始化的指针和字段
    ssize_t count = recv_all(fd, header, sizeof(header)); //接收消息头部
    if (count <= 0)
        return (int)count;

    /*接收到的消息头部放到消息结构中*/
    message->type = ntohl(header[0]);
    message->request_id = ntohl(header[1]);
    message->length = ntohl(header[2]);

    if (message->type < MSG_HELLO || message->type > MSG_QUIT ||
        message->length > MAX_MESSAGE_BYTES) {
        errno = EPROTO;
        return -1; //判断消息类型是否合法，消息长度是否超过最大值
    }
    message->data = malloc((size_t)message->length + 1); //为消息主体分配内存空间，+1是为了给字符串末尾添加'\0'，以便后续处理
    if (message->data == NULL)//内存分配失败，返回错误
        return -1;
    if (message->length != 0) {
        ssize_t body_count = recv_all(fd, message->data, message->length);//接收消息主体
        if (body_count != (ssize_t)message->length) { //成功接收的字节数与消息长度不一致，说明接收失败
            int saved_errno = body_count == 0 ? EPROTO : errno;
            //如果正文接收失败，保存错误码，0代表对方关闭了链接，表示协议错误，其他错误码则用errno的信息来表示
            free_message(message); //释放消息结构体中的数据，避免内存泄漏
            errno = saved_errno;
            return -1;
        }
    }
    message->data[message->length] = '\0'; //在消息主体的末尾添加'\0'，以便后续处理字符串时不会出现越界访问
    return 1; //成功时保留正文交给调用者处理后释放，接收失败的分支自行清理
}

void free_message(Message *message) //释放消息结构体中的数据
{
    free(message->data);
    message->data = NULL; //清除已释放地址，调用者不应再使用原正文
}
