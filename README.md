# Linux本地模型流式对话系统

C服务端通过本机HTTP/SSE调用vLLM与Qwen 4B。C终端客户端和Python Tkinter桌面客户端共用TCP协议，支持多客户端流式问答、独立上下文、清空和断连取消。应用只使用本地模型，没有语料后端。

## 运行条件

已验证环境为WSL2 Ubuntu 22.04、Python 3.10、RTX 4060 Laptop 8GB。需要可供Linux访问的兼容NVIDIA GPU；当前启动配置不是CPU运行方案。其他GPU和Linux版本需另行验证。

源码包不包含模型权重、Python虚拟环境和预编译程序。首次部署需要联网安装依赖、下载并校验固定模型；准备完成后可以使用本机文件运行。下载脚本会校验约2.67GB的权重文件，推理依赖还需要额外磁盘空间。

## 首次部署

在Linux终端克隆仓库并进入项目目录，然后安装依赖：

```bash
git clone https://github.com/illnessemc/Linux_chat.git
cd Linux_chat
sudo apt update
sudo apt install build-essential pkg-config libcurl4-openssl-dev libjson-c-dev python3-venv curl
nvidia-smi
bash scripts/setup_vllm.sh
python3 scripts/download_model.py
make
```

使用桌面客户端还需安装`python3-tk fonts-noto-cjk`，并具有Linux图形会话或WSLg。无图形环境可直接使用C终端客户端。

## 日常启动

在项目目录打开三个终端，依次运行以下命令。模型接口就绪前不要开始提问。

```bash
# 终端一：启动模型服务，默认127.0.0.1:8000
make run-model
# 可在另一个终端确认返回的模型列表中含qwen4b
curl --noproxy '*' --fail http://127.0.0.1:8000/v1/models

# 终端二：编译并启动C服务，默认127.0.0.1:5000
make run

# 终端三：选择一种客户端
make run-client
# 或：make run-desktop
```

终端命令`/info`查询当前会话，`/clear`清空本连接上下文，`/quit`退出。桌面支持新建、切换、关闭会话，以及清空、停止和重连。停止生成会关闭当前连接，重连后创建新上下文。

完整部署、改端口、中文输入与故障处理见[部署与运行说明](docs/运行说明.md)。

## 文件与资源

`src/`和`include/`为C程序，`desktop/app.py`与`desktop/client.py`为桌面程序，`config/`保存固定模型和推理依赖，`scripts/`保存安装、下载和模型启动入口。模型环境默认位于`~/.local/share/linux-chat`，不随项目移动。

构建会自动生成`build/`，C服务启动会自动生成`logs/`。关闭客户端和C服务不会卸载独立的vLLM；需要释放模型资源时，在模型终端按Ctrl+C。`make clean`只清理构建目录，不会删除模型或运行日志。日志可能包含用户输入和模型输出，不宜随意共享。
