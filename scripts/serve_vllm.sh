#!/usr/bin/env bash
# 使用已准备好的本地环境和权重启动常驻推理服务，不负责下载或安装。
set -euo pipefail
[[ "$(uname -s)" == Linux ]] || { echo '请在Linux/WSL中运行。' >&2; exit 1; }
task_runtime_dir="${VLLM_RUNTIME_DIR:-$HOME/.local/share/linux-chat}"
task_model_dir="${VLLM_MODEL_DIR:-$task_runtime_dir/models/Qwen3-4B-Instruct-2507-AWQ-Eslzzyl}"
[[ -x "$task_runtime_dir/venv/bin/vllm" && -f "$task_model_dir/download-manifest.json" ]] || {
    echo '请先运行 scripts/setup_vllm.sh 和 python3 scripts/download_model.py。' >&2; exit 1;
}
# 启动时只使用本地模型文件，关闭在线下载和使用统计。
export HF_HUB_OFFLINE=1 TRANSFORMERS_OFFLINE=1 VLLM_NO_USAGE_STATS=1 DO_NOT_TRACK=1
export OMP_NUM_THREADS=4 VLLM_WORKER_MULTIPROC_METHOD=spawn
export VLLM_ATTENTION_BACKEND=FLASH_ATTN
# 进程间通信选回环接口，便于在只保留lo的离线测试环境中启动。
export VLLM_HOST_IP=127.0.0.1 GLOO_SOCKET_IFNAME=lo NCCL_SOCKET_IFNAME=lo
# 从模型配置识别AWQ，使用预编译注意力内核；只监听回环地址，两个序列与C端模型额度对应。
# 4096是上下文上限，0.60是显存预算比例，不表示GPU计算利用率上限。
exec "$task_runtime_dir/venv/bin/vllm" serve "$task_model_dir" \
    --served-model-name qwen4b --host 127.0.0.1 --port "${VLLM_PORT:-8000}" \
    --max-model-len 4096 --max-num-seqs 2 --max-num-batched-tokens 1024 \
    --gpu-memory-utilization "${VLLM_GPU_MEMORY_UTILIZATION:-0.60}" --swap-space 0 \
    --dtype half --enforce-eager --enable-request-id-headers --enable-log-requests
