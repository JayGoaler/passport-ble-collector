#!/usr/bin/env python3
"""打包当前未提交源码、已校验固件和指定证据；不提交、推送或访问设备。"""
from __future__ import annotations

import argparse
from datetime import datetime, timedelta, timezone
import hashlib
import io
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile
import zipfile

sys.dont_write_bytecode = True
from archive_firmware import inspect_build, safe_path
from verify_collector_handoff import MAX_FILES, MAX_FILE_BYTES, MAX_TOTAL_BYTES, safe_name, sha256_file, verify_handoff

EXCLUDED_DIRS = {".git", ".agents", ".cache", ".toolchains", "build", "managed_components",
                 "logs", "artifacts", "__pycache__", ".vscode", ".idea", ".aws", ".ssh"}
PRIVATE_SUFFIXES = (".private.json", ".pem", ".key", ".p12", ".pfx", ".pyc")
PRIVATE_NAMES = {"credentials.json", "secrets.json", "sdkconfig", "sdkconfig.old", ".DS_Store"}
SECRET_FIELDS = {"password", "token", "wifi_password", "access_token", "api_key", "private_key"}
# 只放行已审查的固定合成 USB 配置；不按目录、字段名或 Token 前缀宽泛豁免。
WOKWI_FIXTURE_NAME = "source/sim/wokwi/fixtures/config.json"
WOKWI_FIXTURE_SHA256 = "6f22f692e4a6ee9c6f6fb2af77b1e8c7a6c1df2c03c8762c9662553bf95cd38d"
SECRET_PATTERNS = (
    re.compile(rb"(?:ghp_|github_pat_)[A-Za-z0-9_]{20,}"),
    re.compile(rb"wok_[A-Za-z0-9_-]{40}(?![A-Za-z0-9_-])"),
    re.compile(rb"AKIA[0-9A-Z]{16}"),
    re.compile(rb"Authorization\s*:\s*Bearer\s+[A-Za-z0-9_-]{43}(?![A-Za-z0-9_-])", re.I),
    re.compile(rb'"(?:token|access_token)"\s*:\s*"[A-Za-z0-9_-]{43}"'),
    re.compile(rb"-----BEGIN (?:RSA |EC |OPENSSH |ENCRYPTED )?PRIVATE KEY-----\s+[A-Za-z0-9+/=\r\n]{32,}-----END (?:RSA |EC |OPENSSH |ENCRYPTED )?PRIVATE KEY-----"),
)


def excluded(name):
    parts = name.split("/")
    return (any(part in EXCLUDED_DIRS for part in parts[:-1]) or parts[-1] in PRIVATE_NAMES
            or parts[-1].startswith(".env") or parts[-1].endswith(PRIVATE_SUFFIXES))


def scan_content(name, data):
    if name == WOKWI_FIXTURE_NAME:
        if hashlib.sha256(data).hexdigest() != WOKWI_FIXTURE_SHA256:
            raise ValueError(f"Wokwi 合成配置与已审查内容不一致，拒绝打包：{name}")
        return
    if any(pattern.search(data) for pattern in SECRET_PATTERNS):
        raise ValueError(f"疑似凭据或私钥，拒绝打包且不回显内容：{name}")
    if name.endswith(".json"):
        try:
            value = json.loads(data)
        except (ValueError, UnicodeError):
            return
        def inspect(item):
            if isinstance(item, dict):
                for key, value in item.items():
                    if key.lower() in SECRET_FIELDS and isinstance(value, str) and value:
                        raise ValueError(f"JSON 含非空秘密字段，拒绝打包：{name}")
                    inspect(value)
            elif isinstance(item, list):
                for value in item:
                    inspect(value)
        inspect(value)


def git_output(source, *args):
    return subprocess.run(["git", "-C", str(source), *args], check=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout


def git_state(source):
    root = Path(os.fsdecode(git_output(source, "rev-parse", "--show-toplevel")).strip()).resolve()
    if root != source.resolve():
        raise ValueError("source 必须是独立 Git 仓库根目录")
    head = git_output(source, "rev-parse", "HEAD").decode("ascii").strip()
    branch = git_output(source, "branch", "--show-current").decode("utf-8").strip()
    status = git_output(source, "status", "--porcelain=v1", "-z", "--untracked-files=all").decode("utf-8").split("\0")
    return {"base_commit": head, "branch": branch or "DETACHED_HEAD", "dirty": any(status),
            "git_status_porcelain_v1_z": [entry for entry in status if entry]}


def source_names(source):
    raw = git_output(source, "ls-files", "--cached", "--others", "--exclude-standard", "-z")
    return sorted(set(name.decode("utf-8") for name in raw.split(b"\0") if name))


def file_bytes(path):
    path = safe_path(path)
    info = path.stat()
    if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_FILE_BYTES:
        raise ValueError("输入必须是大小受限的普通文件")
    return path.read_bytes(), 0o755 if info.st_mode & 0o111 else 0o644


def collect_directory(root, prefix):
    root = safe_path(root)
    if not root.is_dir():
        raise ValueError("证据目录不存在")
    result = {}
    for parent, directories, files in os.walk(root, followlinks=False):
        for directory in list(directories):
            child = Path(parent) / directory
            if directory in EXCLUDED_DIRS:
                directories.remove(directory)
            elif child.is_symlink():
                raise ValueError("证据目录包含符号链接")
        for filename in files:
            child = Path(parent) / filename
            relative = child.relative_to(root).as_posix()
            if excluded(relative):
                continue
            name = f"{prefix}/{relative}"
            data, mode = file_bytes(child)
            scan_content(name, data)
            result[name] = (data, mode)
    return result


def inspect_taskbook(path):
    path = safe_path(path)
    data, mode = file_bytes(path)
    if path.suffix.lower() != ".zip":
        raise ValueError("原始任务书必须是 ZIP 文件")
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        total = 0
        names = set()
        for info in archive.infolist():
            if info.is_dir():
                safe_name(info.filename.rstrip("/"))
                continue
            safe_name(info.filename)
            if info.filename in names or stat.S_IFMT(info.external_attr >> 16) not in (0, stat.S_IFREG):
                raise ValueError("原始任务书包含重复成员或符号链接")
            names.add(info.filename)
            total += info.file_size
            if info.file_size > MAX_FILE_BYTES or total > MAX_TOTAL_BYTES or len(names) > MAX_FILES:
                raise ValueError("原始任务书超过大小或文件数上限")
            if excluded(info.filename):
                raise ValueError("原始任务书包含私密/缓存文件，拒绝原样打包")
            scan_content(info.filename, archive.read(info))
    return data, mode


def create_package(source, firmware_dir, evidence_dir, taskbook, output):
    source, output = safe_path(source), safe_path(output)
    sidecar = output.with_suffix(output.suffix + ".sha256")
    if output.exists() or sidecar.exists():
        raise ValueError("交接 ZIP 或外置校验文件已存在，拒绝覆盖")
    before = git_state(source)
    payload, source_checks = {}, {}
    for relative in source_names(source):
        if excluded(relative):
            continue
        path = source / relative
        # 已删除 tracked 文件不属于当前源码快照；删除状态保留在 Git 元数据。
        if not path.exists() and not path.is_symlink():
            continue
        data, mode = file_bytes(path)
        name = f"source/{relative}"
        scan_content(name, data)
        payload[name] = (data, mode)
        source_checks[relative] = hashlib.sha256(data).hexdigest()
    if not payload:
        raise ValueError("未找到可恢复源码")
    build, artifacts = inspect_build(safe_path(firmware_dir))
    sdk_version = build["app_descriptor"]["idf_version"]
    if sdk_version != "v5.5.3":
        raise ValueError("固件必须由 ESP-IDF v5.5.3 构建")
    firmware_prefix = f"firmware/{build['full_bin_sha256']}"
    for name, data in artifacts.items():
        scan_content(name, data)
        payload[f"{firmware_prefix}/{name}"] = (data, 0o644)
    payload[f"{firmware_prefix}/manifest.json"] = ((json.dumps(build, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode(), 0o644)
    payload.update(collect_directory(evidence_dir, "evidence/logs"))
    taskbook = safe_path(taskbook)
    payload[f"evidence/taskbook/{taskbook.name}"] = inspect_taskbook(taskbook)
    files = {}
    for name, (data, mode) in payload.items():
        safe_name(name)
        files[name] = {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data), "mode": mode}
    manifest = {
        "schema_version": 1, "created_at": datetime.now(timezone(timedelta(hours=8))).isoformat(timespec="seconds"),
        **before, "sdk_version": sdk_version, "device_tests": "NOT_RUN",
        "firmware_archive": firmware_prefix, "full_bin_sha256": build["full_bin_sha256"],
        "app_elf_sha256": build["app_elf_sha256"],
        "source_selection": "git ls-files --cached --others --exclude-standard; current file bytes",
        "excluded_directories": sorted(EXCLUDED_DIRS), "excluded_names": sorted(PRIVATE_NAMES),
        "excluded_suffixes": list(PRIVATE_SUFFIXES),
        "secret_scan": "known credential/private-key patterns and nonempty JSON secret fields; not a universal secret detector",
        "source_transport": "uncommitted source snapshot; no .git; no commit or push performed",
        "files": files,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    published = False
    try:
        with tempfile.NamedTemporaryFile(prefix=f".{output.name}.", suffix=".tmp", dir=output.parent, delete=False) as temp:
            temporary = Path(temp.name)
        with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
            for name, (data, mode) in sorted(payload.items()):
                info = zipfile.ZipInfo(name)
                info.create_system = 3
                info.external_attr = (stat.S_IFREG | mode) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, data)
            info = zipfile.ZipInfo("manifest.json")
            info.create_system = 3
            info.external_attr = (stat.S_IFREG | 0o644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, (json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode())
        verified = verify_handoff(temporary)
        if git_state(source) != before:
            raise ValueError("打包期间 Git 状态发生变化，请停止编辑后重试")
        for relative, expected in source_checks.items():
            if sha256_file(safe_path(source / relative)) != expected:
                raise ValueError("打包期间源码内容发生变化，请重新运行验证后打包")
        os.link(temporary, output)  # 同目录原子发布，已存在时绝不覆盖。
        published = True
        with sidecar.open("x", encoding="utf-8") as handle:
            handle.write(f"{verified['zip_sha256']}  {output.name}\n")
        return output, verified
    except BaseException:
        if published and output.exists() and temporary is not None and os.path.samefile(temporary, output):
            output.unlink()
        raise
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1], help="独立Git源码仓库根目录")
    parser.add_argument("--firmware-dir", type=Path, required=True, help="最终已构建目录或哈希归档目录；需BIN/ELF/MAP/bootloader/分区/flash_args")
    parser.add_argument("--evidence-dir", type=Path, required=True, help="本轮脱敏日志目录；仅此目录中的证据")
    parser.add_argument("--taskbook", type=Path, required=True, help="用户提供的原始任务包ZIP")
    parser.add_argument("--output", type=Path, help="默认 source/artifacts/ble-collector-handoff-20261023.zip，禁止覆盖")
    args = parser.parse_args(argv)
    output = args.output or args.source / "artifacts" / "ble-collector-handoff-20261023.zip"
    try:
        destination, result = create_package(args.source, args.firmware_dir, args.evidence_dir, args.taskbook, output)
        print(f"Handoff package: {destination}")
        print(f"SHA256: {result['zip_sha256']}")
        print(f"Package and temporary restoration: PASS ({result['file_count']} files)")
        print("Device tests: NOT RUN; no commit, push or flash performed")
        return 0
    except (OSError, ValueError, RuntimeError, UnicodeError, zipfile.BadZipFile, subprocess.CalledProcessError) as error:
        # subprocess stderr 不回显，避免环境/路径异常意外携带秘密。
        detail = "Git 只读盘点失败" if isinstance(error, subprocess.CalledProcessError) else str(error)
        print(f"交接打包失败：{detail}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
