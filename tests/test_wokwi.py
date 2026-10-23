"""Wokwi 工具的本地主机测试；所有 CLI 与编译子进程均由测试替身替代。"""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zlib

PROJECT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("wokwi_tool", PROJECT / "tools" / "wokwi.py")
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)
TOKEN = "wok_" + "A" * 40
CONFIG = {"ssid": "Wokwi-GUEST", "password": "", "base_url": "https://fixture.wokwi.invalid",
          "node_id": "Wokwi", "token": "W" * 43,
          "ca": "-----BEGIN CERTIFICATE-----\nSYNTHETIC-NOT-A-CERT\n-----END CERTIFICATE-----\n"}


def png_fixture():
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 2, 0, 0, 0)) +
            chunk(b"tEXt", b"Comment\0synthetic host-test screenshot, not simulator evidence") +
            chunk(b"IDAT", zlib.compress(b"\0\xff\0\0\0\xff\0\0\0\0\xff\xff\xff\xff")) + chunk(b"IEND", b""))


class TokenTests(unittest.TestCase):
    def test_single_token_and_shell_assignment_are_data(self):
        values = [TOKEN, f"'{TOKEN}'", f"WOKWI_CLI_TOKEN={TOKEN}",
                  f'export WOKWI_CLI_TOKEN="{TOKEN}"', f"{TOKEN} # comment\n"]
        with tempfile.TemporaryDirectory() as directory:
            token = Path(directory) / "token.private.txt"
            for value in values:
                with self.subTest(value=value[:12]):
                    token.write_text(value)
                    self.assertEqual(tool.token_from_file(token), TOKEN)

    def test_commands_extra_tokens_and_malformed_content_are_rejected_without_echo(self):
        values = [f"{TOKEN}; touch injected", f"{TOKEN} && touch injected", f"{TOKEN}\necho injected",
                  f"export WOKWI_CLI_TOKEN={TOKEN}\nexport OTHER=x", f"$({TOKEN})", f"`{TOKEN}`",
                  f"{TOKEN} other", f"'{TOKEN}", "wok_short", "", TOKEN + "A"]
        with tempfile.TemporaryDirectory() as directory:
            token = Path(directory) / "token.private.txt"
            for value in values:
                with self.subTest(value=value[:12]):
                    token.write_text(value)
                    out, err = io.StringIO(), io.StringIO()
                    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                        with self.assertRaises(ValueError) as failure:
                            tool.token_from_file(token)
                    self.assertNotIn(TOKEN, str(failure.exception) + out.getvalue() + err.getvalue())


class _Fixture(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="wokwi-tool-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.sim = self.root / "sim/wokwi"
        self.build = self.root / "build/wokwi"
        self.sim.mkdir(parents=True)
        self.build.mkdir(parents=True)
        for name in ["sdkconfig.defaults", "CMakeLists.txt", "partitions.csv", "dependencies.lock",
                     "tools/wokwi.py", "tools/verify_firmware.py", "main/CMakeLists.txt", "main/collector_core.c",
                     "main/collector_scan.c", "main/collector_network.c", "components/bsp/src/bsp_display.c",
                     "components/bsp/src/bsp_button.c", "sim/wokwi/collector_scan.c", "sim/wokwi/collector_network.c",
                     "sim/wokwi/bsp_display.c", "sim/wokwi/bsp_button.c", "sim/wokwi/sdkconfig.defaults"]:
            file = self.root / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text("fixture " + name + "\n")
        (self.sim / "fixtures").mkdir()
        (self.sim / "fixtures/config.json").write_text(json.dumps(CONFIG))
        self.root_config = self.root / "sdkconfig"
        self.root_config.write_text("root config must stay unchanged\n")
        self.dev_config = self.root / "build/dev/sdkconfig"
        self.dev_config.parent.mkdir()
        self.dev_config.write_text("dev config must stay unchanged\n")
        for name, value in (("ROOT", self.root), ("SIM", self.sim), ("BUILD", self.build)):
            setting = patch.object(tool, name, value)
            setting.start()
            self.addCleanup(setting.stop)
        self.commands = []
        self.wrong_source = None
        self.saw_config = []

    def fake_build(self, command, **kwargs):
        self.commands.append(command)
        if command[-1] == "build":
            self.saw_config.append((self.build / "sdkconfig").exists())
            (self.build / "sdkconfig").write_text("CONFIG_COLLECTOR_WOKWI=y\n# CONFIG_BT_ENABLED is not set\n")
            files = [self.sim / name for name in ("collector_scan.c", "collector_network.c", "bsp_display.c", "bsp_button.c")]
            if self.wrong_source:
                files.append(self.root / self.wrong_source)
            (self.build / "compile_commands.json").write_text(json.dumps([{"file": str(file)} for file in files]))
            for name in ("FoloToy-AI-Passport-full.bin", "FoloToy-AI-Passport.elf", "FoloToy-AI-Passport.bin", "flasher_args.json",
                         "bootloader/bootloader.bin", "partition_table/partition-table.bin"):
                artifact = self.build / name
                artifact.parent.mkdir(exist_ok=True)
                artifact.write_bytes((name + " fixture\n").encode())
        return subprocess.CompletedProcess(command, 0)

    def build_mocked(self):
        with patch.object(tool.subprocess, "run", side_effect=self.fake_build), contextlib.redirect_stdout(io.StringIO()):
            tool.build()


class BuildTests(_Fixture):
    def test_build_isolated_config_and_defaults_change_invalidation(self):
        self.build_mocked()
        self.build_mocked()
        (self.sim / "sdkconfig.defaults").write_text("updated simulation defaults\n")
        self.build_mocked()
        self.assertEqual(self.saw_config, [False, True, False])
        self.assertEqual(self.root_config.read_text(), "root config must stay unchanged\n")
        self.assertEqual(self.dev_config.read_text(), "dev config must stay unchanged\n")
        for command in self.commands:
            if "idf.py" in command:
                self.assertEqual(command[command.index("-B") + 1], str(self.build))
                self.assertIn("SDKCONFIG=" + str(self.build / "sdkconfig"), command)
                self.assertIn("SDKCONFIG_DEFAULTS=" + str(self.root / "sdkconfig.defaults") + ";" + str(self.sim / "sdkconfig.defaults"), command)
        self.assertTrue((self.build / "simulation-manifest.json").is_file())

    def test_real_scanner_or_bsp_source_cannot_enter_simulation_manifest(self):
        for source in ("main/collector_scan.c", "main/collector_network.c", "components/bsp/src/bsp_display.c", "components/bsp/src/bsp_button.c"):
            with self.subTest(source=source):
                self.wrong_source = source
                with self.assertRaisesRegex(ValueError, "隔离失败"):
                    self.build_mocked()
        self.assertFalse((self.build / "simulation-manifest.json").exists())

    def test_source_change_during_build_cannot_receive_valid_manifest(self):
        def changing_build(command, **kwargs):
            result = self.fake_build(command, **kwargs)
            if command[-1] == "build":
                (self.root / "main/collector_core.c").write_text("changed during build\n")
            return result
        with patch.object(tool.subprocess, "run", side_effect=changing_build):
            with self.assertRaisesRegex(ValueError, "构建期间源码改变"):
                tool.build()
        self.assertFalse((self.build / "simulation-manifest.json").exists())


class ScenarioTests(unittest.TestCase):
    def test_startup_screens_use_relative_paths_and_no_serial_payload(self):
        scene = tool.scenario(Path("/unused/evidence"))
        steps = scene["steps"]
        self.assertEqual(steps[0], {"wait-serial": "WOKWI_READY"})
        screenshots = [step["take-screenshot"]["save-to"] for step in steps if "take-screenshot" in step]
        self.assertEqual(screenshots, ["01-startup.png", "02-devices-empty.png", "03-diagnostics.png",
                                       "04-menu.png", "05-usb-window.png"])
        self.assertFalse(any("write-serial" in step for step in steps))
        self.assertIn({"wait-serial": "WOKWI_VIEW page=3 menu=1 usb=1"}, steps)
        self.assertTrue(all(Path(path).name == path for path in screenshots))

    def test_flow_opens_usb_before_bounded_payload_and_visits_pause_resume(self):
        scene = tool.scenario(Path("/unused/evidence"), CONFIG)
        steps = scene["steps"]
        writes = [index for index, step in enumerate(steps) if "write-serial" in step]
        usb = steps.index({"wait-serial": "WOKWI_VIEW page=3 menu=1 usb=1"})
        self.assertGreater(min(writes), usb)
        payload = b"".join(bytes(steps[index]["write-serial"]) for index in writes)
        self.assertEqual(json.loads(payload), CONFIG)
        self.assertTrue(payload.endswith(b"\n"))
        self.assertTrue(all(0 < len(steps[index]["write-serial"]) <= 64 for index in writes))
        self.assertTrue(all(steps[index + 1] == {"delay": "20ms"} for index in writes))
        for marker in ["WOKWI_VIEW page=3 menu=1 usb=0 wifi=1 clock=1 revision=1",
                       "WOKWI_HTTP kind=2 ack=1", "devices=3 pause=1", "devices=3 pause=0"]:
            self.assertIn({"wait-serial": marker}, steps)
        second_heartbeat = steps.index({"wait-serial": "WOKWI_FIXTURE_HEARTBEAT count=2"})
        resumed = steps.index({"wait-serial": "devices=3 pause=0"})
        upload_waits = [index for index, step in enumerate(steps) if step.get("wait-serial") == "WOKWI_HTTP kind=2 ack=1"]
        self.assertGreater(second_heartbeat, resumed)
        self.assertGreater(second_heartbeat, upload_waits[-1])
        self.assertNotIn({"delay": "32s"}, steps)
        self.assertEqual(sum("take-screenshot" in step for step in steps), 8)
        self.assertEqual(sum(step.get("wait-serial") == "WOKWI_HTTP kind=2 ack=1" for step in steps), 2)
        pressed = {}
        for step in steps:
            if "set-control" in step:
                control = step["set-control"]
                part = control["part-id"]
                if control["value"]:
                    self.assertFalse(pressed.get(part, False))
                    pressed[part] = True
                else:
                    self.assertTrue(pressed.get(part, False))
                    pressed[part] = False
        self.assertFalse(any(pressed.values()))
        self.assertNotIn(TOKEN, json.dumps(scene))

    def test_configuration_wait_uses_repeated_state_not_already_emitted_receipts(self):
        steps = tool.scenario(Path("/unused/evidence"), CONFIG)["steps"]
        waits = [step["wait-serial"] for step in steps if "wait-serial" in step]
        # CLI 只匹配注册 wait 之后的数据；写入期间产生的一次性回执必须留给最终日志审查。
        for transient in ("RECEIVED_RAM_ONLY", "AUTH_CONFIG_APPLIED", "WOKWI_HTTP kind=1 ack=1"):
            self.assertFalse(any(transient in marker for marker in waits))
        last_write = max(index for index, step in enumerate(steps) if "write-serial" in step)
        first_wait = next(step["wait-serial"] for step in steps[last_write + 1:] if "wait-serial" in step)
        self.assertEqual(first_wait, "WOKWI_VIEW page=3 menu=1 usb=0 wifi=1 clock=1 revision=1")

    def test_each_screenshot_waits_for_ui_consumption_and_spi_refresh(self):
        for credentials in (None, CONFIG):
            steps = tool.scenario(Path("/unused/evidence"), credentials)["steps"]
            for index, step in enumerate(steps):
                if "take-screenshot" in step:
                    self.assertGreater(index, 0)
                    self.assertEqual(steps[index - 1], {"delay": "1s"})


class SimulationTests(_Fixture):
    def setUp(self):
        super().setUp()
        self.build_mocked()
        cli = self.root / "fake-cli"
        cli.write_text("mock only; never executed\n")
        token = self.root / "token.private.txt"
        token.write_text(TOKEN)
        self.args = SimpleNamespace(command="flow", cli=str(cli), token_file=token,
                                    output=self.root / "evidence", timeout=150000)
        self.cli_calls = []
        self.heartbeats = self.uploads = 2
        self.received = self.authenticated = True
        self.flow_trace = "devices=3 pause=0\ndevices=3 pause=1\ndevices=3 pause=0\n"
        self.uploads_after_resume = True
        self.extra_serial = ""
        self.bad_shot = None
        self.omit_shot = None
        self.extra_shots = 0
        self.process_error = None
        self.exit_code = 0

    def fake_cli(self, command, **kwargs):
        self.cli_calls.append(command)
        self.assertNotIn(TOKEN, " ".join(command))
        self.assertFalse(kwargs.get("shell", False))
        self.assertEqual(kwargs["env"]["WOKWI_CLI_TOKEN"], TOKEN)
        scene_path = Path(command[command.index("--scenario") + 1])
        self.assertEqual(scene_path.stat().st_mode & 0o777, 0o600)
        scene_text = scene_path.read_text()
        self.assertNotIn(TOKEN, scene_text)
        scene = json.loads(scene_text)
        if self.process_error:
            raise self.process_error
        serial = "WOKWI_READY\nWOKWI_VIEW page=3 menu=1 usb=1\n" + self.extra_serial
        if self.args.command == "flow":
            if self.received:
                serial += "RECEIVED_RAM_ONLY\n"
            if self.authenticated:
                serial += "AUTH_CONFIG_APPLIED\n"
            serial += "WOKWI_VIEW page=3 menu=1 usb=0 wifi=1 clock=1 revision=1\n"
            if not self.uploads_after_resume:
                serial += "WOKWI_HTTP kind=2 ack=1\n" * self.uploads
            serial += self.flow_trace
            serial += "WOKWI_HTTP kind=1 ack=1\n" * self.heartbeats
            if self.uploads_after_resume:
                serial += "WOKWI_HTTP kind=2 ack=1\n" * self.uploads
        Path(command[command.index("--serial-log-file") + 1]).write_text(serial)
        for step in scene["steps"]:
            if "take-screenshot" in step:
                name = step["take-screenshot"]["save-to"]
                self.assertEqual(Path(name).name, name)
                if name != self.omit_shot:
                    (scene_path.parent / name).write_bytes(b"invalid image" if name == self.bad_shot else png_fixture())
        for number in range(self.extra_shots):
            (scene_path.parent / f"unrelated-{number}.png").write_bytes(png_fixture())
        kwargs["stdout"].write("mock CLI completed; no cloud call\n")
        return subprocess.CompletedProcess(command, self.exit_code)

    def simulate_mocked(self):
        output = io.StringIO()
        with patch.object(tool.subprocess, "run", side_effect=self.fake_cli), contextlib.redirect_stdout(output):
            tool.simulate(self.args)
        return output.getvalue()

    def test_flow_pass_is_separate_from_https_and_device_acceptance_and_has_no_cli_token(self):
        output = self.simulate_mocked()
        report = json.loads((self.args.output / "result.json").read_text())
        self.assertEqual(report["simulation"], "PASS")
        self.assertEqual((report["heartbeat_acks"], report["upload_acks"], report["screenshots"]), (2, 2, 8))
        self.assertEqual(report["https_integration"], "NOT RUN")
        self.assertEqual(report["device_tests"], "NOT RUN")
        self.assertEqual(report["flow_sequence_errors"], [])
        self.assertEqual(len(self.cli_calls), 1)
        self.assertFalse((self.args.output / "scenario.yaml").exists())
        for file in self.args.output.iterdir():
            self.assertNotIn(TOKEN.encode(), file.read_bytes())
        self.assertNotIn(TOKEN, output)
        summary = (self.args.output / "scenario-summary.json").read_text()
        self.assertNotIn(CONFIG["token"], summary)
        self.assertIn("synthetic configuration chunk omitted", summary)

    def test_smoke_accepts_only_startup_evidence(self):
        self.args.command = "smoke"
        self.heartbeats = self.uploads = 0
        self.simulate_mocked()
        report = json.loads((self.args.output / "result.json").read_text())
        self.assertEqual((report["simulation"], report["screenshots"]), ("PASS", 5))

    def test_source_and_binary_changes_are_rejected_before_cli(self):
        core = self.root / "main/collector_core.c"
        original = core.read_bytes()
        core.write_bytes(original + b"changed\n")
        with self.assertRaisesRegex(ValueError, "源码或仿真配置"):
            self.simulate_mocked()
        core.write_bytes(original)
        (self.build / "bootloader/bootloader.bin").write_bytes(b"changed image")
        with self.assertRaisesRegex(ValueError, "构建产物已改变"):
            self.simulate_mocked()
        self.assertEqual(self.cli_calls, [])
        self.assertFalse(self.args.output.exists())

    def test_process_launch_failure_removes_private_scenario(self):
        self.process_error = FileNotFoundError("mock CLI launch failed")
        with self.assertRaises(FileNotFoundError):
            self.simulate_mocked()
        self.assertFalse((self.args.output / "scenario.yaml").exists())

    def test_interrupt_removes_private_scenario(self):
        self.process_error = KeyboardInterrupt()
        with self.assertRaises(KeyboardInterrupt):
            self.simulate_mocked()
        self.assertFalse((self.args.output / "scenario.yaml").exists())

    def test_one_heartbeat_is_not_a_complete_flow(self):
        self.heartbeats = 1
        self.extra_serial = "WOKWI_FIXTURE_HEARTBEAT count=2 NETWORK=FIXTURE\n"
        with self.assertRaisesRegex(ValueError, "场景未通过"):
            self.simulate_mocked()
        report = json.loads((self.args.output / "result.json").read_text())
        self.assertEqual(report["simulation"], "FAIL")
        self.assertEqual(report["heartbeat_acks"], 1)

    def test_stable_config_state_does_not_replace_final_received_receipt(self):
        self.received = False
        with self.assertRaisesRegex(ValueError, "场景未通过"):
            self.simulate_mocked()
        report = json.loads((self.args.output / "result.json").read_text())
        self.assertEqual(report["missing_serial_markers"], ["RECEIVED_RAM_ONLY"])
        self.assertEqual(report["simulation"], "FAIL")

    def test_stable_config_state_does_not_replace_final_auth_receipt(self):
        self.authenticated = False
        with self.assertRaisesRegex(ValueError, "场景未通过"):
            self.simulate_mocked()
        report = json.loads((self.args.output / "result.json").read_text())
        self.assertEqual(report["missing_serial_markers"], ["AUTH_CONFIG_APPLIED"])
        self.assertEqual(report["simulation"], "FAIL")

    def test_one_upload_is_not_a_complete_flow(self):
        self.uploads = 1
        with self.assertRaisesRegex(ValueError, "场景未通过"):
            self.simulate_mocked()

    def test_unpaused_state_before_pause_is_not_resume_evidence(self):
        self.flow_trace = "devices=3 pause=0\ndevices=3 pause=1\n"
        self.uploads_after_resume = False
        with self.assertRaisesRegex(ValueError, "场景未通过"):
            self.simulate_mocked()
        report = json.loads((self.args.output / "result.json").read_text())
        self.assertEqual(report["missing_serial_markers"], [])
        self.assertEqual((report["heartbeat_acks"], report["upload_acks"], report["screenshots"]), (2, 2, 8))
        self.assertEqual(report["flow_sequence_errors"], ["RESUME_AFTER_PAUSE_MISSING: 暂停后未记录恢复状态"])
        self.assertEqual(report["simulation"], "FAIL")

    def test_upload_acks_before_pause_do_not_prove_upload_after_resume(self):
        self.uploads_after_resume = False
        self.flow_trace += "WOKWI_HTTP kind=2 ack=0 status=503\n"
        with self.assertRaisesRegex(ValueError, "场景未通过"):
            self.simulate_mocked()
        report = json.loads((self.args.output / "result.json").read_text())
        self.assertEqual(report["missing_serial_markers"], [])
        self.assertEqual(report["upload_acks"], 2)
        self.assertEqual(report["flow_sequence_errors"], ["UPLOAD_ACK_AFTER_RESUME_MISSING: 恢复后未记录上传成功 ACK"])

    def test_missing_pause_reports_first_missing_stage(self):
        self.flow_trace = "devices=3 pause=0\n"
        with self.assertRaisesRegex(ValueError, "场景未通过"):
            self.simulate_mocked()
        report = json.loads((self.args.output / "result.json").read_text())
        self.assertEqual(report["flow_sequence_errors"], ["PAUSE_MISSING: 未记录暂停状态"])

    def test_invalid_expected_png_is_rejected(self):
        self.bad_shot = "07-devices.png"
        with self.assertRaisesRegex(ValueError, "场景未通过"):
            self.simulate_mocked()
        report = json.loads((self.args.output / "result.json").read_text())
        self.assertEqual(report["bad_screenshots"], ["07-devices.png"])

    def test_missing_expected_screenshot_cannot_be_replaced_by_unrelated_pngs(self):
        self.omit_shot = "11-resumed.png"
        self.extra_shots = 11
        with self.assertRaisesRegex(ValueError, "场景未通过"):
            self.simulate_mocked()
        report = json.loads((self.args.output / "result.json").read_text())
        self.assertEqual(report["bad_screenshots"], ["11-resumed.png"])

    def test_cli_nonzero_exit_with_all_markers_and_images_still_fails(self):
        self.exit_code = 1
        with self.assertRaisesRegex(ValueError, "场景未通过"):
            self.simulate_mocked()
        self.assertEqual(json.loads((self.args.output / "result.json").read_text())["cli_exit"], 1)

    def test_runtime_assert_after_success_markers_still_fails(self):
        self.extra_serial = "assert failed: synthetic task failure\n"
        with self.assertRaisesRegex(ValueError, "场景未通过"):
            self.simulate_mocked()
        self.assertIn("assert failed", json.loads((self.args.output / "result.json").read_text())["runtime_faults"])

    def test_existing_evidence_is_never_overwritten(self):
        self.args.output.mkdir()
        existing = self.args.output / "serial.log"
        existing.write_text("preserve previous evidence\n")
        with self.assertRaisesRegex(ValueError, "证据目录非空"):
            self.simulate_mocked()
        self.assertEqual(existing.read_text(), "preserve previous evidence\n")
        self.assertFalse(self.cli_calls)

    def test_non_fixture_credentials_fail_before_cli(self):
        fixture = self.sim / "fixtures/config.json"
        changed = dict(CONFIG, token="production-looking-token")
        fixture.write_text(json.dumps(changed))
        self.build_mocked()  # 构建匹配此变动，专门验证上传前合成凭据检查。
        with self.assertRaisesRegex(ValueError, "固定合成配置"):
            self.simulate_mocked()
        self.assertFalse(self.cli_calls)


if __name__ == "__main__":
    unittest.main()
