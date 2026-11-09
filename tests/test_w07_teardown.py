#!/usr/bin/env python3
"""Force failure after gate stop with its held delay paused before release.

Run the target sandbox's exact gate regression and outer finally, without its
other cases. The controller retains the leaked identity so even a red run cleans
up its own stopped helper. No firewall commands or privileged operations run.
"""
import ast
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time


def token(pid):
    try:
        data = Path('/proc/%d/stat' % pid).read_text()
        fields = data[data.rfind(') ') + 2:].split()
        return None if fields[0] == 'Z' else '%d:%s' % (pid, fields[19])
    except (FileNotFoundError, ProcessLookupError):
        return None


def main():
    target, script = map(lambda p: Path(p).resolve(), sys.argv[1:])
    tree = ast.parse(target.read_text(), filename=str(target))
    run = next(n for n in tree.body if isinstance(n, ast.FunctionDef)
               and n.name == 'run_tests')
    start = next(i for i, n in enumerate(run.body)
                 if isinstance(n, ast.Assign)
                 and isinstance(n.value, ast.Call)
                 and any(k.arg == 'HOLD_GATE_SLEEP' for k in n.value.keywords))
    end = next(i for i in range(start + 1, len(run.body))
               if isinstance(run.body[i], ast.For))
    body = run.body[start:end]
    # Stop the held wrapper just before the finally publishes release-sleep.
    # A second injected failure after that finally forces outer teardown now,
    # with no 200-round race loop to hide the leak.
    pause = ast.parse('''
held = case.root / 'held-sleep'
pid = int(read_when_ready(held).split(':')[0])
identity = token(pid)
Path(os.environ['W07_TEARDOWN_ID']).write_text(identity)
os.kill(pid, signal.SIGSTOP)
''').body
    for i, n in enumerate(body):
        if isinstance(n, ast.Try):
            n.body.extend(pause)
            break
    else:
        raise AssertionError('gate regression finally not found')
    body += ast.parse("raise AssertionError('injected failure after gate stop')").body
    run.body = body
    ast.fix_missing_locations(tree)
    with tempfile.TemporaryDirectory(prefix='w07-teardown-controller-') as temp:
        path = Path(temp)
        worker = path / 'worker.py'
        worker.write_text(ast.unparse(tree))
        identity_file = path / 'identity'
        identity = None
        try:
            result = subprocess.run(
                [sys.executable, str(worker), str(script)],
                env=dict(os.environ, W07_TEARDOWN_ID=str(identity_file)),
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, timeout=25)
            print(result.stdout, end='')
            assert identity_file.exists(), 'did not reach held-sleep teardown'
            identity = identity_file.read_text()
            pid = int(identity.split(':')[0])
            assert 'injected failure after gate stop' in result.stdout
            assert result.returncode != 0, 'injected assertion did not run'
            # If old teardown removed the release marker first, this resumes
            # a wrapper that will poll forever. Fixed teardown already reaped it.
            if token(pid) == identity:
                os.kill(pid, signal.SIGCONT)
            end = time.monotonic() + 1
            while token(pid) == identity and time.monotonic() < end:
                time.sleep(.01)
            assert token(pid) != identity, 'held-sleep survived failure teardown: ' + identity
            print('W07 failure teardown: PASS (paused held sleep reclaimed)')
        finally:
            if identity is None and identity_file.exists():
                identity = identity_file.read_text()
            if identity and token(int(identity.split(':')[0])) == identity:
                os.kill(int(identity.split(':')[0]), signal.SIGKILL)


if __name__ == '__main__':
    main()
