#!/usr/bin/env python3
"""校验交接 ZIP 清单并在临时目录验证恢复；仅使用 Python 标准库。"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import sys
import tempfile
import unicodedata
import zipfile

sys.dont_write_bytecode = True
MAX_FILES = 10000
MAX_FILE_BYTES = 256 * 1024 * 1024
MAX_TOTAL_BYTES = 512 * 1024 * 1024
SHA256 = re.compile(r"[0-9a-f]{64}")
WINDOWS_RESERVED = re.compile(r"(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?", re.I)


def safe_name(name):
    if (not isinstance(name, str) or not name or name.startswith("/")
            or any(c in name for c in "\\:\x00") or any(ord(c) < 32 for c in name)):
        raise ValueError("归档含不安全路径")
    for part in name.split("/"):
        if (part in ("", ".", "..") or part.endswith((".", " "))
                or any(c in part for c in '<>"|?*') or WINDOWS_RESERVED.fullmatch(part)):
            raise ValueError("归档含不可安全恢复到 Windows 的路径")
    if PurePosixPath(name).as_posix() != name:
        raise ValueError("归档路径不是规范相对路径")


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("manifest 含重复键")
        result[key] = value
    return result


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _validate_manifest(manifest, entries):
    if (not isinstance(manifest, dict) or type(manifest.get("schema_version")) is not int
            or manifest.get("schema_version") != 1
            or manifest.get("device_tests") != "NOT_RUN"):
        raise ValueError("manifest 版本或设备未验标记无效")
    if (not isinstance(manifest.get("base_commit"), str)
            or not re.fullmatch(r"[0-9a-f]{40}", manifest["base_commit"])):
        raise ValueError("manifest 缺少完整代码基线 SHA")
    if not isinstance(manifest.get("branch"), str) or not isinstance(manifest.get("dirty"), bool):
        raise ValueError("manifest 缺少分支或工作区状态")
    files = manifest.get("files")
    if not isinstance(files, dict) or set(files) != set(entries) - {"manifest.json"}:
        raise ValueError("manifest 与 ZIP 实际文件集合不一致")
    if not files or not any(name.startswith("source/") for name in files):
        raise ValueError("交接缺少源码")
    for name, item in files.items():
        safe_name(name)
        if (not isinstance(item, dict) or set(item) != {"sha256", "size", "mode"}
                or not isinstance(item["sha256"], str) or not SHA256.fullmatch(item["sha256"])
                or type(item["size"]) is not int or not 0 <= item["size"] <= MAX_FILE_BYTES
                or type(item["mode"]) is not int or item["mode"] not in (0o644, 0o755)
                or entries[name].file_size != item["size"]):
            raise ValueError(f"文件清单属性无效：{name}")
    return files


def verify_handoff(path, expected_sha256=None, extract_to=None):
    path = Path(path)
    actual_sha = sha256_file(path)
    if expected_sha256 is not None:
        if not SHA256.fullmatch(expected_sha256) or expected_sha256 != actual_sha:
            raise ValueError("交接 ZIP SHA-256 与外部可信值不一致")
    destination = Path(extract_to).absolute() if extract_to is not None else None
    if destination is not None:
        if destination.exists() or destination.is_symlink():
            raise ValueError("恢复目标必须是尚不存在的新目录，不能覆盖旧项目")
        for parent in destination.parents:
            if parent.is_symlink():
                raise ValueError("恢复目标父目录不能是符号链接")
        if not destination.parent.is_dir():
            raise ValueError("恢复目标的父目录必须已存在")
    with zipfile.ZipFile(path) as archive:
        infos = archive.infolist()
        if len(infos) > MAX_FILES:
            raise ValueError("交接文件数量超过上限")
        entries, normalized = {}, set()
        total = 0
        for info in infos:
            safe_name(info.filename)
            mode = info.external_attr >> 16
            if info.is_dir() or stat.S_IFMT(mode) not in (0, stat.S_IFREG) or info.flag_bits & 1:
                raise ValueError("交接只允许普通文件，不允许目录条目、符号链接或加密成员")
            key = unicodedata.normalize("NFC", info.filename).casefold()
            if key in normalized:
                raise ValueError("ZIP 含重复路径或跨平台大小写冲突")
            normalized.add(key)
            if info.file_size > MAX_FILE_BYTES:
                raise ValueError("交接单文件超过大小上限")
            total += info.file_size
            entries[info.filename] = info
        if total > MAX_TOTAL_BYTES or "manifest.json" not in entries:
            raise ValueError("交接总大小超过上限或缺少 manifest")
        if entries["manifest.json"].file_size > 4 * 1024 * 1024:
            raise ValueError("manifest 超过大小上限")
        manifest = json.loads(archive.read("manifest.json").decode("utf-8"), object_pairs_hook=unique_object)
        files = _validate_manifest(manifest, entries)
        # 只向新建临时目录写入，完整校验成功后才复制到用户指定的新目标。
        with tempfile.TemporaryDirectory(prefix="passport-handoff-verify-") as temp:
            root = Path(temp)
            for name, item in files.items():
                target = root.joinpath(*name.split("/"))
                target.parent.mkdir(parents=True, exist_ok=True)
                digest, size = hashlib.sha256(), 0
                with archive.open(entries[name]) as source, target.open("xb") as output:
                    for chunk in iter(lambda: source.read(1024 * 1024), b""):
                        size += len(chunk)
                        if size > item["size"]:
                            raise ValueError(f"解压内容超过声明大小：{name}")
                        digest.update(chunk)
                        output.write(chunk)
                if size != item["size"] or digest.hexdigest() != item["sha256"]:
                    raise ValueError(f"内容 SHA-256/大小不符：{name}")
                target.chmod(item["mode"])
                if sha256_file(target) != item["sha256"]:
                    raise ValueError(f"恢复后的文件校验失败：{name}")
            (root / "manifest.json").write_bytes(archive.read("manifest.json"))
            if destination is not None:
                shutil.copytree(root, destination)
    return {"zip_sha256": actual_sha, "file_count": len(files), "total_bytes": total,
            "restoration_verified": True, "manifest": manifest}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--sha256", help="源端交付的可信 ZIP SHA-256；建议恢复前提供")
    parser.add_argument("--extract-to", type=Path, help="校验全部成功后恢复到尚不存在的新目录")
    args = parser.parse_args(argv)
    try:
        result = verify_handoff(args.archive, args.sha256, args.extract_to)
        print(f"Handoff verification: PASS ({result['file_count']} files; temporary restoration checked)")
        print(f"SHA256: {result['zip_sha256']}")
        print("Device tests: NOT RUN")
        return 0
    except (OSError, ValueError, RuntimeError, UnicodeError, zipfile.BadZipFile) as error:
        print(f"交接校验失败：{error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
