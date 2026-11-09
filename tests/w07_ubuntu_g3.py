#!/usr/bin/env python3
"""Real helper/core/firewall evidence on a disposable hosted Ubuntu runner.

No firewall/core mocks. A temporary, explicit polkit rule grants two disposable
accounts /bin/bash execution. This tests pkexec and real requester UIDs, NOT
interactive authentication, GUI behavior, or desktop session authorization.
"""
import hashlib
import http.server
import json
import os
from pathlib import Path
import pwd
import re
import shutil
import signal
import subprocess
import sys
import threading
import time


def run(*args, expected=0):
    print('+', ' '.join('<embedded-script>' if str(arg).startswith('#!/bin/bash') else str(arg) for arg in args), flush=True)
    p = subprocess.run(list(map(str, args)), stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True, timeout=20)
    print(p.stdout, end='', flush=True)
    assert p.returncode == expected, (p.returncode, expected, args)
    return p.stdout


def until(check, timeout=10):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if check():
            return
        time.sleep(.02)
    raise AssertionError('real Ubuntu readiness/cleanup deadline exceeded')


def identity(pid):
    try:
        data = Path('/proc/%s/stat' % pid).read_text()
        fields = data[data.rfind(') ') + 2:].split()
        return None if fields[0] == 'Z' else '%s:%s' % (pid, fields[19])
    except (FileNotFoundError, ProcessLookupError):
        return None


def alive(token):
    return identity(token.split(':')[0]) == token


def main():
    assert os.geteuid() == 0 and os.environ.get('GITHUB_ACTIONS') == 'true'
    assert 'ID=ubuntu' in Path('/etc/os-release').read_text()
    script_path, binary, evidence = map(lambda p: Path(p).resolve(), sys.argv[1:])
    evidence.mkdir(parents=True, exist_ok=True)
    script = script_path.read_text()
    state = Path('/run/nekoray-vpn.state')
    assert not state.exists(), 'refuse to touch a pre-existing VPN instance'
    policy = Path('/etc/polkit-1/rules.d/00-nekoray-g3.rules')
    assert not policy.exists()
    accounts = ['nkrg3a', 'nkrg3b']
    spawned = []
    tokens = set()
    foreign = []
    work = evidence / "install space 'quote' `ticks` $(touch injected)"
    work.mkdir(mode=0o700)
    log = (evidence / 'core.log').open('wb')
    families = ['iptables', 'ip6tables']
    sequence = 0
    summary = []

    def record():
        return dict(line.split('=', 1) for line in state.read_text().splitlines())

    def capture(label):
        for command in ['iptables-save', 'ip6tables-save']:
            (evidence / (label + '.' + command)).write_text(run(command))
        (evidence / (label + '.addresses')).write_text(run('ip', '-j', 'address'))
        (evidence / (label + '.routes4')).write_text(run('ip', '-j', 'route', 'show', 'table', 'all'))
        (evidence / (label + '.routes6')).write_text(run('ip', '-6', '-j', 'route', 'show', 'table', 'all'))
        if state.exists():
            (evidence / (label + '.state')).write_text(state.read_text())

    def control(mode, user='nkrg3a', expected=0):
        return run('runuser', '-u', user, '--', '/usr/bin/pkexec',
                   '--disable-internal-agent', '/bin/bash', '-c', script,
                   'nekoray-vpn', mode, expected=expected)

    def firewall(family):
        # Save full raw snapshots separately. Generated timestamps and live
        # chain counters are not changes to firewall policy/rules.
        return '\n'.join(re.sub(r'\[\d+:\d+\]', '[counters]', line)
                         for line in run(family + '-save').splitlines()
                         if not line.startswith('#'))

    def clean():
        until(lambda: not state.exists())
        for command in families:
            current = firewall(command)
            assert current == baseline[command], (command, 'firewall residue')
        assert not Path('/sys/class/net/nkrg3').exists(), 'Tun interface residue'
        for item in tokens:
            assert not alive(item), ('recorded identity survived', item)
        assert not (work / 'injected').exists()
        assert not (evidence / 'injected').exists()

    def start():
        nonlocal sequence
        sequence += 1
        p = subprocess.Popen(
            ['runuser', '-u', 'nkrg3a', '--', '/usr/bin/pkexec',
             '--disable-internal-agent', '/bin/bash', '-c', script,
             'nekoray-vpn', str(core), str(config), str(work)],
            stdout=log, stderr=subprocess.STDOUT)
        spawned.append(p)
        def ready():
            if p.poll() is not None:
                raise AssertionError('core exited early; see core.log')
            if not state.exists():
                return False
            r = record()
            return bool(r.get('CORE')) and Path('/sys/class/net/nkrg3').exists()
        until(ready)
        r = record()
        assert r['UID'] == str(uid), r
        assert alive(r['SUP']) and alive(r['CORE'])
        tokens.update([r['SUP'], r['CORE']])
        for command in families:
            text = run(command + '-save')
            assert r['CMT'] in text, (command, 'owned rule missing')
        capture('cycle-%02d-live' % sequence)
        return p, r

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b'W07 real Tun traffic\n')
        def log_message(self, *args):
            pass

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        run('uname', '-a')
        print(Path('/etc/os-release').read_text(), flush=True)
        run('/bin/bash', '--version')
        run('/usr/bin/pkexec', '--version')
        for family in families:
            run(family, '--version')
        for path in [script_path, binary]:
            print('SHA256', hashlib.sha256(path.read_bytes()).hexdigest(), path, flush=True)
        run(binary, 'version')
        capture('initial')
        for user in accounts:
            run('useradd', '--no-create-home', '--shell', '/bin/sh', user)
        uid = pwd.getpwnam('nkrg3a').pw_uid
        # Read/traverse evidence but keep core/config working directory private.
        evidence.chmod(0o755)
        for parent in evidence.parents:
            if str(parent) == '/':
                break
            assert os.stat(parent).st_mode & 0o001, ('not traversable', parent)
        os.chown(work, uid, pwd.getpwnam('nkrg3a').pw_gid)
        core = work / "nekobox_core 'quoted' `ticks` $(touch injected)"
        shutil.copy2(binary, core)
        core.chmod(0o755)
        config = work / "config 'quoted' `ticks` $(touch injected).json"
        config.write_text(json.dumps({
            'log': {'level': 'info'},
            'inbounds': [{'type': 'tun', 'interface_name': 'nkrg3',
                          'inet4_address': ['172.19.0.1/30'],
                          'inet6_address': ['fdfe:dcba:9876::1/126'],
                          'auto_route': False, 'stack': 'gvisor'}],
            'outbounds': [{'type': 'direct', 'override_address': '127.0.0.1',
                           'override_port': server.server_port}]
        }))
        config.chmod(0o600)
        os.chown(config, uid, pwd.getpwnam('nkrg3a').pw_gid)
        assert config.stat().st_mode & 0o777 == 0o600
        assert work.stat().st_mode & 0o777 == 0o700
        run(core, 'check', '-c', config)
        # Probe the real unconfigured privilege path without an agent. Record
        # the actual result; this is not a desktop cancellation experiment.
        probe = subprocess.run(['runuser', '-u', 'nkrg3a', '--', '/usr/bin/pkexec',
                                '--disable-internal-agent', '/usr/bin/id'],
                               capture_output=True, text=True, timeout=10)
        (evidence / 'pkexec-no-agent.txt').write_text(
            'rc=%d\n%s%s' % (probe.returncode, probe.stdout, probe.stderr))
        assert not state.exists()
        policy.write_text('''polkit.addRule(function(action, subject) {
    if (action.id == "org.freedesktop.policykit.exec" &&
        action.lookup("program") == "/bin/bash" &&
        (subject.user == "nkrg3a" || subject.user == "nkrg3b")) {
        return polkit.Result.YES;
    }
});
''')
        policy.chmod(0o644)
        run('systemctl', 'restart', 'polkit')
        run('runuser', '-u', 'nkrg3a', '--', '/usr/bin/pkexec',
            '--disable-internal-agent', '/bin/bash', '-c',
            'id -u; printf "PKEXEC_UID=%s\\n" "$PKEXEC_UID"')

        # Real equivalent untagged and foreign-comment rules in both families.
        for command, source, dest in [('iptables', '172.19.0.2', '172.19.0.1'),
                                      ('ip6tables', 'fdfe:dcba:9876::2', 'fdfe:dcba:9876::1')]:
            for comment in [None, 'w07-g3-foreign']:
                args = ['INPUT', '-s', source, '-d', dest, '-p', 'tcp', '-j', 'ACCEPT']
                if comment:
                    args += ['-m', 'comment', '--comment', comment]
                run(command, '-I', *args)
                foreign.append((command, args))
        baseline = {family: firewall(family) for family in families}
        capture('foreign-baseline')

        # Literal arguments are exercised by the actual core's successful
        # config parse and working-directory use, not by a stand-in executable.
        # Connect across the real Tun to a local HTTP oracle via direct outbound.
        for cycle in range(2):
            p, r = start()
            for host in ['172.19.0.2', '[fdfe:dcba:9876::2]']:
                response = run('curl', '--noproxy', '*', '--connect-timeout', '3',
                               '--max-time', '5', 'http://%s:%d/' % (host, server.server_port))
                assert response.endswith('W07 real Tun traffic\n')
            control('--terminate')
            assert p.wait(timeout=10) == 0
            clean()
            capture('normal-%d-after' % cycle)
        summary += ['G3-1 RUN: hostile paths through actual pkexec and pinned core',
                    'G3-2 NOT_RUN: interactive polkit denial/cancellation and GUI rapid start/stop need a desktop agent',
                    'G3-3 RUN: two real core/Tun IPv4+IPv6 HTTP start-stop cycles; both firewall snapshots restored']

        for sig in [signal.SIGINT, signal.SIGTERM, signal.SIGHUP]:
            p, r = start()
            os.kill(int(r['SUP'].split(':')[0]), sig)
            assert p.wait(timeout=10) == 0
            clean()
        p, r = start()
        os.kill(int(r['CORE'].split(':')[0]), signal.SIGKILL)
        assert p.wait(timeout=10) != 0
        clean()
        capture('core-crash-after')
        for recover in ['stop', 'restart']:
            p, r = start()
            os.kill(int(r['SUP'].split(':')[0]), signal.SIGKILL)
            p.wait(timeout=10)
            assert alive(r['CORE']), 'no live orphan to exercise recovery'
            if recover == 'stop':
                control('--stop')
                control('--stop')
            else:
                replacement, new_record = start()
                assert not alive(r['CORE'])
                control('--terminate')
                assert replacement.wait(timeout=10) == 0
            clean()
            capture('supervisor-crash-%s-after' % recover)
        summary.append('G3-4 RUN: INT/TERM/HUP, core SIGKILL, supervisor SIGKILL; real orphan recovered by --stop and by next start')

        # Actual second core is the same-name decoy, with a different config.
        decoy_config = work / 'decoy.json'
        decoy_config.write_text('{"outbounds":[{"type":"direct"}]}')
        decoy = subprocess.Popen([str(core), 'run', '-c', str(decoy_config)],
                                 stdout=log, stderr=subprocess.STDOUT)
        spawned.append(decoy)
        decoy_token = identity(decoy.pid)
        assert decoy_token
        p, r = start()
        duplicate = subprocess.run(
            ['runuser', '-u', 'nkrg3a', '--', '/usr/bin/pkexec', '--disable-internal-agent',
             '/bin/bash', '-c', script, 'nekoray-vpn', str(core), str(config), str(work)],
            stdout=log, stderr=subprocess.STDOUT, timeout=10)
        assert duplicate.returncode == 75
        control('--stop', expected=75)
        assert alive(r['CORE']) and alive(decoy_token)
        assert record() == r
        live_snapshot = {family: firewall(family) for family in families}
        # No PKEXEC_UID injection: two real accounts traverse real pkexec.
        control('--terminate', user='nkrg3b', expected=77)
        assert record() == r and alive(r['CORE'])
        for family in families:
            assert firewall(family) == live_snapshot[family]
        control('--terminate')
        assert p.wait(timeout=10) == 0
        clean()
        assert alive(decoy_token)
        decoy.send_signal(signal.SIGINT)
        assert decoy.wait(timeout=10) == 0
        capture('isolation-after')
        summary += ['G3-5 RUN: duplicate=75, live --stop=75; real foreign IPv4/IPv6 rules and same-name real-core decoy survived',
                    'G3-6 NOT_RUN (desktop session portion): real pkexec accounts A/B tested (B=77, A=0); two desktop login sessions and interactive elevation unavailable']
        print('\n'.join(summary), flush=True)
        (evidence / 'summary.txt').write_text('\n'.join(summary) + '\n')
    finally:
        # Only recorded identities and Popen handles created here are signalled.
        # Firewall cleanup uses the reviewed helper; never flush/restore a table.
        if state.exists():
            current = record()
            tokens.update(value for key, value in current.items()
                          if key in ('SUP', 'CORE') and value)
        for item in tokens:
            if alive(item):
                os.kill(int(item.split(':')[0]), signal.SIGKILL)
        for p in spawned:
            if p.poll() is None:
                p.kill()
            p.wait(timeout=10)
        if state.exists() and policy.exists():
            control('--stop', expected=0)
        for command, args in reversed(foreign):
            run(command, '-D', *args)
        if policy.exists():
            policy.unlink()
            run('systemctl', 'restart', 'polkit')
        for user in accounts:
            try:
                pwd.getpwnam(user)
            except KeyError:
                continue
            run('userdel', user)
        server.shutdown()
        log.close()
        capture('final')
        for family in families:
            before = (evidence / ('initial.' + family + '-save')).read_text()
            before = '\n'.join(re.sub(r'\[\d+:\d+\]', '[counters]', line) for line in before.splitlines() if not line.startswith('#'))
            assert before == firewall(family)
        assert not state.exists()
        assert not list(Path('/run').glob('nekoray-vpn.ready.*'))
        for p in evidence.rglob('*'):
            if p.is_file():
                print('SHA256', hashlib.sha256(p.read_bytes()).hexdigest(), p.relative_to(evidence), flush=True)


if __name__ == '__main__':
    main()
