#!/usr/bin/env python3
"""使用指定后端的现有 Node/tsx 与临时数据库运行 C 报文联调，不读取生产配置。"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--backend', required=True, type=Path)
p.add_argument('--node', help='Node 24 可执行文件；默认 PATH，其次本机 NVM 安装')
p.add_argument('--openssl', help='OpenSSL 可执行文件；默认 PATH 或 macOS Homebrew 安装')
a = p.parse_args()
backend = a.backend.resolve()
loader = backend / 'node_modules/tsx/dist/loader.mjs'
if not loader.is_file():
    p.error('指定后端须已安装其锁定依赖；不会自动改写后端')
env = os.environ.copy()
# 防止调用者的预加载参数隐式读取 dotenv/生产配置；本工具只运行明确的 tsx loader。
env.pop('NODE_OPTIONS', None)
env['NODE_ENV'] = 'test'
node_candidates = [a.node] if a.node else [shutil.which('node'), *map(str, sorted(
    (Path.home() / '.nvm/versions/node').glob('v24.*/bin/node'), reverse=True))]
node = next((candidate for candidate in node_candidates if candidate and
             subprocess.run([candidate, '-p', 'process.versions.node.split(".")[0]'],
                            env=env, text=True, capture_output=True, check=True).stdout.strip() == '24'), None)
if not node:
    p.error('需要 Node 24；可通过 --node 指定，不会安装或修改全局环境')
openssl = a.openssl or ('/opt/homebrew/bin/openssl' if Path('/opt/homebrew/bin/openssl').is_file()
                         else shutil.which('openssl'))
if not openssl:
    p.error('需要支持 req -addext 的 OpenSSL，可通过 --openssl 指定')
with tempfile.TemporaryDirectory(prefix='passport-c-fixtures-') as tmp:
    subprocess.run([os.sys.executable, str(root / 'tools/test_collector.py'), '--emit-fixtures', tmp], check=True)
    env['TSX_TSCONFIG_PATH'] = str(backend / 'tsconfig.json')
    subprocess.run([node, '--import', str(loader), str(root / 'tools/test_collector_backend.mjs'),
                    str(backend), tmp, openssl], cwd=backend, env=env, check=True)
