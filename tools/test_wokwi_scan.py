#!/usr/bin/env python3
"""复用真实 collector_core/cJSON，验证 Wokwi 合成扫描和默认构建保护。"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path, help="ESP-IDF 源码目录；默认 IDF_PATH 或工作区工具链")
    parser.add_argument("--sanitize", action="store_true", help="启用 AddressSanitizer 和 UndefinedBehaviorSanitizer")
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    idf = args.idf_path or Path(os.environ.get("IDF_PATH", project.parents[1] / ".toolchains" / "esp-idf-v5.5.3"))
    cjson = idf / "components" / "json" / "cJSON"
    if not (cjson / "cJSON.c").is_file():
        parser.error(f"未找到实际 ESP-IDF cJSON 源码: {cjson}")
    with tempfile.TemporaryDirectory(prefix="passport-wokwi-scan-") as directory:
        temporary = Path(directory)
        config = temporary / "sdkconfig.h"
        config.write_text("#define CONFIG_COLLECTOR_WOKWI 1\n", encoding="utf-8")
        common = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
            f"-I{temporary}", f"-I{project / 'main'}", f"-I{cjson}",
            f"-I{project / 'tests' / 'collector_runtime_stubs'}",
        ]
        if args.sanitize:
            common += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        upstream_flags = ["-Wno-deprecated-declarations"] if sys.platform == "darwin" else []
        cjson_object = temporary / "cJSON.o"
        subprocess.run(common + upstream_flags + ["-c", str(cjson / "cJSON.c"), "-o", str(cjson_object)], check=True)
        executable = temporary / ("wokwi-scan.exe" if os.name == "nt" else "wokwi-scan")
        test = project / "tests" / "wokwi_scan_runtime_test.c"
        subprocess.run(common + [str(test), str(project / "main" / "collector_core.c"), str(cjson_object),
                                 "-lm", "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
        for configuration in ("", "#define CONFIG_COLLECTOR_WOKWI 0\n"):
            config.write_text(configuration, encoding="utf-8")
            result = subprocess.run(common + ["-fsyntax-only", str(test)], capture_output=True, text=True)
            if result.returncode == 0 or "Wokwi synthetic scanner must only be compiled" not in result.stderr:
                raise RuntimeError("Wokwi 扫描实现缺少有效的默认/关闭配置编译保护")
        print("wokwi scan: CONFIG_COLLECTOR_WOKWI absent/disabled compile guard PASS")


if __name__ == "__main__":
    main()
