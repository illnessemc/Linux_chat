#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define MAX_MESSAGE_BYTES 65536U // 最大消息字节数
#define MAX_QUESTION_BYTES 4096U // 最大问题字节数

typedef enum { //消息类型
    MSG_HELLO = 1, // 握手消息
    MSG_QUESTION, // 问题消息
    MSG_CLEAR_HISTORY, // 清除历史消息
    MSG_SESSION_INFO, // 会话信息消息
    MSG_STREAM_BEGIN, // 流开始消息
    MSG_STREAM_CHUNK, // 流分块消息
    MSG_STREAM_END, // 流结束消息
    MSG_ERROR, // 错误消息
    MSG_QUIT // 退出消息
} MessageType;

typedef struct {
    uint32_t type; //消息类型，业务层继续判断是否为允许的请求或响应
    uint32_t request_id; //客户端请求编号非0且递增，服务端响应沿用本轮编号
    uint32_t length; //消息头由三部分组成，类型，请求ID，长度，共占用 12字节
    char *data; //存储实际正文地址；线上正文长length字节，接收时本地额外补一个'\0'
} Message; // 消息结构体

ssize_t send_all(int fd, const void *buffer, size_t length); //成功返回提交的字节数，错误返回-1
/*尚未读到数据就EOF返回0；读了一部分后截断返回-1；成功返回指定字节数。*/
ssize_t recv_all(int fd, void *buffer, size_t length); // 实际的接收操作，循环接收至结束

//发送成功返回0，失败返回-1；正文长度为0时只发送头部
int send_message(int fd, uint32_t type, uint32_t request_id,
                 const void *data, uint32_t length);// 组织消息头部和主体，并发送消息
/*返回1代表有消息，0代表EOF，-1代表错误 */
int recv_message(int fd, Message *message); // 接收消息头部和主体，并组织成消息结构体

void free_message(Message *message); // 释放消息结构体中的数据

#endif
