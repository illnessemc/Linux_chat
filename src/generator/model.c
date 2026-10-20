#include "model.h"
#include <curl/curl.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*模型主线：model_generate组装上下文 -> request发送本机HTTP -> receive_data拼接数据行
 * -> event_line提取文字 -> callback调用server.c中的forward_chunk，立即返回客户端。
 * 本文件只做模型接口适配，Qwen权重加载和GPU推理由独立的vLLM服务完成。*/
#define RESPONSE_LIMIT 131072
#define SSE_LINE_LIMIT 65536
typedef struct {
    CURL *easy;
    int client_fd, diagnostic_fd, streaming, bad, done, finished; //客户端连接、日志、流式开关、解析错误、DONE、结束原因
    generator_chunk_callback callback; //调用者传入的片段处理函数，在服务端是forward_chunk
    void *user_data; //原样交给callback，在服务端实际指向本轮StreamContext
    char line[SSE_LINE_LIMIT + 1]; //HTTP回调可能只给半行，在这里累积到换行后才解析
    size_t line_length;
    char body[RESPONSE_LIMIT + 1]; //保存/tokenize的普通JSON响应或HTTP错误正文
    size_t body_length;
} Response;

//单调时钟只计算经过时间，系统日期调整不会改变本轮超时预算
static double now(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) < 0)
        return -1;
    return value.tv_sec + value.tv_nsec / 1e9;
}

//主线程创建工作线程前初始化curl，所有工作线程结束后再统一清理
int model_init(void) { return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? 0 : -1; }
void model_cleanup(void) { curl_global_cleanup(); }

//解析一条完整的SSE数据行；返回0继续，-1表示格式错误或上层片段处理失败
static int event_line(Response *response)
{
    char *line = response->line;
    line[response->line_length] = '\0';
    if (response->line_length && line[response->line_length - 1] == '\r') //兼容CRLF结尾的数据行
        line[response->line_length - 1] = '\0';
    if (strncmp(line, "data:", 5)) //忽略空行、注释等非data行，按当前vLLM的单行JSON格式解析
        return 0;
    line += 5;
    while (*line == ' ')
        ++line;
    if (!strcmp(line, "[DONE]")) { //DONE只表示流结束，还需要有效finish_reason才能判为整轮成功
        response->done = 1;
        return 0;
    }
    if (response->done)
        return -1;
    struct json_object *event = json_tokener_parse(line), *choices, *choice, *delta, *content, *finish;
    int result = -1;
    if (!event || !json_object_object_get_ex(event, "choices", &choices) ||
        !json_object_is_type(choices, json_type_array))
        goto end;
    if (json_object_array_length(choices) == 0) {
        result = 0; /* An optional final usage event has no choices. */
        goto end;
    }
    if (json_object_array_length(choices) != 1)
        goto end;
    choice = json_object_array_get_idx(choices, 0); //当前接口只接受一个回答分支
    if (!json_object_object_get_ex(choice, "delta", &delta))
        goto end;
    if (json_object_object_get_ex(delta, "content", &content) && content != NULL) {
        if (!json_object_is_type(content, json_type_string) || response->finished)
            goto end;
        const char *text = json_object_get_string(content); //文字归当前JSON事件所有，回调须在事件释放前使用或复制
        size_t length = (size_t)json_object_get_string_len(content);
        //实际文字从callback传出，不通过model_generate的返回值传出；user_data把本轮上下文带回去
        if (memchr(text, '\0', length) != NULL ||
            (length && response->callback(text, length, response->user_data) != 0))
            goto end;
    }
    if (json_object_object_get_ex(choice, "finish_reason", &finish) && finish != NULL) {
        const char *reason = json_object_get_string(finish);
        if (!reason || (strcmp(reason, "stop") && strcmp(reason, "length")))
            goto end;
        response->finished = 1; //stop表示正常停止，length表示达到输出上限，两种都是有效结束原因
    }
    result = 0;
end:
    json_object_put(event);
    return result;
}

//curl收到一批HTTP字节就调用这里；这批数据不一定是一行JSON，也可能跨越中文字符边界
static size_t receive_data(char *data, size_t size, size_t count, void *user_data)
{
    Response *response = user_data;
    size_t length = size * count;
    long status = 0;
    curl_easy_getinfo(response->easy, CURLINFO_RESPONSE_CODE, &status);
    if (!response->streaming || status != 200) { //普通JSON和错误正文先完整收集，流式成功响应再逐行处理
        if (length > RESPONSE_LIMIT - response->body_length)
            goto fail;
        memcpy(response->body + response->body_length, data, length);
        response->body_length += length;
        response->body[response->body_length] = '\0';
        return length;
    }
    for (size_t i = 0; i < length; ++i) { //按字节拼接，遇到换行才交给event_line，避免解析半截JSON
        if (data[i] == '\n') {
            if (event_line(response) < 0)
                goto fail;
            response->line_length = 0;
        } else {
            if (response->line_length == SSE_LINE_LIMIT || data[i] == '\0')
                goto fail;
            response->line[response->line_length++] = data[i];
        }
    }
    return length; //curl写回调返回已接收字节数，表示这批数据处理成功
fail:
    response->bad = 1;
    return 0; //返回少于本批长度，通知curl中止这次HTTP传输
}

//执行一次POST请求，同时检查总期限和当前客户端断连；清理自己的HTTP连接，不停止共享vLLM
static int request(const ModelConfig *config, const char *path, struct json_object *payload,
                   const char *request_id, double deadline, Response *response)
{
    char url[256], id_header[128], error[CURL_ERROR_SIZE] = {0};
    if (snprintf(url, sizeof(url), "%s%s", config->base_url, path) >= (int)sizeof(url))
        return GENERATOR_FAILED;
    snprintf(id_header, sizeof(id_header), "X-Request-Id: %s", request_id); //关联C会话日志与vLLM请求日志
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    struct curl_slist *with_id = headers ? curl_slist_append(headers, id_header) : NULL;
    if (!with_id) {
        curl_slist_free_all(headers);
        return GENERATOR_FAILED;
    }
    headers = with_id;
    CURL *easy = curl_easy_init(); //保存本次HTTP请求的配置和接收回调
    CURLM *multi = curl_multi_init(); //逐步推进传输，期间仍能检查客户端断连和总期限
    int outcome = GENERATOR_FAILED, attached = 0;
    if (!easy || !multi || !headers)
        goto end;
    response->easy = easy;
    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, json_object_to_json_string_ext(payload, JSON_C_TO_STRING_PLAIN));
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, receive_data); //注册HTTP收包回调
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, response); //收到字节时，把本次Response地址传给回调
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L); //多线程环境下禁止curl使用相关信号机制
    curl_easy_setopt(easy, CURLOPT_NOPROXY, "*"); //本机模型请求绕过代理
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, error);
    if (curl_multi_add_handle(multi, easy) != CURLM_OK)
        goto end;
    attached = 1;
    //使用multi接口小步推进HTTP，等待模型输出期间仍能及时发现超时和客户端断连
    for (;;) {
        double current = now();
        if (current < 0)
            break;
        if (current >= deadline) {
            outcome = GENERATOR_TIMEOUT;
            break;
        }
        struct pollfd client = {.fd = response->client_fd, .events = POLLRDHUP};
        int ready = poll(&client, 1, 0);
        if (ready < 0 && errno != EINTR)
            break;
        if (client.revents & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) { //只取消当前会话的HTTP请求
            outcome = GENERATOR_CANCELLED;
            break;
        }
        int running;
        if (curl_multi_perform(multi, &running) != CURLM_OK) //推进传输，有数据到达时同步触发receive_data
            break;
        int remaining;
        CURLMsg *message;
        while ((message = curl_multi_info_read(multi, &remaining)) != NULL) {
            if (message->msg != CURLMSG_DONE)
                continue;
            long status = 0;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
            //传输成功且HTTP为200；流式模式还必须有DONE、有效结束原因并且没有残留半行
            if (message->data.result == CURLE_OK && status == 200 && !response->bad &&
                (!response->streaming || (response->done && response->finished && !response->line_length)))
                outcome = GENERATOR_OK;
            else
                dprintf(response->diagnostic_fd, "HTTP=%ld curl=%d error=%s body=%.2048s\n",
                        status, message->data.result, error, response->body);
            goto end;
        }
        if (!running || curl_multi_poll(multi, NULL, 0, 100, NULL) != CURLM_OK) //最多等待100毫秒，再检查取消和超时
            break;
    }
end:
    //取消和正常结束走相同清理路径，关闭本请求的HTTP连接并释放curl资源
    if (attached)
        curl_multi_remove_handle(multi, easy);
    curl_easy_cleanup(easy);
    if (multi)
        curl_multi_cleanup(multi);
    curl_slist_free_all(headers);
    dprintf(response->diagnostic_fd, "request=%s endpoint=%s outcome=%d\n", request_id, path, outcome);
    return outcome;
}

/*模型入口：返回值只表示生成状态，文字通过callback传出，裁剪数量通过*dropped写回。
 * history在此只读，跳过旧轮次仅影响本次输入；生成成功后由服务端提交历史。*/
int model_generate(const ModelConfig *config, const History *history, const char *question,
                   const char *request_id, int client_fd, int diagnostic_fd,
                   generator_chunk_callback callback, void *user_data, size_t *dropped)
{
    double start = now();
    if (start < 0)
        return GENERATOR_FAILED;
    double deadline = start + config->timeout_seconds; //token检查、必要的重试裁剪和生成共用一个总期限
    Response *response = calloc(1, sizeof(*response));
    if (!response)
        return GENERATOR_FAILED;
    int outcome = GENERATOR_FAILED;
    struct json_object *payload = NULL; //请求JSON根对象，释放它时一并释放内部消息数组
    //第一步：先带上全部历史检查token，超预算就多跳过最早一轮完整问答，再重新检查
    for (*dropped = 0; *dropped <= history->count; ++*dropped) {
        payload = json_object_new_object();
        struct json_object *messages = history_messages(history, *dropped, question); //system + 有效历史 + 当前问题
        if (!payload || !messages) {
            json_object_put(messages);
            goto end;
        }
        json_object_object_add(payload, "model", json_object_new_string(config->model));
        json_object_object_add(payload, "messages", messages);
        json_object_object_add(payload, "add_generation_prompt", json_object_new_boolean(1));
        *response = (Response){.client_fd = client_fd, .diagnostic_fd = diagnostic_fd}; //每次token检查前清空旧响应
        outcome = request(config, "/tokenize", payload, request_id, deadline, response); //由vLLM按实际聊天模板计算输入token
        if (outcome != GENERATOR_OK)
            goto end;
        struct json_object *tokens = json_tokener_parse(response->body), *count;
        int valid = tokens && json_object_object_get_ex(tokens, "count", &count) &&
                    json_object_is_type(count, json_type_int) && json_object_get_int64(count) > 0;
        int fits = valid && json_object_get_int64(count) <= (int64_t)(config->context_tokens - config->output_tokens); //为输出预留token空间
        json_object_put(tokens);
        if (!valid) {
            outcome = GENERATOR_FAILED;
            goto end;
        }
        if (fits)
            break;
        json_object_put(payload); //输入超预算，释放本次JSON，多跳过一轮历史后重新构造
        payload = NULL;
    }
    if (!payload) { //即使去掉全部历史，当前问题仍放不下，结束本轮且不改变原历史
        dprintf(diagnostic_fd, "问题超过模型上下文预算\n");
        outcome = GENERATOR_FAILED;
        goto end;
    }
    //第二步：预算通过，使用同一组messages请求流式生成
    json_object_object_add(payload, "max_tokens", json_object_new_int((int)config->output_tokens));
    json_object_object_add(payload, "temperature", json_object_new_double(0.2));
    json_object_object_add(payload, "stream", json_object_new_boolean(1));
    //第三步：绑定片段回调及其上下文；request运行期间就会把文字逐段传回服务端
    *response = (Response){.client_fd = client_fd, .diagnostic_fd = diagnostic_fd,
                           .streaming = 1, .callback = callback, .user_data = user_data};
    outcome = request(config, "/v1/chat/completions", payload, request_id, deadline, response);
end:
    json_object_put(payload);
    free(response);
    return outcome;
}
