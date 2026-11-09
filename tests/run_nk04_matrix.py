#!/usr/bin/env python3
"""Fail-closed NK04 acceptance on the core's unchanged final dependency graph."""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import signal
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / 'go/cmd/nekobox_core'
TAGS = 'with_clash_api,with_gvisor,with_quic,with_wireguard,with_utls,with_ech'
ROWS = {variant + suffix: (tags, race)
        for variant, tags in (('default', ''), ('debug', 'debug'),
                              ('low_memory', 'with_low_memory'),
                              ('debug_low_memory', 'debug,with_low_memory'))
        for suffix, race in (('', False), ('_race', True))}
TOPS = {'TestNK04' + name for name in (
    'Hysteria2TCP', 'Hysteria2UDP', 'Hysteria2WrongPassword',
    'Hysteria2Salamander', 'Hysteria2TCPOnly', 'Hysteria2Reconnect',
    'Hysteria1TCP', 'Hysteria1WrongAuth', 'TUICTCP', 'TUICWrongPassword',
    'TUICUnknownUUID', 'TUICUDPStream', 'TUICUDPDatagram',
    'Hysteria2ConcurrentUDP', 'Hysteria2PortHopping',
    'MatrixStreamLimit', 'MatrixPacketBoundaries', 'MatrixPacketCancelTimeout',
    'MatrixMalformedTimeoutReconnect', 'MatrixUDPStreamPeerCloseReconnect')}
EXPECTED = set(TOPS)
EXPECTED.update('TestNK04Hysteria2Salamander/' + s for s in ('off', 'on', 'wrong_password'))
for mode, limit in (('hysteria2', 4096), ('tuic_datagram', 65535), ('tuic_stream', 65535)):
    parent = 'TestNK04MatrixPacketBoundaries/' + mode
    EXPECTED.add(parent)
    EXPECTED.update(parent + '/' + str(n) for n in (0, 1, limit - 1, limit, limit + 1))
EXPECTED.update('TestNK04MatrixPacketCancelTimeout/timeout_' + s for s in ('false', 'true'))
EXPECTED.update('TestNK04MatrixMalformedTimeoutReconnect/wire_' + s
                for s in ('', 'ffff', '05ff', '050001'))
EXPECTED.update('TestNK04MatrixUDPStreamPeerCloseReconnect/' + s
                for s in ('peer_close', 'local_client_close'))
LEAVES = {n for n in EXPECTED if not any(p.startswith(n + '/') for p in EXPECTED)}
PATTERN = '^(' + '|'.join(sorted(TOPS)) + ')$'


def environment(output):
    # Do not inherit GOFLAGS, saved GOENV, GORACE redirects/exit codes, cross-target
    # settings, runtime experiments, or opt-ins from the invoking shell.
    env = {k: os.environ[k] for k in ('PATH', 'HOME', 'USER', 'LOGNAME', 'SDKROOT')
           if k in os.environ}
    for key in ('GOPATH', 'GOCACHE', 'GOMODCACHE'):
        value = os.environ.get(key, '')
        if not value or not Path(value).is_absolute():
            raise ValueError('Set an isolated absolute ' + key)
        env[key] = value
    tmp = output / 'tmp'
    tmp.mkdir(exist_ok=True)
    env.update(GOTOOLCHAIN='local', GOWORK='off', GOENV='off', GOFLAGS='',
               CGO_ENABLED='1', GOPROXY='off', GOSUMDB='off',
               GORACE='log_path=stderr exitcode=66', TMPDIR=str(tmp))
    return env


def execute(command, path, env, timeout=300, stdin=None):
    with path.open('wb') as stream:
        p = subprocess.Popen(command, cwd=CORE, env=env, stdout=stream,
                             stderr=subprocess.STDOUT, stdin=stdin,
                             start_new_session=True)
        try:
            return p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            return -signal.SIGKILL


def classify(events, status, raw):
    errors = []
    if status != 0:
        errors.append('binary exit ' + str(status))
    if any(s in raw for s in ('WARNING: DATA RACE', 'race detected during execution',
                               'panic:', 'fatal error:', '--- FAIL:', '--- SKIP:')):
        errors.append('failure diagnostic in raw output')
    runs = Counter(e['Test'] for e in events if e.get('Action') == 'run' and 'Test' in e)
    passes = Counter(e['Test'] for e in events if e.get('Action') == 'pass' and 'Test' in e)
    if runs != Counter(EXPECTED) or passes != Counter(EXPECTED):
        errors.append('exact run/pass inventory mismatch')
    if any(e.get('Action') in ('fail', 'skip') for e in events):
        errors.append('failure or skip event')
    package = [e['Action'] for e in events
               if 'Test' not in e and e.get('Action') in ('pass', 'fail', 'skip')]
    if package != ['pass']:
        errors.append('missing unique package pass')
    active = set()
    finished = set()
    for e in events:
        name, action = e.get('Test'), e.get('Action')
        if name is None:
            continue
        if action == 'run':
            parent = name.rpartition('/')[0]
            if name in active or name in finished or (parent and parent not in active):
                errors.append('invalid test lifecycle: ' + name)
            active.add(name)
        elif action == 'pass':
            if name not in active or any(n.startswith(name + '/') for n in active):
                errors.append('invalid completion: ' + name)
            active.discard(name)
            finished.add(name)
    if active:
        errors.append('unfinished tests')
    return errors


def verify_modules(go, output, env):
    path = output / 'modules.json'
    if execute([go, 'list', '-mod=readonly', '-m', '-json', 'all'], path, env):
        raise ValueError('module graph failed; see ' + str(path))
    decoder = json.JSONDecoder()
    remaining = path.read_text()
    modules = {}
    while remaining.strip():
        value, end = decoder.raw_decode(remaining.lstrip())
        remaining = remaining.lstrip()[end:]
        modules[value['Path']] = value
    pins = {'github.com/matsuridayo/libneko': '45e350171b7b',
            'github.com/sagernet/sing-box': '2f78dbe673e2',
            'github.com/sagernet/sing-quic': '201474163bfe'}
    for name, pin in pins.items():
        if not modules[name].get('Replace', {}).get('Version', '').endswith('-' + pin):
            raise ValueError('wrong final pin: ' + name)
    if modules['github.com/sagernet/sing']['Version'] != 'v0.4.3':
        raise ValueError('core must select sing v0.4.3')
    quic = modules['github.com/sagernet/quic-go']
    if quic['Version'] != 'v0.47.0-beta.2':
        raise ValueError('wrong core quic-go version')
    source = Path(quic['Replace']['Dir'])
    sha = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], env=env, text=True).strip()
    if sha != '7101329b86a0c801e643fc8ccf730ec89be10663':
        raise ValueError('wrong quic-go source')
    subprocess.run(['git', '-C', str(source), 'apply', '--reverse', '--check',
                    str(ROOT / 'libs/patches/quic-go-v0.47.0-beta.2-congestion-lock.patch')],
                   env=env, check=True)


def run(row, output, go, env):
    tags, race = ROWS[row]
    binary = output / (row + '.test')
    build = [go, 'test', '-c', '-mod=readonly', '-o', str(binary),
             '-tags=' + TAGS + (',' + tags if tags else '')]
    if race:
        build.append('-race')
    if sys.platform == 'darwin':
        build.append('-ldflags=-linkmode=external')
    build.append('.')
    status = execute(build, output / (row + '.build.log'), env, timeout=600)
    errors = []
    events = []
    if status:
        errors.append('build exit ' + str(status))
    else:
        raw_path = output / (row + '.raw')
        status = execute([str(binary), '-test.v=test2json', '-test.count=1',
                          '-test.timeout=180s', '-test.run=' + PATTERN], raw_path, env, timeout=240)
        json_path = output / (row + '.jsonl')
        with raw_path.open('rb') as source:
            converted = execute([go, 'tool', 'test2json', '-p', 'nekobox_core'],
                                json_path, env, stdin=source)
        if converted:
            errors.append('test2json exit ' + str(converted))
        else:
            try:
                events = [json.loads(line) for line in json_path.read_text().splitlines()]
                raw = raw_path.read_text()
                # test2json consumes Go's marker-prefixed NAME control records.
                visible = ''.join(line.replace('\x16', '') for line in raw.splitlines(keepends=True)
                                  if not line.startswith('\x16=== NAME  '))
                if ''.join(e.get('Output', '') for e in events) != visible:
                    errors.append('lossy test2json output')
                errors.extend(classify(events, status, raw))
            except (ValueError, KeyError) as exc:
                errors.append('invalid test output: ' + str(exc))
    passed = {e['Test'] for e in events if e.get('Action') == 'pass' and 'Test' in e}
    record = dict(row=row, result='FAIL' if errors else 'PASS', exit=status,
                  passes=len(passed), top_passes=len(passed & TOPS),
                  leaves=len(passed & LEAVES), expected_passes=len(EXPECTED),
                  expected_leaves=len(LEAVES), errors=errors, build=build)
    (output / (row + '.result.json')).write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record), flush=True)
    return not errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--row', choices=ROWS)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--go', default='go')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    env = environment(output)
    version = subprocess.check_output([args.go, 'version'], env=env, text=True)
    if not version.startswith('go version go1.22.12 '):
        raise ValueError('NK04 requires Go 1.22.12: ' + version)
    verify_modules(args.go, output, env)
    for row in [args.row] if args.row else ROWS:
        if not run(row, output, args.go, env):
            return 1  # Stop on the first unexplained failure; never retry a row.
    return 0


if __name__ == '__main__':
    sys.exit(main())
