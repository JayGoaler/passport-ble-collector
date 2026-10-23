"""USB 主机工具的协议、保密错误输出和有界串口读取回归。"""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("configure_collector", ROOT / "tools/configure_collector.py")
tool = importlib.util.module_from_spec(SPEC)
sys.dont_write_bytecode = True
SPEC.loader.exec_module(tool)


def config():
    return dict(ssid="合成测试网络", password="synthetic-password", base_url="https://collector.invalid:443",
                node_id="passport-test", token="t" * 43,
                ca="-----BEGIN CERTIFICATE-----\nSYNTHETIC-NOT-A-CERT\n-----END CERTIFICATE-----\n")


class FakePort:
    def __init__(self, chunks, short_write=False):
        self.chunks = list(chunks)
        self.short_write = short_write
        self.opened = False
        self.closed = False
        self.writes = bytearray()

    def open(self):
        assert self.dtr is False and self.rts is False
        self.opened = True

    def reset_input_buffer(self):
        assert self.opened

    def write(self, data):
        n = min(len(data), 7) if self.short_write else len(data)
        self.writes.extend(data[:n])
        return n

    def read(self, limit):
        assert limit == 256
        return self.chunks.pop(0) if self.chunks else b""

    def close(self):
        self.closed = True


class ConfigureTests(unittest.TestCase):
    def test_unicode_json_roundtrip_is_single_line(self):
        frame = tool.validate_config(config())
        self.assertEqual(json.loads(frame), config())
        self.assertEqual(frame.count(10), 1)
        self.assertLessEqual(len(frame), tool.MAX_FRAME)

    def test_fields_types_size_and_controls(self):
        cases = [("ssid", "汉" * 11), ("password", "a" * 65), ("token", "x" * 42),
                 ("token", "=" * 43), ("node_id", "node:wrong"), ("ssid", ""),
                 ("ssid", "a\0b"), ("password", "x\ny"), ("ca", "not PEM"),
                 ("ca", "\0" + config()["ca"]), ("node_id", 123), ("ssid", "\ud800")]
        for key, value in cases:
            with self.subTest(key=key, value_type=type(value).__name__):
                data = config()
                data[key] = value
                with self.assertRaises(tool.ConfigError):
                    tool.validate_config(data)
        data = config()
        data["password"] = ""
        self.assertTrue(tool.validate_config(data))
        for data in [[], dict(config(), extra="bad"), {k: v for k, v in config().items() if k != "token"}]:
            with self.assertRaises(tool.ConfigError):
                tool.validate_config(data)

    def test_https_authority_only(self):
        for url in ["http://collector.invalid", "https://", "https://x/", "https://x/path",
                    "https://x?", "https://x#", "https://u:p@x", "https://x:99999", "https://x\\path",
                    "https://x:bad", "https:// x", "https://[bad"]:
            with self.subTest(url=url):
                with self.assertRaises(tool.ConfigError):
                    tool.validate_config(dict(config(), base_url=url))
        self.assertTrue(tool.validate_config(dict(config(), base_url="https://[::1]:443")))

    def test_json_expansion_must_fit_device_line(self):
        ca = '-----BEGIN CERTIFICATE-----' + '"' * 4000 + '-----END CERTIFICATE-----'
        with self.assertRaises(tool.ConfigError):
            tool.validate_config(dict(config(), ca=ca))

    @unittest.skipUnless(os.name == "posix", "POSIX permission semantics")
    def test_protected_file_and_duplicate_keys(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "input.private.json"
            path.write_text(json.dumps(config()), encoding="utf-8")
            path.chmod(0o600)
            self.assertEqual(tool.load_protected_file(path), config())
            path.chmod(0o644)
            with self.assertRaises(tool.ConfigError):
                tool.load_protected_file(path)
            path.chmod(0o600)
            link = Path(tmp) / "link"
            link.symlink_to(path)
            with self.assertRaises(tool.ConfigError):
                tool.load_protected_file(link)
            for contents in ['{"token":"a", "token":"b"}', "not json", "x" * 8193]:
                path.write_text(contents, encoding="utf-8")
                with self.assertRaises(tool.ConfigError):
                    tool.load_protected_file(path)
            with self.assertRaises(tool.ConfigError):
                tool.load_protected_file(tmp)

    def test_windows_file_acl_not_assumed(self):
        with mock.patch.object(tool.os, "name", "nt"):
            with self.assertRaises(tool.ConfigError):
                tool.load_protected_file("ignored")

    def test_noninteractive_input_refused(self):
        with mock.patch.object(tool.sys.stdin, "isatty", return_value=False):
            with self.assertRaises(tool.ConfigError):
                tool.interactive_config("not-read")

    def test_hidden_input_fallback_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            ca = Path(tmp) / "public-ca.pem"
            ca.write_text(config()["ca"], encoding="utf-8")
            with mock.patch.object(tool.sys.stdin, "isatty", return_value=True), \
                    mock.patch.object(tool.getpass, "getpass", side_effect=tool.getpass.GetPassWarning):
                with self.assertRaises(tool.ConfigError):
                    tool.interactive_config(ca)

    def test_receive_ignores_noise_and_never_claims_auth(self):
        chunks = [b"secret noise\nCOLLECTOR {bad}\n", b"x" * 2048 + b"\n",
                  b'COLLECTOR {"status":"WIFI_CONNECTING"}\n',
                  b'COLLECTOR {"status":"RECEIVED_', b'RAM_ONLY"}\r\n']
        self.assertEqual(tool.wait_received(FakePort(chunks), 1), "RECEIVED_RAM_ONLY")

    def test_negative_responses_and_no_raw_error_leak(self):
        for status in tool.ERRORS:
            reply = b"COLLECTOR " + json.dumps(dict(status=status, secret="DO-NOT-ECHO")).encode() + b"\n"
            with self.assertRaises(tool.ConfigError) as caught:
                tool.wait_received(FakePort([reply]), 1)
            self.assertNotIn("DO-NOT-ECHO", str(caught.exception))

    def test_reply_timeout_not_success_or_resend(self):
        with mock.patch.object(tool.time, "monotonic", side_effect=[0, 0, 2]):
            with self.assertRaises(tool.ConfigError):
                tool.wait_received(FakePort([b'COLLECTOR {"status":"AUTH_CONFIG_APPLIED"}\n']), 1)

    def test_send_short_writes_no_reset_and_closes(self):
        port = FakePort([b'COLLECTOR {"status":"RECEIVED_RAM_ONLY"}\n'], short_write=True)
        serial = mock.Mock()
        serial.Serial.return_value = port
        frame = tool.validate_config(config())
        with mock.patch.object(tool.importlib, "import_module", return_value=serial):
            self.assertEqual(tool.send_config("FAKE", frame, 1), "RECEIVED_RAM_ONLY")
        self.assertEqual(json.loads(port.writes), config())
        self.assertTrue(port.closed)
        self.assertEqual(set(frame), {0})
        serial.Serial.assert_called_once_with(port=None, baudrate=115200, timeout=0.1, write_timeout=2)

    def test_serial_exception_never_echoes_secret_and_closes(self):
        port = FakePort([])
        port.write = mock.Mock(side_effect=OSError("synthetic-password"))
        serial = mock.Mock()
        serial.Serial.return_value = port
        frame = tool.validate_config(config())
        with mock.patch.object(tool.importlib, "import_module", return_value=serial):
            with self.assertRaises(tool.ConfigError) as caught:
                tool.send_config("FAKE", frame, 1)
        self.assertNotIn("synthetic-password", str(caught.exception))
        self.assertTrue(port.closed)
        self.assertEqual(set(frame), {0})

    def test_argparse_never_echoes_accidental_secret_arguments(self):
        for args in [["--port", "FAKE", "--ca-file", "x", "--token", "DO-NOT-ECHO"],
                     ["--port", "FAKE", "--ca-file", "x", "--timeout", "DO-NOT-ECHO"]]:
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit) as caught:
                tool.main(args)
            self.assertEqual(caught.exception.code, 2)
            self.assertNotIn("DO-NOT-ECHO", stderr.getvalue())

    def test_cli_success_is_ram_receipt_only(self):
        stdout = io.StringIO()
        with mock.patch.object(tool, "load_protected_file", side_effect=lambda _: config()), \
                mock.patch.object(tool, "send_config", return_value="RECEIVED_RAM_ONLY"), \
                contextlib.redirect_stdout(stdout):
            self.assertEqual(tool.main(["--port", "FAKE", "--config-file", "private.json"]), 0)
        self.assertIn("RECEIVED_RAM_ONLY", stdout.getvalue())
        self.assertIn("尚不代表", stdout.getvalue())
        self.assertNotIn("synthetic-password", stdout.getvalue())
        self.assertNotIn("t" * 43, stdout.getvalue())

    def test_unexpected_input_error_never_leaks_original(self):
        stderr = io.StringIO()
        with mock.patch.object(tool, "interactive_config", side_effect=OSError("DO-NOT-ECHO")), \
                contextlib.redirect_stderr(stderr):
            self.assertEqual(tool.main(["--port", "FAKE", "--ca-file", "public.pem"]), 2)
        self.assertNotIn("DO-NOT-ECHO", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
