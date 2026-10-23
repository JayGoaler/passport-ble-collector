#!/usr/bin/env python3
"""运行真实 collector_usb.c 和凭据解析器，注入USB/队列/窗口竞争。"""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path)
    parser.add_argument("--sanitize", action="store_true", help="启用 ASan/UBSan")
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    sdk = args.idf_path or (Path(os.environ["IDF_PATH"]) if os.environ.get("IDF_PATH") else
                            project.parents[1] / ".toolchains/esp-idf-v5.5.3")
    cjson = sdk / "components/json/cJSON"
    if not (cjson / "cJSON.c").is_file():
        parser.error("需要真实 ESP-IDF 5.5.3 cJSON，请激活环境或指定 --idf-path")
    with tempfile.TemporaryDirectory(prefix="passport-usb-host-") as temp:
        common = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
            f"-I{project / 'tests/collector_usb_stubs'}", f"-I{project / 'main'}", f"-I{cjson}",
        ]
        if args.sanitize:
            common += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        objects = []
        for name, source, options in [
            ("cJSON", cjson / "cJSON.c", ["-Wno-deprecated-declarations"] if sys.platform == "darwin" else []),
            ("core", project / "main/collector_core.c", ["-Dcol_parse_credentials=col_parse_credentials_real"]),
        ]:
            target = Path(temp) / f"{name}.o"
            subprocess.run(common + options + ["-c", str(source), "-o", str(target)], check=True)
            objects.append(str(target))
        executable = Path(temp) / ("usb-tests.exe" if os.name == "nt" else "usb-tests")
        subprocess.run(common + [str(project / "tests/test_collector_usb.c"), *objects, "-lm", "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
