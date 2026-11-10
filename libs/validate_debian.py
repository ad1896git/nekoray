#!/usr/bin/env python3
"""Inspect the built Debian payload without extracting or executing it."""
import argparse
import io
from pathlib import Path
import subprocess
import tarfile


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
            if entry.mode & 0o6002:
                raise ValueError(f'{entry.name}: unsafe mode {entry.mode:04o}')
            count += 1
    if not count:
        raise ValueError('Empty Debian payload')
    print(f'Validated Debian payload: {count} entries, all 0:0; no world-writable or setuid/setgid modes')


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
