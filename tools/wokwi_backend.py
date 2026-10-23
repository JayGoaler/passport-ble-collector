#!/usr/bin/env python3
"""启动仅监听回环地址的 Wokwi HTTPS 后端，或验收其无秘密摘要。"""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]


def runtime(node=None, openssl=None):
    env = os.environ.copy()
    for key in ('NODE_OPTIONS', 'NODE_PATH', 'WOKWI_CLI_TOKEN'):
        env.pop(key, None)
    env['NODE_ENV'] = 'test'
    candidates = [node] if node else [shutil.which('node'), *map(str, sorted(
        (Path.home() / '.nvm/versions/node').glob('v24.*/bin/node'), reverse=True))]
    selected = None
    for candidate in filter(None, candidates):
        try:
            version = subprocess.run([candidate, '-p', 'process.versions.node.split(".")[0]'],
                                     env=env, text=True, capture_output=True, check=True)
            if version.stdout.strip() == '24':
                selected = candidate
                break
        except (OSError, subprocess.CalledProcessError):
            continue
    ssl = openssl or ('/opt/homebrew/bin/openssl' if Path('/opt/homebrew/bin/openssl').is_file()
                      else shutil.which('openssl'))
    if not selected or not ssl:
        raise ValueError('需要 Node 24 与支持 req -addext 的 OpenSSL；可显式指定，不会自动安装')
    return selected, ssl, env


def check_report(path, ready_only=False):
    report = json.loads(Path(path).read_text(encoding='utf-8'))
    if report.get('schema') != 'wokwi-backend-report-v1' or report.get('backend') != 'personal-tools-real-handler':
        raise ValueError('摘要类型不匹配')
    checks = {'started': report.get('started') is True}
    if ready_only:
        checks['ready'] = report.get('state') == 'ready'
    else:
        counters = report.get('counters', {})
        for name in ('config_ok', 'heartbeat_ok', 'observation_ack_ok', 'accepted_events', 'stored_events'):
            value = counters.get(name)
            checks[name] = type(value) is int and value >= 1
        checks['no_internal_errors'] = counters.get('internal_errors') == 0
    return all(checks.values()), checks


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend', type=Path, help='personal-tools 源码目录，须已安装锁定依赖')
    parser.add_argument('--fixture-dir', type=Path, default=ROOT / 'build/wokwi-fixture',
                        help='须为项目 build 下尚不存在的目录，包含临时证书、凭据及隔离数据库')
    parser.add_argument('--port', type=int, default=8443)
    parser.add_argument('--node')
    parser.add_argument('--openssl')
    parser.add_argument('--assert-report', type=Path, help='只读取已有摘要并返回验收退出码')
    parser.add_argument('--ready-only', action='store_true')
    parser.add_argument('--wait-seconds', type=float, default=0)
    args = parser.parse_args(argv)
    if args.wait_seconds < 0 or args.wait_seconds > 600:
        parser.error('--wait-seconds 须在 0 至 600 秒之间')
    if args.assert_report:
        deadline = time.monotonic() + args.wait_seconds
        while True:
            try:
                ok, checks = check_report(args.assert_report, args.ready_only)
            except (OSError, ValueError, TypeError):
                ok, checks = False, {'readable_report': False}
            if ok or time.monotonic() >= deadline:
                print(json.dumps({'result': 'PASS' if ok else 'NOT_READY', 'checks': checks}, sort_keys=True))
                return 0 if ok else 1
            time.sleep(0.2)
    if not args.backend or not 0 <= args.port <= 65535:
        parser.error('启动需要 --backend，端口须在 0 至 65535（0 表示临时端口）')
    backend = args.backend.resolve()
    loader = backend / 'node_modules/tsx/dist/loader.mjs'
    if not loader.is_file() or not (backend / 'src/features/bluetooth-explorer/server/handler.ts').is_file():
        parser.error('指定目录缺少实际后端或已安装的 tsx；不会安装或改写后端')
    fixture = args.fixture_dir.absolute()
    build = ROOT / 'build'
    if fixture.is_symlink() or build.is_symlink() or not fixture.resolve().is_relative_to(build.resolve()):
        parser.error('fixture-dir 必须位于本项目被忽略的 build 目录内，不能是符号链接')
    if fixture.exists():
        parser.error('fixture-dir 已存在；选择新的 build 子目录，避免覆盖已有证据和凭据')
    try:
        node, openssl, env = runtime(args.node, args.openssl)
        env['TSX_TSCONFIG_PATH'] = str(backend / 'tsconfig.json')
        # 固定目录和启动参数中没有 Token；节点凭据由后端注册后仅写私有文件。
        child = subprocess.Popen([node, '--import', str(loader), str(ROOT / 'tools/wokwi_backend.mjs'),
                                  str(backend), str(fixture), str(args.port), openssl], cwd=backend, env=env)
        signal.signal(signal.SIGTERM, lambda _signum, _frame: child.terminate())
        try:
            return child.wait()
        except KeyboardInterrupt:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
            return 130
    except (OSError, ValueError):
        print('WOKWI_BACKEND_START_FAILED（检查运行时、目录和回环端口）', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
