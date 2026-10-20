#!/usr/bin/env python3
"""Download pinned data files on Linux; resume and verify the model weights."""
# 下载主线：读固定模型清单 -> 检查已有文件 -> 断点续传 -> 大小和SHA-256校验 -> 写完成清单。
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import sys
import time
import urllib.error
import urllib.request


def digest(path):
    # 分块计算完整文件哈希，避免一次把数GB权重读入内存
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def download_large(url, partial, total, jobs):
    """Fetch bounded ranges concurrently, append only a contiguous prefix."""
    # 大文件按16MiB分段并发下载，按顺序追加到.part，已完成的连续前缀可续传
    block = 16 * 1024 * 1024
    offset = partial.stat().st_size if partial.exists() else 0
    if offset > total:
        raise ValueError("已有.part大于清单大小，请检查文件")

    def fetch(start):
        # 请求指定字节范围，中断后从本段已取得的位置重试，并检查Content-Range
        end = min(start + block, total) - 1
        data = bytearray()
        for attempt in range(12):
            position = start + len(data)
            # A distinct resolver URL avoids stale CDN redirects cached for another range.
            separator = "&" if "?" in url else "?"  # 请求URL附带本段范围，避免复用其他范围的缓存重定向
            request = urllib.request.Request(url + f"{separator}range={position}-{end}",
                                             headers={"Range": f"bytes={position}-{end}"})
            try:
                with urllib.request.urlopen(request, timeout=60) as response:
                    content_range = response.headers.get("Content-Range", "")
                    if response.status != 206 or not content_range.startswith(f"bytes {position}-"):
                        raise ValueError(f"下载服务器返回错误的续传范围：{position}-{end}，{content_range}")
                    while len(data) <= end - start:
                        chunk = response.read(min(1024 * 1024, end - start + 1 - len(data)))
                        if not chunk:
                            break
                        data.extend(chunk)
                if len(data) == end - start + 1:
                    return data
            except (OSError, urllib.error.URLError) as error:
                print(f"范围 {position}-{end} 下载中断：{error}", flush=True)
            time.sleep(2)
        raise ValueError(f"范围 {start}-{end} 重试耗尽，可再次执行脚本续传")

    # Limit outstanding data to jobs * 16MiB, even when one range is slow.
    with ThreadPoolExecutor(jobs) as pool, partial.open("ab") as output:  # pool.map按输入顺序返回，确保追加文件连续
        while offset < total:
            starts = list(range(offset, min(offset + jobs * block, total), block))
            for data in pool.map(fetch, starts):
                output.write(data)
                output.flush()
                offset += len(data)
            print(f"{partial.stem}: {offset}/{total} 字节", flush=True)


def main():
    # 模型来源、固定修订号和期望哈希来自config/model.json，不自动改用其他版本
    if not sys.platform.startswith("linux"):
        raise SystemExit("请在Linux/WSL中下载和准备模型。")
    project = Path(__file__).resolve().parent.parent
    config = json.loads((project / "config/model.json").read_text())
    parser = argparse.ArgumentParser()
    default = Path(os.environ.get("VLLM_RUNTIME_DIR", Path.home() / ".local/share/linux-chat"))
    parser.add_argument("--model-dir", type=Path, default=default / "models" / config["directory"])
    parser.add_argument("--jobs", type=int, choices=range(1, 9), default=4,
                        help="权重分段下载并发数，1–8；仍校验完整文件SHA-256")
    args = parser.parse_args()
    args.model_dir.mkdir(parents=True, exist_ok=True)
    manifest = {}  # 记录实际大小和哈希，所有文件验证通过后才写下载完成清单
    for name, expected in config["files"].items():
        if Path(name).name != name:
            raise ValueError("模型清单仅允许文件名")
        target = args.model_dir / name
        if target.exists():
            # 已有正式文件先校验，不匹配就报错，避免静默覆盖用户文件
            checksum = digest(target)
            if expected.get("sha256") and checksum != expected["sha256"]:
                raise ValueError(f"已有文件校验失败，未覆盖：{target}")
            print(f"已有 {name}", flush=True)
        else:
            partial = target.with_name(name + ".part")
            url = (f'https://modelscope.cn/api/v1/models/{config["repository"]}/repo'
                   f'?Revision={config["revision"]}&FilePath={name}')
            total = expected.get("bytes", 0)
            if total >= 256 * 1024 * 1024:
                download_large(url, partial, total, args.jobs)
            for attempt in range(24):
                offset = partial.stat().st_size if partial.exists() else 0
                total = expected.get("bytes", 0)
                if total and offset == total:
                    break
                if total and offset > total:
                    raise ValueError("已有.part大于清单大小，请检查文件")
                # Bound each large transfer so dropped connections can resume efficiently.
                end = min(offset + 256 * 1024 * 1024, total) - 1 if total else ""
                headers = {"Range": f"bytes={offset}-{end}"} if offset or total else {}
                request = urllib.request.Request(url, headers=headers)
                print(f"下载 {name}，已存在 {offset} 字节，尝试 {attempt + 1}", flush=True)
                try:
                    with urllib.request.urlopen(request, timeout=120) as response:
                        if offset and response.status != 206:
                            raise ValueError("下载服务器不支持此次续传，未覆盖已有.part")
                        if response.status == 206 and not response.headers.get("Content-Range", "").startswith(f"bytes {offset}-"):
                            raise ValueError("下载服务器返回错误的续传范围")
                        declared = int(response.headers.get("Content-Length", "0"))
                        received = 0
                        with partial.open("ab" if offset else "wb") as output:
                            for chunk in iter(lambda: response.read(4 * 1024 * 1024), b""):
                                output.write(chunk)
                                received += len(chunk)
                        print(f"{name}: {partial.stat().st_size}/{total or declared} 字节", flush=True)
                        if not total and (not declared or received == declared):
                            break
                except (OSError, urllib.error.URLError) as error:
                    print(f"下载中断，保留部分文件：{error}", flush=True)
                    time.sleep(2)
            else:
                raise ValueError("下载重试耗尽，可再次执行脚本续传")
            checksum = digest(partial)
            if expected.get("bytes") and partial.stat().st_size != expected["bytes"]:
                raise ValueError(f"{name}大小不匹配，保留.part用于检查")
            if expected.get("sha256") and checksum != expected["sha256"]:
                raise ValueError(f"{name}哈希不匹配，保留.part用于检查")
            partial.rename(target)  # 完整校验通过才将临时文件转为正式文件
        manifest[name] = {"bytes": target.stat().st_size, "sha256": checksum}
    # 启动脚本检查此清单，表示下载和校验流程已完成
    (args.model_dir / "download-manifest.json").write_text(
        json.dumps({"source": config["source"], "repository": config["repository"],
                    "revision": config["revision"], "files": manifest}, indent=2) + "\n")
    print(f"模型准备完成：{args.model_dir}", flush=True)


if __name__ == "__main__":
    main()
