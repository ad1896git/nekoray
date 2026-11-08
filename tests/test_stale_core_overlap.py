#!/usr/bin/env python3
"""FU-H: SIGKILL the real GUI; a new CoreProcess must wait for its old core.

Uses the existing loopback authenticated fake core, with the production ten
second parent polling interval. No proxy, TUN or network beyond loopback.
"""
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_coordinated_gui as fixture


def until(predicate, timeout=18):
    deadline = time.monotonic() + timeout
    while not predicate():
        assert time.monotonic() < deadline, 'startup/cleanup deadline exceeded'
        time.sleep(.01)


def core():
    root = Path(os.environ['NKR_QUIT_TEST_ROOT'])
    history = root / 'launches.jsonl'
    prior = [json.loads(line) for line in history.read_text().splitlines()] if history.exists() else []
    old_alive = any(fixture.alive(row['pid']) for row in prior)
    with history.open('a') as output:
        output.write(json.dumps(dict(pid=os.getpid(), parent=os.getppid(),
                                     old_alive=old_alive, at=time.monotonic())) + '\n')
    parent = os.getppid()
    def poll_parent():
        if os.environ.get('NKR_STALE_MODE') == 'refuse':
            while not (root / 'release-core').exists():
                time.sleep(.02)
            os._exit(0)
        while True:
            time.sleep(10)
            if not fixture.alive(parent):
                os._exit(0)
    threading.Thread(target=poll_parent, daemon=True).start()
    fixture.fake_core()


def regression(binary, mode="auth"):
    many = mode in ('many', 'unknown_owner')
    root = Path(tempfile.mkdtemp(prefix='stale-core-', dir=Path.cwd()))
    print('EVIDENCE: ' + str(root), flush=True)
    executable = root / 'nekobox'
    shutil.copy2(binary, executable)
    fake = root / 'nekobox_core'
    fake.write_text('#!' + sys.executable + '\nimport sys\nsys.path.insert(0, ' +
                    repr(str(Path(__file__).resolve().parent)) +
                    ')\nfrom test_stale_core_overlap import core\ncore()\n')
    fake.chmod(0o755)
    (root / 'geoip.db').touch()
    (root / 'geosite.db').touch()
    cfg = root / 'appdata/config'
    for sub in ('groups', 'profiles', 'routes_box'):
        (cfg / sub).mkdir(parents=True)
    (cfg / 'groups/nekobox.json').write_text(json.dumps(dict(
        remember_enable=False, remember_id=-1, spmode2=[], sub_auto_update=0,
        inbound_address='127.0.0.1', language=1, traffic_loop_interval=0)))
    if mode == 'different_appdata':
        shutil.copytree(root / 'appdata', root / 'other-appdata')
    env = os.environ.copy()
    env.update(NKR_QUIT_TEST_ROOT=str(root), NKR_STALE_MODE=mode,
               NKR_QUIT_TEST_MODE='stale_exit_restart' if mode == 'auth' or many or mode == 'different_appdata' else 'idle',
               QT_QPA_PLATFORM=env.get('NKR_QUIT_TEST_QPA', 'offscreen'))
    for key in ('HOME', 'CFFIXED_USER_HOME', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME', 'XDG_DATA_HOME', 'TMPDIR'):
        path = root / key
        path.mkdir()
        env[key] = str(path)
    guis = []
    history = root / 'launches.jsonl'
    def launches():
        return [json.loads(line) for line in history.read_text().splitlines()] if history.exists() else []
    began = time.monotonic()
    try:
        if mode == 'pre_spawn':
            import socket
            with socket.socket() as reserved:
                reserved.bind(('127.0.0.1', 0))
                port = reserved.getsockname()[1]
            record = cfg / '.core-launch.json'
            record.write_text(json.dumps(dict(version=1, config=str(cfg), token='a' * 64,
                                             port=port, pid='0', start='', exe='')))
            record.chmod(0o600)
            # MAC-QUIT/FU-E: an outstanding recovery gate must not delay normal
            # quit or let a queued completion spawn a core after quit was accepted.
            with (root / 'gui-pending.log').open('w') as log:
                pending = subprocess.Popen([str(executable), '-appdata', str(root / 'appdata')],
                                           env=env, stdout=log, stderr=subprocess.STDOUT)
            guis.append(pending)
            until(lambda: 'SIGNAL_HANDLERS_READY' in (root / 'gui-pending.log').read_text() or
                  pending.poll() is not None)
            assert pending.poll() is None, (root / 'gui-pending.log').read_text()
            pending.terminate()
            rc = pending.wait(timeout=3)
            assert rc == 0, ('quit during gate must complete normally: exit=%d\n' % rc +
                             (root / 'gui-pending.log').read_text())
            assert not launches(), 'quit during recovery must not activate a core'
            assert record.exists(), 'unresolved recovery record must survive quit'
            print('QUIT_DURING_GATE_OK no replacement started', flush=True)
        for index in range(1 if mode == 'pre_spawn' else 2):
            (root / 'core.status').unlink(missing_ok=True)
            if many and index == 1:
                incumbent = (cfg / '.core-launch.json').read_bytes()
            appdata = root / ('other-appdata' if mode == 'different_appdata' and index == 1 else 'appdata')
            command = [str(executable), '-appdata', str(appdata)]
            if (many or mode == 'different_appdata') and index == 1:
                command += ['-many']
            if mode == 'unknown_owner' and index == 1:
                # The owner is genuinely alive. Deny only the second GUI's
                # sibling queries, keeping self observation and Qt functional.
                command = ['/usr/bin/sandbox-exec', '-p',
                           '(version 1)(allow default)(deny process-info-pidinfo)'
                           '(allow process-info-pidinfo (target self))'] + command
            with (root / ('gui%d.log' % index)).open('w') as log:
                gui = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
            guis.append(gui)
            if many and index == 1:
                record = cfg / '.core-launch.json'
                until(lambda: 'Startup refused:' in (root / 'gui1.log').read_text() or
                      len(launches()) > 1 or gui.poll() is not None, 8)
                def preserved():
                    assert fixture.alive(launches()[0]['pid']), 'FU-H: same-config -many exited a live GUI core'
                    assert len(launches()) == 1, 'FU-H: same-config -many must refuse a second core'
                    assert record.read_bytes() == incumbent, 'refusal must preserve the incumbent record exactly'
                    assert not (root / 'exit_requests').exists(), 'live owner must never receive Exit'
                    assert guis[0].poll() is None, 'incumbent GUI must remain alive'
                preserved()
                expected = ('Cannot determine whether another NekoBox GUI is using this configuration'
                            if mode == 'unknown_owner' else 'Another NekoBox GUI is using this configuration')
                assert expected in (root / 'gui1.log').read_text()
                assert gui.poll() is None, 'refused GUI must remain responsive'
                # Allow the worker completion/modal warning to be delivered,
                # then check normal quit cannot remove the incumbent record.
                time.sleep(3)
                preserved()
                gui.terminate()
                assert gui.wait(timeout=3) == 0, 'refused GUI must quit normally'
                preserved()
                print('LIVE_OWNER_REFUSED incumbent core and record preserved; no Exit sent', flush=True)
                break
            if mode == 'refuse' and index == 1:
                until(lambda: 'Startup refused:' in (root / 'gui1.log').read_text(), 16)
                assert len(launches()) == 1, 'refused gate must not launch a replacement'
                assert (cfg / '.core-launch.json').exists(), 'refusal must retain the record'
                print('REFUSE_OK old core remains alive; no replacement started', flush=True)
                break
            until(lambda: len(launches()) == index + 1 or gui.poll() is not None)
            assert gui.poll() is None, (root / ('gui%d.log' % index)).read_text()
            until(lambda: (root / 'core.status').exists() and
                  (root / 'core.status').read_text() == 'listening')
            if index == 0 and not many and mode not in ('pre_spawn', 'different_appdata'):
                gui.kill()  # SIGKILL only the GUI, as in KI-2.
                gui.wait(timeout=5)
                assert fixture.alive(launches()[0]['pid']), 'old core must survive GUI SIGKILL'
        observed = launches()
        print(json.dumps(dict(launches=observed, mode=mode,
                              elapsed=time.monotonic() - began)), flush=True)
        if mode == 'different_appdata':
            assert len(observed) == 2 and observed[1]['old_alive'], 'separate appdata must allow both cores'
            assert all(gui.poll() is None for gui in guis)
            assert all(fixture.alive(row['pid']) for row in observed)
            assert not (root / 'exit_requests').exists(), 'separate appdata must not send recovery Exit'
            for index, directory in enumerate(('appdata', 'other-appdata')):
                record = json.loads((root / directory / 'config/.core-launch.json').read_text())
                assert int(record['owner']['pid']) == guis[index].pid and record['owner']['start']
            print('DIFFERENT_APPDATA_OK both GUIs and cores alive; independent owner records', flush=True)
        elif mode == 'auth':
            assert not observed[1]['old_alive'], 'FU-H: replacement core started while stale core was alive'
            assert time.monotonic() - began < 8, 'authenticated Exit should beat the 10s parent poll'
            assert (root / 'exit_requests').exists(), 'the old core must receive authenticated Exit'
            assert not (root / 'auth_rejected_exit').exists(), 'recorded launch auth must match'
        elif mode == 'wait':
            assert not observed[1]['old_alive'], 'self-exit must precede replacement'
            assert time.monotonic() - began >= 9, 'fixture must exercise the parent poll'
        elif mode == 'pre_spawn':
            assert len(observed) == 1 and not observed[0]['old_alive']
            assert time.monotonic() - began >= 11.5, 'pending spawn needs the full initialization bound'
    finally:
        for gui in guis:
            if gui.poll() is None:
                gui.terminate()
                try:
                    gui.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    gui.kill()
                    gui.wait(timeout=5)
        (root / 'release-core').touch()
        # The fake old core self-exits on the same 10s parent poll as production.
        until(lambda: all(not fixture.alive(row['pid']) for row in launches()), 15)
        if mode != 'refuse' and launches():
            assert not (cfg / '.core-launch.json').exists(), 'clean stop must remove its own launch record'
            if mode == 'different_appdata':
                assert not (root / 'other-appdata/config/.core-launch.json').exists()


if __name__ == '__main__':
    regression(Path(sys.argv[1]).resolve(), sys.argv[2] if len(sys.argv) > 2 else "auth")
