"""TCP client only: no Tkinter imports, no changes to the existing C server."""
# 通信主线：request入命令队列 -> _run发送TCP帧 -> _response收片段 -> 事件队列交给界面。
# 每个会话独占一个socket和后台线程，网络线程不直接操作Tk控件。
import codecs
import queue
import re
import select
import socket
import struct
import threading
import time

# Wire values and limits match include/protocol.h; !III is three big-endian uint32s.
HELLO, QUESTION, CLEAR, INFO, BEGIN, CHUNK, END, ERROR, QUIT = range(1, 10)
HEADER = struct.Struct('!III')  # 与C协议一致：三个大端32位无符号整数，头部共12字节
MAX_MESSAGE = 65536
MAX_QUESTION = 4096


def receive_exact(sock, length, deadline):
    """TCP has no message boundaries: keep reading until all requested bytes arrive."""
    # 循环读满指定字节，超时和正文截断都抛异常，不能把一次recv当作一帧
    data = bytearray()
    while len(data) < length:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('等待服务器超时。')
        sock.settimeout(remaining)
        part = sock.recv(length - len(data))
        if not part:
            raise ConnectionError('服务器关闭了连接，重新连接将开启新的上下文。')
        data.extend(part)
    return bytes(data)


def receive_frame(sock, deadline):
    # 先读固定头部，检查类型和长度，再按length读正文
    kind, request, length = HEADER.unpack(receive_exact(sock, HEADER.size, deadline))
    if not HELLO <= kind <= QUIT or length > MAX_MESSAGE:
        raise ValueError('服务器消息类型或长度无效。')
    return kind, request, receive_exact(sock, length, deadline)


def parse_info(body):
    # 从HELLO或INFO正文提取会话编号、后端和实际历史轮数，供界面展示
    text = body.decode('utf-8')
    identity = re.search(r'session=(\d+)\s+backend=(\w+)', text)
    if not identity:
        raise ValueError('会话信息格式无效。')
    turns = re.search(r'history_turns=(\d+)', text)
    return dict(session=identity[1], backend=identity[2],
                history_turns=int(turns[1]) if turns else 0)


class ChatClient:
    """One socket/worker per conversation. The UI polls events, never reads the socket."""
    def __init__(self, host, port):
        # 创建会话私有队列和停止标记，线程启动后自行连接；构造函数不等待握手完成
        self.host, self.port = host, port
        self.events = queue.Queue()
        self.commands = queue.Queue()
        # commands由界面写、网络线程读；events由网络线程写、界面主线程读
        self.lock = threading.Lock()
        self.stopped = threading.Event()  # 跨线程通知停止，关闭socket唤醒接收后由网络线程完成清理
        self.sock = None
        self.state = 'connecting'
        self.sequence = 0
        self.thread = threading.Thread(target=self._run, daemon=True)  # 后台连接和收发，避免阻塞界面
        self.thread.start()

    def _state(self, state):
        # 状态改变才投递事件，避免界面重复处理相同状态
        with self.lock:
            changed = self.state != state
            self.state = state
        if changed:
            self.events.put(('state', state))

    def request(self, kind, question=''):
        # 检查UTF-8字节上限并占用本连接，再入队；此方法本身不等待网络响应
        body = question.encode('utf-8')
        if kind == QUESTION and (not question.strip() or '\0' in question or len(body) > MAX_QUESTION):
            self.events.put(('notice', '问题需为非空文本，最多4096 UTF-8字节。'))
            return False
        if kind not in (QUESTION, CLEAR, INFO):
            return False
        # Atomically claim the connection so double clicks cannot queue two requests.
        with self.lock:
            if self.state != 'ready' or self.stopped.is_set():
                return False
            self.state = 'generating' if kind == QUESTION else 'controlling'
            self.events.put(('state', self.state))
            self.commands.put((kind, body))
        return True

    def close(self):
        """Closing only this socket lets the C server cancel only this client's task."""
        # 标记停止并shutdown唤醒阻塞接收，最终socket.close由网络线程负责
        self.stopped.set()
        if self.sock is not None:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self._state('disconnected')

    def _run(self):
        # 建立TCP连接、验证HELLO，再串行处理本会话命令；每个请求使用递增编号
        try:
            self.sock = socket.create_connection((self.host, self.port), timeout=5)
            if self.stopped.is_set():
                return
            kind, request, body = receive_frame(self.sock, time.monotonic() + 5)
            if kind == ERROR and request == 0:
                raise ConnectionError(body.decode('utf-8'))
            if kind != HELLO or request != 0:
                raise ValueError('服务器握手无效。')
            self.events.put(('info', parse_info(body)))
            self._state('ready')
            while not self.stopped.is_set():
                try:
                    kind, body = self.commands.get(timeout=.1)
                except queue.Empty:
                    # Detect a server shutdown even when no question is being sent.
                    if select.select([self.sock], [], [], 0)[0]:
                        raise ConnectionError('服务器连接已关闭或返回了意外消息。')
                    continue
                self.sequence += 1
                if self.sequence > 0xffffffff:
                    raise ValueError('请求编号已用完，请重新连接。')
                self.sock.settimeout(5)
                self.sock.sendall(HEADER.pack(kind, self.sequence, len(body)) + body)  # 一帧由头部和正文组成
                self._response(kind, time.monotonic() + (130 if kind == QUESTION else 5))  # 每轮共用一个接收期限，不随片段到达重置
        except (OSError, ValueError, UnicodeError) as error:
            if not self.stopped.is_set():
                if self.state == 'generating':
                    self.events.put(('done', (False, str(error))))
                self.events.put(('notice', str(error)))
        finally:
            # 所有正常和异常结束都关闭本连接，界面通过状态事件决定是否允许重连
            if self.sock is not None:
                self.sock.close()
            self._state('disconnected')

    def _response(self, sent_kind, deadline):
        # 按BEGIN -> 多个CHUNK -> END接收一轮回答，ERROR结束本轮但可以继续使用连接
        began, answer_bytes = False, 0
        decoder = codecs.getincrementaldecoder('utf-8')('strict')  # 中文可能跨CHUNK，保留未完整字符的字节
        while not self.stopped.is_set():
            kind, request, body = receive_frame(self.sock, deadline)
            if request != self.sequence:
                raise ValueError('服务器响应编号与当前请求不匹配。')
            if kind == ERROR:
                message = body.decode('utf-8').strip()
                self._state('ready')
                self.events.put(('done', (False, message)) if sent_kind == QUESTION else ('notice', message))
                return
            if sent_kind in (CLEAR, INFO) and kind == sent_kind:
                if kind == INFO:
                    self.events.put(('info', parse_info(body)))
                else:
                    self.events.put(('cleared', None))
                self._state('ready')
                return
            if sent_kind == QUESTION:
                if kind == BEGIN and not began:
                    began = True
                    continue
                if kind == CHUNK and began:
                    answer_bytes += len(body)
                    if answer_bytes > MAX_MESSAGE:
                        raise ValueError('本轮输出超过65536字节。')
                    # UTF-8 characters can straddle CHUNK frames; preserve decoder state.
                    text = decoder.decode(body)
                    if text:
                        self.events.put(('chunk', text))  # 交给界面显示，网络线程不更新控件
                    continue
                if kind == END and began and not body and answer_bytes:
                    decoder.decode(b'', final=True)  # 结束时若仍有半个UTF-8字符，按截断响应报错
                    self._state('ready')
                    self.events.put(('done', (True, '')))
                    return
            raise ValueError('服务器消息不符合当前请求或流式顺序。')
