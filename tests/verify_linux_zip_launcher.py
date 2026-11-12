#!/usr/bin/env python3
"""Exercise the archived launcher with a harmless child; never load bundled Qt."""
import argparse
import os
from pathlib import Path
import re
import signal
import stat
import subprocess
import tempfile
import zipfile


STUB = b'''#!/bin/sh
set -eu
printf '%s\\0' "$0" "$PWD" "$LD_LIBRARY_PATH" "$$" "$@" > "$SMOKE_RECORD"
exit 0
'''
ARGS = ['-appdata', 'argument with spaces', '', 'literal * ? $HOME \' "', 'line\nbreak']


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def verify(archive_path):
    with zipfile.ZipFile(archive_path) as archive:
        entries = [item for item in archive.infolist() if item.filename == 'nekoray/launcher']
        require(len(entries) == 1, 'ZIP must contain exactly one nekoray/launcher')
        mode = entries[0].external_attr >> 16
        require(stat.S_ISREG(mode) and stat.S_IMODE(mode) == 0o755,
                f'launcher must be a regular 0755 file, got {oct(mode)}')
        launcher_bytes = archive.read(entries[0])

    # Extract only the launcher, preserving its archived mode. The child and
    # empty library directory are fixtures, so no packaged ELF/GUI code runs.
    with tempfile.TemporaryDirectory(prefix='launcher-smoke-', dir=archive_path.parent) as tmp:
        root = Path(tmp).resolve()
        bundle = root / 'package with spaces' / 'nekoray'
        (bundle / 'usr/lib').mkdir(parents=True)
        launcher = bundle / 'launcher'
        launcher.write_bytes(launcher_bytes)
        launcher.chmod(stat.S_IMODE(mode))
        child = bundle / 'nekobox'
        child.write_bytes(STUB)
        child.chmod(0o755)
        outside = root / 'different cwd'
        outside.mkdir()
        deeper = outside / 'deeper caller'
        deeper.mkdir()
        (outside / 'relative link').symlink_to(os.path.relpath(launcher, outside))
        (outside / 'absolute link').symlink_to(launcher)
        record = root / 'child-record'
        library = str(bundle / 'usr/lib')
        invocations = [
            ('direct', './launcher', bundle),
            ('other-cwd', str(launcher), outside),
            ('relative-symlink', './relative link', outside),
            ('absolute-symlink', './absolute link', outside),
            ('relative-symlink-other-cwd', str(outside / 'relative link'), deeper),
        ]
        environments = [
            ('unset', None, [library]),
            ('empty', '', [library]),
            ('mixed', ':/existing/first::;/existing/second;:relative:$ORIGIN:;',
             [library, '/existing/first', '/existing/second']),
        ]
        for name, command, cwd in invocations:
            for env_name, inherited, expected_library in environments:
                label = f'{name}/{env_name}'
                record.unlink(missing_ok=True)
                env = {'PATH': os.defpath, 'SMOKE_RECORD': str(record)}
                if inherited is not None:
                    env['LD_LIBRARY_PATH'] = inherited
                process = subprocess.Popen([command, *ARGS], cwd=cwd, env=env,
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                           start_new_session=True)
                try:
                    stdout, stderr = process.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    # Descendants can hold the pipes after the launcher exits.
                    # Kill the whole session's group, then bound draining/reaping
                    # too, even if a descendant has escaped that group.
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    try:
                        process.communicate(timeout=1)
                    except subprocess.TimeoutExpired:
                        process.stdout.close()
                        process.stderr.close()
                        process.wait(timeout=1)
                    raise RuntimeError(f'{label}: launcher timed out') from None
                require(process.returncode == 0,
                        f'{label}: exit {process.returncode}; stdout={stdout!r}; stderr={stderr!r}')
                require(record.is_file(), f'{label}: launcher did not execute the stub')
                fields = record.read_bytes().split(b'\0')
                expected = [str(child), str(cwd), ':'.join(expected_library),
                            str(process.pid), *ARGS, '']
                require(fields == [os.fsencode(value) for value in expected],
                        f'{label}: path/cwd/library/PID/argv mismatch: {fields!r}')
                elements = re.split(b'[:;]', fields[2])
                require(elements[0] == os.fsencode(library) and all(elements),
                        f'{label}: invalid LD_LIBRARY_PATH: {fields[2]!r}')
                print(f'PASS {label}: stub exec, path, cwd, library path and argv', flush=True)
    print(f'PASS Linux ZIP launcher runtime smoke ({len(invocations) * len(environments)} cases)')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    verify(parser.parse_args().archive.resolve())
