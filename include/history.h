#ifndef HISTORY_H
#define HISTORY_H
#include <stddef.h>
#include <json-c/json.h>
//每个连接独立保存完整问答；轮数和字节上限之外，模型请求还检查实际token预算
#define HISTORY_MAX_TURNS 8
#define HISTORY_MAX_BYTES 16384
typedef struct { char *question, *answer; } DialogueTurn; //一轮完整问答，两个字符串由历史模块持有
typedef struct {
    DialogueTurn turns[HISTORY_MAX_TURNS];
    size_t count, bytes; //当前轮数、所有问答正文的总字节数
} History;
void history_clear(History *history); //释放本连接的历史文本，不删除日志
//先复制新问答，再裁剪旧轮次；成功后外部临时字符串可以释放
int history_commit(History *history, size_t dropped, const char *question, const char *answer);
//构造system、从first开始的历史、当前问题；只读历史，返回的JSON由调用者释放
struct json_object *history_messages(const History *history, size_t first, const char *question);
#endif
