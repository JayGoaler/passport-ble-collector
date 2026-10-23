#!/usr/bin/env python3
"""使用真实 LVGL 9.5.0 在主机上检查采集器字体、标签布局及三键回调。"""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zlib
from generate_collector_font import ROOT, fixed_literals, lvgl_root


def png_from_ppm(path):
    data = path.read_bytes().split(b"\n", 3)[3]
    def chunk(kind, value):
        return struct.pack(">I", len(value)) + kind + value + struct.pack(">I", zlib.crc32(kind + value))
    raw = b"".join(b"\0" + data[y*720:(y+1)*720] for y in range(320))
    path.with_suffix(".png").write_bytes(b"\x89PNG\r\n\x1a\n" +
        chunk(b"IHDR", struct.pack(">IIBBBBB", 240, 320, 8, 2, 0, 0, 0)) +
        chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
    path.unlink()


def run(command, timeout=None):
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    if result.returncode:
        print("\n".join(result.stdout.splitlines()[-80:]))
        raise SystemExit(result.returncode)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lvgl-root")
    parser.add_argument("--preview-dir", type=Path)
    parser.add_argument("--heap-kib", type=int, choices=(12, 16, 24), default=24, help="主机 LVGL 内置池容量，默认 24 KiB")
    args = parser.parse_args()
    source = lvgl_root(args.lvgl_root)
    if source is None:
        parser.error("未找到 LVGL 依赖；先获取锁定依赖或指定 --lvgl-root")
    cmake = shutil.which("cmake")
    if not cmake:
        choices = list((ROOT.parent.parent / ".toolchains/espressif/tools/cmake").glob("*/CMake.app/Contents/bin/cmake"))
        cmake = str(choices[0]) if choices else None
    if not cmake:
        parser.error("需要主机 C 编译器与 CMake；这不影响已有镜像刷机")
    # 带真实未知码点的负例会在 C 测试中单独验证，不加入固定字库。
    texts = fixed_literals()
    with tempfile.TemporaryDirectory(prefix="passport-ui-") as temp:
        build = Path(temp)
        (build / "collector_fixed_text.h").write_text("static const char *fixed_text[] = {\n" +
            ",\n".join(json.dumps(s, ensure_ascii=False) for s in texts) + "\n};\n")
        run([cmake, "-S", str(ROOT / "tests/collector_ui"), "-B", temp, f"-DLVGL_SOURCE={source}",
             f"-DCOLLECTOR_UI_HEAP_KIB={args.heap_kib}"])
        run([cmake, "--build", temp, "--config", "Debug", "--parallel", str(min(os.cpu_count() or 2, 8))])
        executable = build / ("collector_ui_host.exe" if os.name == "nt" else "collector_ui_host")
        if not executable.exists():
            executable = build / "Debug" / executable.name
        command = [str(executable)]
        if args.preview_dir:
            args.preview_dir.mkdir(parents=True, exist_ok=True)
            command.append(str(args.preview_dir.resolve()))
        print(run(command, timeout=30), end="")
    if args.preview_dir:
        for ppm in args.preview_dir.glob("page-*.ppm"):
            png_from_ppm(ppm)
        print(f"主机渲染预览：{args.preview_dir}")


if __name__ == "__main__":
    main()
