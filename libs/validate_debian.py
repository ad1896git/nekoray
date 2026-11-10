#!/usr/bin/env python3
"""Inspect the built Debian payload without extracting or executing it."""
import argparse
import io
from pathlib import Path
import subprocess
import shlex
import tarfile

REPO = Path(__file__).resolve().parents[1]


def validate_script(name, content):
    # Defense against reintroducing pathname-based repairs in a reviewed
    # template. Exact byte comparison below also rejects stale packaged copies.
    forbidden = {'chown', 'chmod', 'rm', 'mv'}
    if name == 'preinst':
        forbidden.add('setcap')
    tokens = shlex.split(content.decode('utf-8'), comments=True)
    if any(Path(token).name in forbidden for token in tokens):
        raise ValueError(f'{name}: forbidden privileged mutation command')


def validate(package):
    members = subprocess.check_output(['ar', 't', str(package)], text=True, timeout=30).splitlines()
    data = [name for name in members if name.startswith('data.tar')]
    if len(data) != 1 or data[0] not in ('data.tar', 'data.tar.gz', 'data.tar.xz', 'data.tar.bz2'):
        raise ValueError('Expected exactly one supported data.tar member')
    payload = subprocess.check_output(['ar', 'p', str(package), data[0]], timeout=30)
    count = 0
    with tarfile.open(fileobj=io.BytesIO(payload), mode='r:*') as archive:
        for entry in archive:
            if (entry.uid, entry.gid) != (0, 0):
                raise ValueError(f'{entry.name}: owner {entry.uid}:{entry.gid}, expected 0:0')
            if not (entry.isfile() or entry.isdir()):
                raise ValueError(f'{entry.name}: unexpected payload type')
            if entry.mode & 0o6022:
                raise ValueError(f'{entry.name}: unsafe mode {entry.mode:04o}')
            count += 1
    if not count:
        raise ValueError('Empty Debian payload')
    control = [name for name in members if name.startswith('control.tar')]
    if len(control) != 1 or control[0] not in ('control.tar', 'control.tar.gz', 'control.tar.xz', 'control.tar.bz2'):
        raise ValueError('Expected exactly one supported control.tar member')
    metadata = subprocess.check_output(['ar', 'p', str(package), control[0]], timeout=30)
    with tarfile.open(fileobj=io.BytesIO(metadata), mode='r:*') as archive:
        for name in ('preinst', 'postinst'):
            scripts = [entry for entry in archive.getmembers() if entry.name.removeprefix('./') == name]
            if len(scripts) != 1:
                raise ValueError(f'Expected exactly one {name}')
            entry = scripts[0]
            if not entry.isfile() or (entry.uid, entry.gid, entry.mode) != (0, 0, 0o755):
                raise ValueError(f'{name}: expected regular root-owned mode 0755 script')
            content = archive.extractfile(entry).read()
            validate_script(name, content)
            if content != (REPO / 'libs/debian' / name).read_bytes():
                raise ValueError(f'{name}: does not match the reviewed fail-closed/capability script')
    print(f'Validated Debian payload: {count} entries, all 0:0; no group/world-write or setuid/setgid modes; maintainer scripts match')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package', type=Path)
    args = parser.parse_args()
    try:
        validate(args.package)
    except (ValueError, OSError, tarfile.TarError, EOFError, subprocess.SubprocessError) as error:
        parser.exit(1, 'Debian payload validation failed: ' + str(error) + '\n')


if __name__ == '__main__':
    main()
