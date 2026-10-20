#include "history.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*历史属于单个Session。发送模型请求时只读取历史，完整生成成功后才提交新问答。
 * 轮数、字节数和模型token预算分别限制上下文，裁剪时始终按完整的一问一答处理。*/
static void drop_first(History *history)
{
    //删除最早一轮并释放两个字符串，再把后面的轮次前移；memmove允许源和目标内存重叠
    history->bytes -= strlen(history->turns[0].question) + strlen(history->turns[0].answer);
    free(history->turns[0].question);
    free(history->turns[0].answer);
    --history->count;
    memmove(history->turns, history->turns + 1, history->count * sizeof(DialogueTurn));
}

void history_clear(History *history)
{
    //用于/clear和会话结束，只释放当前连接的历史，不删除已有日志
    while (history->count)
        drop_first(history);
}

//成功提交一轮问答；dropped是模型适配器根据token预算要求裁掉的最早轮数
int history_commit(History *history, size_t dropped, const char *question, const char *answer)
{
    size_t bytes = strlen(question) + strlen(answer); //按UTF-8字节计数，不是字符数或模型token数
    if (bytes > HISTORY_MAX_BYTES || dropped > history->count) {
        errno = EOVERFLOW;
        return -1;
    }
    /*先复制新问答，再修改旧历史；内存分配失败时保留原历史。
     * 调用者随后可以释放消息正文和临时回答缓冲区，历史保存的是自己的副本。*/
    char *q = strdup(question), *a = strdup(answer);
    if (q == NULL || a == NULL) {
        free(q);
        free(a);
        return -1;
    }
    while (dropped-- > 0) //先执行token预算要求的裁剪
        drop_first(history);
    while (history->count == HISTORY_MAX_TURNS || history->bytes + bytes > HISTORY_MAX_BYTES) //再满足8轮和16384字节限制
        drop_first(history);
    history->turns[history->count++] = (DialogueTurn){q, a}; //追加完整问答，更新轮数和字节数
    history->bytes += bytes;
    return 0;
}

//给JSON消息数组追加一个role/content对象，格式与本机vLLM聊天接口一致
static int add_message(struct json_object *array, const char *role, const char *content)
{
    struct json_object *message = json_object_new_object();
    struct json_object *r = json_object_new_string(role), *c = json_object_new_string(content);
    if (!message || !r || !c) {
        json_object_put(message);
        json_object_put(r);
        json_object_put(c);
        return -1;
    }
    //加入父对象后由父对象持有这些JSON值，最后只需释放顶层消息数组
    json_object_object_add(message, "role", r);
    json_object_object_add(message, "content", c);
    json_object_array_add(array, message);
    return 0;
}

//只构造模型输入，不修改历史；first表示从第几轮开始携带旧问答，返回的JSON由调用者释放
struct json_object *history_messages(const History *history, size_t first, const char *question)
{
    struct json_object *array = json_object_new_array();
    if (!array || add_message(array, "system",
        "你是一个中文助手。准确、简洁地回答用户的问题，使用本次对话中已有的信息。") < 0)
        goto fail;
    for (size_t i = first; i < history->count; ++i) //保持system -> user/assistant历史 -> 当前user问题的顺序
        if (add_message(array, "user", history->turns[i].question) < 0 ||
            add_message(array, "assistant", history->turns[i].answer) < 0)
            goto fail;
    if (add_message(array, "user", question) < 0)
        goto fail;
    return array;
fail:
    //任一消息构造失败时释放已追加的部分，避免返回半份上下文
    json_object_put(array);
    return NULL;
}
