#!/usr/bin/env python3
"""Real GUI entry/owned-QProcess regression, with a loopback-only fake RPC core.

No real proxy, TUN or system proxy is enabled. All config and HOME paths are
inside the CTest build tree. Every child is waited for, including failure paths.
"""
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


def frame(kind, flags, stream, payload=b''):
    return len(payload).to_bytes(3, 'big') + bytes([kind, flags]) + struct.pack('!I', stream) + payload


def fake_core():
    root = Path(os.environ['NKR_QUIT_TEST_ROOT'])
    (root / 'core.pid').write_text(str(os.getpid()))
    server = socket.socket()
    server.bind(('127.0.0.1', int(sys.argv[sys.argv.index('-port') + 1])))
    server.listen()
    print('grpc server listening', flush=True)

    def serve(conn):
        data = b''
        while len(data) < 24:
            part = conn.recv(24 - len(data))
            if not part:
                return
            data += part
        conn.sendall(frame(4, 0, 0))
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
                if kind == 4 and not flags & 1:
                    conn.sendall(frame(4, 1, 0))
                elif kind == 6 and not flags & 1:
                    conn.sendall(frame(6, 1, 0, payload))
                elif kind in (0, 1) and flags & 1 and stream:
                    # A pending Start never completes. In the Stop case Start
                    # succeeds, subsequent Stop/Exit remain pending. This peer
                    # deliberately never exits itself: production must reap it.
                    with request_lock:
                        first = not (root / 'request').exists()
                        (root / 'request').touch()
                    if os.environ['NKR_QUIT_TEST_MODE'] == 'pending_stop' and first:
                        conn.sendall(frame(1, 4, stream, b'\x88') +
                                     frame(0, 0, stream, b'\0' * 5) +
                                     frame(1, 5, stream, b'\x00\x0bgrpc-status\x010'))
    request_lock = threading.Lock()
    while True:
        conn, _ = server.accept()
        threading.Thread(target=serve, args=(conn,), daemon=True).start()


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


def run_gui(binary, mode, route):
    # Keep evidence on failure; temporary files live under the test build tree.
    root = Path(tempfile.mkdtemp(prefix='quit-gui-', dir=Path.cwd()))
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
    settings = dict(remember_enable=mode != 'idle', remember_id=0, spmode2=[],
                    sub_auto_update=0, inbound_address='127.0.0.1', language=1,
                    traffic_loop_interval=0)
    (cfg / 'groups/nekobox.json').write_text(json.dumps(settings))
    if mode != 'idle':
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
                assert gui.poll() is None, f'GUI exited during startup: {gui.returncode}'
                assert time.monotonic() < deadline, 'probe readiness timeout'
                time.sleep(.01)
            core = int((root / 'core.pid').read_text())
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
    finally:
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
        fake_core()
    else:
        run_gui(Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3])
