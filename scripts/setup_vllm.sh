#!/usr/bin/env bash
# 首次准备：在用户目录建立独立虚拟环境，安装固定依赖，不覆盖系统Python。
set -euo pipefail
[[ "$(uname -s)" == Linux ]] || { echo '请在Linux/WSL中运行。' >&2; exit 1; }
task_project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
task_runtime_dir="${VLLM_RUNTIME_DIR:-$HOME/.local/share/linux-chat}"
# 网络超时和下载并发可从外部覆盖，限制构建并发以减少准备阶段的内存占用。
export UV_HTTP_TIMEOUT="${UV_HTTP_TIMEOUT:-600}"
export UV_CONCURRENT_DOWNLOADS="${UV_CONCURRENT_DOWNLOADS:-8}"
export UV_CONCURRENT_BUILDS="${UV_CONCURRENT_BUILDS:-2}"
mkdir -p "$task_runtime_dir"
if [[ ! -x "$task_runtime_dir/venv/bin/python" ]]; then
    python3 -m venv "$task_runtime_dir/venv"
fi
# uv负责安装清单中的推理依赖，网络下载只发生在准备阶段。
"$task_runtime_dir/venv/bin/python" -m pip install --disable-pip-version-check uv==0.13.0
"$task_runtime_dir/venv/bin/uv" pip install --python "$task_runtime_dir/venv/bin/python" \
    -r "$task_project_dir/config/requirements-vllm.txt"
# 最后检查实际Python包版本与CUDA可用性，确认环境能够访问GPU。
"$task_runtime_dir/venv/bin/python" -c 'import torch, vllm; print("vLLM", vllm.__version__, "torch", torch.__version__, "CUDA", torch.cuda.is_available()); assert torch.cuda.is_available(); print(torch.cuda.get_device_name())'
