#ifndef MODEL_H
#define MODEL_H
#include "stream.h"
#include "history.h"
//共享模型配置：本机URL、模型别名、上下文/输出token上限、总超时
typedef struct {
    const char *base_url; //本机vLLM服务地址，后面拼接token检查或生成接口路径
    const char *model; //服务中的模型别名，与启动脚本的served-model-name对应
    unsigned context_tokens, output_tokens, timeout_seconds; //输入输出总预算、输出上限、整轮总期限
} ModelConfig;
/*主线程先初始化再创建工作线程，等所有工作线程退出后统一清理。*/
int model_init(void);
void model_cleanup(void);
/*返回生成状态，文字由callback(data, length, user_data)逐段传出。
 * dropped写回需要裁掉的最早历史轮数；成功后调用者才提交新问答，失败保留原历史。*/
int model_generate(const ModelConfig *config, const History *history, const char *question,
                   const char *request_id, int client_fd, int diagnostic_fd,
                   generator_chunk_callback callback, void *user_data, size_t *dropped);
#endif
