#!/usr/bin/env python3
"""Exercise fixture failures without Qt, real cores or shutdown signals."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest

import test_coordinated_gui as fixture


class StartupDiagnostics(unittest.TestCase):
    def run_failure(self, body, reason):
        # Retain the fixture and its output under the CTest build tree.
        root = Path(tempfile.mkdtemp(prefix='quit-diagnostics-', dir=Path.cwd()))
        binary = root / 'stub-gui'
        binary.write_text('#!' + sys.executable + '\n' + body)
        binary.chmod(0o755)
        output = root / 'fixture.log'
        with output.open('w') as log:
            proc = subprocess.Popen([sys.executable, str(Path(fixture.__file__).resolve()),
                                     str(binary), 'idle', 'term'],
                                    cwd=root, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 5
                while not output.stat().st_size and proc.poll() is None and time.monotonic() < deadline:
                    time.sleep(.01)
                early = output.read_text()
                proc.wait(timeout=22)
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait(timeout=5)
        text = output.read_text()
        self.assertNotEqual(proc.returncode, 0, text)
        self.assertIn(reason, text)
        self.assertTrue(early, 'evidence path must be flushed before readiness/failure: ' + text)
        evidence = json.loads(early.splitlines()[0])
        self.assertEqual(evidence['event'], 'fixture_start')
        fixture_root = Path(evidence['evidence'])
        self.assertTrue(fixture_root.is_dir())
        # The failure itself must be useful even when only its last line is retained.
        failure = text.splitlines()[-1]
        self.assertIn(str(fixture_root), failure)
        self.assertIn('gui_exit_code', failure)
        self.assertIn('core_alive', failure)
        return fixture_root, failure

    def test_timeout_before_core_spawn(self):
        root, failure = self.run_failure('import time\ntime.sleep(60)\n', 'probe readiness timeout')
        self.assertIn('"core_pid": null', failure)
        self.assertIn('"core_status": "not started"', failure)
        self.assertIn('"core_stderr": ""', failure)
        self.assertTrue((root / 'gui.log').is_file())

    def test_bind_failure_is_retained_at_timeout(self):
        # The stub holds the port until its child has exited: no port-allocation race.
        body = '''import os, socket, subprocess, time
from pathlib import Path
server = socket.socket()
server.bind(('127.0.0.1', 0))
server.listen()
core = subprocess.Popen([str(Path(__file__).with_name('nekobox_core')),
                         '-port', str(server.getsockname()[1])])
assert core.wait(timeout=5) != 0
time.sleep(60)
'''
        root, failure = self.run_failure(body, 'probe readiness timeout')
        self.assertIn('"core_status": "failed"', failure)
        self.assertIn('"core_alive": false', failure)
        self.assertIn('Address already in use', failure)
        self.assertIn('Traceback', (root / 'core.stderr').read_text())
        self.assertTrue((root / 'core.pid').is_file())

    def test_gui_startup_exit_has_diagnostics(self):
        _, failure = self.run_failure('raise SystemExit(7)\n', 'GUI exited during startup')
        self.assertIn('"gui_exit_code": 7', failure)
        self.assertIn('"core_status": "not started"', failure)

    def test_fake_core_records_listening(self):
        root = Path(tempfile.mkdtemp(prefix='quit-peer-', dir=Path.cwd()))
        core = root / 'nekobox_core'
        core.write_text('#!' + sys.executable + '\n' + Path(fixture.__file__).read_text())
        core.chmod(0o755)
        env = dict(os.environ, NKR_QUIT_TEST_ROOT=str(root), NKR_QUIT_TEST_MODE='idle')
        with (root / 'stdout').open('w') as log:
            proc = subprocess.Popen([str(core), '-port', '0'], env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 5
                while 'grpc server listening' not in (root / 'stdout').read_text():
                    self.assertIsNone(proc.poll())
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(.01)
                self.assertEqual((root / 'core.status').read_text(), 'listening')
                self.assertEqual((root / 'core.stderr').read_text(), '')
                self.assertEqual(int((root / 'core.pid').read_text()), proc.pid)
            finally:
                proc.kill()
                proc.wait(timeout=5)


if __name__ == '__main__':
    unittest.main()
