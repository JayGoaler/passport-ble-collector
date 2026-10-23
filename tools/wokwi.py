#!/usr/bin/env python3
"""独立 Wokwi 构建及仿真。Token 仅传给 CLI 子进程，不写入场景或日志。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SIM = ROOT / 'sim/wokwi'
BUILD = ROOT / 'build/wokwi'


def token_from_file(path):
    text = path.read_text(encoding='utf-8').strip()
    if text.startswith('export '):
        text = text[7:].strip()
    if text.startswith('WOKWI_CLI_TOKEN='):
        text = text.split('=', 1)[1].strip()
    parts = shlex.split(text, comments=True)
    if len(parts) != 1 or not re.fullmatch(r'wok_[A-Za-z0-9_-]{40}', parts[0]):
        raise ValueError('Token 文件须为单个 Token 或 WOKWI_CLI_TOKEN 赋值；不会执行文件中的命令')
    return parts[0]


def cli_path(explicit):
    workspace = Path(os.environ.get('PASSPORT_WORKSPACE', ROOT.parents[1]))
    path = explicit or shutil.which('wokwi-cli') or str(workspace / '.toolchains/wokwi/v0.29.0/wokwi-cli')
    if not Path(path).is_file():
        raise ValueError('找不到 Wokwi CLI；请用 --cli 指定独立安装的可执行文件')
    return str(Path(path).resolve())


def source_manifest():
    files = []
    suffixes = {'.c', '.h', '.cmake', '.json', '.toml', '.yml'}
    for directory in ['main', 'components', 'sim/wokwi', 'assets/fonts']:
        files += [p for p in (ROOT / directory).rglob('*') if p.is_file() and
                  (p.suffix in suffixes or p.name in {'CMakeLists.txt', 'Kconfig', 'Kconfig.projbuild', 'sdkconfig.defaults'})]
    files += [ROOT / name for name in ['CMakeLists.txt', 'sdkconfig.defaults', 'partitions.csv',
              'dependencies.lock', 'tools/wokwi.py', 'tools/verify_firmware.py']]
    return {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(set(files))}


def build():
    BUILD.mkdir(parents=True, exist_ok=True)
    inputs = source_manifest()
    # 只生成本仿真目录内的 SDK 配置，不消费或覆盖根目录 sdkconfig。
    config = BUILD / 'sdkconfig'
    stamp = BUILD / 'defaults.sha256'
    defaults = [ROOT / 'sdkconfig.defaults', SIM / 'sdkconfig.defaults']
    digest = hashlib.sha256(b''.join(p.read_bytes() for p in defaults)).hexdigest()
    if not stamp.exists() or stamp.read_text() != digest:
        config.unlink(missing_ok=True)
    command = [str(ROOT / 'passport'), 'run', 'idf.py', '-B', str(BUILD),
               '-D', 'SDKCONFIG=' + str(config), '-D',
               'SDKCONFIG_DEFAULTS=' + ';'.join(map(str, defaults))]
    subprocess.run(command + ['build'], cwd=ROOT, check=True)
    subprocess.run(command + ['merge-bin', '-o', str(BUILD / 'FoloToy-AI-Passport-full.bin')],
                   cwd=ROOT, check=True)
    subprocess.run([sys.executable, str(ROOT / 'tools/verify_firmware.py'), str(BUILD)], check=True)
    config_text = config.read_text()
    if 'CONFIG_COLLECTOR_WOKWI=y' not in config_text or 'CONFIG_BT_ENABLED=y' in config_text:
        raise ValueError('仿真构建配置不正确')
    commands = json.loads((BUILD / 'compile_commands.json').read_text())
    sources = {Path(c['file']).resolve() for c in commands}
    if SIM / 'collector_scan.c' not in sources or ROOT / 'main/collector_scan.c' in sources:
        raise ValueError('扫描源文件隔离失败')
    if SIM / 'collector_network.c' not in sources or ROOT / 'main/collector_network.c' in sources:
        raise ValueError('网络源文件隔离失败')
    for driver in ['bsp_display.c', 'bsp_button.c']:
        if SIM / driver not in sources or ROOT / 'components/bsp/src' / driver in sources:
            raise ValueError('BSP 源文件隔离失败: ' + driver)
    if inputs != source_manifest():
        raise ValueError('构建期间源码改变，请重新 build')
    stamp.write_text(digest)
    manifest = {'variant': 'WOKWI_ONLY_DO_NOT_FLASH', 'bluetooth': 'synthetic',
                'display': 'ILI9341', 'buttons': 'GPIO4/5/6', 'network': 'in-firmware fixture, no WiFi/TLS',
                'sources': inputs, 'files': {}}
    for name in ['FoloToy-AI-Passport-full.bin', 'FoloToy-AI-Passport.elf',
                 'FoloToy-AI-Passport.bin', 'flasher_args.json', 'sdkconfig',
                 'bootloader/bootloader.bin', 'partition_table/partition-table.bin']:
        manifest['files'][name] = hashlib.sha256((BUILD / name).read_bytes()).hexdigest()
    (BUILD / 'simulation-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Wokwi build: PASS; build/wokwi/simulation-manifest.json; DO NOT FLASH')


def scenario(output, credentials=None):
    steps = [{'wait-serial': 'WOKWI_READY'}]
    if credentials is None:
        steps.append({'delay': '2s'})

    def screenshot(name):
        # 控制器标记早于 UI 消费；覆盖 port 最长500ms休眠和分块 SPI 刷新。
        steps.append({'delay': '1s'})
        steps.append({'take-screenshot': {'part-id': 'lcd', 'save-to': name + '.png'}})

    def press(part, duration='100ms'):
        steps.extend([{'set-control': {'part-id': part, 'name': 'pressed', 'value': 1}},
                      {'delay': duration},
                      {'set-control': {'part-id': part, 'name': 'pressed', 'value': 0}},
                      {'delay': '400ms'}])

    def wait(text):
        steps.append({'wait-serial': text})

    screenshot('01-startup')
    # flow 尽早配置，让30秒周期心跳与后续UI操作并行；空数据页由 smoke 覆盖。
    if credentials is None:
        press('down')
        wait('WOKWI_VIEW page=1')
        screenshot('02-devices-empty')
        press('down')
        wait('WOKWI_VIEW page=2')
        screenshot('03-diagnostics')
    press('ok', '700ms')
    wait('WOKWI_VIEW page=3 menu=0')
    if credentials is None:
        screenshot('04-menu')
    press('down')
    wait('WOKWI_VIEW page=3 menu=1')
    press('ok')
    wait('WOKWI_VIEW page=3 menu=1 usb=1')
    screenshot('05-usb-window')
    if credentials is not None:
        payload = (json.dumps(credentials, ensure_ascii=True, separators=(',', ':')) + '\n').encode()
        # USB RX 有界缓冲；模拟串口分段传输，保留真实 USB 配置解析和物理窗口。
        for offset in range(0, len(payload), 64):
            steps.extend([{'write-serial': list(payload[offset:offset + 64])}, {'delay': '20ms'}])
        # 即时回执可能在 write-serial 返回前到达；等待每秒重复的稳定状态，
        # 再在整份串口记录中核对接收回执、配置版本和周期心跳。
        wait('WOKWI_VIEW page=3 menu=1 usb=0 wifi=1 clock=1 revision=1')
        wait('WOKWI_HTTP kind=2 ack=1')
        press('ok', '700ms')
        wait('WOKWI_VIEW page=0')
        screenshot('06-uploaded')
        press('down')
        wait('WOKWI_VIEW page=1')
        screenshot('07-devices')
        press('ok')
        steps.append({'delay': '1s'})
        screenshot('08-next-device')
        press('down')
        wait('WOKWI_VIEW page=2')
        screenshot('09-running-diagnostics')
        press('ok', '700ms')
        wait('WOKWI_VIEW page=3 menu=1')
        press('up')
        wait('WOKWI_VIEW page=3 menu=0')
        press('ok')
        wait('devices=3 pause=1')
        screenshot('10-paused')
        press('ok')
        wait('devices=3 pause=0')
        wait('WOKWI_HTTP kind=2 ack=1')
        # 此时仍在首次配置后的30秒内；直接等第二次心跳，避免额外固定空等。
        # 最终另核控制器实际接受了至少两次ACK，不能仅凭夹具发送日志判通过。
        wait('WOKWI_FIXTURE_HEARTBEAT count=2')
        press('ok', '700ms')
        wait('WOKWI_VIEW page=0')
        screenshot('11-resumed')
    return {'name': 'Passport synthetic BLE ' + ('in-firmware fixture flow' if credentials else 'startup and UI'),
            'version': 1, 'steps': steps}


def simulate(args):
    cli = cli_path(args.cli)
    if not (BUILD / 'simulation-manifest.json').is_file():
        raise ValueError('先执行 python3 tools/wokwi.py build')
    manifest = json.loads((BUILD / 'simulation-manifest.json').read_text())
    if manifest.get('sources') != source_manifest():
        raise ValueError('源码或仿真配置已经改变，请重新 build')
    for name, digest in manifest['files'].items():
        if hashlib.sha256((BUILD / name).read_bytes()).hexdigest() != digest:
            raise ValueError('仿真构建产物已改变，请重新 build')
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        raise ValueError('证据目录非空，请选择新的 --output，避免覆盖先前结果')
    output.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    if args.token_file:
        env['WOKWI_CLI_TOKEN'] = token_from_file(args.token_file)
    if not re.fullmatch(r'wok_[A-Za-z0-9_-]{40}', env.get('WOKWI_CLI_TOKEN', '')):
        raise ValueError('需要有效的 WOKWI_CLI_TOKEN 或 --token-file（不会打印内容）')
    credentials = None
    if args.command == 'flow':
        credentials = json.loads((SIM / 'fixtures/config.json').read_text())
        # 固定合成配置，不能从外部文件带入生产凭据；该 CA 占位符不用于 TLS。
        if (set(credentials) != {'ssid', 'password', 'base_url', 'node_id', 'token', 'ca'} or
                any(not isinstance(value, str) for value in credentials.values()) or
                credentials['ssid'] != 'Wokwi-GUEST' or credentials['password'] != '' or
                credentials['node_id'] != 'Wokwi' or credentials['token'] != 'W' * 43 or
                credentials['base_url'] != 'https://fixture.wokwi.invalid' or
                credentials['ca'] != '-----BEGIN CERTIFICATE-----\nSYNTHETIC-NOT-A-CERT\n-----END CERTIFICATE-----\n'):
            raise ValueError('仅接受仓库固定合成配置')
    scene = output / 'scenario.yaml'
    with open(scene, 'x', opener=lambda path, flags: os.open(path, flags, 0o600)) as scene_file:
        scene_file.write(json.dumps(scenario(output, credentials), indent=2) + '\n')
    command = [cli, str(SIM), '--scenario', str(scene), '--timeout', str(args.timeout),
               '--serial-log-file', str(output / 'serial.log'), '--fail-text', 'Guru Meditation',
               '--heap-interval', '1000', '--heap-min-free', '20000', '--heap-csv', str(output / 'heap.csv')]
    try:
        with (output / 'cli.log').open('w') as log:
            result = subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
    finally:
        scene.unlink(missing_ok=True)
    # 仅保留不含串口载荷的步骤摘要；Token 从未进入场景。
    safe_scene = scenario(output, credentials)
    for step in safe_scene['steps']:
        if 'write-serial' in step:
            step['write-serial'] = '<synthetic configuration chunk omitted>'
    (output / 'scenario-summary.json').write_text(json.dumps(safe_scene, indent=2) + '\n')
    serial = (output / 'serial.log').read_text(errors='replace') if (output / 'serial.log').exists() else ''
    required = ['WOKWI_READY', 'WOKWI_VIEW page=3 menu=1 usb=1']
    if credentials:
        required += ['RECEIVED_RAM_ONLY', 'AUTH_CONFIG_APPLIED', 'WOKWI_HTTP kind=1 ack=1', 'WOKWI_HTTP kind=2 ack=1',
                     'devices=3 pause=1', 'devices=3 pause=0']
    missing = [marker for marker in required if marker not in serial]
    flow_sequence_errors = []
    if credentials:
        # 暂停前的 pause=0 和上传 ACK 不能证明恢复；只接受按时序出现的证据。
        paused_marker = 'devices=3 pause=1'
        resumed_marker = 'devices=3 pause=0'
        paused = serial.find(paused_marker)
        resumed = serial.find(resumed_marker, paused + len(paused_marker)) if paused >= 0 else -1
        uploaded = serial.find('WOKWI_HTTP kind=2 ack=1', resumed + len(resumed_marker)) if resumed >= 0 else -1
        if paused < 0:
            flow_sequence_errors.append('PAUSE_MISSING: 未记录暂停状态')
        elif resumed < 0:
            flow_sequence_errors.append('RESUME_AFTER_PAUSE_MISSING: 暂停后未记录恢复状态')
        elif uploaded < 0:
            flow_sequence_errors.append('UPLOAD_ACK_AFTER_RESUME_MISSING: 恢复后未记录上传成功 ACK')
    expected = [step['take-screenshot']['save-to'] for step in safe_scene['steps'] if 'take-screenshot' in step]
    shots = [output / name for name in expected]
    bad_shots = [p.name for p in shots if not p.is_file() or p.stat().st_size < 100 or
                 p.read_bytes()[:8] != b'\x89PNG\r\n\x1a\n']
    faults = [marker for marker in ['Guru Meditation', 'assert failed', 'Stack canary',
              'stack overflow', 'abort() was called', 'UI_INIT_FAILED', 'TASK_ALLOCATION',
              'QUEUE_ALLOCATION', 'CONFIG_APPLY_FAILED'] if marker.lower() in serial.lower()]
    heartbeats = serial.count('WOKWI_HTTP kind=1 ack=1')
    uploads = serial.count('WOKWI_HTTP kind=2 ack=1')
    passed = (result.returncode == 0 and not missing and not flow_sequence_errors and not bad_shots and not faults and
              (not credentials or (heartbeats >= 2 and uploads >= 2)))
    report = {'simulation': 'PASS' if passed else 'FAIL', 'cli_exit': result.returncode,
              'missing_serial_markers': missing, 'screenshots': len(shots) - len(bad_shots),
              'flow_sequence_errors': flow_sequence_errors,
              'bad_screenshots': bad_shots, 'runtime_faults': faults,
              'heartbeat_acks': heartbeats, 'upload_acks': uploads, 'https_integration': 'NOT RUN',
              'firmware': manifest, 'device_tests': 'NOT RUN'}
    (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ['simulation', 'cli_exit', 'missing_serial_markers', 'flow_sequence_errors', 'screenshots']}))
    if not passed:
        raise ValueError('Wokwi 场景未通过；查看 ' + str(output / 'cli.log'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['build', 'lint', 'smoke', 'flow'])
    parser.add_argument('--cli')
    parser.add_argument('--token-file', type=Path)
    parser.add_argument('--output', type=Path, default=ROOT / 'logs/wokwi/run')
    parser.add_argument('--timeout', type=int, default=150000)
    args = parser.parse_args()
    try:
        if args.command == 'build':
            build()
        elif args.command == 'lint':
            subprocess.run([cli_path(args.cli), 'lint', str(SIM)], check=True)
        else:
            simulate(args)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
