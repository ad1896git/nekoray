#!/usr/bin/env python3
"""Real GUI entry/owned-QProcess regression, with a loopback-only fake RPC core.

No real proxy, TUN or system proxy is enabled; the macOS stop probe uses an
unprivileged fake launcher and readiness probe. All config and HOME paths are
inside the CTest build tree. Every child is waited for, including failure paths.
"""
import contextlib
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import traceback


def frame(kind, flags, stream, payload=b''):
    return len(payload).to_bytes(3, 'big') + bytes([kind, flags]) + struct.pack('!I', stream) + payload


class ExitHeaders:
    """Recognize the exact Exit :path, including Qt's HPACK dynamic indexing.

    Values remain opaque: the only Huffman value we need is this fixed RPC
    path, encoded with RFC 7541 Appendix B. No optional Python packages needed.
    """
    EXIT = ((False, b'/libcore.LibcoreService/Exit'),
            (True, bytes.fromhex('62834643d855f39a321ec2ee2d9dcc42b183c993')))

    def __init__(self):
        self.dynamic = []

    @staticmethod
    def integer(data, offset, bits):
        value = data[offset] & ((1 << bits) - 1)
        offset += 1
        if value == (1 << bits) - 1:
            shift = 0
            while True:
                byte = data[offset]
                offset += 1
                value += (byte & 127) << shift
                if not byte & 128:
                    break
                shift += 7
        return value, offset

    @classmethod
    def string(cls, data, offset):
        huffman = bool(data[offset] & 128)
        length, offset = cls.integer(data, offset, 7)
        end = offset + length
        assert end <= len(data), 'truncated HPACK string'
        return (huffman, data[offset:end]), end

    def entry(self, index):
        if index > 61:
            return self.dynamic[index - 62]
        # Only :path matters; other static names/values stay opaque.
        return (index in (4, 5), None)

    def is_exit(self, data):
        offset, found = 0, False
        while offset < len(data):
            byte = data[offset]
            if byte & 128:
                index, offset = self.integer(data, offset, 7)
                path, value = self.entry(index)
            elif byte & 32 and not byte & 64:
                size, offset = self.integer(data, offset, 5)
                # Our tiny peer advertises a zero-sized dynamic table below.
                assert size == 0, 'unexpected HPACK table size'
                self.dynamic.clear()
                continue
            else:
                indexed = bool(byte & 64)
                index, offset = self.integer(data, offset, 6 if indexed else 4)
                if index:
                    path, _ = self.entry(index)
                else:
                    name, offset = self.string(data, offset)
                    path = name == (False, b':path')
                value, offset = self.string(data, offset)
                if indexed:
                    self.dynamic.insert(0, (path, value))
            found |= path and value in self.EXIT
        return found


def fake_core():
    root = Path(os.environ['NKR_QUIT_TEST_ROOT'])
    (root / 'core.pid').write_text(str(os.getpid()))
    (root / 'core.status').write_text('starting')
    server = socket.socket()
    server.bind(('127.0.0.1', int(sys.argv[sys.argv.index('-port') + 1])))
    server.listen()
    (root / 'core.status').write_text('listening')
    print('grpc server listening', flush=True)

    def serve(conn):
        headers = ExitHeaders()
        exits = {}
        blocks = {}
        data = b''
        while len(data) < 24:
            part = conn.recv(24 - len(data))
            if not part:
                return
            data += part
        # HEADER_TABLE_SIZE=0 keeps recognition bounded and dependency-free.
        conn.sendall(frame(4, 0, 0, struct.pack('!HI', 1, 0)))
        data = b''
        while True:
            part = conn.recv(65536)
            if not part:
                return
            data += part
            while len(data) >= 9:
                size = int.from_bytes(data[:3], 'big')
                if len(data) < size + 9:
                    break
                kind, flags = data[3:5]
                stream = int.from_bytes(data[5:9], 'big')
                payload, data = data[9:9 + size], data[9 + size:]
                if kind == 1:
                    if flags & 8:
                        payload = payload[1:len(payload) - payload[0]]
                    if flags & 32:
                        payload = payload[5:]
                    blocks[stream] = payload
                elif kind == 9:
                    blocks[stream] += payload
                if kind in (1, 9) and flags & 4:
                    exits[stream] = headers.is_exit(blocks.pop(stream))
                if kind == 4 and not flags & 1:
                    conn.sendall(frame(4, 1, 0))
                elif kind == 6 and not flags & 1:
                    conn.sendall(frame(6, 1, 0, payload))
                elif kind in (0, 1) and flags & 1 and stream:
                    mode = os.environ['NKR_QUIT_TEST_MODE']
                    if exits.get(stream, False):
                        with request_lock:
                            exit_count[0] += 1
                            count = exit_count[0]
                        (root / 'exit_requests').write_text(str(count))
                        if mode in ('exit_cancelled', 'rpc_cancelled') or mode == 'exit_committed' and count > 1:
                            # Match BaseServer.Exit: end the actual owned process.
                            os._exit(0)
                        if mode == 'exit_committed':
                            continue  # Exercise the full 500 ms RPC budget.
                    # Recovery probes acknowledge every call. A pending Start
                    # never completes; pending_stop acknowledges only Start.
                    # Other modes leave reaping to the production kill path.
                    with request_lock:
                        first = not (root / 'request').exists()
                        (root / 'request').touch()
                    if os.environ['NKR_QUIT_TEST_MODE'] == 'owned_core_timeout' and first:
                        continue  # Hold the first Exit until its production RPC deadline.
                    if (os.environ['NKR_QUIT_TEST_MODE'] in ('owned_core_timeout', 'mac_stop_failure', 'exit_cancelled', 'rpc_cancelled', 'wait_cancelled', 'wait_committed', 'kill_cancelled', 'kill_committed', 'complete_cancelled', 'complete_committed') or
                            os.environ['NKR_QUIT_TEST_MODE'] == 'pending_stop' and first):
                        conn.sendall(frame(1, 4, stream, b'\x88') +
                                     frame(0, 0, stream, b'\0' * 5) +
                                     frame(1, 5, stream, b'\x00\x0bgrpc-status\x010'))
    request_lock = threading.Lock()
    exit_count = [0]
    while True:
        conn, _ = server.accept()
        threading.Thread(target=serve, args=(conn,), daemon=True).start()


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


def startup_diagnostics(root, gui):
    def read(name, default=''):
        try:
            return (root / name).read_text(errors='replace')
        except FileNotFoundError:
            return default

    pid_text = read('core.pid').strip()
    pid = int(pid_text) if pid_text.isdecimal() else None
    return json.dumps(dict(evidence=str(root), gui_exit_code=gui.poll(),
                           core_pid=pid, core_alive=bool(pid and alive(pid)),
                           core_status=read('core.status', 'not started'),
                           core_stderr=read('core.stderr')))


def run_gui(binary, mode, route):
    # Keep evidence on failure; temporary files live under the test build tree.
    root = Path(tempfile.mkdtemp(prefix='quit-gui-', dir=Path.cwd()))
    print(json.dumps(dict(event='fixture_start', mode=mode, route=route,
                          evidence=str(root))), flush=True)
    executable = root / 'nekobox'
    shutil.copy2(binary, executable)
    fake = root / 'nekobox_core'
    fake.write_text('#!' + sys.executable + '\n' + Path(__file__).read_text())
    fake.chmod(0o755)
    # BuildConfig requires asset paths; this fake core never reads databases.
    (root / "geoip.db").touch()
    (root / "geosite.db").touch()
    cfg = root / 'appdata/config'
    for sub in ('groups', 'profiles', 'routes_box'):
        (cfg / sub).mkdir(parents=True)
    settings = dict(remember_enable=mode in ('pending_start', 'pending_stop'), remember_id=0, spmode2=[],
                    sub_auto_update=0, inbound_address='127.0.0.1', language=1,
                    traffic_loop_interval=500 if mode in ('owned_core_timeout', 'mac_stop_failure', 'exit_cancelled', 'rpc_cancelled', 'wait_cancelled', 'kill_cancelled') else 0)
    (cfg / 'groups/nekobox.json').write_text(json.dumps(settings))
    if mode in ('pending_start', 'pending_stop', 'owned_core_timeout', 'mac_stop_failure', 'exit_cancelled', 'rpc_cancelled', 'wait_cancelled', 'kill_cancelled'):
        (cfg / 'groups/pm.json').write_text('{"groups":[0]}')
        (cfg / 'groups/0.json').write_text('{"id":0,"name":"quit test","order":[0]}')
        (cfg / 'profiles/0.json').write_text(json.dumps(dict(type='socks', id=0, gid=0,
            bean=dict(_v=0, name='synthetic', addr='127.0.0.1', port=9, v=5))))
    env = os.environ.copy()
    env.update(NKR_QUIT_TEST_MODE=mode, NKR_QUIT_TEST_ROOT=str(root), QT_QPA_PLATFORM=env.get('NKR_QUIT_TEST_QPA', 'offscreen'))
    for key in ('HOME', 'CFFIXED_USER_HOME', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME', 'XDG_DATA_HOME', 'TMPDIR'):
        path = root / key
        path.mkdir()
        env[key] = str(path)
    core = None
    gui = None
    try:
        with (root / 'gui.log').open('w') as log:
            gui = subprocess.Popen([str(executable), '-appdata', str(root / 'appdata')],
                                   env=env, stdout=log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 15
            while 'QUIT_PROBE_READY' not in (root / 'gui.log').read_text():
                assert gui.poll() is None, 'GUI exited during startup: ' + startup_diagnostics(root, gui)
                assert time.monotonic() < deadline, 'probe readiness timeout: ' + startup_diagnostics(root, gui)
                time.sleep(.01)
            if mode == 'mac_stop_failure':
                evidence = (root / 'gui.log').read_text()
                assert 'stop=0 released=1 tun_after_stop=1 shutdown=0 tun_after_shutdown=1 dialogs_restored=1' in evidence
                assert 'MAC_STOP_RECOVERY_OK' in evidence
            if mode in ('owned_core_timeout', 'mac_stop_failure', 'exit_cancelled', 'rpc_cancelled', 'wait_cancelled', 'kill_cancelled'):
                assert 'REFUSAL_RECOVERY prepare_exit=0 save_blocked=0 saved=1 dialogs=0 ui_open=1 started=1 sampling=1' in (root / 'gui.log').read_text()
                saved = json.loads((cfg / 'groups/nekobox.json').read_text())
                assert saved['test_url'] == 'https://shutdown-recovery.invalid/'
            if mode == 'owned_core_timeout':
                assert 'first=0 second=0 first_restored=1 second_restored=1' in (root / 'gui.log').read_text()
                assert 'ACTIVE_CORE_TIMEOUT accepted=0 restored=1 exit_started=1 core_running=1' in (root / 'gui.log').read_text()
            if mode.rsplit('_', 1)[0] in ('exit', 'wait', 'kill', 'complete'):
                phase, order = mode.rsplit('_', 1)
                cancelled = order == 'cancelled'
                expected = (f'SHUTDOWN_RACE phase={phase} cancel_wins={int(cancelled)} '
                            f'accepted=0 restored=1 phase_finished={int(not cancelled)} '
                            f'cancel_before_finished={int(cancelled)}')
                assert expected in (root / 'gui.log').read_text()
                if mode == 'exit_cancelled':
                    assert not (root / 'exit_requests').exists(), 'stale callback issued Exit'
                elif mode == 'exit_committed':
                    assert (root / 'exit_requests').read_text() == '1'
            if mode == 'rpc_cancelled':
                assert 'RPC_ISSUE_RACE accepted=0 restored=1 issued=0 core_running=1' in (root / 'gui.log').read_text()
                assert not (root / 'exit_requests').exists()
            if mode == 'mac_start_recovery':
                assert 'MAC_START_RECOVERY_OK' in (root / 'gui.log').read_text()
            core = int((root / 'core.pid').read_text())
            if mode in ('exit_cancelled', 'rpc_cancelled', 'wait_cancelled', 'kill_cancelled'):
                assert alive(core), 'cancelled shutdown killed the reused core'
            started = time.monotonic()
            gui.send_signal(signal.SIGTERM if route == 'term' else signal.SIGINT)
            # Send additional signals while the slow owned-core wait is active.
            time.sleep(.1)
            if gui.poll() is None:
                for _ in range(4):
                    gui.send_signal(signal.SIGTERM if route == 'term' else signal.SIGINT)
            rc = gui.wait(timeout=4)
            elapsed = time.monotonic() - started
            core_alive = alive(core)
            print(json.dumps(dict(mode=mode, route=route, rc=rc, quit_seconds=round(elapsed, 3),
                                  core_alive_at_gui_exit=core_alive, evidence=str(root))), flush=True)
            assert rc == 0 and not core_alive, 'GUI failed to reap its owned core'
            assert elapsed < 3, 'shutdown blocked by a pending operation dialog'
            if mode in ('exit_cancelled', 'rpc_cancelled', 'exit_committed'):
                assert (root / 'exit_requests').read_text() == ('2' if mode == 'exit_committed' else '1'), 'normal Quit did not issue Exit'
    finally:
        if gui:
            print(startup_diagnostics(root, gui), flush=True)
        if gui and gui.poll() is None:
            gui.kill()
            gui.wait(timeout=5)
        pid_file = root / 'core.pid'
        if core is None and pid_file.exists():
            core = int(pid_file.read_text())
        if core and alive(core):
            os.kill(core, signal.SIGKILL)
            deadline = time.monotonic() + 5
            while alive(core) and time.monotonic() < deadline:
                time.sleep(.01)
            assert not alive(core), "owned fake core was not reaped after cleanup"
        print((root / 'gui.log').read_text(), flush=True)


if __name__ == '__main__':
    if Path(sys.argv[0]).name == 'nekobox_core':
        root = Path(os.environ['NKR_QUIT_TEST_ROOT'])
        with (root / 'core.stderr').open('w', buffering=1) as stderr, contextlib.redirect_stderr(stderr):
            try:
                fake_core()
            except Exception:
                (root / 'core.status').write_text('failed')
                traceback.print_exc()
                sys.exit(1)
    else:
        run_gui(Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3])
