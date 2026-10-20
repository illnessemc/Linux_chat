#ifndef STREAM_H
#define STREAM_H

#include <stddef.h>

/* 模型流式生成的公共约定，由调用者提供片段回调。
 * 每取得一段文字就调用callback；0表示继续，非0表示停止。
 * user_data由调用者提供，回调时原样传回。 */
typedef int (*generator_chunk_callback)(const void *data, size_t length,
                                        void *user_data);

/* 生成状态的名称与数值是模型调用和错误处理的接口约定。 */
enum {
    GENERATOR_OK = 0,
    GENERATOR_CANCELLED = 1,
    GENERATOR_TIMEOUT = 2,
    GENERATOR_FAILED = -1
};

#endif
