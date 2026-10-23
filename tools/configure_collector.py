#!/usr/bin/env python3
"""通过有时限的 USB 配置窗口发送 RAM 临时凭据；不记录串口正文。"""
from __future__ import annotations

import argparse
import getpass
import importlib
import json
import os
from pathlib import Path
import re
import stat
import sys
import time
from urllib.parse import urlsplit
import warnings

FIELDS = {"ssid": 32, "password": 64, "base_url": 256,
          "node_id": 80, "token": 43, "ca": 4096}
MAX_FRAME = 8192  # 包括结尾换行，与设备 8192 字节行缓冲一致。
PREFIX = b"COLLECTOR "
ERRORS = {
    "CONFIG_LOCKED": "设备配置窗口未开启或已超时，请按键重新开启 120 秒窗口。",
    "INPUT_SIZE": "设备拒绝配置长度，请检查字段字节上限。",
    "INVALID_FIELDS": "设备拒绝配置字段，请核对工具与固件版本。",
    "BUSY": "设备尚在处理上次配置，请等待后重新开启配置窗口。",
}


class ConfigError(Exception):
    """仅包含工具生成的安全说明，禁止装入外部异常正文。"""


class SafeParser(argparse.ArgumentParser):
    def error(self, message):
        # argparse 默认会把错误的命令行内容回显，其中可能含误传的秘密。
        self.print_usage(sys.stderr)
        self.exit(2, "参数无效；运行 --help 查看用法。秘密不能作为命令行参数。\n")


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ConfigError("配置含重复字段。")
        result[key] = value
    return result


def validate_config(value):
    if not isinstance(value, dict) or set(value) != set(FIELDS):
        raise ConfigError("配置必须且只能包含 ssid、password、base_url、node_id、token、ca。")
    for field, limit in FIELDS.items():
        text = value[field]
        if not isinstance(text, str):
            raise ConfigError(f"字段 {field} 必须为字符串。")
        try:
            size = len(text.encode("utf-8"))
        except UnicodeError:
            raise ConfigError(f"字段 {field} 不是有效 UTF-8。") from None
        if size > limit or (not size and field != "password"):
            raise ConfigError(f"字段 {field} 长度无效，上限为 {limit} UTF-8 字节。")
        if "\0" in text or (field != "ca" and any(ord(c) < 32 for c in text)):
            raise ConfigError(f"字段 {field} 含不允许的控制字符。")
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,80}", value["node_id"]):
        raise ConfigError("字段 node_id 只能使用字母、数字、下划线、横线。")
    if not re.fullmatch(r"[A-Za-z0-9_-]{43}", value["token"]):
        raise ConfigError("字段 token 必须为 43 字符 base64url 文本。")
    url = value["base_url"]
    try:
        parsed = urlsplit(url)
        valid_url = (url.startswith("https://") and parsed.hostname
                     and not parsed.path and not parsed.query and not parsed.fragment
                     and parsed.username is None and parsed.password is None
                     and not any(c.isspace() or c in "\\?#@" for c in url)
                     and (parsed.port is None or 1 <= parsed.port <= 65535))
    except ValueError:
        valid_url = False
    if not valid_url:
        raise ConfigError("字段 base_url 必须为 https://主机[:端口]，不含路径、尾部斜线或认证信息。")
    if ("-----BEGIN CERTIFICATE-----" not in value["ca"]
            or "-----END CERTIFICATE-----" not in value["ca"]):
        raise ConfigError("字段 ca 必须含 PEM 证书；实际链与主机名由设备 TLS 验证。")
    frame = bytearray(json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))
    frame.append(10)
    if len(frame) > MAX_FRAME:
        frame[:] = b"\0" * len(frame)
        raise ConfigError("转义后的配置超过设备 USB 行缓冲上限。")
    return frame


def load_protected_file(path):
    # Windows st_mode 不代表 ACL；不假装检查成功，改用隐藏交互输入。
    if os.name != "posix":
        raise ConfigError("此平台未实现配置文件 ACL 验证，请使用 --ca-file 和隐藏交互输入。")
    fd = None
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid()
                or stat.S_IMODE(info.st_mode) & 0o077):
            raise ConfigError("配置文件必须由当前用户拥有、为普通文件且仅当前用户可读写（chmod 600）。")
        if info.st_size > MAX_FRAME:
            raise ConfigError("配置文件超过 8192 字节。")
        with os.fdopen(fd, "r", encoding="utf-8") as handle:
            fd = None
            raw = handle.read(MAX_FRAME + 1)
        if len(raw.encode("utf-8")) > MAX_FRAME:
            raise ConfigError("配置文件超过 8192 字节。")
        return json.loads(raw, object_pairs_hook=_object)
    except ConfigError:
        raise
    except (OSError, ValueError, UnicodeError, RecursionError):
        raise ConfigError("无法安全读取配置文件或 JSON 无效；未回显内容。") from None
    finally:
        if fd is not None:
            os.close(fd)


def interactive_config(ca_path):
    if not sys.stdin.isatty():
        raise ConfigError("隐藏输入需要交互终端，禁止通过管道传入秘密。")
    try:
        with Path(ca_path).open("r", encoding="utf-8") as handle:
            ca = handle.read(4097)
        if len(ca.encode("utf-8")) > FIELDS["ca"]:
            raise ConfigError("CA 文件超过 4096 UTF-8 字节。")
    except (OSError, UnicodeError):
        raise ConfigError("无法读取 CA 文件。") from None
    prompts = {
        "ssid": "Wi-Fi SSID（隐藏输入）：", "password": "Wi-Fi 密码（开放网络留空）：",
        "base_url": "HTTPS 基址（无尾部斜线）：", "node_id": "独立 Passport 节点 ID：",
        "token": "节点 Token（隐藏输入）：",
    }
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("error", getpass.GetPassWarning)
            config = {key: getpass.getpass(prompt) for key, prompt in prompts.items()}
    except (getpass.GetPassWarning, EOFError):
        raise ConfigError("终端无法保证隐藏输入，已停止；请使用正常交互终端。") from None
    config["ca"] = ca
    return config


def wait_received(port, timeout):
    deadline = time.monotonic() + timeout
    line = bytearray()
    overflow = False
    while time.monotonic() < deadline:
        for ch in port.read(256):
            if ch == 10:
                if not overflow and line.startswith(PREFIX):
                    try:
                        reply = json.loads(bytes(line[len(PREFIX):]))
                        status = reply.get("status") if isinstance(reply, dict) else None
                    except (ValueError, UnicodeError):
                        status = None
                    if status == "RECEIVED_RAM_ONLY":
                        return status
                    if isinstance(status, str) and status in ERRORS:
                        raise ConfigError(ERRORS[status])
                line.clear()
                overflow = False
            elif len(line) < 1024:
                line.append(ch)
            else:
                overflow = True
    raise ConfigError("未收到配置接收确认；未自动重发。请检查端口、窗口和设备界面后重试。")


def send_config(port_name, frame, timeout):
    port = None
    try:
        try:
            serial = importlib.import_module("serial")
        except ImportError:
            raise ConfigError("当前 Python 缺少 pyserial，请使用 ESP-IDF 5.5.3 的已激活 Python 环境。") from None
        # 打开前设置 DTR/RTS，避免工具主动复位；驱动/系统行为仍需实机验证。
        port = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=2)
        port.dtr = False
        port.rts = False
        port.port = port_name
        port.open()
        port.reset_input_buffer()
        sent = 0
        while sent < len(frame):
            written = port.write(memoryview(frame)[sent:])
            if not written or written > len(frame) - sent:
                raise ConfigError("USB 配置发送不完整，未自动重发。")
            sent += written
        return wait_received(port, timeout)
    except ConfigError:
        raise
    except Exception:
        # 串口驱动、端口名称及异常可能包含输入内容，永不转发原始异常。
        raise ConfigError("USB 通信失败；请确认实际端口及占用情况，未记录原始串口内容。") from None
    finally:
        frame[:] = b"\0" * len(frame)
        if port is not None:
            try:
                port.close()
            except Exception:
                pass


def main(argv=None):
    parser = SafeParser(description="Passport BLE 采集器 USB RAM 临时配置；不刷机、不记录秘密。")
    parser.add_argument("--port", required=True, help="实际串口，例如 COM5 或 /dev/cu.usbmodem...")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--ca-file", help="公开可信 CA PEM 文件；其余字段隐藏输入，支持 macOS/Windows")
    group.add_argument("--config-file", help="包含六个字段的 0600 JSON 文件；仅 POSIX 平台")
    parser.add_argument("--timeout", type=float, default=10, help="接收确认等待秒数，1–120，默认 10")
    args = parser.parse_args(argv)
    if not 1 <= args.timeout <= 120:
        parser.error("timeout")
    try:
        value = load_protected_file(args.config_file) if args.config_file else interactive_config(args.ca_file)
        frame = validate_config(value)
        value.clear()
        print("请先在设备操作菜单开启 USB 配置窗口（120 秒）；发送后窗口自动关闭。", flush=True)
        status = send_config(args.port, frame, args.timeout)
        print(status)
        print("设备仅接收 RAM 临时配置；尚不代表 Wi-Fi、校时或认证成功。重启后需重新配置。")
        return 0
    except ConfigError as exc:
        print(f"配置未完成：{exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("配置已取消；请查看设备状态。", file=sys.stderr)
        return 130
    except Exception:
        print("配置未完成：本机输入或通信发生错误，已隐藏原始异常内容。", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
