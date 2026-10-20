# 构建入口：C程序编译到build/bin，Python桌面直接运行源码。
CC = gcc
CPPFLAGS = -D_GNU_SOURCE -Iinclude
# GNU扩展提供POLLRDHUP等接口，-pthread启用线程支持，只有服务端链接curl和json-c。
CFLAGS = -std=c11 -O2 -g -Wall -Wextra -Wpedantic -pthread
LDFLAGS = -pthread
BUILD_DIR ?= build
BIN_DIR ?= $(BUILD_DIR)/bin
OBJ_DIR = $(BUILD_DIR)/obj
# 链接时组合各模块对象文件；客户端只需要自己的入口和公共TCP协议。
SERVER_OBJECTS = $(addprefix $(OBJ_DIR)/,server/server.o common/protocol.o generator/model.o session/history.o)
SERVER_LIBS = $(shell pkg-config --libs libcurl json-c)
CLIENT_OBJECTS = $(addprefix $(OBJ_DIR)/,client/client.o common/protocol.o)
OBJECTS = $(sort $(SERVER_OBJECTS) $(CLIENT_OBJECTS))
# 运行默认值可在命令行覆盖；服务端只调用本机模型。
PORT ?= 5000
HOST ?= 127.0.0.1
TIMEOUT ?= 120
VLLM_URL ?= http://127.0.0.1:8000
MODEL ?= qwen4b

.PHONY: all run run-server run-client run-model clean desktop run-desktop
all: $(BIN_DIR)/server $(BIN_DIR)/client

# 竖线后的目录只要求存在；$^表示普通依赖，$@表示目标文件。
$(BIN_DIR)/server: $(SERVER_OBJECTS) | $(BIN_DIR)
	$(CC) $^ $(LDFLAGS) $(SERVER_LIBS) -o $@

$(BIN_DIR)/client: $(CLIENT_OBJECTS) | $(BIN_DIR)
	$(CC) $^ $(LDFLAGS) -o $@

# 按目录编译，并生成头文件依赖，方便修改源码后增量构建。
$(OBJ_DIR)/%.o: src/%.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BIN_DIR):
	mkdir -p $@

# 三个独立入口：模型服务、C服务端、客户端要在不同终端运行。
run: run-server

run-server: all
	$(BIN_DIR)/server "$(PORT)" "$(VLLM_URL)" "$(TIMEOUT)" "$(MODEL)"

run-model:
	bash scripts/serve_vllm.sh

run-client: all
	$(BIN_DIR)/client "$(HOST)" "$(PORT)"

# 桌面检查不创建窗口，run-desktop才启动Tk界面。
desktop:
	python3 desktop/app.py --check

run-desktop: desktop
	python3 desktop/app.py --host "$(HOST)" --port "$(PORT)"

# 清理可再生成的构建目录，保留源码、模型配置和运行日志。
clean:
	rm -rf build

# 首次构建可能还没有.d文件，前缀减号让缺失依赖文件不报错。
-include $(OBJECTS:.o=.d)
