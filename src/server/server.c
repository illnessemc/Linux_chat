#include "model.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/*核心业务阅读顺序：main接入连接 -> add_client分配会话和线程 -> serve_client接收并分派请求。
 * 提问主线：answer_question -> model_generate -> forward_chunk逐段转发。
 * 模型成功后由history_commit保存完整问答；取消或失败不提交本轮历史。
 * 区分三类数据：Message保存一条消息，Session保存一个连接，StreamContext保存一次生成的信息。*/
#define MAX_CLIENTS 32 //最大客户端连接数

typedef struct Server Server; //提前声明并起别名，让Session可以先保存Server指针，完整定义在后面
typedef struct Session { //会话结构体，保存每个客户端的连接信息和状态
    Server *server; //对话服务器的指针
    struct Session *next; //指向下一个会话的指针，旨在把会话组织成链表
    uint64_t id;//会话的唯一标识符
    int client_fd;//客户端套接字文件描述符
    FILE *log;//会话日志文件指针
    int diagnostic_fd;//诊断日志文件描述符
    char log_path[512];//会话日志文件路径
    History history;//会话历史记录，保存对话的上下文信息
} Session;

struct Server { //服务器结构体，保存服务器的全局状态和配置
    pthread_mutex_t sessions_mutex;//互斥锁，用于保护会话链表的访问
    pthread_cond_t sessions_done;//条件变量，用于等待所有会话完成
    pthread_mutex_t log_mutex;//互斥锁，用于保护日志文件的访问
    Session *sessions;//指向会话链表的头指针
    size_t active_count;//当前活动会话的数量
    FILE *log;//服务器日志文件指针
    char log_directory[256];//服务器日志目录路径
    const char *backend;//固定为vllm，供协议中的会话信息与日志使用
    ModelConfig model;//模型配置，保存模型的相关参数
    sem_t model_slots;//信号量，用于限制同时处理的模型请求数量
};

static volatile sig_atomic_t stop_requested; //信号处理函数写入、主循环读取的停止标记，初始为0
static int signal_write_fd = -1; //自管道写端，信号到来时写字节唤醒主线程的poll

//信号回调只设置标记并唤醒主线程，实际加锁、关闭连接等清理由主线程和工作线程完成
static void stop_signal(int number)
{
    (void)number; //SIGINT和SIGTERM走同一停止流程，不需要区分信号编号
    int saved_errno = errno; //信号可能打断其他操作，返回前要恢复它原来的错误码
    stop_requested = 1;
    char byte = 1;
    ssize_t written = write(signal_write_fd, &byte, 1);
    (void)written; //管道满时已有数据可唤醒poll；停止标记也已设置，不在回调中阻塞等待
    errno = saved_errno;
}

//写公共server.log；一条记录由多次输出组成，所以整条记录共用一把锁，避免线程间交错
static void log_event(Server *server, uint64_t id, const char *format, ...)
{
    pthread_mutex_lock(&server->log_mutex);
    fprintf(server->log, "time=%lld session=%" PRIu64 " ", (long long)time(NULL), id);
    va_list args; //接收format之后的可变参数，供vfprintf按格式输出
    va_start(args, format);
    vfprintf(server->log, format, args);
    va_end(args);
    fputc('\n', server->log);
    if (fflush(server->log) < 0)
        perror("server log");
    pthread_mutex_unlock(&server->log_mutex);
}

//以追加方式打开日志，返回stdio文件流；新文件权限0600，仅所属用户可读写
static FILE *open_log(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0)
        return NULL;
    FILE *file = fdopen(fd, "a"); //把已有描述符包装为FILE流，成功后由fclose负责关闭它
    if (file == NULL) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
    }
    return file;
}

static int send_text(Session *session, uint32_t type, uint32_t id, const char *text) //发送文本消息给客户端
{
    return send_message(session->client_fd, type, id, text, (uint32_t)strlen(text));
}

typedef struct {
    Session *session;//属于哪个会话的指针
    uint32_t request_id;//请求编号，用于标识当前生成回答的请求
    int send_failed;//标记发送消息是否失败，如果发送失败则为1，否则为0
    char *answer;//保存生成的回答内容的缓冲区指针
    size_t answer_length;//本轮已累积的回答字节数，模型成功后用完整回答提交历史
} StreamContext; //流上下文结构体，保存生成回答的相关信息，包括会话指针、请求编号、发送状态、回答内容和回答长度

/*片段回调：后端每拿到一段文字就调用这里，不需要等整个回答生成完。
 * data是当前文字片段，user_data是answer_question传入的&context。
 * 返回0表示继续；返回非0让后端停止本轮生成。*/
static int forward_chunk(const void *data, size_t length, void *user_data)
{
    StreamContext *context = user_data; //恢复本轮上下文，找到所属会话、请求编号和回答缓冲区
    //一边转发一边累积完整回答，成功后用完整问答提交历史
    if (context->answer) {
        if (length > MAX_MESSAGE_BYTES - context->answer_length) {
            errno = EOVERFLOW;
            return -1;
        }
        memcpy(context->answer + context->answer_length, data, length); //在已累积内容之后追加片段
        context->answer_length += length;
        context->answer[context->answer_length] = '\0';
    }
    //片段先写入当前连接的日志，即使本轮后来失败，已产生的文本仍有记录
    if (fwrite(data, 1, length, context->session->log) != length ||
        fflush(context->session->log) < 0) {
        errno = EIO;
        return -1;
    }
    //马上发送CHUNK；完整回答缓冲区只为保存历史，不会阻止即时发送
    int status = send_message(context->session->client_fd, MSG_STREAM_CHUNK,
                              context->request_id, data, (uint32_t)length);
    context->send_failed = status < 0; //若帧只发出一部分就失败，上层应关闭连接，不能再追加错误帧
    return status;
}

static int answer_question(Session *session, const Message *message) //处理提问消息的核心代码
{
    //第一步：业务检查；协议层只检查帧格式，这里继续检查问题内容
    int blank = 1;
    for (uint32_t i = 0; i < message->length; ++i) {
        if (!isspace((unsigned char)message->data[i])) {
            blank = 0;
            break;
        } //检查消息内容是否为空白，如果是空白则返回错误消息给客户端
    }
    if (blank || message->length > MAX_QUESTION_BYTES ||
        memchr(message->data, '\0', message->length) != NULL)
        return send_text(session, MSG_ERROR, message->request_id,
                         "问题需为非空文本，最多4096字节。\n"); //检查消息是否合法

    //第二步：记录问题日志，此时还没有提交对话历史，也没有把日志发给客户端
    if (fprintf(session->log, "\nrequest=%u QUESTION: %s\nANSWER:\n",
                message->request_id, message->data) < 0 || fflush(session->log) < 0)
        return -1;//将提问消息记录到会话日志中，如果写入失败则返回错误

    //第三步：模型任务最多同时两个，trywait没有可用额度就立即返回忙碌，不排队
    if (sem_trywait(&session->server->model_slots) < 0)//尝试取得一个额度，成功后可用额度减1
        return send_text(session, MSG_ERROR, message->request_id, "模型正在处理两个请求，请稍后重试。\n");
    //第四步：告诉客户端本轮输出开始；发送失败时归还刚取得的额度
    char backend_info[32];//缓冲区，用于存储后端模型信息
    snprintf(backend_info, sizeof(backend_info), "backend=%s", session->server->backend);
    if (send_text(session, MSG_STREAM_BEGIN, message->request_id, backend_info) < 0) {
        sem_post(&session->server->model_slots);
        return -1;
    }

    //第五步：准备本轮回答缓冲区和回调上下文，调用本机模型。
    StreamContext context = {.session = session, .request_id = message->request_id}; //本轮资料，其余成员初始化为0
    int outcome; //只保存生成结果码；文字通过forward_chunk回调传出
    context.answer = calloc(MAX_MESSAGE_BYTES + 1, 1);//为回答内容分配缓冲区，大小为最大消息字节数加1，并初始化为0
    char request[96];//缓冲区，用于存储请求的唯一标识符，格式为"server_<pid>_session_<session_id>_request_<request_id>"
    snprintf(request, sizeof(request), "server_%ld_session_%" PRIu64 "_request_%u",
             (long)getpid(), session->id, message->request_id);
    size_t dropped = 0; //输出参数：模型适配器写回需要裁掉的最早历史轮数，成功提交时才真正删除
    //把forward_chunk函数地址和&context交给后端，后端在生成过程中多次回调
    outcome = context.answer ?
        model_generate(&session->server->model, &session->history, message->data, request,
                       session->client_fd, session->diagnostic_fd, forward_chunk, &context, &dropped) :
        GENERATOR_FAILED;//缓冲区申请失败则将outcome设为失败，否则调用模型适配器取得结果码
    //第六步：完整生成成功且回答非空，才复制本轮问答进历史；失败或取消的半截回答不提交
    if (outcome == GENERATOR_OK && (context.answer_length == 0 ||
        history_commit(&session->history, dropped, message->data, context.answer) < 0))
        outcome = GENERATOR_FAILED; //如果生成成功但回答长度为0，或者提交历史记录失败，则将outcome设置为生成器失败
    free(context.answer);//history_commit已复制问答；释放本轮临时缓冲区，不会删除已保存的历史
    sem_post(&session->server->model_slots);//释放信号量，允许其他请求进入模型处理
    dprintf(session->diagnostic_fd, "request=%s history_turns=%zu history_bytes=%zu\n",
            request, session->history.count, session->history.bytes);
    //第七步：记录模型生成状态；每轮不创建C生成子进程。
    if (fprintf(session->log, "\nrequest=%u outcome=%d\n", message->request_id, outcome) < 0 ||
        fflush(session->log) < 0)
        return -1;
    log_event(session->server, session->id, "request=%u outcome=%d", message->request_id, outcome);
    //第八步：成功发END，失败发ERROR；函数返回值表示发送/连接处理结果，与outcome的生成结果不同
    if (outcome == GENERATOR_OK) //如果生成成功，则发送流结束消息给客户端，表示回答已完成
        return send_text(session, MSG_STREAM_END, message->request_id, "");
    if (outcome == GENERATOR_CANCELLED)//检测到当前客户端断连，直接让上层结束会话，不再发送取消提示
        return -1;
    if (outcome == GENERATOR_TIMEOUT)//如果生成超时，则发送错误消息给客户端，提示生成任务已取消
        return send_text(session, MSG_ERROR, message->request_id,
                         "文本生成超时，所属生成任务已取消。\n");
    if (context.send_failed) //片段可能只发出半个帧，直接关闭连接，避免追加错误帧造成协议错位
        return -1;
    return send_text(session, MSG_ERROR, message->request_id,
                     "文本生成失败，请检查本会话的生成诊断日志。\n"); //如果生成失败，则发送错误消息给客户端
}

/*一个连接对应一个工作线程；同一连接的请求按顺序处理，不同连接由各自线程并发处理。
 * 已建立TCP连接后才进入这里；connect在客户端，accept4在服务端main中。*/
static void *serve_client(void *argument) //核心函数，处理客户端请求，接收消息并生成回答
{
    Session *session = argument; //获取会话指针
    Server *server = session->server; //获取服务器指针
    char info[768]; //缓冲区，用于存储会话信息和日志路径
    snprintf(info, sizeof(info), "session=%" PRIu64 " backend=%s\nlog=%s\n",
             session->id, server->backend, session->log_path); //格式化会话信息和日志路径存入info缓冲区
    log_event(server, session->id, "connected log=%s", session->log_path);
    uint32_t last_request = 0; //记录上一个请求的编号，初始为0，确保请求编号递增且不为0
    int connected = send_text(session, MSG_HELLO, 0, info) == 0; //连接已建立，发送会话欢迎信息；成功后进入接收循环
    while (connected) { //建立连接后，循环接收客户端消息并处理
        Message message;
        int received = recv_message(session->client_fd, &message); //返回状态码；完整消息通过&message写回
        if (received <= 0) {
            if (received < 0) { //接收消息失败，记录错误日志并发送错误消息给客户端
                log_event(server, session->id, "receive error: %s", strerror(errno));
                (void)send_text(session, MSG_ERROR, 0, "连接消息损坏或未完整传输。\n");
            }
            break;
        }
        int status = 0; //记录请求处理的返回值；错误提示发成功也可以为0，让连接继续使用
        if (message.request_id == 0 || message.request_id <= last_request) {//验证消息的合法性，必须是递增且不为0的请求编号
            status = send_text(session, MSG_ERROR, message.request_id,
                               "请求编号必须递增且不为0。\n");
        } else { //处理合法的请求编号，更新last_request并根据消息类型进行相应处理
            last_request = message.request_id;
            if (message.type == MSG_QUESTION) {//如果消息类型是提问，则调用answer_question函数生成回答
                status = answer_question(session, &message);
            } else if (message.length != 0) {//如果消息类型不是提问，但消息长度不为0，则发送错误消息，表示控制消息不能包含正文
                status = send_text(session, MSG_ERROR, message.request_id,
                                   "控制消息不能包含正文。\n");
            } else if (message.type == MSG_SESSION_INFO) {//如果消息类型是请求会话信息，则发送会话信息和日志路径给客户端
                snprintf(info, sizeof(info), "session=%" PRIu64 " backend=%s\nlog=%s\nhistory_turns=%zu\n",
                         session->id, server->backend, session->log_path, session->history.count);
                status = send_text(session, MSG_SESSION_INFO, message.request_id, info);
            } else if (message.type == MSG_CLEAR_HISTORY) {//如果消息类型是清空历史记录，则调用history_clear函数清空会话的历史记录，并发送确认消息给客户端
                history_clear(&session->history);
                log_event(server, session->id, "history cleared");
                status = send_text(session, MSG_CLEAR_HISTORY, message.request_id, "已清空本会话上下文，日志保留。\n");
            } else if (message.type == MSG_QUIT) {//如果消息类型是退出，则设置connected为0，表示客户端请求断开连接
                connected = 0;
            } else {//如果消息类型不在已知范围内，则发送错误消息，提示客户端支持的消息类型
                status = send_text(session, MSG_ERROR, message.request_id,
                                   "支持提问、/clear、/info 和 /quit。\n");
            }
        }
        free_message(&message);//成功接收的正文在这里释放；接收失败则已提前break，由接收函数自行清理
        if (status < 0)//发送失败或生成取消等情况，需要结束当前连接处理
            break;
    }
    /*释放会话占用的资源，并且从链表中删除，并且在日志中记录*/
    log_event(server, session->id, "disconnected");//记录客户端断开连接的日志
    history_clear(&session->history);//清空会话的历史记录，释放相关资源
    fclose(session->log);//关闭会话日志文件，释放文件指针
    close(session->diagnostic_fd);//关闭诊断日志文件，释放文件描述符

    pthread_mutex_lock(&server->sessions_mutex); //锁定服务器的会话互斥锁，确保对会话链表的操作是线程安全的
    //主线程停止时也在同一把锁下遍历并shutdown，所以链表中的socket必须保持有效
    Session **link = &server->sessions; //指向链表中某个指针的位置，先指向头指针，再逐个指向next成员
    while (*link != session)
        link = &(*link)->next;
    *link = session->next;//从会话链表中删除当前会话，更新链表指针
    close(session->client_fd); //关闭客户端套接字文件描述符，释放网络资源
    --server->active_count;
    pthread_cond_signal(&server->sessions_done);//唤醒等待会话完成的线程，通知有会话已结束
    pthread_mutex_unlock(&server->sessions_mutex);//解锁服务器的会话互斥锁，允许其他线程访问会话链表
    free(session); //释放add_client申请的会话对象，各份历史和日志已在前面清理
    return NULL; //当前工作线程结束，其他客户端和服务器主线程继续运行
}

static int add_client(Server *server, int fd, uint64_t id, pthread_attr_t *attributes)//增加客户端连接，创建会话并启动线程处理客户端请求
{
    //第一步：检查连接数。连接最多32个，与模型生成最多两个任务是不同的限制
    pthread_mutex_lock(&server->sessions_mutex);
    int full = server->active_count >= MAX_CLIENTS;
    pthread_mutex_unlock(&server->sessions_mutex);
    if (full) {
        const char *text = "服务器连接数已达上限。\n";
        (void)send_message(fd, MSG_ERROR, 0, text, (uint32_t)strlen(text));
        close(fd);
        return 0;
    }
    //第二步：为这个连接分配独立Session，清零后历史为空；结束时由serve_client释放
    Session *session = calloc(1, sizeof(*session));
    if (session == NULL) {
        close(fd);
        return -1;
    }
    session->server = server;
    session->client_fd = fd;
    session->id = id;
    //第三步：每个连接各有问答日志和生成诊断日志，不混入其他连接的文本
    snprintf(session->log_path, sizeof(session->log_path),
             "%s/session_%" PRIu64 ".log", server->log_directory, id);
    session->log = open_log(session->log_path);
    char diagnostic_path[512];
    snprintf(diagnostic_path, sizeof(diagnostic_path),
             "%s/session_%" PRIu64 ".generator.log", server->log_directory, id);
    session->diagnostic_fd = open(diagnostic_path,
                                 O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (session->log == NULL || session->diagnostic_fd < 0) {
        if (session->log != NULL)
            fclose(session->log);
        if (session->diagnostic_fd >= 0)
            close(session->diagnostic_fd);
        close(fd);
        free(session);
        return -1;
    }
    //第四步：头插法加入活动链表，并创建线程；主线程只负责接入，工作线程负责当前连接
    pthread_mutex_lock(&server->sessions_mutex);
    session->next = server->sessions;
    server->sessions = session;
    ++server->active_count;
    pthread_t thread;
    int error = pthread_create(&thread, attributes, serve_client, session); //新线程从serve_client(session)开始执行
    if (error != 0) {
        //线程创建失败，必须撤回刚才的链表和计数修改，再释放已取得的资源
        server->sessions = session->next;
        --server->active_count;
    }
    pthread_mutex_unlock(&server->sessions_mutex);
    if (error != 0) {
        fclose(session->log);
        close(session->diagnostic_fd);
        close(fd);
        free(session);
        errno = error;
        return -1;
    }
    return 0;
}

//检查端口、超时等正整数参数；返回0成功、-1失败，转换结果通过number指针写回
static int number_argument(const char *text, unsigned limit, unsigned *number)
{
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10); //end检查是否有未转换字符，errno检查溢出等错误
    if (errno || end == text || *end != '\0' || value == 0 || value > limit)
        return -1;
    *number = (unsigned)value;
    return 0;
}

int main(int argc, char **argv)
{
    //第一步：解析本地模型参数，只允许回环HTTP地址和合法端口。
    unsigned port = 5000;
    unsigned timeout_seconds = 120;
    const char *model_url = argc > 2 ? argv[2] : "http://127.0.0.1:8000";
    if (argc > 5 || (argc > 1 && number_argument(argv[1], 65535, &port) < 0) ||
        (argc > 3 && number_argument(argv[3], 3600, &timeout_seconds) < 0)) {
        fprintf(stderr, "用法：%s [端口] [本机模型URL] [超时秒数] [模型名]\n", argv[0]);
        return 2;
    }
    unsigned model_port;
    if (strncmp(model_url, "http://127.0.0.1:", 17) ||
        number_argument(model_url + 17, 65535, &model_port) < 0) {
        fprintf(stderr, "模型URL仅支持本机 http://127.0.0.1:端口。\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0); //启动和退出提示即时输出，便于终端观察和自动验收
    //第二步：初始化共享管理对象；未显式指定的链表和活动计数初始化为空、0
    //所有工作线程共享此管理对象；每条连接自己的历史和日志保存在Session中
    Server server = {
        .sessions_mutex = PTHREAD_MUTEX_INITIALIZER,
        .sessions_done = PTHREAD_COND_INITIALIZER,
        .log_mutex = PTHREAD_MUTEX_INITIALIZER,
        .backend = "vllm",
        .model = {.base_url = model_url, .model = argc > 4 ? argv[4] : "qwen4b",
                  .context_tokens = 4096, .output_tokens = 1024, .timeout_seconds = timeout_seconds}
    };
    struct timespec now; //第三步：每次运行创建独立日志目录，用秒、纳秒和PID区分
    if (clock_gettime(CLOCK_REALTIME, &now) < 0 ||
        (mkdir("logs", 0700) < 0 && errno != EEXIST)) {
        perror("日志目录初始化");
        return 1;
    }
    snprintf(server.log_directory, sizeof(server.log_directory),
             "logs/run_%lld_%ld_%ld", (long long)now.tv_sec, now.tv_nsec, (long)getpid());
    char server_log_path[512];
    snprintf(server_log_path, sizeof(server_log_path), "%s/server.log", server.log_directory);
    if (mkdir(server.log_directory, 0700) < 0 ||
        (server.log = open_log(server_log_path)) == NULL) {
        perror("服务器日志初始化");
        return 1;
    }
    //第四步：准备监听socket和自管道；CLOEXEC避免描述符被未来exec调用意外继承
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0); //非阻塞监听，accept暂时无连接时不阻塞
    int signals[2] = {-1, -1}; //0端由主线程poll监控，1端由信号回调写入停止通知
    int reuse = 1;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK) //只监听127.0.0.1，不接受外部主机直接连接
    };
    int exit_status = 1;
    int model_ready = 0, slots_ready = 0; //记录初始化进度，失败跳到cleanup时只销毁已初始化资源
    pthread_attr_t attributes;
    int attributes_ready = 0;
    if (model_init() < 0)
        goto cleanup;
    model_ready = 1;
    if (sem_init(&server.model_slots, 0, 2) < 0) //模型任务的两个初始额度；0表示同一进程的线程共享
        goto cleanup;
    slots_ready = 1;
    //绑定监听地址并开始监听；backlog=16是待accept队列参数，与32个活动连接上限不同
    if (listener < 0 || setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0 ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(listener, 16) < 0 || pipe2(signals, O_CLOEXEC | O_NONBLOCK) < 0) {
        perror("监听初始化");
        goto cleanup;
    }
    //第五步：注册停止信号。自管道设为非阻塞，信号回调不会因管道写满而等待
    signal_write_fd = signals[1];
    struct sigaction action = {.sa_handler = stop_signal};
    sigemptyset(&action.sa_mask);
    struct sigaction ignore = {.sa_handler = SIG_IGN};
    sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0 ||
        sigaction(SIGPIPE, &ignore, NULL) < 0) { //对断开的连接写入时检查错误，不让SIGPIPE直接终止整个服务
        perror("信号初始化");
        goto cleanup;
    }
    int error = pthread_attr_init(&attributes); //线程接口失败直接返回错误码，下面转存errno供perror输出
    if (error != 0) {
        errno = error;
        perror("线程属性初始化");
        goto cleanup;
    }
    attributes_ready = 1;
    error = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED); //分离线程结束后自动回收线程资源，不逐个join
    if (error != 0) {
        errno = error;
        perror("线程属性设置");
        goto cleanup;
    }
    printf("监听 127.0.0.1:%u，后端 %s，超时 %u 秒\n日志：%s\n",
           port, server.backend, timeout_seconds, server.log_directory);
    log_event(&server, 0, "started port=%u backend=%s", port, server.backend);
    exit_status = 0;
    uint64_t next_session = 1; //第六步：主线程只接受连接和协调停止，每次接入分配递增会话号
    while (!stop_requested) {
        struct pollfd fds[2] = {
            {.fd = listener, .events = POLLIN},
            {.fd = signals[0], .events = POLLIN}
        };
        if (poll(fds, 2, -1) < 0) { //-1表示持续等待，直到新连接、停止通知或信号中断
            if (errno == EINTR)
                continue;
            perror("poll");
            exit_status = 1;
            break;
        }
        if (stop_requested || fds[1].revents != 0) //停止优先，收到停止请求后不再接入新连接
            break;
        if (fds[0].revents & POLLIN) {
            int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC); //取得新连接的socket；listener继续用于接受其他连接
            if (fd < 0) {
                if (errno != EINTR && errno != EAGAIN) //中断或连接暂时不可取不算致命错误，继续监听
                    perror("accept");
                continue;
            }
            struct timeval send_timeout = {5, 0}; //限制向该客户端发送时的等待时间，不是模型生成的120秒超时
            if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout)) < 0) {
                perror("socket timeout");
                close(fd);
                continue;
            }
            if (add_client(&server, fd, next_session++, &attributes) < 0) //分配会话和线程，完成后主线程继续接入新连接
                perror("客户端线程初始化");
        } else if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            exit_status = 1;
            break;
        }
    }
    //整体停止：先shutdown唤醒各连接的阻塞收发，再等工作线程自行清理；不停止共享vLLM
    log_event(&server, 0, "stopping");
    pthread_mutex_lock(&server.sessions_mutex);
    for (Session *session = server.sessions; session != NULL; session = session->next)
        (void)shutdown(session->client_fd, SHUT_RDWR);
    while (server.active_count != 0) //分离线程不逐个join，按活动计数判断是否已完成共享资源的使用
        pthread_cond_wait(&server.sessions_done, &server.sessions_mutex);
    pthread_mutex_unlock(&server.sessions_mutex);
    log_event(&server, 0, "stopped; all client threads and owned generation tasks released");
    printf("服务器已停止，客户端线程及所属生成任务已清理。\n");

cleanup:
    //第七步：正常退出和初始化失败共用清理入口，此时已运行的工作线程不再使用这些共享资源
    if (slots_ready)
        sem_destroy(&server.model_slots);
    if (model_ready)
        model_cleanup();
    if (attributes_ready)
        pthread_attr_destroy(&attributes);
    if (listener >= 0)
        close(listener);
    /*先忽略停止信号，再关闭自管道，避免后到的信号写入已关闭或已被复用的描述符。*/
    struct sigaction reset = {.sa_handler = SIG_IGN};
    sigemptyset(&reset.sa_mask);
    (void)sigaction(SIGINT, &reset, NULL);
    (void)sigaction(SIGTERM, &reset, NULL);
    if (signals[0] >= 0)
        close(signals[0]);
    if (signals[1] >= 0)
        close(signals[1]);
    fclose(server.log);
    pthread_mutex_destroy(&server.sessions_mutex);
    pthread_cond_destroy(&server.sessions_done);
    pthread_mutex_destroy(&server.log_mutex);
    return exit_status;
}
