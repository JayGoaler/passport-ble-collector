#!/usr/bin/env python3
"""从锁定的 LVGL 素材生成采集器固定文案子集，不修改上游字体。"""
import argparse
import ast
import hashlib
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def fixed_literals():
    result = []
    token = re.compile(r'"(?:\\.|[^"\\])*"|/\*.*?\*/|//[^\n]*', re.S)
    for name in ("collector_ui.c", "collector_controller.c"):
        for match in token.finditer((ROOT / "main" / name).read_text()):
            if match[0].startswith('"'):
                result.append(ast.literal_eval(match[0]))
    return result


def lvgl_root(explicit=None):
    paths = [Path(explicit)] if explicit else [
        ROOT / "managed_components/lvgl__lvgl",
        ROOT.parent.parent / "firmware/managed_components/lvgl__lvgl",
    ]
    return next((path.resolve() for path in paths if (path / "lvgl.h").exists()), None)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--converter", default="lv_font_conv")
    parser.add_argument("--lvgl-root")
    args = parser.parse_args()
    lvgl = lvgl_root(args.lvgl_root)
    if not lvgl:
        parser.error("未找到 LVGL 9.5.0；先拉取固件依赖或指定 --lvgl-root")
    if subprocess.check_output([args.converter, "--version"], text=True).strip() != "1.5.3":
        parser.error("请使用 lv_font_conv 1.5.3")
    source = lvgl / "scripts/built_in_font/SourceHanSansSC-Normal.otf"
    target = ROOT / "assets/fonts"
    target.mkdir(parents=True, exist_ok=True)
    symbols = "".join(sorted({c for text in fixed_literals() for c in text if ord(c) > 126}))
    (target / "collector-symbols.txt").write_text(symbols + "\n")
    output = target / "collector_font_16.c"
    subprocess.run([args.converter, "--font", str(source), "--range", "0x20-0x7E",
                    "--symbols", symbols, "--size", "16", "--bpp", "4", "--format", "lvgl",
                    "--no-compress", "--no-kerning", "--lv-font-name", "collector_font_16",
                    "--lv-include", "lvgl.h", "--output", str(output)], check=True)
    # 转换器将输入的绝对路径写入注释；替换为可移植的再生命令。
    output.write_text(re.sub(r" \* Opts:.*", " * Rebuild: python3 tools/generate_collector_font.py (lv_font_conv 1.5.3)", output.read_text()))
    license_file = lvgl / "scripts/built_in_font/font_license/SourceHanSansSC/LICENSE.txt"
    (target / "SourceHanSansSC-LICENSE.txt").write_bytes(license_file.read_bytes())
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    (target / "collector-font-source.sha256").write_text(f"{digest}  SourceHanSansSC-Normal.otf\n")
    print(f"Generated ASCII 95 + {len(symbols)} CJK glyphs: {output.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
