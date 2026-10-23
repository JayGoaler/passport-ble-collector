#!/usr/bin/env python3
"""运行实际 Wokwi 网络应答 C 模块与真实核心/cJSON 的主机回归。"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    sdk = Path(os.environ.get('IDF_PATH', root.parents[1] / '.toolchains/esp-idf-v5.5.3'))
    cjson = sdk / 'components/json/cJSON'
    with tempfile.TemporaryDirectory(prefix='wokwi-fixture-host-') as tmp:
        temp = Path(tmp)
        (temp / 'sdkconfig.h').write_text('#define CONFIG_COLLECTOR_WOKWI 1\n')
        common = shlex.split(os.environ.get('CC', 'cc')) + [
            '-std=c11', '-Wall', '-Wextra', '-Werror', '-g', '-O1',
            f'-I{temp}', f'-I{root / "tests/collector_network_stubs"}',
            f'-I{root / "main"}', f'-I{cjson}',
        ]
        if args.sanitize:
            common += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        cjson_flags = ['-Wno-deprecated-declarations'] if sys.platform == 'darwin' else []
        obj = temp / 'cJSON.o'
        subprocess.run(common + cjson_flags + ['-c', str(cjson / 'cJSON.c'), '-o', str(obj)], check=True)
        exe = temp / ('fixture.exe' if os.name == 'nt' else 'fixture')
        subprocess.run(common + [str(root / 'tests/test_wokwi_fixture.c'), str(root / 'main/collector_core.c'),
                                str(obj), '-lm', '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
