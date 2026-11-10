#!/usr/bin/env python3
"""Positive and negative archive checks using controlled numeric tar metadata."""
import io
from pathlib import Path
import subprocess
import tarfile
import tempfile

REPO = Path(__file__).resolve().parents[1]


def write_deb(path, uid=0, gid=0, mode=0o755, empty=False):
    payload = io.BytesIO()
    with tarfile.open(fileobj=payload, mode='w:xz') as archive:
        if not empty:
            entry = tarfile.TarInfo('./opt/nekoray/')
            entry.type = tarfile.DIRTYPE
            entry.uid, entry.gid, entry.mode = uid, gid, mode
            entry.uname = entry.gname = 'root'
            archive.addfile(entry)
    # Minimal ar container: no extraction and no host ownership changes.
    with path.open('wb') as output:
        output.write(b'!<arch>\n')
        for name, content in [('debian-binary', b'2.0\n'), ('data.tar.xz', payload.getvalue())]:
            output.write(f'{name:<16}{0:<12}{0:<6}{0:<6}{"100644":<8}{len(content):<10}`\n'.encode())
            output.write(content)
            if len(content) % 2:
                output.write(b'\n')


def main():
    with tempfile.TemporaryDirectory() as tmp:
        package = Path(tmp) / 'fixture.deb'
        cases = [({}, True, 'all 0:0'),
                 ({'uid': 1001}, False, 'owner 1001:0'),
                 ({'gid': 1001}, False, 'owner 0:1001'),
                 ({'uid': 1001, 'gid': 1001}, False, 'owner 1001:1001'),
                 ({'mode': 0o777}, False, 'unsafe mode'),
                 ({'mode': 0o4755}, False, 'unsafe mode'),
                 ({'mode': 0o2755}, False, 'unsafe mode'),
                 ({'empty': True}, False, 'Empty Debian payload')]
        for options, success, message in cases:
            write_deb(package, **options)
            result = subprocess.run(['python3', str(REPO / 'libs/validate_debian.py'), str(package)],
                                    capture_output=True, text=True, timeout=30)
            assert (result.returncode == 0) == success, result
            assert message in result.stdout + result.stderr, result
            print(f'{options or "root-owned positive"}: exit {result.returncode}; ' + (result.stdout + result.stderr).strip())
        package.write_bytes(b'not a Debian archive')
        result = subprocess.run(['python3', str(REPO / 'libs/validate_debian.py'), str(package)],
                                capture_output=True, text=True, timeout=30)
        assert result.returncode != 0
        print('malformed archive: rejected')


if __name__ == '__main__':
    main()
