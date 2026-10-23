#!/usr/bin/env python3
"""运行真实 collector_network.c 的主机故障注入；ESP/网络驱动为替身。"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path, help="ESP-IDF 5.5.3 源码目录，默认 IDF_PATH")
    parser.add_argument("--sanitize", action="store_true", help="启用 ASan/UBSan")
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    sdk = args.idf_path or (Path(os.environ["IDF_PATH"]) if os.environ.get("IDF_PATH") else None)
    if sdk is None:
        sdk = project.parents[1] / ".toolchains" / "esp-idf-v5.5.3"
    http = sdk / "components" / "http_parser"
    if not (http / "http_parser.c").is_file():
        parser.error("需真实 ESP-IDF http_parser.c，请先激活环境或提供 --idf-path")
    with tempfile.TemporaryDirectory(prefix="passport-network-host-") as temp:
        executable = Path(temp) / ("network-tests.exe" if os.name == "nt" else "network-tests")
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
            f"-I{project / 'tests' / 'collector_network_stubs'}", f"-I{project / 'main'}", f"-I{http}",
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        command += [str(project / "tests" / "test_collector_network.c"), str(http / "http_parser.c"), "-o", str(executable)]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
