#!/usr/bin/env python3
"""未提交源码打包与跨平台 ZIP 恢复安全边界测试；不创建提交。"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import stat
import subprocess
import sys
import unittest
from unittest import mock
import zipfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import package_collector as package
import verify_collector_handoff as verify
import test_archive_firmware as archive_tests


class HandoffTests(unittest.TestCase):
    def setUp(self):
        # 复用现有、可被真实固件校验器识别的合成BIN/ELF/分区fixture。
        self.fixture = archive_tests.FirmwareArchiveTest("test_round_trip_records_only_real_artifacts_and_descriptor_metadata")
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.root = self.fixture.root
        self.source = self.root / "source"
        self.source.mkdir()
        subprocess.run(["git", "init", "-q", str(self.source)], check=True)
        (self.source / "CMakeLists.txt").write_text("# synthetic source snapshot\n", encoding="utf-8")
        (self.source / "new-untracked.c").write_text("int synthetic = 1;\n", encoding="utf-8")
        (self.source / ".gitignore").write_text("build/\n", encoding="utf-8")
        self.evidence = self.root / "evidence"
        self.evidence.mkdir()
        (self.evidence / "host-tests.log").write_text("Synthetic tests only\n", encoding="utf-8")
        self.taskbook = self.root / "original-taskbook.zip"
        with zipfile.ZipFile(self.taskbook, "w") as archive:
            archive.writestr("README.txt", "Synthetic taskbook fixture")
        self.output = self.root / "delivery.zip"
        # 无提交的临时Git仓库用于真实枚举未跟踪文件；只替换不存在的HEAD身份。
        self.state = {"base_commit": "a" * 40, "branch": "feature/synthetic", "dirty": True,
                      "git_status_porcelain_v1_z": ["?? new-untracked.c"]}
        patch = mock.patch.object(package, "git_state", return_value=self.state)
        patch.start()
        self.addCleanup(patch.stop)

    def create(self):
        return package.create_package(self.source, self.fixture.build, self.evidence, self.taskbook, self.output)

    def rewrite(self, transform):
        with zipfile.ZipFile(self.output) as archive:
            rows = [(info, archive.read(info)) for info in archive.infolist()]
        rows = transform(rows)
        rewritten = self.root / "modified.zip"
        with zipfile.ZipFile(rewritten, "w") as archive:
            for info, data in rows:
                archive.writestr(info, data)
        return rewritten

    def test_roundtrip_includes_untracked_and_verified_firmware(self):
        output, result = self.create()
        manifest = result["manifest"]
        self.assertTrue(manifest["dirty"])
        self.assertEqual(manifest["base_commit"], "a" * 40)
        self.assertEqual(manifest["sdk_version"], "v5.5.3")
        self.assertEqual(manifest["device_tests"], "NOT_RUN")
        self.assertIn("source/new-untracked.c", manifest["files"])
        self.assertIn("evidence/taskbook/original-taskbook.zip", manifest["files"])
        self.assertIn("evidence/logs/host-tests.log", manifest["files"])
        self.assertEqual(output.with_suffix(".zip.sha256").read_text().split()[0], verify.sha256_file(output))
        target = self.root / "restored"
        verified = verify.verify_handoff(output, result["zip_sha256"], target)
        self.assertTrue(verified["restoration_verified"])
        self.assertEqual((target / "source/new-untracked.c").read_bytes(), (self.source / "new-untracked.c").read_bytes())
        self.assertEqual(package.inspect_build(target / manifest["firmware_archive"])[0]["full_bin_sha256"], manifest["full_bin_sha256"])
        self.assertFalse((target / "source/.git").exists())

    def test_excludes_cache_secrets_and_generated_skills(self):
        for relative in ["build/generated.c", "artifacts/old.zip", ".agents/skills/link.txt",
                         "managed_components/cache.c", "credentials.private.json", ".env.local",
                         "secrets.json", "cert.pem", "tools/__pycache__/cached.pyc"]:
            path = self.source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("must not package", encoding="utf-8")
        _, result = self.create()
        files = result["manifest"]["files"]
        self.assertEqual({name for name in files if name.startswith("source/")},
                         {"source/.gitignore", "source/CMakeLists.txt", "source/new-untracked.c"})

    def test_source_symlink_is_rejected(self):
        link = self.source / "linked.c"
        self.fixture.symlink(link, self.source / "new-untracked.c")
        with self.assertRaises(ValueError):
            self.create()
        self.assertFalse(self.output.exists())

    def test_nonempty_json_credentials_are_rejected_without_secret_output(self):
        secret = "sensitive-fixture-value"
        (self.source / "untrusted.json").write_text(json.dumps({"nested": {"token": secret}}))
        with self.assertRaises(ValueError) as caught:
            self.create()
        self.assertNotIn(secret, str(caught.exception))
        self.assertFalse(self.output.exists())

    def test_exact_wokwi_fixture_is_recoverable(self):
        data = (ROOT / "sim/wokwi/fixtures/config.json").read_bytes()
        path = self.source / "sim/wokwi/fixtures/config.json"
        path.parent.mkdir(parents=True)
        path.write_bytes(data)
        output, result = self.create()
        self.assertIn(package.WOKWI_FIXTURE_NAME, result["manifest"]["files"])
        with zipfile.ZipFile(output) as archive:
            self.assertEqual(archive.read(package.WOKWI_FIXTURE_NAME), data)

    def test_wokwi_fixture_allowlist_does_not_extend_to_other_paths(self):
        data = (ROOT / "sim/wokwi/fixtures/config.json").read_bytes()
        for name in ["sim/wokwi/fixtures/config.json", "source/config.json",
                     "source/sim/wokwi/fixtures/another.json", "source/SIM/wokwi/fixtures/config.json",
                     "evidence/logs/sim/wokwi/fixtures/config.json"]:
            with self.subTest(name=name), self.assertRaises(ValueError):
                package.scan_content(name, data)

    def test_wokwi_fixture_rejects_every_field_change_and_extra_content(self):
        original = (ROOT / "sim/wokwi/fixtures/config.json").read_bytes()
        fields = json.loads(original)
        variants = [original + b" ", original + b"{}", original.replace(b'"ssid":', b'"ssid":"duplicate","ssid":', 1)]
        for key in fields:
            changed = dict(fields)
            changed[key] += "changed"
            variants.append(json.dumps(changed).encode())
        variants.append(json.dumps({**fields, "extra": "not-allowed"}).encode())
        for data in variants:
            with self.subTest(sha256=hashlib.sha256(data).hexdigest()), self.assertRaises(ValueError):
                package.scan_content(package.WOKWI_FIXTURE_NAME, data)

    def test_wokwi_cli_token_is_rejected_without_echo(self):
        synthetic = "wok_" + "x" * 40
        for name in ["source/token.txt", "evidence/logs/cli.log"]:
            with self.subTest(name=name):
                with self.assertRaises(ValueError) as caught:
                    package.scan_content(name, ("export WOKWI_CLI_TOKEN=" + synthetic).encode())
                self.assertNotIn(synthetic, str(caught.exception))

    def test_parent_token_file_is_outside_source_snapshot(self):
        # 不使用用户的 Token 文件；仅证明枚举范围不会跨出独立源码仓库。
        (self.source.parent / "wokwi-token.txt").write_text("wok_" + "x" * 40)
        _, result = self.create()
        self.assertFalse(any("wokwi-token" in name for name in result["manifest"]["files"]))

    def test_evidence_bearer_and_taskbook_secrets_are_rejected(self):
        (self.evidence / "leak.log").write_text("Authorization: Bearer " + "x" * 43)
        with self.assertRaises(ValueError):
            self.create()
        (self.evidence / "leak.log").unlink()
        with zipfile.ZipFile(self.taskbook, "w") as archive:
            archive.writestr("settings.private.json", "{}")
        with self.assertRaises(ValueError):
            self.create()

    def test_existing_delivery_and_restore_destination_are_preserved(self):
        output, result = self.create()
        original = output.read_bytes()
        with self.assertRaises(ValueError):
            self.create()
        self.assertEqual(output.read_bytes(), original)
        target = self.root / "restored"
        target.mkdir()
        (target / "keep.txt").write_text("keep")
        with self.assertRaises(ValueError):
            verify.verify_handoff(output, result["zip_sha256"], target)
        self.assertEqual((target / "keep.txt").read_text(), "keep")

    def test_tampered_payload_and_wrong_external_hash_fail(self):
        self.create()
        modified = self.rewrite(lambda rows: [(info, b"changed" if info.filename == "source/new-untracked.c" else data) for info, data in rows])
        with self.assertRaises(ValueError):
            verify.verify_handoff(modified)
        with self.assertRaises(ValueError):
            verify.verify_handoff(self.output, "0" * 64)

    def test_extra_file_and_duplicate_manifest_keys_fail(self):
        self.create()
        modified = self.rewrite(lambda rows: rows + [(zipfile.ZipInfo("source/unlisted.txt"), b"extra")])
        with self.assertRaises(ValueError):
            verify.verify_handoff(modified)
        modified = self.rewrite(lambda rows: [(info, b'{"schema_version":1,"schema_version":1}' if info.filename == "manifest.json" else data) for info, data in rows])
        with self.assertRaises(ValueError):
            verify.verify_handoff(modified)

    def test_paths_are_safe_on_windows_and_posix(self):
        for name in ["../outside", "/absolute", "C:/drive", "source/../outside", "source\\file",
                     "source//file", "source/./file", "source/name.", "source/NUL.txt", "source/file:stream"]:
            with self.subTest(name=name), self.assertRaises(ValueError):
                verify.safe_name(name)
        verify.safe_name("evidence/taskbook/中文任务书.zip")
        self.create()
        modified = self.rewrite(lambda rows: rows + [(zipfile.ZipInfo("../outside"), b"bad")])
        with self.assertRaises(ValueError):
            verify.verify_handoff(modified)

    def test_zip_symlink_and_case_collision_fail(self):
        self.create()
        def link(rows):
            for info, data in rows:
                if info.filename == "source/new-untracked.c":
                    info.create_system = 3
                    info.external_attr = (stat.S_IFLNK | 0o777) << 16
            return rows
        with self.assertRaises(ValueError):
            verify.verify_handoff(self.rewrite(link))
        with self.assertRaises(ValueError):
            verify.verify_handoff(self.rewrite(lambda rows: rows + [(zipfile.ZipInfo("SOURCE/NEW-UNTRACKED.C"), b"bad")]))

    def test_firmware_elf_mismatch_blocks_packaging(self):
        (self.fixture.build / self.fixture.elf_name).write_bytes(b"not ELF")
        with self.assertRaises(ValueError):
            self.create()
        self.assertFalse(self.output.exists())

    def test_source_changes_during_packaging_block_publication(self):
        real_verify = package.verify_handoff
        def changing_verify(path):
            result = real_verify(path)
            (self.source / "new-untracked.c").write_text("changed concurrently")
            return result
        with mock.patch.object(package, "verify_handoff", side_effect=changing_verify):
            with self.assertRaises(ValueError):
                self.create()
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
