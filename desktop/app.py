#!/usr/bin/env python3
"""Small Linux desktop UI: Tkinter widgets in this file, TCP in client.py."""
# 界面主线：new_conversation建连接 -> send提交问题 -> poll_events取片段并追加显示。
# Tk主线程只处理控件与队列，TCP阻塞收发由client.py的后台线程完成。
import argparse
from dataclasses import dataclass, field
import ipaddress
import queue
import sys
try:
    import tkinter as tk
    from tkinter import font, messagebox, ttk
except ImportError:
    raise SystemExit('缺少Tkinter，请在Ubuntu中执行 sudo apt install python3-tk。')

from client import ChatClient, QUESTION, CLEAR, INFO, MAX_QUESTION

BACKGROUND = '#f5f7f5'
INK = '#24312d'
GREEN = '#256c50'


@dataclass
class Conversation:
    # 一个界面会话保存自己的通信对象、聊天页、草稿和显示记录；模型历史实际保存在C服务端
    client: ChatClient
    page: tk.Frame
    text: tk.Text
    welcome: tk.Label
    title: str
    draft: str = ''
    answer: str = ''
    notice: str = ''
    named: bool = False
    info: dict = field(default_factory=dict)


class ChatApp:
    def __init__(self, root, host, port):
        # 创建字体、菜单、会话列表和聊天区，绑定操作回调，最后启动事件轮询
        self.root, self.host, self.port = root, host, port
        self.conversations = []
        self.selected = -1
        self.next_title = 1
        self.closed = False
        self.base_font = font.Font(root=root, family='Noto Sans CJK SC', size=14)
        self.small_font = font.Font(root=root, family='Noto Sans CJK SC', size=12)
        self.title_font = font.Font(root=root, family='Noto Sans CJK SC', size=20)
        self.welcome_font = font.Font(root=root, family='Noto Sans CJK SC', size=24)
        root.title('Linux Chat — 本地对话')
        root.geometry('1120x760')
        root.minsize(640, 480)
        root.resizable(True, True)
        root.configure(bg=BACKGROUND)
        root.protocol('WM_DELETE_WINDOW', self.close)
        menu = tk.Menu(root, font=self.base_font)
        display = tk.Menu(menu, tearoff=False, font=self.base_font)
        display.add_command(label='字体与字号…', command=self.choose_font)
        display.add_command(label='增大字号', accelerator='Ctrl++', command=lambda: self.change_size(1))
        display.add_command(label='减小字号', accelerator='Ctrl+-', command=lambda: self.change_size(-1))
        display.add_command(label='恢复默认字体', accelerator='Ctrl+0', command=lambda: self.set_font('Noto Sans CJK SC', 14))
        menu.add_cascade(label='显示', menu=display)
        root.configure(menu=menu)
        for key in ('<Control-plus>', '<Control-equal>'):
            root.bind(key, lambda event: self.change_size(1))
        root.bind('<Control-minus>', lambda event: self.change_size(-1))
        root.bind('<Control-0>', lambda event: self.set_font('Noto Sans CJK SC', 14))

        # 左右区域用可拖动分栏布局，控件随窗口缩放
        self.panes = tk.PanedWindow(root, orient=tk.HORIZONTAL, bg='#dce4de', sashwidth=6, bd=0)
        self.panes.pack(fill=tk.BOTH, expand=True)
        side = tk.Frame(self.panes, bg='white', padx=12, pady=18)
        self.panes.add(side, minsize=180, width=240)
        tk.Label(side, text='Linux Chat', bg='white', fg=GREEN, font=self.title_font).pack(anchor='w', pady=(0, 16))
        self.new_button = self.button(side, '＋ 新建会话', self.new_conversation)
        self.new_button.pack(fill=tk.X, pady=(0, 14))
        self.sessions = tk.Listbox(side, bg='white', fg=INK, selectbackground='#e4efe7',
                                   selectforeground=GREEN, font=self.base_font, bd=0,
                                   highlightthickness=0, exportselection=False, width=12, height=1)
        self.sessions.pack(fill=tk.BOTH, expand=True)
        self.sessions.bind('<<ListboxSelect>>', self.select_conversation)
        self.close_button = self.button(side, '关闭当前会话', self.close_conversation)
        self.close_button.pack(fill=tk.X, pady=(12, 0))

        body = tk.Frame(self.panes, bg=BACKGROUND, padx=16, pady=14)
        self.panes.add(body, minsize=360, stretch='always')
        body.columnconfigure(0, weight=1)
        body.rowconfigure(2, weight=1)
        heading = tk.Frame(body, bg=BACKGROUND)
        heading.grid(row=0, column=0, sticky='ew')
        heading.columnconfigure(0, weight=1)
        self.heading = tk.Label(heading, bg=BACKGROUND, fg=INK, font=self.title_font, anchor='w')
        self.heading.grid(row=0, column=0, sticky='ew')
        self.status = tk.Label(heading, bg='#e5f1e9', fg=GREEN, font=self.small_font, padx=8, pady=4)
        self.status.grid(row=0, column=1, sticky='e')
        self.meta = tk.Label(heading, bg=BACKGROUND, fg='#66766d', font=self.small_font, anchor='w')
        self.meta.grid(row=1, column=0, columnspan=2, sticky='ew', pady=(3, 6))
        tools = tk.Frame(body, bg=BACKGROUND)
        tools.grid(row=1, column=0, sticky='e', pady=(0, 8))
        self.info_button = self.button(tools, '刷新状态', lambda: self.control(INFO))
        self.info_button.pack(side=tk.LEFT, padx=3)
        self.clear_button = self.button(tools, '清空上下文', lambda: self.control(CLEAR))
        self.clear_button.pack(side=tk.LEFT, padx=3)
        self.connection_button = self.button(tools, '断开', self.toggle_connection)
        self.connection_button.pack(side=tk.LEFT, padx=3)
        self.pages = tk.Frame(body, bg=BACKGROUND)
        self.pages.grid(row=2, column=0, sticky='nsew')
        self.pages.rowconfigure(0, weight=1)
        self.pages.columnconfigure(0, weight=1)
        self.notice = tk.Label(body, bg=BACKGROUND, fg='#8a5c32', font=self.small_font, anchor='w', justify=tk.LEFT)
        self.notice.grid(row=3, column=0, sticky='ew', pady=3)
        self.composer = tk.Text(body, height=3, width=1, wrap=tk.WORD, font=self.base_font,
                                bg='white', fg=INK, bd=1, relief=tk.SOLID, padx=10, pady=8,
                                undo=True, insertofftime=0)
        self.composer.grid(row=4, column=0, sticky='ew')
        self.composer.bind('<Return>', self.enter)
        self.composer.bind('<KeyRelease>', lambda event: self.update_controls())
        input_tools = tk.Frame(body, bg=BACKGROUND)
        input_tools.grid(row=5, column=0, sticky='ew', pady=(8, 0))
        self.counter = tk.Label(input_tools, bg=BACKGROUND, fg='#66766d', font=self.small_font)
        self.counter.pack(side=tk.LEFT)
        self.send_button = self.button(input_tools, '发送', self.send)
        self.send_button.configure(bg=GREEN, fg='white', activebackground='#1e5942', activeforeground='white')
        self.send_button.pack(side=tk.RIGHT)
        body.bind('<Configure>', lambda event: self._wrap_labels(event.width))
        self.new_conversation()
        self.poll_id = root.after(30, self.poll_events)

    def button(self, parent, text, command):
        # 统一按钮外观，command是点击时执行的函数
        return tk.Button(parent, text=text, command=command, font=self.base_font,
                         bg='white', fg=INK, activebackground='#e4efe7', bd=0,
                         relief=tk.FLAT, padx=10, pady=7, cursor='hand2')

    @property
    def current(self):
        # 通过当前选中下标取得会话，其他操作无需直接管理下标
        return self.conversations[self.selected] if 0 <= self.selected < len(self.conversations) else None

    def _wrap_labels(self, width):
        # 窗口变窄时让标题、会话信息和提示自动换行
        self.heading.configure(wraplength=max(120, width - self.status.winfo_reqwidth() - 40))
        self.meta.configure(wraplength=max(120, width - 32))
        self.notice.configure(wraplength=max(120, width - 32))

    def set_font(self, family, size):
        # 修改共享Font对象，已有聊天页和输入框会一起更新字号
        size = max(9, min(32, int(size)))
        for item, extra in ((self.base_font, 0), (self.small_font, -2),
                            (self.title_font, 6), (self.welcome_font, 10)):
            item.configure(family=family, size=max(9, size + extra))
        return 'break'

    def change_size(self, delta):
        # 快捷键只改变当前字号，最终范围限制统一由set_font处理
        return self.set_font(self.base_font.actual('family'), self.base_font.actual('size') + delta)

    def choose_font(self):
        # 字体设置对话框，应用前检查字号；不涉及网络和模型配置
        dialog = tk.Toplevel(self.root)
        dialog.title('字体与字号')
        dialog.transient(self.root)
        dialog.resizable(False, False)
        family = tk.StringVar(dialog, self.base_font.actual('family'))
        size = tk.StringVar(dialog, str(self.base_font.actual('size')))
        tk.Label(dialog, text='字体', font=self.base_font).grid(row=0, column=0, padx=12, pady=12)
        ttk.Combobox(dialog, textvariable=family, values=sorted(set(font.families(self.root))),
                     state='readonly', width=28, font=self.base_font).grid(row=0, column=1, padx=12)
        tk.Label(dialog, text='字号（9–32 pt）', font=self.base_font).grid(row=1, column=0, padx=12)
        tk.Spinbox(dialog, from_=9, to=32, textvariable=size, font=self.base_font, width=5).grid(row=1, column=1, sticky='w', padx=12)

        def apply():
            # 字号通过检查后应用并关闭对话框，非法输入保留窗口供用户修改
            try:
                self.set_font(family.get(), int(size.get()))
            except ValueError:
                messagebox.showerror('字号无效', '请输入9–32之间的整数。', parent=dialog)
                return
            dialog.destroy()
        self.button(dialog, '应用', apply).grid(row=2, column=1, sticky='e', padx=12, pady=12)

    def new_conversation(self):
        # 新建独立ChatClient即新建TCP连接，服务端因此分配独立Session和历史
        if len(self.conversations) >= 8:
            return
        self.save_draft()
        page = tk.Frame(self.pages, bg=BACKGROUND)
        page.grid(row=0, column=0, sticky='nsew')
        text = tk.Text(page, wrap=tk.WORD, width=1, height=1, font=self.base_font,
                       bg=BACKGROUND, fg=INK, bd=0, padx=12, pady=12,
                       state=tk.DISABLED, insertontime=0)
        scroll = tk.Scrollbar(page, command=text.yview)
        scroll.pack(side=tk.RIGHT, fill=tk.Y)
        text.configure(yscrollcommand=scroll.set)
        text.pack(fill=tk.BOTH, expand=True)
        text.bind('<Configure>', lambda event: self.layout_messages(text))
        text.tag_configure('user', foreground=INK, background='#d9efdd',
                           justify=tk.RIGHT, spacing1=8, spacing3=8)
        text.tag_configure('assistant', foreground=INK, background='#e7edf4',
                           justify=tk.LEFT, spacing1=8, spacing3=8)
        welcome = tk.Label(page, text='有什么想聊的', bg=BACKGROUND, fg=GREEN, font=self.welcome_font)
        welcome.place(relx=.5, rely=.4, anchor=tk.CENTER)
        c = Conversation(ChatClient(self.host, self.port), page, text, welcome, f'新会话 {self.next_title}')
        self.next_title += 1
        self.conversations.append(c)
        self.sessions.insert(tk.END, c.title)
        self.select(len(self.conversations) - 1)

    @staticmethod
    def layout_messages(text):
        # 根据聊天区宽度调整消息缩进，让用户文本靠右、助手文本靠左，不修改消息本身
        # 左右留出四分之一宽度，长消息换行后仍保留所属一侧。
        inset = max(24, text.winfo_width() // 4)
        text.tag_configure('user', lmargin1=inset, lmargin2=inset, rmargin=8)
        text.tag_configure('assistant', lmargin1=8, lmargin2=8, rmargin=inset)
        text.see(tk.END)

    def save_draft(self):
        # 切换会话前保存尚未发送的输入，不把草稿发给服务端
        if self.current:
            self.current.draft = self.composer.get('1.0', 'end-1c')

    def select(self, index):
        # 切换显示页并恢复该会话草稿，其他会话的后台生成仍继续
        self.save_draft()
        self.selected = index
        self.sessions.selection_clear(0, tk.END)
        self.sessions.selection_set(index)
        self.current.page.tkraise()
        self.composer.configure(state=tk.NORMAL)
        self.composer.delete('1.0', tk.END)
        self.composer.insert('1.0', self.current.draft)
        self.update_controls()

    def select_conversation(self, event=None):
        # 列表事件只切换选中会话，保存草稿和刷新控件交给select处理
        selection = self.sessions.curselection()
        if selection:
            self.select(selection[0])

    def close_conversation(self):
        # 只关闭当前会话连接和页面，不影响其他会话或共享模型服务
        c = self.current
        if c is None:
            return
        index = self.selected
        c.client.close()
        c.page.destroy()
        self.conversations.pop(index)
        self.sessions.delete(index)
        self.selected = -1
        if self.conversations:
            self.select(min(index, len(self.conversations) - 1))
        else:
            self.new_conversation()

    @staticmethod
    def append(c, text, tag='assistant'):
        # 聊天记录保持只读，追加时临时允许写入，并滚动到底部
        c.text.configure(state=tk.NORMAL)
        c.text.insert(tk.END, text, tag)
        c.text.configure(state=tk.DISABLED)
        c.text.see(tk.END)

    def send(self):
        # 发送时把问题交给通信队列；生成中同一按钮改为停止，只断开当前连接
        c = self.current
        if c.client.state == 'generating':
            self.toggle_connection()
            return
        question = self.composer.get('1.0', 'end-1c')
        if not c.client.request(QUESTION, question):
            return
        c.welcome.place_forget()
        self.append(c, f'{question}\n', 'user')
        c.answer, c.notice, c.draft = '', '', ''
        if not c.named:
            c.title = ' '.join(question.split())[:16]
            c.named = True
            self.sessions.delete(self.selected)
            self.sessions.insert(self.selected, c.title)
            self.sessions.selection_set(self.selected)
        self.composer.delete('1.0', tk.END)
        self.update_controls()

    def enter(self, event):
        # Enter发送，Shift+Enter保留输入框的默认换行行为
        if event.state & 0x0001:  # Shift+Enter keeps Tk's normal newline behavior.
            return None
        if self.current.client.state == 'ready':
            self.send()
        return 'break'

    def control(self, kind):
        # INFO和CLEAR同样走TCP协议，实际会话数据由服务端查询或清空
        self.current.client.request(kind)
        self.update_controls()

    def toggle_connection(self):
        # 停止通过断开连接实现；重连创建新Session，界面旧记录只供查看，不会自动恢复模型历史
        c = self.current
        if c.client.state == 'disconnected':
            c.client = ChatClient(self.host, self.port)
            c.info = {}
            c.notice = '正在重新连接；旧记录仅供查看，新连接使用新的上下文。'
        else:
            generating = c.client.state == 'generating'
            c.client.close()
            if generating:
                self.append(c, '\n')
            c.notice = '已停止生成，重新连接后开启新的上下文。' if generating else '已断开当前会话。'
        self.update_controls()

    def poll_events(self):
        # This method runs on the Tk main thread. Workers only put data in queues.
        # 遍历所有会话取事件，合并本次取出的连续片段以减少控件刷新，不等待整轮生成结束
        for c in self.conversations:
            chunks = []
            for _ in range(500):
                try:
                    kind, value = c.client.events.get_nowait()
                except queue.Empty:
                    break
                if kind == 'chunk':
                    c.answer += value
                    chunks.append(value)
                    continue
                if chunks:
                    self.append(c, ''.join(chunks))
                    chunks.clear()
                if kind == 'info':
                    c.info = value
                elif kind == 'notice':
                    c.notice = value
                elif kind == 'done':
                    success, message = value
                    if not success and not c.answer:
                        self.append(c, '本轮未收到完整回答。')
                    self.append(c, '\n')
                    c.notice = '' if success else message
                    if success:
                        # 成功后向服务端查询实际历史轮数，避免界面自行猜测裁剪结果
                        c.client.request(INFO)
                elif kind == 'cleared':
                    c.text.configure(state=tk.NORMAL)
                    c.text.delete('1.0', tk.END)
                    c.text.configure(state=tk.DISABLED)
                    c.welcome.place(relx=.5, rely=.4, anchor=tk.CENTER)
                    c.answer = ''
                    c.info['history_turns'] = 0
                    c.notice = ''
            if chunks:
                self.append(c, ''.join(chunks))
        self.update_controls()
        self.poll_id = self.root.after(30, self.poll_events)  # 30毫秒后再次处理事件，主线程不做阻塞等待

    def update_controls(self):
        # 根据连接状态和输入字节数更新按钮；仅在需要时修改输入框state，避免光标频闪
        c = self.current
        if c is None:
            return
        state = c.client.state
        ready, generating = state == 'ready', state == 'generating'
        self.heading.configure(text=c.title)
        self.status.configure(text=dict(connecting='连接中', ready='已连接', generating='生成中…',
                                        controlling='处理中', disconnected='未连接')[state])
        backend = '本地模型'
        self.meta.configure(text=f"{backend} · 会话 {c.info['session']} · 上下文 {c.info['history_turns']} 轮"
                            if c.info else f'{self.host}:{self.port}')
        self.notice.configure(text=c.notice)
        self.notice.grid() if c.notice else self.notice.grid_remove()
        for button in (self.clear_button, self.info_button):
            button.configure(state=tk.NORMAL if ready else tk.DISABLED)
        self.new_button.configure(state=tk.NORMAL if len(self.conversations) < 8 else tk.DISABLED)
        self.connection_button.configure(text='重新连接' if state == 'disconnected' else '断开')
        composer_state = tk.NORMAL if ready else tk.DISABLED
        if str(self.composer['state']) != composer_state:
            self.composer.configure(state=composer_state)
        question = self.composer.get('1.0', 'end-1c')
        size = len(question.encode('utf-8'))
        self.counter.configure(text=f'{size} / 4096 字节')
        valid = ready and bool(question.strip()) and '\0' not in question and size <= MAX_QUESTION
        self.send_button.configure(text='停止生成' if generating else '发送',
                                   state=tk.NORMAL if generating or valid else tk.DISABLED)

    def close(self):
        # 窗口退出时先停止轮询并关闭各连接，再销毁Tk窗口
        if self.closed:
            return
        self.closed = True
        self.root.after_cancel(self.poll_id)
        for c in self.conversations:
            c.client.close()
        self.root.destroy()


def main():
    # --check只验证环境；正常启动则创建窗口并进入Tk事件循环
    parser = argparse.ArgumentParser(description='Linux原生Tkinter多会话客户端；Enter发送，Shift+Enter换行。')
    parser.add_argument('--host', type=ipaddress.IPv4Address, default=ipaddress.IPv4Address('127.0.0.1'))
    parser.add_argument('--port', type=int, default=5000)
    parser.add_argument('--check', action='store_true', help='仅检查Linux与Tkinter环境，不创建窗口')
    args = parser.parse_args()
    if not sys.platform.startswith('linux'):
        parser.error('请在Linux或WSL中运行桌面客户端。')
    if not 1 <= args.port <= 65535:
        parser.error('端口必须在1–65535之间。')
    if args.check:
        print(f'Linux / Python {sys.version.split()[0]} / Tk {tk.TkVersion}：环境检查通过')
        return 0
    try:
        root = tk.Tk()
    except tk.TclError as error:
        print(f'无法打开Linux图形会话：{error}。请使用WSLg、X11，或运行 make run-client。', file=sys.stderr)
        return 1
    ChatApp(root, str(args.host), args.port)
    root.mainloop()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
