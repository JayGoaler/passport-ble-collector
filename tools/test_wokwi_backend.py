#!/usr/bin/env python3
"""实测本地 Wokwi 后端夹具；不启动模拟器，不读取 Wokwi Token。"""
import argparse
from datetime import datetime, timezone
import http.client
import json
import os
from pathlib import Path
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import time
import unittest

sys.dont_write_bytecode = True
import wokwi_backend as backend_tool

ROOT = Path(__file__).resolve().parents[1]
PREFIX = '/api/tools/bluetooth-explorer/ingest/v1/'


class LocalTLS(http.client.HTTPSConnection):
    def connect(self):
        raw = socket.create_connection(('127.0.0.1', self.port), timeout=self.timeout)
        try:
            self.sock = self._context.wrap_socket(raw, server_hostname=self.host)
        except BaseException:
            raw.close()
            raise


def utc():
    return datetime.now(timezone.utc).isoformat(timespec='milliseconds').replace('+00:00', 'Z')


class WokwiBackendTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        (ROOT / 'build').mkdir(exist_ok=True)
        cls.temp = Path(tempfile.mkdtemp(prefix='wokwi-host-test-', dir=ROOT / 'build'))
        cls.fixture = cls.temp / 'fixture'
        cls.log = cls.temp / 'output.txt'
        cls.output = cls.log.open('wb')
        cls.child = subprocess.Popen([sys.executable, str(ROOT / 'tools/wokwi_backend.py'),
                                     '--backend', str(OPTIONS.backend), '--fixture-dir', str(cls.fixture),
                                     '--port', '0', '--node', NODE, '--openssl', OPENSSL],
                                    stdout=cls.output, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                if cls.child.poll() is not None:
                    raise RuntimeError('后端未启动（日志留在临时测试目录，不回显内容）')
                try:
                    if backend_tool.check_report(cls.fixture / 'status.json', True)[0]:
                        break
                except (OSError, ValueError):
                    pass
                time.sleep(0.1)
            else:
                raise RuntimeError('后端启动超时')
            cls.creds = json.loads((cls.fixture / 'credentials.private.json').read_text())
            cls.port = json.loads((cls.fixture / 'status.json').read_text())['port']
            cls.context = ssl.create_default_context(cafile=cls.fixture / 'server.crt')
        except BaseException:
            cls.child.terminate()
            cls.child.wait(timeout=5)
            cls.output.close()
            raise

    @classmethod
    def tearDownClass(cls):
        cls.child.terminate()
        try:
            cls.child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            cls.child.kill()
            cls.child.wait()
        cls.output.close()
        shutil.rmtree(cls.temp)

    def request(self, endpoint, body=None, token=None, content_type='application/json', method=None):
        if body is not None:
            time.sleep(0.25)  # 保留真实后端每秒 5 次写限制，避免测试自身抢占速率窗口。
        conn = LocalTLS('host.wokwi.internal', self.port, context=self.context, timeout=5)
        raw = body if isinstance(body, str) else json.dumps(body) if body is not None else None
        headers = {'Authorization': 'Bearer ' + (token if token is not None else self.creds['token']),
                   'Content-Type': content_type}
        try:
            conn.request(method or ('POST' if body is not None else 'GET'), PREFIX + endpoint, raw, headers)
            response = conn.getresponse()
            return response.status, json.loads(response.read())
        finally:
            conn.close()

    def heartbeat(self):
        return {'schema_version':'1.0', 'node_id':'Wokwi', 'observed_at':utc(),
                'collector_version':'wokwi-host-fixture', 'scan_state':'scanning',
                'bluetooth_state':'powered_on', 'permission_state':'not_applicable',
                'last_observation_at':None, 'pending_count':0, 'dropped_count':0,
                'error_code':None, 'applied_config_revision':1}

    def batch(self):
        return {'schema_version':'1.0', 'node_id':'Wokwi', 'batch_id':'wokwi-batch', 'sent_at':utc(),
                'observations':[{'event_id':'wokwi-event', 'observed_at':utc(),
                    'local_device_id':'C0:00:00:00:00:01', 'identifier_type':'ble_address',
                    'address_type':'random_static', 'local_name':'Wokwi synthetic only',
                    'rssi_dbm':-58.5, 'rssi_min_dbm':-65, 'rssi_max_dbm':-55,
                    'sample_count':4, 'window_ms':2000, 'tx_power_dbm':None,
                    'service_uuids':['180D'], 'manufacturer_id':None}]}

    def test_01_private_fixture_and_ready(self):
        self.assertEqual(self.creds['ssid'], 'Wokwi-GUEST')
        self.assertEqual(self.creds['password'], '')
        self.assertEqual(self.creds['node_id'], 'Wokwi')
        self.assertEqual(len(self.creds['token']), 43)
        self.assertTrue(backend_tool.check_report(self.fixture / 'status.json', True)[0])
        self.assertFalse(backend_tool.check_report(self.fixture / 'status.json')[0])
        report = json.loads((self.fixture / 'status.json').read_text())
        self.assertEqual(report['host'], '127.0.0.1')
        if os.name != 'nt':
            for name in ('credentials.private.json', 'server.key', 'isolated.sqlite'):
                self.assertEqual((self.fixture / name).stat().st_mode & 0o077, 0)

    def test_02_trusted_hostname_and_config(self):
        code, response = self.request('config')
        self.assertEqual(code, 200)
        self.assertEqual(response['node_id'], 'Wokwi')
        self.assertEqual(response['config_revision'], 1)
        self.assertEqual(response['heartbeat_interval_sec'], 30)

    def test_03_tls_wrong_hostname_and_untrusted_ca_fail(self):
        for name, context in [('wrong.invalid', self.context), ('host.wokwi.internal', ssl.create_default_context())]:
            conn = LocalTLS(name, self.port, context=context, timeout=5)
            try:
                with self.assertRaises(ssl.SSLCertVerificationError):
                    conn.connect()
            finally:
                conn.close()

    def test_04_heartbeat_and_observation_ack_actual_db(self):
        code, heartbeat = self.request('heartbeat', self.heartbeat())
        self.assertEqual(code, 200)
        self.assertEqual(heartbeat['config_revision'], 1)
        code, ack = self.request('observations', self.batch())
        self.assertEqual(code, 200)
        self.assertEqual((ack['accepted'], ack['duplicates'], ack['rejected']), (1, 0, 0))
        self.assertEqual(ack['batch_id'], 'wokwi-batch')
        result = subprocess.run([sys.executable, str(ROOT / 'tools/wokwi_backend.py'),
                                 '--assert-report', str(self.fixture / 'status.json'), '--wait-seconds', '2'],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0)

    def test_05_retry_is_idempotent(self):
        code, ack = self.request('observations', self.batch())
        self.assertEqual(code, 200)
        self.assertEqual((ack['accepted'], ack['duplicates']), (0, 1))
        self.assertEqual(json.loads((self.fixture / 'status.json').read_text())['counters']['stored_events'], 1)

    def test_06_bad_token_and_wrong_node_rejected(self):
        self.assertEqual(self.request('config', token='x' * 43)[0], 401)
        wrong = self.heartbeat()
        wrong['node_id'] = 'another-node'
        self.assertEqual(self.request('heartbeat', wrong)[0], 403)

    def test_07_invalid_payload_does_not_write(self):
        invalid = self.batch()
        invalid['observations'][0]['rssi_dbm'] = 0
        self.assertEqual(self.request('observations', invalid)[0], 400)
        self.assertEqual(self.request('heartbeat', self.heartbeat(), content_type='text/plain')[0], 415)
        self.assertEqual(self.request('observations', 'x' * 8193)[0], 413)

    def test_08_no_admin_routes_and_no_secret_output(self):
        self.assertEqual(self.request('../../nodes')[0], 404)
        self.assertEqual(self.request('config?token=do-not-log-this')[0], 404)
        self.output.flush()
        text = self.log.read_text() + (self.fixture / 'status.json').read_text()
        for hidden in (self.creds['token'], 'BEGIN PRIVATE KEY', 'C0:00:00:00:00:01',
                       'do-not-log-this', 'Wokwi synthetic only'):
            self.assertFalse(hidden in text, '日志出现不应回显的字段')
        self.assertEqual(json.loads((self.fixture / 'status.json').read_text())['counters']['internal_errors'], 0)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend', required=True, type=Path)
    parser.add_argument('--node')
    parser.add_argument('--openssl')
    OPTIONS = parser.parse_args()
    NODE, OPENSSL, _ENV = backend_tool.runtime(OPTIONS.node, OPTIONS.openssl)
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(WokwiBackendTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
