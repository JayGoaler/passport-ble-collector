#!/usr/bin/env python3
"""编译真实 collector_core 和 ESP-IDF cJSON，运行合成主机测试。"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import time


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path, help="ESP-IDF 源码目录；默认 IDF_PATH")
    parser.add_argument("--emit-fixtures", type=Path, help="输出真实 C 序列化生成的隔离后端测试报文")
    parser.add_argument("--now-ms", type=int, default=None, help="fixture 的 UTC 毫秒；默认当前时间")
    parser.add_argument("--sanitize", action="store_true", help="启用 AddressSanitizer 和 UndefinedBehaviorSanitizer")
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    idf_path = args.idf_path or (Path(os.environ["IDF_PATH"]) if os.environ.get("IDF_PATH") else None)
    if idf_path is None:
        candidate = project.parents[1] / ".toolchains" / "esp-idf-v5.5.3"
        if candidate.is_dir():
            idf_path = candidate
    if idf_path is None:
        parser.error("请先激活 ESP-IDF 5.5.3 或提供 --idf-path；不使用 cJSON 测试替身")
    cjson = idf_path / "components" / "json" / "cJSON"
    if not (cjson / "cJSON.c").is_file():
        parser.error(f"未找到实际 ESP-IDF cJSON 源码: {cjson}")
    with tempfile.TemporaryDirectory(prefix="passport-collector-host-") as directory:
        executable = Path(directory) / ("collector-tests.exe" if os.name == "nt" else "collector-tests")
        common = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
            f"-I{project / 'main'}", f"-I{cjson}",
        ]
        if args.sanitize:
            common += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        # Apple SDK 标注上游 sprintf 为 deprecated；仅对原版 cJSON 屏蔽该告警。
        cjson_flags = ["-Wno-deprecated-declarations"] if sys.platform == "darwin" else []
        cjson_object = Path(directory) / "cJSON.o"
        subprocess.run(common + cjson_flags + ["-c", str(cjson / "cJSON.c"), "-o", str(cjson_object)], check=True)
        command = common + [
            str(project / "tests" / "collector" / "test_core.c"),
            str(project / "main" / "collector_core.c"), str(cjson_object),
            "-lm", "-o", str(executable),
        ]
        subprocess.run(command, check=True)
        run = [str(executable)]
        if args.emit_fixtures:
            args.emit_fixtures.mkdir(parents=True, exist_ok=True)
            now = args.now_ms if args.now_ms is not None else time.time_ns() // 1_000_000
            run += [str(args.emit_fixtures.resolve()), str(now)]
        subprocess.run(run, check=True)
        controller = Path(directory) / ("collector-controller.exe" if os.name == "nt" else "collector-controller")
        subprocess.run(common + [
            f"-I{project / 'tests' / 'collector_runtime_stubs'}",
            str(project / "tests" / "test_collector_controller.c"),
            str(project / "main" / "collector_core.c"), str(cjson_object),
            "-lm", "-o", str(controller),
        ], check=True)
        subprocess.run([str(controller)], check=True)


if __name__ == "__main__":
    main()
